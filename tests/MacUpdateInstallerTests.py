import argparse
import hashlib
from pathlib import Path
import plistlib
import shlex
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
SWAP_HELPER = None


class MacUpdateInstallerTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="studio-duo-update-drill-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.parent = self.root / "install"
        self.parent.mkdir()
        self.target = self.parent / "Studio Duo.app"
        self.bundle(self.target, "1.0.0")
        self.original_hash = self.tree_hash(self.target)
        candidate = self.root / "candidate" / "Studio Duo.app"
        self.bundle(candidate, "1.0.1")
        self.archive = self.root / "update.zip"
        subprocess.run(["/usr/bin/ditto", "-c", "-k", "--keepParent", str(candidate), str(self.archive)],
                       check=True, capture_output=True, timeout=10)
        self.script = self.root / "install-update.sh"
        shutil.copyfile(ROOT / "packaging/macos/install-update.sh", self.script)
        self.log = self.root / "update.log"
        self.user_data = self.root / "synthetic-user-data.json"
        self.user_data.write_text('{"syntheticProject":"preserved"}\n', encoding="utf-8")

    def bundle(self, path, version):
        executable = path / "Contents/MacOS/fixture"
        executable.parent.mkdir(parents=True)
        shutil.copyfile("/usr/bin/true", executable)
        executable.chmod(0o755)
        with (path / "Contents/Info.plist").open("wb") as output:
            plistlib.dump(dict(CFBundleIdentifier="dev.mbianchi.studioduo",
                               CFBundleShortVersionString=version, CFBundleVersion=version,
                               CFBundleExecutable="fixture", CFBundleName="Synthetic Studio Duo",
                               CFBundlePackageType="APPL"), output)
        subprocess.run(["/usr/bin/codesign", "--force", "--deep", "--sign", "-", str(path)],
                       check=True, capture_output=True, timeout=10)

    def tree_hash(self, root):
        value = hashlib.sha256()
        for path in sorted(root.rglob("*")):
            if path.is_file():
                value.update(str(path.relative_to(root)).encode())
                value.update(path.read_bytes())
        return value.hexdigest()

    def apply(self, *, version="1.0.1", bundle_id="dev.mbianchi.studioduo", helper=None):
        exited = subprocess.Popen(["/usr/bin/true"])
        exited.wait(timeout=5)
        result = subprocess.run(
            ["/bin/sh", str(self.script), str(exited.pid), str(self.archive), str(self.target),
             str(self.log), version, bundle_id, str(helper or SWAP_HELPER), "--no-relaunch"],
            capture_output=True, timeout=15,
        )
        self.assertEqual(self.user_data.read_text(encoding="utf-8"), '{"syntheticProject":"preserved"}\n')
        return result

    def test_different_version_replacement_is_atomic_and_preserves_user_data(self):
        result = self.apply()
        self.assertEqual(result.returncode, 0, self.log.read_text().replace(str(self.root), "<owned-update-drill>"))
        with (self.target / "Contents/Info.plist").open("rb") as source:
            self.assertEqual(plistlib.load(source)["CFBundleShortVersionString"], "1.0.1")
        subprocess.run(["/usr/bin/codesign", "--verify", "--deep", "--strict", str(self.target)],
                       check=True, capture_output=True, timeout=10)
        self.assertFalse(self.archive.exists())
        self.assertEqual(list(self.parent.glob(".studio-duo-update.*")), [])

    def test_wrong_version_or_bundle_identity_preserves_the_previous_installation(self):
        for settings in (dict(version="1.0.2"), dict(bundle_id="invalid.synthetic.bundle")):
            with self.subTest(settings=settings):
                result = self.apply(**settings)
                self.assertNotEqual(result.returncode, 0)
                self.assertEqual(self.tree_hash(self.target), self.original_hash)
                self.assertTrue(self.archive.exists())

    def test_corrupt_archive_never_moves_the_installed_app(self):
        self.archive.write_bytes(b"synthetic invalid archive")
        self.assertNotEqual(self.apply().returncode, 0)
        self.assertEqual(self.tree_hash(self.target), self.original_hash)

    def test_invalid_signature_never_replaces_the_installed_app(self):
        candidate = self.root / "candidate/Studio Duo.app"
        with (candidate / "Contents/MacOS/fixture").open("r+b") as executable:
            executable.seek(4096)
            executable.write(b"synthetic invalid code signature")
        self.archive.unlink()
        subprocess.run(["/usr/bin/ditto", "-c", "-k", "--keepParent", str(candidate), str(self.archive)],
                       check=True, capture_output=True, timeout=10)
        self.assertNotEqual(self.apply().returncode, 0)
        self.assertEqual(self.tree_hash(self.target), self.original_hash)

    def test_failed_atomic_publication_preserves_the_installed_app(self):
        helper = self.root / "fail-swap.sh"
        helper.write_text("#!/bin/sh\nexit 1\n", encoding="utf-8")
        helper.chmod(0o700)
        self.assertNotEqual(self.apply(helper=helper).returncode, 0)
        self.assertEqual(self.tree_hash(self.target), self.original_hash)

    def test_signal_after_exchange_rolls_back_and_retains_the_archive(self):
        helper = self.root / "interrupt-swap.sh"
        marker = self.root / "interrupted"
        helper.write_text(
            "#!/bin/sh\n" + shlex.quote(str(SWAP_HELPER)) + ' "$@" || exit $?\n'
            + "if [ ! -e " + shlex.quote(str(marker)) + " ]; then\n"
            + "  /usr/bin/touch " + shlex.quote(str(marker)) + '\n  /bin/kill -TERM "$PPID"\nfi\n',
            encoding="utf-8",
        )
        helper.chmod(0o700)
        self.assertNotEqual(self.apply(helper=helper).returncode, 0)
        self.assertEqual(self.tree_hash(self.target), self.original_hash)
        self.assertTrue(self.archive.exists())
        self.assertEqual(list(self.parent.glob(".studio-duo-update.*")), [])

    def test_concurrent_helper_lock_is_not_removed_or_overwritten(self):
        lock = self.parent / ".studio-duo-update.lock"
        lock.mkdir()
        sentinel = lock / "other-owner"
        sentinel.write_text("synthetic owner", encoding="utf-8")
        self.assertNotEqual(self.apply().returncode, 0)
        self.assertEqual(self.tree_hash(self.target), self.original_hash)
        self.assertEqual(sentinel.read_text(), "synthetic owner")

    def test_untrappable_kill_keeps_a_complete_app_and_the_previous_generation(self):
        helper = self.root / "kill-after-swap.sh"
        helper.write_text("#!/bin/sh\n" + shlex.quote(str(SWAP_HELPER))
                          + ' "$@" || exit $?\n/bin/kill -KILL "$PPID"\n', encoding="utf-8")
        helper.chmod(0o700)
        self.assertNotEqual(self.apply(helper=helper).returncode, 0)
        with (self.target / "Contents/Info.plist").open("rb") as source:
            self.assertEqual(plistlib.load(source)["CFBundleShortVersionString"], "1.0.1")
        stages = [path for path in self.parent.glob(".studio-duo-update.*")
                  if path.name != ".studio-duo-update.lock"]
        self.assertEqual(len(stages), 1)
        self.assertEqual(self.tree_hash(stages[0] / "Studio Duo.app"), self.original_hash)
        self.assertTrue((self.parent / ".studio-duo-update.lock").is_dir())
        self.assertTrue(self.archive.exists())

    def test_failed_rollback_preserves_both_generations_and_the_lock(self):
        helper = self.root / "fail-rollback.sh"
        marker = self.root / "swapped"
        helper.write_text(
            "#!/bin/sh\nif [ -e " + shlex.quote(str(marker)) + " ]; then exit 1; fi\n"
            + shlex.quote(str(SWAP_HELPER)) + ' "$@" || exit $?\n/usr/bin/touch '
            + shlex.quote(str(marker)) + '\n/bin/kill -TERM "$PPID"\n', encoding="utf-8")
        helper.chmod(0o700)
        self.assertNotEqual(self.apply(helper=helper).returncode, 0)
        stages = [path for path in self.parent.glob(".studio-duo-update.*")
                  if path.name != ".studio-duo-update.lock"]
        self.assertEqual(len(stages), 1)
        self.assertEqual(self.tree_hash(stages[0] / "Studio Duo.app"), self.original_hash)
        self.assertTrue((self.parent / ".studio-duo-update.lock").is_dir())
        self.assertIn("Rollback failed", self.log.read_text())


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--swap-helper", required=True, type=Path)
    args, remaining = parser.parse_known_args()
    SWAP_HELPER = args.swap_helper.resolve()
    unittest.main(argv=[__file__] + remaining)
