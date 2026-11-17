# SPDX-FileCopyrightText: 2026 Wisbendji Fimerlus <archledger236@gmail.com>
# SPDX-License-Identifier: GPL-2.0-or-later
# /etc/kwin-ci/env.sh in the NixOS image, sourced by suites.sh as ". /etc/kwin-ci/env.sh build|test".
# Both phases load nixpkgs' build environment of kdePackages.kwin, saved at image build time
# (/etc/kwin-ci/devenv.sh, from nix print-dev-env), so no Nix command runs here.
# The system profile /nix/var/nix/profiles/kwin-ci holds exactly the packages-nixos.txt attributes from the
# pinned nixpkgs revision; Containerfile.nixos checks this.
# build: nixpkgs' configure phase for kwin (cmakeConfigurePhase with the derivation's cmakeFlags) from /source
# into /build, with the testing tree on; nixpkgs' compiler wrappers add its hardening and debug flags.
# One deviation from nixpkgs' sandboxed recipe: the pinned xwayland's bin directory is on PATH. KDE's
# Plasma/6.8 made the Xwayland executable a configure requirement after 6.8.3 (3092543983, "cmake: Make
# Xwayland REQUIRED"), which the fork carries. nixpkgs' kwin 6.8.3 recipe has xwayland as a host input only,
# where CMake does not look for programs; a recipe for 6.8.4 needs it as a native build input, which puts
# its bin directory on PATH as here.
# test: the system profile's programs on PATH and its data as /usr/share, the Qt plugin and QML directories
# of every kwin input, as nixpkgs' Qt wrapper sets them for kwin_wayland, and Mesa. NixOS provides Mesa's
# drivers and its GLX and EGL vendor libraries under /run/opengl-driver, which libglvnd searches; here the
# pinned Mesa's lib directory follows /build/bin on LD_LIBRARY_PATH (glxgears needs Mesa's GLX library)
# and the EGL, DRI and GBM search paths point into the profile.
profile=/nix/var/nix/profiles/kwin-ci
case $1 in
build | test)
    # The saved environment reads unset variables.
    set +u
    # shellcheck source=/dev/null # saved in the image
    . /etc/kwin-ci/devenv.sh
    set -u
    ;;
*)
    echo "env.sh: unknown phase '$1'" >&2
    return 64
    ;;
esac
if [ "$1" = build ]; then
    xwayland=$(readlink -f "$profile/bin/Xwayland")
    PATH=$PATH:${xwayland%/*}
    kwin_ci_configure() {
        # shellcheck disable=SC2034 # read by cmakeConfigurePhase
        cmakeDir=/source cmakeBuildDir=/build
        cmakeFlags="$cmakeFlags -DBUILD_TESTING=ON"
        set +u
        cmakeConfigurePhase
        set -u
    }
else
    PATH=$PATH:$profile/bin
    QT_PLUGIN_PATH=
    NIXPKGS_QT6_QML_IMPORT_PATH=
    # shellcheck disable=SC2154 # qtHostPathSeen comes from the saved environment
    for dir in "${qtHostPathSeen[@]}"; do
        [ ! -d "$dir/lib/qt-6/plugins" ] || QT_PLUGIN_PATH=${QT_PLUGIN_PATH:+$QT_PLUGIN_PATH:}$dir/lib/qt-6/plugins
        [ ! -d "$dir/lib/qt-6/qml" ] || NIXPKGS_QT6_QML_IMPORT_PATH=${NIXPKGS_QT6_QML_IMPORT_PATH:+$NIXPKGS_QT6_QML_IMPORT_PATH:}$dir/lib/qt-6/qml
    done
    export QT_PLUGIN_PATH NIXPKGS_QT6_QML_IMPORT_PATH
    export XDG_DATA_DIRS=$XDG_DATA_DIRS:/usr/share
    mesa=$(readlink -f "$profile/lib/libGLX_mesa.so.0")
    export LD_LIBRARY_PATH=${LD_LIBRARY_PATH:+$LD_LIBRARY_PATH:}${mesa%/*}
    export __EGL_VENDOR_LIBRARY_DIRS=$profile/share/glvnd/egl_vendor.d LIBGL_DRIVERS_PATH=$profile/lib/dri
    export GBM_BACKENDS_PATH=$profile/lib/gbm FONTCONFIG_FILE=/etc/kwin-ci/fonts.conf
fi
