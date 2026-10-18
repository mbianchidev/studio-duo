# Candidate qualification procedures

This runbook covers [#59](https://github.com/mbianchidev/studio-duo/issues/59),
[#58](https://github.com/mbianchidev/studio-duo/issues/58),
[#55](https://github.com/mbianchidev/studio-duo/issues/55), and
[#60](https://github.com/mbianchidev/studio-duo/issues/60).
It is not their sign-off. Actual hardware, approved vendor processors, VoiceOver
and NVDA, and supported physical testing systems remain mandatory. Never
reinterpret a short fixture, virtual device, headless callback, or component
accessibility handler as that evidence.

## Candidate and evidence integrity

Use one exact, clean commit for each candidate. Configure after committing so
the characterisation binary embeds that SHA and a non-dirty build revision.
Retain the installed binary/artifact SHA-256, OS version, architecture, CPU,
RAM and storage class, driver/plugin/assistive-tool versions, sample rate,
buffer size, counts, expected tolerances, observations, and raw measurements.
Do not include serials, usernames/personal paths, customer media, licenses, or
license keys. Do not purge system caches or change a user's device configuration
to obtain a measurement. Preserve raw evidence privately until reviewed for
redaction, then hash the exact reviewed files.

Python 3.9+ and the standard library are sufficient:

```sh
candidate="$(git rev-parse HEAD)"
python3 -B scripts/qualify.py create --candidate-sha "$candidate" \
  --output build/qualification/evidence.json
python3 -B scripts/qualify.py check build/qualification/evidence.json \
  --candidate-sha "$candidate"
```

On Windows use `python`, `$candidate = git rev-parse HEAD`, and PowerShell path
syntax. Creation refuses to overwrite an existing report. The template contains
all four areas on macOS ARM64, macOS Intel, Windows 10 x64, and Windows 11 x64;
every check starts `not_run`. Fill each run's reproducible procedure, settings,
tool versions, timezone-qualified time, and manual attestation. Every required
check needs an observation and nonempty raw-evidence files with relative paths
and lowercase SHA-256 hashes. Evidence paths cannot escape the report directory,
including through symlinks. Different checks may reference the same detailed
raw matrix where its individual results are unambiguous.

`check` exits 0 only when the complete matrix, metadata, numerical budgets,
vendor format/role inventory, release disposition, and file hashes pass.
Exit 1 means malformed/unreadable input or an I/O error; exit 2 means incomplete
or failed qualification. `skip`, `blocked`, missing rows, duplicate checks,
wrong candidates, empty evidence, and changed files never pass. Duplicate JSON
keys and nonfinite numbers are invalid. This verifies completeness and integrity,
not the truth of a human observation or a signature: it cannot grant release
approval, certify accessibility, or verify an external approver's authority.

## Long-session resource and deadline qualification

### Driver-free characterisation

Build Release and the native tests as described in [development](development.md).
The opt-in command uses no audio/MIDI manager, opens no device, and produces no
audible output:

```sh
./build/StudioDuoTests_artefacts/Release/StudioDuoTests \
  --qualify-long-session \
  "--qualification-output=$PWD/build/qualification/long-48k-256" \
  --tracks=16 --songs=3 --song-seconds=180 --clips-per-song=4 \
  --sample-rate=48000 --buffer-size=256 --memory-budget-mib=16384
```

Use the `.exe` on Windows and an absolute, **new** output directory for every
run. Repeat at 96 kHz and 64/128/256/512/1024 samples. Profile arguments are
validated; unknown/duplicate settings fail. A low-memory dry preflight can be
obtained by selecting a smaller memory budget; the resulting `blocked` report
is useful evidence, not a successful playback run.

Add `--buffer-matrix` to run **64, 128, 256, 512, and 1024** samples against the
same generated fixture at the selected rate. Repeat the command at 96 kHz with
a fresh output directory. Source creation, native save/load and portable-copy
measurements are shared explicitly across cases at that rate; each case builds
its own engine and records complete playback, graph edits, callbacks and offline
null in a separate report/CSV. Any failing or blocked case fails the matrix.
PCM/packages are removed only after every case finishes; raw results remain.

The default is nine minutes of actual, unique stereo media per track, three
songs, split edits, one alternate take lane per track, gain inserts, a latent
master limiter, tempo changes, and 3,072 sample-accurate automation points.
This is not a long ruler over short repeated media. Native save/reopen, a
content-validated portable copy, first/middle/late software viewports, complete
callback playback, concurrent graph republication, and a complete offline/live
null are exercised. Graph edits rename a track and rebuild identical audio so
the full render remains comparable; destructive/audio-changing edits still need
the manual repeated-edit workflow. The callback output buffer is preallocated;
the measured interval excludes harness buffer allocation and CSV writing.
The null uses the graph's **declared** output latency and captures that many
extra live samples before comparison with the latency-trimmed offline range;
it never estimates a convenient offset from the measured audio.
The independent offline engine is configured to that same rate before rendering,
and its restored rate is retained and checked. Otherwise a fresh engine's default
48 kHz clock causes the render API to restore an unrequested second prepared
graph after a 96 kHz export. Cross-rate export/restoration can retain both rates'
PCM and requires separate memory measurements; it is not included in this
same-rate fixture's preflight.

The hosted CTest smoke verifies complete media, graph overlap, null accuracy,
and the CLI's outcome contract, with separate generous regression limits of
100 ms maximum callback and 10 seconds playback wall time. A shared runner can
preempt the callback past its much shorter audio deadline. Such a measurement
must still retain `status: fail`, a nonzero qualification outcome, its actual
miss counter and raw CSV; a passing **smoke contract test** does not convert it
to a deadline pass. The opt-in command and manual evidence gate keep the strict
zero-miss / 0.70 p99 requirements unchanged.

`report.json` records the embedded candidate/build, exact profile, source bytes,
estimated decoded/prepared exposure, process CPU and peak working set by phase,
platform I/O operation counters, decode count, save/load/portable/render timings,
graph-edit durations, callback p99/max/deadline misses, overlap count, late/drop
counters, viewport timing, and null error. `callbacks.csv` retains every measured
callback, its start lateness, deadline, and graph-work overlap flag. I/O counters
are OS-specific operations, **not disk throughput bytes**. Peak values are
process-lifetime high-water marks, not current resident-set measurements.
Source creation/read timings include processing and OS caching; first/repeated
reads do not certify cold storage throughput.

The default callback loop is accelerated. `--paced` schedules callbacks against
a monotonic clock and also counts harness scheduling lateness. Neither mode
measures a physical interface's driver deadline misses. Run at least three
ten-minute songs with `--song-seconds=600 --paced` for a thirty-minute sustained
session, on a dedicated qualification system with adequate capacity. Add longer
passes/repeated manual edits for a multi-hour soak. Do not increase the memory
budget beyond the testing system's available headroom.

Current playback prepares whole files/clips in memory. Immutable PCM is now
shared across clips/publication slots; the metadata-only edits in this fixture
do not create six duplicate full-media generations. Reliable local APFS or
NTFS/USN change revisions permit prepared-data reuse without trusting size/mtime.
An open Windows writer disables cross-update reuse until it closes.
Unsupported filesystems decode freshly rather than silently accepting stale
audio. Source changes observed before publication reject the new graph.

The preflight includes a prepared-data upper bound including alternate takes,
two large-source decode buffers, clip-processing scratch, 64 MiB of small-source
cache, both full stereo render buffers and 512 MiB overhead. It is specific to
this generated workload's metadata edits; destructive changes to all source
content can require simultaneous old/new prepared versions and still need the
manual repeated-edit qualification.
Phase checks stop further work after a measured budget
overrun. This is an estimate/soft guard, not a hard allocator or OS memory limit.
A thirty-minute or 96 kHz profile needs more memory than the default nine-minute
48 kHz example; select only headroom actually available on the testing system.
That result is a resource blocker for
that configuration, **not** a streaming guarantee. The exact point depends on
edits, takes, transient processing allocations, graph generations, and plugins.
Generated PCM/packages are removed from the owned `synthetic-workload` child
directory on normal completion/failure; reports and CSV stay. An externally
killed run can leave that explicitly named child for manual cleanup.

### Workstation budgets and physical measurements

These are provisional qualification gates for a dedicated workstation, not
advertised minimum hardware requirements or evidence of an achieved pass.
The evidence checker enforces them for every 48/96 kHz and supported buffer pair:

| Measurement | Gate |
| --- | --- |
| Sustained measured content | At least 30 minutes, 16 audio tracks, 3 songs |
| Physical callback deadline misses | Zero, including graph edits during playback |
| Callback p99 / buffer deadline | At most 0.70 |
| Peak working set | At most 75% of the recorded physical RAM |
| Warm open / native save | Each at most 10 seconds |
| Completed graph edit | At most 250 ms |
| Late viewport interaction | At most 50 ms |
| Deterministic, latency-aligned null | Maximum absolute sample error at most `1e-5` |

For each pair, retain the actual callback/driver counter trace and CPU sampling
procedure, media sizes/clip/insert/automation counts, initial and repeated open,
save, graph/edit, take/portable-memory observations, and disk throughput from a
dedicated OS measurement tool. Record whether caches were warm or genuinely
cold without claiming first-read equals cold-read. Include bundled and approved
third-party processors, plugin latency, tempo changes, and deterministic nulls.
Measure real-time and offline render durations and listening/alignment outcomes.
Any exceedance or missing measurement blocks that configuration. Triage whole-file
memory growth and synchronous graph rebuild cost rather than widening budgets
until a regression passes.

## Physical audio, MIDI, reamp, and control surfaces

Only a human at the testing system may configure/connect the hardware or start
calibration. The tools above never enumerate/open devices, change settings,
transmit MIDI, or invoke the calibration command. Keep an existing user's
configuration untouched; use a disposable test profile/session.

On macOS ARM64 and Intel, qualify CoreAudio. On real Windows 10 and 11 x64,
qualify the professional interface's native ASIO driver **and** Windows Audio
fallback. Record device model/driver version, clock source, channel labels,
active mono/stereo mappings, rates and buffer sizes, not serials. Use a
professional multi-I/O interface, not only built-in stereo audio.

1. From a clean candidate installation, import synthetic full-length sources,
   record, edit, mix, and export a three-song session. Record monotonic position
   references/drop counters and compare captured/exported timing for lost or
   duplicated audio/MIDI.
2. Verify monitoring, mono/stereo input maps, multichannel outputs, punch, loop,
   count-in, clock behavior and sample-rate/buffer changes. Disconnect/reconnect
   during idle, playback and recording; retain visible failure/recovery outcomes
   and verify the original project/media remain valid.
3. Send a synthetic MIDI sequence with documented note/channel/event timing,
   channel pressure and sustain; capture input/output and retrospective capture.
   Compare every expected event, channel and timestamp rather than counting
   only notes. Gate dropped and duplicated events/samples at zero.
4. Use only an explicitly configured, attenuated **line-level** loopback with
   monitoring/feedback paths safely disabled. Do not improvise speaker/headphone
   connections, emit calibration into an unknown route, or change gain/phantom/
   clock settings unattended. Confirm the selected output/input and safe level
   locally before choosing **Calibrate round-trip latency**.
5. Retain repeated physical impulse/correlation captures, measured round-trip
   latency, stored latency, signed fine-alignment offset, sample rate and buffer.
   Verify recorded residual after the stored latency plus offset is within
   **+/-1 sample**, including repeated passes and a reopen. Clock drift or an
   unstable result is a blocker, not an averaged-away pass.
6. Identify the actual supported surface/protocol and firmware; exercise transport,
   record/arm, fader/pan, bank/track selection, feedback and reconnect with a
   documented mapping and no unintended musical MIDI recording.

The common eight-strip/master **Mackie Control** profile is implemented in
**Settings > Controls**. Follow the [dedicated-port setup](user-guide.md#mackie-control-surfaces);
it defaults to disabled and must not share musical MIDI assignments. Its
synthetic protocol/connection tests cover mappings, differential feedback,
musical exclusion, stale/overflow safeguards, reconnect and undo/automation,
but are not a physical-device pass. Qualify the actual model/firmware and every
documented mapping, including touch and partial banks. HUI, MIDI learn, extenders,
timecode/meters and plug-in layers are not implemented by this profile; do not
advertise them or substitute ordinary note/CC routing.
Windows remains best effort until physical-system evidence exists.

## Vendor and reduced-isolation policy

Default external inserts remain sandboxed. Never auto-promote a failing vendor
to trusted mode to obtain a pass. Treat each exact vendor/version/architecture/
format/mode as a separate result. Use only legally installed, explicitly
approved processors; do not redistribute proprietary binaries, presets,
licenses, or customer sessions. The evidence inventory requires sandboxed VST3
and CLAP instruments/effects everywhere and Audio Units on macOS, plus separately
identified trusted and ARA-capable VST3/Audio Unit instances. Every processor
entry must record `approved: true`; this is the tester's recorded approval,
not permission granted by the script.

For each approved processor cover state/presets, multiple instances, native and
generic editors, sidechain/multi-output/nested-rack routing, bypass/reload,
latency/tails, tempo/sample-accurate automation, and real-time/offline export.
Disable random/analog/noise modes for deterministic null checks and document
the configured tolerance/reference. Keep stochastic listening/repeatability
results separate: a nondeterministic render cannot pass a deterministic null
by silently raising its tolerance.

**Advanced Validate** / `--validate-plugin <identifier>` is a supplemental
separate-process public-standard probe, not this entire vendor workflow.
Retain its actual checks/skips, not just its aggregate pass. Probe logs may
contain plugin paths; review/redact them before sharing.

For legally available processors held in an isolated test directory, the native
test executable also accepts an explicit source without scanning or changing
the user's persistent catalog:

```sh
./build/StudioDuoTests_artefacts/Release/StudioDuoTests \
  "--qualify-plugin=/absolute/isolated/processor.vst3" \
  "--qualification-output=$PWD/build/qualification/vendor-probe"
```

Use a `.clap` path or a registered `AudioUnit:` identifier for those formats.
Run each invocation in a bounded subprocess and isolate filesystem/network
access before loading third-party code. Never install a test processor into an
existing user's global plugin folders or use audible device output. Reports
retain exact processor version, manufacturer, architecture, role and every
standard check/skip, but always say `qualificationStatus: not-qualified`.
This command creates no host audio-device manager; processor code is still
third-party code, not a permission boundary. Its basic instantiation/state/
processing/editor probe is not the complete vendor/mode, crash-containment,
determinism or assistive-technology matrix.

`--exercise-sandbox-effect` additionally requires a stateful effect with an
observable initial response and a reported automatable continuous control.
It uses the real worker executable, exercises a non-quantum-aligned synthetic
loop, edits that named control through the actual generic native parameter
panel with a complete non-drag gesture, observes worker feedback, and requires a distinct
non-default state blob before saving the native package. It then terminates
only that worker, checks visible failure and dry fallback, verifies failed state
capture preserves the previous blob, reopens the package, and explicitly reloads
the edited state. Resumed processing and saved automatable parameter IDs/values
must survive; reloading only initial defaults is not a pass. Calls are paced at
30 ms per 256 samples to test exchange/recovery, **not** physical deadlines.
It does not simulate a vendor hang inside state capture or certify every mode.

On macOS, `scripts/vendor-probe-macos.sb` denies network access, Apple events,
and writes outside explicit work/output roots. Supply those parameters to
`sandbox-exec`, set a private child `HOME` and `TMPDIR`, and bound the executable
with a subprocess timeout. Source-built JUCE uses the inherited private
`TMPDIR` for its worker FIFOs. Do not weaken the profile to allow arbitrary
global pipe/user-directory writes. Some external tools, notably `auval -a`,
cannot complete inside that restricted context: `--omit-platform-tools` records
an **explicit skip**, and the independent registry/tool check must be retained
separately. The normal validator still runs supplemental tools by default.

Crash/hang tests use disposable synthetic sessions. For **sandboxed** instances,
exercise playback and state capture: the DAW must stay open/responsive, show
failure, preserve the prior valid package/state, and restore that saved state
only on explicit reload. An unavailable plugin or worker that never became
ready cannot pass crash recovery.

ARA 2 needs document binding, immutable source access, audio edits, tempo/meter
changes, archive restore, and reopening after an unclean exit with marked
instances disabled. Test trusted in-process recovery independently.

Both third-party reduced-isolation modes require a saved project, a warning
that a crash/hang can close/freeze the DAW, explicit mode-specific consent, and a
successful current recovery checkpoint before activation. Cancellation/failure
must preserve the previous mode. The selected mode persists per instance;
switching modes needs fresh consent. Recovery markers and safe-disabled reopen
are mitigation, **not containment**.
The checkpoint captures current processor state through the existing state
store first; an unsaved plugin editor change must not be replaced by an older
state hash simply because the native document was already saved.

The accepted design permits these opt-ins but the Phase 6 tracker has an
unqualified crash-containment exit criterion. Therefore a reduced-isolation
qualification needs an **explicit human release disposition**, rationale, and
hashed decision evidence in `reduced_isolation_disposition`. Runtime opt-in is
not that approval; the checker template defaults to unapproved. This PR/tool
does not approve an exception, change the accepted exit criterion, or count a
trusted/ARA crash as sandbox containment. Until the owner resolves that
criterion and all per-mode blockers, issue #55/Phase 6 remain blocked.

## Keyboard, VoiceOver and NVDA audit

Audit the actual installed candidate without desktop automation from the
qualification script. Record OS and VoiceOver/NVDA versions and every theme,
display scale and magnification setting used. First complete the journey using
keyboard only; repeat with VoiceOver on both Macs and NVDA on both Windows
versions. Native component tests are source contracts, not those runs.

| Journey | Required observations |
| --- | --- |
| New/open/import | Startup hub, menus, native choosers, cancel/error paths and focus restoration |
| Arm/record/hardware | Reach every control without drag, identify channels/values, stop/finalize, recover disconnect |
| Audio and MIDI edits | Timeline F6, arrows, musical nudges, Shift+F10, offscreen reveal, split/trim, piano-roll entry/resize/expression |
| Takes/comp/linked edits | Expand/select take lanes, create/adjust comp and linked operations without mouse-only steps |
| Routing/mix/automation | Inspector sliders, every existing route through Routing EDIT, mode/lane/target/value, writing and preview |
| Plugins | Insert tree navigation/actions, generic named parameters and native vendor editors, failure/reload/mode consent |
| Mastering/portable/export | Named song rows, reorder/metadata, source mixes, analysis, portable creation/repair, export and native destinations |
| Modals and status | Names/roles/values/selection, errors/warnings, Escape/Return, focus visible and restored to a sensible trigger |
| Themes and display | All four themes/custom validation, magnification/high DPI, non-color-only mute/solo/arm/error/selection states |

Component regression coverage now includes named generic parameter and
automation controls, discrete parameter keyboard steps, parameter-change
automation gestures without dragging,
a native **Routing EDIT** action for all existing routes, named mastering
song rows, opaque custom-color/text/focus contrast validation, and linear/rotary
slider focus outlines. Keep those contracts while auditing the rest.

No source-only review can certify modal focus restoration, native accelerated
focus rendering, assistive announcements, magnification, or third-party dialogs.
Record each reproducible blocker with severity, exact workflow/control,
expected/observed behavior, and a synthetic reproduction. An inaccessible
primary command, keyboard trap, lost focus preventing continuation, or missing
essential announcement blocks sign-off until fixed and retested on the actual
assistive technology. Source remediation is not a completed manual audit.
