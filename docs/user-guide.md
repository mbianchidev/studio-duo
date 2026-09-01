# Studio Duo user guide

Studio Duo is under active development. The current application includes the
Phase 1 vertical slice, Phase 2 professional tracking and editing workflows, and
the first Phase 3 routing foundation. Sends, sidechains, VCAs, control-room
routing, automation, real-time plugin-inclusive bounce, MIDI, mastering,
DAWproject exchange, and bundled devices remain on the roadmap.

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
- Sandboxed VST3 and Audio Unit discovery and processing
- Persistent plugin chains, opaque state, missing-plugin preservation, bypass,
  latency metadata, crash status, and explicit reload
- Nested stereo bus output routing with cycle prevention
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

Double-click a track name in the inspector, timeline, or mixer to edit its name.
**COLOR** provides palette choices and an HSV/RGB picker. Appearance changes are
persistent and undoable.

Use **+ BUS TRACK** to create a stereo summing bus. Select a root track and
choose **OUTPUT** in the inspector. Audio, instrument, aux, and bus tracks can
feed a bus or the master. Buses can feed later buses. Destinations that would
create a cycle are excluded.

Bus gain, pan, mute, solo, and plugin inserts process the summed input. Plugin
latencies are aligned at every summing destination. Deleting a bus reroutes its
incoming tracks to the master; undo restores the bus and its routes.

## Use plugins

Choose **SCAN** in the plugin catalog to probe installed VST3 plugins and Audio
Units outside the main process. Select a catalog entry and choose **ADD** to
attach it to the selected track.

Ready inserts process playback in sandbox workers. The inspector reports
loading, ready, missing, bypassed, crashed, and late-block states. Click a
crashed insert status to reload its worker. Missing plugins retain their
identifier, routing, automation-ready model data, and opaque state reference.

Fast offline export refuses a project with effective active inserts rather than
silently omitting processing. Plugin-inclusive real-time bounce is a later
roadmap checkpoint.

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

Project format version 2 adds explicit root-track output routing. Version 1
projects load with every non-master track routed directly to the master.

Stereo WAV export is deterministic at 48 kHz and 24-bit depth when no effective
active plugin inserts are present.

## Brand assets

Editable logo, icon, PNG, ICNS, and ICO sources are stored in
[`../assets/branding`](../assets/branding). The application embeds the SVG mark
in its header and uses the platform icon sources during builds.
