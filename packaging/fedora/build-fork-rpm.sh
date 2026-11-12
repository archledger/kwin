#!/bin/bash
# SPDX-FileCopyrightText: 2026 Wisbendji Fimerlus <archledger236@gmail.com>
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Build the fork's KWin RPMs from a tag (docs/full-profile.md in archledger/plasma-fusion):
# the spec is packaging/fedora/kwin-fusion.spec; the source is the fork's tagged archive.
# Usage: packaging/fedora/build-fork-rpm.sh TAG [OUTPUT_DIR]
#   TAG: the fork tag (e.g. v6.8.2-pf3)
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
TAG=${1:?usage: build-fork-rpm.sh TAG [OUTPUT_DIR]}
OUT=${2:-$ROOT/build/fork-rpms}
SPEC="$ROOT/packaging/fedora/kwin-fusion.spec"

[ -f "$SPEC" ] || { echo "no spec at $SPEC" >&2; exit 1; }

# The version and pf number from the tag
version=${TAG#v}
version=${version%-pf*}
pfn=${TAG##*.pf}
[ "$pfn" != "$TAG" ] || { echo "tag $TAG is not a pf tag (v<version>-pf<N>)" >&2; exit 1; }

# The distag: the stock release plus .pfN
distag="3.fc44.pf${pfn}"
echo "building kwin-${version}-${distag} from ${TAG}"

mkdir -p "$OUT"/{BUILD,BUILDROOT,RPMS,SOURCES,SPECS,SRPMS}

# Download the tagged archive
archive="kwin-${TAG}.tar.gz"
if [ ! -f "$OUT/SOURCES/$archive" ]; then
    url="https://github.com/archledger/kwin/archive/${TAG}/${archive}"
    echo "downloading $url"
    curl -fsSL -o "$OUT/SOURCES/$archive" "$url"
fi

# The stock Fedora patch (from the src.rpm)
patch="c9d720793bbcef9483ab290673032d39441c0e6f.patch"
if [ ! -f "$OUT/SOURCES/$patch" ]; then
    # Extract from the installed src.rpm or download
    if [ -f "/tmp/opencode/kwin-spec/$patch" ]; then
        cp "/tmp/opencode/kwin-spec/$patch" "$OUT/SOURCES/"
    else
        dnf download --source kwin --destdir /tmp/opencode/kwin-spec >/dev/null 2>&1 || true
        cd /tmp/opencode/kwin-spec && rpm2cpio kwin-*.src.rpm | cpio -id "$patch" 2>/dev/null && cd "$ROOT"
        cp "/tmp/opencode/kwin-spec/$patch" "$OUT/SOURCES/"
    fi
fi

# The macros source (from the spec's Source10)
if [ ! -f "$OUT/SOURCES/macros.kwin.in" ]; then
    dnf download --source kwin --destdir /tmp/opencode/kwin-spec >/dev/null 2>&1 || true
    cd /tmp/opencode/kwin-spec && rpm2cpio kwin-*.src.rpm | cpio -id "macros.kwin.in" 2>/dev/null && cd "$ROOT"
    cp "/tmp/opencode/kwin-spec/macros.kwin.in" "$OUT/SOURCES/"
fi

# Build
rpmbuild -bb \
    --define "_topdir $OUT" \
    --define "version ${version}" \
    --define "distag ${distag}" \
    "$SPEC"

echo "RPMs in $OUT/RPMS:"
find "$OUT/RPMS" -name '*.rpm' -exec basename {} \; | sort
