<p align="center">
  <img src="assets/branding/studio-duo-logo.svg" alt="Studio Duo" width="720">
</p>

[![CI](https://github.com/mbianchidev/studio-duo/actions/workflows/ci.yml/badge.svg)](https://github.com/mbianchidev/studio-duo/actions/workflows/ci.yml)

Studio Duo is an open-source desktop digital audio workstation for recording,
editing, mixing, and mastering music. Its familiar single-window workflow is
optimized for modern metal production without making the core DAW unfamiliar.

## Current vertical slice

The repository now contains a working native C++20 and JUCE 9 application with:

- CoreAudio/ASIO/WASAPI device selection, tempo and meter maps, routed
  subdivided metronome, punch/count-in/pre/post-roll transport, looping, and
  sample-aligned lock-free multitrack audio recording
- Audio import, playback, track arm/mute/solo, gain, pan, clip move, split,
  delete, transient detection, pitch-preserving stretch/warp, fades,
  crossfades, consolidation, undo, and redo
- Out-of-process VST3 and Audio Unit discovery with a persistent searchable
  catalog, timeout isolation, and crash blacklisting
- Persistent per-track plugin insert chains with sandbox mode, bypass, removal,
  latency metadata, opaque state references, and missing-plugin preservation
- A fixed one-block shared-memory bridge transport with lock-free sequence
  counters, worker supervision, last-valid-output fallback, and late-block
  diagnostics
- Out-of-process plugin instantiation, state restoration, channel-layout
  validation, preparation, and audio block processing
- Active parent, version-lane, and master insert chains during playback with
  matched graph/snapshot publication, delay compensation, tail draining, and
  metronome alignment
- A dark single-window arrangement, inspector, transport, and mixer workspace
- Versioned `.studioduo` directory packages with generation-based saves and a
  recovery point
- Deterministic 48 kHz, 24-bit stereo WAV export
- Automated model, command-history, and project-persistence tests on macOS and
  Windows

This now includes the Phase 1 vertical slice and Phase 2 professional tracking
and editing workflows, not the full 1.0 feature set. Plugin editors, automation,
real-time plugin-inclusive bounce, MIDI, mastering, DAWproject exchange, and the
bundled device suite remain on the accepted roadmap.

## Build

### Requirements

- macOS with Xcode command-line tools, or Windows with Visual Studio 2022
- CMake 3.25 or newer
- Internet access for the first configure, unless JUCE 9.0.1 is installed as a
  CMake package

```sh
git clone https://github.com/mbianchidev/studio-duo.git
cd studio-duo
cmake -S . -B build -DSTUDIO_DUO_BUILD_TESTS=ON
cmake --build build --config Release --parallel
ctest --test-dir build --build-config Release --output-on-failure
```

On macOS, launch:

```sh
open "build/StudioDuo_artefacts/Release/Studio Duo.app"
```

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
| Zoom timeline out/in | `Command/Ctrl+-` / `Command/Ctrl++` |
| Reset timeline zoom | `Command/Ctrl+0` |

Use **I/O** to select audio devices. Arm an audio track before recording. Drag
clips to move them on the beat grid. Use **SCAN** in the plugin catalog to probe
installed VST3 and Audio Unit plugins outside the main process. Select a catalog
entry and choose **ADD** to attach its bridge-ready insert record to the selected
track. Ready inserts process playback in sandbox workers; the inspector reports
loading, ready, missing, bypassed, crashed, and late-block states. Click a
crashed insert status to reload its worker.

Each timeline track header has **M**, **S**, and **R** controls for mute, solo,
and record arm. Use either **+ AUDIO TRACK** control to add tracks. **REC**
captures every armed audio parent from its configured mono or stereo hardware
input into a separate sample-aligned WAV. If no track is armed, the selected
audio track becomes the recording target for that pass. Press **REC** or
**STOP** to finish all files at the same callback boundary. The session sidebar
also duplicates or deletes the selected non-master track with full undo support.

**TRACKING SETUP** adds tempo or meter changes at the playhead, configures jump
or ramp transitions, punch points, count-in bars, pre/post-roll, loop bounds,
click subdivision, and the hardware output used by the metronome. All settings
are saved with the project and participate in undo/redo.

Arm two or more parent tracks and choose **Link armed parent tracks** in
**TRACKING SETUP** to create a phase-locked edit group. Split, trim, move,
delete, and comp operations then apply as one undoable command across the active
takes at the same timeline position. The same menu selects the timing reference,
quantize strength, protected anchors, group suspension, and unlinking.

For reamping, select a DI parent in **TRACKING SETUP** and create either a
hardware path to an existing return track or a plugin tone path. Hardware paths
route the processed DI to the selected interface output, capture the configured
return input, and can send an impulse to measure round-trip latency. Recorded
returns are shifted by the measured latency plus the saved fine-alignment offset
and can invert polarity. Plugin paths create a non-destructive track referencing
the DI media; add VST3 inserts there to build the tone.

Each completed pass creates grouped `v1`, `v2`, `v3`, ... child tracks directly
below every recorded parent. A multitrack pass enters the project as one
undoable command, so every synchronized lane is added, undone, or redone
together. Version tracks retain normal arm, mute, solo, split, trim, move, and
delete behavior. The parent can collapse the versions and shows their combined
waveform/result. Two-finger/right-click the left track header for mute, solo,
arm, collapse/expand, and group-aware delete actions.

Loop recording writes one continuous synchronized WAV per armed parent and
creates one alternate version lane per loop pass. Right-click a take clip to use
its lane as the active playlist or assign that clip's edited range to the parent
comp. New comp selections replace only overlapping regions; **Clear parent
comp** returns playback to the active playlist.

The track inspector selects the hardware input, chooses mono or adjacent-channel
stereo capture, and enables software monitoring. Monitoring is off by default
to avoid accidental feedback. Gain defaults to `0.0 dB`, accepts signed decimal
entry, and uses a rotary control. Pan defaults to `Center` and displays `% L` or
`% R`. Double-click the inspector track name to rename it. **COLOR** offers
quick palette choices plus an HSV/RGB custom picker; name and color changes are
persistent and undoable. Double-click a timeline track name or the title area of
a lower mixer strip to edit both values in one anchored panel. Every lower mixer
strip also has a draggable L/R pan knob; double-click the knob area to return it
to center.

Selected clips show edge handles. Drag the body to move a clip, the left edge
to change its timeline start and source offset, or the right edge to shorten or
restore the available source range. Trimmed-away audio remains visible as a
subtle dashed waveform ghost and can be restored by dragging the edge back.
Splitting creates two independent source ranges: the left clip cannot expand
past the split point, and the right clip cannot expand before it, even after the
playhead moves. The command bar above the timeline exposes trim left, split,
trim right, and delete actions. Clicking the selected clip or its track places
the playhead without dropping clip selection, so the command bar and shortcuts
act at that cursor. Drag a clip vertically to move it to another audio track
while preserving its source and edit history.

Right-click a clip for deterministic transient detection, source-specific
stretch modes, playback-rate presets, transient-to-playhead warp markers,
fade-in/out placement, linked crossfade and gap closing, polarity inversion,
reverse playback, and consolidation to a new immutable WAV. Green lines mark
transients, orange triangles mark warp points, and fade curves remain visible
on the clip.

Recording draws a live waveform from lock-free peak buckets. Stopping creates
and flushes the WAV before adding its clip; the inspector shows the saved WAV
filename and the status bar reports the completed file. The waveform baseline
is visible immediately in a full-width recording region, and either **STOP REC**
or **STOP** finalizes capture and stops transport through the same path.

Use the **-**, **100%**, and **+** timeline controls, the zoom shortcuts, or
Command/Ctrl+mouse-wheel to zoom while keeping the playhead centered. The
percentage button reflects the current zoom and resets it when clicked.

During playback and recording, the arrangement follows the playhead before it
reaches the right edge. Track headers remain pinned on the left while the
timeline scrolls. Pressing Play after the project reaches its end rewinds and
starts again instead of remaining at the terminal position.

Two-finger click or right-click the timeline to move the playhead and open the
clip context menu. Trim start, split, trim end, and delete show their keyboard
shortcuts alongside each action, and the menu opens at the gesture point.

## Documentation

- [Product and technical design](docs/design.md)
- [Development and architecture](docs/development.md)
- [Contributing](docs/contributing.md)

## Brand assets

The editable vector logo, app icon, PNG sizes, macOS ICNS, and Windows ICO are
kept in [`assets/branding`](assets/branding). The packaged JUCE application uses
the 512 px and 32 px icon sources for native platform icon generation.

## License

Studio Duo is licensed under the
[GNU Affero General Public License v3.0 only](LICENSE). JUCE 9 is consumed under
its AGPLv3 option for this open-source application. Signalsmith Stretch 1.1.0
is fetched under its MIT license for pitch-preserving elastic audio.
