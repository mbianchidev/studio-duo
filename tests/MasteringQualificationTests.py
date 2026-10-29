import argparse
import importlib.util
from array import array
import copy
from pathlib import Path
import sys
import subprocess
import tempfile
import unittest
from unittest.mock import patch
import wave


SCRIPTS = Path(__file__).resolve().parents[1] / "scripts"
sys.path.insert(0, str(SCRIPTS))
spec = importlib.util.spec_from_file_location("qualify_mastering", SCRIPTS / "qualify-mastering.py")
qualification = importlib.util.module_from_spec(spec)
spec.loader.exec_module(qualification)
NATIVE_BINARY = None


class MasteringQualificationTests(unittest.TestCase):
    def test_delivery_private_cleanup_preserves_public_evidence_and_unrelated_files(self):
        with tempfile.TemporaryDirectory(prefix="studio-duo-delivery-cleanup-") as directory:
            root = Path(directory) / "native"
            case = qualification.delivery_cases()[0]
            case_directory = root / case["id"]
            case_directory.mkdir(parents=True)
            signing = root / "private-signing"
            signing.mkdir()
            (signing / "render-report-key.json").write_text('{"syntheticTestKey":true}', encoding="utf-8")
            for name in ("master", "repeat"):
                (case_directory / f"{name}.report.json").write_text('{"syntheticPrivateReport":true}', encoding="utf-8")
            public = root / "report.json"
            public.write_bytes(b"synthetic public evidence")
            encoded = case_directory / "master.wav"
            encoded.write_bytes(b"synthetic encoded evidence")
            sibling = Path(directory) / "unrelated"
            sibling.write_bytes(b"keep")
            qualification.cleanup_delivery_private_files(root, [case])
            qualification.cleanup_delivery_private_files(root, [case])
            self.assertFalse(signing.exists())
            self.assertFalse(any(case_directory.glob("*.report.json")))
            self.assertEqual(public.read_bytes(), b"synthetic public evidence")
            self.assertEqual(encoded.read_bytes(), b"synthetic encoded evidence")
            self.assertEqual(sibling.read_bytes(), b"keep")

    def test_native_delivery_cli_rejects_bad_inputs_without_overwriting_existing_data(self):
        if NATIVE_BINARY is None:
            self.skipTest("Pass --test-binary to exercise the real driver-free native CLI.")
        with tempfile.TemporaryDirectory(prefix="studio-duo-delivery-cli-") as directory:
            root = Path(directory).resolve()
            fixture = qualification.write_delivery_fixture(root)
            request = root / "fixture.json"
            qualification.write_json(request, fixture)
            existing = root / "existing"
            existing.mkdir()
            marker = existing / "keep"
            marker.write_bytes(b"synthetic destination")

            def invoke(arguments, expected_message=None):
                result = subprocess.run([str(NATIVE_BINARY), *arguments], stdin=subprocess.DEVNULL,
                                        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=20, check=False)
                self.assertEqual(result.returncode, 1, result.stdout.decode("utf-8", errors="replace"))
                self.assertIn(b"Mastering delivery fixture failed:", result.stdout)
                if expected_message is not None:
                    self.assertIn(expected_message, result.stdout)
                self.assertEqual(marker.read_bytes(), b"synthetic destination")
                self.assertEqual(list(existing.iterdir()), [marker])

            base = ["--export-mastering=" + str(request), "--delivery-output=" + str(root / "output")]
            invoke([base[0], "--delivery-output=" + str(existing)])
            for arguments in ([base[0]], base + [base[0]], base + ["--unknown-setting"], [
                    "--export-mastering=relative.json", base[1]]):
                with self.subTest(arguments=arguments):
                    invoke(arguments)
                    self.assertFalse((root / "output").exists())
            for index, mutation in enumerate(("wrong-hash", "unsafe-source", "bad-codec", "duplicate-id")):
                changed = copy.deepcopy(fixture)
                source = changed["album"]["tracks"][0]["sources"][0]
                if mutation == "wrong-hash":
                    source["sourceHash"] = "0" * 64
                elif mutation == "unsafe-source":
                    source["file"] = "../excluded.wav"
                elif mutation == "bad-codec":
                    changed["exports"][0]["format"] = "unavailable"
                else:
                    changed["exports"][1]["id"] = changed["exports"][0]["id"]
                    changed["exports"] = changed["exports"][:2]
                changed_request = root / f"bad-{index}.json"
                qualification.write_json(changed_request, changed)
                with self.subTest(mutation=mutation):
                    invoke(["--export-mastering=" + str(changed_request),
                            "--delivery-output=" + str(root / f"bad-output-{index}")],
                           b"unique safe leaf names" if mutation == "duplicate-id"
                           else b"Invalid delivery case settings" if mutation == "bad-codec"
                           else b"A fixture source is unsafe, changed")
                    self.assertFalse(any(root.rglob("render-report-key.json")))
                    self.assertFalse(any(root.rglob("*.report.json")))

    def test_delivery_matrix_covers_integer_dither_float_lossless_and_lossy_exports(self):
        cases = qualification.delivery_cases()
        self.assertEqual(len(cases), 29)
        self.assertEqual(len({case["id"] for case in cases}), len(cases))
        for rate in (44100, 48000, 96000):
            for channels in (1, 2):
                for depth in (16, 24):
                    self.assertTrue(any(
                        case["format"] == "wav" and case["sampleRate"] == rate
                        and case["channels"] == channels and case["bitDepth"] == depth
                        and case["dither"] == "tpdf" for case in cases))
        for mode in ("constant", "variable"):
            for rate in (44100, 48000):
                for channels in (1, 2):
                    self.assertTrue(any(
                        case["format"] == "mp3" and case["mp3BitrateMode"] == mode
                        and case["sampleRate"] == rate and case["channels"] == channels
                        for case in cases))
        self.assertEqual({case["format"] for case in cases}, {"wav", "aiff", "flac", "ogg", "mp3"})

    def test_independent_delivery_checks_fail_closed_on_corrupt_or_missing_audio_and_tags(self):
        case = qualification.delivery_cases()[0]
        metadata = {"title": "Synthetic release", "artist": "Fixture Ensemble"}
        probe = {"streams": [{
            "codec_name": "pcm_s16le", "sample_rate": str(case["sampleRate"]),
            "channels": case["channels"], "bits_per_sample": 16,
        }], "format": {"tags": metadata}}
        qualification.validate_delivery_stream(probe, case, metadata)
        for field, value in (("codec_name", "mp3"), ("sample_rate", "48000"),
                             ("channels", 2), ("bits_per_sample", 24)):
            changed = copy.deepcopy(probe)
            changed["streams"][0][field] = value
            with self.subTest(field=field), self.assertRaises(ValueError):
                qualification.validate_delivery_stream(changed, case, metadata)
        with self.assertRaises(ValueError):
            qualification.validate_delivery_stream(dict(probe, format={"tags": {}}), case, metadata)
        with self.assertRaises(ValueError):
            qualification.validate_delivery_stream(dict(probe, streams=[]), case, metadata)
        exact = array("f", [0.0, 0.25, -0.5])
        self.assertEqual(qualification.audio_difference(exact, exact)["maximumAbsoluteSampleError"], 0)
        for invalid in (array("f"), array("f", [0, float("nan"), 0]),
                        array("f", [0, float("inf"), 0])):
            with self.assertRaises(ValueError):
                qualification.audio_difference(invalid, exact)
        self.assertGreater(qualification.audio_difference(array("f", [0, 0, 0]), exact)["rmsError"], 0.1)
        lsb = 2 ** -15
        noise = array("f", [value * lsb for value in (-1, 0, 0, 0, 1, 0, 0, 0)] * 1024)
        stats = qualification.dither_statistics(noise, 1, 16)
        self.assertEqual(stats["channels"][0]["meanLsb"], 0)
        self.assertEqual(stats["channels"][0]["rmsLsb"], 0.5)
        for invalid in (array("f", [0] * 8192), array("f", [lsb] * 8192)):
            with self.assertRaises(ValueError):
                qualification.dither_statistics(invalid, 1, 16)

    def test_delivery_metadata_never_excuses_missing_vorbis_copyright(self):
        case = next(case for case in qualification.delivery_cases() if case["format"] == "ogg")
        probe = dict(streams=[dict(codec_name="vorbis", sample_rate=str(case["sampleRate"]),
                                   channels=case["channels"])], format=dict(tags={}))
        with self.assertRaises(ValueError):
            qualification.validate_delivery_stream(probe, case, {"copyright": "Synthetic material only"})
        probe["streams"][0]["tags"] = {"COPYRIGHT": "Synthetic material only"}
        qualification.validate_delivery_stream(probe, case, {"copyright": "Synthetic material only"})

    def test_delivery_oracle_checks_selected_sources_overlap_gaps_fades_and_mono(self):
        first = dict(sources=[dict(id="unused", file="unused"), dict(id="selected", file="first")],
                     selectedSourceId="selected", gapBeforeSeconds=0.25, overlapPreviousSeconds=0,
                     fadeInSeconds=0.25, fadeOutSeconds=0.25, gainDecibels=0)
        second = dict(sources=[dict(id="second", file="second")], selectedSourceId="second",
                      gapBeforeSeconds=0, overlapPreviousSeconds=0.5,
                      fadeInSeconds=0, fadeOutSeconds=0, gainDecibels=0)
        sources = {"first": dict(channels=2, samples=array("f", [0.25, 0.5] * 4)),
                   "second": dict(channels=1, samples=array("f", [0.125] * 4))}
        album = dict(tracks=[first, second], outputGainDecibels=0)
        stereo = qualification.assemble_delivery(album, sources, 4, 2)
        self.assertEqual(stereo, array("f", [0, 0, 0, 0, 0.25, 0.5, 0.375, 0.625,
                                           0.125, 0.125, 0.125, 0.125, 0.125, 0.125]))
        mono = qualification.assemble_delivery(album, sources, 4, 1)
        self.assertEqual(mono, array("f", [sum(stereo[i:i + 2]) / 2 for i in range(0, len(stereo), 2)]))
        with tempfile.TemporaryDirectory(prefix="studio-duo-delivery-contract-") as directory:
            fixture = qualification.write_delivery_fixture(Path(directory))
            self.assertEqual(len(fixture["album"]["tracks"]), 3)
            self.assertEqual(len(fixture["exports"]), 29)
            self.assertTrue(all(not Path(source["file"]).is_absolute()
                                for track in fixture["album"]["tracks"] for source in track["sources"]))
            for name, rate, channels in (("song-a.wav", 96000, 2), ("song-b.wav", 48000, 2),
                                         ("song-c.wav", 44100, 1), ("excluded.wav", 48000, 2)):
                with wave.open(str(Path(directory) / name), "rb") as source:
                    self.assertEqual((source.getframerate(), source.getnchannels(), source.getnframes()),
                                     (rate, channels, rate * 4))

    def test_delivery_native_manifest_requires_every_case_settings_signature_and_exact_candidate(self):
        cases = qualification.delivery_cases()
        rows = []
        for case in cases:
            report = dict(scope="mastering-album", mode="offline", status="success", durationSeconds=12,
                          format=case["format"], sampleRate=case["sampleRate"],
                          bitDepth=0 if case["format"] in ("mp3", "ogg") else case["bitDepth"],
                          sourceHash="2" * 64, chainHash="3" * 64, outputHash="4" * 64, settingsHash="5" * 64,
                          integratedLoudnessLufs=-23, loudnessRangeLu=3, truePeakDbtp=-10,
                          samplePeakDbfs=-10.1, correlation=0.9)
            row = dict(id=case["id"], settings=qualification.delivery_settings(case))
            for name in ("master", "repeat"):
                row[name] = dict(file=f"{name}.{case['format']}", report=report,
                                 nativeDecodedFile=name + ".native.f32le",
                                 nativeDecodedFrames=case["sampleRate"] * 12, nativeDecodedChannels=case["channels"],
                                 signedReportSha256="6" * 64, signatureVerifiedBeforeRedaction=True)
            rows.append(row)
        native = dict(schemaVersion=1, candidateSha="1" * 40, buildRevision="1111111",
                      fixtureSha256="7" * 64, exports=rows)
        self.assertEqual(qualification.validate_delivery_native(native, "7" * 64, cases, "1" * 40), [])
        for field, value in (("candidateSha", "2" * 40), ("buildRevision", "1111111-dirty"),
                             ("fixtureSha256", "8" * 64), ("exports", rows[:-1]),
                             ("exports", rows + [rows[0]])):
            with self.subTest(field=field):
                self.assertTrue(qualification.validate_delivery_native(
                    dict(native, **{field: value}), "7" * 64, cases, "1" * 40))
        for mutation in ("unsigned", "wrong-settings", "missing-metric", "nonfinite"):
            changed = copy.deepcopy(native)
            first = changed["exports"][0]
            if mutation == "unsigned":
                first["master"]["signatureVerifiedBeforeRedaction"] = False
            elif mutation == "wrong-settings":
                first["settings"]["normalizePeak"] = True
            elif mutation == "missing-metric":
                del first["master"]["report"]["integratedLoudnessLufs"]
            else:
                first["repeat"]["report"]["truePeakDbtp"] = float("nan")
            with self.subTest(mutation=mutation):
                self.assertTrue(qualification.validate_delivery_native(changed, "7" * 64, cases, "1" * 40))

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
    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument("--test-binary", type=Path)
    options, remaining = parser.parse_known_args()
    NATIVE_BINARY = options.test_binary.resolve() if options.test_binary is not None else None
    unittest.main(argv=[sys.argv[0], *remaining])
