# SPDX-FileCopyrightText: 2026 Wisbendji Fimerlus <archledger236@gmail.com>
# SPDX-License-Identifier: GPL-2.0-or-later
"""Prepare upstream integration trees without changing a checkout or remote."""

import argparse
import json
import os
import re
import subprocess
import sys
import tempfile
from dataclasses import asdict, dataclass
from pathlib import Path

UPSTREAM = "https://github.com/KDE/kwin.git"
MANIFEST = ".github/upstream.json"


class IntegrationError(RuntimeError):
    pass


@dataclass(frozen=True)
class Integration:
    status: str
    base: str
    fork: str
    upstream: str
    tree: str | None
    diagnostics: str = ""


def git(
    repo: Path,
    *args: str,
    input: str | bytes | None = None,
    index: Path | None = None,
    check: bool = True,
    binary: bool = False,
) -> subprocess.CompletedProcess:
    env = {
        key: value for key, value in os.environ.items() if not key.startswith("GIT_")
    }
    env.update(
        GIT_CONFIG_NOSYSTEM="1", GIT_CONFIG_GLOBAL=os.devnull, GIT_TERMINAL_PROMPT="0"
    )
    if index is not None:
        env["GIT_INDEX_FILE"] = str(index)
    try:
        result = subprocess.run(
            [
                "git",
                "-c",
                "core.hooksPath=" + os.devnull,
                "-c",
                "core.fsmonitor=false",
                "-c",
                "commit.gpgsign=false",
                "-C",
                str(repo),
                *args,
            ],
            input=input,
            text=not binary,
            encoding=None if binary else "utf-8",
            errors=None if binary else "replace",
            capture_output=True,
            env=env,
            timeout=120,
            check=False,
        )
    except (OSError, subprocess.TimeoutExpired) as error:
        raise IntegrationError("Git execution unavailable or timed out") from error
    if check and result.returncode:
        raise IntegrationError(
            f"git {args[0]} failed ({result.returncode}): {result.stderr[:2000]}"
        )
    return result


def commit_oid(repo: Path, value: str) -> str:
    if not isinstance(value, str) or not re.fullmatch(r"[0-9a-f]{40}", value):
        raise IntegrationError("Expected a full commit SHA")
    resolved = git(repo, "rev-parse", "--verify", value + "^{commit}").stdout.strip()
    if resolved != value:
        raise IntegrationError("Object is not the specified commit")
    return value


def unique_keys(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise IntegrationError("Duplicate manifest key")
        result[key] = value
    return result


def read_manifest(repo: Path, fork: str) -> dict:
    raw = git(repo, "show", fork + ":" + MANIFEST).stdout
    if len(raw) > 4096:
        raise IntegrationError("Manifest is too large")
    try:
        manifest = json.loads(raw, object_pairs_hook=unique_keys)
    except (ValueError, TypeError) as error:
        raise IntegrationError("Invalid upstream manifest") from error
    keys = {"schema", "repository", "stable_ref", "stable_commit", "preview_ref"}
    if not isinstance(manifest, dict) or set(manifest) != keys:
        raise IntegrationError("Unexpected upstream manifest fields")
    if type(manifest["schema"]) is not int or manifest["schema"] != 1:
        raise IntegrationError("Unsupported manifest schema")
    if (
        manifest["repository"] != UPSTREAM
        or manifest["preview_ref"] != "refs/heads/master"
    ):
        raise IntegrationError("Unexpected upstream repository or preview branch")
    if not isinstance(manifest["stable_ref"], str) or not re.fullmatch(
        r"refs/heads/Plasma/[0-9]+\.[0-9]+", manifest["stable_ref"]
    ):
        raise IntegrationError("Invalid stable branch")
    if not isinstance(manifest["stable_commit"], str) or not re.fullmatch(
        r"[0-9a-f]{40}", manifest["stable_commit"]
    ):
        raise IntegrationError("Invalid upstream baseline SHA")
    return manifest


def update_manifest(repo: Path, tree: str, manifest: dict) -> str:
    blob = git(
        repo,
        "hash-object",
        "-w",
        "--stdin",
        input=json.dumps(manifest, indent=2) + "\n",
    ).stdout.strip()
    # This index is private; no checkout, filters or tracked build scripts execute here.
    with tempfile.TemporaryDirectory(prefix="kwin-upstream-index-") as directory:
        index = Path(directory) / "index"
        git(repo, "read-tree", tree, index=index)
        git(
            repo,
            "update-index",
            "--add",
            "--cacheinfo",
            "100644",
            blob,
            MANIFEST,
            index=index,
        )
        return git(repo, "write-tree", index=index).stdout.strip()


def integrate(repo: Path, fork: str, upstream: str, track: str) -> Integration:
    fork, upstream = commit_oid(repo, fork), commit_oid(repo, upstream)
    if track not in {"stable", "preview"}:
        raise IntegrationError("Unknown integration track")
    manifest = read_manifest(repo, fork)
    base = commit_oid(repo, manifest["stable_commit"])
    if track == "stable":
        ancestry = git(repo, "merge-base", "--is-ancestor", base, upstream, check=False)
        if ancestry.returncode != 0:
            raise IntegrationError(
                "Stable upstream does not descend from the recorded baseline"
            )
        if base == upstream:
            tree = git(repo, "rev-parse", fork + "^{tree}").stdout.strip()
            return Integration("current", base, fork, upstream, tree)
    merged = git(
        repo,
        "merge-tree",
        "--write-tree",
        "--name-only",
        "--merge-base=" + base,
        fork,
        upstream,
        check=False,
    )
    if merged.returncode == 1:
        return Integration(
            "conflict", base, fork, upstream, None, merged.stdout[:65536]
        )
    if merged.returncode:
        raise IntegrationError(
            f"Merge execution failed ({merged.returncode}): {merged.stderr[:2000]}"
        )
    tree = merged.stdout.splitlines()[0]
    if (
        not re.fullmatch(r"[0-9a-f]{40}", tree)
        or git(repo, "cat-file", "-t", tree).stdout.strip() != "tree"
    ):
        raise IntegrationError("Merge did not produce a valid tree")
    if track == "stable":
        manifest["stable_commit"] = upstream
        tree = update_manifest(repo, tree, manifest)
    return Integration("clean", base, fork, upstream, tree, merged.stdout[:65536])


def materialize(repo: Path, result: Integration, destination: Path) -> str:
    if (
        result.status != "clean"
        or not result.tree
        or not re.fullmatch(r"[0-9a-f]{40}", result.tree)
    ):
        raise IntegrationError("Only a clean integration can be materialized")
    if destination.exists() or destination.is_symlink():
        raise IntegrationError("Candidate destination already exists")
    commit_oid(repo, result.fork)
    commit = git(
        repo,
        "-c",
        "user.name=KWin integration check",
        "-c",
        "user.email=integration@example.invalid",
        "commit-tree",
        result.tree,
        "-p",
        result.fork,
        input="Disposable upstream integration candidate\n",
    ).stdout.strip()
    commit_oid(repo, commit)
    if git(repo, "rev-parse", commit + "^{tree}").stdout.strip() != result.tree:
        raise IntegrationError("Candidate commit has the wrong tree")
    git(repo, "worktree", "add", "--detach", str(destination.absolute()), commit)
    return commit


def project_version(source: Path) -> str:
    path = source / "CMakeLists.txt"
    if path.is_symlink() or path.stat().st_size > 1024 * 1024:
        raise IntegrationError("Unsafe CMake version metadata")
    assignments = re.findall(
        r"^\s*set\s*\(\s*PROJECT_VERSION\b([^\n]*)", path.read_text(), re.MULTILINE
    )
    if len(assignments) != 1:
        raise IntegrationError("Expected one literal project version")
    match = re.fullmatch(
        r'\s+"([0-9]+\.[0-9]+\.[0-9]+)"\s*\)\s*(?:#.*)?', assignments[0]
    )
    if not match:
        raise IntegrationError("Expected a numeric literal project version")
    return match.group(1)


def fetch_tip(repo: Path, ref: str) -> str:
    if ref != "refs/heads/master" and not re.fullmatch(
        r"refs/heads/Plasma/[0-9]+\.[0-9]+", ref
    ):
        raise IntegrationError("Unexpected upstream ref")
    git(repo, "fetch", "--no-tags", UPSTREAM, ref)
    return commit_oid(repo, git(repo, "rev-parse", "FETCH_HEAD").stdout.strip())


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    version = commands.add_parser("version")
    version.add_argument("source", type=Path)
    prepare = commands.add_parser("prepare")
    prepare.add_argument("--repo", type=Path, required=True)
    prepare.add_argument("--fork")
    prepare.add_argument("--upstream")
    prepare.add_argument("--track", choices=("stable", "preview"), required=True)
    prepare.add_argument("--output", type=Path, required=True)
    prepare.add_argument("--source-dir", type=Path)
    args = parser.parse_args()
    if args.command == "version":
        try:
            print(project_version(args.source))
            return 0
        except (IntegrationError, OSError) as error:
            print(error, file=sys.stderr)
            return 1
    try:
        args.output.mkdir(parents=True, exist_ok=False)
    except OSError as error:
        print(f"Cannot create fresh result directory: {error}", file=sys.stderr)
        return 1
    try:
        fork = commit_oid(
            args.repo, args.fork or git(args.repo, "rev-parse", "HEAD").stdout.strip()
        )
        manifest = read_manifest(args.repo, fork)
        if args.upstream:
            tip = commit_oid(args.repo, args.upstream)
        else:
            stable = fetch_tip(args.repo, manifest["stable_ref"])
            base = commit_oid(args.repo, manifest["stable_commit"])
            if git(
                args.repo, "merge-base", "--is-ancestor", base, stable, check=False
            ).returncode:
                raise IntegrationError(
                    "Recorded baseline is not in the current stable history"
                )
            tip = (
                stable
                if args.track == "stable"
                else fetch_tip(args.repo, manifest["preview_ref"])
            )
        result = integrate(args.repo, fork, tip, args.track)
        report = asdict(result)
        (args.output / "merge.log").write_text(result.diagnostics)
        if result.status == "clean":
            (args.output / "candidate.patch").write_bytes(
                git(
                    args.repo,
                    "diff",
                    "--binary",
                    "--no-ext-diff",
                    "--no-textconv",
                    fork,
                    result.tree,
                    binary=True,
                ).stdout
            )
            if args.source_dir:
                report["candidate_commit"] = materialize(
                    args.repo, result, args.source_dir
                )
        code = 2 if result.status == "conflict" else 0
    except (IntegrationError, OSError, ValueError) as error:
        report = {"status": "error", "message": str(error)}
        code = 1
    (args.output / "result.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report))
    return code


if __name__ == "__main__":
    raise SystemExit(main())
