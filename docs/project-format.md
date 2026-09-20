# Studio Duo native project format

Studio Duo format version 11 is a directory package with immutable generation
files and content-addressed processor state.

```text
Project.studioduo/
  manifest.json
  session/generation-XXXXXXXX.json
  automation/generation-XXXXXXXX.json
  plugin-state/<sha256>.bin
  media/
  analysis/
  portable-manifest.json
  recovery/latest.json
  recovery/in-process-active.json
  renders/tones/*.wav
  renders/tones/*.report.json
```

## Save transaction

1. Capture active processor state outside the audio callback.
2. Write new state blobs only when their SHA-256 hash is new.
3. Write the next session and automation generation files.
4. Flush those files.
5. Atomically replace `manifest.json` so it selects the new generation.
6. Write `recovery/latest.json`.

The prior manifest and generation remain valid until step 5 succeeds.

## Manifest

`manifest.json` contains:

- `formatVersion`
- `applicationVersion`
- stable project identity
- active session and automation generation paths
- generation number and save time
- `requiredCapabilities`

Current manifests include `midiCompositionV1`,
`midiChannelPressureV1`, `bundledCompositionDevicesV1`, `scenesV1`,
`compatibilityReportsV1`, `renderReportsV2`, `dawprojectV1`, and
`masteringAlbumV1`, `sectionTransportV1`, and `timelineMarkersV1`. Readers must reject a manifest
version newer than they support rather than silently dropping MIDI, bundled
output/cabinet state, scenes, or interchange diagnostics.

Paths must be relative children of the package and cannot contain `..`.

## Session document

The session document stores transport, tempo and meter maps, tracks, audio and
MIDI clips, take/comp state, edit groups, reamp routes, the typed routing graph,
processor records, tone snapshots, mixer snapshots, render reports, drum maps,
pattern aliases, MIDI routing templates, general project metadata, launch
scenes, structured compatibility reports, and the mastering album. Automation lanes are stored
separately.

### Section transport and loops

`markers` stores independent named timeline flags with stable IDs, names, and
non-negative positions. Markers may be dragged, renamed, used for navigation,
or resolved into loop/export ranges. They do not own transport settings or
define song ranges.

`sections` stores named song-range boundaries. A section starts at its
`timeSeconds`; optional `endTimeSeconds` stores a user-resized end, otherwise
the section extends to the next section or timeline end. Sections are managed
independently from marker flags and cannot overlap the next section.

`tempoChanges` and `meterChanges` remain the authoritative timeline maps.
Their optional `sectionId` binds a point to a named section with the same
`timeSeconds`; absent/empty IDs identify ordinary manual points. Owners must
exist and own at most one point in each map. Section moves and removals update
owned points atomically. Clearing section settings never changes the project
default tempo or default 4/4 meter.

A section may contain `clickSettings`: `enabled`, `subdivision` (1-8), `level`
and `accentLevel` (finite 0-1), and `accentBeats` (unique integers 1-32, possibly
empty). Missing settings inherit the last earlier explicit section override or
the global click defaults. The global `metronomeEnabled` remains a master gate.
Audio snapshots compile patterns into immutable scalar events and accent masks;
project IDs and dynamic pattern arrays are not copied on the audio callback.

`loopStartSeconds`, `loopEndSeconds`, and `loopEnabled` persist arbitrary loop
bounds. Musical-position and marker selection are editing conveniences resolved
to seconds, not new persistent references. Playback and loop exports round these
positions to the relevant sample grid.

External and bundled processor records retain a stable insert ID, format,
vendor, version, architecture, isolation mode, state path/hash, latency, tail,
ARA capability, missing state, recovery-disabled state, and `stateFormat`.
State format distinguishes Studio Duo host-opaque bytes from preserved generic,
VST2, VST3, CLAP, and Audio Unit preset containers so interchange never
mislabels one encoding as another.

Bundled guitar and bass amp state includes normalized automatable parameters
and either the documented embedded cabinet identity or validated mono/stereo
cabinet samples. Custom IR state is self-contained; reopening does not require
the original external file. Invalid or truncated state fails processor restore
and leaves the insert in the normal failed/missing-state path.

### MIDI clips and notes

Every MIDI or instrument track has a `midiClips` array. Other track types and
version lanes cannot own MIDI clips. A clip contains:

- stable `id` and `name`
- musical `startBeats` and `durationBeats`
- `editorMode`: `pianoRoll` or `drums`
- optional `drumMapId`
- decimal-string `humanizeSeed`, `humanizeTimingTicks`, and
  `humanizeVelocity`
- `muted`
- ordinary `notes`

Each note stores a stable `id`, MIDI `pitch` and `channel`, grid
`startBeats`, saved `timingOffsetBeats`, `durationBeats`, `velocity`,
`releaseVelocity`, and `probability`. Drum-aware notes can additionally retain
`drumMapEntryId`, `articulation`, `chokeGroup`, `cymbalState`,
`footControlValue`, and `roundRobinHint`. These fields are metadata on an
ordinary MIDI note; no bundled instrument is required to interpret the clip.

The note `expressions` array contains stable point IDs, an `offsetBeats` within
the note, and a value. Supported `type` values are `pitchBend`, `pressure`,
`channelPressure`, `timbre`, and `controller`. `pressure` is polyphonic key
pressure, while `channelPressure` applies to the whole MIDI channel; controller
points also store a MIDI controller number. Pitch values use `-1.0..1.0`;
other expression values use `0.0..1.0`.

All persisted MIDI object IDs are unique. Loading rejects invalid ranges,
duplicate IDs, unsupported enum values, MIDI clips on incompatible tracks, and
dangling drum-map or drum-map-entry references.

### Drum maps

The project-level `drumMaps` array is also the import format accepted by the
drum editor one object at a time:

```json
{
  "id": "stable-map-id",
  "name": "Example metal kit",
  "source": "optional source description",
  "entries": [
    {
      "id": "stable-entry-id",
      "noteNumber": 46,
      "name": "Hi-hat open",
      "articulation": "edge",
      "chokeGroup": "hihat",
      "cymbalState": "open",
      "footControlCC": 4,
      "roundRobinNotes": [],
      "outputGroup": "Cymbals"
    }
  ]
}
```

`cymbalState` is one of `none`, `edge`, `bow`, `bell`, `choke`, `open`,
`closed`, `pedal`. Base note numbers are unique within a map. Round-robin note
numbers are hints/variants and the expanded note keeps its actual MIDI pitch.

### Pattern aliases and routing templates

`midiPatterns` contains stable aliases with `lengthBeats` and ordinary pattern
events (`pitch`, map-entry reference, offset, duration, velocity, and
probability). Expansion always creates fresh note and expression IDs in a
normal clip; the clip does not retain an opaque alias instance.

`midiRoutingTemplates` contains named outputs with stable IDs, destination
track IDs when pre-bound, a `midiChannel`, and pitch lists. Applying an
unbound output creates a named MIDI destination track. The resulting
`RoutingConnection` records use `signalType: "midi"` and `midiChannel` `1..16`;
`0` means no channel filter for ordinary manually-created routes.

An audio `RoutingConnection` can additionally contain `sourceInsertId` and
`sourceBusIndex`. Both are absent/zero for normal track sends. A nonzero bus
must belong to the final bundled insert on the source track and currently uses
a pre-fader send to an aux or bus track. This preserves drum Kick, Snare, Toms,
and Cymbals stem routing in the same typed graph used by live and offline
processing.

Humanization uses saved integer parameters and stores the exact resulting note
velocity/timing values. Its fixed integer generator and mapping do not depend
on a platform standard-library random engine or distribution.

### Project metadata

The `metadata` object stores artist, album, original artist, composer,
songwriter, producer, arranger, year, genre, copyright, website, and comment.
The project `name` remains the title. These fields are native Studio Duo data
and map to DAWproject metadata only at the interchange boundary.

### Mastering album and media hashes

The `mastering` object stores album/release metadata, output gain, ordered
songs, alternate source mixes, the selected mix per song, gaps, overlaps,
fades, gain, ISRC, relative index markers, and references. References remain
outside album rendering and output gain.

Audio clips and mastering resources can retain a SHA-256 `sourceHash`. Portable
copy writes content-addressed files below `media/`, serializes package-local
paths as `${PROJECT_DIR}/...`, and records relative path, size, hash, and object
path in `portable-manifest.json`. Package loading resolves the token only to a
safe child path. Transfer validation and repair reports remain explicit.

### Render reports

Mastering reports extend the original render-report fields with format, sample
rate, bit depth, settings hash, integrated loudness, loudness range, true peak,
sample peak, correlation, a public signing key, and an RSA signature. Reports
are measured from the completed encoded file. The local signature detects
changes; it is not a publisher or code-signing identity.

### Scenes

The `scenes` array exists before the live session view. Each scene has a stable
ID, name, color, and at most one slot per track. A slot stores its own stable
ID, track relationship, stop behavior, and either an ordinary audio clip or an
ordinary MIDI clip. Scene clips use the same internal clip and note types as
the arrangement, but remain independent scene content.

Scene, slot, clip, note, and expression IDs are validated on load. Dangling
track or drum-map references, duplicate scene tracks, and a slot containing
both clip types are rejected.

### Compatibility reports

The `compatibilityReports` array stores interchange format, operation, source,
destination, creation time, and typed issues. Every issue has a severity,
stable code, affected object path, and message. Import reports are saved in the
new native package. Export reports enter the normal project/recovery lifecycle
and persist on the next save.

## Automation document

The automation generation stores stable lanes, targets, points, timebase,
interpolation, trim offset, and enablement. Targets can address track controls,
routes, bundled-device parameters, external plugin parameters, or
channel-pressure messages on a specific MIDI channel. MIDI channel-pressure
targets use `type: "midiChannelPressure"` and persist `midiChannel` in the
internal `1..16` range.

## Recovery

`recovery/latest.json` contains the latest complete project state.
`recovery/in-process-active.json` records trusted or ARA instances active during
the current run. If the application exits uncleanly, those instances reopen
disabled until explicit reload. Clean shutdown removes the marker.

## Migrations

- Version 1 projects gain direct-to-master routes.
- Version 2 `outputTrackId` values become version 3 main-output connections.
- Versions 1 and 2 gain empty automation, snapshot, and report collections.
- Versions 1-4 gain empty per-track `midiClips` plus the deterministic default
  metal map, pattern aliases, and routing template. Their fixed IDs remain
  stable before and after the first version 5 save.
- Version 5 gains version 6 bundled processor-output route fields with empty
  source insert IDs and bus index zero for every existing route.
- Version 6 gains version 7 empty metadata, scenes, and compatibility-report
  collections.
- Version 7 gains version 8 `midiChannel: -1` defaults on existing automation
  targets; channel-pressure targets require `1..16`.
- Version 8 gains version 9 empty mastering album data. Existing media hashes
  remain optional and are populated during import, collection, or repair.
- Versions 1-9 migrate to version 10 without adding section ownership or click
  overrides. Existing maps, loop bounds and project meter remain unchanged.
- Versions 1-10 migrate to version 11 by preserving existing song sections and
  copying their former marker identity into standalone draggable markers.
- The manifest and referenced session format versions must agree.

[`schema/project-v11.schema.json`](schema/project-v11.schema.json) documents the
current public session envelope; older schema files remain historical.
