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
- A dark single-window arrangement, inspector, transport, and mixer workspace
- Versioned `.studioduo` directory packages with generation-based saves and a
  recovery point
- Deterministic 48 kHz, 24-bit stereo WAV export
- Automated model, command-history, and project-persistence tests on macOS and
  Windows

This is the first vertical slice, not the full 1.0 feature set. Plugin hosting,
linked multitrack editing, MIDI, mastering, DAWproject exchange, and the bundled
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
| Delete selected clip | `Delete` or `Backspace` |

Use **I/O** to select audio devices. Arm an audio track before recording. Drag
clips to move them on the beat grid.

## Documentation

- [Product and technical design](docs/design.md)
- [Development and architecture](docs/development.md)
- [Contributing](docs/contributing.md)

## License

Studio Duo is licensed under the
[GNU Affero General Public License v3.0 only](LICENSE). JUCE 9 is consumed under
its AGPLv3 option for this open-source application.
