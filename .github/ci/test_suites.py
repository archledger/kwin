# SPDX-FileCopyrightText: 2026 Wisbendji Fimerlus <archledger236@gmail.com>
# SPDX-License-Identifier: GPL-2.0-or-later
import json
import os
import subprocess
import tempfile
import unittest
from pathlib import Path


class SuiteWrapperTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.repo = self.root / "repo"
        self.repo.mkdir()
        self.git("init", "-q")
        self.git("config", "user.name", "Fixture")
        self.git("config", "user.email", "fixture@example.invalid")
        self.git("config", "commit.gpgsign", "false")
        (self.repo / "CMakeLists.txt").write_text('set(PROJECT_VERSION "6.8.80")\n')
        (self.repo / ".github/ci").mkdir(parents=True)
        (self.repo / ".github/ci/Containerfile").write_text(
            "Untrusted candidate recipe\n"
        )
        self.git("add", ".")
        self.git("commit", "-qm", "fixture")
        self.bin = self.root / "bin"
        self.bin.mkdir()
        self.capture = self.root / "podman.json"
        (self.bin / "find").write_text("#!/bin/sh\nprintf '/dev/dri/renderDtest\\n'\n")
        (self.bin / "podman").write_text(
            "#!/usr/bin/python3\nimport json,os,sys\n"
            "open(os.environ['CAPTURE'],'w').write(json.dumps(sys.argv[1:]))\n"
            "raise SystemExit(99)\n"
        )
        for path in self.bin.iterdir():
            path.chmod(0o755)
        self.ci = Path(__file__).resolve().parent
        self.work = self.root / "work"

    def git(self, *args):
        return subprocess.check_output(
            ["git", "-C", str(self.repo), *args], text=True, stderr=subprocess.PIPE
        )

    def run_wrapper(self):
        env = dict(
            os.environ,
            PATH=str(self.bin) + os.pathsep + os.environ["PATH"],
            CAPTURE=str(self.capture),
        )
        return subprocess.run(
            ["bash", str(self.ci / "suites.sh"), str(self.repo), str(self.work)],
            capture_output=True,
            text=True,
            env=env,
            check=False,
        )

    def test_image_recipe_comes_from_trusted_helper_directory(self):
        result = self.run_wrapper()
        self.assertNotEqual(
            result.returncode, 0
        )  # The fake builder always stops before building.
        args = json.loads(self.capture.read_text())
        self.assertEqual(args[0], "build")
        self.assertEqual(Path(args[-1]), self.ci)
        self.assertEqual(Path(args[args.index("-f") + 1]), self.ci / "Containerfile")

    def test_existing_worktree_is_not_force_deleted(self):
        tree = self.work / "source"
        self.work.mkdir()
        self.git("worktree", "add", "-q", "--detach", str(tree), "HEAD")
        marker = tree / "unique-untracked-work"
        marker.write_text("keep")
        result = self.run_wrapper()
        self.assertNotEqual(result.returncode, 0)
        self.assertTrue(marker.exists())
        self.assertEqual(marker.read_text(), "keep")
        self.assertFalse(self.capture.exists())


if __name__ == "__main__":
    unittest.main()
