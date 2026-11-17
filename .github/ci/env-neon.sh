# SPDX-FileCopyrightText: 2026 Wisbendji Fimerlus <archledger236@gmail.com>
# SPDX-License-Identifier: GPL-2.0-or-later
# /etc/kwin-ci/env.sh in the KDE neon image, sourced by suites.sh as ". /etc/kwin-ci/env.sh build|test".
# build: neon's packaging recipe for kwin, as the debian/rules of neon's kwin source package has it
# (4:6.8.2-0zneon+24.04+noble+release+build115): CC=gcc-14 and CXX=g++-14 ("need gcc 14"; Ubuntu 24.04's
# default GCC 13 lacks std::ranges::to), dh --with kf6 --buildsystem kf6, and override_dh_auto_configure
# with -DCMAKE_CXX_STANDARD=23 -DCMAKE_CXX_STANDARD_REQUIRED=TRUE. debhelper's kf6 build system runs with
# the Ninja backend and applies dpkg-buildflags as a package build does, with the source tree at /source.
# test: nothing; neon's own paths serve the suites.
case $1 in
build)
    export CC=gcc-14 CXX=g++-14
    kwin_ci_configure() {
        # debhelper runs in the source package root, which must contain the source tree, so it runs in /.
        # It reads the compatibility level from debian/control and the version from debian/changelog;
        # the image links /debian to this writable scratch directory.
        mkdir -p /scratch/debian
        printf 'Source: kwin\nBuild-Depends: debhelper-compat (= 13)\n\nPackage: kwin\nArchitecture: any\n' \
            >/scratch/debian/control
        printf 'kwin (4:%s-0ci) noble; urgency=medium\n\n  * Fork CI build.\n\n -- Fork CI <ci@localhost.invalid>  %s\n' \
            "$KWIN_CI_VERSION" "$(date -R)" >/scratch/debian/changelog
        cd /
        DEB_BUILD_PATH=/source dh_auto_configure --buildsystem=kf6+ninja --sourcedirectory=source \
            --builddirectory=build -- -DCMAKE_CXX_STANDARD=23 -DCMAKE_CXX_STANDARD_REQUIRED=TRUE -DBUILD_TESTING=ON
    }
    ;;
test) ;;
*)
    echo "env.sh: unknown phase '$1'" >&2
    return 64
    ;;
esac
