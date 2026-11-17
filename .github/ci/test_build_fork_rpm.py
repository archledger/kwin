# SPDX-FileCopyrightText: 2026 Wisbendji Fimerlus <archledger236@gmail.com>
# SPDX-License-Identifier: GPL-2.0-or-later
"""packaging/fedora/build-fork-rpm.sh: the source package it builds from a tag or a candidate commit.

Needs rpmbuild and rpmspec (the fork's Fedora CI image, localhost/kwin-fork-ci:fedora44); skipped
where they are missing. Nothing is downloaded: the tag's archive and the macros file are placed
where the script looks for them first.
"""

import os
import re
import shutil
import subprocess
import tarfile
import tempfile
import unittest
import unittest.mock
from pathlib import Path

CHECKOUT = Path(__file__).resolve().parents[2]
# A caller's GIT_DIR, GIT_WORK_TREE or GIT_INDEX_FILE would point the fixtures' git elsewhere.
ENVIRONMENT = unittest.mock.patch.dict(os.environ)


def setUpModule():
    ENVIRONMENT.start()
    # Nor the build script's own settings from an operator's shell.
    own = {"KWIN_RELEASE", "KWIN_STOCK_SRPM", "SRPM"}
    for name in [name for name in os.environ if name.startswith("GIT_") or name in own]:
        del os.environ[name]


def tearDownModule():
    ENVIRONMENT.stop()


TOOLS = all(shutil.which(tool) for tool in ("rpmbuild", "rpmspec", "rpm", "git"))


# Skipped where the tools are missing, except in CI (GitHub sets CI): a runner that lost them
# must not pass by running none of these.
@unittest.skipUnless(
    TOOLS or os.environ.get("CI"),
    "needs rpmbuild, rpmspec and git (the fork's Fedora CI image)",
)
class BuildForkRpmTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        # The script and the spec in a tree of their own: the script builds below its own
        # repository by default, and the test must not write into the checkout.
        self.fork = self.root / "fork"
        (self.fork / "packaging/fedora").mkdir(parents=True)
        for name in ("build-fork-rpm.sh", "kwin-fusion.spec"):
            shutil.copy2(
                CHECKOUT / "packaging/fedora" / name,
                self.fork / "packaging/fedora" / name,
            )
        spec = (self.fork / "packaging/fedora/kwin-fusion.spec").read_text()
        self.spec_version = re.search(r"^Version:\s*(\S+)$", spec, re.M).group(1)
        self.spec_release = re.search(
            r"^Release:\s*(\d+)%\{\?dist\}\.pf%\{pf\}$", spec, re.M
        ).group(1)
        self.spec_first_entry = re.search(r"^%changelog\n(\* .*)$", spec, re.M).group(1)
        self.dist = subprocess.check_output(
            ["rpm", "--eval", "%{?dist}"], text=True
        ).strip()
        # A source history: a commit that is still KWin 6.8.2, then 6.8.3 (the candidate).
        self.src = self.root / "src"
        self.src.mkdir()
        self.git("init", "-q", "-b", "main")
        self.git("config", "user.name", "Fixture")
        self.git("config", "user.email", "fixture@example.invalid")
        self.git("config", "commit.gpgsign", "false")
        self.write(
            "CMakeLists.txt",
            'set(PROJECT_VERSION "6.8.2") # Handled by release scripts\n',
        )
        self.write("src/window.cpp", "window\n")
        self.old = self.commit("6.8.2")
        self.write(
            "CMakeLists.txt",
            'set(PROJECT_VERSION "6.8.3") # Handled by release scripts\n',
        )
        self.new = self.commit("6.8.3")
        self.out = self.root / "out"
        (self.out / "SOURCES").mkdir(parents=True)
        (self.out / "SOURCES/macros.kwin.in").write_text(
            "%_kwin_version @PROJECT_VERSION@\n"
        )

    def git(self, *args):
        return subprocess.check_output(
            ["git", "-C", str(self.src), *args], text=True
        ).strip()

    def write(self, name, content):
        path = self.src / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content)

    def commit(self, message):
        self.git("add", "-A")
        self.git("commit", "-qm", message)
        return self.git("rev-parse", "HEAD")

    def build(self, *args, **env):
        environment = dict(os.environ, SRPM="1", **env)
        script = self.fork / "packaging/fedora/build-fork-rpm.sh"
        return subprocess.run(
            [str(script), *args, str(self.out)],
            capture_output=True,
            text=True,
            env=environment,
        )

    def tag_archive(self, tag, commit):
        """The archive GitHub serves for a tag, placed so the script does not download it."""
        version, pf = re.fullmatch(r"v(.+)-pf(\d+)", tag).groups()
        path = self.out / "SOURCES" / f"kwin-{tag}.tar.gz"
        subprocess.check_call(
            [
                "git",
                "-C",
                str(self.src),
                "archive",
                "--format=tar.gz",
                f"--prefix=kwin-{version}-pf{pf}/",
                "-o",
                str(path),
                commit,
            ]
        )

    def srpm(self):
        found = sorted((self.out / "SRPMS").glob("*.src.rpm"))
        self.assertEqual(len(found), 1, found)
        return found[0]

    def query(self, package, form):
        return subprocess.check_output(
            ["rpm", "-qp", "--qf", form, str(package)],
            text=True,
            stderr=subprocess.DEVNULL,
        )

    def first_entry(self):
        spec = (self.out / "SPECS/kwin-fusion.spec").read_text()
        return re.search(r"^%changelog\n(\* .*)\n(.*)$", spec, re.M).groups()

    def test_candidate_commit_builds_the_tags_source_package(self):
        result = self.build("--commit", self.new, "--repo", str(self.src), "v6.8.3-pf1")
        self.assertEqual(result.returncode, 0, result.stderr)
        package = self.srpm()
        self.assertEqual(package.name, f"kwin-6.8.3-1{self.dist}.pf1.src.rpm")
        self.assertEqual(
            self.query(package, "%{VERSION}-%{RELEASE}"), f"6.8.3-1{self.dist}.pf1"
        )
        files = self.query(package, "[%{FILENAMES}\n]").split()
        self.assertEqual(
            sorted(files),
            ["kwin-fusion.spec", "kwin-v6.8.3-pf1.tar.gz", "macros.kwin.in"],
        )
        # The archive is the commit under the tag archive's top directory.
        with tarfile.open(self.out / "SOURCES/kwin-v6.8.3-pf1.tar.gz") as archive:
            names = [member.name for member in archive.getmembers()]
            self.assertTrue(
                all(
                    n == "kwin-6.8.3-pf1" or n.startswith("kwin-6.8.3-pf1/")
                    for n in names
                ),
                names,
            )
            cmake = archive.extractfile("kwin-6.8.3-pf1/CMakeLists.txt").read().decode()
        self.assertIn('"6.8.3"', cmake)
        head, body = self.first_entry()
        self.assertTrue(head.endswith(" - 6.8.3-1.pf1"), head)
        self.assertIn(self.new, body)
        self.assertIn("to be tagged v6.8.3-pf1", body)
        info = dict(
            line.split("=", 1)
            for line in (self.out / "build-info.txt").read_text().splitlines()
        )
        self.assertEqual(info["commit"], self.new)
        self.assertEqual(info["evr"], f"6.8.3-1{self.dist}.pf1")
        # %prep finds the archive's directory.
        prep = subprocess.run(
            [
                "rpmbuild",
                "-bp",
                "--nodeps",
                "--define",
                f"_topdir {self.out}",
                str(self.out / "SPECS/kwin-fusion.spec"),
            ],
            capture_output=True,
            text=True,
        )
        self.assertEqual(prep.returncode, 0, prep.stderr[-2000:])
        self.assertTrue(
            list((self.out / "BUILD").glob("**/kwin-6.8.3-pf1/src/window.cpp"))
        )

    def test_source_package_rebuilds_from_its_own_spec(self):
        # What Copr does with the upload: the source package rebuilt from its spec alone.
        self.assertEqual(
            self.build(
                "--commit", self.new, "--repo", str(self.src), "v6.8.3-pf2"
            ).returncode,
            0,
        )
        package = self.srpm()
        again = self.root / "again"
        subprocess.check_call(
            ["rpm", "-i", "--nodeps", "--define", f"_topdir {again}", str(package)],
            stderr=subprocess.DEVNULL,
        )
        rebuild = subprocess.run(
            [
                "rpmbuild",
                "-bs",
                "--define",
                f"_topdir {again}",
                str(again / "SPECS/kwin-fusion.spec"),
            ],
            capture_output=True,
            text=True,
        )
        self.assertEqual(rebuild.returncode, 0, rebuild.stderr)
        self.assertEqual(
            [p.name for p in (again / "SRPMS").glob("*.src.rpm")], [package.name]
        )

    def test_commit_of_another_version_is_refused(self):
        result = self.build("--commit", self.old, "--repo", str(self.src), "v6.8.3-pf1")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("is KWin 6.8.2, the tag v6.8.3-pf1 is for 6.8.3", result.stderr)
        self.assertEqual(
            (
                list((self.out / "SRPMS").glob("*.rpm"))
                if (self.out / "SRPMS").exists()
                else []
            ),
            [],
        )

    def test_tag_of_the_specs_version_keeps_its_release_and_changelog(self):
        tag = f"v{self.spec_version}-pf9"
        self.tag_archive(tag, self.old)
        result = self.build(tag)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(
            self.query(self.srpm(), "%{VERSION}-%{RELEASE}"),
            f"{self.spec_version}-{self.spec_release}{self.dist}.pf9",
        )
        self.assertEqual(self.first_entry()[0], self.spec_first_entry)

    def test_tag_of_another_version_takes_release_one_or_kwin_release(self):
        self.write("CMakeLists.txt", 'set(PROJECT_VERSION "9.9.9")\n')
        self.tag_archive("v9.9.9-pf2", self.commit("9.9.9"))
        result = self.build("v9.9.9-pf2")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(
            self.query(self.srpm(), "%{VERSION}-%{RELEASE}"), f"9.9.9-1{self.dist}.pf2"
        )
        head, body = self.first_entry()
        self.assertTrue(head.endswith(" - 9.9.9-1.pf2"), head)
        self.assertIn("the fork tag v9.9.9-pf2", body)
        shutil.rmtree(self.out / "SRPMS")
        result = self.build("v9.9.9-pf2", KWIN_RELEASE="4")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(
            self.query(self.srpm(), "%{VERSION}-%{RELEASE}"), f"9.9.9-4{self.dist}.pf2"
        )

    def test_macros_come_from_fedoras_source_package(self):
        # A stand-in for Fedora's kwin source package, built by this machine's rpm (rpm 6 names
        # its archive members ./macros.kwin.in).
        stock = self.root / "stock"
        (stock / "SOURCES").mkdir(parents=True)
        (stock / "SOURCES/macros.kwin.in").write_text("%_kwin_stand_in 1\n")
        (stock / "kwin.spec").write_text(
            "Name: kwin\nVersion: 1\nRelease: 1\nSummary: stand-in\nLicense: MIT\n"
            "Source10: macros.kwin.in\n%description\nstand-in\n%files\n"
        )
        subprocess.check_call(
            [
                "rpmbuild",
                "-bs",
                "--define",
                f"_topdir {stock}",
                str(stock / "kwin.spec"),
            ],
            stdout=subprocess.DEVNULL,
        )
        (source,) = (stock / "SRPMS").glob("*.src.rpm")
        (self.out / "SOURCES/macros.kwin.in").unlink()
        result = self.build(
            "--commit",
            self.new,
            "--repo",
            str(self.src),
            "v6.8.3-pf1",
            KWIN_STOCK_SRPM=str(source),
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(
            (self.out / "SOURCES/macros.kwin.in").read_text(), "%_kwin_stand_in 1\n"
        )

    def test_source_package_without_macros_is_refused(self):
        stock = self.root / "stock"
        (stock / "SOURCES").mkdir(parents=True)
        (stock / "kwin.spec").write_text(
            "Name: kwin\nVersion: 1\nRelease: 1\nSummary: stand-in\nLicense: MIT\n%description\nstand-in\n%files\n"
        )
        subprocess.check_call(
            [
                "rpmbuild",
                "-bs",
                "--define",
                f"_topdir {stock}",
                str(stock / "kwin.spec"),
            ],
            stdout=subprocess.DEVNULL,
        )
        (source,) = (stock / "SRPMS").glob("*.src.rpm")
        (self.out / "SOURCES/macros.kwin.in").unlink()
        result = self.build(
            "--commit",
            self.new,
            "--repo",
            str(self.src),
            "v6.8.3-pf1",
            KWIN_STOCK_SRPM=str(source),
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("has no macros.kwin.in", result.stderr)

    def test_a_kde_re_release_has_four_parts(self):
        # KDE's re-release tags keep the three-part version in CMakeLists.txt (v5.18.4.1).
        self.write("CMakeLists.txt", 'set(PROJECT_VERSION "6.8.3") # re-released\n')
        rerelease = self.commit("6.8.3.1")
        result = self.build(
            "--commit", rerelease, "--repo", str(self.src), "v6.8.3.1-pf1"
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(
            self.query(self.srpm(), "%{VERSION}-%{RELEASE}"),
            f"6.8.3.1-1{self.dist}.pf1",
        )

    def test_a_re_release_tag_on_another_version_is_refused(self):
        result = self.build(
            "--commit", self.old, "--repo", str(self.src), "v6.8.3.1-pf1"
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(
            "is KWin 6.8.2, the tag v6.8.3.1-pf1 is for 6.8.3.1", result.stderr
        )

    def test_a_four_part_version_in_cmake_is_refused(self):
        # KDE's convention, which upstream.py enforces: three parts, re-release or not.
        self.write("CMakeLists.txt", 'set(PROJECT_VERSION "6.8.3.1")\n')
        four = self.commit("6.8.3.1 in CMake")
        result = self.build("--commit", four, "--repo", str(self.src), "v6.8.3.1-pf1")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(
            "is KWin (no version), the tag v6.8.3.1-pf1 is for 6.8.3.1", result.stderr
        )

    def test_a_commit_setting_the_version_twice_is_refused(self):
        # CMake reads command names in any case: SET counts as set.
        for second in ('set(PROJECT_VERSION "6.9.0")', 'SET(PROJECT_VERSION "6.9.0")'):
            with self.subTest(second=second):
                self.write(
                    "CMakeLists.txt", f'set(PROJECT_VERSION "6.8.3")\n{second}\n'
                )
                twice = self.commit("two versions")
                result = self.build(
                    "--commit", twice, "--repo", str(self.src), "v6.8.3-pf1"
                )
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("sets PROJECT_VERSION 2 times", result.stderr)

    def test_a_tag_archive_of_another_version_is_refused(self):
        # A mistagged archive: its name says 6.8.3, its CMakeLists.txt 6.8.2.
        self.tag_archive("v6.8.3-pf7", self.old)
        result = self.build("v6.8.3-pf7")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("is KWin 6.8.2, the tag v6.8.3-pf7 is for 6.8.3", result.stderr)

    def test_a_relative_output_directory_with_another_repository(self):
        # git -C would read a relative output directory as below the other repository.
        here = self.root / "here"
        (here / "rel/SOURCES").mkdir(parents=True)
        shutil.copy(self.out / "SOURCES/macros.kwin.in", here / "rel/SOURCES/")
        result = subprocess.run(
            [
                str(self.fork / "packaging/fedora/build-fork-rpm.sh"),
                "--commit",
                self.new,
            ]
            + ["--repo", str(self.src), "v6.8.3-pf1", "rel"],
            cwd=here,
            env=dict(os.environ, SRPM="1"),
            capture_output=True,
            text=True,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(len(list((here / "rel/SRPMS").glob("*.src.rpm"))), 1)

    def test_names_that_are_not_pf_tags_are_refused(self):
        for tag in [
            "v6.8.3",
            "v6.8.3-pf0",
            "6.8.3-pf1",
            "v6.8-pf1",
            "v6.8.3-pf1x",
            "v6.8.3.1.1-pf1",
        ]:
            with self.subTest(tag=tag):
                result = self.build("--commit", self.new, "--repo", str(self.src), tag)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("is not a pf tag", result.stderr)


if __name__ == "__main__":
    unittest.main()
