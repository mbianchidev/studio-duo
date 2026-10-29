#!/usr/bin/env python3
"""Qualify Studio Duo mastering metrology and encoded delivery with independent FFmpeg tools."""

import argparse
from array import array
import hashlib
import json
import math
from pathlib import Path
import platform
import re
import shutil
import subprocess
import sys
import tempfile
import wave

from qualify import (REFERENCE_KINDS, REFERENCE_RATES, REFERENCE_TOLERANCES, digest,
                     number, read_json, reference_measurement_errors)

ROOT = Path(__file__).resolve().parents[1]


def delivery_cases():
    cases = []

    def add(format_name, rate, channels, depth, dither="none", mode="constant"):
        cases.append(dict(
            id=f"{format_name}-{rate}-{channels}ch-{depth}-{dither}-{mode}",
            format=format_name, sampleRate=rate, channels=channels, bitDepth=depth,
            dither=dither, mp3BitrateMode=mode))

    for rate in REFERENCE_RATES:
        for channels in (1, 2):
            for depth in (16, 24):
                add("wav", rate, channels, depth, "tpdf")
        add("wav", rate, 2, 32)
    for channels in (1, 2):
        add("aiff", 48000, channels, 24)
        add("flac", 96000, channels, 24)
        add("ogg", 48000, channels, 32)
        for rate in (44100, 48000):
            for mode in ("constant", "variable"):
                add("mp3", rate, channels, 32, mode=mode)
    return cases


def delivery_settings(case):
    return dict({key: value for key, value in case.items() if key != "id"},
                mp3BitrateKbps=320, mp3VbrQuality=2, oggQuality=6, flacCompressionLevel=5,
                normalizePeak=False, normalizePeakDbfs=-1.0, distributionPresetId="streaming-balanced")


DELIVERY_SOURCES = (("song-a.wav", 96000, 2), ("song-b.wav", 48000, 2),
                    ("song-c.wav", 44100, 1), ("excluded.wav", 48000, 2))
DELIVERY_METADATA = dict(title="Synthetic r\u00e9lease", artist="Fixture Ensemble",
                         album="Synthetic r\u00e9lease", date="2026-10-01", genre="Metal",
                         copyright="Synthetic material only")
DELIVERY_TOLERANCES = dict(losslessMaximum=0.0001, losslessRms=0.00001,
                           lossyMaximum=0.03, lossyRms=0.005,
                           nativeLossyMaximum=0.001, nativeDecodedRms=0.00001, **REFERENCE_TOLERANCES)


def write_delivery_fixture(directory):
    sources = {}
    for index, (name, rate, channels) in enumerate(DELIVERY_SOURCES):
        samples = array("i")
        if samples.itemsize != 4:
            raise ValueError("Synthetic delivery requires 32-bit signed PCM.")
        for sample in range(rate * 4):
            time = sample / rate
            taper = min(1.0, time / 0.02, (rate * 4 - 1 - sample) / (rate * 0.02))
            taper = (0.5 - 0.5 * math.cos(math.pi * taper))
            for channel in range(channels):
                phase = channel * 0.31
                frequency = (997, 613, 1703, 311)[index]
                level = (0.18, 0.05 + 0.1 * min(1.0, max(0.0, (time - 1.95) / 0.1)),
                         0.12, 0.7)[index]
                value = level * math.sin(2 * math.pi * frequency * time + phase)
                if index == 0:
                    value += 0.04 * math.sin(2 * math.pi * 30000 * time + phase)
                samples.append(round(taper * value * (2**31 - 1)))
        if sys.byteorder != "little":
            samples.byteswap()
        path = directory / name
        with path.open("xb") as stream, wave.open(stream, "wb") as output:
            output.setnchannels(channels)
            output.setsampwidth(4)
            output.setframerate(rate)
            output.writeframes(samples.tobytes())
        sources[name] = dict(id=name, name=name, file=name, sourceHash=file_hash(path), durationSeconds=4)
    tracks = []
    for index, name in enumerate(("song-a.wav", "song-b.wav", "song-c.wav")):
        tracks.append(dict(
            id=f"song-{index + 1}", title=f"Synthetic song {index + 1}", artist="Fixture Ensemble",
            songwriter="", isrc=f"USAAA260000{index + 1}",
            gapBeforeSeconds=(0.25, 0, 0.25)[index], overlapPreviousSeconds=(0, 0.5, 0)[index],
            fadeInSeconds=(0.125, 0.5, 0.25)[index], fadeOutSeconds=(0.5, 0.25, 0.125)[index],
            gainDecibels=(-3, -6, 0)[index], indexMarkersSeconds=[0, 2],
            sources=([sources["excluded.wav"]] if index == 0 else []) + [sources[name]],
            selectedSourceId=name))
    album = dict(title=DELIVERY_METADATA["title"], artist=DELIVERY_METADATA["artist"],
                 releaseDate=DELIVERY_METADATA["date"], genre=DELIVERY_METADATA["genre"],
                 copyright=DELIVERY_METADATA["copyright"], outputGainDecibels=-1, tracks=tracks,
                 references=[dict(id="excluded-reference", name="Excluded reference",
                                  file="excluded.wav", sourceHash=sources["excluded.wav"]["sourceHash"])])
    return dict(schemaVersion=1, album=album, exports=delivery_cases())


def assemble_delivery(album, sources, rate, channels):
    placements, end = [], 0.0
    for track in album["tracks"]:
        source = next(source for source in track["sources"] if source["id"] == track["selectedSourceId"])
        decoded = sources[source["file"]]
        start = max(0.0, end + track["gapBeforeSeconds"] - track["overlapPreviousSeconds"])
        frames = len(decoded["samples"]) // decoded["channels"]
        end = start + frames / rate
        placements.append((track, decoded, round(start * rate), frames))
    output = array("f", [0.0]) * (round(end * rate) * channels)
    for track, decoded, start, frames in placements:
        gain = 10 ** ((track["gainDecibels"] + album["outputGainDecibels"]) / 20)
        fade_in, fade_out = round(track["fadeInSeconds"] * rate), round(track["fadeOutSeconds"] * rate)
        for sample in range(frames):
            envelope = min(1.0, sample / fade_in) if fade_in else 1.0
            if fade_out:
                envelope *= min(1.0, (frames - 1 - sample) / fade_out)
            left = decoded["samples"][sample * decoded["channels"]]
            right = decoded["samples"][sample * decoded["channels"] + decoded["channels"] - 1]
            values = ((left + right) * 0.5,) if channels == 1 else (left, right)
            for channel, value in enumerate(values):
                position = (start + sample) * channels + channel
                output[position] += value * gain * envelope
    return output


def validate_delivery_stream(probe, case, metadata):
    streams = probe.get("streams") if isinstance(probe, dict) else None
    if not isinstance(streams, list) or len(streams) != 1 or not isinstance(streams[0], dict):
        raise ValueError("Delivery must contain exactly one independently readable audio stream.")
    stream = streams[0]
    codec = {"wav": {16: "pcm_s16le", 24: "pcm_s24le", 32: "pcm_f32le"}[case["bitDepth"]],
             "aiff": "pcm_s24be", "flac": "flac", "ogg": "vorbis", "mp3": "mp3"}[case["format"]]
    if (stream.get("codec_name") != codec or stream.get("sample_rate") != str(case["sampleRate"])
            or type(stream.get("channels")) is not int or stream["channels"] != case["channels"]):
        raise ValueError("Independent decoder found the wrong codec, rate or channel count.")
    if case["format"] not in ("ogg", "mp3"):
        depth = stream.get("bits_per_raw_sample")
        if depth in (None, "0"):
            depth = str(stream.get("bits_per_sample"))
        if depth != str(case["bitDepth"]):
            raise ValueError("Independent decoder found the wrong PCM precision.")
    tags = {}
    for container in (probe.get("format", {}), stream):
        values = container.get("tags", {}) if isinstance(container, dict) else None
        if not isinstance(values, dict) or any(not isinstance(value, str) for value in values.values()):
            raise ValueError("Independent decoder returned malformed metadata.")
        tags.update({key.lower(): value for key, value in values.items()})
    for key, value in metadata.items():
        if tags.get(key) != value:
            raise ValueError(f"Independent decoder found missing/changed {key} metadata.")
    return tags


def audio_difference(actual, expected):
    if not actual or len(actual) != len(expected):
        raise ValueError("Independent decode has the wrong complete programme frame count.")
    maximum, squared = 0.0, 0.0
    for measured, reference in zip(actual, expected):
        if not math.isfinite(measured) or not math.isfinite(reference):
            raise ValueError("Decoded delivery/reference contains nonfinite samples.")
        difference = abs(measured - reference)
        maximum = max(maximum, difference)
        squared += difference * difference
    return dict(maximumAbsoluteSampleError=maximum, rmsError=math.sqrt(squared / len(actual)))


def dither_statistics(samples, channels, depth):
    if channels not in (1, 2) or depth not in (16, 24) or len(samples) < channels * 1024:
        raise ValueError("Dither statistics require a complete bounded integer silence interval.")
    if len(samples) % channels:
        raise ValueError("Dither silence has incomplete interleaved frames.")
    lsb = 2 ** (1 - depth)
    stats, units = [], []
    for channel in range(channels):
        values = [value / lsb for value in samples[channel::channels]]
        if any(not math.isfinite(value) or value not in (-1, 0, 1) for value in values):
            raise ValueError("TPDF silence exceeds the quantized one-LSB bound.")
        mean = math.fsum(values) / len(values)
        rms = math.sqrt(math.fsum(value * value for value in values) / len(values))
        if abs(mean) > 0.025 or not 0.45 <= rms <= 0.55:
            raise ValueError("TPDF silence is absent, biased or has the wrong noise power.")
        stats.append(dict(meanLsb=mean, rmsLsb=rms,
                          histogram={str(value): values.count(value) for value in (-1, 0, 1)}))
        units.append(values)
    correlation = None
    if channels == 2:
        covariance = math.fsum(left * right for left, right in zip(*units)) / len(units[0])
        covariance -= stats[0]["meanLsb"] * stats[1]["meanLsb"]
        variance = [(row["rmsLsb"] ** 2 - row["meanLsb"] ** 2) for row in stats]
        correlation = covariance / math.sqrt(variance[0] * variance[1])
        if abs(correlation) > 0.05:
            raise ValueError("TPDF silence is correlated between channels.")
    return dict(channels=stats, interchannelCorrelation=correlation, frames=len(samples) // channels)


def validate_candidate(report, expected_sha):
    errors = []
    if not digest(report.get("candidateSha"), 40):
        errors.append("Native measurement lacks a complete candidate SHA.")
    if expected_sha is not None:
        revision = report.get("buildRevision")
        if (report.get("candidateSha") != expected_sha or not isinstance(revision, str)
                or not 7 <= len(revision) <= 40 or not expected_sha.startswith(revision)):
            errors.append("Native measurement is not the exact clean requested candidate.")
    return errors


def validate_delivery_native(report, fixture_hash, cases, expected_sha):
    if not isinstance(report, dict):
        return ["Missing native delivery manifest."]
    errors = validate_candidate(report, expected_sha)
    if type(report.get("schemaVersion")) is not int or report["schemaVersion"] != 1:
        errors.append("Wrong native delivery schema.")
    if report.get("fixtureSha256") != fixture_hash:
        errors.append("Native delivery used a different fixture.")
    rows = report.get("exports")
    if not isinstance(rows, list):
        return errors + ["Missing native delivery export cases."]
    expected, seen = {case["id"]: case for case in cases}, set()
    for row in rows:
        identity = row.get("id") if isinstance(row, dict) else None
        if not isinstance(identity, str) or identity not in expected or identity in seen:
            errors.append("Unknown, duplicate or malformed native delivery case.")
            continue
        seen.add(identity)
        case, settings = expected[identity], row.get("settings")
        wanted = delivery_settings(case)
        if (not isinstance(settings, dict) or settings != wanted
                or any(isinstance(settings.get(key), bool) != isinstance(value, bool)
                       for key, value in wanted.items())):
            errors.append(identity + ": native export settings changed.")
        for name in ("master", "repeat"):
            result = row.get(name)
            if (not isinstance(result, dict) or result.get("file") != f"{name}.{case['format']}"
                    or result.get("signatureVerifiedBeforeRedaction") is not True
                    or not digest(result.get("signedReportSha256"), 64)):
                errors.append(identity + ": missing verified final-file " + name + " report.")
                continue
            if (result.get("nativeDecodedFile") != name + ".native.f32le"
                    or type(result.get("nativeDecodedFrames")) is not int
                    or result["nativeDecodedFrames"] != case["sampleRate"] * 12
                    or type(result.get("nativeDecodedChannels")) is not int
                    or result["nativeDecodedChannels"] != case["channels"]):
                errors.append(identity + ": missing/wrong complete native verification PCM.")
            measurement = result.get("report")
            if not isinstance(measurement, dict):
                errors.append(identity + ": missing product measurement.")
                continue
            for key, value in (("scope", "mastering-album"), ("mode", "offline"), ("status", "success"),
                               ("format", case["format"]), ("sampleRate", case["sampleRate"]),
                               ("bitDepth", 0 if case["format"] in ("mp3", "ogg") else case["bitDepth"])):
                if measurement.get(key) != value or isinstance(measurement.get(key), bool):
                    errors.append(identity + ": wrong product report " + key + ".")
            for key in ("sourceHash", "chainHash", "outputHash", "settingsHash"):
                if not digest(measurement.get(key), 64):
                    errors.append(identity + ": missing product " + key + ".")
            for key in ("durationSeconds", "integratedLoudnessLufs", "loudnessRangeLu",
                        "truePeakDbtp", "samplePeakDbfs", "correlation"):
                if not number(measurement.get(key)):
                    errors.append(identity + ": missing/nonfinite product " + key + ".")
            if any(key in measurement for key in ("outputFile", "signingPublicKey", "signature")):
                errors.append(identity + ": private paths/signing material were not redacted.")
    if seen != set(expected):
        errors.append("Native delivery matrix is incomplete.")
    return errors


def file_hash(path):
    result = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            result.update(block)
    return result.hexdigest()


def parse_reference(programme_text, peak_text):
    values = {}
    for field, text in (("I", programme_text), ("LRA", programme_text), ("true_peak", peak_text)):
        matches = re.findall(r"\blavfi\.r128\." + field + r"=([^\s]+)", text)
        if not matches:
            raise ValueError("Missing independent R128 " + field + " metadata.")
        value = float(matches[-1])
        if not math.isfinite(value):
            raise ValueError("Nonfinite independent R128 " + field + " metadata.")
        values[field] = value
    if values["true_peak"] < 0 or values["LRA"] < 0:
        raise ValueError("Invalid independent linear peak or loudness range.")
    if values["true_peak"] == 0:
        return {"lufs": None, "lra": None, "dbtp": None}
    return {"lufs": values["I"] if values["I"] > -70 else None, "lra": values["LRA"],
            "dbtp": 20 * math.log10(values["true_peak"])}


def validate_native(report, source_hash, rate, channels, frames, expected_sha):
    if not isinstance(report, dict):
        return ["Missing native measurement report."]
    errors = []
    for field, expected in (("schemaVersion", 1), ("sourceSha256", source_hash),
                            ("sampleRateHz", rate), ("channels", channels), ("frames", frames)):
        if report.get(field) != expected or isinstance(report.get(field), bool):
            errors.append("Native measurement has the wrong " + field + ".")
    errors.extend(validate_candidate(report, expected_sha))
    values = report.get("measurements")
    if (not isinstance(values, dict) or set(values) != {"lufs", "lra", "dbtp"}
            or any(value is not None and not number(value) for value in values.values())):
        errors.append("Native measurement values are missing or nonfinite.")
    return errors


def write_source(path, kind, rate, channels):
    if kind not in REFERENCE_KINDS or rate not in REFERENCE_RATES or channels not in (1, 2):
        raise ValueError("Unknown synthetic metrology profile.")
    seconds = {"steady": 6, "gated": 12, "dynamic": 18,
               "intersample": 4, "silence": 4, "tail-transient": 4}[kind]
    frames = seconds * rate
    samples = array("i")
    if samples.itemsize != 4:
        raise ValueError("Synthetic PCM requires 32-bit signed integers.")
    for sample in range(frames):
        time = sample / rate
        amplitude = 0.1
        if kind == "silence":
            amplitude = 0.0
        elif kind == "gated":
            amplitude = (0.18, 0.000001, 0.08)[min(2, int(time / 4))]
        elif kind == "dynamic":
            amplitude = (0.03, 0.3, 0.1)[min(2, int(time / 6))]
        elif kind == "intersample":
            amplitude = 0.7 * min(1.0, time / 0.02, (frames - 1 - sample) / (0.02 * rate))
        elif kind == "tail-transient":
            amplitude = 0.02
        for channel in range(channels):
            angle = (2 * math.pi * sample / 4 + math.pi / 4 if kind == "intersample"
                     else 2 * math.pi * 997 * time + channel * 0.23)
            value = amplitude * math.sin(angle)
            if kind == "tail-transient" and sample == frames - 1:
                value = 0.8 if channel == 0 else -0.8
            samples.append(round(value * (2**31 - 1)))
    if sys.byteorder != "little":
        samples.byteswap()
    with path.open("xb") as stream, wave.open(stream, "wb") as output:
        output.setnchannels(channels)
        output.setsampwidth(4)
        output.setframerate(rate)
        output.writeframes(samples.tobytes())
    return frames


def write_json(path, value):
    with path.open("x", encoding="utf-8") as stream:
        json.dump(value, stream, indent=2, allow_nan=False)
        stream.write("\n")


def run_process(command, directory, replacements, timeout=90):
    with tempfile.TemporaryFile() as output:
        result = subprocess.run(command, cwd=directory, stdin=subprocess.DEVNULL,
                                stdout=output, stderr=subprocess.STDOUT, timeout=timeout, check=False)
        output.seek(0)
        raw = output.read(2 * 1024 * 1024 + 1)
    if len(raw) > 2 * 1024 * 1024:
        raise ValueError("The measurement process exceeded its bounded log size.")
    text = raw.decode("utf-8", errors="replace")
    for source, label in replacements:
        text = text.replace(source, label).replace(source.replace("\\", "/"), label)
    if result.returncode != 0:
        raise ValueError(f"Measurement process exited {result.returncode}: {text[-4096:]}")
    return text


def verify_candidate_checkout(expected_sha, directory, replacements):
    if expected_sha is None:
        return
    head = run_process(["git", "-C", str(ROOT), "rev-parse", "HEAD"], directory, replacements).strip()
    status = run_process(["git", "-C", str(ROOT), "status", "--porcelain", "--untracked-files=normal"],
                         directory, replacements).strip()
    if head != expected_sha or status:
        raise ValueError("Exact-candidate metrology requires the requested commit and a clean source checkout.")


def measure_reference(meter, source, rate, case, directory, replacements):
    programme = run_process(
        [str(meter), "-nostdin", "-hide_banner", "-nostats", "-i", str(source),
         "-af", "ebur128=metadata=1,ametadata=print", "-f", "null", "-"], directory, replacements)
    (case / "reference.log").write_text(programme, encoding="utf-8")
    peak = run_process(
        [str(meter), "-nostdin", "-hide_banner", "-nostats", "-i", str(source),
         "-af", f"apad=pad_len={rate // 5},ebur128=peak=true:metadata=1,ametadata=print",
         "-f", "null", "-"], directory, replacements)
    (case / "reference-peak.log").write_text(peak, encoding="utf-8")
    return parse_reference(programme, peak)


def read_float_pcm(path, samples):
    if path.stat().st_size != samples * 4:
        raise ValueError("Independent decode has the wrong complete programme frame count.")
    result = array("f")
    if result.itemsize != 4:
        raise ValueError("Independent PCM comparison requires 32-bit floats.")
    result.frombytes(path.read_bytes())
    if sys.byteorder != "little":
        result.byteswap()
    if any(not math.isfinite(value) for value in result):
        raise ValueError("Independent decoder returned nonfinite PCM.")
    return result


def write_float_pcm(path, samples):
    output = array("f", samples)
    if sys.byteorder != "little":
        output.byteswap()
    with path.open("xb") as stream:
        stream.write(output.tobytes())


def cleanup_delivery_private_files(output, cases):
    try:
        for case in cases:
            for name in ("master", "repeat"):
                (output / case["id"] / (name + ".report.json")).unlink(missing_ok=True)
    finally:
        signing = output / "private-signing"
        (signing / "render-report-key.json").unlink(missing_ok=True)
        if signing.exists():
            signing.rmdir()


def qualify_delivery(executable, meter, probe, directory, replacements, report, expected_sha):
    fixture = write_delivery_fixture(directory)
    request = directory / "fixture.json"
    write_json(request, fixture)
    fixture_hash = file_hash(request)
    native_output = directory / "native"
    try:
        log = run_process([str(executable), "--export-mastering=" + str(request),
                           "--delivery-output=" + str(native_output)], directory, replacements, timeout=300)
    finally:
        cleanup_delivery_private_files(native_output, fixture["exports"])
    (directory / "native.log").write_text(log, encoding="utf-8")
    native_bytes = (native_output / "report.json").read_bytes()
    native = read_json(native_bytes.decode("utf-8"))
    report["nativeManifestSha256"] = hashlib.sha256(native_bytes).hexdigest()
    problems = validate_delivery_native(native, fixture_hash, fixture["exports"], expected_sha)
    if problems:
        raise ValueError("; ".join(problems))
    report["candidateSha"] = native["candidateSha"]
    report["buildRevision"] = native["buildRevision"]
    report["fixtureSha256"] = fixture_hash
    oracle = directory / "independent-oracle"
    oracle.mkdir()
    expected_audio = {}
    for rate in REFERENCE_RATES:
        sources = {}
        for name, _, channels in DELIVERY_SOURCES[:3]:
            output = oracle / f"{name}-{rate}.f32le"
            log = run_process(
                [str(meter), "-nostdin", "-hide_banner", "-nostats", "-i", str(directory / name),
                 "-map", "0:a:0", "-af",
                 f"aresample={rate}:resampler=swr:filter_size=64:phase_shift=10:exact_rational=1",
                 "-c:a", "pcm_f32le", "-f", "f32le", "-n", str(output)], directory, replacements)
            output.with_suffix(".log").write_text(log, encoding="utf-8")
            sources[name] = dict(channels=channels, samples=read_float_pcm(output, rate * 4 * channels))
        for channels in (1, 2):
            samples = assemble_delivery(fixture["album"], sources, rate, channels)
            expected_audio[rate, channels] = samples
            write_float_pcm(oracle / f"album-{rate}-{channels}ch.f32le", samples)
    source_identity = "".join(
        track["id"] + ":" + source["id"] + ":" + source["sourceHash"] + "\n"
        for track in fixture["album"]["tracks"]
        for source in track["sources"] if source["id"] == track["selectedSourceId"])
    source_hash = hashlib.sha256(source_identity.encode("utf-8")).hexdigest()
    for case, row in zip(fixture["exports"], native["exports"]):
        if case["id"] != row["id"]:
            raise ValueError("Native delivery case order changed.")
        name, rate, channels = case["id"], case["sampleRate"], case["channels"]
        case_directory = native_output / name
        expected = expected_audio[rate, channels]
        lossy = case["format"] in ("mp3", "ogg")
        decoder_maximum = DELIVERY_TOLERANCES["nativeLossyMaximum" if lossy else "losslessMaximum"]
        decoded, output_hashes, native_differences = [], [], []
        for pass_name in ("master", "repeat"):
            result = row[pass_name]
            audio = case_directory / result["file"]
            output_hashes.append(file_hash(audio))
            if output_hashes[-1] != result["report"]["outputHash"] or result["report"]["sourceHash"] != source_hash:
                raise ValueError(name + ": product source/output hashes disagree with retained files.")
            if abs(result["report"]["durationSeconds"] - len(expected) / channels / rate) > 1e-6:
                report["problems"].append(name + ": " + pass_name + " product duration includes missing/extra audio.")
            output = case_directory / f"{pass_name}.f32le"
            log = run_process([str(meter), "-nostdin", "-hide_banner", "-nostats", "-i", str(audio),
                               "-map", "0:a:0", "-c:a", "pcm_f32le", "-f", "f32le", "-n", str(output)],
                              directory, replacements)
            output.with_suffix(".log").write_text(log, encoding="utf-8")
            decoded.append(read_float_pcm(output, len(expected)))
            native_pcm = read_float_pcm(case_directory / result["nativeDecodedFile"], len(expected))
            difference = audio_difference(native_pcm, decoded[-1])
            if (difference["maximumAbsoluteSampleError"] > decoder_maximum
                    or difference["rmsError"] > DELIVERY_TOLERANCES["nativeDecodedRms"]):
                report["problems"].append(name + ": " + pass_name + " native/independent decoded PCM disagrees.")
            native_differences.append(difference)
        if decoded[0] != decoded[1] or (case["format"] != "ogg" and output_hashes[0] != output_hashes[1]):
            report["problems"].append(name + ": repeated exports are not deterministic.")
        probe_text = run_process(
            [str(probe), "-v", "error", "-select_streams", "a", "-show_entries",
             "stream=codec_name,sample_rate,channels,bits_per_sample,bits_per_raw_sample,bit_rate:stream_tags:format_tags",
             "-of", "json", str(case_directory / row["master"]["file"])], directory, replacements)
        (case_directory / "probe.json").write_text(probe_text, encoding="utf-8")
        probe_data = read_json(probe_text)
        tags = validate_delivery_stream(probe_data, case, DELIVERY_METADATA)
        if (case["format"] == "mp3" and case["mp3BitrateMode"] == "constant"
                and probe_data["streams"][0].get("bit_rate") != "320000"):
            report["problems"].append(name + ": the MP3 stream did not honor 320 kbps CBR.")
        difference = audio_difference(decoded[0], expected)
        lsb = 2 ** (1 - case["bitDepth"]) if not lossy and case["bitDepth"] != 32 else 0
        maximum = DELIVERY_TOLERANCES["lossyMaximum" if lossy else "losslessMaximum"]
        rms = DELIVERY_TOLERANCES["lossyRms" if lossy else "losslessRms"]
        maximum += lsb * (1.5 if case["dither"] == "tpdf" else 1)
        rms += lsb * 0.6
        if difference["maximumAbsoluteSampleError"] > maximum or difference["rmsError"] > rms:
            report["problems"].append(name + ": complete independent sequence/SRC waveform exceeds fixed bounds.")
        reference = measure_reference(meter, case_directory / row["master"]["file"],
                                      rate, case_directory, directory, replacements)
        metrics = {"lufs": "integratedLoudnessLufs", "lra": "loudnessRangeLu", "dbtp": "truePeakDbtp"}
        measured = dict(id=name, settings=delivery_settings(case), frames=len(expected) // channels,
                        outputSha256=output_hashes[0], repeatSha256=output_hashes[1],
                        decodedSha256=file_hash(case_directory / "master.f32le"), metadata=tags,
                        waveform=dict(**difference, maximumTolerance=maximum, rmsTolerance=rms),
                        nativeDecoderDifferences=native_differences,
                        nativeDecoderTolerances=dict(maximum=decoder_maximum, rms=DELIVERY_TOLERANCES["nativeDecodedRms"]),
                        repeatPcmIdentical=decoded[0] == decoded[1])
        for metric, field in metrics.items():
            measured["actual_" + metric] = row["master"]["report"][field]
            measured["reference_" + metric] = reference[metric]
            for pass_name in ("master", "repeat"):
                if (not number(reference[metric])
                        or abs(row[pass_name]["report"][field] - reference[metric]) > REFERENCE_TOLERANCES[metric]):
                    report["problems"].append(name + ": final-file " + metric + " exceeds the fixed tolerance.")
        if case["dither"] == "tpdf":
            measured["dither"] = dither_statistics(decoded[0][:round(rate * 0.25) * channels],
                                                   channels, case["bitDepth"])
        report["measurements"].append(measured)
        print(name + ": independently decoded, measured and compared", flush=True)
    for track in fixture["album"]["tracks"]:
        for source in track["sources"]:
            if file_hash(directory / source["file"]) != source["sourceHash"]:
                raise ValueError("A delivery source changed during independent verification.")
    if file_hash(request) != fixture_hash:
        raise ValueError("The delivery fixture changed during independent verification.")
    if file_hash(native_output / "report.json") != report["nativeManifestSha256"]:
        raise ValueError("The native delivery manifest changed during independent verification.")
    for row in native["exports"]:
        for name in ("master", "repeat"):
            result = row[name]
            if file_hash(native_output / row["id"] / result["file"]) != result["report"]["outputHash"]:
                raise ValueError(row["id"] + ": encoded delivery changed during independent verification.")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--test-binary", required=True, type=Path)
    parser.add_argument("--ffmpeg", default="ffmpeg")
    parser.add_argument("--ffprobe", default="ffprobe")
    parser.add_argument("--delivery", action="store_true", help="Run the independent 29-case encoded album matrix.")
    parser.add_argument("--evidence-root", required=True, type=Path)
    parser.add_argument("--expected-sha")
    args = parser.parse_args()
    if args.expected_sha is not None and not digest(args.expected_sha, 40):
        parser.error("--expected-sha must be the exact lowercase 40-character candidate SHA.")
    executable = args.test_binary.resolve()
    meter = shutil.which(args.ffmpeg)
    if not executable.is_file() or meter is None:
        parser.error("An existing native test executable and FFmpeg with the ebur128 filter are required.")
    meter = Path(meter).resolve()
    probe = shutil.which(args.ffprobe) if args.delivery else None
    if args.delivery and probe is None:
        parser.error("Encoded delivery qualification also requires an existing FFprobe executable.")
    probe = Path(probe).resolve() if probe is not None else None
    args.evidence_root.mkdir(parents=True, exist_ok=True)
    directory = Path(tempfile.mkdtemp(prefix="run-", dir=args.evidence_root.resolve()))
    replacements = [(str(directory), "<evidence>"), (str(ROOT), "<source>"),
                    (str(Path.home()), "<home>")]
    report = dict(
        schemaVersion=1, status="running", qualificationStatus="not-qualified",
        scope="independent-synthetic-encoded-album-delivery" if args.delivery
              else "independent-synthetic-mono-stereo-mastering-metrology",
        requestedCandidateSha=args.expected_sha, trial=args.expected_sha is None,
        testBinarySha256=file_hash(executable), ffmpegSha256=file_hash(meter),
        runnerSha256=file_hash(Path(__file__)), evidenceGateSha256=file_hash(ROOT / "scripts/qualify.py"),
        hostOs=platform.platform(), hostArchitecture=platform.machine(), pythonVersion=platform.python_version(),
        tolerances=DELIVERY_TOLERANCES if args.delivery else REFERENCE_TOLERANCES, measurements=[], problems=[],
        notCertified=["licensed DDP encoder and independent checker", "external DAW interoperability",
                      "complete release workflows beyond the recorded synthetic matrix", "official licensed reference assets",
                      "clean-machine trust", "hardware and accessibility", "human release approval"],
    )
    try:
        verify_candidate_checkout(args.expected_sha, directory, replacements)
        version = run_process([str(meter), "-version"], directory, replacements)
        if not version.startswith("ffmpeg version "):
            raise ValueError("The independent meter did not return a recognizable FFmpeg version.")
        (directory / "ffmpeg-version.log").write_text(version, encoding="utf-8")
        report["ffmpegVersion"] = version.splitlines()[0]
        if args.delivery:
            report["notCertified"].append("bit-exact lossy-decoder or MPEG conformance")
            report["ffprobeSha256"] = file_hash(probe)
            version = run_process([str(probe), "-version"], directory, replacements)
            if not version.startswith("ffprobe version "):
                raise ValueError("The independent container probe returned an unrecognizable version.")
            (directory / "ffprobe-version.log").write_text(version, encoding="utf-8")
            report["ffprobeVersion"] = version.splitlines()[0]
            qualify_delivery(executable, meter, probe, directory, replacements, report, args.expected_sha)
        else:
            report["notCertified"].append("encoded delivery")
            for rate in REFERENCE_RATES:
                for channels in (1, 2):
                    for kind in REFERENCE_KINDS:
                        name = f"{kind}-{rate}-{channels}ch"
                        case = directory / name
                        case.mkdir()
                        source = case / "source.wav"
                        frames = write_source(source, kind, rate, channels)
                        source_hash = file_hash(source)
                        native_output = case / "native"
                        native_log = run_process(
                            [str(executable), "--measure-mastering=" + str(source),
                             "--measurement-output=" + str(native_output)], directory, replacements)
                        (case / "native.log").write_text(native_log, encoding="utf-8")
                        native = read_json((native_output / "report.json").read_text(encoding="utf-8"))
                        problems = validate_native(native, source_hash, rate, channels, frames, args.expected_sha)
                        if problems:
                            raise ValueError("; ".join(problems))
                        reference = measure_reference(meter, source, rate, case, directory, replacements)
                        row = dict(kind=kind, sample_rate_hz=rate, channels=channels, source_sha256=source_hash,
                                   reference_tool="FFmpeg ebur128 original-rate programme and flushed reconstruction peak",
                                   reference_version=report["ffmpegVersion"],
                                   provenance="Deterministic 32-bit PCM; original programme I/LRA, separate 200 ms "
                                              "zero-extension for reconstruction TP only; source and both raw logs retained.")
                        for metric in REFERENCE_TOLERANCES:
                            row["actual_" + metric] = native["measurements"][metric]
                            row["reference_" + metric] = reference[metric]
                        if file_hash(source) != source_hash:
                            raise ValueError("Synthetic source changed between native and independent measurement.")
                        report["measurements"].append(row)
                        print(name + ": measured", flush=True)
            report["problems"] = reference_measurement_errors(report["measurements"], "mastering")
        if (file_hash(executable) != report["testBinarySha256"] or file_hash(meter) != report["ffmpegSha256"]):
            report["problems"].append("A measurement executable changed during the run.")
        if (file_hash(Path(__file__)) != report["runnerSha256"]
                or file_hash(ROOT / "scripts/qualify.py") != report["evidenceGateSha256"]):
            report["problems"].append("Qualification tooling changed during the run.")
        if args.delivery and file_hash(probe) != report["ffprobeSha256"]:
            report["problems"].append("The independent container probe changed during the run.")
        verify_candidate_checkout(args.expected_sha, directory, replacements)
        report["status"] = "fail" if report["problems"] else "pass"
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        message = str(error)
        for source, label in replacements:
            message = message.replace(source, label)
        report["problems"].append(message)
        report["status"] = "fail"
    if args.delivery:
        report["evidence"] = [dict(path=path.relative_to(directory).as_posix(), sha256=file_hash(path),
                                   bytes=path.stat().st_size)
                              for path in sorted(directory.rglob("*")) if path.is_file()]
    write_json(directory / "report.json", report)
    for problem in report["problems"]:
        print(problem, file=sys.stderr)
    print(f"{report['status'].upper()}: {directory}; release qualification remains incomplete.", flush=True)
    return 0 if report["status"] == "pass" else 1


if __name__ == "__main__":
    try:
        code = main()
    except OSError as error:
        print(f"Mastering qualification I/O error: {error}", file=sys.stderr)
        code = 1
    sys.exit(code)
