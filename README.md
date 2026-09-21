<p align="center">
  <img src="assets/branding/studio-duo-logo.svg" alt="Studio Duo" width="720">
</p>

[![CI](https://github.com/mbianchidev/studio-duo/actions/workflows/ci.yml/badge.svg)](https://github.com/mbianchidev/studio-duo/actions/workflows/ci.yml)

Studio Duo is an open-source native desktop DAW for recording, editing, mixing,
and mastering music. It follows familiar professional workflows while making
modern metal production faster.

## Status

The working C++20 and JUCE 9 application includes Phases 1 through 5: tracking,
editing, mixing, plugin hosting, MIDI composition, bundled instruments and
amps, DAWproject 1.0 interchange, and mastering/release delivery. Version 1.0
hardening remains before the complete DAW described in the
[accepted design](docs/design.md).

## Highlights

- Sample-aligned multitrack recording, punch, loop, count-in, tempo maps, and
  routed metronome
- Named timeline markers with undoable editing and whole-project, loop,
  marker-to-marker, or custom-range audio export
- Editable loop bounds in seconds, bars/beats/ticks, or marker positions, plus
  section-specific tempo, time signature, and click/accent patterns
- WAV, AIFF, FLAC, Ogg Vorbis, and built-in MP3 encoding with sample-rate,
  bit-depth, mono/stereo, bitrate/quality, peak-normalization, and dither controls
- Take lanes, comping, linked multitrack edits, transient tools, elastic audio,
  fades, crossfades, and consolidation
- Cycle-safe sends, per-insert sidechains, auxes, nested buses, folders, VCAs,
  control room, graph-routed input monitoring, flexible hardware outputs,
  solo-safe behavior, and plugin delay compensation
- Live, recorded, and retrospective MIDI with ordinary beat-based clips,
  stable note/expression IDs, cycle-safe channel-filtered routing, and exact
  sample scheduling through in-process and sandboxed processors
- Keyboard-accessible piano-roll and metal drum lower editors with velocity,
  timing, duration, probability, per-note expression, editable drum maps,
  deterministic entry tools, seeded humanization, pattern expansion, and
  multi-output routing templates
- A deterministic bundled metal drum instrument with velocity, round robin,
  cymbal chokes/foot control, a useful synthesized kit, and routable kick,
  snare, tom, and cymbal outputs
- Bundled guitar and bass amps with nonlinear tone stages, embedded cabinets,
  validated user cabinet IR loading, persistent state, and fixed-latency
  partitioned convolution
- Transactional DAWproject 1.0 import/export with official embedded schema
  validation, deterministic ZIP output, media and plug-in state transfer,
  preserved scenes, and object-specific compatibility reports
- Dedicated multi-song mastering with alternate source mixes, gaps, overlaps,
  fades, metadata, BS.1770/R128 loudness analysis, true peak, configurable audio
  export, deterministic TPDF dither, signed reports, and external licensed DDP
  encoder integration
- Content-addressed portable copies with package-relative media paths,
  SHA-256 transfer validation, and hash-based missing-resource repair
- Sample-accurate mixer and plugin automation with read, touch, latch, write,
  trim, and preview modes
- Sandboxed VST3, Audio Unit, and CLAP processing plus explicit ARA 2
  compatibility mode, crash records, state recovery, and missing placeholders
- Thirteen bundled devices plus plugin-backed reamp snapshots, freeze, print,
  level-matched comparison, batch rendering, and reports
- Startup hub with blank songs, curated templates, ordered recent projects,
  and stale-entry cleanup
- Four accessible dark themes, persisted custom surface/accent colors, and
  version-matched in-app access to the user guide

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
open "build/StudioDuo_artefacts/Release/Studio Duo.app"
```

## Install

Published builds are available from
[GitHub Releases](https://github.com/mbianchidev/studio-duo/releases). Releases
include a universal macOS DMG, a signed Windows x64 installer, a portable
Windows x64 ZIP, and SHA-256 checksums. Installed macOS and Windows builds
check for releases at launch, download verified updates in the background by
default, and install them only when the user chooses **Restart and Update**.

Windows checks MIDI discovery and automatic ASIO startup in separate processes.
If startup still fails, `--safe-audio` opens the app without initializing audio
or MIDI. See [startup troubleshooting and logs](docs/user-guide.md#logs-and-diagnostics).

## Documentation

- [Documentation index](docs/README.md)
- [User guide](docs/user-guide.md)
- [Product, roadmap, and technical design](docs/design.md)
- [Development and architecture](docs/development.md)
- [Bundled devices](docs/devices.md)
- [Mastering and release](docs/mastering.md)
- [DAWproject interchange](docs/dawproject.md)
- [Contributing](docs/contributing.md)
- [Releasing](docs/releasing.md)

## License

Studio Duo is licensed under the
[GNU Affero General Public License v3.0 only](LICENSE). JUCE 9 uses its AGPLv3
option. On Windows, Studio Duo uses JUCE's dual-licensed Steinberg ASIO SDK
headers under their GPLv3 option. Signalsmith Stretch 1.1.0, CLAP 1.2.10, and
clap-helpers are MIT licensed. libsamplerate 0.2.2 is BSD-2-Clause licensed.
ARA SDK 2.3.0 is Apache-2.0 licensed.
The bundled, encoder-only LAME 3.100 library is LGPL-2.0-or-later licensed;
its license, library and application source archives, scalar build configuration,
and rebuilding/relinking notice ship with the application.
The vendored DAWproject 1.0 schemas and upstream XML example are MIT licensed.
