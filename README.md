# Studio Duo

<p align="center">
  <img src="assets/branding/studio-duo-logo.svg" alt="Studio Duo" width="720">
</p>

[![CI](https://github.com/mbianchidev/studio-duo/actions/workflows/ci.yml/badge.svg)](https://github.com/mbianchidev/studio-duo/actions/workflows/ci.yml)

[Website](https://mbianchidev.github.io/studio-duo/) ·
[Download](https://github.com/mbianchidev/studio-duo/releases/latest) ·
[User guide](docs/user-guide.md) ·
[Build from source](#build-from-source) ·
[Contribute](docs/contributing.md)

Studio Duo is an open-source digital audio workstation (DAW) for macOS and
Windows, built in C++20 with JUCE 9. Record, edit, compose, mix, and master music
in a native desktop application, with bundled drums, guitar and bass amps, and
reamping tools focused on modern metal production.

**Under active development.** The capabilities below are implemented, but
published `1.x` releases are not a sign-off on full production qualification.
See [status and limits](#status-and-limits) before relying on a release.

<img width="1707" height="1107" alt="Studio Duo desktop workspace" src="https://github.com/user-attachments/assets/21b07b83-1ca7-4809-9e0d-e17088a1581e" />

## What works today

- **Record and edit multitrack audio.** Sample-aligned recording, take lanes,
  comping, linked edits, transient tools, elastic audio, fades, and crossfades.
  Tempo/meter maps, count-in, punch, routed click, named markers, and editable
  loops support tracking and arrangement. See the [user guide](docs/user-guide.md).
- **Compose MIDI and drums.** Live, recorded, and retrospective MIDI; piano-roll
  and drum editors with per-note expression, probability, drum maps, seeded
  humanization, and multi-output routing. Twelve performance pads and a
  [drum geometry view](docs/user-guide.md#drum-geometry) share the same editable
  clips; the synthesized drum instrument includes style presets and kit tuning.
- **Mix with plugins and automation.** Cycle-safe sends, sidechains, auxes,
  nested buses, folders, VCAs, control room, hardware routing, and delay
  compensation. Serial/parallel racks and sample-accurate automation work with
  bundled devices and sandboxed VST3/CLAP plugins, plus Audio Units on macOS.
  See [plugin setup and isolation modes](docs/user-guide.md#use-plugins).
- **Build and compare guitar/bass tones.** Thirteen [bundled devices](docs/devices.md)
  cover drums, guitar/bass amp and cabinet DSP, dynamics, EQ, and utilities.
  Load custom cabinet IRs, create hardware or plugin reamp paths, compare
  level-matched snapshots, freeze/print tracks, and batch-render reports.
- **Save and exchange sessions.** Versioned `.studioduo` packages include
  recovery points, portable copies, SHA-256 media validation, and missing-file
  repair. [DAWproject 1.0 interchange](docs/dawproject.md) transfers media and
  plugin state with schema validation and compatibility reports;
  [Studio One migration](docs/studio-one.md) also supports direct `.song` import
  within its documented limits.
- **Master and deliver releases.** Sequence multiple songs and alternate mixes,
  edit gaps/fades/metadata, measure BS.1770/R128 loudness and true peak, and
  export WAV, AIFF, FLAC, Ogg Vorbis, or MP3 with encoding, normalization, and
  dither controls. [Mastering](docs/mastering.md) includes signed reports and
  integration with an external licensed DDP encoder.

## Status and limits

Tracking, editing, mixing, plugin hosting, MIDI composition, interchange, and
mastering implement Phases 1 through 5 of the [accepted design](docs/design.md).
The design also describes future work, not just current behavior. Phase 6
hardening and candidate-specific qualification remain open.

The [release-readiness matrix](docs/release-readiness.md) distinguishes automated
regression coverage from required physical-device, vendor/ARA, accessibility,
long-session, clean-machine, and upgrade checks. Green CI alone does not qualify
those workflows. The [qualification runbook](docs/qualification.md) describes
the remaining procedures and evidence.
The gate includes distribution and independent mastering references; native CI
also compares synthetic mono/stereo metrology against FFmpeg and exercises
atomic macOS update replacement without opening the desktop.

- VST3 and CLAP are supported on both platforms; Audio Units are macOS-only.
  VST2 is not enabled. Third-party **Trusted in-process** and **ARA 2** modes
  require a saved project and explicit consent, and do not contain plugin
  crashes or hangs.
- Direct Studio One `.song` import covers supported audio/mixer data, not
  private MIDI, automation, or processor state. Use DAWproject for broader
  transfer and review the compatibility report.
- DDP delivery requires a separately installed licensed encoder adapter and
  independent validation; no DDP encoder is bundled.

## Platform support and requirements

| Platform | Requirements |
| --- | --- |
| macOS | macOS 12 Monterey or later; Apple silicon (ARM64) or Intel (x86_64); CoreAudio-compatible interface or built-in audio |
| Windows (best effort) | Windows 10/11 x64; Microsoft Visual C++ x64 Runtime (included by Setup); native ASIO driver or Windows Audio fallback |

**Windows support is best effort only.** This is a one-person development team
without a Windows testing device, and limited development time restricts
hands-on Windows testing. Automated Windows CI does not replace physical
audio-interface and MIDI-device testing.

Audio-interface testing is on **macOS ARM with a Focusrite Scarlett Duo**,
using CoreAudio. ASIO-compatible interfaces are supported on Windows; ASIO is
not the macOS audio backend. RAM, storage, and CPU needs depend on media, track
count, and plugins; fixed performance minima have not been validated. Audio
preparation currently decodes whole files, so large sessions can require
substantial memory.

**Linux is unsupported.** Covering audio-driver integrations and different
distributions is beyond the available time of the one-person development team.

## Install

Get the [latest release](https://github.com/mbianchidev/studio-duo/releases/latest);
developer tools are not needed to run a published build. Download the package
for your platform and `SHA256SUMS.txt` from the same release, and verify the
package's SHA-256 checksum before installing.

### macOS first launch

Open the DMG and drag **Studio Duo.app** into **Applications** first
(`/Applications/Studio Duo.app`). Current macOS releases are ad hoc signed,
not Apple-notarized, so Gatekeeper may block the first launch.

Only continue if you trust the download from the
[official GitHub release](https://github.com/mbianchidev/studio-duo/releases/latest)
and have verified its SHA-256 checksum against `SHA256SUMS.txt` from that
release. If macOS blocks the installed app, open **Terminal** and run:

```sh
xattr -dr com.apple.quarantine "/Applications/Studio Duo.app"
```

Reopen **Studio Duo** from **Applications**. This removes the quarantine flag
only from this app; it does not verify the download or make an unsafe app safe.
Gatekeeper remains enabled globally.

### Windows

Run `Studio-Duo-<version>-Windows-x64-Setup.exe`, then open **Studio Duo** from
the Start Menu. Setup includes the Microsoft Visual C++ x64 Runtime.
Alternatively, extract `Studio-Duo-<version>-Windows-x64.zip` into a writable
directory and run `Studio Duo.exe`; the runtime requirement still applies.
Signing does not establish public-CA trust or SmartScreen reputation.
See [installation and distribution details](docs/releasing.md#install-released-artifacts).

Installed builds check for updates at launch and download verified packages in
the background by default. Installation waits for **Restart and Update**.
Portable Windows ZIP builds must be replaced manually.

## Your first session

1. Launch Studio Duo and choose **New Song**, or **Create From Template** for
   Metal Tracking, Songwriting, or Mix Session layouts.
2. Open **Settings** (gear icon) > **Audio / MIDI**. Select your audio device,
   enable the required inputs/outputs, and check the sample rate and buffer
   size. On macOS, allow microphone access when prompted.
3. Use **Add Track** to create an audio track, choose its input, and save with
   `Command/Ctrl+S` to a `.studioduo` directory package **before recording**.
   Recorded media will be stored inside the package's `media/` directory.
4. Arm only the intended tracks, start recording with the circle control, and stop
   with the square control. The completed take should appear on the timeline.
   To try playback without recording hardware, use **Import Audio**
   (`Command/Ctrl+I`) instead. Press `Space` to play/pause, then save your edits.
5. Choose **Export > Export audio** for a mix file, or
   **Export > Open mastering and release workspace** for album delivery.
   MP3 encoding is built in and needs no separate encoder installation.

Input monitoring is off by default to prevent accidental feedback; enable it
only when intended. For MIDI, drum pads, routing, and editing shortcuts, open
**Help > User Guide**. The indexed, searchable guide is embedded, works offline,
and matches the installed application version.

## Configuration

Settings persist across launches. Use the gear icon to configure:

| Settings tab | What to configure |
| --- | --- |
| General | Autosave recovery after edits, enabled by default for already-saved projects. Manual Save still publishes the durable project generation. |
| Appearance | Four dark themes and custom surface/text/accent colors with contrast validation. |
| VST Plug-ins | Default/custom VST3 folders, startup folder discovery, manual rescan, and explicit advanced validation. |
| Controls | Optional eight-strip Mackie Control profile, disabled by default. Select dedicated MIDI ports, separate from musical MIDI routing. |
| Updates | Manual checks and automatic-download preferences. Updates are installed only after choosing Restart and Update. |

In the processor catalog, choose **SCAN** to probe installed VST3, CLAP, and
macOS Audio Unit plugins outside the main process, then **ADD** to insert one
on the selected track. Bundled devices are listed without a third-party scan.
See [plugin setup](docs/user-guide.md#use-plugins) and
[control-surface setup and qualification limits](docs/user-guide.md#mackie-control-surfaces).

## Build from source

You need CMake 3.25+, Git, a C++20 compiler, and either macOS with Xcode
command-line tools or Windows with Visual Studio 2022's C++ desktop tools.
Python 3.9+ is required for the test build below and qualification tools, not
to run the application. CMake fetches pinned native dependencies during
configuration, so the first configuration needs network access.

```sh
git clone https://github.com/mbianchidev/studio-duo.git
cd studio-duo
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DSTUDIO_DUO_BUILD_TESTS=ON
cmake --build build --config Release --parallel 2
ctest --test-dir build --build-config Release --output-on-failure --no-tests=error
```

To omit tests and their Python requirement, configure with
`-DSTUDIO_DUO_BUILD_TESTS=OFF` and skip `ctest`.

Launch the built application on macOS:

```sh
open "build/StudioDuo_artefacts/Release/Studio Duo.app"
```

On Windows, from PowerShell:

```powershell
& ".\build\StudioDuo_artefacts\Release\Studio Duo.exe"
```

These commands use an explicit Release configuration. For universal macOS
builds, native test binaries, compilation caching, and focused test runs, see
the [development guide](docs/development.md).

## Startup troubleshooting

If a device cannot be restored, startup offers **Retry**, **Open I/O**, or
**Start without audio** rather than blocking the window. Launch-only recovery
switches are available on both platforms:

| Switch | Effect |
| --- | --- |
| `--safe-audio` | Skip automatic audio/MIDI hardware initialization. |
| `--safe-start` | Safe audio plus bypassing third-party runtime restoration, including ARA. |
| `--default-audio` | Ignore the saved audio device setup for this launch. |

These switches do not delete the saved device setup. **Help > Startup
diagnostics** retains step history and exports a privacy-safe JSON report.
See [startup troubleshooting and logs](docs/user-guide.md#logs-and-diagnostics).

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
- [Native project format and recovery](docs/project-format.md)
- [Contributing](docs/contributing.md)
- [Releasing](docs/releasing.md)
- [Release readiness and compatibility matrix](docs/release-readiness.md)
- [Candidate qualification procedures and evidence](docs/qualification.md)

## Contributing

Focused contributions are welcome. Read the [contributing guide](docs/contributing.md)
for build/test commands, real-time audio rules, dependency expectations, and
the pull-request workflow. Open an
[issue](https://github.com/mbianchidev/studio-duo/issues) before large
architectural changes or new dependencies.

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
