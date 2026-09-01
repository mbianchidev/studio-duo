<p align="center">
  <img src="assets/branding/studio-duo-logo.svg" alt="Studio Duo" width="720">
</p>

[![CI](https://github.com/mbianchidev/studio-duo/actions/workflows/ci.yml/badge.svg)](https://github.com/mbianchidev/studio-duo/actions/workflows/ci.yml)

Studio Duo is an open-source native desktop DAW for recording, editing, mixing,
and mastering music. It follows familiar professional workflows while making
modern metal production faster.

## Status

The working C++20 and JUCE 9 application includes the Phase 1 vertical slice,
Phase 2 professional tracking and editing, and the first Phase 3 nested-bus
routing foundation. It is not yet the complete 1.0 DAW described in the
[accepted design](docs/design.md).

## Highlights

- Sample-aligned multitrack recording, punch, loop, count-in, tempo maps, and
  routed metronome
- Take lanes, comping, linked multitrack edits, transient tools, elastic audio,
  fades, crossfades, and consolidation
- Sandboxed VST3 and Audio Unit discovery and processing with crash isolation
- Hardware and plugin reamp paths with round-trip latency calibration
- Cycle-safe nested stereo buses, plugin delay compensation, project recovery,
  and deterministic WAV export

## Build

Requirements: CMake 3.25+, a C++20 compiler, and either macOS with Xcode
command-line tools or Windows with Visual Studio 2022.

```sh
git clone https://github.com/mbianchidev/studio-duo.git
cd studio-duo
cmake -S . -B build -DSTUDIO_DUO_BUILD_TESTS=ON
cmake --build build --config Release --parallel
ctest --test-dir build --build-config Release --output-on-failure
```

On macOS with the default generator:

```sh
open "build/StudioDuo_artefacts/Studio Duo.app"
```

## Documentation

- [Documentation index](docs/README.md)
- [User guide](docs/user-guide.md)
- [Product, roadmap, and technical design](docs/design.md)
- [Development and architecture](docs/development.md)
- [Contributing](docs/contributing.md)

## License

Studio Duo is licensed under the
[GNU Affero General Public License v3.0 only](LICENSE). JUCE 9 uses its AGPLv3
option; Signalsmith Stretch 1.1.0 is MIT licensed.
