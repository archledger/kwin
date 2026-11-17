# SPDX-FileCopyrightText: 2026 Wisbendji Fimerlus <archledger236@gmail.com>
# SPDX-License-Identifier: GPL-2.0-or-later
import json
import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


class SuiteWrapperTest(unittest.TestCase):
    SYSTEMS = ("arch", "neon", "nixos")

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
        for name in ("Containerfile", *(f"Containerfile.{d}" for d in self.SYSTEMS)):
            (self.repo / ".github/ci" / name).write_text("Untrusted candidate recipe\n")
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

    def run_wrapper(self, **variables):
        env = dict(
            os.environ,
            PATH=str(self.bin) + os.pathsep + os.environ["PATH"],
            CAPTURE=str(self.capture),
        )
        for name in ("DISTRO", "MEMCHECK", "KWIN_CI_RENDER"):
            env.pop(name, None)
        env.update(variables)
        return subprocess.run(
            ["bash", str(self.ci / "suites.sh"), str(self.repo), str(self.work)],
            capture_output=True,
            text=True,
            env=env,
            check=False,
        )

    def image_build(self, **variables):
        result = self.run_wrapper(**variables)
        self.assertNotEqual(
            result.returncode, 0
        )  # The fake builder always stops before building.
        args = json.loads(self.capture.read_text())
        self.assertEqual(args[0], "build")
        self.assertEqual(Path(args[-1]), self.ci)
        return args

    def test_image_recipe_comes_from_trusted_helper_directory(self):
        args = self.image_build()
        self.assertEqual(Path(args[args.index("-f") + 1]), self.ci / "Containerfile")
        self.assertEqual(args[args.index("-t") + 1], "localhost/kwin-fork-ci:fedora44")
        self.assertEqual(args[args.index("--build-arg") + 1], "FEDORA_RELEASE=44")

    def test_each_system_builds_its_own_trusted_recipe(self):
        expected = {
            "fedora44": ("Containerfile", ["FEDORA_RELEASE=44"]),
            "fedora45": ("Containerfile", ["FEDORA_RELEASE=45"]),
            **{name: (f"Containerfile.{name}", []) for name in self.SYSTEMS},
        }
        for name, (recipe, build_args) in expected.items():
            with self.subTest(system=name):
                self.capture.unlink(missing_ok=True)
                args = self.image_build(DISTRO=name)
                self.assertEqual(Path(args[args.index("-f") + 1]), self.ci / recipe)
                self.assertEqual(args[args.index("-t") + 1], f"localhost/kwin-fork-ci:{name}")
                self.assertEqual(
                    [args[i + 1] for i, arg in enumerate(args) if arg == "--build-arg"],
                    build_args,
                )
                helper = "fedora" if name.startswith("fedora") else name
                self.assertTrue((self.ci / recipe).is_file())
                self.assertTrue((self.ci / f"env-{helper}.sh").is_file())

    def test_unknown_system_or_option_is_refused_before_any_work(self):
        for variables in (
            {"DISTRO": "debian"},
            {"DISTRO": "../Containerfile"},
            {"DISTRO": "arch", "MEMCHECK": "1"},
            {"DISTRO": "nixos", "MEMCHECK": "1"},
            {"KWIN_CI_RENDER": "gpu"},
        ):
            with self.subTest(**variables):
                result = self.run_wrapper(**variables)
                self.assertEqual(result.returncode, 64)
                self.assertFalse(self.capture.exists())
                self.assertFalse(self.work.exists())

    def test_software_render_device_ignores_the_render_node(self):
        result = self.run_wrapper(KWIN_CI_RENDER="software")
        self.assertNotEqual(result.returncode, 0)
        out = self.work / "out"
        if os.access("/dev/udmabuf", os.W_OK):
            self.assertEqual(
                (out / "render-device.txt").read_text(), "/dev/udmabuf (software)\n"
            )
            self.assertTrue(self.capture.exists())
        else:
            self.assertIn("device=1", (out / "STATUS").read_text())
            self.assertFalse(self.capture.exists())
        self.assertNotIn("renderDtest", result.stderr)

    def install_image_builder(self):
        # A builder that succeeds and an image that names IMAGE_USER; the first build container stops.
        (self.bin / "podman").write_text(
            "#!/usr/bin/python3\nimport json,os,sys\n"
            "args=sys.argv[1:]\n"
            "if args[0]=='build': raise SystemExit(0)\n"
            "if args[:2]==['image','inspect']:\n"
            "    print(os.environ.get('IMAGE_USER','') if 'User' in args[-1] else 'sha256:fixture')\n"
            "    raise SystemExit(0)\n"
            "if 'cat' in args: raise SystemExit(0)\n"
            "open(os.environ['CAPTURE'],'w').write(json.dumps(args))\n"
            "if os.environ.get('PARTIAL_CACHE'):\n"
            "    # CMake can leave a cache behind when configuration fails.\n"
            "    build=[a[:-len(':/build:rw,z')] for a in args if a.endswith(':/build:rw,z')][0]\n"
            "    open(os.path.join(build,'CMakeCache.txt'),'w').write('partial\\n')\n"
            "raise SystemExit(99)\n"
        )

    def test_failed_configure_leaves_no_cache_or_record(self):
        self.install_image_builder()
        result = self.run_wrapper(DISTRO="arch", PARTIAL_CACHE="1")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("kwin_ci_configure", json.loads(self.capture.read_text())[-1])
        self.assertIn("configure=1", (self.work / "out/STATUS").read_text())
        self.assertFalse((self.work / "build/CMakeCache.txt").exists())
        self.assertFalse((self.work / "build/kwin-ci-distro").exists())

    def build_container_args(self, image_user):
        self.install_image_builder()
        result = self.run_wrapper(IMAGE_USER=image_user)
        self.assertNotEqual(result.returncode, 0)
        return json.loads(self.capture.read_text())

    def test_build_directory_is_not_reused_for_another_system(self):
        self.install_image_builder()
        build = self.work / "build"
        for record, system, reused in (
            ("arch", "arch", True),
            (None, "fedora44", True),
            ("fedora44", "arch", False),
            (None, "nixos", False),
        ):
            with self.subTest(record=record, system=system):
                self.capture.unlink(missing_ok=True)
                shutil.rmtree(self.work, ignore_errors=True)
                self.git("worktree", "prune")
                build.mkdir(parents=True)
                (build / "CMakeCache.txt").write_text("fixture\n")
                if record:
                    (build / "kwin-ci-distro").write_text(record + "\n")
                result = self.run_wrapper(DISTRO=system)
                self.assertNotEqual(result.returncode, 0)
                if reused:
                    # Configure is skipped and the first container is the build.
                    self.assertIn("cmake --build", json.loads(self.capture.read_text())[-1])
                else:
                    self.assertFalse(self.capture.exists())
                    self.assertIn("configure=1", (self.work / "out/STATUS").read_text())
                    self.assertIn(f"configured for {record or 'fedora44'}, not {system}", result.stderr)

    def test_containers_run_as_the_invoking_user(self):
        for image_user in ("", "0:0", "neon"):
            with self.subTest(image_user=image_user):
                self.capture.unlink(missing_ok=True)
                args = self.build_container_args(image_user)
                self.assertEqual(args[0], "run")
                self.assertIn("--userns=keep-id", args)
                if image_user:
                    self.assertEqual(
                        args[args.index("--user") + 1], f"{os.getuid()}:{os.getgid()}"
                    )
                else:
                    self.assertNotIn("--user", args)

    def test_render_node_is_used_by_default(self):
        self.image_build()
        self.assertEqual(
            (self.work / "out/render-device.txt").read_text(), "/dev/dri/renderDtest\n"
        )

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
