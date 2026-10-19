# Release readiness and Phase 6 qualification

[Phase 6](https://github.com/mbianchidev/studio-duo/issues/7) is a qualification
milestone, not a version-number check. A published `1.x` tag does not establish
that the complete version 1.0 exit criteria have passed. Keep the tracker open
until the exact release candidate has both automated and hands-on evidence.
Never infer a hardware, vendor, accessibility, signing, or upgrade pass from an
unrelated synthetic test.

## Automated release gate

CI and tagged releases build and run the native suite on macOS and Windows.
The `hardening` CTest label selects the release-critical coverage:

| Area | Evidence |
| --- | --- |
| Startup | Coordinated progress/history, first paint, large/corrupt catalogs, unavailable devices, safe recovery, and worker timeouts; installed driver-free startup |
| Persistence/recovery | Eight abrupt child-process exits around temporary-file flush/publication for session, automation, manifest, and recovery; immutable prior/orphan generations; simultaneous-writer exclusion and lock release after termination |
| Format/migrations | Committed version 1/2 fixtures, ordered version 3-12 cases, stable legacy route IDs, source immutability, identity/schema/counter checks, and rejection of unknown required capabilities/future versions |
| Native exchange | Each platform creates the same complete synthetic package; both native readers open both packages in a separate dependent CI job |
| Keyboard/accessibility | Native timeline focus, selection role/value, chronological audio/MIDI clip navigation, musical nudges, offscreen-selection reveal, button activation, and named mastering controls |
| Engine/processing | Routing, linked edits, recording, automation, latency/racks, deterministic live/offline renders, sandbox crash/reload, and public-standard plugin fixtures |
| Qualification tooling | Full-media one-second smoke at 96 kHz, concurrent graph/callback work, complete live/offline null, portable validation, raw callback CSV, and fail-closed six-area evidence matrix contracts |
| Interchange/mastering | Official embedded DAWproject schemas, transactional failures, compatibility reports, portable/missing-resource checks, analytical fixtures, independent FFmpeg mono/stereo metrology and multi-song delivery |
| Packaging | macOS DMG/updater ZIP signatures, binary equality, license/source material, private install/startup/removal; disposable Windows signed install, same-version replacement, startup, and user-data-preserving uninstall |
| Update failure handling | Verified/cancellable download publication; actual embedded macOS payload against signed synthetic versions, atomic directory exchange, signal rollback and generation preservation after an untrappable kill |

The exchange package contains stable identities, Unicode text, audio edits,
take/comp data, bus/send routing, automation, MIDI expressions and a full-width
humanization seed, racks/missing-plugin placeholders, calibrated-route metadata,
scenes, compatibility warnings, mastering songs, and package-relative synthetic
PCM. Verification compares the complete reopened native model and every PCM
sample, rather than merely checking that a file exists. No driver or vendor
plugin is loaded by the exchange verifier.

Tagged publication depends on both platform builds **and** both cross-platform
exchange readers. Windows release signing verifies signature integrity and the
configured signer, temporarily trusting only that public certificate on the
disposable runner. CI uses an ephemeral certificate through the same verification
path and checks that temporary trust is removed. These checks do not establish
public-CA trust or SmartScreen reputation.

## Reproduce the gate

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DSTUDIO_DUO_BUILD_TESTS=ON
cmake --build build --config Release --parallel
ctest --test-dir build --build-config Release --parallel 2 \
  -L hardening --output-on-failure --no-tests=error
```

Run the complete suite before release, not only the label. CTest writes
`build/Testing/phase6-performance.json` for the performance suite. CI retains
that numeric report, JUnit results, and failure logs in `release-hardening-*`
artifacts, plus native-exchange fixtures and test packages. Reports describe
only synthetic data, architecture/platform, counts, and measurements.

Create a fresh native exchange artifact on macOS:

```sh
cmake \
  "-DTEST_BINARY=$PWD/build/StudioDuoTests_artefacts/Release/StudioDuoTests" \
  "-DOUTPUT_DIRECTORY=$PWD/build/phase6-exchange" \
  -P cmake/StudioDuoExchangeFixture.cmake
```

Use `StudioDuoTests.exe` on Windows and absolute paths in PowerShell. Existing
fixture packages are deliberately not overwritten. To verify an extracted
foreign-platform package, use the **local platform's** test executable:

```sh
./build/StudioDuoTests_artefacts/Release/StudioDuoTests \
  --verify-exchange-fixture=/absolute/path/to/project.studioduo
```

The verifier returns nonzero for missing/changed media, model differences, an
unexpected recovery path, or source mutation.

## Synthetic performance profile and budgets

The regression profile has 128 audio tracks, eight buses, 2,048 clips, 8,192
automation points, and a song-section range extending its timeline to thirty minutes.
Its shared stereo source is **0.125 seconds**, and its arranged audio is one
second long. This exercises edit density, graph rebuilding, and late-viewport
painting; it does **not** prove full-length streaming or establish minimum
hardware requirements.

| Measurement | Regression budget |
| --- | --- |
| Native save / load | Each under 10 seconds |
| Completed graph publication / refresh | Each under 5 seconds |
| Callback batch: 128 blocks of 256 samples | Under 10 seconds wall time and 5 seconds process CPU |
| Longest measured callback | Under 100 ms |
| Offline reference render | Under 5 seconds |
| Each 1024 x 600 software-rendered late viewport | Under 500 ms; at most 8 rows, 128 clips, and 128 grid iterations visited |
| Peak process working set | Under 1 GiB |
| Saved native documents/recovery | Under 32 MiB, excluding the external synthetic source |
| Unclipped deterministic live/offline null | Maximum absolute sample error below `1e-5` |

These generous hosted-runner limits catch pathological regressions. They are
not real-time audio deadlines: 256 samples at 48 kHz have a roughly 5.33 ms
deadline. Physical-device deadline misses and realistic long-media memory/disk
behavior require the qualification below.
The pixel/CPU fixture uses an explicit software image so Windows Direct2D
deferred GPU submission cannot turn an immediate pixel read into a false blank
frame. Native accelerated presentation remains part of hands-on UI qualification.

Prepared PCM is immutable and shared across clips and published snapshots,
instead of being copied into every publication/template. Metadata-only updates
can reuse it on local APFS using file identity and nanosecond change time, or
local NTFS using identity, change time, and the file's USN journal record.
Size/mtime alone never authorize reuse. Other filesystems decode freshly on
each update. Preparation rechecks source revisions before publishing and rejects
observed changes without replacing the active graph. Both replaced and in-place
same-size/same-mtime mutations have regression coverage.

The cache holds weak references only; retired/removed PCM is not kept alive by
the cache. Decode scratch is limited to 64 MiB of small sources plus the most
recent large source. Whole-file preparation remains, so this is not a streaming
guarantee or a fixed minimum RAM claim. The opt-in buffer matrix retains separate
complete callback/null results for all five buffer sizes at each selected rate.

## Candidate compatibility matrix and blockers

Every hands-on row is **not run** until candidate-specific evidence is attached.
Record the commit/artifact hashes, OS/architecture, relevant driver/plugin/tool
versions, settings, procedure, expected tolerance, actual measurements, and
pass/fail/blocker result. Use synthetic media and metadata; omit device serials,
personal paths, customer sessions, license material, and signing credentials.

| Qualification | Required coverage | Blocking issue |
| --- | --- | --- |
| Realistic performance | Full-length multitrack/multi-song sessions, CPU deadlines, peak memory, disk, UI, graph edits, and rendering on all supported architectures | [#59](https://github.com/mbianchidev/studio-duo/issues/59) |
| Physical hardware | macOS ARM64/Intel CoreAudio; Windows 10/11 x64 ASIO/fallback; audio/MIDI, professional I/O, control surfaces, calibrated reamp alignment | [#58](https://github.com/mbianchidev/studio-duo/issues/58) |
| Vendor processors | VST3/CLAP on both platforms, AU on macOS, ARA 2, state/editors/routing, latency/automation/nulls, crash/hang recovery | [#55](https://github.com/mbianchidev/studio-duo/issues/55) |
| Accessibility | Keyboard-only full production workflow, VoiceOver/NVDA, focus restoration, magnification/high DPI, themes/contrast, and drag alternatives | [#60](https://github.com/mbianchidev/studio-duo/issues/60) |
| Distribution | Clean supported OS installs, production signing/trust and Gatekeeper policy, **different-version** upgrades, actual updater interruption/rollback, clean uninstall/reinstall | [#56](https://github.com/mbianchidev/studio-duo/issues/56) |
| Trusted references | Independent BS.1770/R128/true-peak measurements, external DDP verification, and external-DAW interchange/visible unsupported-data reports | [#57](https://github.com/mbianchidev/studio-duo/issues/57) |

The accepted design explicitly allows opt-in ARA/trusted in-process modes with
reduced crash isolation. Sandbox fixture success cannot establish containment
for these modes. The [qualification policy](qualification.md#vendor-and-reduced-isolation-policy)
requires saved-project, mode-specific consent and a successful recovery point
before third-party opt-in. It requires separate vendor/mode results and an
explicit human release disposition for the in-process exception. No such
disposition is inferred from consent, recovery, or green CI; the tracker's
unqualified crash-containment criterion remains blocked until that decision.

The [qualification runbook](qualification.md) defines repeatable procedures,
provisional workstation budgets, and the `scripts/qualify.py` evidence gate for
issues #59, #58, #55, #60, #56 and #57. Its schema-version-2 matrix has all 24
target/area runs, each initially **not run**. Different-version preservation,
minimum/current OS records, production trust and independent numerical reference
coverage are required; historical four-area reports cannot pass the complete gate.
The opt-in long-media command is driver-free computational characterisation,
not a physical deadline or vendor pass. Whole-file decoding/prepared storage
remains a resource risk; the preflight estimate and phase peaks expose it rather
than advertising streaming. The optional dedicated-port Mackie Control profile
now has protocol, connection, musical-exclusion, undo/automation and native
settings contracts. Its physical model/firmware mappings and feedback remain
unqualified. HUI, MIDI learn and extender/layer support are not implemented;
ordinary musical note/CC routing still cannot substitute for surface testing.

The pinned public Surge XT VST3/CLAP software matrix now runs on native ARM64
macOS, Intel macOS and hosted Windows x64, with Apple AUBandpass on macOS.
It retains independent-state/concurrent-worker, declared-latency null,
trusted-reference, nested-rack, auxiliary-route and effect recovery results.
Per-state sidechain response skips remain explicit. These driver-free artifacts
do not establish physical deadlines, Windows 10/11 device compatibility,
licensed ARA workflows, vendor hangs during state capture, or a human release
disposition.

Native keyboard contracts include precise gain/fade editing, take/comp menu
actions, current track/clip state values, track-name validation and a scrollable
200-message status history with stable selection. They do not establish actual
VoiceOver/NVDA behavior or completion of the manual production journey.

Native CI separately retains 36 independent FFmpeg metrology comparisons at
44.1/48/96 kHz with fixed 0.1 LU integrated, 1.0 LU LRA and 0.3 dBTP tolerances.
Original PCM, exact source/tool/test-binary hashes and both programme/peak raw
logs stay with the candidate. This does not replace licensed DDP verification,
external-DAW acceptance, encoded-delivery checks or human release approval.
Synthetic macOS updater fixtures do not establish actual released-app upgrades,
clean-machine Gatekeeper behavior, production Windows trust or a complete
post-restart workflow.

For blocker triage, attach a minimal synthetic reproducer and candidate evidence,
classify data-loss/crash, unusable primary workflow, deadline/resource, or
compatibility failure, and link its focused fix and rerun. A missing device,
vendor, trusted reference, certificate, or testing system is **blocked/not run**,
never a pass. Windows remains best effort as documented in the README.

Release sign-off requires every exit criterion to have relevant evidence, no
unresolved release blockers, and the clean-install record/edit/mix/master/export
journey on supported systems. Do not close Phase 6 or promise a readiness date
based solely on green CI.
