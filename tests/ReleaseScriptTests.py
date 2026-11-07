import errno
import os
from pathlib import Path
import select
import shlex
import signal
import subprocess
import tempfile
import time
import unittest

if os.name == "posix":
    import pty


RELEASE_SCRIPT = Path(__file__).resolve().parents[1] / "scripts" / "release.sh"


@unittest.skipUnless(os.name == "posix", "Release terminal tests require a POSIX pseudoterminal.")
class ReleaseScriptTests(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory(prefix="studio-duo-release-")
        self.addCleanup(directory.cleanup)
        self.root = Path(directory.name)
        self.checkout = self.root / "checkout"
        self.checkout.mkdir()
        tools = self.root / "tools"
        tools.mkdir()
        self.pager_log = self.root / "pager.log"
        self.build_log = self.root / "build.log"
        self.env = {key: value for key, value in os.environ.items()
                    if not key.startswith("GIT_") and key != "PAGER"}
        self.env.update({
            "GIT_CONFIG_NOSYSTEM": "1",
            "GIT_CONFIG_GLOBAL": os.devnull,
            "GIT_TERMINAL_PROMPT": "0",
            "PATH": str(tools) + os.pathsep + os.environ["PATH"],
            "TERM": "xterm",
            "STUDIO_DUO_RELEASE_PAGER_LOG": str(self.pager_log),
            "STUDIO_DUO_RELEASE_BUILD_LOG": str(self.build_log),
        })
        self.pager = tools / "pager"
        self.pager.write_text(
            '#!/usr/bin/env bash\nprintf "pager started\\n" >> "$STUDIO_DUO_RELEASE_PAGER_LOG"\ncat\n',
            encoding="utf-8")
        self.pager.chmod(0o700)
        for name in ("cmake", "ctest"):
            tool = tools / name
            tool.write_text(
                f'#!/usr/bin/env bash\nprintf "{name} %s\\n" "$*" >> "$STUDIO_DUO_RELEASE_BUILD_LOG"\n',
                encoding="utf-8")
            tool.chmod(0o700)
        self.git("init", "--bare", str(self.root / "remote.git"))
        self.git("init", "--initial-branch=main")
        self.git("config", "user.name", "Release Fixture")
        self.git("config", "user.email", "release@example.invalid")
        self.git("config", "core.hooksPath", str(self.root / "no-hooks"))
        self.git("config", "tag.gpgSign", "false")
        (self.checkout / "VERSION").write_text("1.2.3\n", encoding="utf-8")
        self.git("add", "VERSION")
        self.git("commit", "--no-gpg-sign", "-m", "Initial synthetic release")
        self.git("remote", "add", "origin", str(self.root / "remote.git"))
        self.git("push", "--set-upstream", "origin", "main")

    def git(self, *arguments):
        return subprocess.run(
            ["git", *arguments], cwd=self.checkout, env=self.env, check=True,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, timeout=10).stdout.strip()

    def assert_release_without_pager(self):
        master, slave = pty.openpty()
        output = bytearray()
        with os.fdopen(master, "rb", buffering=0) as terminal, os.fdopen(slave, "wb", buffering=0) as destination:
            with subprocess.Popen(
                    ["bash", str(RELEASE_SCRIPT)], cwd=self.checkout, env=self.env,
                    stdin=subprocess.DEVNULL, stdout=destination, stderr=subprocess.STDOUT,
                    start_new_session=True) as process:
                destination.close()
                deadline = time.monotonic() + 20
                try:
                    while True:
                        remaining = deadline - time.monotonic()
                        if remaining <= 0 or not select.select([terminal], [], [], remaining)[0]:
                            raise subprocess.TimeoutExpired(process.args, 20)
                        try:
                            chunk = terminal.read(4096)
                        except OSError as error:
                            if error.errno != errno.EIO:
                                raise
                            break
                        if not chunk:
                            break
                        output.extend(chunk)
                    returncode = process.wait(timeout=max(0, deadline - time.monotonic()))
                except subprocess.TimeoutExpired:
                    os.killpg(process.pid, signal.SIGKILL)
                    process.wait()
                    self.fail("Release timed out:\n" + output.decode("utf-8", errors="replace"))
        self.assertEqual(returncode, 0, output.decode("utf-8", errors="replace"))
        self.assertFalse(self.pager_log.exists(), "Release validation launched an interactive Git pager.")
        self.assertEqual((self.checkout / "VERSION").read_text(encoding="utf-8"), "1.2.4\n")
        self.assertEqual(self.git("log", "-1", "--format=%s"), "chore: release v1.2.4")
        self.assertEqual(self.git("cat-file", "-t", "v1.2.4"), "tag")
        self.assertEqual(self.git("rev-parse", "HEAD"), self.git("rev-parse", "origin/main"))
        self.assertEqual(
            self.git("ls-remote", "--tags", "origin", "refs/tags/v1.2.4").split()[0],
            self.git("rev-parse", "v1.2.4"))
        self.assertEqual(self.git("status", "--porcelain"), "")
        self.assertEqual(self.build_log.read_text(encoding="utf-8").splitlines(), [
            "cmake -S . -B build/release-validation -DSTUDIO_DUO_BUILD_TESTS=ON -DCMAKE_BUILD_TYPE=Release",
            "cmake --build build/release-validation --config Release --parallel",
            "ctest --test-dir build/release-validation --build-config Release --output-on-failure",
        ])

    def test_release_bypasses_git_pager(self):
        self.env["GIT_PAGER"] = shlex.quote(str(self.pager))
        self.assert_release_without_pager()

    def test_release_bypasses_diff_pager(self):
        self.git("config", "pager.diff", shlex.quote(str(self.pager)))
        self.assert_release_without_pager()

    def test_release_bypasses_fallback_pager(self):
        self.env["PAGER"] = shlex.quote(str(self.pager))
        self.assert_release_without_pager()


if __name__ == "__main__":
    unittest.main()
