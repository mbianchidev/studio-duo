<p align="center">
  <img src="assets/branding/studio-duo-logo.svg" alt="Studio Duo" width="720">
</p>

[![CI](https://github.com/mbianchidev/studio-duo/actions/workflows/ci.yml/badge.svg)](https://github.com/mbianchidev/studio-duo/actions/workflows/ci.yml)

[Website](https://mbianchidev.github.io/studio-duo/) ·
[Download](https://github.com/mbianchidev/studio-duo/releases/latest) ·
[User guide](docs/user-guide.md)

Studio Duo is an open-source native desktop DAW for recording, editing, mixing,
and mastering music. It follows familiar professional workflows while making
modern metal production faster.

**Windows support is best effort only.** This is a one-person development team
without a Windows testing device, and limited development time restricts
hands-on Windows testing. Automated Windows CI does not replace physical
audio-interface and MIDI-device testing.

<img width="1707" height="1107" alt="image" src="https://github.com/user-attachments/assets/21b07b83-1ca7-4809-9e0d-e17088a1581e" />

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
- Nested instrument/effect racks and adjustable, latency-aligned parallel
  splitters on track, aux/send-return, bus, and master inserts
- Live, recorded, and retrospective MIDI with ordinary beat-based clips,
  stable note/expression IDs, cycle-safe channel-filtered routing, and exact
  sample scheduling through in-process and sandboxed processors
- Keyboard-accessible piano-roll and metal drum lower editors with velocity,
  timing, duration, probability, per-note expression, editable drum maps,
  deterministic entry tools, seeded humanization, pattern expansion, and
  multi-output routing templates
- A ready-to-play drum-track preset and twelve visual performance pads with
  saved sound/key assignments, velocity accents, track-isolated live recording,
  and the existing song transport, click, and MIDI editing workflow
- An alternate drum geometry view with colored rhythm rings, connected hit
  patterns, velocity-sized dots, a named voice legend, and a tempo-aware playback
  sweep over the same editable MIDI clip
- A deterministic bundled metal drum instrument with velocity, round robin,
  cymbal chokes/foot control, synthesized Punk, Hardcore Punk, Death Metal,
  Modern Metal, and Deathcore sound presets, individual batter/resonant head
  tuning, damping, snare-wire tension, and routable kick, snare, tom, and
  cymbal outputs
- Bundled guitar and bass amps with nonlinear tone stages, embedded cabinets,
  validated user cabinet IR loading, persistent state, and fixed-latency
  partitioned convolution
- Transactional DAWproject 1.0 import/export with official embedded schema
  validation, deterministic ZIP output, media and plug-in state transfer,
  preserved scenes, and object-specific compatibility reports
- Guided Studio One migration through direct native `.song` audio/mixer import
  or its broader DAWproject export, with Open/drag-and-drop access and explicit
  confirmation before any partial native transfer
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
- Four accessible dark themes, persisted custom surface/accent colors, and an
  indexed, offline, version-matched in-app user guide

## Platform support and requirements

| Platform | Requirements |
| --- | --- |
| macOS | macOS 12 Monterey or later; Apple silicon (ARM64) or Intel (x86_64); CoreAudio-compatible interface or built-in audio |
| Windows (best effort) | Windows 10/11 64-bit target platforms; x64-compatible system; Microsoft Visual C++ x64 Runtime (included by the installer); native ASIO driver or Windows Audio fallback |

Audio-interface testing is on **macOS ARM with a Focusrite Scarlett Duo**,
using CoreAudio. ASIO-compatible interfaces are supported on Windows; ASIO is
not the macOS audio backend. RAM, storage, and CPU needs depend on project media,
track count, and plug-ins; fixed performance minima have not been validated.

**Linux is unsupported.** Covering audio-driver integrations and different
distributions is beyond the available time of the one-person development team.

## Build

Requirements: CMake 3.25+, Git, a C++20 compiler, and either macOS with Xcode
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
- [Website development and GitHub Pages](docs/website.md)
- [User guide](docs/user-guide.md)
- [Product, roadmap, and technical design](docs/design.md)
- [Development and architecture](docs/development.md)
- [Bundled devices](docs/devices.md)
- [Mastering and release](docs/mastering.md)
- [DAWproject interchange](docs/dawproject.md)
- [Studio One import](docs/studio-one.md)
- [Contributing](docs/contributing.md)
- [Releasing](docs/releasing.md)

## Website

The static product website lives in `website/`, separate from the native app.
With Node.js 24 LTS, run `cd website && npm ci && npm run dev` and open the
printed URL at `/studio-duo/`. `npm run build` produces `website/dist/`.
Website changes are checked on pull requests and deployed to GitHub Pages
only from `main`; select **GitHub Actions** in the repository's Pages settings.
See [website development](docs/website.md) for browser checks, screenshots,
and deployment details.

## License

Studio Duo is licensed under the
[GNU Affero General Public License v3.0 only](LICENSE). JUCE 9 uses its AGPLv3
option. On Windows, Studio Duo uses JUCE's dual-licensed Steinberg ASIO SDK
headers under their GPLv3 option. Signalsmith Stretch 1.4.0, Signalsmith Linear
0.6.5, CLAP 1.2.10, and clap-helpers are MIT licensed.
libsamplerate 0.2.2 is BSD-2-Clause licensed.
ARA SDK 2.3.0 is Apache-2.0 licensed.
The bundled, encoder-only LAME 4.0 library is LGPL-2.0-or-later licensed and
includes documented portability fixes;
its license, library and application source archives, scalar build configuration,
and rebuilding/relinking notice ship with the application.
The Signalsmith Stretch and Linear MIT license notices also ship with the application.
The vendored DAWproject 1.0 schemas and upstream XML example are MIT licensed.
