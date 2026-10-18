#!/usr/bin/env python3
"""Run a pinned public-vendor software matrix; never certify hardware or release policy."""

import argparse
import hashlib
import json
import math
import os
from pathlib import Path, PurePosixPath
import platform
import re
import signal
import stat
import subprocess
import sys
import tempfile
import urllib.request
from zipfile import BadZipFile, ZipFile


ROOT = Path(__file__).resolve().parents[1]
TARGETS = ("Surge XT.vst3", "Surge XT Effects.vst3", "Surge XT.clap", "Surge XT Effects.clap")
RELEASE = "https://github.com/surge-synthesizer/releases-xt/releases/download/1.3.4/"
ASSETS = {
    "Darwin": {
        "url": RELEASE + "surge-xt-macos-1.3.4-pluginsonly.zip",
        "bytes": 186958216,
        "sha256": "37d8d809693adc86587b7bc75a2384fc4e905f259aa9b9a21777749d36fbf8aa",
    },
    "Windows": {
        "url": RELEASE + "surge-xt-win64-1.3.4-pluginsonly.zip",
        "bytes": 49093875,
        "sha256": "564e162c560af07ad4ed47fe1bfcd827cf97a575de30d06c48249aad2e7c35e6",
    },
}
REQUIRED_CHECKS = {
    "descriptor", "instantiate", "process", "parameters", "state", "latency-tail", "editor",
    "workflow-instance-independence", "workflow-repeat-null", "workflow-sandbox-trusted-null",
    "workflow-live-offline-null", "workflow-nested-rack-null", "workflow-parent-bypass",
    "workflow-simultaneous-workers",
}
RECOVERY_CHECKS = {
    "sandbox-ready", "sandbox-state-capture", "sandbox-playback", "sandbox-generic-editor",
    "sandbox-parameter-feedback", "sandbox-nondefault-state", "sandbox-crash-visible",
    "sandbox-dry-fallback", "sandbox-failed-state-preserved", "sandbox-package-valid",
    "sandbox-explicit-reload", "sandbox-restored-processing", "sandbox-restored-parameters",
}
NULL_MEASUREMENTS = (
    "repeatMaximumNullError", "sandboxTrustedMaximumNullError", "liveOfflineMaximumNullError",
    "nestedMaximumNullError", "bypassMaximumNullError", "simultaneousMaximumNullError",
)


class ProbeError(ValueError):
    pass


def digest(path):
    value = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            value.update(block)
    return value.hexdigest()


def verify_archive(path, asset):
    if path.stat().st_size != asset["bytes"] or digest(path) != asset["sha256"]:
        raise ProbeError("The public vendor archive does not match its pinned size and SHA-256.")


def extract_plugins(path, destination):
    seen, found = set(), set()
    total = 0
    with ZipFile(path) as archive:
        for entry in archive.infolist():
            raw_name = entry.orig_filename
            parts = raw_name.rstrip("/").split("/")
            if (not parts or any(part in ("", ".", "..") or ":" in part for part in parts)
                    or "\\" in raw_name or "\0" in raw_name or PurePosixPath(raw_name).is_absolute()
                    or stat.S_ISLNK(entry.external_attr >> 16)):
                raise ProbeError("The vendor archive contains an unsafe path or symbolic link.")
            relative = PurePosixPath(*parts)
            if relative in seen:
                raise ProbeError("The vendor archive contains duplicate paths.")
            seen.add(relative)
            if parts[0] not in TARGETS:
                continue
            total += entry.file_size
            if total > 2 * 1024**3 or entry.file_size > 512 * 1024**2:
                raise ProbeError("The vendor archive exceeds the bounded extraction budget.")
            found.add(parts[0])
            target = destination.joinpath(*parts)
            if entry.is_dir():
                target.mkdir(parents=True, exist_ok=True)
                continue
            target.parent.mkdir(parents=True, exist_ok=True)
            with archive.open(entry) as source, target.open("xb") as output:
                for block in iter(lambda: source.read(1024 * 1024), b""):
                    output.write(block)
            mode = (entry.external_attr >> 16) & 0o777
            if mode:
                target.chmod(mode & 0o755)
    if found != set(TARGETS):
        raise ProbeError("The pinned archive is missing an expected VST3 or CLAP target.")


def redact(value, replacements):
    if isinstance(value, str):
        for source, label in replacements:
            if source:
                value = value.replace(source, label).replace(source.replace("\\", "/"), label)
        return value
    if isinstance(value, list):
        return [redact(item, replacements) for item in value]
    if isinstance(value, dict):
        return {key: redact(item, replacements) for key, item in value.items()}
    return value


def write_json(path, value):
    temporary = path.with_suffix(".partial")
    temporary.write_text(json.dumps(value, indent=2, allow_nan=False) + "\n", encoding="utf-8")
    temporary.replace(path)


def probe_environment(home, temporary):
    allowed = {
        "PATH", "SYSTEMROOT", "WINDIR", "COMSPEC", "PATHEXT", "SYSTEMDRIVE", "PROGRAMFILES",
        "PROGRAMFILES(X86)", "COMMONPROGRAMFILES", "COMMONPROGRAMFILES(X86)", "PROGRAMW6432",
        "COMMONPROGRAMW6432", "OS", "PROCESSOR_ARCHITECTURE", "NUMBER_OF_PROCESSORS",
        "LANG", "LC_ALL", "LC_CTYPE", "TZ",
    }
    environment = {key: value for key, value in os.environ.items() if key.upper() in allowed}
    environment.update(HOME=str(home), USERPROFILE=str(home), TMPDIR=str(temporary) + os.sep,
                       TEMP=str(temporary), TMP=str(temporary), APPDATA=str(home / "AppData/Roaming"),
                       LOCALAPPDATA=str(home / "AppData/Local"))
    return environment


def run_bounded(command, cwd, environment, timeout):
    with tempfile.TemporaryFile() as log:
        process = subprocess.Popen(
            command, cwd=cwd, env=environment, stdout=log, stderr=subprocess.STDOUT,
            start_new_session=os.name != "nt",
            creationflags=subprocess.CREATE_NEW_PROCESS_GROUP if os.name == "nt" else 0,
        )
        timed_out = False
        try:
            process.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            timed_out = True
            if os.name == "nt":
                subprocess.run(
                    ["taskkill", "/PID", str(process.pid), "/T", "/F"],
                    check=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=15,
                )
            else:
                os.killpg(process.pid, signal.SIGKILL)
            process.wait(timeout=15)
        size = log.seek(0, os.SEEK_END)
        log.seek(max(0, size - 1024**2))
        text = log.read().decode("utf-8", errors="replace")
    return process.returncode, text, timed_out, size > 1024**2


def validate_probe(report, expected_sha=None):
    errors = []
    if not isinstance(report, dict):
        return ["The native probe report is not an object."]
    if report.get("status") != "pass" or report.get("qualificationStatus") != "not-qualified":
        errors.append("The native workflow failed or incorrectly claimed qualification.")
    if report.get("hostAudioDeviceManagerCreated") is not False:
        errors.append("The probe did not confirm driver-free operation.")
    if report.get("workflowsRequested") is not True or report.get("scope") != "vendor-headless-app-workflow-probe":
        errors.append("The full requested software workflow was not executed.")
    revision = report.get("buildRevision")
    if expected_sha and (report.get("candidateSha") != expected_sha
                         or not isinstance(revision, str) or "dirty" in revision):
        errors.append("The native probe is not the requested clean candidate.")
    processors = report.get("processors")
    if not isinstance(processors, list) or len(processors) != 1:
        return errors + ["Each explicit source must report exactly one processor."]
    processor = processors[0]
    if not isinstance(processor, dict):
        return errors + ["The selected processor did not pass its requested checks."]
    if processor.get("status") != "pass":
        checks = processor.get("checks")
        details = [str(check.get("name")) + ": " + str(check.get("message")) for check in checks
                   if isinstance(check, dict) and check.get("status") == "fail"] if isinstance(checks, list) else []
        return errors + ["The selected processor did not pass its requested checks."] + details
    required = set(REQUIRED_CHECKS)
    if processor.get("role") == "instrument":
        required.add("workflow-midi-instrument")
    elif processor.get("role") == "effect":
        required.add("workflow-effect-audio")
    else:
        errors.append("The processor omitted its typed instrument/effect role.")
    if any(not isinstance(processor.get(key), str) or not processor[key]
           for key in ("name", "version", "manufacturer", "architecture", "format")):
        errors.append("The exact native processor identity is incomplete.")
    if report.get("effectSandboxRequested") is True:
        required.update(RECOVERY_CHECKS)
    checks = processor.get("checks")
    if not isinstance(checks, list) or any(not isinstance(check, dict) for check in checks):
        return errors + ["The native probe omitted its typed checks."]
    names = [check.get("name") for check in checks]
    if any(not isinstance(name, str) for name in names) or len(names) != len(set(names)):
        errors.append("The native probe has malformed or duplicate check names.")
    else:
        missing = required.union({"workflow-sidechain", "workflow-multi-output"}) - set(names)
        if missing:
            errors.append("Missing workflow checks: " + ", ".join(sorted(missing)))
    for check in checks:
        if check.get("status") not in ("pass", "skip") or (
                check.get("name") in required and check.get("status") != "pass"):
            errors.append("Failed/incomplete check: " + str(check.get("name")))
    metrics = processor.get("workflowMeasurements")
    if not isinstance(metrics, dict):
        return errors + ["The native probe omitted workflow measurements."]
    if (metrics.get("sampleRate") != 48000 or metrics.get("bufferSize") != 512
            or metrics.get("contentSamples") != 24000 or metrics.get("nullTolerance") != 1e-5):
        errors.append("The native workload or original null tolerance changed.")
    def bounded_number(value, maximum=None):
        return (isinstance(value, (int, float)) and not isinstance(value, bool)
                and math.isfinite(value) and value >= 0 and (maximum is None or value <= maximum))
    if any(not bounded_number(metrics.get(name), 1e-5) for name in NULL_MEASUREMENTS):
        errors.append("A required measured null is missing, non-finite, negative or above 1e-5.")
    output_count = metrics.get("outputBuses")
    if not isinstance(output_count, int) or isinstance(output_count, bool) or not 0 <= output_count <= 128:
        errors.append("The native output-bus count is invalid.")
    elif output_count > 1:
        buses = metrics.get("auxiliaryOutputBuses")
        if (not isinstance(buses, list) or len(buses) != output_count - 1
                or any(not isinstance(bus, dict) for bus in buses)):
            errors.append("Advertised auxiliary output routes are missing.")
        elif ([bus.get("index") for bus in buses] != list(range(1, output_count))
              or any(not bounded_number(bus.get("maximumNullError"), 1e-5)
                     or not bounded_number(bus.get("peak")) for bus in buses)
              or not any(check.get("name") == "workflow-multi-output" and check.get("status") == "pass" for check in checks)):
            errors.append("An advertised auxiliary bus failed its measured route/null check.")
    input_count = metrics.get("inputBuses")
    if not isinstance(input_count, int) or isinstance(input_count, bool) or not 0 <= input_count <= 128:
        errors.append("The native input-bus count is invalid.")
    elif input_count > 1:
        if (not bounded_number(metrics.get("sidechainMaximumNullError"), 1e-5)
                or not bounded_number(metrics.get("configuredSidechainMaximumInfluence"))
                or not any(check.get("name") == "workflow-sidechain-route-null" and check.get("status") == "pass" for check in checks)):
            errors.append("The advertised auxiliary input lacks a measured native route reference.")
    return errors


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--test-binary", required=True, type=Path)
    parser.add_argument("--work-root", required=True, type=Path)
    parser.add_argument("--vendor-archive", type=Path)
    parser.add_argument("--expected-sha")
    parser.add_argument("--approve-public-surge", action="store_true", required=True)
    parser.add_argument("--disposable-windows-host", action="store_true")
    parser.add_argument("--timeout-seconds", type=int, default=180)
    arguments = parser.parse_args(argv)
    system = platform.system()
    if system not in ASSETS:
        parser.error("Only native macOS and disposable Windows CI hosts are supported.")
    if system == "Windows" and not (
            arguments.disposable_windows_host and os.environ.get("GITHUB_ACTIONS") == "true"):
        parser.error("Windows probes require an explicitly disposable GitHub Actions host, not a user installation.")
    if arguments.expected_sha and not re.fullmatch(r"[0-9a-f]{40}", arguments.expected_sha):
        parser.error("--expected-sha must be a complete lowercase candidate SHA.")
    if not 10 <= arguments.timeout_seconds <= 600:
        parser.error("--timeout-seconds must be between 10 and 600.")
    executable = arguments.test_binary.resolve()
    work = arguments.work_root.resolve()
    if not executable.is_file() or work.exists():
        parser.error("Use an existing test executable and a new, exclusively owned work root.")
    work.mkdir(parents=True, mode=0o700)
    evidence = work / "evidence"
    evidence.mkdir()
    asset = ASSETS[system]
    matrix = {
        "schemaVersion": 1, "status": "running", "qualificationStatus": "not-qualified",
        "requestedCandidateSha": arguments.expected_sha, "trial": arguments.expected_sha is None,
        "scope": "public-vendor-headless-software-matrix", "hostOs": platform.platform(),
        "hostArchitecture": platform.machine(), "testBinarySha256": digest(executable),
        "source": dict(asset, version="1.3.4", license="GPL-3.0", globallyInstalled=False),
        "isolation": "macOS seatbelt plus private HOME/TMPDIR" if system == "Darwin"
            else "disposable hosted Windows environment; not an OS capability sandbox",
        "notCertified": ["physical hardware/deadlines", "VoiceOver/NVDA", "licensed ARA workflows",
                         "hang during state capture", "human reduced-isolation release disposition"],
        "cases": [],
    }
    write_json(evidence / "matrix.json", matrix)
    replacements = [(str(work), "<owned-work>"), (str(ROOT), "<source>"), (str(Path.home()), "<home>")]
    try:
        with tempfile.TemporaryDirectory(prefix=".private-", dir=work) as private_name:
            private = Path(private_name)
            archive = arguments.vendor_archive.resolve() if arguments.vendor_archive else private / "vendor.zip"
            if not arguments.vendor_archive:
                with urllib.request.urlopen(asset["url"], timeout=60) as source, archive.open("xb") as output:
                    count = 0
                    for block in iter(lambda: source.read(1024 * 1024), b""):
                        count += len(block)
                        if count > asset["bytes"]:
                            raise ProbeError("The download exceeds the pinned vendor archive size.")
                        output.write(block)
            verify_archive(archive, asset)
            plugins = private / "plugins"
            plugins.mkdir()
            extract_plugins(archive, plugins)
            cases = [(name, str(plugins / name)) for name in TARGETS]
            if system == "Darwin":
                cases.append(("Apple AUBandpass", "AudioUnit:Effects/aufx,bpas,appl"))
            for index, (name, source) in enumerate(cases):
                case_root = private / ("case-" + str(index))
                home, temporary, output = case_root / "home", case_root / "tmp", case_root / "result"
                home.mkdir(parents=True)
                (home / "Documents").mkdir()
                (home / "AppData/Roaming").mkdir(parents=True)
                (home / "AppData/Local").mkdir()
                temporary.mkdir()
                environment = probe_environment(home, temporary)
                command = [str(executable), "--qualify-plugin=" + source, "--qualification-output=" + str(output),
                           "--omit-platform-tools", "--exercise-workflows"]
                if name.startswith("Surge XT.") and "Effects" not in name:
                    profile_name = "surge-xt-1.3.4-" + ("vst3-" if name.endswith(".vst3") else "") + "deterministic.json"
                    command.append("--workflow-profile=" + str(ROOT / "tests" / "vendor-profiles" / profile_name))
                else:
                    command.append("--exercise-sandbox-effect")
                if system == "Darwin":
                    command = ["/usr/bin/sandbox-exec", "-f", str(ROOT / "scripts" / "vendor-probe-macos.sb"),
                               "-D", "WORK_ROOT=" + str(case_root), "-D", "OUTPUT_ROOT=" + str(output)] + command
                code, log, timed_out, truncated = run_bounded(command, case_root, environment, arguments.timeout_seconds)
                case_evidence = evidence / ("case-" + str(index))
                case_evidence.mkdir()
                (case_evidence / "probe.log").write_text(redact(log, replacements), encoding="utf-8")
                native_path = output / "report.json"
                report = json.loads(native_path.read_text(encoding="utf-8")) if native_path.is_file() else None
                problems = validate_probe(report, arguments.expected_sha)
                if code != 0 or timed_out:
                    problems.append("Probe timed out." if timed_out else "Native process exited with code " + str(code) + ".")
                if report is not None:
                    write_json(case_evidence / "report.json", redact(report, replacements))
                result = {"name": name, "status": "fail" if problems else "pass",
                          "returnCode": code, "timedOut": timed_out, "logTruncated": truncated,
                          "problems": redact(problems, replacements)}
                if isinstance(report, dict):
                    result["candidateSha"] = report.get("candidateSha")
                    result["buildRevision"] = report.get("buildRevision")
                    processors = report.get("processors")
                    result["skippedChecks"] = [check.get("name") for processor in processors
                        if isinstance(processor, dict) and isinstance(processor.get("checks"), list)
                        for check in processor["checks"] if isinstance(check, dict) and check.get("status") == "skip"
                    ] if isinstance(processors, list) else []
                matrix["cases"].append(result)
                write_json(evidence / "matrix.json", matrix)
                print(name + ": " + result["status"], flush=True)
                for problem in result["problems"]:
                    print("  " + problem[:2048], flush=True)
        matrix["status"] = "pass" if all(case["status"] == "pass" for case in matrix["cases"]) else "fail"
        write_json(evidence / "matrix.json", matrix)
        return 0 if matrix["status"] == "pass" else 1
    except (OSError, ValueError, BadZipFile, subprocess.SubprocessError) as error:
        matrix["status"] = "fail"
        matrix["error"] = redact(str(error), replacements)
        write_json(evidence / "matrix.json", matrix)
        print("Vendor matrix failed: " + matrix["error"], file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
