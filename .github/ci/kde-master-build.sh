#!/bin/bash
# SPDX-FileCopyrightText: 2026 Wisbendji Fimerlus <archledger236@gmail.com>
# SPDX-License-Identifier: GPL-2.0-or-later
# Run in Containerfile.kde-master as root: build each KDE master dependency of kde-master-deps.txt from
# its pinned commit, as Fedora's own package of the same name builds it, and install it over Fedora's.
#   kde-master-build.sh DEPS PACKAGES_TSV
# DEPS: one "REPOSITORY COMMIT" per line (REPOSITORY under https://invent.kde.org/plasma/, the Fedora
# source package of the same name; COMMIT the full commit id); empty lines and # comments are skipped.
# Each built dependency is added to PACKAGES_TSV as "kde-master:REPOSITORY<TAB>COMMIT".
set -euo pipefail
deps_file=${1:?the dependency list} packages=${2:?the package list}
deps=$(grep -v -E '^[[:space:]]*(#|$)' "$deps_file" || true)
[ -n "$deps" ] || { echo "no dependencies in $deps_file" >&2; exit 1; }
# Every pin first: a malformed line stops the build before anything is fetched.
while read -r repo commit rest; do
    if ! [[ $repo =~ ^[a-z0-9][a-z0-9-]*$ && $commit =~ ^[0-9a-f]{40}$ && -z $rest ]]; then
        echo "malformed pin in $deps_file: $repo $commit $rest" >&2
        exit 1
    fi
done <<<"$deps"
dnf -y --setopt=install_weak_deps=False install git-core
while read -r repo _; do
    dnf -y --setopt=install_weak_deps=False builddep --enablerepo=fedora-source --enablerepo=updates-source "$repo"
done <<<"$deps"
work=/tmp/kde-master
while read -r repo commit; do
    src=$work/$repo bld=$work/$repo-build
    git init -q "$src"
    git -C "$src" fetch -q --depth 1 "https://invent.kde.org/plasma/$repo.git" "$commit"
    git -C "$src" checkout -q --detach FETCH_HEAD
    [ "$(git -C "$src" rev-parse HEAD)" = "$commit" ] || { echo "$repo: fetched another commit" >&2; exit 1; }
    version=$(sed -n 's/^set(PROJECT_VERSION "\([^"]*\)").*/\1/p' "$src/CMakeLists.txt" | head -n 1)
    [ -n "$version" ] || { echo "$repo: no PROJECT_VERSION in CMakeLists.txt" >&2; exit 1; }
    # The package identity Fedora's linker specs (redhat-package-notes) read, as rpmbuild sets it.
    RPM_ARCH=$(rpm --eval '%{_arch}') RPM_PACKAGE_NAME=$repo RPM_PACKAGE_VERSION=$version \
        RPM_PACKAGE_RELEASE=0.git${commit:0:10}$(rpm --eval '%{?dist}') \
        bash -euc "$(rpm --define "_vpath_srcdir $src" --define "_vpath_builddir $bld" \
            --define "name $repo" --define "version $version" \
            --eval '%cmake_kf6 -G Ninja -DBUILD_TESTING=OFF')"
    RPM_ARCH=$(rpm --eval '%{_arch}') RPM_PACKAGE_NAME=$repo RPM_PACKAGE_VERSION=$version \
        RPM_PACKAGE_RELEASE=0.git${commit:0:10}$(rpm --eval '%{?dist}') \
        cmake --build "$bld"
    cmake --install "$bld"
    printf 'kde-master:%s\t%s\n' "$repo" "$commit" >>"$packages"
    echo "kde-master: $repo $version at $commit installed"
done <<<"$deps"
rm -rf "$work"
dnf clean all
