# Studio Duo product and technical design

Status: accepted product direction

Date: 2026-08-30

## Summary

Studio Duo is a complete desktop DAW for recording, editing, mixing, and
mastering music. It is useful for general music production, but its fastest
workflows target modern metal.

The first audience is the solo musician producing a full record at home.
Independent producers and professional engineers recording large metal bands
must not outgrow it. Collaboration matters only as reliable project exchange.
Studio Duo has no server component.

The interface follows the conventions of established professional DAWs. The
product earns its place through faster metal production, not unfamiliar basic
controls.

## Fixed decisions

| Area | Decision |
| --- | --- |
| Platforms | macOS and Windows |
| Core technology | Native C++20 and JUCE |
| License | AGPLv3 core |
| Business model | Paid optional plugins and content |
| Main interface | Single window with dockable panels |
| Plugin formats | VST3, AU on macOS, CLAP, and ARA 2 |
| Plugin safety | Scanning and standard DSP execution are sandboxed; ARA uses an explicit compatibility mode |
| Native project format | Documented, versioned Studio Duo format |
| Interchange | Complete DAWproject 1.0 import and export |
| Collaboration | Portable bundles and deterministic session exchange |
| Hardware control | MIDI learn and Mackie Control/HUI-class surfaces |
| AI | Never part of the product |
| Cloud | No cloud service or required account |

## Product principles

1. **A full DAW, not a companion tool.** A user can start with an empty project
   and finish a release master without opening another DAW.
2. **Metal workflows are first-class.** Grouped performance editing, DI and
   reamp management, drum programming, dense routing, and aggressive vocal
   production need fewer manual steps.
3. **Familiar controls win.** Tracks, clips, lanes, buses, sends, inserts,
   automation, comping, and a conventional timeline behave as experienced
   engineers expect.
4. **Everything important is non-destructive.** Editing, comping, timing,
   tuning integration, reamping, freezing, and mastering preserve source
   material.
5. **Projects survive machines and years.** Saves are versioned, documented,
   crash-safe, and explicit about missing media or plugins.
6. **Open standards come first.** Studio Duo hosts modern plugin formats and
   treats DAWproject interchange as a core feature.
7. **Real-time safety outranks visual polish.** The audio thread never waits on
   disk, UI, plugin scanning, allocation, or locks.
8. **No generated decisions.** Studio Duo can use deterministic signal
   analysis, transient detection, pitch detection, and seeded humanization.
   It does not include AI assistants, generation, or cloud models.

## Visual identity

The Studio Duo mark is one intense-red engineered waveform without an enclosing
letterform. It represents controlled signal editing without relying on initials,
guitars, skulls, speakers, or generic play-button imagery.

Brand colors inherit the application interface: near-black `#101214`, panel
black `#171a1d`, warm off-white `#e7e4df`, and signal red `#ff2525`. The icon
must remain legible at 16 px and the horizontal
logo keeps the mark intact rather than redrawing it for marketing use.

## Version 1.0 scope

Version 1.0 must support the whole production path.

### Recording and transport

- Mono, stereo, and multichannel audio recording
- MIDI recording with retrospective capture where the backend permits it
- Input monitoring, direct-monitoring integration, and low-latency mode
- Punch in/out, loop recording, pre-roll, post-roll, and count-in
- Take lanes, playlists, comping, and alternate takes
- Sample, beat, bar, timecode, and musical position displays
- Native metronome with custom sounds, accents, subdivisions, and routing
- Tempo maps with ramps and abrupt changes
- Arbitrary time-signature changes
- CoreAudio on macOS
- ASIO for professional Windows devices, with WASAPI as a fallback

The transport and tempo map are project services. The click is routable to
headphones, control-room outputs, print tracks, or nowhere in the final render.

### Audio editing

- Non-destructive clips with slip editing, fades, crossfades, gain, polarity,
  transpose, reverse, stretch, and warp markers
- Sample-accurate split, trim, move, duplicate, ripple, and nudge operations
- Configurable snap, grid, relative snap, and transient snap
- Elastic audio with algorithms selected for drums, monophonic sources,
  polyphonic sources, and full mixes
- Clip and track freeze, bounce in place, consolidate, and flatten
- Spectral and waveform views backed by rebuildable analysis caches
- Unlimited undo and redo within practical memory limits
- A persistent operation journal for crash recovery

Phase 0 must select or build an AGPLv3-compatible stretch engine and test it on
multitrack drums, distorted guitars, vocals, and full mixes. Proprietary
stretch libraries cannot become a required part of the open core. Version 1.0
does not ship until the selected engine meets documented timing, phase, and
artifact thresholds.

### Linked multitrack performance editing

This is the flagship Studio Duo workflow.

An edit group can link guitar microphones, doubled guitars, bass DI and amp
tracks, or a full drum recording. Splits, comp selections, slip edits, fades,
warps, and quantization apply across the group while preserving phase and
relative timing.

The workflow includes:

- Group-level transient detection with per-track weighting
- A designated timing reference, such as kick, snare, or guitar DI
- Phase-locked editing for multichannel drums and multi-mic cabinets
- Group comping across take lanes
- Quantization strength, timing exclusions, and protected anchor points
- Gap closing and crossfade generation with editable defaults
- Audition before commit
- One command to remove or suspend group edits without touching source files

All analysis is deterministic. The same sources, settings, and application
version produce the same markers and edits.

### MIDI and metal drum editing

Studio Duo has a standard piano roll and a dedicated drum editor.

The drum editor adds:

- Named kit pieces and articulations
- Importable and editable drum maps
- Choke groups, cymbal states, foot control, and round-robin hints
- Velocity, timing, duration, probability, and articulation lanes
- Fast flam, roll, gravity blast, blast beat, and double-kick entry tools
- Scalable humanization with a saved random seed
- Pattern aliases that can be expanded into ordinary editable notes
- Multi-output routing templates

The editor remains ordinary MIDI underneath. Projects do not depend on the
bundled drum instrument.

### Reamp and tone comparison

DI is the source of truth for guitar and bass production.

Studio Duo models reamping as a non-destructive relationship between a source
track and one or more tone paths. A tone path can use plugins, external
hardware, or both.

Required features:

- DI and printed tone pairing
- Reamp send and return routing
- Hardware round-trip latency calibration
- Polarity and alignment tools
- Named tone snapshots that capture routing, plugin state, and level
- Instant level-matched A/B between snapshots
- Batch rendering for several tones
- Freeze and print without removing the source path
- Clear stale-render indicators after a DI or tone-chain change

### Mixing

- Audio, instrument, MIDI, aux, bus, folder, VCA, master, and control-room
  tracks
- Arbitrary sends, sidechains, parallel paths, and nested routing
- Pre-fader and post-fader metering
- Gain, pan, mute, solo, solo-safe, polarity, and channel configuration
- Plugin delay compensation across playback, sidechains, and offline renders
- Sample-accurate read, touch, latch, write, trim, and preview automation
- Track versions, mixer snapshots, and scoped snapshot recall
- Mono, stereo, surround-ready internal routing, and flexible hardware I/O
- Stem, bus, selected-track, and full-mix rendering
- Deterministic offline rendering with a real-time fallback for hardware

### Mastering

Mastering is a dedicated workspace, not a larger master-bus panel.

It supports:

- Album sequencing with gaps, fades, transitions, and track markers
- Several source mixes per song
- Integrated loudness, loudness range, true peak, peak, and correlation meters
- ITU-R BS.1770 and EBU R128 analysis
- Reference tracks that bypass the processing chain and output gain
- ISRC, album, artist, songwriter, and release metadata
- Dither and sample-rate conversion
- DDP export with validation
- WAV and FLAC masters
- Platform-supported compressed reference exports
- Distribution presets without forcing a target loudness onto the mix
- A signed render report containing source hashes, settings, warnings, and
  measured output values

### Plugin hosting

Version 1.0 hosts:

- VST3 on macOS and Windows
- Audio Units on macOS
- CLAP on macOS and Windows
- ARA 2 extensions for compatible VST3 and AU plugins

Plugin discovery runs outside the main process. A failed scan records the
plugin, error, and crash report, then continues with the remaining plugins.

Third-party plugins run in bridge processes. Audio and event data use bounded
shared-memory queues. Sandboxed processing uses a fixed one-block pipeline so
the audio callback never waits for another process. Plugin delay compensation
accounts for that block. A late result substitutes the last valid output or
silence, increments a diagnostic counter, and keeps playback moving. If a
bridge fails, the engine keeps the project open, marks the instance as crashed,
and offers an explicit reload.

ARA 2 requires synchronous access to the host document model and cannot use the
standard block bridge without a much larger proxy implementation. Version 1.0
therefore provides a clearly marked ARA compatibility mode that runs the ARA
plugin instance in the main process after a sandboxed scan. Studio Duo saves a
recovery point before activation and warns that the instance has reduced crash
isolation. A future out-of-process ARA proxy can remove this exception.

Users may also opt a trusted standard plugin into in-process low-latency mode
for monitored recording. Sandboxed mode remains the default.

Plugin state is saved as opaque data with the plugin identifier, format,
vendor, reported version, architecture, and a content hash. A missing plugin
keeps its routing, automation, and state so another machine can restore it.

VST2 and AAX hosting are outside version 1.0. AAX is a Pro Tools plugin target,
not a general third-party host format.

## Bundled devices

The bundled suite is small by design. It lets a new installation complete a
project while leaving advanced sound design to third-party and paid plugins.

- Parametric EQ
- Compressor
- Limiter with true-peak mode
- Algorithmic reverb
- Noise gate
- Gain, polarity, delay, tuner, and signal generator utilities
- Simple guitar amp with cabinet loading
- Simple bass amp with cabinet loading
- Drum composition instrument with a basic bundled kit

The devices use the same DSP core inside Studio Duo and in standalone VST3,
AU, and CLAP builds where the format applies. Core devices remain part of the
AGPLv3 repository. Paid products remain separate standard plugins.

## Scream Forge integration

[Scream Forge](https://github.com/mbianchidev/scream-forge) is the preferred
paid vocal processor, but Studio Duo does not require it.

Integration uses public plugin standards:

- Normal VST3 or AU hosting
- ARA 2 source access for graphical pitch editing
- Factory vocal track and bus templates
- Preserved plugin state and automation in Studio Duo and DAWproject exports
- A clear missing-plugin placeholder when Scream Forge is unavailable

Studio Duo may identify an installed Scream Forge plugin and offer relevant
templates. The open-source host must not link to proprietary Scream Forge code
or depend on its license service.

## Interface

Studio Duo uses one main window with saved workspaces and dockable panels.

The default production workspace contains:

- A top transport with record state, position, tempo, time signature,
  metronome, loop, punch, and performance meters
- A left browser and inspector
- A central arrangement timeline
- A lower editor for audio, MIDI, drums, automation, or plugin parameters
- A right routing and channel inspector
- A mixer that can replace the timeline or open as a docked lower panel

Editing stays keyboard-friendly. Every command has a searchable command ID and
can receive a shortcut. Context menus expose the same commands rather than
separate hidden behavior.

The visual design is dark and restrained. Metal character comes from typography,
meter behavior, waveform contrast, and a small accent palette. It does not use
decorative flames, distressed textures, or hard-to-read novelty controls.

MIDI learn is available for plugin parameters, mixer controls, transport, and
commands. Mackie Control and HUI-class mappings cover common control surfaces.

## Native project format

The native extension is `.studioduo`.

During editing, a project is a directory package rather than one repeatedly
rewritten archive. Its documented structure contains:

```text
Project.studioduo/
  manifest.json
  session/
  automation/
  plugin-state/
  media/
  analysis/
  recovery/
```

Rules:

- `manifest.json` declares the format version, application version, stable
  object IDs, media policy, and required capabilities.
- Session and automation data use documented JSON schemas.
- Plugin state and audio remain binary files referenced by content hash.
- Analysis files are caches. Deleting them never changes the project.
- Every save writes session, automation, plugin state, and changed metadata to
  new generation paths. It never reopens a file referenced by the prior
  manifest for writing.
- Saves flush the new generation, then atomically replace the manifest that
  selects it.
- The recovery journal records commands since the last complete save.
- Schema migrations are ordered, tested, and never overwrite the only copy.
- Unknown fields survive a read and save when possible.
- Missing files and plugins produce a repair report, not a silent substitute.

A normal project may reference external media. "Save portable copy" gathers
all media and verifies hashes. A portable bundle can be archived for transfer,
but Studio Duo expands it before normal editing.

The project schema is public and versioned in the repository.

## DAWproject support

Studio Duo implements the open
[DAWproject](https://github.com/bitwig/dawproject) 1.0 exchange format. The
format is a ZIP container with `project.xml`, `metadata.xml`, media, and plugin
state.

DAWproject is not the native Studio Duo format because its stated non-goals
include native DAW persistence, application view state, and optimal runtime
performance.

Import and export are still first-class features:

- Import creates a new Studio Duo project and never mutates the source file.
- Export includes every item representable by DAWproject.
- Tempo, time signatures, tracks, channels, routing, clips, notes, note
  expressions, automation, devices, scenes, warps, and embedded plugin states
  map through a dedicated translation layer.
- Studio Duo validates output against the official XML schemas.
- Unsupported source or destination data appears in a structured report.
- The application never silently drops unsupported items.
- Every warning identifies the affected track, clip, device, or parameter.
- Round-trip tests cover the official examples and generated edge cases.
- Compatibility tests use files from supporting DAWs when their licenses allow
  redistribution.

The internal model must not copy DAWproject's object model. Importers and
exporters translate between the two so the engine can evolve independently.

DAWproject scenes map to an internal scene model from the start. The first
release can import, preserve, and export them even though the live session view
ships later.

## Technical architecture

Studio Duo is a native C++20 application built with JUCE. Major components have
one-way dependencies toward the project model and real-time engine.

```text
Application shell and workspaces
        |
Commands, undo, project services
        |
Timeline, MIDI, routing, automation, mastering
        |
Real-time audio graph and render engine
        |
Audio/MIDI backends and plugin bridges
```

Proposed modules:

| Module | Responsibility |
| --- | --- |
| `studio_app` | Startup, windows, workspaces, commands, preferences |
| `project_model` | Stable IDs, tracks, clips, devices, routing, undo commands |
| `project_io` | Native schema, migration, recovery, portable bundles |
| `audio_engine` | Real-time graph, scheduling, latency, transport, metronome |
| `timeline` | Audio edits, comping, grouping, warps, tempo and meter maps |
| `midi` | MIDI recording, piano roll, drum maps, expressions, control |
| `plugin_host` | Discovery, state, automation, format adapters, ARA |
| `plugin_bridge` | Sandboxed scan and execution processes |
| `mix_engine` | Routing, buses, sends, automation, snapshots, control room |
| `mastering` | Album assembly, analysis, metadata, DDP, release renders |
| `dawproject_io` | DAWproject XML mapping, validation, compatibility reports |
| `devices` | Bundled DSP and instrument implementations |
| `studio_ui` | Shared controls, panels, meters, accessibility, themes |

### Real-time rules

- No heap allocation on the audio callback
- No mutex acquisition, file access, logging, XML, JSON, or blocking IPC on the
  audio callback
- Preallocated bounded queues between real-time and non-real-time work
- Sandboxed plugin exchange uses a fixed pipeline and a hard deadline. A late
  bridge never stalls the callback.
- Immutable graph snapshots swapped at safe boundaries
- Sample-accurate event and automation scheduling
- Explicit latency values for every node and path
- Denormal handling and deterministic reset behavior
- Glitch counters and diagnostics collected without blocking

Offline rendering uses the same graph and scheduling code as playback. It can
run faster than real time only when every active node permits it.

### Editing model

User actions become typed commands. Commands update the project model and
produce a new engine snapshot when playback state changes. The command stream
drives undo, redo, autosave recovery, reproducible tests, and future scripting.

Audio files are immutable. Clip objects reference a source and store edit
decisions. Destructive file processing always creates a new source file.

### Security and failure handling

- Plugin scanners and bridges receive only required file and IPC access.
- The host records plugin crashes without taking down the project.
- Corrupt project data identifies the failing object and file.
- Importers enforce archive size, path traversal, XML entity, and resource
  limits.
- Recovery always writes to a new location before replacing user data.
- Logs have stable categories and include project-safe object IDs.
- Logs do not include audio, plugin state, personal metadata, or full project
  paths unless the user chooses a diagnostic export.

## Delivery plan

The plan assumes one primary developer with open-source contributions. The
scope is large and likely multi-year. No public 1.0 date should be promised
before the first vertical slice measures engine, plugin, save, and cross-platform
risk. Each phase ends with a working application and automated tests.

### Phase 0: foundations

- Confirm dependency and AGPLv3 compatibility
- Set supported macOS, Windows, CPU, and plugin architectures
- Establish CMake targets, coding rules, sanitizers, and continuous integration
- Define the native project schema and real-time coding rules
- Build audio and MIDI device diagnostics

### Phase 1: first vertical slice

- Create and open a project
- Record and play one audio track
- Draw, move, split, and trim clips
- Scan and host one sandboxed plugin
- Save, close, reopen, and recover the project
- Export a stereo WAV

This phase proves the project model, engine boundary, plugin bridge, and save
format before feature breadth grows.

### Phase 2: professional tracking and editing

- Multitrack recording
- Tempo, meter, metronome, punch, and loop systems
- Take lanes and comping
- Linked multitrack editing
- Stretch, warp, transient, fade, and consolidation tools
- Hardware reamp routing, calibration, and the core DI relationship model
- Basic VST3 tone paths built on the Phase 1 host

### Phase 3: mixer and plugin platform

- Full routing graph, buses, sends, sidechains, VCAs, and control room
- Automation and plugin delay compensation
- VST3, AU, CLAP, and ARA 2 support
- Plugin crash recovery and compatibility database
- Bundled utility devices
- Complete plugin-backed reamp snapshots and batch rendering
- Scream Forge VST3, AU, and ARA 2 compatibility validation

### Phase 4: MIDI and composition

- MIDI recording and editing
- Piano roll
- Metal drum editor and maps
- Bundled drum instrument
- Guitar and bass amp devices
- Complete DAWproject import and export

### Phase 5: mastering and release

- Mastering workspace and album sequencing
- Loudness and true-peak analysis
- Metadata, DDP, distribution exports, and render reports
- Project collection, portable copies, and repair tools

### Phase 6: version 1.0 hardening

- Large-session performance work
- Accessibility and complete keyboard operation
- Cross-platform project exchange
- Plugin and hardware compatibility passes
- Crash recovery drills
- Documentation, migration tests, and release packaging

## Version 1.0 exit criteria

Studio Duo reaches 1.0 only when:

- A clean install can record, edit, mix, master, and export a multi-song
  release.
- A plugin crash does not close the DAW or corrupt the project.
- Killing the application during a save leaves either the prior valid save or
  a recoverable new state.
- The same project opens on macOS and Windows with identical edits and routing.
- Offline and real-time renders null within the expected tolerance when all
  plugins are deterministic.
- Automation remains sample-accurate across tempo changes and plugin latency.
- Linked drum edits preserve phase across all grouped tracks.
- A hardware reamp round trip aligns to the calibrated latency.
- DAWproject exports pass the official schemas and the compatibility test set.
- Unsupported DAWproject data always produces a visible report.
- Mastering analysis matches trusted reference files within documented
  tolerances.
- Every command in the primary workflow is reachable by keyboard.

## After version 1.0

The architecture reserves data models and command IDs for:

- Staff notation and guitar/bass tablature editing
- A live performance and clip-launcher session view
- More control-surface protocols
- Importers for other documented project formats

Notation and the session view are not required for 1.0. DAWproject scene data
must still survive import, save, and export before those interfaces exist.

## Explicit non-goals

- AI generation, assistants, recommendations, or cloud inference
- A Studio Duo cloud, account system, or collaboration server
- Real-time remote collaboration
- Video post-production
- A closed plugin ecosystem
- Replacing advanced third-party processors with a large bundled suite
- Novel timeline behavior that makes experienced DAW users relearn basic work

## Open decisions for phase 0

- Minimum macOS and Windows versions
- Intel macOS support duration
- Internal mix precision and supported channel layouts
- Exact native project JSON partitioning and migration API
- Plugin bridge isolation policy per instance, vendor, or trusted group
- DDP library and compressed-audio codec choices
- JUCE and dependency compatibility with AGPLv3 core distribution
- JUCE commercial licensing for any closed-source paid plugin that uses JUCE
