import importlib.util
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch
import wave


SCRIPTS = Path(__file__).resolve().parents[1] / "scripts"
sys.path.insert(0, str(SCRIPTS))
spec = importlib.util.spec_from_file_location("qualify_mastering", SCRIPTS / "qualify-mastering.py")
qualification = importlib.util.module_from_spec(spec)
spec.loader.exec_module(qualification)


class MasteringQualificationTests(unittest.TestCase):
    def test_reference_parser_reads_final_original_programme_and_separate_flushed_peak(self):
        text = "lavfi.r128.I=-24.000\nlavfi.r128.LRA=0.000\nlavfi.r128.I=-23.01\nlavfi.r128.LRA=6.10\n"
        values = qualification.parse_reference(text, "lavfi.r128.true_peak=0.707945784\n")
        self.assertEqual(values["lufs"], -23.01)
        self.assertEqual(values["lra"], 6.1)
        self.assertAlmostEqual(values["dbtp"], -3.0, places=6)

    def test_silence_is_explicitly_undefined_not_a_finite_floor(self):
        self.assertEqual(qualification.parse_reference(
            "lavfi.r128.I=-70.000\nlavfi.r128.LRA=0.000\n", "lavfi.r128.true_peak=0.000\n"),
            {"lufs": None, "lra": None, "dbtp": None})

    def test_missing_and_nonfinite_reference_results_fail(self):
        for text in ("", "{}", "lavfi.r128.I=NaN\nlavfi.r128.LRA=0\n",
                     "lavfi.r128.I=-23\nlavfi.r128.LRA=Infinity\n",
                     "lavfi.r128.I=-23\n", "lavfi.r128.I=-23\nlavfi.r128.LRA=-1\n"):
            with self.subTest(text=text), self.assertRaises(ValueError):
                qualification.parse_reference(text, "lavfi.r128.true_peak=0.5\n")
        with self.assertRaises(ValueError):
            qualification.parse_reference("lavfi.r128.I=-23\nlavfi.r128.LRA=0\n", "")

    def native_report(self):
        return {
            "schemaVersion": 1, "candidateSha": "1" * 40, "buildRevision": "1111111",
            "sourceSha256": "2" * 64, "sampleRateHz": 48000, "channels": 2, "frames": 288000,
            "measurements": {"lufs": -23.0, "lra": 0.0, "dbtp": -20.0},
        }

    def test_native_results_match_exact_source_shape_and_clean_candidate(self):
        report = self.native_report()
        self.assertEqual(qualification.validate_native(
            report, "2" * 64, 48000, 2, 288000, "1" * 40), [])
        for field, value in (("candidateSha", "3" * 40), ("buildRevision", "1111111-dirty"),
                             ("sourceSha256", "4" * 64), ("channels", 1),
                             ("frames", 287999), ("sampleRateHz", 96000)):
            with self.subTest(field=field):
                changed = dict(report, **{field: value})
                self.assertTrue(qualification.validate_native(
                    changed, "2" * 64, 48000, 2, 288000, "1" * 40))

    def test_native_missing_values_cannot_become_passes(self):
        for report in (None, [], {}, dict(self.native_report(), measurements={}),
                       dict(self.native_report(), measurements={"lufs": float("nan"), "lra": 0, "dbtp": -3})):
            with self.subTest(report=report):
                self.assertTrue(qualification.validate_native(
                    report, "2" * 64, 48000, 2, 288000, "1" * 40))

    def test_synthetic_sources_are_complete_and_deterministic(self):
        with tempfile.TemporaryDirectory(prefix="studio-duo-meter-contract-") as directory:
            first, second = Path(directory) / "first.wav", Path(directory) / "second.wav"
            frames = qualification.write_source(first, "tail-transient", 44100, 2)
            qualification.write_source(second, "tail-transient", 44100, 2)
            self.assertEqual(first.read_bytes(), second.read_bytes())
            with wave.open(str(first), "rb") as source:
                self.assertEqual((source.getframerate(), source.getnchannels(), source.getsampwidth()),
                                 (44100, 2, 4))
                self.assertEqual(source.getnframes(), frames)
            with self.assertRaises(FileExistsError):
                qualification.write_source(first, "silence", 44100, 2)

    def test_exact_candidate_requires_clean_matching_checkout(self):
        for results in (["1" * 40, ""], ["2" * 40, ""], ["1" * 40, " M src/synthetic.cpp"]):
            with self.subTest(results=results), patch.object(qualification, "run_process", side_effect=results):
                if results == ["1" * 40, ""]:
                    qualification.verify_candidate_checkout("1" * 40, Path("."), [])
                else:
                    with self.assertRaises(ValueError):
                        qualification.verify_candidate_checkout("1" * 40, Path("."), [])


if __name__ == "__main__":
    unittest.main()
