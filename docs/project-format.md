# Studio Duo native project format

Studio Duo format version 7 is a directory package with immutable generation
files and content-addressed processor state.

```text
Project.studioduo/
  manifest.json
  session/generation-XXXXXXXX.json
  automation/generation-XXXXXXXX.json
  plugin-state/<sha256>.bin
  media/
  analysis/
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

Version 7 manifests include `midiCompositionV1`,
`bundledCompositionDevicesV1`, `scenesV1`, `compatibilityReportsV1`, and
`dawprojectV1`. Readers must reject a manifest version newer than they support
rather than silently dropping MIDI, bundled output/cabinet state, scenes, or
interchange diagnostics.

Paths must be relative children of the package and cannot contain `..`.

## Session document

The session document stores transport, tempo and meter maps, tracks, audio and
MIDI clips, take/comp state, edit groups, reamp routes, the typed routing graph,
processor records, tone snapshots, mixer snapshots, render reports, drum maps,
pattern aliases, MIDI routing templates, general project metadata, launch
scenes, and structured compatibility reports. Automation lanes are stored
separately.

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
`timbre`, and `controller`; controller points also store a MIDI controller
number. Pitch values use `-1.0..1.0`; other expression values use `0.0..1.0`.

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
routes, bundled-device parameters, or external plugin parameters.

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
- The manifest and referenced session format versions must agree.

[`schema/project-v7.schema.json`](schema/project-v7.schema.json) documents the
current public session envelope; older schema files remain historical.
