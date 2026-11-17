<!-- SPDX-FileCopyrightText: 2026 Wisbendji Fimerlus <archledger236@gmail.com> -->
<!-- SPDX-License-Identifier: GPL-2.0-or-later -->

# Candidates for a KDE point release

Each KDE KWin release on the stable branch (`vX.Y.Z`, or a re-release `vX.Y.Z.1`) gets a fork
build: KDE's tag plus the fork's delta, and nothing else. No later stable fix is carried early:
the replay stops on a later KDE commit that is in the result as KDE made it, and lists the
later commits whose lines the result has in another version for the owner's review. The candidate is made by replaying the previous candidate's commits
onto the new tag, then its packages are built in the candidates Copr project and tested there.
The owner signs the tag and moves the build to the live project. A new series (6.9) is a
manual rebase; the replay refuses one.

## The run

On the pipeline host (archhost), from a checkout of this branch's tools:

```sh
.github/ci/candidate-run.sh --fork ~/pf3-build/kwin-src --previous candidate/6.8.2-pf5 \
    --tag v6.8.3 --pf 1 --key F35053398E3C80FE20891B82C10B8492BD7F30C6 --work DIR
```

`--fork` is a clone of the fork (origin `archledger/kwin`). The run keeps the user's git
configuration out, so the clone needs its own `user.name` and `user.email`. The commits are
signed with `--key` (the full fingerprint), which signs without a passphrase on that host.
The key must be trusted in the host's keyring, so that its signatures check as good (`G`, not
`U`). One of its user IDs must carry the clone's `user.email`, or GitHub would not verify the
commits. `copr-cli` must have the
account's API configuration, `gh` must be logged in, and podman must have the fork's CI image
(`localhost/kwin-fork-ci:fedora44`, built by `suites.sh`).

Every step writes its result into `DIR` and is skipped when the command runs again with the
same arguments, so a run that stopped goes on from where it stopped. To redo a step, remove its
`.done` file. A new source package (`srpm.done` removed) is built in Copr again.

1. **KDE's tags.** The base tag (from the previous candidate's name) and the new tag are
   fetched from invent.kde.org when the clone lacks them. `candidate.py` checks both against
   KDE's again.
2. **The mirror** (`candidate.py mirror`). Fork CI counts every commit above the `Plasma/*`
   branches as a fork commit, so the fork's `Plasma/X.Y` is KDE's branch and must contain the
   tag. A mirror behind KDE's branch moves to it by a fast forward, with a plain push. A mirror
   with a commit KDE does not have stops the run.
3. **The replay** (`candidate.py replay`, `replay.json`). The previous candidate's commits are
   replayed onto the tag in a temporary worktree, and each commit is signed. Commits that the
   tag already contains drop out; when all of them do, the run stops with status `empty`. The
   result must pass these checks:
   - **The merge.** Git merges the previous candidate and the tag over the base tag
     (`git merge-tree`). Every path it merges cleanly must be exactly that merge's version.
   - **The lines.** Every line the result changes against the previous candidate is part of
     KDE's change between the two tags. No replayed commit changes more lines than its
     original did; a commit may shrink, when the tag already has part of it, but never grow.
     The lines of a path are compared as lists, without their positions. These are the only
     checks for the paths that the merge cannot merge, which are listed as `conflicted`. That
     happens where the delta imported upstream code early and then changed it, as KWin
     6.8.2-pf5 did with 6.8.3's hang detector.
   - **Nothing from later.** No KDE commit on the branch after the tag is in the result. A
     commit is in it when its patch reverses on the result and not on the tag; a revert of a
     later commit reverses on the tag itself. A commit that applies to the tag but neither way
     to the result is listed as `undetermined`: the result has those lines in another version.
     That is the fork's own change of those lines, or an import of the fix in another form. It
     does not stop the run; the owner reviews them before the tag. A commit that builds on an
     earlier later one applies to neither, and says nothing.
   - **No control files.** A KDE change between the tags to `.github`, `.gitattributes` or
     `.gitmodules` stops the run. It would run on the fork's CI unreviewed.
   - **Fork CI's commit rules.** One `Signed-off-by` and no other trailer, and no AI
     attribution. There must be a good signature by `--key` and the clone's `user.email` as
     committer.

   `replay.json` also lists the commits the tag changed (`rewritten`: their own lines differ
   from the original's); review them. A conflict or a failed check stops the run before the
   candidate branch is pushed (the mirror may have moved already). The result is kept as
   `refs/candidate-pipeline/replay/<branch>`. Resolving a conflict is a manual rebase: the
   steps after the replay can then be run by hand on the branch it gives.
4. **The candidate branch** (`candidate.py push`): `candidate/X.Y.Z-pfN`. Fork CI runs on it.
   The commit is checked again first: a linear history on the tag, with at least one fork
   commit, and the commit rules. A candidate branch never moves. A branch that already holds
   the same candidate is kept, if its commits pass the rules. The same candidate means commit by commit the same author,
   message and tree, made again at another time. Anything else under the name is refused, and
   the next candidate takes the next pf number.
5. **The source package** (`packaging/fedora/build-fork-rpm.sh --commit`, in the CI image).
   The candidate commit is archived under the name the tag's archive will have. Its
   `CMakeLists.txt` must carry the tag's version; a re-release `X.Y.Z.1` keeps `X.Y.Z` there,
   as KDE's do. The version, the release and the pf number are written into the spec the
   package carries. The release
   number in front of the dist tag is 1 for a version other than the spec's, or
   `KWIN_RELEASE`. `build-info.txt` records the tag, the commit, the EVR and the archive's
   checksum.
6. **Copr.** The source package is built in `archledger/plasma-fusion-full-candidates`. A
   submitted build is watched again, not submitted twice, and the step passes only when Copr
   reports the build `succeeded`. To retry after a failed build, remove `DIR/copr-build`.
7. **Fork CI.** The run waits for fork CI on the candidate branch.

`DIR/SUMMARY.md` collects the results.

## After the run

- **Fedora's packaging.** Compare Fedora's `kwin.spec` for the previous version with the new
  one (the source packages are on Koji). The fork's spec follows Fedora's requirements, file
  lists and downstream patches.
- **The full profile's candidate** (plasma-fusion `docs/RELEASING.md`, step 2). Build Plasma
  Fusion pinned to the candidate's EVR in the candidates project. Then run
  `tools/tests/release-vm/fulltest.sh` fresh and switch with
  `PF_FULL_REPO=archledger/plasma-fusion-full-candidates`, and the update from the previous
  pin.
- **The owner.**
  - Sign the tag on the candidate commit: `git tag -s vX.Y.Z-pfN <commit>`.
  - Build the candidate's own source package in the live project; `SUMMARY.md` has its
    checksum. A rebuild from the tag
    (`SRPM=1 packaging/fedora/build-fork-rpm.sh vX.Y.Z-pfN OUT`) has the same tree, but other
    bytes: GitHub's archive, and a changelog entry of another date.
  - Write the fork's release notes, and make the Plasma Fusion release that moves the pin.

  The candidates project already has the candidate's build, so it needs no second build of
  the same version.
