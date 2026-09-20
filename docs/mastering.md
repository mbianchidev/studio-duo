# Mastering and release

Studio Duo's **MASTERING** workspace assembles several finished mixes into one
release. It is separate from the arrangement and mixer: source mixes are
treated as immutable release inputs, while sequencing, fades, metadata,
analysis, exports, and collection remain part of the `.studioduo` project.

## Album workflow

1. Open **MASTERING** in the main header.
2. Add each final mix with **+ SONG**.
3. Add alternate mixes to the selected song with **+ ALT MIX**, then choose the
   active source from **Source mix**.
4. Set the gap, overlap, fade-in, fade-out, and gain for each song. Reorder songs
   with **UP** and **DOWN**.
5. Enter album and song metadata. ISRC values are normalized to the 12-character
   form without spaces or hyphens.
6. Add comparison material with **+ REFERENCE**. Reference files remain outside
   release rendering, album output gain, and export processing.
7. Run **ANALYZE ALBUM**, then export a master, DDP fileset, or portable copy.

Each song has a stable ID, a selected source, any number of alternate sources,
and relative index markers. The saved sequence derives each song start from the
preceding end plus its gap minus its overlap. Fades and per-song gain are
applied before album output gain.

## Loudness analysis

`MasteringEngine` reports:

- ITU-R BS.1770-4 integrated loudness with 400 ms blocks, 100 ms hops, the
  -70 LUFS absolute gate, and a relative gate 10 LU below the absolute-gated
  mean
- EBU Tech 3342 loudness range from 3-second short-term windows, 100 ms hops,
  the -70 LUFS absolute gate, the -20 LU relative gate, and the 10th/95th
  percentiles
- sample peak and oversampled true peak
- stereo correlation

K-weighting coefficients are generated for the source sample rate. File LRA
finalization includes 1.5 seconds of analysis silence. The automated calibration
fixture checks a generated 1 kHz tone at -23 LUFS and verifies that true-peak
analysis detects an inter-sample peak.

The built-in distribution presets are reporting targets only. Studio Duo does
not turn a streaming or broadcast target into automatic loudness normalization.
The signed report records the selected preset, measured output, and any target
or true-peak warning.

## Master exports

**EXPORT MASTER** opens the same encoding settings as mix export:

- WAV (16/24-bit PCM or 32-bit float), AIFF, and FLAC masters
- Ogg Vorbis quality and MP3 constant-bitrate or variable-bitrate references
- Sample rate, mono/stereo, FLAC compression, optional peak normalization,
  and deterministic TPDF dither for integer output
- An Audio CD preset for 44.1 kHz / 16-bit stereo WAV with TPDF dither

Source-rate conversion uses libsamplerate's band-limited best-quality sinc
converter, and a selected source is rendered continuously into the album
timeline before final quantization. TPDF noise is deterministic at each
channel/sample position. The selected format and processing settings are part
of the signed settings hash. MP3 encoding is bundled; no separate installation
is needed. Peak normalization is a separate opt-in sample-peak adjustment, not
the distribution preset's LUFS target or true-peak limiting.

After encoding, Studio Duo decodes and remeasures the final file. The adjacent
`.report.json` includes source, sequence/settings, and output hashes; format,
sample rate, bit depth, duration, loudness, LRA, true peak, sample peak, and
correlation; warnings; and an RSA signature.

The per-install signing key is stored in the Studio Duo application-data
directory under `Signing/render-report-key.json`. The signature detects report
changes and identifies the local signing key. It is not a publisher certificate,
code-signing identity, notarization, or substitute for a release authority.
Verification requires that trusted local key (or an independently distributed
copy of its public key); the public key embedded in a report is metadata and is
never trusted by itself.

## DDP export

DDP's byte-level specification is licensed by DCA. Studio Duo therefore does not
copy that specification into the AGPL source or claim independent Red Book/DDP
conformance. **EXPORT DDP** uses a separately installed, licensed encoder
adapter.

The adapter is launched with:

```text
<encoder> --input-wav <44.1-kHz-16-bit-album.wav> \
          --cue <album.cue> \
          --output <empty-output-directory>
```

Studio Duo validates MCN/EAN check digits and requires every track/index start
to align to a 1/75-second CD sector. It generates the album WAV and CUE input,
then requires the adapter output to contain non-empty `DDPID`, `DDPMS`,
`PQ_DESCR`, and at least one `*.DAT` image. It writes
`CHECKSUMS.sha256`, reuses the final audio measurements, and signs
`render-report.json`.

Replication delivery still requires acceptance by a licensed independent DDP
validator or the destination plant. Exact lead-in, lead-out, PQ/subcode,
CD-Text, and Red Book checks remain the licensed encoder and validator's
responsibility.

## Portable copies and repair

**PORTABLE COPY** gathers arrangement clips, mastering sources, references, and
project plugin-state files. Media is copied to `media/<sha256>.<extension>` and
all package-local paths are serialized as `${PROJECT_DIR}/...`, so moving the
package does not preserve machine-specific absolute paths.

`portable-manifest.json` records every collected object path, relative file
path, byte size, and SHA-256 hash. The copy also contains
`analysis/portable-copy-report.json`. Validation rejects unsafe paths, missing
files, size mismatches, and hash mismatches.

**REPAIR FILES** scans the selected folder recursively. A missing resource with
a saved hash is repaired only from matching content. Legacy resources without
a hash fall back to an exact filename match. Unresolved files remain explicit
report entries; Studio Duo never silently substitutes different media.

Mastering data and media hashes were introduced in format 9 and remain in
format 10. Version 8 and
older projects migrate with an empty mastering album.
