# Studio Duo user guide

Studio Duo is under active development. The current application includes the
Phase 1 vertical slice, Phase 2 professional tracking and editing workflows, and
the complete Phase 3 mixer and plugin platform. MIDI composition, DAWproject
exchange, and mastering remain later roadmap phases.

## Current capabilities

- CoreAudio, ASIO, and WASAPI device selection
- Tempo and meter maps with jump or ramp changes
- Routed metronome with accents and subdivisions
- Punch, count-in, pre-roll, post-roll, and loop transport
- Sample-aligned lock-free multitrack audio recording
- Audio import, playback, arm, mute, solo, gain, and pan
- Clip move, split, trim, delete, fades, crossfades, and consolidation
- Deterministic transient detection and pitch-preserving stretch and warp
- Take lanes, playlists, comping, and linked multitrack editing
- Hardware reamp routing, calibration, polarity, and fine alignment
- Plugin tone paths that reference the original DI media
- Sandboxed VST3, Audio Unit, and CLAP discovery and processing
- Explicit ARA 2 compatibility and trusted in-process modes with recovery
  warnings and safe-disabled reopening
- Persistent plugin state, compatibility records, missing-plugin replacement,
  latency metadata, crash/timeout diagnostics, and per-insert reload
- Pre/post-fader sends, sidechains, parallel paths, auxes, nested buses,
  folders, VCAs, control room, hardware outputs, and solo-safe routing
- Sample-accurate mixer, send, bundled-device, and plugin automation
- Parametric EQ, compressor, true-peak limiter, reverb, gate, gain, polarity,
  delay, tuner, and signal generator devices
- Tone and mixer snapshots, level-matched A/B, stale detection, freeze, print,
  plugin-inclusive rendering, batch reports, and Scream Forge validation
- Versioned `.studioduo` packages, generation saves, and recovery points
- Deterministic 48 kHz, 24-bit stereo WAV export when active plugins are absent

## Essential controls

| Action | Shortcut |
| --- | --- |
| Play or pause | `Space` |
| Save | `Command/Ctrl+S` |
| Open | `Command/Ctrl+O` |
| Import audio | `Command/Ctrl+I` |
| Undo | `Command/Ctrl+Z` |
| Redo | `Command/Ctrl+Shift+Z` |
| Split selected clip at playhead | `S` |
| Trim selected clip start to playhead | `[` |
| Trim selected clip end to playhead | `]` |
| Delete selected clip | `Delete` or `Backspace` |
| Zoom timeline out or in | `Command/Ctrl+-` or `Command/Ctrl++` |
| Reset timeline zoom | `Command/Ctrl+0` |

## Start a session

1. Open **I/O** and enable the required hardware inputs and outputs.
2. Add audio tracks with **+ AUDIO TRACK**, or import WAV, AIFF, FLAC, or MP3
   files with **IMPORT AUDIO**.
3. Select a track to configure its input, mono or stereo capture, monitoring,
   volume, pan, color, inserts, and output.
4. Save the project as a `.studioduo` directory package before recording so new
   media is written below its `media/` directory.

Monitoring is off by default to avoid accidental feedback. Stereo capture uses
the selected hardware input and the adjacent channel.

## Manage tracks

The session sidebar and track-header context menu can duplicate or delete the
selected non-master track with full undo support. Duplicating a parent copies
its complete track family: clips, take lanes, inserts, input and mix settings,
playlist and comp state, and connected tone-path sends and owned returns. New
track, clip, insert, comp, route, and owned-return IDs prevent later edits from
aliasing the source family.

## Record and manage takes

Each timeline track header has **M**, **S**, and **R** controls for mute, solo,
and record arm. **REC** captures every armed audio parent into a separate,
sample-aligned WAV. If no track is armed, the selected audio track becomes the
single recording target.

Press **REC** again or **STOP** to finish every active recording at the same
audio callback boundary. The timeline draws a live waveform from lock-free peak
buckets while recording. Stopping flushes each WAV before its clip is added; the
inspector identifies the saved filename and the status bar reports completion.

Each completed pass creates grouped `v1`, `v2`, `v3`, and later child tracks
below the recorded parent. The whole multitrack pass is one undoable command.
Version tracks retain ordinary mute, solo, arm, split, trim, move, and delete
behavior. Collapse or expand versions from the parent header.

Loop recording writes one continuous synchronized WAV per armed parent and
creates one version lane per loop pass. Right-click a take clip to choose its
lane as the active playlist or assign its edited range to the parent comp. New
comp selections replace only overlapping regions. **Clear parent comp** returns
playback to the active playlist.

## Configure transport and linked editing

**TRACKING SETUP** manages:

- Tempo and meter changes at the playhead
- Jump and ramp tempo transitions
- Punch points, count-in, pre-roll, and post-roll
- Loop bounds
- Metronome subdivision and hardware output

All settings are persistent and undoable.

Arm two or more parent tracks and choose **Link armed parent tracks** to create a
phase-locked edit group. Split, trim, move, delete, comp, warp, and quantize
operations then apply across the active takes at the same timeline position.
The setup menu also selects the timing reference, quantize strength, protected
anchors, suspension, and unlinking.

## Edit clips

Select a clip to expose edge handles. Drag its body horizontally to move it on
the beat grid or vertically to another audio track. Drag the left edge to change
the timeline start and source offset. Drag the right edge to shorten or restore
the available source range.

Trimmed audio remains visible as a dashed waveform ghost and can be restored.
After a split, each half keeps independent source boundaries and cannot expand
through the split point.

The command bar above the timeline exposes trim left, split, trim right, and
delete. Clicking the selected clip or its track moves the playhead without
dropping clip selection, so those commands and their shortcuts act at the
visible cursor.

Right-click a clip for:

- Transient detection
- Drum, monophonic, polyphonic, and full-mix stretch modes
- Playback-rate presets and transient-to-playhead warp markers
- Fade-in and fade-out placement
- Linked crossfade generation and gap closing
- Polarity inversion and reverse playback
- Consolidation to a new immutable WAV

Green lines identify transients, orange triangles identify warp points, and fade
curves remain visible on the clip.

## Mix and route tracks

The inspector and lower mixer expose gain, pan, mute, and solo. Gain defaults to
`0.0 dB`; pan defaults to `Center`. Drag mixer faders and pan knobs to edit them.
Double-click a fader lane to return to `0.0 dB` or a pan knob to return to
center.

The mixer also contains a scrollable **INSERTS & SENDS** list across all root
tracks. Click an insert to open its editor or use its **ON/OFF** control to
bypass and restore it while audio is playing. Click a send or sidechain to open
its routing editor.

Use the sidebar arrow to collapse Session controls to an icon rail. **INSPECT**
and **MIX** show or hide the fixed-size inspector and mixer. Their dividers also
collapse when dragged closed and restore when dragged open or double-clicked.
The processor search moves above its action buttons on narrow layouts.

Double-click a track name in the inspector, timeline, or mixer to edit its name.
**COLOR** provides palette choices and an HSV/RGB picker. Appearance changes are
persistent and undoable.

Use **+ TRACK** for audio, aux, bus, folder, VCA, and control-room tracks.
**+ BUS TRACK** remains a direct bus shortcut. Select a root track and
choose **OUTPUT** in the inspector. Audio, instrument, aux, and bus tracks can
feed a bus or the master. Buses can feed later buses. Destinations that would
create a cycle are excluded.

The routing panel adds pre-fader and post-fader sends, plugin sidechains, and
direct hardware outputs. Click a route to change tap, level, mute, enablement,
or remove it. Aux and bus tracks sum every incoming path. The graph rejects
cycles across main routes, sends, and sidechains.

**TRACK** in the routing panel changes mono/stereo layout, polarity, solo-safe
state, folder placement, and VCA assignment. Folder mute and solo scope their
children without hiding summing; use a bus for audio summing. VCAs control the
assigned track faders without changing signal routing.

A control-room track receives the master monitor path without entering exports.
Its menu selects monitor hardware, dim, mono, mute, inserts, and click routing.
Mixer strips show separate pre-fader and post-fader meters. Plugin and bridge
latencies are aligned at every summing and sidechain destination.

## Automate controls and plugin parameters

Choose **AUTOMATION** to open the lane editor for the selected track.

- Select read, touch, latch, write, trim, or preview mode.
- Arm writing with **WRITE ARM**.
- Add lanes for volume, pan, mute, polarity, sends, bundled devices, or
  automatable plugin parameters.
- Choose seconds or beat time, and linear or step interpolation.
- Add or remove points at the playhead with a normalized value.

Beat lanes follow tempo ramps and abrupt changes. Playback schedules mixer
changes and plugin events at exact sample offsets. Mixer fader and pan gestures
write the armed track according to its selected mode. Preview remains
non-destructive until a lane edit is committed.

## Use plugins

Choose **SCAN** in the processor catalog to probe installed VST3 plugins, Audio
Units, and CLAP bundles outside the main process. Bundled utility devices are
always listed. Select an entry and choose **ADD** to attach it to the selected
track.

Ready external inserts process playback in sandbox workers. Double-click an
insert to open its plugin editor in the worker process; plugins without a native
editor receive an isolated generic editor. Right-click an insert to open the
generic parameter editor or choose sandboxed, trusted in-process, or advertised
ARA 2 mode.

ARA activation requires a saved project, writes a recovery point, and warns
about reduced crash isolation. Studio Duo registers the track's immutable audio
sources, clip playback regions, tempo map, and meter map with the ARA document.
Processor and ARA document state are archived together, including unsaved ARA
edits preserved across clip, tempo, and meter graph rebuilds.

The inspector reports loading, ready, missing, bypassed, recovery-disabled,
crashed, and late-block states. Click a failed insert to reload that runtime.
Click a missing insert, then choose a catalog processor to replace it while
preserving the insert ID, routing, automation, and prior state reference.

**TEST** runs black-box public-standard compatibility checks in a separate
process. The tracking menu can validate installed Scream Forge VST3, Audio Unit,
and advertised ARA capability without proprietary source code.

## Create reamp paths

Select a DI parent in **TRACKING SETUP** and create a hardware or plugin tone
path.

A hardware path sends the processed DI to an interface output and records the
configured return input. **Calibrate round-trip latency** emits an impulse and
stores the measured delay. Recorded returns are shifted by the measured latency
plus the saved fine-alignment offset and can invert polarity.

A plugin path creates a non-destructive audio track that references the active
DI playlist. Add inserts to that track to build the tone without replacing the
source DI.

For a selected tone path, **TRACKING SETUP** can:

- Capture named snapshots of routing, processor state, level, and automation
- Recall a snapshot with undo
- Show stale state after the DI, playlist, routing, automation, or chain changes
- Freeze a plugin tone to an immutable WAV while preserving and muting the live
  return
- Unfreeze by restoring the live return and removing the frozen track
- Print a tone to a separate editable audio track
- Batch render every snapshot with one JSON report per item

Rendered snapshots store content hashes. Batch comparison uses deterministic
gated RMS trims for level-matched A/B; it does not claim mastering-loudness
compliance.

## Navigate the timeline

Use the **-**, **100%**, and **+** controls, keyboard shortcuts, or
`Command/Ctrl` plus the mouse wheel to zoom while keeping the playhead centered.
The arrangement follows the playhead during playback and recording while track
headers remain pinned.

Right-click the timeline to place the playhead and open the context menu.
Pressing Play at the project end rewinds before starting.

## Projects and export

Studio Duo projects are versioned `.studioduo` directory packages. A save writes
a new session generation before atomically replacing `manifest.json`; the latest
complete state is also copied to `recovery/latest.json`.

Project format version 3 stores a typed routing graph, separate automation
generations, content-addressed plugin state, compatibility policy, tone and
mixer snapshots, and render reports. Version 1 and 2 projects migrate on load.
See [project-format.md](project-format.md).

Stereo WAV export is 48 kHz and 24-bit. Projects without processors use the
fast deterministic graph. Bundled and trusted processors render offline;
sandboxed third-party processors use the same one-block pipeline in a real-time
fallback so processing is never silently omitted.

## Brand assets

Editable logo, icon, PNG, ICNS, and ICO sources are stored in
[`../assets/branding`](../assets/branding). The application embeds the SVG mark
in its header and uses the platform icon sources during builds.
