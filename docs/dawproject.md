# DAWproject 1.0 interchange

Studio Duo imports and exports the open
[DAWproject 1.0](https://github.com/bitwig/dawproject) interchange format
through `src/dawproject_io/`. DAWproject is not the native engine or persistence
model; translation keeps the two object models independent.

## Application workflow

Open **DAWPROJECT** in the main header.

- **Import DAWproject 1.0...** selects a `.dawproject` ZIP and a destination
  `.studioduo` package. A successful import opens that new project.
- **Export DAWproject 1.0...** captures current plug-in state into a temporary
  export-only package, then writes a `.dawproject` archive.
- **View latest compatibility report** displays the report from the most recent
  import/export or the last report persisted in the project.
- **Save latest compatibility report...** writes its structured JSON form.

Import never edits the source archive or external media. Export never edits
referenced source media or rewrites native media/processor state. Its
compatibility report follows the normal project dirty/recovery lifecycle.

## Container and validation

Every archive contains root-level `project.xml` and `metadata.xml`, followed by
sorted `media/` and `plugin-state/` entries. Studio Duo validates both XML
documents against the authoritative schemas from DAWproject tag `v1.0.0`,
commit `1651bf069601be47b028052f9c6dd5d20a05ddb5`.

The vendored files are:

- `third_party/dawproject/v1.0.0/Project.xsd`
- `third_party/dawproject/v1.0.0/MetaData.xsd`
- `third_party/dawproject/v1.0.0/LICENSE`
- `third_party/dawproject/v1.0.0/examples/bitwig-readme-project.xml`

The schemas are embedded into the application. Validation performs no runtime
network access. It enforces XSD sequences/choices, required attributes, simple
types, enumerations, lists, abstract types, unique XML IDs, and resolved
IDREFs. A second semantic pass checks the 1.0 rules the XSD cannot express,
including:

- Exact `Project version="1.0"`
- Finite string-encoded numbers and Studio Duo-supported ranges
- One parameter or expression per automation target
- Homogeneous automation point types
- MIDI channel, key, and normalized velocity ranges
- Positive media format/duration values
- At least two ordered warp points
- Safe embedded paths and embedded-only plug-in state
- No clip that combines inline content with a shared reference

ZIP import rejects duplicate/unsafe paths, symlinks, excessive entry counts,
oversized XML, and excessive total expansion before extraction.

## Transactions and reproducibility

Import validates first, extracts/copies assets into a sibling staging package,
translates and validates the complete internal model, saves it using native
generation persistence, and reopens it. Only then is the staging directory
renamed to a new requested destination. Existing destination packages are
rejected rather than temporarily moved or replaced. A failure leaves both the
currently open project and destination unchanged.

Export builds and validates both XML documents before writing a sibling
temporary archive. It verifies the staged ZIP and only then atomically replaces
the destination. Failed exports do not leave a partial destination.

For reproducible archives, Studio Duo uses:

- Kind-specific deterministic XML IDs
- Native semantic ordering for tracks/devices/clips and sorted payload paths
- Fixed XML element/attribute creation order
- Locale-independent six-decimal numeric formatting with trailing-zero removal
- UTF-8 and LF line endings
- Fixed ZIP timestamps and stored, uncompressed entries written as a stream
- ZIP local headers containing CRC and sizes rather than deferred descriptors

With identical input bytes and model state, repeated exports are byte-identical.

## Supported mapping

| Studio Duo | DAWproject 1.0 |
| --- | --- |
| Project name and metadata | `metadata.xml` fields |
| Base tempo/signature and maps | `Transport`, `TempoAutomation`, `TimeSignatureAutomation` |
| Song sections | Arrangement `Markers` |
| Track order and folder hierarchy | Nested `Track` elements |
| Audio, MIDI, instrument, aux, bus, VCA, master | Track `Channel` role/content |
| Main outputs and audio sends | Channel destination and `Send` |
| Volume, pan, mute, solo | Channel parameters/attribute |
| Plug-in/bundled insert descriptors | VST, CLAP, AU, built-in, or generic device |
| Captured plug-in state | Embedded `State` payload |
| Audio clips, offsets, fades, gain | `Clip`, `Audio`, and gain expression points |
| Stretch mode/rate and warp markers | Audio algorithm and `Warps` |
| MIDI clips and notes | `Clip` and `Notes` |
| Pitch, pressure, timbre, controller expression | Note-level `Points` |
| Track/send/device automation | Parameter-targeted `Points` |
| Scenes and per-track stop/clip slots | `Scenes`, `Scene`, and `ClipSlot` |
| Embedded or external source media on import | Copied into native `media/` |

Imported DAWproject IDs are deterministically mapped to native IDs. Channel,
track, parameter, route, scene, and slot aliases are registered explicitly so
all reconstructed relationships stay stable across repeated imports and native
save/reopen.

## Compatibility reports

Every unsupported source or destination item produces a report issue with:

- `severity`
- Stable `code`
- Affected object path containing the track, clip, note, device, route,
  parameter, scene, or slot
- Human-readable message

Warnings do not prevent import/export when the supported subset remains valid.
Errors stop publication and remain visible in the workflow.

The current reported compatibility boundary is:

- Studio Duo input routing/monitoring, record arm, hardware-output selection,
  solo-safe, track/clip polarity, and control-room state
- Take/version, active-playlist, comp-region, linked-edit, reamp, tone snapshot,
  mixer snapshot, render-report, drum-map, pattern-alias, MIDI routing-template,
  metronome, punch/count-in/pre/post-roll, and transport-loop semantics
- Sidechain, hardware, per-insert/bus, disabled/muted, and MIDI routes that have
  no DAWproject 1.0 routing representation
- Clip reverse, tagged-1.0 clip enable/mute, fade curve shape, transient cache,
  and recoverable source bounds; active range, fades, gain, stretch, and warps
  still export
- MIDI probability, editor mode, drum-map/articulation/choke/cymbal/foot-control,
  round-robin, and saved humanization parameters; rendered note timing,
  velocity, duration, channel, and supported expressions still export
- Track-level generic MIDI/expression automation and unsupported parameter
  targets
- Video, arbitrary scene timelines, duplicate slots per track, unresolved
  shared clip references, and nested clip/marker constructs that cannot be
  flattened without changing meaning
- Unknown vendor warp algorithms, which import as generic polyphonic stretch
- VST and Audio Unit preset-container portability: captured/imported bytes are
  preserved. Native DAWproject preset containers re-export byte-for-byte but
  remain safe missing placeholders until an adapter can restore them; Studio
  Duo-captured opaque VST/AU state exports through a generic device instead of
  being mislabeled as a native preset
- DAWproject AU state/identifier conventions, which 1.0 does not fully define
- ZIP64 and archives at or above the current 2 GiB classic-ZIP safety limit

These entries are intentionally not hidden or converted into success-shaped
defaults.

## Automated coverage

`tests/DawProjectTests.cpp` covers the official schemas and upstream example,
schema order/types/IDREFs, semantic numeric checks, unsafe archives, complete
supported round trips, metadata, hierarchy, routing, clips, notes, note
expressions, automation, devices, scenes, warps, embedded/external media,
plug-in state, source immutability, deterministic archive bytes, transactional
failure, compatibility reports, report JSON, stable IDs, native persistence,
and migration.
