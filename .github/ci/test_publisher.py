# SPDX-FileCopyrightText: 2026 Wisbendji Fimerlus <archledger236@gmail.com>
# SPDX-License-Identifier: GPL-2.0-or-later
import fcntl
import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

import publisher
import test_upstream
import upstream


class CandidateTest(unittest.TestCase):
    def setUp(self):
        self.fixture = test_upstream.IntegrationTest()
        self.fixture.setUp()
        self.addCleanup(self.fixture.doCleanups)
        self.repo = self.fixture.repo

    def candidate(self, name="library", text="version 1\n"):
        tip = self.fixture.upstream_commit(self.fixture.base, name, text)
        return upstream.integrate(self.repo, self.fixture.fork, tip, "stable")

    def test_regular_source_update_preserves_downstream_and_advances_manifest(self):
        result = self.candidate()
        publisher.validate_candidate(self.repo, result)
        manifest = json.loads(
            self.fixture.git("show", result.tree + ":" + upstream.MANIFEST)
        )
        self.assertEqual(manifest["stable_commit"], result.upstream)
        self.assertEqual(self.fixture.git("show", result.tree + ":app"), "downstream")

    def test_ci_changes_refused_before_signing(self):
        result = self.candidate(
            ".github/workflows/attack.yml", "unreviewed host code\n"
        )
        with self.assertRaisesRegex(upstream.IntegrationError, "control path"):
            publisher.validate_candidate(self.repo, result)

    def test_git_control_files_refused_even_outside_root(self):
        for name in (".gitmodules", ".gitattributes", "src/.gitattributes"):
            with self.subTest(name=name):
                result = self.candidate(name, "* merge=custom\n")
                with self.assertRaisesRegex(upstream.IntegrationError, "control path"):
                    publisher.validate_candidate(self.repo, result)

    def test_conflict_never_becomes_publication(self):
        result = self.candidate("app", "conflicting upstream edit\n")
        with self.assertRaisesRegex(upstream.IntegrationError, "clean"):
            publisher.validate_candidate(self.repo, result)

    def test_branch_and_message_bind_both_full_input_shas(self):
        result = self.candidate()
        branch = publisher.branch_name(result)
        message = publisher.commit_message(result)
        self.assertIn(result.fork, branch)
        self.assertIn(result.upstream, branch)
        self.assertIn(result.base, message)
        self.assertIn(result.upstream, message)
        self.assertEqual(message.count("Signed-off-by:"), 1)
        other = upstream.Integration(
            "clean", result.base, "1" * 40, result.upstream, result.tree
        )
        self.assertNotEqual(branch, publisher.branch_name(other))

    def test_forged_manifest_tree_refused(self):
        result = self.candidate()
        manifest = upstream.read_manifest(self.repo, result.fork)
        wrong = upstream.update_manifest(self.repo, result.tree, manifest)
        forged = upstream.Integration(
            result.status, result.base, result.fork, result.upstream, wrong
        )
        with self.assertRaisesRegex(upstream.IntegrationError, "manifest"):
            publisher.validate_candidate(self.repo, forged)

    def test_control_filename_with_newline_or_non_utf8_is_refused(self):
        for name in (".github/workflows/a\nb.yml", ".github/bad-\udcff"):
            with self.subTest(name=name):
                result = self.candidate(name, "unreviewed\n")
                with self.assertRaisesRegex(upstream.IntegrationError, "control path"):
                    publisher.validate_candidate(self.repo, result)

    def test_control_deletion_and_rename_are_refused(self):
        self.fixture.git("checkout", "--detach", self.fixture.base)
        self.fixture.write(".github/control", "existing control\n")
        base = self.fixture.commit("old upstream with control")
        self.fixture.write_manifest(base)
        fork = self.fixture.commit("fork manifest")
        self.fixture.git("mv", ".github/control", "moved-outside")
        tip = self.fixture.commit("move control outside guarded directory")
        result = upstream.integrate(self.repo, fork, tip, "stable")
        with self.assertRaisesRegex(upstream.IntegrationError, "control path"):
            publisher.validate_candidate(self.repo, result)


class FakeForge:
    def __init__(self, remote):
        self.remote = str(remote)
        self.pulls = []
        self.created = 0

    def prs(self, branch):
        return list(self.pulls)

    def create(self, branch, result, commit):
        self.created += 1
        self.pulls.append(
            {
                "number": 1,
                "state": "OPEN",
                "headRefOid": commit,
                "headRefName": branch,
                "baseRefName": publisher.MAIN,
                "headRepositoryOwner": {"login": "archledger"},
                "isCrossRepository": False,
                "url": "https://github.com/archledger/kwin/pull/1",
            }
        )


class PublicationTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.keydir = tempfile.TemporaryDirectory(prefix="kwin-test-key-")
        cls.home = cls.keydir.name
        subprocess.run(
            [
                "gpg",
                "--homedir",
                cls.home,
                "--batch",
                "--pinentry-mode",
                "loopback",
                "--passphrase",
                "",
                "--quick-generate-key",
                "Publisher fixture <fixture@example.invalid>",
                "ed25519",
                "sign",
                "0",
            ],
            check=True,
            capture_output=True,
            timeout=30,
        )
        keys = subprocess.check_output(
            ["gpg", "--homedir", cls.home, "--with-colons", "--list-keys"], text=True
        )
        cls.signer = next(
            line.split(":")[9] for line in keys.splitlines() if line.startswith("fpr:")
        )

    @classmethod
    def tearDownClass(cls):
        subprocess.run(
            ["gpgconf", "--homedir", cls.home, "--kill", "gpg-agent"],
            check=True,
            timeout=10,
        )
        cls.keydir.cleanup()

    def setUp(self):
        self.env = patch.dict(os.environ, {"GNUPGHOME": self.home})
        self.env.start()
        self.addCleanup(self.env.stop)
        self.fixture = test_upstream.IntegrationTest()
        self.fixture.setUp()
        self.addCleanup(self.fixture.doCleanups)
        self.repo = self.fixture.repo
        tip = self.fixture.upstream_commit(self.fixture.base, "library", "version 1\n")
        self.result = upstream.integrate(self.repo, self.fixture.fork, tip, "stable")
        remote = self.repo.parent / "remote.git"
        subprocess.run(["git", "init", "--bare", "-q", str(remote)], check=True)
        self.fixture.git(
            "push", str(remote), self.fixture.fork + ":refs/heads/" + publisher.MAIN
        )
        self.forge = FakeForge(remote)

    def test_sign_and_verify_then_reject_wrong_key_and_unsigned_commit(self):
        commit = publisher.sign_candidate(self.repo, self.result, self.signer)
        publisher.verify_candidate(self.repo, commit, self.result, self.signer)
        with self.assertRaisesRegex(upstream.IntegrationError, "signing key"):
            publisher.verify_candidate(self.repo, commit, self.result, "0" * 40)
        unsigned = self.fixture.git(
            "-c",
            "user.name=" + publisher.NAME,
            "-c",
            "user.email=" + publisher.EMAIL,
            "commit-tree",
            self.result.tree,
            "-p",
            self.result.fork,
            input=publisher.commit_message(self.result),
        )
        with self.assertRaises(upstream.IntegrationError):
            publisher.verify_candidate(self.repo, unsigned, self.result, self.signer)

    def test_first_publication_and_duplicate_reuse_same_commit(self):
        first = publisher.propose(self.repo, self.result, self.forge, self.signer)
        self.assertEqual(first["status"], "proposed")
        with patch.object(
            publisher,
            "sign_candidate",
            side_effect=AssertionError("must reuse signature"),
        ):
            second = publisher.propose(self.repo, self.result, self.forge, self.signer)
        self.assertEqual(second["commit"], first["commit"])
        self.assertEqual(second["status"], "existing")
        self.assertEqual(self.forge.created, 1)
        self.assertEqual(
            publisher.remote_commit(
                self.repo, self.forge.remote, "refs/heads/" + publisher.MAIN
            ),
            self.fixture.fork,
        )

    def test_publication_interrupted_after_push_recovers_without_resigning(self):
        with (
            patch.object(
                self.forge,
                "create",
                side_effect=upstream.IntegrationError("network failure"),
            ),
            self.assertRaisesRegex(upstream.IntegrationError, "network failure"),
        ):
            publisher.propose(self.repo, self.result, self.forge, self.signer)
        with patch.object(
            publisher, "sign_candidate", side_effect=AssertionError("must reuse")
        ):
            report = publisher.propose(self.repo, self.result, self.forge, self.signer)
        self.assertEqual(report["status"], "proposed")
        self.assertEqual(self.forge.created, 1)

    def test_successful_pr_creation_with_lost_response_is_reused(self):
        create = self.forge.create

        def lost(*args):
            create(*args)
            raise upstream.IntegrationError("response lost after PR was created")

        with (
            patch.object(self.forge, "create", side_effect=lost),
            self.assertRaises(upstream.IntegrationError),
        ):
            publisher.propose(self.repo, self.result, self.forge, self.signer)
        result = publisher.propose(self.repo, self.result, self.forge, self.signer)
        self.assertEqual(result["status"], "existing")
        self.assertEqual(self.forge.created, 1)

    def test_successful_ref_creation_with_lost_response_is_reused(self):
        create = publisher.create_branch

        def lost(*args):
            create(*args)
            raise upstream.IntegrationError("response lost after ref was created")

        with (
            patch.object(publisher, "create_branch", side_effect=lost),
            self.assertRaises(upstream.IntegrationError),
        ):
            publisher.propose(self.repo, self.result, self.forge, self.signer)
        with patch.object(
            publisher, "sign_candidate", side_effect=AssertionError("must reuse")
        ):
            result = publisher.propose(self.repo, self.result, self.forge, self.signer)
        self.assertEqual(result["status"], "proposed")
        self.assertEqual(self.forge.created, 1)

    def test_closed_or_mismatched_pull_is_not_overwritten(self):
        publisher.propose(self.repo, self.result, self.forge, self.signer)
        original = self.forge.pulls[0].copy()
        for change in (
            {"state": "CLOSED"},
            {"headRefOid": "0" * 40},
            {"baseRefName": "other"},
            {"isCrossRepository": True},
        ):
            with self.subTest(change=change):
                self.forge.pulls[0] = original | change
                with self.assertRaises(upstream.IntegrationError):
                    publisher.propose(self.repo, self.result, self.forge, self.signer)
        self.assertEqual(self.forge.created, 1)

    def test_existing_wrong_branch_refused_without_change(self):
        ref = "refs/heads/" + publisher.branch_name(self.result)
        self.fixture.git("push", self.forge.remote, self.fixture.fork + ":" + ref)
        with self.assertRaises(upstream.IntegrationError):
            publisher.propose(self.repo, self.result, self.forge, self.signer)
        self.assertEqual(
            publisher.remote_commit(self.repo, self.forge.remote, ref),
            self.fixture.fork,
        )
        self.assertEqual(self.forge.created, 0)

    def test_concurrent_branch_creation_cannot_be_overwritten(self):
        ref = "refs/heads/" + publisher.branch_name(self.result)
        commit = publisher.sign_candidate(self.repo, self.result, self.signer)
        self.fixture.git("push", self.forge.remote, self.fixture.fork + ":" + ref)
        with self.assertRaises(upstream.IntegrationError):
            publisher.create_branch(self.repo, self.forge.remote, ref, commit)
        self.assertEqual(
            publisher.remote_commit(self.repo, self.forge.remote, ref),
            self.fixture.fork,
        )

    def test_main_movement_stops_publication(self):
        self.fixture.git(
            "push",
            self.forge.remote,
            self.result.upstream + ":refs/heads/" + publisher.MAIN,
            "--force",
        )
        with self.assertRaisesRegex(upstream.IntegrationError, "main moved"):
            publisher.propose(self.repo, self.result, self.forge, self.signer)
        self.assertEqual(self.forge.created, 0)
        self.assertIsNone(
            publisher.remote_commit(
                self.repo,
                self.forge.remote,
                "refs/heads/" + publisher.branch_name(self.result),
            )
        )


class RunnerTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.state = self.root / "state"

    def test_manual_runs_do_not_reuse_an_inherited_service_identity(self):
        with (
            patch.dict(
                os.environ, {"INVOCATION_ID": "a" * 32, "KWIN_PUBLISHER_SERVICE": "0"}
            ),
            patch.object(publisher, "work", return_value={"status": "current"}),
        ):
            self.assertEqual(publisher.run(self.state, publish=False), 0)
            self.assertEqual(publisher.run(self.state, publish=False), 0)
        self.assertEqual(len(list((self.state / "runs").iterdir())), 2)

    def test_readonly_failure_does_not_send_an_alert(self):
        with (
            patch.object(
                publisher, "work", side_effect=upstream.IntegrationError("test failure")
            ),
            patch.object(publisher, "notify", side_effect=AssertionError("readonly")),
        ):
            self.assertEqual(publisher.run(self.state, publish=False), 1)

    def test_supervised_early_storage_error_leaves_alert_to_failure_service(self):
        with (
            patch.dict(os.environ, {"KWIN_PUBLISHER_SERVICE": "1"}),
            patch.object(
                sys, "argv", ["publisher.py", "--publish", "--state", str(self.state)]
            ),
            patch.object(publisher, "run", side_effect=OSError("state inaccessible")),
            patch.object(
                publisher,
                "notify",
                side_effect=AssertionError("duplicate supervisor alert"),
            ),
        ):
            self.assertEqual(publisher.main(), 1)

    def test_failure_is_recorded_and_alerted_without_leaking_helper_output(self):
        with (
            patch.object(
                publisher, "work", side_effect=upstream.IntegrationError("test failure")
            ),
            patch.object(publisher, "notify", return_value=True) as notify,
        ):
            self.assertEqual(publisher.run(self.state, publish=True), 1)
        result = json.loads((self.state / "latest.json").read_text())
        self.assertEqual(result["status"], "error")
        self.assertEqual(result["message"], "test failure")
        self.assertTrue(result["alert_accepted"])
        self.assertEqual(notify.call_count, 1)
        retained = self.state / "runs" / result["run_id"] / "result.json"
        self.assertEqual(json.loads(retained.read_text()), result)
        self.assertEqual(self.state.stat().st_mode & 0o777, 0o700)

    def test_alert_failure_preserves_original_failure(self):
        with (
            patch.object(publisher, "work", return_value={"status": "conflict"}),
            patch.object(publisher, "notify", return_value=False),
        ):
            self.assertEqual(publisher.run(self.state, publish=True), 1)
        result = json.loads((self.state / "latest.json").read_text())
        self.assertEqual(result["status"], "conflict")
        self.assertFalse(result["alert_accepted"])

    def test_lock_contention_does_not_publish_or_overwrite_active_result(self):
        self.state.mkdir(mode=0o700)
        latest = self.state / "latest.json"
        latest.write_text('{"status":"running"}\n')
        with (self.state / "run.lock").open("a") as lock:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            with patch.object(
                publisher, "work", side_effect=AssertionError("must not run")
            ):
                self.assertEqual(publisher.run(self.state, publish=True), 0)
        self.assertEqual(latest.read_text(), '{"status":"running"}\n')

    def test_fresh_repository_ignores_global_hooks_drivers_and_source_config(self):
        marker = self.root / "POISON_EXECUTED"
        global_config = self.root / ".gitconfig"
        global_config.write_text(
            f'[init]\n templateDir = /nonexistent\n[core]\n hooksPath = /nonexistent\n[merge "poison"]\n driver = touch {marker}\n'
        )
        with (
            patch.dict(
                os.environ,
                {
                    "HOME": str(self.root),
                    "GIT_CONFIG_COUNT": "1",
                    "GIT_CONFIG_KEY_0": "alias.init",
                    "GIT_CONFIG_VALUE_0": "!false",
                },
            ),
            publisher.fresh_repository(self.root) as repo,
        ):
            self.assertEqual(
                upstream.git(repo, "rev-parse", "--is-bare-repository").stdout.strip(),
                "true",
            )
            result = upstream.git(
                repo, "config", "--get", "merge.poison.driver", check=False
            )
            self.assertEqual(result.returncode, 1)
            self.assertEqual(
                upstream.git(repo, "config", "--get", "core.hooksPath").stdout.strip(),
                "/dev/null",
            )
        self.assertFalse(marker.exists())

    def test_real_no_update_check_leaves_remote_refs_unchanged(self):
        fixture = test_upstream.IntegrationTest()
        fixture.setUp()
        self.addCleanup(fixture.doCleanups)
        fixture.git("branch", publisher.MAIN, fixture.fork)
        fixture.git("branch", "Plasma/6.8", fixture.base)
        refs = fixture.git("show-ref")
        with (
            patch.object(publisher, "REMOTE", str(fixture.repo)),
            patch.object(publisher, "UPSTREAM", str(fixture.repo)),
        ):
            result = publisher.work(self.root, publish=False)
        self.assertEqual(result["status"], "current")
        self.assertEqual(fixture.git("show-ref"), refs)

    def test_readonly_clean_update_does_not_sign_or_publish(self):
        fixture = test_upstream.IntegrationTest()
        fixture.setUp()
        self.addCleanup(fixture.doCleanups)
        tip = fixture.upstream_commit(fixture.base, "library", "updated\n")
        fixture.git("branch", publisher.MAIN, fixture.fork)
        fixture.git("branch", "Plasma/6.8", tip)
        refs = fixture.git("show-ref")
        with (
            patch.object(publisher, "REMOTE", str(fixture.repo)),
            patch.object(publisher, "UPSTREAM", str(fixture.repo)),
            patch.object(
                publisher, "sign_candidate", side_effect=AssertionError("read-only")
            ),
        ):
            result = publisher.work(self.root, publish=False)
        self.assertEqual(result["status"], "ready")
        self.assertEqual(fixture.git("show-ref"), refs)


if __name__ == "__main__":
    unittest.main()
