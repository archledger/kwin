#!/bin/bash
# SPDX-FileCopyrightText: 2026 Wisbendji Fimerlus <archledger236@gmail.com>
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Build the fork's KWin RPMs from a tag (docs/full-profile.md in archledger/plasma-fusion):
# the spec is packaging/fedora/kwin-fusion.spec; the source is the fork's tagged archive.
# Usage: [SRPM=1] packaging/fedora/build-fork-rpm.sh TAG [OUTPUT_DIR]
#   TAG: the fork tag (e.g. v6.8.2-pf3)
#   SRPM=1: build the source package only (the Copr project's input)
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
TAG=${1:?usage: build-fork-rpm.sh TAG [OUTPUT_DIR]}
OUT=${2:-$ROOT/build/fork-rpms}
SPEC="$ROOT/packaging/fedora/kwin-fusion.spec"

[ -f "$SPEC" ] || { echo "no spec at $SPEC" >&2; exit 1; }

# The version and the fork's sequence number from the tag: v<version>-pf<N>
case $TAG in
    v[0-9]*-pf[0-9]*) ;;
    *) echo "tag $TAG is not a pf tag (v<version>-pf<N>)" >&2; exit 1 ;;
esac
version=${TAG#v}
pfn=${version##*-pf}
version=${version%-pf*}
case $pfn in '' | *[!0-9]*) echo "tag $TAG has no sequence number after -pf" >&2; exit 1 ;; esac

# The spec carries the version itself (a --define cannot override its Version: line); a tag of
# another upstream version needs the spec changed first.
spec_version=$(sed -n 's/^Version:[[:space:]]*//p' "$SPEC" | head -n 1)
[ "$spec_version" = "$version" ] || { echo "the spec is for $spec_version, the tag for $version" >&2; exit 1; }
echo "building kwin-${version} with the dist tag suffix .pf${pfn} from ${TAG}"

mkdir -p "$OUT"/{BUILD,BUILDROOT,RPMS,SOURCES,SPECS,SRPMS}

# The tagged archive (GitHub names its top directory kwin-<version>-pf<N>)
archive="kwin-${TAG}.tar.gz"
if [ ! -f "$OUT/SOURCES/$archive" ]; then
    url="https://github.com/archledger/kwin/archive/${TAG}/${archive}"
    echo "downloading $url"
    curl -fsSL -o "$OUT/SOURCES/$archive" "$url"
fi

# The macros source (the stock package's Source10)
if [ ! -f "$OUT/SOURCES/macros.kwin.in" ]; then
    stock=$(mktemp -d)
    trap 'rm -rf "$stock"' EXIT
    dnf download --source kwin --destdir "$stock" >/dev/null
    (cd "$stock" && rpm2cpio kwin-*.src.rpm | cpio -id macros.kwin.in 2>/dev/null)
    cp "$stock/macros.kwin.in" "$OUT/SOURCES/"
fi

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
