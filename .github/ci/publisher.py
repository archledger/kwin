# SPDX-FileCopyrightText: 2026 Wisbendji Fimerlus <archledger236@gmail.com>
# SPDX-License-Identifier: GPL-2.0-or-later
"""Publish signed stable-update proposals; never merge or run fetched code."""

import argparse
import fcntl
import json
import os
import re
import stat
import subprocess
import tempfile
import uuid
from contextlib import contextmanager
from dataclasses import asdict
from datetime import datetime, timezone
from pathlib import Path, PurePosixPath

from publisher_report import atomic_json, notify, private_directory
from upstream import (
    MANIFEST,
    UPSTREAM,
    Integration,
    IntegrationError,
    commit_oid,
    git,
    integrate,
    read_manifest,
)

REPOSITORY = "archledger/kwin"
REMOTE = "https://github.com/archledger/kwin.git"
MAIN = "plasma-fusion"
NAME = "Wisbendji Fimerlus"
EMAIL = "archledger236@gmail.com"
SIGNER = "F35053398E3C80FE20891B82C10B8492BD7F30C6"


def branch_name(result: Integration) -> str:
    return f"sync/weekly-{result.fork}-{result.upstream}"


def commit_message(result: Integration) -> str:
    return (
        f"sync: Integrate stable upstream {result.upstream[:12]}\n\n"
        f"Apply the stable source changes from {result.base}\n"
        f"through {result.upstream}, preserving the fork delta.\n"
        f"Parent: {result.fork}\n"
        "Advance the upstream manifest in the same source commit.\n\n"
        f"Signed-off-by: {NAME} <{EMAIL}>\n"
    )


def validate_candidate(repo, result: Integration):
    if result.status != "clean" or not result.tree:
        raise IntegrationError("Only a clean integration can be published")
    # Disable rename detection so a control-file deletion cannot hide in a rename.
    paths = git(
        repo, "diff", "--name-only", "--no-renames", "-z", result.fork, result.tree
    ).stdout.split("\0")
    for path in filter(None, paths):
        if path == MANIFEST:
            continue
        parts = PurePosixPath(path).parts
        if parts[0].lower() == ".github" or any(
            part.lower() in {".gitattributes", ".gitmodules"} for part in parts
        ):
            raise IntegrationError(
                f"Imported control path requires manual review: {path}"
            )
    expected = read_manifest(repo, result.fork)
    expected["stable_commit"] = result.upstream
    # read_manifest also checks shape, duplicate keys, repository and branch types.
    if read_manifest(repo, result.tree) != expected:
        raise IntegrationError("Candidate manifest differs from the generated baseline")
    entry = git(repo, "ls-tree", result.tree, "--", MANIFEST).stdout
    if not entry.startswith("100644 blob "):
        raise IntegrationError("Candidate manifest is not a regular file")


def verify_candidate(repo, commit: str, result: Integration, signer: str = SIGNER):
    record = git(
        repo, "show", "-s", "--format=%T%n%P%n%an%n%ae%n%cn%n%ce%n%B", commit
    ).stdout
    expected = [result.tree, result.fork, NAME, EMAIL, NAME, EMAIL]
    lines = record.splitlines()
    if (
        lines[:6] != expected
        or "\n".join(lines[6:]).rstrip() != commit_message(result).rstrip()
    ):
        raise IntegrationError(
            "Existing candidate parent/tree/identity/provenance mismatch"
        )
    git(
        repo,
        "-c",
        "gpg.program=/usr/bin/gpg",
        "-c",
        "gpg.format=openpgp",
        "verify-commit",
        "--raw",
        commit,
    )
    fingerprint = git(
        repo,
        "-c",
        "gpg.program=/usr/bin/gpg",
        "-c",
        "gpg.format=openpgp",
        "show",
        "-s",
        "--format=%GF",
        commit,
    ).stdout.strip()
    if fingerprint != signer:
        raise IntegrationError("Candidate was not signed by the approved signing key")


def sign_candidate(repo, result: Integration, signer: str = SIGNER) -> str:
    validate_candidate(repo, result)
    commit = git(
        repo,
        "-c",
        f"user.name={NAME}",
        "-c",
        f"user.email={EMAIL}",
        "-c",
        "gpg.program=/usr/bin/gpg",
        "-c",
        "gpg.format=openpgp",
        "commit-tree",
        "-S" + signer,
        result.tree,
        "-p",
        result.fork,
        input=commit_message(result),
    ).stdout.strip()
    verify_candidate(repo, commit, result, signer)
    return commit


def remote_commit(repo, remote: str, ref: str) -> str | None:
    lines = git(repo, "ls-remote", "--refs", remote, ref).stdout.splitlines()
    if not lines:
        return None
    if len(lines) != 1 or not re.fullmatch(
        r"[0-9a-f]{40}\t" + re.escape(ref), lines[0]
    ):
        raise IntegrationError("Unexpected remote reference response")
    return lines[0].split("\t")[0]


def fetch_commit(repo, remote: str, ref: str) -> str:
    git(repo, "fetch", "--no-tags", remote, ref)
    return commit_oid(repo, git(repo, "rev-parse", "FETCH_HEAD").stdout.strip())


def create_branch(repo, remote: str, ref: str, commit: str):
    # git-push(1): empty <expect> requires the ref NOT to exist. Never replace it.
    git(repo, "push", "--force-with-lease=" + ref + ":", remote, commit + ":" + ref)
    if remote_commit(repo, remote, ref) != commit:
        raise IntegrationError("Published branch readback mismatch")


def checked_pr(pulls, branch: str, commit: str) -> dict:
    if not isinstance(pulls, list) or len(pulls) != 1:
        raise IntegrationError("Expected exactly one candidate pull request")
    pr = pulls[0]
    if not isinstance(pr, dict) or not isinstance(pr.get("headRepositoryOwner"), dict):
        raise IntegrationError("Malformed candidate pull request")
    if (
        pr.get("state") != "OPEN"
        or pr.get("headRefOid") != commit
        or pr.get("headRefName") != branch
        or pr.get("baseRefName") != MAIN
        or pr.get("headRepositoryOwner", {}).get("login") != "archledger"
        or pr.get("isCrossRepository") is not False
        or type(pr.get("number")) is not int
        or pr["number"] <= 0
        or pr.get("url") != f"https://github.com/{REPOSITORY}/pull/{pr['number']}"
    ):
        raise IntegrationError("Closed or mismatched candidate pull request")
    return pr


def propose(repo, result: Integration, forge, signer: str = SIGNER) -> dict:
    validate_candidate(repo, result)
    branch = branch_name(result)
    ref = "refs/heads/" + branch

    def check_main():
        if remote_commit(repo, forge.remote, "refs/heads/" + MAIN) != result.fork:
            raise IntegrationError("Fork main moved; prepare a fresh proposal")

    check_main()
    existing = remote_commit(repo, forge.remote, ref)
    pulls = forge.prs(branch)  # All states, including rejected/closed proposals.
    if existing:
        if fetch_commit(repo, forge.remote, ref) != existing:
            raise IntegrationError("Candidate ref changed during reconciliation")
        verify_candidate(repo, existing, result, signer)
        commit = existing
        if pulls:
            checked_pr(pulls, branch, commit)
    else:
        if pulls:
            raise IntegrationError("Pull request exists but its branch is absent")
        commit = sign_candidate(repo, result, signer)
        check_main()
        create_branch(repo, forge.remote, ref, commit)
    if not pulls:
        check_main()
        forge.create(branch, result, commit)
    pr = checked_pr(forge.prs(branch), branch, commit)
    check_main()
    if remote_commit(repo, forge.remote, ref) != commit:
        raise IntegrationError("Candidate ref changed after publication")
    return {
        "status": "existing" if pulls else "proposed",
        "branch": branch,
        "commit": commit,
        "pr": pr["number"],
        "url": pr["url"],
    }


def command(args, *, input=None, cwd=None):
    try:
        result = subprocess.run(
            args,
            input=input,
            cwd=cwd,
            text=True,
            capture_output=True,
            timeout=120,
            check=False,
        )
    except (OSError, subprocess.TimeoutExpired) as error:
        raise IntegrationError("External command unavailable or timed out") from error
    if result.returncode:
        # Authentication helpers must never put their output into reports/alerts.
        raise IntegrationError(f"{args[0]} {args[1]} failed (exit {result.returncode})")
    return result.stdout


class GitHub:
    remote = REMOTE

    def __init__(self, repo):
        self.repo = repo

    def prs(self, branch):
        output = command(
            [
                "/usr/bin/gh",
                "pr",
                "list",
                "--repo",
                "github.com/" + REPOSITORY,
                "--head",
                branch,
                "--state",
                "all",
                "--limit",
                "100",
                "--json",
                "number,state,headRefOid,headRefName,baseRefName,headRepositoryOwner,isCrossRepository,url",
            ],
            cwd=self.repo,
        )
        pulls = json.loads(output)
        if (
            not isinstance(pulls, list)
            or len(pulls) >= 100
            or any(not isinstance(pr, dict) for pr in pulls)
        ):
            raise IntegrationError("Incomplete or invalid pull request inventory")
        return pulls

    def create(self, branch, result, commit):
        body = (
            "## Summary\n\n"
            f"Integrate stable upstream `{result.base}` through `{result.upstream}` "
            f"on fork parent `{result.fork}`.\n\n"
            f"Candidate: `{commit}`\n\nTree: `{result.tree}`\n\n"
            "The upstream baseline advances with the source changes. "
            "Imported CI/control-path changes are refused by the publisher.\n\n"
            "## Validation\n\n"
            "Exact parent, tree, provenance, owner signature and published ref verified. "
            "Native qualification and source review are pending in this PR. "
            "Merge requires owner approval after the required checks pass.\n"
        )
        command(
            [
                "/usr/bin/gh",
                "pr",
                "create",
                "--repo",
                "github.com/" + REPOSITORY,
                "--base",
                MAIN,
                "--head",
                "archledger:" + branch,
                "--draft",
                "--title",
                f"sync: Update stable upstream to {result.upstream[:12]}",
                "--body-file",
                "-",
            ],
            input=body,
            cwd=self.repo,
        )


@contextmanager
def fresh_repository(parent: Path):
    # No checkout, source repository config, templates, hooks or custom merge drivers.
    with tempfile.TemporaryDirectory(prefix="repository-", dir=parent) as directory:
        repo = Path(directory)
        git(repo, "init", "--bare", "--template=", "--initial-branch=" + MAIN)
        for key, value in {
            "core.attributesFile": os.devnull,
            "transfer.fsckObjects": "true",
            "fetch.fsckObjects": "true",
            "credential.helper": "!/usr/bin/gh auth git-credential",
        }.items():
            git(repo, "config", key, value)
        yield repo


def work(directory: Path, *, publish: bool) -> dict:
    with fresh_repository(directory) as repo:
        fork = fetch_commit(repo, REMOTE, "refs/heads/" + MAIN)
        manifest = read_manifest(repo, fork)
        tip = fetch_commit(repo, UPSTREAM, manifest["stable_ref"])
        result = integrate(repo, fork, tip, "stable")
        report = asdict(result)
        atomic_json(directory / "integration.json", report, max_bytes=None)
        report.pop("diagnostics")
        if result.status != "clean":
            return report
        validate_candidate(repo, result)
        if publish:
            report.update(propose(repo, result, GitHub(repo)))
        else:
            report["status"] = "ready"
        return report


def run(state: Path, *, publish: bool) -> int:
    private_directory(state)
    fd = os.open(state / "run.lock", os.O_CREAT | os.O_RDWR | os.O_NOFOLLOW, 0o600)
    with os.fdopen(fd, "a") as lock:
        if not stat.S_ISREG(os.fstat(lock.fileno()).st_mode):
            raise IntegrationError("Invalid publisher lock")
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            print(json.dumps({"status": "busy"}))
            return 0
        private_directory(state / "runs")
        run_id = (
            os.environ.get("INVOCATION_ID", "")
            if os.environ.get("KWIN_PUBLISHER_SERVICE") == "1"
            else uuid.uuid4().hex
        )
        if not re.fullmatch(r"[0-9a-f]{32}", run_id):
            raise IntegrationError("Invalid service invocation identity")
        directory = state / "runs" / run_id
        directory.mkdir(mode=0o700)
        report = {
            "run_id": run_id,
            "time": datetime.now(timezone.utc).isoformat(),
            "publish": publish,
            "status": "running",
        }
        atomic_json(directory / "result.json", report)
        atomic_json(state / "latest.json", report)
        try:
            report.update(work(directory, publish=publish))
        except (IntegrationError, OSError, ValueError) as error:
            report.update(status="error", message=str(error)[:4096])
        failed = report["status"] not in {"current", "ready", "existing", "proposed"}
        # Persist the failure before attempting a network notification.
        atomic_json(directory / "result.json", report)
        atomic_json(state / "latest.json", report)
        if failed and publish:
            report["alert_accepted"] = notify(run_id)
            atomic_json(directory / "result.json", report)
            atomic_json(state / "latest.json", report)
        print(json.dumps(report))
        return 1 if failed else 0


def main() -> int:
    # Retain only the existing user-service identity/authentication environment.
    allowed = {
        "HOME",
        "USER",
        "LOGNAME",
        "XDG_RUNTIME_DIR",
        "DBUS_SESSION_BUS_ADDRESS",
        "GNUPGHOME",
        "GH_TOKEN",
        "GITHUB_TOKEN",
        "INVOCATION_ID",
        "KWIN_PUBLISHER_SERVICE",
    }
    environment = {key: value for key, value in os.environ.items() if key in allowed}
    os.environ.clear()
    os.environ.update(
        environment,
        PATH="/usr/bin:/bin",
        LANG="C.UTF-8",
        GH_PROMPT_DISABLED="1",
        GH_HOST="github.com",
    )
    parser = argparse.ArgumentParser(description=__doc__)
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument(
        "--check", action="store_true", help="prepare only; no signing or remote writes"
    )
    group.add_argument(
        "--publish",
        action="store_true",
        help="create or reuse a signed stable-update PR",
    )
    group.add_argument(
        "--test-alert",
        action="store_true",
        help="send one clearly marked alert via the existing channel",
    )
    parser.add_argument(
        "--state",
        type=Path,
        default=Path.home() / ".local/state/kwin-upstream-publisher",
    )
    args = parser.parse_args()
    try:
        if args.test_alert:
            private_directory(args.state)
            result = {"run_id": str(uuid.uuid4()), "test": True}
            result["alert_accepted"] = notify(result["run_id"], test=True)
            atomic_json(args.state / "test-alert.json", result)
            print(json.dumps(result))
            return 0 if result["alert_accepted"] else 1
        return run(args.state, publish=args.publish)
    except (IntegrationError, OSError, ValueError):
        run_id = str(uuid.uuid4())
        print(
            json.dumps(
                {
                    "status": "local_state_error",
                    "run_id": run_id,
                    "alert_accepted": (
                        notify(run_id)
                        if args.publish
                        and os.environ.get("KWIN_PUBLISHER_SERVICE") != "1"
                        else False
                    ),
                }
            )
        )
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
