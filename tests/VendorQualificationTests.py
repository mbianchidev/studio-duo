import copy
import hashlib
import importlib.util
import os
from pathlib import Path
import stat
import sys
import tempfile
import unittest
from unittest import mock
from zipfile import ZipFile, ZipInfo


spec = importlib.util.spec_from_file_location(
    "qualify_vendors", Path(__file__).resolve().parents[1] / "scripts" / "qualify-vendors.py"
)
vendors = importlib.util.module_from_spec(spec)
spec.loader.exec_module(vendors)


class VendorQualificationTests(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory(prefix="studio-duo-vendor-contract-")
        self.addCleanup(directory.cleanup)
        self.root = Path(directory.name)

    def archive(self, bad=None):
        path = self.root / "synthetic.zip"
        with ZipFile(path, "w") as output:
            for name in vendors.TARGETS:
                target = name if name.endswith(".clap") else name + "/Contents/mock.bin"
                output.writestr(target, b"Synthetic bytes; never a real processor.")
            output.writestr("Surge XT.exe", b"Must never be extracted or executed.")
            if bad:
                name, data = bad
                if isinstance(name, str):
                    entry = ZipInfo("synthetic-invalid-entry")
                    entry.filename = entry.orig_filename = name
                    name = entry
                output.writestr(name, data)
        return path

    def report(self):
        checks = vendors.REQUIRED_CHECKS | {"workflow-effect-audio"}
        return {
            "status": "pass", "qualificationStatus": "not-qualified",
            "hostAudioDeviceManagerCreated": False, "workflowsRequested": True,
            "scope": "vendor-headless-app-workflow-probe",
            "candidateSha": "1" * 40, "buildRevision": "1111111",
            "effectSandboxRequested": False,
            "processors": [{
                "status": "pass", "role": "effect", "name": "Synthetic processor",
                "version": "1", "manufacturer": "Synthetic vendor", "architecture": "synthetic",
                "format": "CLAP",
                "workflowMeasurements": dict(
                    {name: 0.0 for name in vendors.NULL_MEASUREMENTS},
                    sampleRate=48000, bufferSize=512, contentSamples=24000, nullTolerance=1e-5,
                    inputBuses=1, outputBuses=1,
                ),
                "checks": [{"name": name, "status": "pass"} for name in sorted(checks)]
                    + [{"name": "workflow-sidechain", "status": "skip"},
                       {"name": "workflow-multi-output", "status": "skip"}],
            }],
        }

    def test_hash_and_size_are_checked_before_loading_vendor_code(self):
        path = self.archive()
        expected = {"bytes": path.stat().st_size, "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}
        vendors.verify_archive(path, expected)
        with self.assertRaises(vendors.ProbeError):
            vendors.verify_archive(path, dict(expected, sha256="0" * 64))
        with self.assertRaises(vendors.ProbeError):
            vendors.verify_archive(path, dict(expected, bytes=expected["bytes"] + 1))

    def test_only_the_four_explicit_plugin_targets_are_extracted(self):
        output = self.root / "plugins"
        vendors.extract_plugins(self.archive(), output)
        self.assertEqual({path.name for path in output.iterdir()}, set(vendors.TARGETS))
        self.assertFalse((output / "Surge XT.exe").exists())

    def test_traversal_drive_names_and_backslashes_are_rejected(self):
        for index, name in enumerate((
                "../escape", "/absolute", "C:/escape", "Surge XT.vst3/../escape",
                "Surge XT.vst3/Contents/stream:data", "Surge XT.vst3\\escape",
                "./Surge XT.clap", "Surge XT.vst3//alias")):
            with self.subTest(name=name), self.assertRaises(vendors.ProbeError):
                vendors.extract_plugins(self.archive((name, b"Invalid entry")), self.root / str(index))
        self.assertFalse((self.root / "escape").exists())

    def test_symbolic_links_and_incomplete_archives_are_rejected(self):
        link = ZipInfo("Surge XT.vst3/Contents/link")
        link.create_system = 3
        link.external_attr = (stat.S_IFLNK | 0o777) << 16
        with self.assertRaises(vendors.ProbeError):
            vendors.extract_plugins(self.archive((link, "../escape")), self.root / "links")
        path = self.root / "incomplete.zip"
        with ZipFile(path, "w") as output:
            output.writestr("Surge XT.clap", b"Synthetic")
        with self.assertRaises(vendors.ProbeError):
            vendors.extract_plugins(path, self.root / "incomplete")

    def test_reports_preserve_skips_without_claiming_full_qualification(self):
        report = self.report()
        self.assertEqual(vendors.validate_probe(report, "1" * 40), [])
        self.assertEqual(report["processors"][0]["checks"][-1]["status"], "skip")
        report["qualificationStatus"] = "qualified"
        self.assertTrue(vendors.validate_probe(report, "1" * 40))

    def test_wrong_sha_dirty_builds_and_missing_checks_are_rejected(self):
        for change in ({"candidateSha": "2" * 40}, {"buildRevision": "1111111-dirty"},
                       {"workflowsRequested": False}):
            report = dict(self.report(), **change)
            self.assertTrue(vendors.validate_probe(report, "1" * 40))
        report = self.report()
        report["processors"][0]["checks"].pop(0)
        self.assertTrue(vendors.validate_probe(report))
        report = self.report()
        report["effectSandboxRequested"] = True
        self.assertTrue(vendors.validate_probe(report))

    def test_malformed_or_duplicate_checks_do_not_create_a_pass(self):
        for value in (None, [], {"processors": []}):
            self.assertTrue(vendors.validate_probe(value))
        report = self.report()
        report["processors"][0]["checks"].append(copy.deepcopy(report["processors"][0]["checks"][0]))
        self.assertTrue(vendors.validate_probe(report))
        report = self.report()
        report["processors"][0]["checks"][0]["status"] = "skip"
        self.assertTrue(vendors.validate_probe(report))

    def test_personal_paths_are_redacted_recursively_without_mutating_input(self):
        value = {"message": "/mock-home/work/private/file", "nested": [r"C:\mock-profile\state"]}
        redacted = vendors.redact(value, [("/mock-home/work", "<work>"), (r"C:\mock-profile", "<home>")])
        self.assertEqual(redacted, {"message": "<work>/private/file", "nested": [r"<home>\state"]})
        self.assertEqual(value["message"], "/mock-home/work/private/file")

    def test_native_pass_flags_cannot_override_the_actual_null_measurements(self):
        for value in (0.00001001, -1, None, float("nan"), float("inf"), True):
            report = self.report()
            report["processors"][0]["workflowMeasurements"]["simultaneousMaximumNullError"] = value
            self.assertTrue(vendors.validate_probe(report))
        report = self.report()
        report["processors"][0]["workflowMeasurements"]["nullTolerance"] = 0.1
        self.assertTrue(vendors.validate_probe(report))

    def test_advertised_buses_require_measured_native_reference_routes(self):
        report = self.report()
        report["processors"][0]["workflowMeasurements"]["outputBuses"] = 3
        self.assertTrue(vendors.validate_probe(report))
        report = self.report()
        report["processors"][0]["workflowMeasurements"]["inputBuses"] = 2
        self.assertTrue(vendors.validate_probe(report))

    def test_timeouts_terminate_only_the_owned_probe(self):
        code, text, timed_out, truncated = vendors.run_bounded(
            [sys.executable, "-c", "import time; print('Synthetic probe', flush=True); time.sleep(10)"],
            self.root, None, 0.1,
        )
        self.assertTrue(timed_out)
        self.assertNotEqual(code, 0)
        self.assertIsInstance(text, str)
        self.assertFalse(truncated)

    def test_probes_inherit_only_runtime_environment_not_host_credentials(self):
        with mock.patch.dict(os.environ, {"PATH": "mock-runtime", "GH_TOKEN": "synthetic-not-a-token",
                                          "UNRELATED_PRIVATE_SETTING": "synthetic"}, clear=True):
            environment = vendors.probe_environment(self.root / "home", self.root / "tmp")
        self.assertEqual(environment["PATH"], "mock-runtime")
        self.assertEqual(environment["HOME"], str(self.root / "home"))
        self.assertEqual(environment["APPDATA"], str(self.root / "home/AppData/Roaming"))
        self.assertEqual(environment["LOCALAPPDATA"], str(self.root / "home/AppData/Local"))
        self.assertNotIn("GH_TOKEN", environment)
        self.assertNotIn("UNRELATED_PRIVATE_SETTING", environment)


if __name__ == "__main__":
    unittest.main()
