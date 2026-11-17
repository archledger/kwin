#!/bin/bash
# SPDX-FileCopyrightText: 2026 Wisbendji Fimerlus <archledger236@gmail.com>
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Build the fork's KWin RPMs from a tag (docs/full-profile.md in archledger/plasma-fusion), or
# from a candidate commit before it is tagged (.github/ci/CANDIDATES.md): the spec is
# packaging/fedora/kwin-fusion.spec; the source is the fork's tagged archive, or the commit's
# git archive under the name the tag's archive will have.
# Usage: [SRPM=1] packaging/fedora/build-fork-rpm.sh [--commit SHA [--repo DIR]] TAG [OUTPUT_DIR]
#   TAG: the fork tag (e.g. v6.8.2-pf3); with --commit, the tag the commit is to get
#   --commit SHA: build that commit of the fork's history (DIR, default this repository) instead
#     of downloading the tag's archive; the commit's CMakeLists.txt must carry the tag's version
#   SRPM=1: build the source package only (the Copr project's input)
#   KWIN_RELEASE: the release number in front of the dist tag, for a version other than the
#     spec's own (default 1: Fedora's first build of a KDE version is release 1, and the pf
#     sequence sorts the fork's build above it); the spec's version keeps the spec's number
# The version, the release and the sequence number are written into the spec the source package
# carries: Copr rebuilds the source package from that spec alone.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
COMMIT=
REPO=$ROOT
while [ $# -gt 0 ]; do
    case $1 in
        --commit) COMMIT=${2:?--commit needs a commit}; shift 2 ;;
        --repo) REPO=${2:?--repo needs a directory}; shift 2 ;;
        --) shift; break ;;
        -*) echo "unknown option: $1" >&2; exit 2 ;;
        *) break ;;
    esac
done
TAG=${1:?usage: build-fork-rpm.sh [--commit SHA [--repo DIR]] TAG [OUTPUT_DIR]}
OUT=${2:-$ROOT/build/fork-rpms}
SPEC="$ROOT/packaging/fedora/kwin-fusion.spec"

[ -f "$SPEC" ] || { echo "no spec at $SPEC" >&2; exit 1; }

# The version and the fork's sequence number from the tag: v<version>-pf<N>
# (KDE re-releases a point release now and then as X.Y.Z.1)
[[ $TAG =~ ^v([0-9]+\.[0-9]+\.[0-9]+(\.[0-9]+)?)-pf([1-9][0-9]*)$ ]] ||
    { echo "tag $TAG is not a pf tag (v<version>-pf<N>)" >&2; exit 1; }
version=${BASH_REMATCH[1]}
pfn=${BASH_REMATCH[3]}

# The spec's own version and release number (Release: N%{?dist}.pf%{pf}).
spec_version=$(sed -n 's/^Version:[[:space:]]*//p' "$SPEC")
spec_release=$(sed -n 's/^Release:[[:space:]]*\([0-9][0-9]*\)%{?dist}\.pf%{pf}$/\1/p' "$SPEC")
[ "$(grep -c '^Version:' "$SPEC")" = 1 ] && [[ $spec_version =~ ^[0-9]+\.[0-9]+\.[0-9]+(\.[0-9]+)?$ ]] ||
    { echo "the spec has no single 'Version: X.Y.Z' line" >&2; exit 1; }
[ "$(grep -c '^Release:' "$SPEC")" = 1 ] && [ -n "$spec_release" ] ||
    { echo "the spec has no single 'Release: N%{?dist}.pf%{pf}' line" >&2; exit 1; }
if [ "$version" = "$spec_version" ]; then
    release=${KWIN_RELEASE:-$spec_release}
else
    release=${KWIN_RELEASE:-1}
fi
[[ $release =~ ^[1-9][0-9]*$ ]] || { echo "KWIN_RELEASE '$release' is not a release number" >&2; exit 1; }

mkdir -p "$OUT"/{BUILD,BUILDROOT,RPMS,SOURCES,SPECS,SRPMS}
# Absolute: git -C below reads a relative path against the other repository.
OUT=$(cd "$OUT" && pwd)
archive="kwin-${TAG}.tar.gz"
if [ -n "$COMMIT" ]; then
    # The candidate commit, archived under the tag's archive name and top directory (GitHub
    # names it kwin-<version>-pf<N>), so the source package is the one the tag will build.
    sha=$(git -C "$REPO" rev-parse --verify --end-of-options "$COMMIT^{commit}") ||
        { echo "$COMMIT is not a commit in $REPO" >&2; exit 1; }
    rm -f "$OUT/SOURCES/$archive"
    git -C "$REPO" archive --format=tar.gz --prefix="kwin-${version}-pf${pfn}/" -o "$OUT/SOURCES/$archive" "$sha"
    echo "building kwin-${version}-${release} with the dist tag suffix .pf${pfn} from commit ${sha} (to be tagged ${TAG})"
else
    sha=
    echo "building kwin-${version}-${release} with the dist tag suffix .pf${pfn} from ${TAG}"
    # The tagged archive (GitHub names its top directory kwin-<version>-pf<N>)
    if [ ! -f "$OUT/SOURCES/$archive" ]; then
        url="https://github.com/archledger/kwin/archive/${TAG}/${archive}"
        echo "downloading $url"
        curl -fsSL -o "$OUT/SOURCES/$archive" "$url"
    fi
fi

# The archive's KWin is the tag's, whichever way it came: its CMakeLists.txt sets one literal
# PROJECT_VERSION (as .github/ci/upstream.py requires; CMake reads command names in any case),
# the tag's version, or for a re-release X.Y.Z.1 the X.Y.Z that KDE keeps there (KWin 5.18.4.1).
top="kwin-${version}-pf${pfn}"
cmake=$(tar -xzOf "$OUT/SOURCES/$archive" "$top/CMakeLists.txt" 2>/dev/null) ||
    { echo "$archive has no $top/CMakeLists.txt" >&2; exit 1; }
assignments=$(grep -cE '^[[:space:]]*[sS][eE][tT][[:space:]]*\([[:space:]]*PROJECT_VERSION([^A-Za-z0-9_]|$)' <<<"$cmake" || true)
[ "$assignments" = 1 ] ||
    { echo "$archive sets PROJECT_VERSION $assignments times; one literal version is expected" >&2; exit 1; }
source_version=$(sed -nE 's/^[[:space:]]*[sS][eE][tT][[:space:]]*\([[:space:]]*PROJECT_VERSION[[:space:]]+"([0-9]+\.[0-9]+\.[0-9]+)"[[:space:]]*\)[[:space:]]*(#.*)?$/\1/p' <<<"$cmake")
expected=$version
[[ $version =~ ^([0-9]+\.[0-9]+\.[0-9]+)\.[0-9]+$ ]] && expected=${BASH_REMATCH[1]}
[ "$source_version" = "$expected" ] ||
    { echo "$archive is KWin ${source_version:-(no version)}, the tag $TAG is for $version" >&2; exit 1; }

# The macros source (the stock package's Source10), from Fedora's kwin source package: the
# newest one dnf finds, or KWIN_STOCK_SRPM (a source package downloaded before, from Koji say).
if [ ! -f "$OUT/SOURCES/macros.kwin.in" ]; then
    stock=$(mktemp -d)
    trap 'rm -rf "$stock"' EXIT
    if [ -n "${KWIN_STOCK_SRPM:-}" ]; then
        cp -- "$KWIN_STOCK_SRPM" "$stock/stock.src.rpm"
    else
        dnf download --source kwin --destdir "$stock" >/dev/null
    fi
    # rpm 6's rpm2cpio names the members ./macros.kwin.in, rpm 4's macros.kwin.in; cpio finds
    # no member by the other name and still exits 0.
    (cd "$stock" && rpm2cpio ./*.src.rpm | cpio -id --quiet '*macros.kwin.in')
    [ -f "$stock/macros.kwin.in" ] || { echo "Fedora's kwin source package has no macros.kwin.in" >&2; exit 1; }
    cp "$stock/macros.kwin.in" "$OUT/SOURCES/"
fi

# The spec to build from: the tag's version, release number and sequence number written in as
# its own. The source package carries this spec, and Copr rebuilds the source package from it
# alone (rpmbuild -bs in mock, with no --define): with the repository's default every source
# package named the pf3 archive, and a pf4 build failed for a missing kwin-v6.8.2-pf3.tar.gz.
built_spec="$OUT/SPECS/$(basename "$SPEC")"
sed -e "s/^%{!?pf:%global pf [0-9]*}\$/%{!?pf:%global pf ${pfn}}/" \
    -e "s/^Version:\([[:space:]]*\).*\$/Version:\1${version}/" \
    -e "s/^Release:\([[:space:]]*\)[0-9][0-9]*%{?dist}\.pf%{pf}\$/Release:\1${release}%{?dist}.pf%{pf}/" \
    "$SPEC" > "$built_spec"
[ "$(grep -c -x "%{!?pf:%global pf ${pfn}}" "$built_spec")" = 1 ] ||
    { echo "the spec has no single pf default line to set" >&2; exit 1; }
# A build of another version or release than the spec's, or of a commit, says what it is in
# the changelog's first entry.
if [ -n "$sha" ] || [ "$version-$release" != "$spec_version-$spec_release" ]; then
    if [ -n "$sha" ]; then
        when=$(git -C "$REPO" log -1 --format=%ct "$sha")
        what="the candidate commit ${sha}, to be tagged ${TAG}"
    else
        when=$(date +%s)
        what="the fork tag ${TAG}"
    fi
    day=$(LC_ALL=C date -u -d "@$when" '+%a %b %d %Y')
    entry="* ${day} Wisbendji Fimerlus <archledger236@gmail.com> - ${version}-${release}.pf${pfn}\n- KDE's KWin ${version} with the fork's changes: ${what}\n"
    sed -i "0,/^%changelog\$/s//%changelog\n${entry}/" "$built_spec"
fi
# What a rebuild from the spec alone takes: the tag's own archive (the directory it unpacks is
# named by the same number), and the version and release built here.
parsed=$(rpmspec -P "$built_spec")
grep -q -x "Source0: https://github.com/archledger/kwin/archive/${TAG}/kwin-${TAG}.tar.gz" <<<"$parsed" ||
    { echo "the spec does not name ${archive} on its own" >&2; exit 1; }
evr=$(rpmspec -q --srpm --qf '%{version}-%{release}\n' "$built_spec")
case $evr in
    # With the dist tag (.fc44) or without one (an rpm that defines none, as Ubuntu's)
    "${version}-${release}.pf${pfn}" | "${version}-${release}."*".pf${pfn}") ;;
    *) echo "the spec builds ${evr}, not ${version}-${release}<dist>.pf${pfn}" >&2; exit 1 ;;
esac
SPEC=$built_spec
{
    echo "tag=${TAG}"
    echo "commit=${sha}"
    echo "evr=${evr}"
    echo "archive_sha256=$(sha256sum "$OUT/SOURCES/$archive" | cut -d' ' -f1)"
} > "$OUT/build-info.txt"

# SRPM=1 builds the source package only (what Copr takes); otherwise the binary packages.
if [ "${SRPM:-0}" = 1 ]; then
    rpmbuild -bs --define "_topdir $OUT" --define "pf ${pfn}" "$SPEC"
    echo "SRPM in $OUT/SRPMS:"
    find "$OUT/SRPMS" -name '*.src.rpm' -exec basename {} \; | sort
else
    rpmbuild -bb --define "_topdir $OUT" --define "pf ${pfn}" "$SPEC"
    echo "RPMs in $OUT/RPMS:"
    find "$OUT/RPMS" -name '*.rpm' -exec basename {} \; | sort
fi
