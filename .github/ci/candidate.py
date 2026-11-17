# SPDX-FileCopyrightText: 2026 Wisbendji Fimerlus <archledger236@gmail.com>
# SPDX-License-Identifier: GPL-2.0-or-later
"""The fork's candidate for a new KDE KWin point release: the fork's delta replayed onto the tag.

Run from a checkout of the fork's default branch (the trusted tools), never from candidate source.

  candidate.py mirror --repo DIR --tag vX.Y.Z [--push] [--output FILE]
      The fork's Plasma/X.Y mirror is KDE's branch, and must contain the tag before a candidate
      is pushed: fork CI counts every commit above the Plasma/* mirrors as a fork commit. KDE's
      branch and tag are fetched from invent.kde.org; the local tag must be KDE's; a mirror
      behind KDE's branch moves to it by a fast forward (a plain push, never a forced one), and
      a mirror with a commit KDE lacks is an error.
  candidate.py replay --repo DIR --previous REF [--base vX.Y.Z] --tag vX.Y.Z --pf N --sign KEY
                      [--output FILE]
      The previous candidate's commits (base tag..REF) replayed onto the new tag in a temporary
      worktree, each commit signed with KEY. The result's checks:
      - every path git merges cleanly from the previous candidate and the new tag (git
        merge-tree over the base tag) is that merge's version exactly;
      - for every path, the lines the result changes against the previous candidate are KDE's
        change between the two tags, and no replayed commit changes more lines than its
        original did (lists of lines per path, without their positions: the only check of the
        paths that merge-tree cannot merge);
      - no KDE commit of the branch after the new tag is in the result (a later stable fix
        carried early), and no KDE change between the tags touches .github, .gitattributes or
        .gitmodules (it would run on the fork's CI unreviewed);
      - every commit has fork CI's commit rules, a good signature by KEY and the clone's
        user.email as committer.
      The commit is kept under refs/candidate-pipeline/; no branch moves.
  candidate.py push --repo DIR --branch candidate/X.Y.Z-pfN --commit SHA [--sign KEY]
                    [--output FILE]
      Creates the candidate branch on the fork. An existing branch is never moved: it is kept
      when it holds the same candidate (commit by commit the same message, author and tree; a
      replay made again differs only in its commit times) and, with KEY, its commits pass the
      commit rules; anything else under the name is an error. Prints the branch's commit.
"""

import argparse
import json
import os
import re
import subprocess
import sys
import tempfile
from dataclasses import asdict, dataclass, field
from pathlib import Path

KDE = "https://invent.kde.org/plasma/kwin.git"
NAMESPACE = "refs/candidate-pipeline"
VERSION = r"[0-9]+\.[0-9]+\.[0-9]+(?:\.[0-9]+)?"
TAG = re.compile(r"v([0-9]+)\.([0-9]+)\.([0-9]+)(?:\.([0-9]+))?")
BRANCH = re.compile(rf"candidate/{VERSION}-pf[1-9][0-9]*")
# Files that steer git or GitHub: a KDE change to one is held for review.
CONTROL = re.compile(r"(\.github/.*|(.*/)?\.gitattributes|\.gitmodules)", re.IGNORECASE)
# A signing key, as git and gpg name it in a signature: the full fingerprint.
FINGERPRINT = re.compile(r"[0-9A-Fa-f]{40}")
# The same rules as fork CI's commits job (.github/workflows/fork-ci.yml).
SIGNOFF = re.compile(r"Signed-off-by: .+ <.+@.+>")
ATTRIBUTION = re.compile(
    r"claude|anthropic|openai|chatgpt|copilot|codex|opencode|gemini"
    r"|generated (with|by) (an? )?ai|ai[- ]generated|co-authored-by",
    re.IGNORECASE,
)


class CandidateError(RuntimeError):
    pass


def git(repo, *args, check=True, timeout=120, input=None, index=None):
    env = {
        key: value for key, value in os.environ.items() if not key.startswith("GIT_")
    }
    env.update(
        GIT_CONFIG_NOSYSTEM="1", GIT_CONFIG_GLOBAL=os.devnull, GIT_TERMINAL_PROMPT="0"
    )
    if index is not None:
        env["GIT_INDEX_FILE"] = str(index)
    # The user's configuration stays out; a push to GitHub names its credential helper here
    # (CANDIDATE_GIT_CREDENTIAL_HELPER, e.g. "!gh auth git-credential").
    helper = os.environ.get("CANDIDATE_GIT_CREDENTIAL_HELPER", "")
    credentials = (
        ["-c", "credential.helper=", "-c", "credential.helper=" + helper]
        if helper
        else []
    )
    # The clone's own configuration still applies: nothing in it may change what is parsed
    # (colour) or how a signature is made and read.
    command = [
        "git",
        "-c",
        "core.hooksPath=" + os.devnull,
        "-c",
        "core.fsmonitor=false",
        "-c",
        "commit.gpgsign=false",
        "-c",
        "rerere.enabled=false",
        "-c",
        "color.ui=never",
        "-c",
        "diff.ignoreSubmodules=none",
        "-c",
        "gpg.format=openpgp",
        "-c",
        "gpg.program=gpg",
        "-c",
        "log.showSignature=false",
        "-c",
        "core.autocrlf=false",
        "-c",
        "apply.whitespace=nowarn",
        "-c",
        "apply.ignoreWhitespace=no",
        *credentials,
        "-C",
        str(repo),
        *args,
    ]
    # Bytes in and out, decoded by hand: Python's text mode would turn \r\n into \n, and a path
    # that is not UTF-8 keeps its bytes (surrogateescape) when it is passed back to git.
    try:
        raw = subprocess.run(
            command,
            capture_output=True,
            env=env,
            timeout=timeout,
            check=False,
            input=None if input is None else input.encode("utf-8", "surrogateescape"),
        )
    except (OSError, subprocess.TimeoutExpired) as error:
        raise CandidateError(f"git {args[0]} unavailable or timed out") from error
    result = subprocess.CompletedProcess(
        raw.args,
        raw.returncode,
        raw.stdout.decode("utf-8", "surrogateescape"),
        raw.stderr.decode("utf-8", "surrogateescape"),
    )
    if check and result.returncode:
        raise CandidateError(
            f"git {args[0]} failed ({result.returncode}): {result.stderr[:2000]}"
        )
    return result


def resolve(repo, name):
    result = git(
        repo, "rev-parse", "--verify", "-q", "--end-of-options", name, check=False
    )
    value = result.stdout.strip()
    if result.returncode or not re.fullmatch(r"[0-9a-f]{40}", value):
        raise CandidateError(f"{name} does not name an object")
    return value


def is_ancestor(repo, older, newer):
    return (
        git(repo, "merge-base", "--is-ancestor", older, newer, check=False).returncode
        == 0
    )


def parse_tag(tag):
    """(major, minor, patch[, re-release]) of a KDE point release tag: v6.8.3, v6.3.2.1."""
    match = TAG.fullmatch(tag) if isinstance(tag, str) else None
    if not match:
        raise CandidateError(f"{tag!r} is not a KDE point release tag (vX.Y.Z)")
    version = tuple(int(part) for part in match.groups() if part is not None)
    if version[2] >= 80:
        raise CandidateError(
            f"{tag} is a pre-release of the next series, not a point release"
        )
    return version


def check_step(base, tag):
    old, new = parse_tag(base), parse_tag(tag)
    if old[:2] != new[:2]:
        raise CandidateError(
            f"{base} -> {tag} starts a new series: that is a manual rebase"
        )
    if new <= old:
        raise CandidateError(f"{tag} is not newer than {base}")


def candidate_branch(tag, pf):
    parse_tag(tag)
    if type(pf) is not int or pf < 1:
        raise CandidateError("the fork's sequence number is a positive integer")
    return f"candidate/{tag[1:]}-pf{pf}"


def base_of(previous):
    """The tag a candidate branch is named after: candidate/6.8.2-pf5 -> v6.8.2."""
    match = re.fullmatch(
        rf"(?:refs/remotes/[^/]+/|[^/]+/)?candidate/({VERSION})-pf[0-9]+", previous
    )
    if not match:
        raise CandidateError(f"cannot tell the base tag of {previous}; give --base")
    return "v" + match.group(1)


def committer_email(repo):
    """The committer a replay writes: the clone's own user.email (the user's is kept out)."""
    email = git(repo, "config", "--get", "user.email", check=False).stdout.strip()
    if not email:
        raise CandidateError(
            f"{repo} has no user.email of its own; set it with git config user.email"
        )
    return email


def key_emails(sign):
    """The addresses of the signing key's user IDs, as the host's gpg has them."""
    try:
        listing = subprocess.run(
            ["gpg", "--batch", "--with-colons", "--list-keys", sign],
            capture_output=True,
            timeout=60,
            check=False,
        )
    except (OSError, subprocess.TimeoutExpired) as error:
        raise CandidateError("gpg is unavailable or timed out") from error
    if listing.returncode:
        raise CandidateError(f"gpg does not know the key {sign}")
    emails = set()
    for line in listing.stdout.decode("utf-8", "replace").splitlines():
        fields = line.split(":")
        if fields[0] == "uid" and len(fields) > 9:
            uid = fields[9].replace("\\x3a", ":")
            emails.update(e.lower() for e in re.findall(r"<([^<>]+)>", uid))
    return emails


def signing_identity(repo, sign):
    """The committer address a signed replay writes, checked against the key: GitHub verifies
    a signature only when the committer's address is one of the key's."""
    email = committer_email(repo)
    if email.lower() not in key_emails(sign):
        raise CandidateError(
            f"the key {sign} has no user ID with {email} (the clone's user.email); "
            "GitHub would not verify its commits"
        )
    return email


def commit_problems(repo, start, end, sign=None, email=None):
    """Fork CI's commit rules for every commit in start..end, one line per failing commit.
    With sign (a key's fingerprint), each commit must carry a good signature by that key
    (primary or subkey) and, with email, that committer address."""
    problems = []
    listing = git(
        repo, "rev-list", "--reverse", "--no-merges", f"{start}..{end}"
    ).stdout.split()
    for commit in listing:
        reasons = []
        trailers = git(
            repo, "log", "-1", "--format=%(trailers:only=true,unfold=true)", commit
        ).stdout
        lines = [line for line in trailers.splitlines() if line]
        signoffs = sum(1 for line in lines if SIGNOFF.fullmatch(line))
        if signoffs != 1:
            reasons.append(f"{signoffs} Signed-off-by trailers (need exactly 1)")
        other = [line for line in lines if not line.startswith("Signed-off-by: ")]
        if other:
            reasons.append("other trailers: " + "; ".join(other))
        body = git(repo, "log", "-1", "--format=%B", commit).stdout
        hits = [line for line in body.splitlines() if ATTRIBUTION.search(line)]
        if hits:
            reasons.append("credits an AI tool: " + "; ".join(hits))
        if sign:
            status, fingerprint, primary, address = (
                git(repo, "log", "-1", "--format=%G?%x00%GF%x00%GP%x00%ce", commit)
                .stdout.rstrip("\n")
                .split("\0")
            )
            if status != "G" or sign.upper() not in (
                fingerprint.upper(),
                primary.upper(),
            ):
                reasons.append(
                    f"no good signature by {sign} (status {status}, key {fingerprint or 'none'})"
                )
            if email and address != email:
                reasons.append(f"committer {address}, not {email}")
        if reasons:
            subject = git(repo, "log", "-1", "--format=%h %s", commit).stdout.strip()
            problems.append(f"{subject}: " + "; ".join(reasons))
    return problems


def merge_tree(repo, base, previous, tag):
    """The previous candidate and the new tag merged over the base tag: (tree, conflicted paths).
    git writes the tree also when paths conflict; those are listed after it.

    A delta that imported upstream code early and then changed it conflicts here on those paths
    although the commit-by-commit replay is clean: the import and the fork's own lines next to it
    are one hunk against the base."""
    result = git(
        repo,
        "merge-tree",
        "--write-tree",
        "--name-only",
        "-z",
        "--no-messages",
        f"--merge-base={base}",
        previous,
        tag,
        check=False,
        timeout=600,
    )
    if result.returncode not in (0, 1):
        raise CandidateError(
            f"git merge-tree failed ({result.returncode}): {result.stderr[:2000]}"
        )
    fields = result.stdout.split("\0")
    tree = fields[0]
    if not re.fullmatch(r"[0-9a-f]{40}", tree):
        raise CandidateError("git merge-tree wrote no tree")
    conflicted = []
    for path in fields[1:]:
        if not path:
            break
        conflicted.append(path)
    if result.returncode == 1 and not conflicted:
        raise CandidateError("git merge-tree reported a conflict without a path")
    return tree, sorted(set(conflicted))


def tree_entries(repo, commit):
    """{path: (mode, object)} for every file, link and submodule in the commit's tree."""
    entries = {}
    out = git(repo, "ls-tree", "-r", "-z", "--full-tree", commit, timeout=600).stdout
    for record in out.split("\0"):
        if record:
            meta, path = record.split("\t", 1)
            mode, _, oid = meta.split()
            entries[path] = (mode, oid)
    return entries


def changed_paths(repo, old, new):
    out = git(
        repo,
        "diff",
        "--name-only",
        "-z",
        "--no-renames",
        "--ignore-submodules=none",
        old,
        new,
        timeout=600,
    ).stdout
    return sorted(path for path in out.split("\0") if path)


def line_changes(repo, old, new, path):
    """(added, removed) lines of path from old to new, as multisets; None for a binary change."""
    out = git(
        repo,
        "diff",
        "-U0",
        "--no-color",
        "--no-renames",
        "--no-ext-diff",
        "--no-textconv",
        "--ignore-submodules=none",
        old,
        new,
        "--",
        f":(literal){path}",
        timeout=600,
    ).stdout
    added, removed = {}, {}
    in_hunk = False
    for line in out.split("\n"):
        if line.startswith("diff --git "):
            in_hunk = False
        elif line.startswith("Binary files ") and not in_hunk:
            return None
        elif line.startswith("@@"):
            in_hunk = True
        elif in_hunk and line[:1] == "+":
            added[line[1:]] = added.get(line[1:], 0) + 1
        elif in_hunk and line[:1] == "-":
            removed[line[1:]] = removed.get(line[1:], 0) + 1
    return added, removed


def commit_changes(repo, parent, commit):
    """{path: (mode before, mode after, object before, object after)} of parent..commit."""
    out = git(
        repo,
        "diff-tree",
        "-r",
        "-z",
        "--no-renames",
        "--no-commit-id",
        "--ignore-submodules=none",
        parent,
        commit,
        timeout=600,
    ).stdout
    fields = out.split("\0")
    changes = {}
    for meta, path in zip(fields[0::2], fields[1::2]):
        mode_a, mode_b, oid_a, oid_b, _ = meta.lstrip(":").split()
        none = "0" * 40
        changes[path] = (
            None if mode_a == "000000" else mode_a,
            None if mode_b == "000000" else mode_b,
            None if oid_a == none else oid_a,
            None if oid_b == none else oid_b,
        )
    return changes


def beyond(repo, path, ours, ref, mode, entries, side):
    """The problem with one path's change (ours: here..there) that ref's change does not cover."""
    if mode[0] != mode[1] and (mode[0], mode[1]) != (mode[2], mode[3]):
        return f"{path}: mode or presence {mode[0]} -> {mode[1]}, {side}"
    text = {m for m in mode if m} <= {"100644", "100755"}
    here, there = ours
    ref_old, ref_new = ref
    changes = line_changes(repo, here, there, path) if text else None
    covered = line_changes(repo, ref_old, ref_new, path) if text else None
    if changes is None or covered is None:
        # A binary, a link or a submodule: only the same version on both sides.
        if entries[0] != entries[2] or entries[1] != entries[3]:
            return (
                f"{path}: a binary, link or submodule change, {side}; review it by hand"
            )
        return None
    extra_added = {
        line: n for line, n in changes[0].items() if covered[0].get(line, 0) < n
    }
    extra_removed = {
        line: n for line, n in changes[1].items() if covered[1].get(line, 0) < n
    }
    if extra_added or extra_removed:
        sample = [f"+{line}" for line in list(extra_added)[:3]] + [
            f"-{line}" for line in list(extra_removed)[:3]
        ]
        return (
            f"{path}: {len(extra_added)} added and {len(extra_removed)} removed line(s) {side}: "
            + " | ".join(sample)
        )
    return None


def unexplained(repo, base, previous, tag, new, paths=None):
    """The replay's policy, line by line, one problem line per path that breaks it:
    1. what the new candidate changes against the previous one is upstream's change between the
       two tags;
    2. each replayed commit changes no more than its original did. A replay may shrink a commit
       (the new tag already has a fix the delta imported early), never add to one; that keeps
       the fork's own changes of upstream code it imported early (they replay unchanged) and
       finds an upstream change a replay lost or a change from nowhere.
    The lines of a path are compared as lists, without their positions. paths limits rule 1:
    the replay gives it the paths git cannot merge, as every other path must be exactly the
    merge's version already."""
    problems = []
    trees = {name: tree_entries(repo, name) for name in (base, previous, tag, new)}
    side = "not upstream's change between the tags"
    for path in changed_paths(repo, previous, new):
        if paths is not None and path not in paths:
            continue
        entries = [trees[name].get(path) for name in (previous, new, base, tag)]
        mode = [entry[0] if entry else None for entry in entries]
        problem = beyond(repo, path, (previous, new), (base, tag), mode, entries, side)
        if problem:
            problems.append(problem)
    pairs, unmatched = pair(repo, base, previous, tag, new)
    for _, _, subject in unmatched:
        problems.append(
            f"{subject}: a replayed commit that is not in the previous candidate"
        )
    for (original, _, _), replayed in pairs:
        if replayed is not None:
            problems.extend(commit_beyond(repo, original, replayed))
    return problems


def commit_beyond(repo, original, commit):
    """Rule 2 for one replayed commit: the problems with what it changes beyond its original."""
    problems = []
    subject = git(repo, "log", "-1", "--format=%s", commit).stdout.strip()
    side = f"beyond the original commit {original[:10]} ({subject})"
    theirs = commit_changes(repo, original + "^", original)
    for path, change in commit_changes(repo, commit + "^", commit).items():
        covered = theirs.get(path, (change[0], change[0], change[2], change[2]))
        entries = [change[2], change[3], covered[2], covered[3]]
        mode = [change[0], change[1], covered[0], covered[1]]
        problem = beyond(
            repo,
            path,
            (commit + "^", commit),
            (original + "^", original),
            mode,
            entries,
            side,
        )
        if problem:
            problems.append(problem)
    return problems


def pair(repo, base, previous, tag, new):
    """Each original commit (base..previous) with the commit it became in the replay
    (tag..new), or None where it dropped out; and the replayed commits no original explains.
    Commits keep their order in a replay. Two originals can share the key (author, author
    date, message): the replayed commit goes with the first of them whose change covers it.
    """
    originals = commit_keys(repo, base, previous)
    pairs, unmatched, start = [], [], 0
    for commit, key, subject in commit_keys(repo, tag, new):
        matches = [i for i in range(start, len(originals)) if originals[i][1] == key]
        if not matches:
            unmatched.append((commit, key, subject))
            continue
        chosen = matches[0]
        if len(matches) > 1:
            chosen = next(
                (
                    i
                    for i in matches
                    if not commit_beyond(repo, originals[i][0], commit)
                ),
                matches[0],
            )
        pairs.extend((originals[i], None) for i in range(start, chosen))
        pairs.append((originals[chosen], commit))
        start = chosen + 1
    pairs.extend((originals[i], None) for i in range(start, len(originals)))
    return pairs, unmatched


def merge_differences(repo, merged, conflicted, new):
    """The paths git merged cleanly whose version in the new candidate is another one."""
    skip = set(conflicted)
    ours, theirs = tree_entries(repo, merged), tree_entries(repo, new)
    return sorted(
        path
        for path in set(ours) | set(theirs)
        if path not in skip and ours.get(path) != theirs.get(path)
    )


def carried_early(repo, tag, later, new):
    """KDE's commits after the tag on its branch (tag..later) that the new candidate already
    has: (present, undetermined). Every commit counts, a merge with what it brings against its
    first parent as well as each commit it merges. A commit is present when its patch reverses
    cleanly on the
    candidate and not on the tag (a revert of a later commit reverses on the tag itself). It is
    undetermined when it applies to the tag but neither way to the candidate: the candidate
    has those lines in another version; listed for review. A commit that builds on an earlier
    later commit applies to neither and says nothing."""
    present, undetermined = [], []
    commits = git(repo, "rev-list", "--reverse", f"{tag}..{later}").stdout.split()
    if not commits:
        return present, undetermined
    with tempfile.TemporaryDirectory(prefix="candidate-index-") as scratch:
        candidate_index, tag_index = Path(scratch) / "candidate", Path(scratch) / "tag"
        git(repo, "read-tree", new, index=candidate_index)
        git(repo, "read-tree", tag, index=tag_index)
        for commit in commits:
            # Plumbing: the clone's diff settings (prefixes, context) do not shape it.
            patch = git(
                repo,
                "diff-tree",
                "-p",
                "--binary",
                "--no-renames",
                "--ignore-submodules=none",
                # A merge against its first parent: what it brought to the branch.
                commit + "^1",
                commit,
                timeout=600,
            ).stdout
            if not patch:
                continue

            def applies(index, *reverse):
                return not git(
                    repo,
                    "apply",
                    "--cached",
                    "--check",
                    *reverse,
                    index=index,
                    input=patch,
                    check=False,
                ).returncode

            subject = git(repo, "log", "-1", "--format=%h %s", commit).stdout.strip()
            if applies(candidate_index, "-R") and not applies(tag_index, "-R"):
                present.append(subject)
            elif (
                applies(tag_index)
                and not applies(candidate_index)
                and not applies(candidate_index, "-R")
            ):
                undetermined.append(subject)
    return present, undetermined


def patch_id(repo, commit):
    """The commit's changed lines, hashed (git patch-id over a diff without context): equal for
    a replayed commit whose own lines are its original's, whatever the lines around them.
    """
    diff = git(
        repo,
        "show",
        "--no-color",
        "--format=",
        "--no-ext-diff",
        "--no-renames",
        "-U0",
        commit,
        timeout=600,
    ).stdout
    out = git(repo, "patch-id", "--verbatim", input=diff).stdout.split()
    return out[0] if out else ""


def commit_keys(repo, start, end):
    """(commit, key, subject) for start..end; the key is what a replay keeps: author and message."""
    out = git(
        repo,
        "log",
        "--reverse",
        "--format=%H%x00%an <%ae> %ad%x00%B%x01",
        "--date=raw",
        f"{start}..{end}",
    ).stdout
    entries = []
    for record in out.split("\x01"):
        record = record.lstrip("\n")
        if not record:
            continue
        commit, author, message = record.split("\x00", 2)
        subject = (message.strip().splitlines() or [""])[0]
        entries.append((commit, author + "\x00" + message.strip(), subject))
    return entries


@dataclass
class Replay:
    status: str
    branch: str
    previous: str
    base_commit: str
    tag_commit: str
    commit: str | None = None
    tree: str | None = None
    merged_tree: str | None = None
    delta: int = 0
    replayed: int = 0
    dropped: list = field(default_factory=list)
    changed: list = field(default_factory=list)
    merged: list = field(default_factory=list)
    conflicted: list = field(default_factory=list)
    three_way: str | None = None
    merge_differences: list = field(default_factory=list)
    unexplained: list = field(default_factory=list)
    rewritten: list = field(default_factory=list)
    early: list = field(default_factory=list)
    undetermined: list = field(default_factory=list)
    control: list = field(default_factory=list)
    problems: list = field(default_factory=list)
    diagnostics: str = ""

    def to_json(self):
        return asdict(self)


def replay(repo, previous, base, tag, pf, sign, later=None, verified=None):
    """Replay base..previous onto tag; sign is a key fingerprint, or None for an unsigned
    replay (tests). later: KDE's branch after the tag, for the check of fixes carried early.
    """
    check_step(base, tag)
    branch = candidate_branch(tag, pf)
    email = signing_identity(repo, sign) if sign else None
    old = resolve(repo, previous + "^{commit}")
    base_commit = resolve(repo, f"refs/tags/{base}^{{commit}}")
    tag_commit = resolve(repo, f"refs/tags/{tag}^{{commit}}")
    # The commits KDE's tags were checked to name (kde_tag): a local tag moved since is no tag
    # to build on.
    if verified is not None and (base_commit, tag_commit) != tuple(verified):
        raise CandidateError(
            f"the local tags {base} and {tag} moved after they were checked against KDE's"
        )
    if git(repo, "merge-base", "--all", old, tag_commit).stdout.split() != [
        base_commit
    ]:
        raise CandidateError(
            f"{previous} is not built on {base}: its merge base with {tag} is not that tag"
        )
    if git(repo, "rev-list", "--merges", f"{base_commit}..{old}").stdout.strip():
        raise CandidateError(
            f"{previous} has merge commits above {base}; a replay needs a linear delta"
        )
    originals = commit_keys(repo, base_commit, old)
    if not originals:
        raise CandidateError(f"{previous} has no commits above {base}")
    result = Replay("clean", branch, old, base_commit, tag_commit, delta=len(originals))
    with tempfile.TemporaryDirectory(prefix="candidate-replay-") as scratch:
        worktree = Path(scratch) / "tree"
        git(repo, "worktree", "add", "-q", "--detach", str(worktree), old)
        try:
            args = [
                "rebase",
                "--quiet",
                "--onto",
                tag_commit,
                base_commit,
                "--empty=drop",
                "--no-autosquash",
                "--no-update-refs",
            ]
            args.append(f"--gpg-sign={sign}" if sign else "--no-gpg-sign")
            rebase = git(worktree, *args, check=False, timeout=3600)
            if rebase.returncode:
                conflicted = git(
                    worktree, "diff", "--name-only", "--diff-filter=U", check=False
                ).stdout.split()
                stopped = git(
                    worktree, "log", "-1", "--format=%h %s", "REBASE_HEAD", check=False
                ).stdout.strip()
                git(worktree, "rebase", "--abort", check=False)
                # A replay that stopped without a conflicting path failed for another reason
                # (the signature, the identity).
                result.status = "conflict" if conflicted else "error"
                result.diagnostics = (
                    f"the replay stopped at {stopped or 'an unknown commit'}; conflicting paths: "
                    + (", ".join(conflicted) or "none listed")
                    + "\n"
                    + (rebase.stderr or rebase.stdout)[-4000:]
                )
                return result
            new = resolve(worktree, "HEAD")
        finally:
            git(repo, "worktree", "remove", "--force", str(worktree), check=False)
            git(repo, "worktree", "prune", check=False)
    git(repo, "update-ref", f"{NAMESPACE}/replay/{branch}", new)
    result.commit = new
    result.tree = resolve(repo, new + "^{tree}")
    replayed = commit_keys(repo, tag_commit, new)
    result.replayed = len(replayed)
    pairs, _ = pair(repo, base_commit, old, tag_commit, new)
    for (commit, _, subject), replayed_commit in pairs:
        if replayed_commit is None:
            result.dropped.append({"commit": commit, "subject": subject})
        elif patch_id(repo, commit) != patch_id(repo, replayed_commit):
            result.rewritten.append(subject)
    result.changed = changed_paths(repo, old, new)
    fork_paths = set(changed_paths(repo, base_commit, old))
    result.merged = [path for path in result.changed if path in fork_paths]
    result.control = [
        path
        for path in changed_paths(repo, base_commit, tag_commit)
        if CONTROL.fullmatch(path)
    ]
    result.problems = commit_problems(repo, tag_commit, new, sign=sign, email=email)
    result.merged_tree, result.conflicted = merge_tree(
        repo, base_commit, old, tag_commit
    )
    result.unexplained = unexplained(
        repo, base_commit, old, tag_commit, new, paths=set(result.conflicted)
    )
    result.merge_differences = merge_differences(
        repo, result.merged_tree, result.conflicted, new
    )
    if result.merge_differences:
        result.three_way = "differs"
    elif result.conflicted:
        result.three_way = "partial"
    else:
        result.three_way = "equal"
    if later:
        result.early, result.undetermined = carried_early(repo, tag_commit, later, new)
    if not result.replayed:
        result.status = "empty"
        result.diagnostics = "every commit of the previous candidate is in the tag: there is no fork commit to publish"
    elif result.merge_differences:
        result.status = "mismatch"
        result.diagnostics = (
            "paths git merges cleanly from the previous candidate and the tag, in another version:\n"
            + "\n".join(result.merge_differences[:50])
        )
    elif result.unexplained:
        result.status = "mismatch"
        result.diagnostics = (
            "changes the tags and the previous candidate do not explain:\n"
            + "\n".join(result.unexplained)
        )
    elif result.early:
        result.status = "early"
        result.diagnostics = (
            "KDE commits after the tag that the candidate already has:\n"
            + "\n".join(result.early)
        )
    elif result.control:
        result.status = "control"
        result.diagnostics = (
            "KDE changes between the tags to files that steer git or GitHub, held for review:\n"
            + "\n".join(result.control)
        )
    elif result.problems:
        result.status = "rules"
        result.diagnostics = "commits that fork CI would refuse:\n" + "\n".join(
            result.problems
        )
    return result


def kde_tag(repo, tag, kde=KDE):
    """The tag's object and commit at KDE, checked against the local tag of the same name."""
    parse_tag(tag)
    git(
        repo,
        "fetch",
        "--quiet",
        "--no-tags",
        kde,
        f"+refs/tags/{tag}:{NAMESPACE}/kde/tags/{tag}",
        timeout=600,
    )
    theirs = (
        resolve(repo, f"{NAMESPACE}/kde/tags/{tag}"),
        resolve(repo, f"{NAMESPACE}/kde/tags/{tag}^{{commit}}"),
    )
    try:
        ours = (
            resolve(repo, f"refs/tags/{tag}"),
            resolve(repo, f"refs/tags/{tag}^{{commit}}"),
        )
    except CandidateError:
        raise CandidateError(f"no local tag {tag}; fetch KDE's tags first") from None
    if ours != theirs:
        raise CandidateError(
            f"the local tag {tag} ({ours[0]}) is not KDE's ({theirs[0]})"
        )
    return theirs


def kde_branch(repo, tag, kde=KDE):
    """KDE's stable branch of the tag's series, fetched: (name, commit)."""
    major, minor = parse_tag(tag)[:2]
    series = f"Plasma/{major}.{minor}"
    git(
        repo,
        "fetch",
        "--quiet",
        "--no-tags",
        kde,
        f"+refs/heads/{series}:{NAMESPACE}/kde/heads/{series}",
        timeout=600,
    )
    return series, resolve(repo, f"{NAMESPACE}/kde/heads/{series}^{{commit}}")


def remote_ref(repo, remote, ref):
    """The object a remote's ref names, by its full name only (ls-remote matches name tails)."""
    for line in git(repo, "ls-remote", remote, ref, timeout=300).stdout.splitlines():
        oid, _, name = line.partition("\t")
        if name == ref:
            return oid
    return None


@dataclass
class Mirror:
    status: str
    branch: str
    old: str
    new: str
    kde_branch: str
    tag_commit: str

    def to_json(self):
        return asdict(self)


def mirror(repo, tag, kde=KDE, remote="origin", push=False):
    _, tag_commit = kde_tag(repo, tag, kde)
    series, kde_tip = kde_branch(repo, tag, kde)
    if not is_ancestor(repo, tag_commit, kde_tip):
        raise CandidateError(f"KDE's {tag} is not on KDE's {series}")
    old = remote_ref(repo, remote, f"refs/heads/{series}")
    if old is None:
        raise CandidateError(f"the fork has no {series} mirror branch")
    git(
        repo,
        "fetch",
        "--quiet",
        "--no-tags",
        remote,
        f"+refs/heads/{series}:{NAMESPACE}/fork/heads/{series}",
        timeout=600,
    )
    if resolve(repo, f"{NAMESPACE}/fork/heads/{series}") != old:
        raise CandidateError(f"the fork's {series} moved while it was read")
    # The mirror bounds what fork CI counts as the fork's commits: KDE's history only.
    if not is_ancestor(repo, old, kde_tip):
        raise CandidateError(
            f"the fork's {series} ({old}) is not on KDE's ({kde_tip}): "
            "it holds commits KDE does not have, or a fast forward is impossible"
        )
    if old == kde_tip:
        return Mirror("current", series, old, old, kde_tip, tag_commit)
    if not push:
        return Mirror("behind", series, old, kde_tip, kde_tip, tag_commit)
    git(
        repo,
        "push",
        "--quiet",
        "--no-verify",
        remote,
        f"{kde_tip}:refs/heads/{series}",
        timeout=900,
    )
    now = remote_ref(repo, remote, f"refs/heads/{series}")
    if now != kde_tip:
        raise CandidateError(
            f"the fork's {series} is {now} after the push, not {kde_tip}"
        )
    return Mirror("updated", series, old, kde_tip, kde_tip, tag_commit)


def history(repo, base, tip):
    """(key, tree) of every commit in base..tip: what a candidate made twice has in common."""
    return [
        (key, resolve(repo, commit + "^{tree}"))
        for commit, key, _ in commit_keys(repo, base, tip)
    ]


def equivalent(repo, one, other):
    """The same candidate made twice: commit by commit the same author, message and tree above
    their common base. Only the commit times, and so the commit names, differ."""
    base = git(repo, "merge-base", one, other, check=False).stdout.strip()
    if not base:
        return False
    return history(repo, base, one) == history(repo, base, other)


def push(repo, branch, commit, remote="origin", sign=None):
    """Create the candidate branch; returns (the branch's commit, what happened). The candidate
    is checked first, whichever way: a linear history on the tag the branch is named after,
    with fork CI's commit rules (and, with sign, a good signature by that key). A branch that
    exists is kept as it is: accepted when it holds the same candidate, an error otherwise.
    """
    if not isinstance(branch, str) or not BRANCH.fullmatch(branch):
        raise CandidateError(
            f"{branch!r} is not a candidate branch (candidate/X.Y.Z-pfN)"
        )
    commit = resolve(repo, commit + "^{commit}")
    tag = "v" + branch.split("/", 1)[1].rsplit("-pf", 1)[0]
    # A point release, as replay and mirror require (no X.Y.80 pre-release).
    parse_tag(tag)
    tag_commit = resolve(repo, f"refs/tags/{tag}^{{commit}}")
    email = signing_identity(repo, sign) if sign else None

    def validate(tip, what):
        if (
            not is_ancestor(repo, tag_commit, tip)
            or git(repo, "rev-list", "--merges", f"{tag_commit}..{tip}").stdout.strip()
        ):
            raise CandidateError(f"{what} is not a linear history on {tag}")
        if not git(repo, "rev-list", f"{tag_commit}..{tip}").stdout.strip():
            raise CandidateError(f"{what} has no fork commit above {tag}")
        problems = commit_problems(repo, tag_commit, tip, sign=sign, email=email)
        if problems:
            raise CandidateError(
                f"{what}: its commits fail the rules:\n" + "\n".join(problems)
            )

    existing = remote_ref(repo, remote, f"refs/heads/{branch}")
    if existing == commit:
        validate(commit, f"{branch} on {remote}")
        if remote_ref(repo, remote, f"refs/heads/{branch}") != commit:
            raise CandidateError(f"{branch} moved on {remote} while it was checked")
        return commit, "exists"
    if existing is not None:
        git(
            repo,
            "fetch",
            "--quiet",
            "--no-tags",
            remote,
            f"+refs/heads/{branch}:{NAMESPACE}/fork/heads/{branch}",
            timeout=600,
        )
        if resolve(repo, f"{NAMESPACE}/fork/heads/{branch}") != existing:
            raise CandidateError(f"{branch} moved on {remote} while it was read")
        if not equivalent(repo, existing, commit):
            raise CandidateError(
                f"{branch} already exists on {remote} at {existing} with other content; "
                "a candidate branch never moves"
            )
        validate(existing, f"{branch} on {remote}, the same candidate,")
        if remote_ref(repo, remote, f"refs/heads/{branch}") != existing:
            raise CandidateError(f"{branch} moved on {remote} while it was checked")
        return existing, "equivalent"
    validate(commit, commit)
    git(
        repo,
        "push",
        "--quiet",
        "--no-verify",
        f"--force-with-lease=refs/heads/{branch}:",
        remote,
        f"{commit}:refs/heads/{branch}",
        timeout=900,
    )
    if remote_ref(repo, remote, f"refs/heads/{branch}") != commit:
        raise CandidateError(f"{branch} on {remote} is not {commit} after the push")
    return commit, "created"


def write_output(path, data):
    if path:
        Path(path).write_text(
            json.dumps(data, indent=2, sort_keys=True) + "\n", encoding="utf-8"
        )
    print(json.dumps(data, indent=2, sort_keys=True))


def main(argv=None):
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    commands = parser.add_subparsers(dest="command", required=True)
    m = commands.add_parser("mirror")
    m.add_argument("--repo", type=Path, required=True)
    m.add_argument("--tag", required=True)
    m.add_argument("--remote", default="origin")
    m.add_argument("--push", action="store_true")
    m.add_argument("--output")
    r = commands.add_parser("replay")
    r.add_argument("--repo", type=Path, required=True)
    r.add_argument("--previous", required=True)
    r.add_argument("--base")
    r.add_argument("--tag", required=True)
    r.add_argument("--pf", type=int, required=True)
    r.add_argument("--sign", required=True, help="the signing key's fingerprint")
    r.add_argument("--output")
    p = commands.add_parser("push")
    p.add_argument("--repo", type=Path, required=True)
    p.add_argument("--branch", required=True)
    p.add_argument("--commit", required=True)
    p.add_argument("--remote", default="origin")
    p.add_argument("--sign", help="the signing key's fingerprint, for a branch kept")
    p.add_argument("--output")
    args = parser.parse_args(argv)
    try:
        if args.command == "mirror":
            result = mirror(args.repo, args.tag, remote=args.remote, push=args.push)
            write_output(args.output, result.to_json())
            return 0
        if args.command == "replay":
            if not FINGERPRINT.fullmatch(args.sign):
                raise CandidateError("--sign takes the signing key's full fingerprint")
            base = args.base or base_of(args.previous)
            verified = tuple(kde_tag(args.repo, tag)[1] for tag in (base, args.tag))
            _, later = kde_branch(args.repo, args.tag)
            result = replay(
                args.repo,
                args.previous,
                base,
                args.tag,
                args.pf,
                args.sign,
                later,
                verified=verified,
            )
            write_output(args.output, result.to_json())
            return 0 if result.status == "clean" else 1
        if args.sign is not None and not FINGERPRINT.fullmatch(args.sign):
            raise CandidateError("--sign takes the signing key's full fingerprint")
        commit, status = push(
            args.repo, args.branch, args.commit, remote=args.remote, sign=args.sign
        )
        write_output(
            args.output, {"branch": args.branch, "commit": commit, "status": status}
        )
        return 0
    except CandidateError as error:
        print(f"candidate: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
