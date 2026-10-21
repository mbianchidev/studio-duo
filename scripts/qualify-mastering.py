#!/usr/bin/env python3
"""Compare actual Studio Duo mastering measurements with independent FFmpeg R128 metrology."""

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
    if not digest(report.get("candidateSha"), 40):
        errors.append("Native measurement lacks a complete candidate SHA.")
    if expected_sha is not None:
        revision = report.get("buildRevision")
        if (report.get("candidateSha") != expected_sha or not isinstance(revision, str)
                or not 7 <= len(revision) <= 40 or not expected_sha.startswith(revision)):
            errors.append("Native measurement is not the exact clean requested candidate.")
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


def run_process(command, directory, replacements):
    with tempfile.TemporaryFile() as output:
        result = subprocess.run(command, cwd=directory, stdin=subprocess.DEVNULL,
                                stdout=output, stderr=subprocess.STDOUT, timeout=90, check=False)
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


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--test-binary", required=True, type=Path)
    parser.add_argument("--ffmpeg", default="ffmpeg")
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
    args.evidence_root.mkdir(parents=True, exist_ok=True)
    directory = Path(tempfile.mkdtemp(prefix="run-", dir=args.evidence_root.resolve()))
    replacements = [(str(directory), "<evidence>"), (str(ROOT), "<source>"),
                    (str(Path.home()), "<home>")]
    report = dict(
        schemaVersion=1, status="running", qualificationStatus="not-qualified",
        scope="independent-synthetic-mono-stereo-mastering-metrology",
        requestedCandidateSha=args.expected_sha, trial=args.expected_sha is None,
        testBinarySha256=file_hash(executable), ffmpegSha256=file_hash(meter),
        runnerSha256=file_hash(Path(__file__)), evidenceGateSha256=file_hash(ROOT / "scripts/qualify.py"),
        hostOs=platform.platform(), hostArchitecture=platform.machine(),
        tolerances=REFERENCE_TOLERANCES, measurements=[], problems=[],
        notCertified=["licensed DDP encoder and independent checker", "external DAW interoperability",
                      "encoded delivery and complete multi-song workflows", "official licensed reference assets",
                      "clean-machine trust", "hardware and accessibility", "human release approval"],
    )
    try:
        verify_candidate_checkout(args.expected_sha, directory, replacements)
        version = run_process([str(meter), "-version"], directory, replacements)
        if not version.startswith("ffmpeg version "):
            raise ValueError("The independent meter did not return a recognizable FFmpeg version.")
        (directory / "ffmpeg-version.log").write_text(version, encoding="utf-8")
        report["ffmpegVersion"] = version.splitlines()[0]
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
                    reference_log = run_process(
                        [str(meter), "-nostdin", "-hide_banner", "-nostats", "-i", str(source),
                         "-af", "ebur128=metadata=1,ametadata=print", "-f", "null", "-"],
                        directory, replacements)
                    (case / "reference.log").write_text(reference_log, encoding="utf-8")
                    peak_log = run_process(
                        [str(meter), "-nostdin", "-hide_banner", "-nostats", "-i", str(source),
                         "-af", f"apad=pad_len={rate // 5},ebur128=peak=true:metadata=1,ametadata=print",
                         "-f", "null", "-"], directory, replacements)
                    (case / "reference-peak.log").write_text(peak_log, encoding="utf-8")
                    reference = parse_reference(reference_log, peak_log)
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
        verify_candidate_checkout(args.expected_sha, directory, replacements)
        report["status"] = "fail" if report["problems"] else "pass"
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        message = str(error)
        for source, label in replacements:
            message = message.replace(source, label)
        report["problems"].append(message)
        report["status"] = "fail"
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
