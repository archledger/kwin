# SPDX-FileCopyrightText: 2026 Wisbendji Fimerlus <archledger236@gmail.com>
# SPDX-License-Identifier: GPL-2.0-or-later
# /etc/kwin-ci/env.sh in the Fedora images, sourced by suites.sh as ". /etc/kwin-ci/env.sh build|test".
# build: Fedora's packaging recipe, rpm's %cmake_kf6 as kwin.spec configures, and the package identity that
# Fedora's linker specs (redhat-package-notes) read from the environment, as in rpmbuild.
# test: nothing; Fedora's own paths serve the suites.
case $1 in
build)
    export RPM_ARCH=x86_64 RPM_PACKAGE_NAME=kwin RPM_PACKAGE_VERSION="$KWIN_CI_VERSION"
    RPM_PACKAGE_RELEASE=1$(rpm --eval '%{?dist}').ci
    export RPM_PACKAGE_RELEASE
    kwin_ci_configure() {
        local cmd
        cmd=$(rpm --define '_vpath_srcdir /source' --define '_vpath_builddir /build' \
            --define 'name kwin' --define "version $RPM_PACKAGE_VERSION" --define "release $RPM_PACKAGE_RELEASE" \
            --eval '%cmake_kf6 -G Ninja -DBUILD_TESTING=ON')
        printf '%s\n' "$cmd"
        bash -euc "$cmd"
    }
    ;;
test) ;;
*)
    echo "env.sh: unknown phase '$1'" >&2
    return 64
    ;;
esac
