# SPDX-FileCopyrightText: 2026 Wisbendji Fimerlus <archledger236@gmail.com>
# SPDX-License-Identifier: GPL-2.0-or-later
# /etc/kwin-ci/env.sh in the Arch Linux image, sourced by suites.sh as ". /etc/kwin-ci/env.sh build|test".
# build: makepkg's own build environment (its library code, /etc/makepkg.conf and the options the kwin
# PKGBUILD leaves on: debug and lto), and the PKGBUILD's cmake call with the testing tree on. The
# PKGBUILD sets no build type. ECM's default is Debug for a git checkout, and the tested tree is a git
# worktree (Arch builds from a release tarball, which has none); Debug adds only -g, which makepkg's
# flags carry anyway.
# test: nothing; Arch's own paths serve the suites.
case $1 in
build)
    # makepkg's library reads unset variables.
    set +u
    MAKEPKG_LIBRARY=/usr/share/makepkg
    # shellcheck source=/dev/null # makepkg's library in the image
    source "$MAKEPKG_LIBRARY/util/config.sh"
    # shellcheck source=/dev/null
    source "$MAKEPKG_LIBRARY/buildenv.sh"
    load_makepkg_config
    # The kwin PKGBUILD sets no options; the sources are the tree mounted at /source.
    # shellcheck disable=SC2034 # read by makepkg's library
    options=() srcdir=/source pkgbase=kwin
    prepare_buildenv
    set -u
    kwin_ci_configure() {
        local cmd=(cmake -B /build -S /source -G Ninja -DKDE_INSTALL_LIBEXECDIR=lib -DBUILD_TESTING=ON)
        printf 'CFLAGS=%s\nCXXFLAGS=%s\nLDFLAGS=%s\n%s\n' "$CFLAGS" "$CXXFLAGS" "$LDFLAGS" "${cmd[*]}"
        "${cmd[@]}"
    }
    ;;
test) ;;
*)
    echo "env.sh: unknown phase '$1'" >&2
    return 64
    ;;
esac
