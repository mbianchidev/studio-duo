import copy
import hashlib
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest


spec = importlib.util.spec_from_file_location(
    "qualify", Path(__file__).resolve().parents[1] / "scripts" / "qualify.py"
)
qualify = importlib.util.module_from_spec(spec)
spec.loader.exec_module(qualify)


class QualificationTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="studio-duo-qualification-test-")
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.sha = "1" * 40
        self.report = qualify.new_report(self.sha)

    def issues(self, report=None):
        return qualify.validate(report or self.report, self.root, self.sha)

    def completed_report(self):
        report = copy.deepcopy(self.report)
        raw = self.root / "synthetic-evidence.txt"
        raw.write_text("Synthetic test evidence, not a real qualification.", encoding="utf-8")
        digest = hashlib.sha256(raw.read_bytes()).hexdigest()
        for system in report["systems"]:
            system.update(
                os_version="Synthetic OS 1", cpu="Synthetic CPU", ram_gib=32,
                storage="Synthetic SSD", binary_sha256="2" * 64,
            )
        for run in report["runs"]:
            run.update(
                procedure="Synthetic reproducible procedure",
                tool_versions="Synthetic driver or assistive tool 1",
                settings="Synthetic documented settings",
                performed_at="2026-10-17T12:00:00+00:00",
                manual_attestation=True,
            )
            for check in run["checks"]:
                check.update(
                    status="pass", observation="Synthetic expected outcome observed",
                    evidence=[{"path": raw.name, "sha256": digest}],
                )
            if run["area"] == "vendors":
                formats = ["VST3", "CLAP"]
                if run["target"].startswith("macos"):
                    formats.append("AudioUnit")
                run["processors"] = [
                    dict(name="Synthetic processor", version="1", format=fmt,
                         architecture=run["target"].split("-", 1)[1],
                         role=role, mode="sandboxed", approved=True)
                    for fmt in formats for role in ("instrument", "effect")
                ]
                for mode in ("trustedInProcess", "araCompatibility"):
                    run["processors"].append(dict(
                        name="Synthetic reduced-isolation processor", version="1", format="VST3",
                        architecture=run["target"].split("-", 1)[1], role="effect", mode=mode,
                        ara_capable=mode == "araCompatibility", approved=True,
                    ))
                run["reduced_isolation_disposition"] = {
                    "approved": True, "evidence": [{"path": raw.name, "sha256": digest}],
                    "rationale": "Synthetic test-only disposition",
                }
            if run["area"] == "performance":
                run["measurements"] = [
                    dict(sample_rate_hz=rate, buffer_samples=buffer, duration_seconds=1800,
                         tracks=16, songs=3, deadline_misses=0, callback_p99_ratio=0.5,
                         peak_working_set_bytes=1024**3, warm_open_seconds=1,
                         save_seconds=1, graph_edit_seconds=0.1, viewport_ms=10,
                         null_peak_error=0.000001)
                    for rate in qualify.SAMPLE_RATES for buffer in qualify.BUFFER_SIZES
                ]
            if run["area"] == "hardware":
                run["measurements"] = dict(
                    reamp_residual_samples=0, dropped_audio_samples=0, duplicated_audio_samples=0,
                    dropped_midi_events=0, duplicated_midi_events=0,
                )
        return report

    def test_template_is_explicitly_incomplete_on_every_target(self):
        self.assertEqual(len(self.report["systems"]), 4)
        self.assertEqual(len(self.report["runs"]), 16)
        self.assertTrue(all(c["status"] == "not_run" for r in self.report["runs"] for c in r["checks"]))
        self.assertTrue(self.issues())

    def test_complete_synthetic_contract_is_accepted(self):
        self.assertEqual(self.issues(self.completed_report()), [])

    def test_wrong_candidate_and_missing_matrix_rows_are_rejected(self):
        report = self.completed_report()
        report["candidate_sha"] = "3" * 40
        report["runs"].pop()
        errors = self.issues(report)
        self.assertTrue(any("candidate" in e for e in errors))
        self.assertTrue(any("Missing" in e for e in errors))

    def test_duplicate_and_unknown_checks_cannot_fill_coverage(self):
        report = self.completed_report()
        report["runs"][0]["checks"].append(copy.deepcopy(report["runs"][0]["checks"][0]))
        report["runs"][1]["checks"][0]["id"] = "made-up"
        self.assertTrue(self.issues(report))

    def test_pass_requires_manual_attestation_and_actual_raw_evidence(self):
        report = self.completed_report()
        report["runs"][0]["manual_attestation"] = False
        report["runs"][1]["checks"][0]["evidence"][0]["path"] = "missing.txt"
        self.assertTrue(self.issues(report))

    def test_tampered_evidence_and_path_escape_are_rejected(self):
        report = self.completed_report()
        report["runs"][0]["checks"][0]["evidence"][0]["sha256"] = "0" * 64
        report["runs"][1]["checks"][0]["evidence"][0]["path"] = "../private.txt"
        report["runs"][2]["checks"][0]["evidence"][0]["path"] = "C:\\private.txt"
        self.assertGreaterEqual(len(self.issues(report)), 3)

    def test_skips_and_failed_checks_never_pass(self):
        for status in ("skip", "blocked", "fail", "not_run"):
            report = self.completed_report()
            report["runs"][0]["checks"][0]["status"] = status
            self.assertTrue(self.issues(report))

    def test_all_rate_buffer_pairs_are_required_without_duplicates(self):
        report = self.completed_report()
        performance = next(r for r in report["runs"] if r["area"] == "performance")
        performance["measurements"][-1] = copy.deepcopy(performance["measurements"][0])
        self.assertTrue(self.issues(report))

    def test_resource_deadline_and_null_thresholds_are_enforced(self):
        for field, value in (("deadline_misses", 1), ("callback_p99_ratio", 0.71),
                             ("duration_seconds", 1799), ("graph_edit_seconds", 0.251),
                             ("null_peak_error", 0.00002), ("peak_working_set_bytes", 25 * 1024**3),
                             ("save_seconds", float("nan")), ("tracks", True), ("tracks", 16.5)):
            with self.subTest(field=field):
                report = self.completed_report()
                run = next(r for r in report["runs"] if r["area"] == "performance")
                run["measurements"][0][field] = value
                self.assertTrue(self.issues(report))

    def test_physical_reamp_and_loss_tolerances_are_enforced(self):
        report = self.completed_report()
        run = next(r for r in report["runs"] if r["area"] == "hardware")
        run["measurements"]["reamp_residual_samples"] = -2
        run["measurements"]["duplicated_midi_events"] = 1
        self.assertTrue(self.issues(report))

    def test_vendor_formats_roles_and_exception_disposition_are_required(self):
        report = self.completed_report()
        run = next(r for r in report["runs"] if r["area"] == "vendors")
        run["processors"].pop()
        run["reduced_isolation_disposition"]["approved"] = False
        self.assertTrue(self.issues(report))

    def test_ara_format_and_explicit_vendor_approval_are_required(self):
        report = self.completed_report()
        run = next(r for r in report["runs"] if r["area"] == "vendors")
        run["processors"][-1]["format"] = "CLAP"
        run["processors"][0]["approved"] = False
        self.assertTrue(self.issues(report))

    def test_native_windows_architecture_label_is_accepted(self):
        report = self.completed_report()
        for run in report["runs"]:
            if run["area"] == "vendors" and run["target"].startswith("windows"):
                for processor in run["processors"]:
                    processor["architecture"] = "x86_64"
        self.assertEqual(self.issues(report), [])

    def test_extreme_untrusted_ram_metadata_does_not_overflow(self):
        report = self.completed_report()
        report["systems"][0]["ram_gib"] = 10**400
        self.assertTrue(self.issues(report))

    def test_invalid_shapes_are_errors_not_exceptions(self):
        for report in ([], {}, {"systems": None}, dict(self.report, runs=[None]),
                       dict(self.report, systems="invalid")):
            with self.subTest(report=report):
                self.assertTrue(qualify.validate(report, self.root, self.sha))

    def test_json_rejects_duplicate_keys_and_nonfinite_numbers(self):
        for text in ('{"candidate_sha":"a","candidate_sha":"b"}', '{"value":NaN}'):
            with self.assertRaises(ValueError):
                qualify.read_json(text)


if __name__ == "__main__":
    unittest.main()
