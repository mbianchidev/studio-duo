#!/usr/bin/env python3
"""Candidate-specific Phase 6 evidence completeness and integrity gate.

This tool never opens audio/MIDI devices, loads plugins, drives the desktop, or
turns automated regression results into manual qualification evidence.
"""

import argparse
from datetime import datetime
import hashlib
import json
import math
from pathlib import Path
import re
import sys


TARGETS = ("macos-arm64", "macos-x86_64", "windows10-x64", "windows11-x64")
SAMPLE_RATES = (48000, 96000)
BUFFER_SIZES = (64, 128, 256, 512, 1024)
REFERENCE_RATES = (44100, 48000, 96000)
REFERENCE_KINDS = ("steady", "gated", "dynamic", "intersample", "silence", "tail-transient")
REFERENCE_TOLERANCES = {"lufs": 0.1, "lra": 1.0, "dbtp": 0.3}
PRESERVED_DATA = ("projects", "media", "preferences", "processor_state", "recent_projects")
CHECKS = {
    "performance": (
        "full-length-multisong-media", "callback-deadlines-cpu-during-graph-edits",
        "repeated-edits-takes-portable-memory", "disk-throughput-cold-warm-open-save",
        "late-viewport-interaction", "latency-automation-live-offline-render",
        "bundled-and-approved-vendor-processors",
    ),
    "hardware": (
        "clean-install-record-edit-mix-export", "professional-multichannel-mono-stereo",
        "sample-rates-buffers-clock", "hotplug-disconnect-reconnect", "punch-loop-count-in",
        "midi-pressure-sustain-timing-retrospective", "attenuated-physical-reamp-alignment",
        "control-surface-mappings",
    ),
    "vendors": (
        "approved-formats-instruments-effects", "state-presets-multiple-instances",
        "native-and-generic-editors", "sidechains-multioutput-nested-racks",
        "bypass-reload-latency-tails", "deterministic-live-offline-null",
        "tempo-sample-accurate-automation-with-latency",
        "sandbox-crash-hang-playback-state-capture-package-reload",
        "ara-document-source-edits-archive-unclean-exit",
        "reduced-isolation-consent-quarantine-recovery",
    ),
    "accessibility": (
        "new-open-import", "record-arm-hardware-setup", "audio-and-midi-editing",
        "comping-linked-edits", "routing-and-automation", "plugins-and-generic-parameters",
        "mastering-portable-repair-export", "timeline-nudge-context-scroll-selection",
        "names-roles-values-selection-errors-status", "modal-focus-restoration-escape-return",
        "all-themes-custom-colours-noncolour-states", "magnification-high-dpi-visible-focus",
        "no-drag-only-or-unreachable-primary-controls",
    ),
    "distribution": (
        "minimum-current-clean-install", "release-artifact-hashes",
        "different-version-upgrade-preservation",
        "actual-in-app-update-cancel-interrupt-rollback-restart",
        "unwritable-target-error-preservation", "uninstall-preserves-user-data-reinstall",
    ),
    "references": (
        "independent-bs1770-r128-true-peak", "dither-resampling-encoded-file",
        "multisong-gaps-overlaps-fades-metadata", "licensed-ddp-independent-verifier",
        "external-dawproject-roundtrips", "visible-unsupported-transactional-transfer",
    ),
}


def check_ids(area, target):
    result = list(CHECKS[area])
    if area == "hardware":
        result += (["coreaudio-audio-midi"] if target.startswith("macos")
                   else ["native-asio-audio-midi", "windows-audio-fallback"])
    if area == "distribution":
        result += (["quarantine-gatekeeper-user-approval"] if target.startswith("macos")
                   else ["authenticode-trust-timestamp-signer", "standard-user-admin-vc-runtime"])
    return result


def new_report(candidate_sha, candidate_version=""):
    systems = [
        dict(target=target, os_version="", cpu="", ram_gib=0, storage="", binary_sha256="")
        for target in TARGETS
    ]
    runs = []
    for target in TARGETS:
        for area in CHECKS:
            run = dict(
                target=target, area=area, procedure="", tool_versions="", settings="",
                performed_at="", manual_attestation=False,
                checks=[dict(id=name, status="not_run", observation="", evidence=[])
                        for name in check_ids(area, target)],
            )
            if area in ("hardware", "performance"):
                run["measurements"] = [] if area == "performance" else {}
            if area == "vendors":
                run["processors"] = []
                run["reduced_isolation_disposition"] = dict(approved=False, rationale="", evidence=[])
            if area == "accessibility":
                run["assistive_technology"] = "VoiceOver" if target.startswith("macos") else "NVDA"
            if area == "distribution":
                run.update(package_sha256="", signing_policy="", signer_thumbprint="",
                           signature_trusted=False, signature_timestamped=False, notarized=False,
                           os_coverage=[], upgrades=[])
            if area == "references":
                run["measurements"] = []
            runs.append(run)
    return dict(schema_version=2, candidate_sha=candidate_sha, candidate_version=candidate_version,
                systems=systems, runs=runs)


def read_json(text):
    def object_pairs(pairs):
        result = {}
        for key, value in pairs:
            if key in result:
                raise ValueError(f"Duplicate JSON key: {key}")
            result[key] = value
        return result

    def invalid_constant(value):
        raise ValueError(f"Non-finite JSON number: {value}")

    return json.loads(text, object_pairs_hook=object_pairs, parse_constant=invalid_constant)


def nonempty(value):
    return isinstance(value, str) and bool(value.strip())


def digest(value, length):
    return isinstance(value, str) and re.fullmatch(rf"[0-9a-f]{{{length}}}", value) is not None


def number(value):
    return type(value) is int or (type(value) is float and math.isfinite(value))


def stable_version(value):
    if not isinstance(value, str) or len(value) > 64:
        return None
    if re.fullmatch(r"(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)", value) is None:
        return None
    return tuple(int(part) for part in value.split("."))


def reference_measurement_errors(rows, label):
    errors = []
    if not isinstance(rows, list):
        return [f"{label}: reference measurements must be an array."]
    seen = set()
    for row in rows:
        if not isinstance(row, dict):
            errors.append(f"{label}: invalid reference measurement.")
            continue
        kind, rate, channels = row.get("kind"), row.get("sample_rate_hz"), row.get("channels")
        if (not isinstance(kind, str) or kind not in REFERENCE_KINDS
                or type(rate) is not int or rate not in REFERENCE_RATES
                or type(channels) is not int or channels not in (1, 2)):
            errors.append(f"{label}: unsupported reference kind/rate/channel layout.")
            continue
        key = (kind, rate, channels)
        if key in seen:
            errors.append(f"{label}: duplicate reference measurement {key}.")
            continue
        seen.add(key)
        for field in ("reference_tool", "reference_version", "provenance"):
            if not nonempty(row.get(field)):
                errors.append(f"{label} {key}: {field} is required.")
        if not digest(row.get("source_sha256"), 64):
            errors.append(f"{label} {key}: exact source_sha256 is required.")
        for metric, tolerance in REFERENCE_TOLERANCES.items():
            actual, reference = row.get("actual_" + metric), row.get("reference_" + metric)
            if kind == "silence":
                if actual is not None or reference is not None:
                    errors.append(f"{label} {key}: silence requires null {metric}, not a finite floor.")
                if "actual_" + metric not in row or "reference_" + metric not in row:
                    errors.append(f"{label} {key}: explicit silence results are required.")
                continue
            minimum, maximum = (0, 200) if metric == "lra" else (-300, 60)
            if any(not number(value) or not minimum <= value <= maximum for value in (actual, reference)):
                errors.append(f"{label} {key}: finite bounded {metric} measurements are required.")
            elif abs(actual - reference) > tolerance + 1e-9:
                errors.append(f"{label} {key}: {metric} exceeds the fixed {tolerance} tolerance.")
    for kind in REFERENCE_KINDS:
        for rate in REFERENCE_RATES:
            for channels in (1, 2):
                if (kind, rate, channels) not in seen:
                    errors.append(f"{label}: Missing reference measurement {(kind, rate, channels)}.")
    return errors


def validate(report, root, candidate_sha):
    errors = []
    root = Path(root).resolve()
    if not isinstance(report, dict):
        return ["Report must be a JSON object."]
    if type(report.get("schema_version")) is not int or report["schema_version"] != 2:
        errors.append("Unsupported schema_version.")
    if not digest(candidate_sha, 40) or report.get("candidate_sha") != candidate_sha:
        errors.append("Report candidate SHA does not match the exact requested candidate.")
    candidate_version = stable_version(report.get("candidate_version"))
    if candidate_version is None:
        errors.append("An exact stable candidate_version is required.")

    def evidence(items, label):
        if not isinstance(items, list) or not items:
            errors.append(f"{label}: raw evidence is required.")
            return
        for item in items:
            if not isinstance(item, dict):
                errors.append(f"{label}: evidence must contain path/sha256 objects.")
                continue
            name, expected = item.get("path"), item.get("sha256")
            if (not nonempty(name) or "\\" in name or ":" in name
                    or Path(name).is_absolute() or ".." in Path(name).parts):
                errors.append(f"{label}: evidence path must stay relative to the report directory.")
                continue
            if not digest(expected, 64):
                errors.append(f"{label}: evidence requires a lowercase SHA-256 digest.")
                continue
            try:
                file = (root / name).resolve(strict=True)
                file.relative_to(root)
                if not file.is_file() or file.stat().st_size == 0:
                    raise ValueError("Evidence must be a nonempty file.")
                actual = hashlib.sha256()
                with file.open("rb") as stream:
                    for block in iter(lambda: stream.read(1024 * 1024), b""):
                        actual.update(block)
                if actual.hexdigest() != expected:
                    errors.append(f"{label}: evidence hash mismatch for {name}.")
            except (OSError, ValueError, RuntimeError) as error:
                errors.append(f"{label}: unavailable or escaping evidence ({error}).")

    systems = report.get("systems")
    if not isinstance(systems, list):
        errors.append("systems must be an array.")
        systems = []
    known_systems = {}
    for system in systems:
        if not isinstance(system, dict) or not isinstance(system.get("target"), str):
            errors.append("Invalid system object.")
            continue
        target = system["target"]
        if target not in TARGETS or target in known_systems:
            errors.append(f"Unknown or duplicate system: {target}.")
            continue
        known_systems[target] = system
        for field in ("os_version", "cpu", "storage"):
            if not nonempty(system.get(field)):
                errors.append(f"{target}: {field} is required.")
        if not number(system.get("ram_gib")) or not 0 < system["ram_gib"] <= 65536:
            errors.append(f"{target}: positive numeric ram_gib is required.")
        if not digest(system.get("binary_sha256"), 64):
            errors.append(f"{target}: exact installed binary_sha256 is required.")
    for target in TARGETS:
        if target not in known_systems:
            errors.append(f"Missing system: {target}.")

    def performance(run, label):
        rows = run.get("measurements")
        if not isinstance(rows, list):
            errors.append(f"{label}: measurements must be an array.")
            return
        pairs = set()
        ram = known_systems.get(run["target"], {}).get("ram_gib")
        if not number(ram) or not 0 < ram <= 65536:
            ram = 0
        for row in rows:
            if not isinstance(row, dict):
                errors.append(f"{label}: invalid measurement.")
                continue
            rate, buffer = row.get("sample_rate_hz"), row.get("buffer_samples")
            if type(rate) is not int or type(buffer) is not int:
                errors.append(f"{label}: integer sample rate and buffer size are required.")
                continue
            pair = (rate, buffer)
            if rate not in SAMPLE_RATES or buffer not in BUFFER_SIZES or pair in pairs:
                errors.append(f"{label}: unknown or duplicate rate/buffer pair {pair}.")
                continue
            pairs.add(pair)
            bounds = {
                "duration_seconds": (1800, math.inf), "tracks": (16, math.inf), "songs": (3, math.inf),
                "deadline_misses": (0, 0), "callback_p99_ratio": (0, 0.7),
                "warm_open_seconds": (0, 10), "save_seconds": (0, 10),
                "graph_edit_seconds": (0, 0.25), "viewport_ms": (0, 50),
                "null_peak_error": (0, 1e-5),
                "peak_working_set_bytes": (1, ram * 0.75 * 1024**3 if number(ram) else 0),
            }
            for field, (minimum, maximum) in bounds.items():
                value = row.get(field)
                if (not number(value) or not minimum <= value <= maximum
                        or (field in ("tracks", "songs", "deadline_misses") and type(value) is not int)):
                    errors.append(f"{label} {pair}: {field} is missing or outside its qualification budget.")
        for rate in SAMPLE_RATES:
            for buffer in BUFFER_SIZES:
                if (rate, buffer) not in pairs:
                    errors.append(f"{label}: Missing measurement for {rate}/{buffer}.")

    def hardware(run, label):
        measurements = run.get("measurements")
        if not isinstance(measurements, dict):
            errors.append(f"{label}: hardware measurements must be an object.")
            return
        for field in ("dropped_audio_samples", "duplicated_audio_samples",
                      "dropped_midi_events", "duplicated_midi_events"):
            if type(measurements.get(field)) is not int or measurements[field] != 0:
                errors.append(f"{label}: {field} must be measured as zero.")
        residual = measurements.get("reamp_residual_samples")
        if not number(residual) or abs(residual) > 1:
            errors.append(f"{label}: physical reamp residual must be within +/-1 sample.")

    def vendors(run, label):
        processors = run.get("processors")
        if not isinstance(processors, list):
            errors.append(f"{label}: processors must be an array.")
            processors = []
        covered, modes = set(), set()
        architecture = run["target"].split("-", 1)[1]
        formats = ("VST3", "CLAP", "AudioUnit") if run["target"].startswith("macos") else ("VST3", "CLAP")
        for processor in processors:
            if not isinstance(processor, dict):
                errors.append(f"{label}: invalid processor metadata.")
                continue
            if any(not nonempty(processor.get(field)) for field in
                   ("name", "version", "format", "architecture", "role", "mode")):
                errors.append(f"{label}: exact processor name/version/format/architecture/role/mode are required.")
                continue
            allowed_architectures = {architecture, "x86_64"} if run["target"].startswith("windows") else {architecture}
            if processor["architecture"] not in allowed_architectures:
                errors.append(f"{label}: processor architecture differs from the tested system.")
            fmt, role, mode = processor["format"], processor["role"], processor["mode"]
            if (fmt not in formats or role not in ("instrument", "effect")
                    or mode not in ("sandboxed", "trustedInProcess", "araCompatibility")
                    or processor.get("approved") is not True):
                errors.append(f"{label}: unsupported or unapproved processor format/role/mode.")
            if mode == "araCompatibility" and fmt not in ("VST3", "AudioUnit"):
                errors.append(f"{label}: ARA requires a VST3 or Audio Unit processor.")
            if mode == "sandboxed":
                covered.add((fmt, role))
            if mode == "trustedInProcess":
                modes.add(mode)
            if mode == "araCompatibility" and processor.get("ara_capable") is True:
                modes.add(mode)
        for fmt in formats:
            for role in ("instrument", "effect"):
                if (fmt, role) not in covered:
                    errors.append(f"{label}: Missing approved sandboxed {fmt} {role}.")
        if modes != {"trustedInProcess", "araCompatibility"}:
            errors.append(f"{label}: trusted and ARA modes require separately identified processors.")
        disposition = run.get("reduced_isolation_disposition")
        if (not isinstance(disposition, dict) or disposition.get("approved") is not True
                or not nonempty(disposition.get("rationale"))):
            errors.append(f"{label}: reduced-isolation release disposition is not explicitly approved.")
        else:
            evidence(disposition.get("evidence"), label + " reduced-isolation disposition")

    def distribution(run, label):
        if not digest(run.get("package_sha256"), 64):
            errors.append(f"{label}: exact release package_sha256 is required.")
        mac = run["target"].startswith("macos")
        policies = ("ad-hoc-user-approved", "developer-id-notarized") if mac else ("authenticode",)
        if run.get("signing_policy") not in policies:
            errors.append(f"{label}: an explicit supported production signing/trust policy is required.")
        if not mac or run.get("signing_policy") == "developer-id-notarized":
            if (not digest(run.get("signer_thumbprint"), 40)
                    or run.get("signature_trusted") is not True
                    or run.get("signature_timestamped") is not True):
                errors.append(f"{label}: verified signer, trust and timestamp are required.")
        if run.get("signing_policy") == "developer-id-notarized" and run.get("notarized") is not True:
            errors.append(f"{label}: Developer ID policy requires verified notarization.")
        coverage = run.get("os_coverage")
        if (not isinstance(coverage, list) or len(coverage) != 2
                or any(not isinstance(row, dict) or not nonempty(row.get("os_version"))
                       or row.get("coverage") not in ("minimum", "current") for row in coverage)
                or {row["coverage"] for row in coverage} != {"minimum", "current"}):
            errors.append(f"{label}: separate minimum/current OS installation records are required.")
        upgrades = run.get("upgrades")
        if not isinstance(upgrades, list) or not upgrades:
            errors.append(f"{label}: at least one actual different-version upgrade is required.")
            return
        for upgrade in upgrades:
            if not isinstance(upgrade, dict):
                errors.append(f"{label}: invalid upgrade record.")
                continue
            before, after = stable_version(upgrade.get("from_version")), stable_version(upgrade.get("to_version"))
            if before is None or after is None or after != candidate_version or before >= after:
                errors.append(f"{label}: upgrade versions must increase to the exact candidate version.")
            preserved = upgrade.get("preserved_data")
            if not isinstance(preserved, dict):
                errors.append(f"{label}: semantic before/after preservation hashes are required.")
                continue
            for name in PRESERVED_DATA:
                pair = preserved.get(name)
                if (not isinstance(pair, dict) or not digest(pair.get("before_sha256"), 64)
                        or pair.get("before_sha256") != pair.get("after_sha256")):
                    errors.append(f"{label}: {name} preservation hashes are missing or changed.")

    runs = report.get("runs")
    if not isinstance(runs, list):
        errors.append("runs must be an array.")
        runs = []
    seen = set()
    for run in runs:
        if not isinstance(run, dict):
            errors.append("Invalid run object.")
            continue
        target, area = run.get("target"), run.get("area")
        if not isinstance(target, str) or not isinstance(area, str):
            errors.append("Run target and area must be strings.")
            continue
        label = f"{target}/{area}"
        if target not in TARGETS or area not in CHECKS or (target, area) in seen:
            errors.append(f"Unknown or duplicate run: {label}.")
            continue
        seen.add((target, area))
        for field in ("procedure", "tool_versions", "settings"):
            if not nonempty(run.get(field)):
                errors.append(f"{label}: {field} is required.")
        if run.get("manual_attestation") is not True:
            errors.append(f"{label}: manual attestation is required; synthetic tests are not a qualification.")
        try:
            timestamp = datetime.fromisoformat(str(run.get("performed_at", "")).replace("Z", "+00:00"))
            if timestamp.tzinfo is None:
                raise ValueError("Timezone required.")
        except ValueError:
            errors.append(f"{label}: performed_at requires an ISO 8601 timestamp with timezone.")
        checks = run.get("checks")
        if not isinstance(checks, list):
            errors.append(f"{label}: checks must be an array.")
            checks = []
        checked = set()
        required = check_ids(area, target)
        for check in checks:
            if not isinstance(check, dict) or not isinstance(check.get("id"), str):
                errors.append(f"{label}: invalid check.")
                continue
            name = check["id"]
            if name not in required or name in checked:
                errors.append(f"{label}: unknown or duplicate check {name}.")
                continue
            checked.add(name)
            if check.get("status") != "pass":
                errors.append(f"{label}/{name}: {check.get('status', 'missing status')}; qualification remains blocked.")
            if not nonempty(check.get("observation")):
                errors.append(f"{label}/{name}: observed results or a blocker must be recorded.")
            evidence(check.get("evidence"), label + "/" + name)
        for name in required:
            if name not in checked:
                errors.append(f"{label}: Missing check {name}.")
        if area == "performance":
            performance(run, label)
        elif area == "hardware":
            hardware(run, label)
        elif area == "vendors":
            vendors(run, label)
        elif area == "distribution":
            distribution(run, label)
        elif area == "references":
            errors.extend(reference_measurement_errors(run.get("measurements"), label))
        elif area == "accessibility" and run.get("assistive_technology") != (
                "VoiceOver" if target.startswith("macos") else "NVDA"):
            errors.append(f"{label}: the target's actual VoiceOver/NVDA tool is required.")
    for target in TARGETS:
        for area in CHECKS:
            if (target, area) not in seen:
                errors.append(f"Missing run: {target}/{area}.")
    return errors


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    subcommands = parser.add_subparsers(dest="command", required=True)
    create = subcommands.add_parser("create", help="Create an explicitly not-run evidence template.")
    create.add_argument("--candidate-sha", required=True)
    create.add_argument("--candidate-version", required=True)
    create.add_argument("--output", required=True, type=Path)
    check = subcommands.add_parser("check", help="Fail closed unless the exact candidate has complete evidence.")
    check.add_argument("report", type=Path)
    check.add_argument("--candidate-sha", required=True)
    args = parser.parse_args()
    if not digest(args.candidate_sha, 40):
        parser.error("--candidate-sha must be the exact lowercase 40-character Git commit SHA.")
    if args.command == "create" and stable_version(args.candidate_version) is None:
        parser.error("--candidate-version must be an exact stable MAJOR.MINOR.PATCH version.")
    try:
        if args.command == "create":
            args.output.parent.mkdir(parents=True, exist_ok=True)
            with args.output.open("x", encoding="utf-8") as stream:
                json.dump(new_report(args.candidate_sha, args.candidate_version), stream, indent=2, allow_nan=False)
                stream.write("\n")
            print(f"Created {args.output}. All qualifications remain not_run.")
            return 0
        report = read_json(args.report.read_text(encoding="utf-8"))
        errors = validate(report, args.report.parent, args.candidate_sha)
    except (OSError, ValueError) as error:
        print(f"Qualification error: {error}", file=sys.stderr)
        return 1
    if errors:
        for error in errors:
            print(error, file=sys.stderr)
        print(f"BLOCKED: {len(errors)} unmet evidence requirements.", file=sys.stderr)
        return 2
    print("Evidence completeness and hashes pass for this candidate; human release approval is still required.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
