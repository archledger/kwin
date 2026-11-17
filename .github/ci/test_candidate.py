# SPDX-FileCopyrightText: 2026 Wisbendji Fimerlus <archledger236@gmail.com>
# SPDX-License-Identifier: GPL-2.0-or-later
import contextlib
import io
import json
import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

import candidate

# A caller's GIT_DIR, GIT_WORK_TREE or GIT_INDEX_FILE (a hook, a wrapper) would point the
# fixtures' git at its own repository: none of git's variables reach these tests.
ENVIRONMENT = patch.dict(os.environ)


def setUpModule():
    ENVIRONMENT.start()
    for name in [name for name in os.environ if name.startswith("GIT_")]:
        del os.environ[name]


def tearDownModule():
    ENVIRONMENT.stop()


SIGNOFF = "Signed-off-by: Fixture <fixture@example.invalid>"


class Fixture(unittest.TestCase):
    """An upstream line with two tags and a fork candidate on the first one."""

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.repo = self.root / "repo"
        self.repo.mkdir()
        self.git("init", "-q", "-b", "Plasma/6.8")
        self.git("config", "user.name", "Fixture")
        self.git("config", "user.email", "fixture@example.invalid")
        self.git("config", "commit.gpgsign", "false")
        self.write(
            "CMakeLists.txt", 'set(PROJECT_VERSION "6.8.2")\nadd_subdirectory(src)\n'
        )
        self.write("src/window.cpp", "window\n")
        self.write("po/de/kwin.po", "de 1\n")
        self.write("po/fr/kwin.po", "fr 1\n")
        self.commit("upstream: 6.8.2")
        self.git("tag", "v6.8.2")
        # Upstream's point release: the version line, a translation and a fix.
        self.write("po/de/kwin.po", "de 2\n")
        self.commit("GIT_SILENT Sync po/docbooks with svn")
        self.write("src/output.cpp", "output fix\n")
        self.fix = self.commit("drm: a fix")
        self.write(
            "CMakeLists.txt", 'set(PROJECT_VERSION "6.8.3")\nadd_subdirectory(src)\n'
        )
        self.commit("Update version for new release 6.8.3")
        self.git("tag", "v6.8.3")
        # The previous candidate: the fork's delta on v6.8.2, with upstream's fix imported early.
        self.git("checkout", "-q", "-b", "candidate/6.8.2-pf5", "v6.8.2")
        self.write("src/window.cpp", "window\nfork tiling\n")
        self.commit("tiling: the fork's change\n\n" + SIGNOFF)
        self.write(
            "CMakeLists.txt",
            'set(PROJECT_VERSION "6.8.2")\nadd_subdirectory(src)\nadd_subdirectory(fork)\n',
        )
        self.write("fork/placement.cpp", "placement\n")
        self.commit("placement: a fork module\n\n" + SIGNOFF)
        self.write("src/output.cpp", "output fix\n")
        self.commit("drm: a fix (stable import)\n\n" + SIGNOFF)
        self.git("checkout", "-q", "--detach", "v6.8.3")

    def stand_in_gpg(self):
        """A gpg on PATH that knows the fixture's identity and cannot sign."""
        tools = self.root / "bin"
        tools.mkdir(exist_ok=True)
        (tools / "gpg").write_text(
            "#!/bin/sh\n"
            'case "$*" in *--list-keys*) echo "uid:u::::::::Fixture <fixture@example.invalid>:"; exit 0 ;; esac\n'
            "echo 'gpg: signing failed' >&2\nexit 2\n"
        )
        (tools / "gpg").chmod(0o755)
        return patch.dict(
            os.environ, {"PATH": f"{tools}{os.pathsep}{os.environ.get('PATH', '')}"}
        )

    def git(self, *args, repo=None):
        return subprocess.check_output(
            ["git", "-C", str(repo or self.repo), *args],
            text=True,
            stderr=subprocess.PIPE,
        ).strip()

    def write(self, name, content):
        path = self.repo / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content)

    def commit(self, message):
        self.git("add", "-A")
        self.git("commit", "-qm", message)
        return self.git("rev-parse", "HEAD")

    def replay(self, **kwargs):
        args = dict(
            previous="candidate/6.8.2-pf5", base="v6.8.2", tag="v6.8.3", pf=1, sign=None
        )
        args.update(kwargs)
        return candidate.replay(self.repo, **args)


class TagTest(unittest.TestCase):
    def test_point_release_tags_only(self):
        self.assertEqual(candidate.parse_tag("v6.8.3"), (6, 8, 3))
        for tag in ["6.8.3", "v6.8", "v6.8.3-pf1", "v6.8.3.1.1", "v6.8.90", "v6.8.3\n"]:
            with self.subTest(tag=tag), self.assertRaises(candidate.CandidateError):
                candidate.parse_tag(tag)

    def test_candidate_branch_names_the_tag_and_sequence(self):
        self.assertEqual(candidate.candidate_branch("v6.8.3", 1), "candidate/6.8.3-pf1")
        for pf in [0, -1, "1"]:
            with self.subTest(pf=pf), self.assertRaises(candidate.CandidateError):
                candidate.candidate_branch("v6.8.3", pf)

    def test_kde_re_releases_have_four_parts(self):
        self.assertEqual(candidate.parse_tag("v6.3.2.1"), (6, 3, 2, 1))
        self.assertEqual(
            candidate.candidate_branch("v6.3.2.1", 1), "candidate/6.3.2.1-pf1"
        )
        self.assertEqual(candidate.base_of("candidate/6.3.2.1-pf2"), "v6.3.2.1")
        self.assertTrue(candidate.BRANCH.fullmatch("candidate/6.3.2.1-pf2"))
        candidate.check_step("v6.3.2", "v6.3.2.1")
        candidate.check_step("v6.3.2.1", "v6.3.3")
        with self.assertRaises(candidate.CandidateError):
            candidate.check_step("v6.3.2.1", "v6.3.2")

    def test_the_replay_command_wants_a_full_fingerprint(self):
        # A long key id signs, but a signature names the full fingerprint.
        for key in ["", "0123456789ABCDEF"]:
            with self.subTest(key=key):
                errors = io.StringIO()
                with contextlib.redirect_stderr(errors):
                    status = candidate.main(
                        ["replay", "--repo", ".", "--previous", "candidate/6.8.2-pf5"]
                        + ["--tag", "v6.8.3", "--pf", "1", "--sign", key]
                    )
                self.assertEqual(status, 2)
                self.assertIn("full fingerprint", errors.getvalue())

    def test_a_new_series_is_not_a_replay(self):
        with self.assertRaises(candidate.CandidateError):
            candidate.check_step("v6.8.6", "v6.9.0")
        with self.assertRaises(candidate.CandidateError):
            candidate.check_step("v6.8.3", "v6.8.3")
        with self.assertRaises(candidate.CandidateError):
            candidate.check_step("v6.8.3", "v6.8.2")
        candidate.check_step("v6.8.2", "v6.8.4")


class ReplayTest(Fixture):
    def test_replay_carries_the_delta_onto_the_tag(self):
        result = self.replay()
        self.assertEqual(result.status, "clean", result.diagnostics)
        self.assertEqual(result.branch, "candidate/6.8.3-pf1")
        self.assertEqual(
            self.git("rev-parse", result.tag_commit),
            self.git("rev-parse", "v6.8.3^{commit}"),
        )
        # The new candidate is the tag plus the fork's commits, in order, without merges.
        subjects = self.git(
            "log", "--format=%s", "--reverse", f"v6.8.3..{result.commit}"
        ).splitlines()
        self.assertEqual(
            subjects, ["tiling: the fork's change", "placement: a fork module"]
        )
        self.assertEqual(result.replayed, 2)
        self.assertEqual(
            [c["subject"] for c in result.dropped], ["drm: a fix (stable import)"]
        )
        # Its tree is the old candidate with upstream's later changes: the version and translation.
        self.assertEqual(
            self.git("show", f"{result.commit}:CMakeLists.txt"),
            'set(PROJECT_VERSION "6.8.3")\nadd_subdirectory(src)\nadd_subdirectory(fork)',
        )
        self.assertEqual(self.git("show", f"{result.commit}:po/de/kwin.po"), "de 2")
        self.assertEqual(
            self.git("show", f"{result.commit}:src/window.cpp"), "window\nfork tiling"
        )
        self.assertEqual(result.changed, ["CMakeLists.txt", "po/de/kwin.po"])
        self.assertEqual(result.merged, ["CMakeLists.txt"])
        self.assertEqual(
            self.git("rev-parse", "HEAD"), self.git("rev-parse", "v6.8.3^{commit}")
        )
        self.assertEqual(self.git("status", "--porcelain"), "")
        self.assertEqual(
            self.git("worktree", "list", "--porcelain").count("worktree "), 1
        )
        self.assertNotIn("candidate/6.8.3-pf1", self.git("branch", "--list"))

    def test_result_records_every_input_as_a_commit(self):
        result = self.replay()
        record = json.loads(json.dumps(result.to_json()))
        for key in [
            "previous",
            "base_commit",
            "tag_commit",
            "commit",
            "merged_tree",
            "tree",
        ]:
            with self.subTest(key=key):
                self.assertRegex(record[key], r"^[0-9a-f]{40}$")
        self.assertEqual(record["tree"], record["merged_tree"])

    def test_conflict_stops_without_a_candidate(self):
        self.git("checkout", "-q", "candidate/6.8.2-pf5")
        self.write("po/de/kwin.po", "de fork\n")
        self.commit("po: a conflicting change\n\n" + SIGNOFF)
        self.git("checkout", "-q", "--detach", "v6.8.3")
        result = self.replay()
        self.assertEqual(result.status, "conflict")
        self.assertIsNone(result.commit)
        self.assertIn("po/de/kwin.po", result.diagnostics)
        self.assertEqual(self.git("status", "--porcelain"), "")
        self.assertEqual(
            self.git("worktree", "list", "--porcelain").count("worktree "), 1
        )

    def test_tree_different_from_a_clean_three_way_merge_is_a_mismatch(self):
        original = candidate.merge_tree

        def wrong(repo, base, previous, tag):
            return self.git("rev-parse", "v6.8.2^{tree}"), []

        candidate.merge_tree = wrong
        self.addCleanup(setattr, candidate, "merge_tree", original)
        result = self.replay()
        self.assertEqual(result.status, "mismatch")
        self.assertEqual(result.three_way, "differs")
        self.assertIn("CMakeLists.txt", result.merge_differences)
        self.assertIn("CMakeLists.txt", result.diagnostics)

    def test_clean_merge_is_compared_path_by_path(self):
        result = self.replay()
        self.assertEqual(result.three_way, "equal")
        self.assertEqual(result.conflicted, [])
        self.assertEqual(result.merge_differences, [])
        self.assertEqual(result.merged_tree, result.tree)
        self.assertEqual(result.rewritten, [])

    def test_unexplained_lines_make_the_replay_a_mismatch(self):
        original = candidate.unexplained
        candidate.unexplained = lambda repo, base, previous, tag, new, paths=None: [
            "src/window.cpp: 1 added line(s) from nowhere"
        ]
        self.addCleanup(setattr, candidate, "unexplained", original)
        result = self.replay()
        self.assertEqual(result.status, "mismatch")
        self.assertIn("src/window.cpp", result.diagnostics)

    def amended(self, result, path, content):
        """The replay's last commit with one more change in it: a stand-in for a faulty replay."""
        self.git("checkout", "-q", "--detach", result.commit)
        if content is None:
            self.git("rm", "-q", path)
        else:
            self.write(path, content)
        self.git("add", "-A")
        self.git("commit", "-q", "--amend", "--no-edit")
        commit = self.git("rev-parse", "HEAD")
        self.git("checkout", "-q", "--detach", "v6.8.3")
        return commit

    def unexplained(self, result, commit):
        return candidate.unexplained(
            self.repo, result.base_commit, result.previous, result.tag_commit, commit
        )

    def test_clean_replay_is_explained_by_the_tags_and_the_delta(self):
        result = self.replay()
        self.assertEqual(self.unexplained(result, result.commit), [])

    def test_a_lost_fork_line_is_unexplained(self):
        result = self.replay()
        problems = self.unexplained(
            result, self.amended(result, "src/window.cpp", "window\n")
        )
        self.assertEqual(
            [p.split(":")[0] for p in problems], ["src/window.cpp", "src/window.cpp"]
        )
        self.assertIn("-fork tiling", problems[0])
        self.assertIn("not upstream's change", problems[0])
        self.assertIn("beyond the original commit", problems[1])

    def test_an_upstream_change_left_out_is_unexplained(self):
        result = self.replay()
        problems = self.unexplained(
            result, self.amended(result, "po/de/kwin.po", "de 1\n")
        )
        self.assertEqual(len(problems), 1, problems)
        self.assertIn("po/de/kwin.po", problems[0])
        self.assertIn("beyond the original commit", problems[0])
        self.assertIn("-de 2", problems[0])

    def test_a_change_from_nowhere_is_unexplained(self):
        result = self.replay()
        problems = self.unexplained(
            result, self.amended(result, "fork/extra.cpp", "extra\n")
        )
        self.assertEqual(
            [p.split(":")[0] for p in problems], ["fork/extra.cpp", "fork/extra.cpp"]
        )
        problems = self.unexplained(
            result, self.amended(result, "fork/placement.cpp", None)
        )
        self.assertEqual([p.split(":")[0] for p in problems], ["fork/placement.cpp"])
        self.assertIn("presence 100644 -> None", problems[0])

    def test_a_line_from_nowhere_in_a_fork_file_is_unexplained(self):
        result = self.replay()
        problems = self.unexplained(
            result,
            self.amended(result, "src/window.cpp", "window\nfork tiling\ninjected\n"),
        )
        self.assertEqual(
            [p.split(":")[0] for p in problems], ["src/window.cpp", "src/window.cpp"]
        )
        self.assertTrue(all("+injected" in p for p in problems), problems)

    def test_a_commit_the_previous_candidate_lacks_is_unexplained(self):
        result = self.replay()
        self.git("checkout", "-q", "--detach", result.commit)
        self.write("fork/extra.cpp", "extra\n")
        extra = self.commit("extra: not in the delta\n\n" + SIGNOFF)
        self.git("checkout", "-q", "--detach", "v6.8.3")
        problems = self.unexplained(result, extra)
        self.assertIn(
            "extra: not in the delta: a replayed commit that is not in the previous candidate",
            problems,
        )

    def test_a_delta_wholly_in_the_tag_is_empty(self):
        self.git("checkout", "-q", "-b", "candidate/6.8.2-pf9", "v6.8.2")
        self.write("src/output.cpp", "output fix\n")
        self.commit("drm: a fix (stable import)\n\n" + SIGNOFF)
        self.git("checkout", "-q", "--detach", "v6.8.3")
        result = self.replay(previous="candidate/6.8.2-pf9")
        self.assertEqual(result.status, "empty")
        self.assertEqual(result.replayed, 0)

    def test_tags_moved_after_their_check_are_refused(self):
        checked = (
            self.git("rev-parse", "v6.8.2^{commit}"),
            self.git("rev-parse", "v6.8.3^{commit}~1"),
        )
        with self.assertRaises(candidate.CandidateError) as raised:
            self.replay(verified=checked)
        self.assertIn("moved after they were checked", str(raised.exception))

    def test_previous_candidate_must_be_built_on_the_base_tag(self):
        self.git("checkout", "-q", "-b", "elsewhere", "v6.8.3")
        self.write("fork/other.cpp", "other\n")
        self.commit("other\n\n" + SIGNOFF)
        self.git("checkout", "-q", "--detach", "v6.8.3")
        with self.assertRaises(candidate.CandidateError):
            self.replay(previous="elsewhere")

    def test_merge_commits_in_the_delta_are_refused(self):
        self.git("checkout", "-q", "-b", "side", "candidate/6.8.2-pf5~1")
        self.write("fork/side.cpp", "side\n")
        self.commit("side\n\n" + SIGNOFF)
        self.git("checkout", "-q", "candidate/6.8.2-pf5")
        self.git("merge", "-q", "--no-ff", "-m", "merge side\n\n" + SIGNOFF, "side")
        self.git("checkout", "-q", "--detach", "v6.8.3")
        with self.assertRaises(candidate.CandidateError):
            self.replay()

    def test_unsigned_replay_fails_the_commit_rules(self):
        result = self.replay()
        problems = candidate.commit_problems(
            self.repo, result.tag_commit, result.commit, sign="0123456789ABCDEF"
        )
        self.assertEqual(len(problems), 2)
        self.assertTrue(
            all("no good signature by 0123456789ABCDEF" in p for p in problems)
        )
        self.assertEqual(
            candidate.commit_problems(self.repo, result.tag_commit, result.commit), []
        )

    def test_the_committer_must_be_the_clones_own(self):
        result = self.replay()
        problems = candidate.commit_problems(
            self.repo,
            result.tag_commit,
            result.commit,
            sign="0123456789ABCDEF",
            email="someone@example.invalid",
        )
        self.assertEqual(len(problems), 2)
        self.assertTrue(
            all(
                "committer fixture@example.invalid, not someone@example.invalid" in p
                for p in problems
            ),
            problems,
        )

    def test_a_replay_that_cannot_sign_is_an_error_not_a_conflict(self):
        with self.stand_in_gpg():
            result = self.replay(sign="0123456789ABCDEF0123456789ABCDEF01234567")
        self.assertEqual(result.status, "error", result.diagnostics)
        self.assertIsNone(result.commit)
        self.assertEqual(self.git("status", "--porcelain"), "")

    def test_a_commit_partly_in_the_tag_is_listed_as_rewritten(self):
        # The stable import also brings a fork file: on the tag it keeps only that file.
        self.git("checkout", "-q", "candidate/6.8.2-pf5")
        self.write("fork/extra.cpp", "extra\n")
        self.git("add", "-A")
        self.git("commit", "-q", "--amend", "--no-edit")
        self.git("checkout", "-q", "--detach", "v6.8.3")
        result = self.replay()
        self.assertEqual(result.status, "clean", result.diagnostics)
        self.assertEqual(result.dropped, [])
        self.assertEqual(result.rewritten, ["drm: a fix (stable import)"])

    @unittest.skipUnless(shutil.which("gpg") and shutil.which("gpgconf"), "needs gpg")
    def test_a_signed_replay_passes_only_with_a_good_signature_by_the_key(self):
        home = self.root / "gnupg"
        home.mkdir(mode=0o700)
        self.addCleanup(
            subprocess.run,
            ["gpgconf", "--homedir", str(home), "--kill", "gpg-agent"],
            capture_output=True,
        )
        gpg = ["gpg", "--homedir", str(home), "--batch", "--pinentry-mode", "loopback"]
        subprocess.run(
            gpg
            + ["--passphrase", "", "--quick-gen-key"]
            + ["Fixture <fixture@example.invalid>", "ed25519", "sign", "never"],
            check=True,
            capture_output=True,
        )
        listing = subprocess.check_output(
            gpg + ["--list-secret-keys", "--with-colons"], text=True
        )
        key = next(
            l.split(":")[9] for l in listing.splitlines() if l.startswith("fpr:")
        )
        with patch.dict(os.environ, {"GNUPGHOME": str(home)}):
            result = self.replay(sign=key)
            self.assertEqual(result.status, "clean", result.diagnostics)
            self.assertEqual(result.problems, [])
            email = "fixture@example.invalid"
            other = candidate.commit_problems(
                self.repo,
                result.tag_commit,
                result.commit,
                sign="0123456789ABCDEF",
                email=email,
            )
            self.assertEqual(len(other), 2, other)
            # A good signature whose key is not trusted (status U) is not enough.
            subprocess.run(
                gpg + ["--import-ownertrust"],
                input=f"{key}:2:\n",
                text=True,
                check=True,
                capture_output=True,
            )
            untrusted = candidate.commit_problems(
                self.repo, result.tag_commit, result.commit, sign=key, email=email
            )
            self.assertEqual(len(untrusted), 2, untrusted)
            self.assertTrue(all("status U" in p for p in untrusted), untrusted)
            # A clone whose committer address is not the key's.
            self.git("config", "user.email", "other@example.invalid")
            with self.assertRaises(candidate.CandidateError) as raised:
                self.replay(sign=key, pf=2)
            self.assertIn(
                "has no user ID with other@example.invalid", str(raised.exception)
            )

    def test_commits_in_another_order_are_unexplained(self):
        result = self.replay()
        one, two = self.git(
            "rev-list", "--reverse", f"{result.tag_commit}..{result.commit}"
        ).split()
        self.git("checkout", "-q", "--detach", result.tag_commit)
        self.git("cherry-pick", two)
        self.git("cherry-pick", one)
        swapped = self.git("rev-parse", "HEAD")
        self.git("checkout", "-q", "--detach", "v6.8.3")
        problems = self.unexplained(result, swapped)
        self.assertIn(
            "tiling: the fork's change: a replayed commit that is not in the previous candidate",
            problems,
        )


class SameKeyTest(Fixture):
    def test_commits_with_the_same_author_date_and_message_both_replay(self):
        self.git("checkout", "-q", "candidate/6.8.2-pf5")
        same = dict(os.environ, GIT_AUTHOR_DATE="1700000000 +0000")
        for content in ("one\n", "two\n"):
            self.write("fork/same.cpp", content)
            self.git("add", "-A")
            subprocess.check_call(
                [
                    "git",
                    "-C",
                    str(self.repo),
                    "commit",
                    "-qm",
                    "fork: the same\n\n" + SIGNOFF,
                ],
                env=same,
            )
        self.git("checkout", "-q", "--detach", "v6.8.3")
        result = self.replay()
        self.assertEqual(result.status, "clean", result.diagnostics)
        self.assertEqual(result.replayed, 4)
        self.assertEqual(
            [c["subject"] for c in result.dropped], ["drm: a fix (stable import)"]
        )
        self.assertEqual(result.rewritten, [])


class SameKeyDropTest(Fixture):
    def test_the_second_of_two_commits_with_one_key_survives(self):
        # The first is upstream's 6.8.3 translation (in the tag), the second the fork's own.
        self.git("checkout", "-q", "candidate/6.8.2-pf5")
        same = dict(os.environ, GIT_AUTHOR_DATE="1700000000 +0000")
        for name, content in (
            ("po/de/kwin.po", "de 2\n"),
            ("fork/second.cpp", "second\n"),
        ):
            self.write(name, content)
            self.git("add", "-A")
            subprocess.check_call(
                [
                    "git",
                    "-C",
                    str(self.repo),
                    "commit",
                    "-qm",
                    "fork: the same\n\n" + SIGNOFF,
                ],
                env=same,
            )
        self.git("checkout", "-q", "--detach", "v6.8.3")
        result = self.replay()
        self.assertEqual(result.status, "clean", result.diagnostics)
        self.assertEqual(
            [c["subject"] for c in result.dropped],
            ["drm: a fix (stable import)", "fork: the same"],
        )
        self.assertEqual(
            self.git("show", "--format=", "--name-only", result.dropped[1]["commit"]),
            "po/de/kwin.po",
        )
        self.assertEqual(result.rewritten, [])


class CarriageReturnTest(Fixture):
    def setUp(self):
        super().setUp()
        self.git("checkout", "-q", "--orphan", "crlf")
        self.git("rm", "-rfq", ".")
        (self.repo / "dos.txt").write_bytes(b"one\r\ntwo\r\n")
        self.write("README", "readme\n")
        self.commit("upstream: 7.4.1")
        self.git("tag", "v7.4.1")
        self.write("README", "readme, newer\n")
        self.commit("upstream: 7.4.2")
        self.git("tag", "v7.4.2")
        self.git("checkout", "-q", "-b", "candidate/7.4.1-pf1", "v7.4.1")
        (self.repo / "dos.txt").write_bytes(b"one\r\ntwo\r\nthree\r\n")
        self.commit("fork: a third line\n\n" + SIGNOFF)
        self.git("checkout", "-q", "--detach", "v7.4.2")

    def test_a_line_that_lost_its_carriage_return_is_unexplained(self):
        result = self.replay(
            previous="candidate/7.4.1-pf1", base="v7.4.1", tag="v7.4.2"
        )
        self.assertEqual(result.status, "clean", result.diagnostics)
        self.git("checkout", "-q", "--detach", result.commit)
        (self.repo / "dos.txt").write_bytes(b"one\r\ntwo\r\nthree\n")
        self.git("commit", "-q", "-a", "--amend", "--no-edit")
        lost = self.git("rev-parse", "HEAD")
        self.git("checkout", "-q", "--detach", "v7.4.2")
        problems = candidate.unexplained(
            self.repo, result.base_commit, result.previous, result.tag_commit, lost
        )
        self.assertEqual([p.split(":")[0] for p in problems], ["dos.txt", "dos.txt"])


class BinaryTest(Fixture):
    def setUp(self):
        super().setUp()
        self.git("checkout", "-q", "--orphan", "other")
        self.git("rm", "-rfq", ".")
        (self.repo / "icon.png").write_bytes(b"\x89PNG\0one")
        self.write("README", "readme\n")
        self.commit("upstream: 7.0.1")
        self.git("tag", "v7.0.1")
        (self.repo / "icon.png").write_bytes(b"\x89PNG\0two")
        self.commit("upstream: 7.0.2")
        self.git("tag", "v7.0.2")
        self.git("checkout", "-q", "-b", "candidate/7.0.1-pf1", "v7.0.1")
        self.write("README", "readme\nfork\n")
        self.commit("fork: readme\n\n" + SIGNOFF)
        self.git("checkout", "-q", "--detach", "v7.0.2")

    def test_upstream_binary_change_is_explained(self):
        result = self.replay(
            previous="candidate/7.0.1-pf1", base="v7.0.1", tag="v7.0.2"
        )
        self.assertEqual(result.status, "clean", result.diagnostics)
        self.assertEqual(result.changed, ["icon.png"])

    def test_binary_changed_on_both_sides_is_not_replayed(self):
        self.git("checkout", "-q", "candidate/7.0.1-pf1")
        (self.repo / "icon.png").write_bytes(b"\x89PNG\0fork")
        self.commit("fork: icon\n\n" + SIGNOFF)
        self.git("checkout", "-q", "--detach", "v7.0.2")
        result = self.replay(
            previous="candidate/7.0.1-pf1", base="v7.0.1", tag="v7.0.2"
        )
        self.assertEqual(result.status, "conflict")
        self.assertIn("icon.png", result.diagnostics)

    def test_a_binary_changed_by_hand_is_unexplained(self):
        result = self.replay(
            previous="candidate/7.0.1-pf1", base="v7.0.1", tag="v7.0.2"
        )
        self.git("checkout", "-q", "--detach", result.commit)
        (self.repo / "icon.png").write_bytes(b"\x89PNG\0other")
        self.git("add", "-A")
        self.git("commit", "-q", "--amend", "--no-edit")
        tampered = self.git("rev-parse", "HEAD")
        problems = candidate.unexplained(
            self.repo, result.base_commit, result.previous, result.tag_commit, tampered
        )
        self.assertEqual([p.split(":")[0] for p in problems], ["icon.png", "icon.png"])
        self.assertIn("binary", problems[0])


class EarlyImportTest(Fixture):
    """The delta imported an upstream change before its release and then changed it (KWin
    6.8.2-pf5: the hang detector, imported from 6.8.3 and made to stop at shutdown)."""

    def setUp(self):
        super().setUp()
        self.git("checkout", "-q", "--orphan", "early")
        self.git("rm", "-rfq", ".")
        self.write("main.cpp", "int main()\n{\n    run();\n}\n")
        self.write("po/kwin.po", "one\n")
        self.commit("upstream: 7.1.1")
        self.git("tag", "v7.1.1")
        detector = "int main()\n{\n    run();\n    while (wait()) {\n        return ping;\n    }\n}\n"
        self.write("main.cpp", detector)
        self.commit("main: a hang detector")
        self.write("po/kwin.po", "two\n")
        self.commit("upstream: 7.1.2")
        self.git("tag", "v7.1.2")
        self.git("checkout", "-q", "-b", "candidate/7.1.1-pf1", "v7.1.1")
        self.write("main.cpp", detector)
        self.commit("main: a hang detector (stable import)\n\n" + SIGNOFF)
        self.write(
            "main.cpp", detector.replace("return ping;", "return ping || stopping;")
        )
        self.commit("main: stop the hang detector at shutdown\n\n" + SIGNOFF)
        self.git("checkout", "-q", "--detach", "v7.1.2")

    def test_the_fork_change_of_imported_code_replays(self):
        result = self.replay(
            previous="candidate/7.1.1-pf1", base="v7.1.1", tag="v7.1.2"
        )
        self.assertEqual(result.status, "clean", result.diagnostics)
        self.assertEqual(result.three_way, "partial")
        self.assertEqual(result.conflicted, ["main.cpp"])
        self.assertEqual(result.changed, ["po/kwin.po"])
        self.assertEqual(
            [c["subject"] for c in result.dropped],
            ["main: a hang detector (stable import)"],
        )
        self.assertIn(
            "return ping || stopping;", self.git("show", f"{result.commit}:main.cpp")
        )

    def test_the_fork_change_lost_in_the_replay_is_unexplained(self):
        result = self.replay(
            previous="candidate/7.1.1-pf1", base="v7.1.1", tag="v7.1.2"
        )
        # A replay that dropped the fork's own commit too: the new tag alone.
        problems = candidate.unexplained(
            self.repo,
            result.base_commit,
            result.previous,
            result.tag_commit,
            result.tag_commit,
        )
        self.assertEqual([p.split(":")[0] for p in problems], ["main.cpp"], problems)
        self.assertIn("return ping || stopping;", problems[0])
        self.assertIn("not upstream's change", problems[0])


class PositionTest(Fixture):
    """Two changes to one file that git merges cleanly: KDE's in a(), the fork's in b()."""

    def setUp(self):
        super().setUp()
        self.git("checkout", "-q", "--orphan", "position")
        self.git("rm", "-rfq", ".")
        self.source = "a()\n{\n    return false;\n}\n\nb()\n{\n    return true;\n}\n"
        self.write("f.cpp", self.source)
        self.commit("upstream: 8.0.1")
        self.git("tag", "v8.0.1")
        self.write("f.cpp", self.source.replace("return false;", "return true;"))
        self.commit("upstream: 8.0.2")
        self.git("tag", "v8.0.2")
        self.git("checkout", "-q", "-b", "candidate/8.0.1-pf1", "v8.0.1")
        self.write("f.cpp", self.source.replace("return true;", "return false;"))
        self.commit("fork: b() is false\n\n" + SIGNOFF)
        self.git("checkout", "-q", "--detach", "v8.0.2")

    def test_both_changes_lost_in_one_file_are_found_by_the_clean_merge(self):
        result = self.replay(
            previous="candidate/8.0.1-pf1", base="v8.0.1", tag="v8.0.2"
        )
        self.assertEqual(result.status, "clean", result.diagnostics)
        self.assertEqual(result.three_way, "equal")
        self.assertEqual(
            self.git("show", f"{result.commit}:f.cpp"),
            "a()\n{\n    return true;\n}\n\nb()\n{\n    return false;\n}",
        )
        # A replay that lost both changes: f.cpp as it was at the base.
        self.git("checkout", "-q", "--detach", result.commit)
        self.write("f.cpp", self.source)
        self.git("commit", "-q", "-a", "--amend", "--no-edit")
        lost = self.git("rev-parse", "HEAD")
        self.git("checkout", "-q", "--detach", "v8.0.2")
        # The lists of lines cannot see it: the same lines, in other places.
        self.assertEqual(
            candidate.unexplained(
                self.repo, result.base_commit, result.previous, result.tag_commit, lost
            ),
            [],
        )
        # The clean merge can.
        self.assertEqual(
            candidate.merge_differences(
                self.repo, result.merged_tree, result.conflicted, lost
            ),
            ["f.cpp"],
        )


class LaterTest(Fixture):
    """KDE's branch goes on after the tag; the delta may have imported a later fix early."""

    def setUp(self):
        super().setUp()
        self.git("checkout", "-q", "Plasma/6.8")
        self.write("src/later.cpp", "a later fix\n")
        self.commit("drm: a later fix")
        self.write("po/fr/kwin.po", "fr 2\n")
        self.commit("GIT_SILENT Sync po/docbooks with svn")
        self.later = self.git("rev-parse", "HEAD")
        self.git("checkout", "-q", "--detach", "v6.8.3")

    def test_later_fixes_not_in_the_delta_pass(self):
        result = self.replay(later=self.later)
        self.assertEqual(result.status, "clean", result.diagnostics)
        self.assertEqual(result.early, [])
        self.assertEqual(result.undetermined, [])

    def test_a_later_commit_and_its_revert_are_not_carried(self):
        # In a file the candidate has as the tag has it: the revert's patch reverses on both.
        self.git("checkout", "-q", "Plasma/6.8")
        self.write("src/output.cpp", "output fix\nan experiment\n")
        self.commit("output: an experiment")
        self.write("src/output.cpp", "output fix\n")
        self.commit('Revert "output: an experiment"')
        later = self.git("rev-parse", "HEAD")
        self.git("checkout", "-q", "--detach", "v6.8.3")
        result = self.replay(later=later)
        self.assertEqual(result.status, "clean", result.diagnostics)
        self.assertEqual(result.early, [])

    def test_a_later_commit_on_another_later_one_says_nothing(self):
        self.git("checkout", "-q", "Plasma/6.8")
        self.write("po/fr/kwin.po", "fr 3\n")
        self.commit("GIT_SILENT Sync po/docbooks with svn")
        later = self.git("rev-parse", "HEAD")
        self.git("checkout", "-q", "--detach", "v6.8.3")
        result = self.replay(later=later)
        self.assertEqual(result.status, "clean", result.diagnostics)
        self.assertEqual((result.early, result.undetermined), ([], []))

    def test_a_later_change_the_candidate_has_otherwise_is_undetermined(self):
        self.git("checkout", "-q", "Plasma/6.8")
        self.write("src/window.cpp", "window, later\n")
        self.commit("window: a later change")
        later = self.git("rev-parse", "HEAD")
        self.git("checkout", "-q", "--detach", "v6.8.3")
        result = self.replay(later=later)
        self.assertEqual(result.status, "clean", result.diagnostics)
        self.assertEqual(
            [e.split(" ", 1)[1] for e in result.undetermined],
            ["window: a later change"],
        )

    def test_a_later_fix_kde_merged_is_checked_as_the_merge_too(self):
        self.git("checkout", "-q", "-b", "kde-topic", "v6.8.3")
        self.write("src/merged.cpp", "a merged fix\n")
        self.commit("drm: a fix on a topic branch")
        self.git("checkout", "-q", "Plasma/6.8")
        self.git(
            "merge",
            "-q",
            "--no-ff",
            "-m",
            "Merge branch 'kde-topic' into Plasma/6.8",
            "kde-topic",
        )
        later = self.git("rev-parse", "HEAD")
        self.git("checkout", "-q", "candidate/6.8.2-pf5")
        self.write("src/merged.cpp", "a merged fix\n")
        self.commit("drm: the merged fix (stable import)\n\n" + SIGNOFF)
        self.git("checkout", "-q", "--detach", "v6.8.3")
        result = self.replay(later=later)
        self.assertEqual(result.status, "early")
        self.assertEqual(
            sorted(entry.split(" ", 1)[1] for entry in result.early),
            [
                "Merge branch 'kde-topic' into Plasma/6.8",
                "drm: a fix on a topic branch",
            ],
        )

    def test_a_later_fix_carried_early_stops_the_replay(self):
        self.git("checkout", "-q", "candidate/6.8.2-pf5")
        self.write("src/later.cpp", "a later fix\n")
        self.commit("drm: a later fix (stable import)\n\n" + SIGNOFF)
        self.git("checkout", "-q", "--detach", "v6.8.3")
        result = self.replay(later=self.later)
        self.assertEqual(result.status, "early")
        self.assertEqual(
            [entry.split(" ", 1)[1] for entry in result.early], ["drm: a later fix"]
        )


class ControlTest(Fixture):
    def setUp(self):
        super().setUp()
        self.git("checkout", "-q", "--orphan", "control")
        self.git("rm", "-rfq", ".")
        self.write("README", "readme\n")
        self.commit("upstream: 7.2.1")
        self.git("tag", "v7.2.1")
        self.write(".github/workflows/kde.yml", "on: push\n")
        self.commit("upstream: 7.2.2")
        self.git("tag", "v7.2.2")
        self.git("checkout", "-q", "-b", "candidate/7.2.1-pf1", "v7.2.1")
        self.write("README", "readme\nfork\n")
        self.commit("fork: readme\n\n" + SIGNOFF)
        self.git("checkout", "-q", "--detach", "v7.2.2")

    def test_a_kde_workflow_change_the_delta_imported_early_is_held_too(self):
        self.git("checkout", "-q", "candidate/7.2.1-pf1")
        self.write(".github/workflows/kde.yml", "on: push\n")
        self.commit("ci: KDE's workflow, imported\n\n" + SIGNOFF)
        self.git("checkout", "-q", "--detach", "v7.2.2")
        result = self.replay(
            previous="candidate/7.2.1-pf1", base="v7.2.1", tag="v7.2.2"
        )
        self.assertEqual(result.status, "control")
        self.assertEqual(result.control, [".github/workflows/kde.yml"])

    def test_a_kde_change_to_the_workflows_is_held(self):
        result = self.replay(
            previous="candidate/7.2.1-pf1", base="v7.2.1", tag="v7.2.2"
        )
        self.assertEqual(result.status, "control")
        self.assertEqual(result.control, [".github/workflows/kde.yml"])


class ModeTest(Fixture):
    def setUp(self):
        super().setUp()
        self.git("checkout", "-q", "--orphan", "mode")
        self.git("rm", "-rfq", ".")
        self.write("tool.sh", "echo tool\n")
        self.write("README", "one\n")
        self.commit("upstream: 7.3.1")
        self.git("tag", "v7.3.1")
        self.write("README", "two\n")
        self.commit("upstream: 7.3.2")
        self.git("tag", "v7.3.2")
        self.git("checkout", "-q", "-b", "candidate/7.3.1-pf1", "v7.3.1")
        (self.repo / "tool.sh").chmod(0o755)
        self.commit("fork: tool.sh is a program\n\n" + SIGNOFF)
        self.git("checkout", "-q", "--detach", "v7.3.2")

    def test_a_mode_change_replays_and_its_loss_is_unexplained(self):
        result = self.replay(
            previous="candidate/7.3.1-pf1", base="v7.3.1", tag="v7.3.2"
        )
        self.assertEqual(result.status, "clean", result.diagnostics)
        self.assertEqual(
            self.git("ls-tree", result.commit, "tool.sh").split()[0], "100755"
        )
        problems = candidate.unexplained(
            self.repo,
            result.base_commit,
            result.previous,
            result.tag_commit,
            result.tag_commit,
        )
        self.assertEqual([p.split(":")[0] for p in problems], ["tool.sh"])
        self.assertIn("100755 -> 100644", problems[0])


class CommitRulesTest(Fixture):
    def test_trailers_and_attribution_follow_fork_ci(self):
        self.git("checkout", "-q", "--detach", "v6.8.3")
        start = self.git("rev-parse", "HEAD")
        self.write("a", "1\n")
        self.commit("no trailer")
        self.write("a", "2\n")
        self.commit("two\n\n" + SIGNOFF + "\n" + SIGNOFF)
        self.write("a", "3\n")
        self.commit(
            "other trailer\n\nCo-authored-by: Someone <s@example.invalid>\n" + SIGNOFF
        )
        self.write("a", "4\n")
        self.commit("Written by an AI-generated tool\n\n" + SIGNOFF)
        self.write("a", "5\n")
        self.commit("fine\n\n" + SIGNOFF)
        problems = candidate.commit_problems(self.repo, start, "HEAD")
        self.assertEqual(len(problems), 4, problems)
        joined = "\n".join(problems)
        for word in ["no trailer", "two", "other trailer", "AI-generated"]:
            self.assertIn(word, joined)


class EmptyMessageTest(Fixture):
    def test_a_commit_without_a_message_has_an_empty_subject(self):
        start = self.git("rev-parse", "HEAD")
        self.write("a", "1\n")
        self.git("add", "-A")
        self.git("commit", "-q", "--allow-empty-message", "-m", "")
        (entry,) = candidate.commit_keys(self.repo, start, "HEAD")
        self.assertEqual(entry[2], "")


class MirrorTest(Fixture):
    def setUp(self):
        super().setUp()
        # "kde": the upstream repository; "fork": the fork's remote with a stale mirror branch.
        self.kde = self.repo
        self.fork = self.root / "fork.git"
        subprocess.check_call(["git", "init", "-q", "--bare", str(self.fork)])
        stale = self.git("rev-parse", "v6.8.2^{commit}")
        self.git("push", "-q", str(self.fork), f"{stale}:refs/heads/Plasma/6.8")
        self.clone = self.root / "clone"
        subprocess.check_call(
            ["git", "clone", "-q", str(self.fork), str(self.clone)],
            stderr=subprocess.DEVNULL,
        )
        self.git(
            "fetch", "-q", str(self.kde), "+refs/tags/*:refs/tags/*", repo=self.clone
        )

    def mirror(self, push):
        return candidate.mirror(
            self.clone, "v6.8.3", kde=str(self.kde), remote="origin", push=push
        )

    def test_stale_mirror_is_fast_forwarded_to_kde(self):
        dry = self.mirror(push=False)
        self.assertEqual(dry.status, "behind")
        self.assertEqual(
            self.git("rev-parse", "Plasma/6.8", repo=self.fork),
            self.git("rev-parse", "v6.8.2^{commit}"),
        )
        result = self.mirror(push=True)
        self.assertEqual(result.status, "updated")
        self.assertEqual(result.new, self.git("rev-parse", "Plasma/6.8", repo=self.kde))
        self.assertEqual(
            self.git("rev-parse", "Plasma/6.8", repo=self.fork), result.new
        )
        self.assertEqual(self.mirror(push=True).status, "current")

    def test_mirror_that_is_not_an_ancestor_is_left_alone(self):
        self.git("checkout", "-q", "--detach", "v6.8.2")
        self.write("elsewhere", "x\n")
        other = self.commit("not KDE's")
        self.git("push", "-q", "-f", str(self.fork), f"{other}:refs/heads/Plasma/6.8")
        with self.assertRaises(candidate.CandidateError):
            self.mirror(push=True)
        self.assertEqual(self.git("rev-parse", "Plasma/6.8", repo=self.fork), other)

    def test_a_mirror_at_the_tag_follows_kde_further(self):
        # KDE's branch goes on after the tag: the mirror is KDE's branch, not the tag.
        self.git("checkout", "-q", "Plasma/6.8")
        self.write("src/later.cpp", "later\n")
        later = self.commit("drm: a later fix")
        self.git("checkout", "-q", "--detach", "v6.8.3")
        tag = self.git("rev-parse", "v6.8.3^{commit}")
        self.git("push", "-q", "-f", str(self.fork), f"{tag}:refs/heads/Plasma/6.8")
        self.assertEqual(self.mirror(push=False).status, "behind")
        result = self.mirror(push=True)
        self.assertEqual((result.status, result.new), ("updated", later))
        self.assertEqual(self.git("rev-parse", "Plasma/6.8", repo=self.fork), later)

    def test_a_mirror_with_a_commit_kde_lacks_is_refused_although_it_has_the_tag(self):
        self.git("checkout", "-q", "--detach", "v6.8.3")
        self.write("fork-only", "x\n")
        extra = self.commit("not KDE's")
        self.git("push", "-q", "-f", str(self.fork), f"{extra}:refs/heads/Plasma/6.8")
        for push in (False, True):
            with self.subTest(push=push), self.assertRaises(candidate.CandidateError):
                self.mirror(push=push)
        self.assertEqual(self.git("rev-parse", "Plasma/6.8", repo=self.fork), extra)

    def test_local_tag_must_be_kde_tag(self):
        self.git("tag", "-f", "v6.8.3", "v6.8.2", repo=self.clone)
        with self.assertRaises(candidate.CandidateError):
            self.mirror(push=False)


class PushTest(Fixture):
    def setUp(self):
        super().setUp()
        self.fork = self.root / "fork.git"
        subprocess.check_call(["git", "init", "-q", "--bare", str(self.fork)])
        self.git("remote", "add", "origin", str(self.fork))

    def test_push_creates_the_branch_once(self):
        result = self.replay()
        candidate.push(self.repo, result.branch, result.commit, remote="origin")
        self.assertEqual(
            self.git("rev-parse", result.branch, repo=self.fork), result.commit
        )
        with self.assertRaises(candidate.CandidateError):
            candidate.push(
                self.repo,
                result.branch,
                self.git("rev-parse", "v6.8.3^{commit}"),
                remote="origin",
            )
        self.assertEqual(
            self.git("rev-parse", result.branch, repo=self.fork), result.commit
        )

    def test_the_same_candidate_made_again_keeps_the_branch(self):
        first = self.replay()
        self.assertEqual(
            candidate.push(self.repo, first.branch, first.commit, remote="origin"),
            (first.commit, "created"),
        )
        # The replay made again, a minute later: other commit times, so other commit names.
        again = subprocess.check_output(
            [
                "git",
                "-C",
                str(self.repo),
                "rebase",
                "-q",
                "--force-rebase",
                "--onto",
                first.tag_commit,
                first.tag_commit,
                first.commit,
            ],
            text=True,
            env=dict(os.environ, GIT_COMMITTER_DATE="2030-01-01T00:00:00Z"),
            stderr=subprocess.STDOUT,
        )
        remade = self.git("rev-parse", "HEAD")
        self.git("checkout", "-q", "--detach", "v6.8.3")
        self.assertNotEqual(remade, first.commit)
        self.assertEqual(
            candidate.push(self.repo, first.branch, remade, remote="origin"),
            (first.commit, "equivalent"),
        )
        self.assertEqual(
            self.git("rev-parse", first.branch, repo=self.fork), first.commit
        )

    def test_another_candidate_under_the_name_is_refused(self):
        first = self.replay()
        candidate.push(self.repo, first.branch, first.commit, remote="origin")
        self.git("checkout", "-q", "--detach", first.commit)
        self.git(
            "commit", "-q", "--amend", "-m", "placement: another message\n\n" + SIGNOFF
        )
        other = self.git("rev-parse", "HEAD")
        self.git("checkout", "-q", "--detach", "v6.8.3")
        with self.assertRaises(candidate.CandidateError):
            candidate.push(self.repo, first.branch, other, remote="origin")
        # The same commits with other content.
        self.git("checkout", "-q", "--detach", first.commit)
        self.write("fork/placement.cpp", "placement, changed\n")
        self.git("commit", "-q", "-a", "--amend", "--no-edit")
        changed = self.git("rev-parse", "HEAD")
        self.git("checkout", "-q", "--detach", "v6.8.3")
        with self.assertRaises(candidate.CandidateError):
            candidate.push(self.repo, first.branch, changed, remote="origin")
        self.assertEqual(
            self.git("rev-parse", first.branch, repo=self.fork), first.commit
        )

    def test_only_the_branch_by_its_full_name_counts(self):
        result = self.replay()
        decoy = f"refs/heads/x/refs/heads/{result.branch}"
        self.git("push", "-q", str(self.fork), f"{result.commit}:{decoy}")
        self.assertEqual(
            candidate.push(self.repo, result.branch, result.commit, remote="origin"),
            (result.commit, "created"),
        )
        self.assertEqual(
            self.git("rev-parse", f"refs/heads/{result.branch}", repo=self.fork),
            result.commit,
        )

    def test_the_same_end_by_other_commits_is_another_candidate(self):
        first = self.replay()
        candidate.push(self.repo, first.branch, first.commit, remote="origin")
        one, two = self.git(
            "rev-list", "--reverse", f"{first.tag_commit}..{first.commit}"
        ).split()
        # The same two commits (author, message) and the same final tree, with the module moved
        # into the first commit.
        index = dict(os.environ, GIT_INDEX_FILE=str(self.root / "index"))
        run = lambda *args: subprocess.check_output(
            ["git", "-C", str(self.repo), *args], text=True, env=index
        ).strip()
        run("read-tree", one)
        blob = self.git("rev-parse", f"{two}:fork/placement.cpp")
        run("update-index", "--add", "--cacheinfo", f"100644,{blob},fork/placement.cpp")
        moved = run("write-tree")

        def remake(tree, parent, original):
            author = {
                name: self.git("log", "-1", f"--format=%{code}", "--date=raw", original)
                for name, code in (
                    ("GIT_AUTHOR_NAME", "an"),
                    ("GIT_AUTHOR_EMAIL", "ae"),
                    ("GIT_AUTHOR_DATE", "ad"),
                )
            }
            message = self.git("log", "-1", "--format=%B", original)
            return subprocess.check_output(
                [
                    "git",
                    "-C",
                    str(self.repo),
                    "commit-tree",
                    tree,
                    "-p",
                    parent,
                    "-F",
                    "-",
                ],
                input=message,
                text=True,
                env=dict(os.environ, **author),
            ).strip()

        other = remake(
            self.git("rev-parse", f"{two}^{{tree}}"),
            remake(moved, first.tag_commit, one),
            two,
        )
        self.assertEqual(self.git("rev-parse", f"{other}^{{tree}}"), first.tree)
        with self.assertRaises(candidate.CandidateError):
            candidate.push(self.repo, first.branch, other, remote="origin")
        self.assertEqual(
            self.git("rev-parse", first.branch, repo=self.fork), first.commit
        )

    def test_a_kept_branch_must_pass_the_commit_rules(self):
        first = self.replay()
        candidate.push(self.repo, first.branch, first.commit, remote="origin")
        subprocess.check_output(
            ["git", "-C", str(self.repo), "rebase", "-q", "--force-rebase", "--onto"]
            + [first.tag_commit, first.tag_commit, first.commit],
            text=True,
            env=dict(os.environ, GIT_COMMITTER_DATE="2030-01-01T00:00:00Z"),
            stderr=subprocess.STDOUT,
        )
        remade = self.git("rev-parse", "HEAD")
        self.git("checkout", "-q", "--detach", "v6.8.3")
        with self.stand_in_gpg(), self.assertRaises(candidate.CandidateError) as raised:
            candidate.push(
                self.repo,
                first.branch,
                remade,
                remote="origin",
                sign="0123456789ABCDEF0123456789ABCDEF01234567",
            )
        self.assertIn("fail the rules", str(raised.exception))

    def test_a_commit_not_built_on_the_tag_is_not_pushed(self):
        result = self.replay()
        with self.assertRaises(candidate.CandidateError):
            candidate.push(
                self.repo,
                result.branch,
                self.git("rev-parse", "candidate/6.8.2-pf5"),
                remote="origin",
            )
        self.assertEqual(self.git("ls-remote", str(self.fork), result.branch), "")

    def test_commits_that_fail_the_rules_are_not_pushed(self):
        result = self.replay()
        with self.stand_in_gpg(), self.assertRaises(candidate.CandidateError) as raised:
            candidate.push(
                self.repo,
                result.branch,
                result.commit,
                remote="origin",
                sign="0123456789ABCDEF0123456789ABCDEF01234567",
            )
        self.assertIn("fail the rules", str(raised.exception))
        self.assertEqual(self.git("ls-remote", str(self.fork), result.branch), "")

    def test_a_branch_at_the_commit_is_checked_too(self):
        result = self.replay()
        candidate.push(self.repo, result.branch, result.commit, remote="origin")
        with self.stand_in_gpg(), self.assertRaises(candidate.CandidateError) as raised:
            candidate.push(
                self.repo,
                result.branch,
                result.commit,
                remote="origin",
                sign="0123456789ABCDEF0123456789ABCDEF01234567",
            )
        self.assertIn("fail the rules", str(raised.exception))

    def test_a_candidate_without_a_fork_commit_is_not_pushed(self):
        with self.assertRaises(candidate.CandidateError) as raised:
            candidate.push(
                self.repo,
                "candidate/6.8.3-pf1",
                self.git("rev-parse", "v6.8.3^{commit}"),
                remote="origin",
            )
        self.assertIn("no fork commit", str(raised.exception))

    def test_a_branch_that_moves_during_the_check_is_refused(self):
        result = self.replay()
        candidate.push(self.repo, result.branch, result.commit, remote="origin")
        reads = []
        original = candidate.remote_ref

        def moving(repo, remote, ref):
            reads.append(ref)
            if len(reads) > 1:
                return "0" * 40
            return original(repo, remote, ref)

        candidate.remote_ref = moving
        self.addCleanup(setattr, candidate, "remote_ref", original)
        with self.assertRaises(candidate.CandidateError) as raised:
            candidate.push(self.repo, result.branch, result.commit, remote="origin")
        self.assertIn("moved", str(raised.exception))

    def test_a_pre_release_branch_is_not_pushed(self):
        # A local tag of a pre-release patch number does not make it a point release.
        self.git("tag", "v6.8.80", "v6.8.3")
        with self.assertRaises(candidate.CandidateError) as raised:
            candidate.push(
                self.repo,
                "candidate/6.8.80-pf1",
                self.git("rev-parse", "candidate/6.8.2-pf5"),
                remote="origin",
            )
        self.assertIn("pre-release", str(raised.exception))
        self.assertEqual(self.git("ls-remote", str(self.fork)), "")

    def test_push_only_candidate_branches(self):
        with self.assertRaises(candidate.CandidateError):
            candidate.push(
                self.repo,
                "plasma-fusion",
                self.git("rev-parse", "v6.8.3^{commit}"),
                remote="origin",
            )


if __name__ == "__main__":
    unittest.main()
