# Studio Duo native project format

Studio Duo format version 3 is a directory package with immutable generation
files and content-addressed processor state.

```text
Project.studioduo/
  manifest.json
  session/generation-XXXXXXXX.json
  automation/generation-XXXXXXXX.json
  plugin-state/<sha256>.bin
  media/
  analysis/
  recovery/latest.json
  recovery/in-process-active.json
  renders/tones/*.wav
  renders/tones/*.report.json
```

## Save transaction

1. Capture active processor state outside the audio callback.
2. Write new state blobs only when their SHA-256 hash is new.
3. Write the next session and automation generation files.
4. Flush those files.
5. Atomically replace `manifest.json` so it selects the new generation.
6. Write `recovery/latest.json`.

The prior manifest and generation remain valid until step 5 succeeds.

## Manifest

`manifest.json` contains:

- `formatVersion`
- `applicationVersion`
- stable project identity
- active session and automation generation paths
- generation number and save time
- `requiredCapabilities`

Paths must be relative children of the package and cannot contain `..`.

## Session document

The session document stores transport, tempo and meter maps, tracks, clips,
take/comp state, edit groups, reamp routes, the typed routing graph, processor
records, tone snapshots, mixer snapshots, and render reports. Automation lanes
are stored separately.

External and bundled processor records retain a stable insert ID, format,
vendor, version, architecture, isolation mode, state path/hash, latency, tail,
ARA capability, missing state, and recovery-disabled state.

## Automation document

The automation generation stores stable lanes, targets, points, timebase,
interpolation, trim offset, and enablement. Targets can address track controls,
routes, bundled-device parameters, or external plugin parameters.

## Recovery

`recovery/latest.json` contains the latest complete project state.
`recovery/in-process-active.json` records trusted or ARA instances active during
the current run. If the application exits uncleanly, those instances reopen
disabled until explicit reload. Clean shutdown removes the marker.

## Migrations

- Version 1 projects gain direct-to-master routes.
- Version 2 `outputTrackId` values become version 3 main-output connections.
- Versions 1 and 2 gain empty automation, snapshot, and report collections.
- The manifest and referenced session format versions must agree.

The schemas in [`schema/`](schema/) document the current public envelope.
