# Studio Duo

[![CI](https://github.com/mbianchidev/studio-duo/actions/workflows/ci.yml/badge.svg)](https://github.com/mbianchidev/studio-duo/actions/workflows/ci.yml)

Studio Duo is an open-source desktop digital audio workstation for recording,
editing, mixing, and mastering music. Its familiar single-window workflow is
optimized for modern metal production without making the core DAW unfamiliar.

## Current vertical slice

The repository now contains a working native C++20 and JUCE 9 application with:

- CoreAudio/ASIO/WASAPI device selection, transport, metronome, looping, and
  lock-free audio recording
- Audio import, playback, track arm/mute/solo, gain, pan, clip move, split,
  delete, undo, and redo
- Out-of-process VST3 and Audio Unit discovery with a persistent searchable
  catalog, timeout isolation, and crash blacklisting
- Persistent per-track plugin insert chains with sandbox mode, bypass, removal,
  latency metadata, opaque state references, and missing-plugin preservation
- A fixed one-block shared-memory bridge transport with lock-free sequence
  counters, worker supervision, last-valid-output fallback, and late-block
  diagnostics
- Out-of-process plugin instantiation, state restoration, channel-layout
  validation, preparation, and audio block processing
- A dark single-window arrangement, inspector, transport, and mixer workspace
- Versioned `.studioduo` directory packages with generation-based saves and a
  recovery point
- Deterministic 48 kHz, 24-bit stereo WAV export
- Automated model, command-history, and project-persistence tests on macOS and
  Windows

This is the first vertical slice, not the full 1.0 feature set. Routing project
insert chains through bridge clients, plugin editors and automation, linked
multitrack editing, MIDI, mastering, DAWproject exchange, and the bundled
device suite remain on the accepted roadmap.

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
track.

Each timeline track header has **M**, **S**, and **R** controls for mute, solo,
and record arm. Use either **+ AUDIO TRACK** control to add tracks. Pressing
**REC** with an unarmed audio track selected arms it automatically and starts
transport; press **REC** or **STOP** to finish the take. The session sidebar
also duplicates or deletes the selected non-master track with full undo support.

The track inspector selects the hardware input, chooses mono or adjacent-channel
stereo capture, and enables software monitoring. Monitoring is off by default
to avoid accidental feedback.

Selected clips show edge handles. Drag the body to move a clip, the left edge
to change its timeline start and source offset, or the right edge to shorten or
restore the available source range. During move or trim, the original bounds
remain as a ghost outline. The command bar above the timeline exposes trim
left, split, trim right, and delete actions. Clicking the selected clip or its
track places the playhead without dropping clip selection, so the command bar
and shortcuts act at that cursor. Drag a clip vertically to move it to another
audio track while preserving its source and edit history. The ghost keeps a
subtle copy of the original waveform visible under an edge-trim preview.

Recording draws a live waveform from lock-free peak buckets. Stopping creates
and flushes the WAV before adding its clip; the inspector shows the saved WAV
filename and the status bar reports the completed file. The waveform baseline
is visible immediately in a full-width recording region, and either **STOP REC**
or **STOP** finalizes capture and stops transport through the same path.

Use the **-**, **100%**, and **+** timeline controls, the zoom shortcuts, or
Command/Ctrl+mouse-wheel to zoom while keeping the playhead centered.

## Documentation

- [Product and technical design](docs/design.md)
- [Development and architecture](docs/development.md)
- [Contributing](docs/contributing.md)

## License

Studio Duo is licensed under the
[GNU Affero General Public License v3.0 only](LICENSE). JUCE 9 is consumed under
its AGPLv3 option for this open-source application.
