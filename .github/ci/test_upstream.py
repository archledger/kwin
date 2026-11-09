# SPDX-FileCopyrightText: 2026 Wisbendji Fimerlus <archledger236@gmail.com>
# SPDX-License-Identifier: GPL-2.0-or-later
import json
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

import upstream


class IntegrationTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.repo = Path(self.temporary.name) / "repo"
        self.repo.mkdir()
        self.git("init", "-q", "-b", "fork")
        self.git("config", "user.name", "Fixture")
        self.git("config", "user.email", "fixture@example.invalid")
        self.git("config", "commit.gpgsign", "false")
        self.write("app", "original\n")
        self.write("library", "version 0\n")
        self.base = self.commit("base")
        self.write("app", "downstream\n")
        self.write_manifest(self.base)
        self.fork = self.commit("downstream changes")

    def git(self, *args, input=None):
        return subprocess.check_output(
            ["git", "-C", str(self.repo), *args],
            text=True,
            input=input,
            stderr=subprocess.PIPE,
        ).strip()

    def write(self, name, content):
        path = self.repo / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content)

    def write_manifest(self, commit):
        self.write(
            ".github/upstream.json",
            json.dumps(
                {
                    "schema": 1,
                    "repository": "https://github.com/KDE/kwin.git",
                    "stable_ref": "refs/heads/Plasma/6.8",
                    "stable_commit": commit,
                    "preview_ref": "refs/heads/master",
                }
            ),
        )

    def commit(self, message):
        self.git("add", ".")
        self.git("commit", "-qm", message)
        return self.git("rev-parse", "HEAD")

    def upstream_commit(self, parent, name, content):
        self.git("checkout", "-q", "--detach", parent)
        self.write(name, content)
        result = self.commit("upstream change")
        self.git("checkout", "-q", "--detach", self.fork)
        return result

    def test_stable_sync_preserves_patch_and_advances_pin(self):
        tip = self.upstream_commit(self.base, "library", "version 1\n")
        result = upstream.integrate(self.repo, self.fork, tip, "stable")
        self.assertEqual(result.status, "clean")
        self.assertEqual(self.git("show", result.tree + ":app"), "downstream")
        self.assertEqual(self.git("show", result.tree + ":library"), "version 1")
        manifest = json.loads(self.git("show", result.tree + ":.github/upstream.json"))
        self.assertEqual(manifest["stable_commit"], tip)
        self.assertEqual(self.git("rev-parse", "HEAD"), self.fork)
        self.assertEqual(self.git("status", "--porcelain"), "")

    def test_conflict_is_not_a_candidate(self):
        tip = self.upstream_commit(self.base, "app", "upstream\n")
        result = upstream.integrate(self.repo, self.fork, tip, "stable")
        self.assertEqual(result.status, "conflict")
        self.assertIsNone(result.tree)
        self.assertIn("CONFLICT", result.diagnostics)
        self.assertEqual(self.git("status", "--porcelain"), "")

    def test_repeated_linear_sync_uses_updated_upstream_base(self):
        first = self.upstream_commit(self.base, "library", "version 1\n")
        result = upstream.integrate(self.repo, self.fork, first, "stable")
        linear = self.git("commit-tree", result.tree, "-p", self.fork, input="sync\n")
        self.assertNotEqual(self.git("merge-base", linear, first), first)
        second = self.upstream_commit(first, "library", "version 2\n")
        result = upstream.integrate(self.repo, linear, second, "stable")
        self.assertEqual(result.status, "clean")
        self.assertEqual(result.base, first)
        self.assertEqual(self.git("show", result.tree + ":library"), "version 2")
        self.assertEqual(self.git("show", result.tree + ":app"), "downstream")

    def test_preview_does_not_require_stable_ancestor(self):
        stable = self.upstream_commit(self.base, "library", "stable-only\n")
        result = upstream.integrate(self.repo, self.fork, stable, "stable")
        linear = self.git("commit-tree", result.tree, "-p", self.fork, input="sync\n")
        preview = self.upstream_commit(self.base, "preview", "development\n")
        result = upstream.integrate(self.repo, linear, preview, "preview")
        self.assertEqual(result.status, "clean")
        self.assertEqual(self.git("show", result.tree + ":app"), "downstream")
        self.assertEqual(self.git("show", result.tree + ":preview"), "development")
        self.assertEqual(self.git("show", result.tree + ":library"), "version 0")
        manifest = json.loads(self.git("show", result.tree + ":.github/upstream.json"))
        self.assertEqual(manifest["stable_commit"], stable)

    def test_upstream_rename_preserves_downstream_edit(self):
        self.git("checkout", "-q", "--detach", self.base)
        self.git("mv", "app", "renamed")
        renamed = self.commit("upstream rename")
        result = upstream.integrate(self.repo, self.fork, renamed, "stable")
        self.assertEqual(result.status, "clean")
        self.assertEqual(self.git("show", result.tree + ":renamed"), "downstream")
        self.assertNotIn(
            "app", self.git("ls-tree", "--name-only", result.tree).splitlines()
        )

    def test_upstream_deletion_of_edited_file_is_conflict(self):
        self.git("checkout", "-q", "--detach", self.base)
        self.git("rm", "app")
        deleted = self.commit("upstream deletion")
        result = upstream.integrate(self.repo, self.fork, deleted, "stable")
        self.assertEqual(result.status, "conflict")
        self.assertIsNone(result.tree)

    def test_existing_index_and_worktree_are_untouched(self):
        tip = self.upstream_commit(self.base, "library", "version 1\n")
        self.write("staged", "user change\n")
        self.git("add", "staged")
        self.write("untracked", "another user change\n")
        before = self.git("status", "--porcelain")
        index = self.git("write-tree")
        upstream.integrate(self.repo, self.fork, tip, "stable")
        self.assertEqual(self.git("status", "--porcelain"), before)
        self.assertEqual(self.git("write-tree"), index)

    def test_missing_object_and_rewound_stable_are_errors(self):
        with self.assertRaises(upstream.IntegrationError):
            upstream.integrate(self.repo, self.fork, "0" * 40, "stable")
        tip = self.upstream_commit(self.base, "library", "version 1\n")
        result = upstream.integrate(self.repo, self.fork, tip, "stable")
        linear = self.git("commit-tree", result.tree, "-p", self.fork, input="sync\n")
        with self.assertRaises(upstream.IntegrationError):
            upstream.integrate(self.repo, linear, self.base, "stable")

    def test_current_stable_has_no_source_update(self):
        result = upstream.integrate(self.repo, self.fork, self.base, "stable")
        self.assertEqual(result.status, "current")
        self.assertEqual(result.tree, self.git("rev-parse", self.fork + "^{tree}"))

    def test_manifest_cannot_redirect_upstream(self):
        data = json.loads((self.repo / ".github/upstream.json").read_text())
        data["repository"] = "http://127.0.0.1/private"
        self.write(".github/upstream.json", json.dumps(data))
        fork = self.commit("invalid source")
        with self.assertRaises(upstream.IntegrationError):
            upstream.integrate(self.repo, fork, self.base, "stable")

    def test_candidate_commit_and_checkout_have_exact_tree(self):
        tip = self.upstream_commit(self.base, "library", "version 1\n")
        result = upstream.integrate(self.repo, self.fork, tip, "stable")
        before = self.git("show-ref")
        destination = self.repo.parent / "candidate"
        commit = upstream.materialize(self.repo, result, destination)
        self.assertEqual(self.git("show-ref"), before)
        self.assertEqual(self.git("rev-parse", commit + "^{tree}"), result.tree)
        self.assertEqual(self.git("rev-parse", commit + "^"), self.fork)
        self.assertEqual((destination / "app").read_text(), "downstream\n")
        self.assertEqual((destination / "library").read_text(), "version 1\n")

    def test_materialization_refuses_conflict_and_existing_directory(self):
        conflict = self.upstream_commit(self.base, "app", "upstream\n")
        result = upstream.integrate(self.repo, self.fork, conflict, "stable")
        destination = self.repo.parent / "candidate"
        with self.assertRaises(upstream.IntegrationError):
            upstream.materialize(self.repo, result, destination)
        self.assertFalse(destination.exists())
        tip = self.upstream_commit(self.base, "library", "version 1\n")
        result = upstream.integrate(self.repo, self.fork, tip, "stable")
        destination.mkdir()
        (destination / "keep").write_text("user work")
        with self.assertRaises(upstream.IntegrationError):
            upstream.materialize(self.repo, result, destination)
        self.assertEqual((destination / "keep").read_text(), "user work")

    def test_git_timeout_is_an_integration_error(self):
        with (
            patch(
                "upstream.subprocess.run",
                side_effect=subprocess.TimeoutExpired("git", 120),
            ),
            self.assertRaises(upstream.IntegrationError),
        ):
            upstream.git(self.repo, "status")

    def test_prepare_cli_exports_exact_candidate(self):
        tip = self.upstream_commit(self.base, "library", "version 1\n")
        output, source = self.repo.parent / "result", self.repo.parent / "candidate"
        command = [
            "python3",
            str(Path(upstream.__file__)),
            "prepare",
            "--repo",
            str(self.repo),
            "--fork",
            self.fork,
            "--upstream",
            tip,
            "--track",
            "stable",
            "--output",
            str(output),
            "--source-dir",
            str(source),
        ]
        result = subprocess.run(command, capture_output=True, text=True, check=False)
        self.assertEqual(result.returncode, 0, result.stderr)
        report = json.loads((output / "result.json").read_text())
        self.assertEqual(report["status"], "clean")
        self.assertEqual((source / "app").read_text(), "downstream\n")
        self.assertEqual(
            self.git("rev-parse", report["candidate_commit"] + "^{tree}"),
            report["tree"],
        )

    def test_conflict_cli_fails_and_keeps_diagnostics(self):
        tip = self.upstream_commit(self.base, "app", "upstream\n")
        output, source = self.repo.parent / "result", self.repo.parent / "candidate"
        result = subprocess.run(
            [
                "python3",
                str(Path(upstream.__file__)),
                "prepare",
                "--repo",
                str(self.repo),
                "--fork",
                self.fork,
                "--upstream",
                tip,
                "--track",
                "stable",
                "--output",
                str(output),
                "--source-dir",
                str(source),
            ],
            capture_output=True,
            text=True,
            check=False,
        )
        self.assertEqual(result.returncode, 2)
        report = json.loads((output / "result.json").read_text())
        self.assertEqual(report["status"], "conflict")
        self.assertFalse(source.exists())
        self.assertIn("CONFLICT", (output / "merge.log").read_text())

    def test_project_version_is_data_not_executed_code(self):
        self.write(
            "CMakeLists.txt",
            'set(PROJECT_VERSION "6.8.80") # release\nexecute_process(COMMAND false)\n',
        )
        self.assertEqual(upstream.project_version(self.repo), "6.8.80")
        for value in (
            'set(PROJECT_VERSION "6.8.2;command")\n',
            'set(PROJECT_VERSION "6.8.2")\nset(PROJECT_VERSION "6.9.0")\n',
        ):
            self.write("CMakeLists.txt", value)
            with self.assertRaises(upstream.IntegrationError):
                upstream.project_version(self.repo)

    def test_exported_patch_preserves_non_utf8_blob(self):
        self.git("checkout", "-q", "--detach", self.base)
        (self.repo / "legacy-text").write_bytes(b"non-utf8: \xff\n")
        tip = self.commit("upstream non-utf8 data")
        self.git("checkout", "-q", "--detach", self.fork)
        output = self.repo.parent / "result"
        result = subprocess.run(
            [
                "python3",
                str(Path(upstream.__file__)),
                "prepare",
                "--repo",
                str(self.repo),
                "--fork",
                self.fork,
                "--upstream",
                tip,
                "--track",
                "stable",
                "--output",
                str(output),
            ],
            capture_output=True,
            text=True,
            check=False,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        report = json.loads((output / "result.json").read_text())
        self.git("apply", "--index", str(output / "candidate.patch"))
        self.assertEqual(self.git("write-tree"), report["tree"])


if __name__ == "__main__":
    unittest.main()
