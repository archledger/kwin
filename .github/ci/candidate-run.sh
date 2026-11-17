#!/bin/bash
# SPDX-FileCopyrightText: 2026 Wisbendji Fimerlus <archledger236@gmail.com>
# SPDX-License-Identifier: GPL-2.0-or-later
#
# The fork's candidate for a KDE KWin point release, from KDE's tag to its packages in the
# candidates Copr project (CANDIDATES.md). Runs on the pipeline host, from a checkout of the
# fork's tools; the candidate is made in FORK, a clone of the fork (origin: archledger/kwin).
#
#   candidate-run.sh --fork DIR --previous candidate/X.Y.Z-pfN --tag vX.Y.Z --pf N --key FPR --work DIR
#
# Each step leaves its result in WORK and is skipped when the script runs again with the same
# arguments, so a run that stopped (a conflict, a failed build, a lost session) goes on from
# there. Needs git, python3, podman with the fork's CI image, copr-cli and gh.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
KDE=https://invent.kde.org/plasma/kwin.git
REPO=archledger/kwin
PROJECT=${CANDIDATE_COPR_PROJECT:-archledger/plasma-fusion-full-candidates}
IMAGE=${CANDIDATE_IMAGE:-localhost/kwin-fork-ci:fedora44}
COPR=${COPR_CLI:-copr-cli}
VERSION='[0-9]+\.[0-9]+\.[0-9]+(\.[0-9]+)?'
usage() {
    sed -n '9p' "$0" | sed 's/^# *//' >&2
    exit 2
}
fork='' previous='' tag='' pf='' key='' work=''
while [ $# -gt 0 ]; do
    [ $# -ge 2 ] || usage
    case $1 in
        --fork) fork=$2 ;;
        --previous) previous=$2 ;;
        --tag) tag=$2 ;;
        --pf) pf=$2 ;;
        --key) key=$2 ;;
        --work) work=$2 ;;
        *) usage ;;
    esac
    shift 2
done
[ -n "$fork" ] && [ -n "$previous" ] && [ -n "$tag" ] && [ -n "$pf" ] && [ -n "$key" ] && [ -n "$work" ] || usage
[[ $previous =~ ^candidate/${VERSION}-pf[1-9][0-9]*$ ]] ||
    { echo "--previous names a candidate branch (candidate/X.Y.Z-pfN): $previous" >&2; exit 2; }
[[ $tag =~ ^v${VERSION}$ ]] || { echo "--tag names a KDE tag (vX.Y.Z): $tag" >&2; exit 2; }
[[ $pf =~ ^[1-9][0-9]*$ ]] || { echo "--pf is a positive number: $pf" >&2; exit 2; }
[[ $key =~ ^[0-9A-Fa-f]{40}$ ]] || { echo "--key is the signing key's full fingerprint: $key" >&2; exit 2; }
mkdir -p "$work"
work=$(cd "$work" && pwd)
fork=$(cd "$fork" && pwd)
# The same run goes on; another run needs another WORK.
params="fork=$fork previous=$previous tag=$tag pf=$pf key=$key image=$IMAGE project=$PROJECT release=${KWIN_RELEASE:-}"
if [ -f "$work/params" ]; then
    [ "$(cat "$work/params")" = "$params" ] || { echo "$work holds another run: $(cat "$work/params")" >&2; exit 2; }
else
    printf '%s\n' "$params" >"$work/params"
fi
# Pushes to GitHub (candidate.py keeps the user's git configuration out).
export CANDIDATE_GIT_CREDENTIAL_HELPER=${CANDIDATE_GIT_CREDENTIAL_HELPER:-'!gh auth git-credential'}

log() { printf '%s %s\n' "$(date -u +%FT%TZ)" "$*" | tee -a "$work/run.log"; }
fail() {
    log "$1"
    exit 1
}
done_before() {
    [ -f "$work/$1.done" ] || return 1
    log "$1: done before ($(cat "$work/$1.done"))"
}
finish() { date -u +%FT%TZ >"$work/$1.done"; }
candidate() { python3 -I "$HERE/candidate.py" "$@"; }
field() {
    python3 -I -c 'import json, sys; sys.stdout.reconfigure(errors="backslashreplace"); print(json.load(open(sys.argv[1]))[sys.argv[2]])' \
        "$work/$1" "$2"
}
count() { python3 -I -c 'import json, sys; print(len(json.load(open(sys.argv[1]))[sys.argv[2]]))' "$work/$1" "$2"; }
log "run with the tools at $(git -C "$ROOT" rev-parse HEAD)$([ -z "$(git -C "$ROOT" status --porcelain)" ] || echo ' (with changes)')"

# KDE's tags in the clone, and the previous candidate as the fork has it (candidate.py checks the
# tags against KDE's again).
base=v${previous#candidate/}
base=${base%-pf*}
git -C "$fork" fetch --quiet --no-tags origin "+refs/heads/$previous:refs/remotes/origin/$previous" ||
    fail "fetch: the fork's $previous could not be fetched"
for t in "$base" "$tag"; do
    git -C "$fork" rev-parse -q --verify "refs/tags/$t" >/dev/null ||
        git -C "$fork" fetch --quiet --no-tags "$KDE" "refs/tags/$t:refs/tags/$t" ||
        fail "fetch: KDE's $t could not be fetched"
done

if ! done_before mirror; then
    log "mirror: the fork's mirror of KDE's branch, up to $tag"
    candidate mirror --repo "$fork" --tag "$tag" --push --output "$work/mirror.json" >/dev/null ||
        fail "mirror: stopped (the error is above); nothing else was done"
    log "mirror: $(field mirror.json status) ($(field mirror.json old) -> $(field mirror.json new))"
    finish mirror
fi

if ! done_before replay; then
    log "replay: $previous onto $tag"
    rm -f "$work/replay.json"
    rc=0
    candidate replay --repo "$fork" --previous "origin/$previous" --base "$base" --tag "$tag" --pf "$pf" \
        --sign "$key" --output "$work/replay.json" >/dev/null || rc=$?
    if [ "$rc" != 0 ]; then
        [ -f "$work/replay.json" ] || fail "replay: stopped (the error is above); no candidate branch was pushed"
        field replay.json diagnostics >&2
        fail "replay: $(field replay.json status); no candidate branch was pushed (replay.json)"
    fi
    log "replay: clean, $(field replay.json replayed) of $(field replay.json delta) commits; changed against the previous candidate: $(field replay.json changed)"
    log "replay: $(count replay.json conflicted) path(s) git cannot merge, checked by their lines only; $(count replay.json rewritten) commit(s) changed by the tag"
    finish replay
fi
branch=$(field replay.json branch)

if ! done_before push; then
    candidate push --repo "$fork" --branch "$branch" --commit "$(field replay.json commit)" --sign "$key" \
        --output "$work/push.json" >/dev/null || fail "push: stopped (the error is above)"
    log "push: $branch $(field push.json status) at $(field push.json commit)"
    finish push
fi
commit=$(field push.json commit)
ptag="$tag-pf$pf"

if ! done_before srpm; then
    log "source package: $ptag from $commit"
    # A new source package is a new Copr build: the receipt of the old one goes.
    rm -rf "$work/srpm" "$work/copr.done" "$work/copr-build"
    release=()
    [ -z "${KWIN_RELEASE:-}" ] || release=(-e "KWIN_RELEASE=$KWIN_RELEASE")
    # shellcheck disable=SC2016 # expanded by the container's shell
    podman run --rm --userns=keep-id -e HOME=/tmp "${release[@]}" -v "$ROOT:/tools:ro" -v "$fork:/fork:ro" \
        -v "$work:/work" --entrypoint bash "$IMAGE" \
        -c 'SRPM=1 /tools/packaging/fedora/build-fork-rpm.sh --commit "$1" --repo /fork "$2" /work/srpm' \
        _ "$commit" "$ptag" >"$work/srpm.log" 2>&1 ||
        { tail -n 20 "$work/srpm.log" >&2; fail "source package: failed (srpm.log)"; }
    log "source package: $(cd "$work/srpm/SRPMS" && ls ./*.src.rpm) ($(grep '^evr=' "$work/srpm/build-info.txt"))"
    finish srpm
fi

if ! done_before copr; then
    # A build already submitted is watched again, not submitted twice; to try again after a
    # failed build, remove copr-build (or srpm.done, for a new source package).
    if [ ! -s "$work/copr-build" ]; then
        rc=0
        "$COPR" build "$PROJECT" "$work"/srpm/SRPMS/*.src.rpm --nowait >"$work/copr-submit.log" 2>&1 || rc=$?
        sed -n 's/^Created builds: \([0-9][0-9]*\)$/\1/p' "$work/copr-submit.log" >"$work/copr-build"
        if [ ! -s "$work/copr-build" ]; then
            rm -f "$work/copr-build"
            tail -n 20 "$work/copr-submit.log" >&2
            fail "copr: no build was created (rc $rc, copr-submit.log); look for one in $PROJECT before running again"
        fi
    fi
    id=$(cat "$work/copr-build")
    log "copr: build $id in $PROJECT"
    "$COPR" watch-build "$id" >"$work/copr-watch.log" 2>&1 || true
    # copr-cli's watch ends without an error on an interrupt, and on a skipped build.
    state=$("$COPR" --no-color status "$id" 2>/dev/null) || state=unknown
    [ "$state" = succeeded ] || fail "copr: build $id is $state (copr-watch.log)"
    log "copr: build $id succeeded"
    finish copr
fi

if ! done_before ci; then
    run=
    for _ in $(seq 1 30); do
        run=$(gh run list -R "$REPO" --branch "$branch" --workflow fork-ci.yml --json databaseId,headSha \
            --jq "map(select(.headSha == \"$commit\")) | .[0].databaseId // empty") ||
            fail "fork CI: gh could not list the runs of $branch"
        [ -n "$run" ] && break
        sleep 10
    done
    [ -n "$run" ] || fail "fork CI: no run for $commit on $branch"
    echo "$run" >"$work/ci-run"
    log "fork CI: run $run"
    gh run watch "$run" -R "$REPO" --exit-status --interval 60 >"$work/ci-watch.log" 2>&1 ||
        fail "fork CI: run $run did not succeed (ci-watch.log)"
    log "fork CI: run $run succeeded"
    finish ci
fi

[ -s "$work/copr-build" ] && [ -s "$work/ci-run" ] ||
    fail "summary: the Copr build or the fork CI run is not recorded; run again"
evr=$(sed -n 's/^evr=//p' "$work/srpm/build-info.txt")
srpm=$(cd "$work/srpm/SRPMS" && ls ./*.src.rpm)
{
    echo "# $branch"
    echo
    echo "- KDE's $tag: $(field replay.json tag_commit); the previous candidate $previous: $(field replay.json previous)"
    echo "- the mirror: $(field mirror.json branch) $(field mirror.json status) at $(field mirror.json new)"
    echo "- the candidate: $commit (tree $(field replay.json tree)), $(field replay.json replayed) of $(field replay.json delta) commits replayed and signed"
    echo "- dropped (already in $tag): $(field replay.json dropped)"
    echo "- changed by the tag (their own lines differ now; review them): $(field replay.json rewritten)"
    echo "- changed against the previous candidate: $(field replay.json changed)"
    echo "- paths git cannot merge from the previous candidate and the tag (checked by their lines only): $(field replay.json conflicted)"
    echo "- KDE commits after $tag found in the candidate: $(field replay.json early); undetermined: $(field replay.json undetermined)"
    echo "- source package: $(cd "$work/srpm/SRPMS" && sha256sum "$srpm")"
    echo "- Copr: build $(cat "$work/copr-build") in $PROJECT succeeded ($evr)"
    echo "- fork CI: run $(cat "$work/ci-run") succeeded"
    echo
    echo "Next: Plasma Fusion's full-profile candidate pinned to $evr in $PROJECT and its VM tests"
    echo "(plasma-fusion docs/RELEASING.md, step 2). Then, the owner: git tag -s $ptag $commit, and"
    echo "this source package (the sha256 above) built in the live project, archledger/plasma-fusion-full."
} >"$work/SUMMARY.md"
log "done: $work/SUMMARY.md"
