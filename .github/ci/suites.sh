#!/bin/bash
# SPDX-FileCopyrightText: 2026 Wisbendji Fimerlus <archledger236@gmail.com>
# SPDX-License-Identifier: GPL-2.0-or-later
# The fork's test image, as the ThinkPad qualification lane builds it: Fedora 44 with current updates,
# the package names of the earlier KWin builder (packages-old-builder.txt) and the build dependencies
# of Fedora's kwin source package. Packages that no longer exist are skipped and listed in /skipped.txt.
# The image includes Fedora's own KWin; suites.sh hides its scripted effects and loads the built libkwin.
# The build recipe and package list come from this trusted helper directory, never from the candidate
# source, and the package version is parsed from the candidate without executing any of its code.
set -euo pipefail
(( $# == 2 )) || { echo "usage: $0 SOURCE_DIR WORK_DIR" >&2; exit 64; }
ci=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
src=$(cd "$1" && pwd)
mkdir -p "$2"
work=$(cd "$2" && pwd)
[ -z "$(git -C "$src" status --porcelain)" ] || { echo "source not clean" >&2; exit 1; }
# The tested tree is HEAD exactly, as a detached worktree: no untracked files, and no .git directory for KDE's
# commit-hook module (KDEGitCommitHooks) to write into, since the source is mounted read-only.
tree=$work/source
[ ! -e "$tree" ] && [ ! -L "$tree" ] || { echo "candidate worktree already exists: $tree" >&2; exit 1; }
git -C "$src" worktree add -q --detach "$tree" HEAD
trap 'git -C "$src" worktree remove "$tree" >/dev/null 2>&1 || echo "retained non-clean candidate: $tree" >&2' EXIT
version=$(python3 "$ci/upstream.py" version "$tree")
out=$work/out
bld=$work/build
mkdir -p "$out" "$bld" "$work/tmp" "$work/empty-effects"
image=localhost/kwin-fork-ci:fedora44
suites='X11Window|DecorationInput|DontCrashEmptyDeco|DontCrashAuroraeDestroyDeco|Tiles|XdgShellWindow|QuickTiling|MoveResize|FractionalScaling|OutputChanges|Maximized|ScreenChanges|OffscreenQuickView|InternalWindow|TabBox|Placement|TransientPlacement|StartupDefaults|Rect|RectF|Fifo|HangDetectorShutdown|WindowThumbnailSource'
targets="kwin kill fakedecoshadows testWaylandSurface $(tr '|' '\n' <<<"$suites" | sed 's/^/test/' | tr '\n' ' ')"
cpus=${CPUS:-4}
stage=device
fail() {
    echo "::error::stage $stage failed (logs: $out)" >&2
    printf '%s=1\n' "$stage" >>"$out/STATUS"
    exit 1
}
node=$(find /dev/dri -maxdepth 1 -name 'renderD*' -print 2>/dev/null | sort | head -n 1)
if [ -n "$node" ]; then
    dri=(--device="$node:$node:rw" --group-add=keep-groups)
elif [ -w /dev/udmabuf ]; then
    # No GPU: KWin's software render device (GpuManager, llvmpipe over udmabuf).
    dri=(--device=/dev/udmabuf:/dev/udmabuf:rw)
else
    echo "::error::no render node and no writable /dev/udmabuf: KWin 6.8 cannot composite" >&2
    fail
fi
printf '%s\n' "${node:-/dev/udmabuf (software)}" >"$out/render-device.txt"
printf 'device=0\n' >>"$out/STATUS"
stage=image
git -C "$src" rev-parse HEAD 'HEAD^{tree}' >"$out/source-commit.txt"
podman build --pull=newer -t "$image" -f "$ci/Containerfile" "$ci" >"$out/image.log" 2>&1 || fail
podman image inspect "$image" --format '{{.Id}}' >"$out/image-id.txt"
for f in packages.tsv skipped.txt; do podman run --rm "$image" cat "/$f" >"$out/$f"; done
printf 'image=0\n' >>"$out/STATUS"
# shellcheck disable=SC2054 # the commas belong to the --tmpfs option value
common=(--rm --userns=keep-id --network=none --ulimit core=1:1 --cap-drop=all --security-opt=no-new-privileges --read-only
    --cpus="$cpus" --memory=12g --memory-swap=12g --tmpfs /tmp:rw,nosuid,nodev,size=1g)
container() {
    # Fedora's linker specs (redhat-package-notes) read the package identity from the environment, as in rpmbuild.
    podman run "${common[@]}" -e TMPDIR=/scratch -e RPM_ARCH=x86_64 -e RPM_PACKAGE_NAME=kwin -e RPM_PACKAGE_VERSION="$version" \
        -e RPM_PACKAGE_RELEASE=1.fc44.ci -v "$tree:/source:ro,z" -v "$bld:/build:rw,z" -v "$work/tmp:/scratch:rw,z" \
        --entrypoint bash "$image" -euc "$1"
}
run_test() {
    podman run "${common[@]}" "${dri[@]}" \
        -e HOME=/tmp/home -e XDG_CONFIG_HOME=/tmp/home/config -e XDG_CACHE_HOME=/tmp/home/cache \
        -e XDG_RUNTIME_DIR=/tmp/runtime -e LD_LIBRARY_PATH=/build/bin -e QT_QPA_PLATFORM=offscreen \
        -e QSG_RHI_BACKEND=opengl -e KWIN_FORCE_SW_CURSOR=1 -v "$work/empty-effects:/usr/share/kwin/effects:ro,z" \
        -v "$tree:/source:ro,z" -v "$bld:/build:rw,z" --entrypoint bash "$image" \
        -euc 'mkdir -p "$HOME" "$XDG_CONFIG_HOME" "$XDG_CACHE_HOME" "$XDG_RUNTIME_DIR"
          chmod 700 "$XDG_RUNTIME_DIR"
          if [ ! -d /tmp/.X11-unix ]; then mkdir /tmp/.X11-unix; chmod 1777 /tmp/.X11-unix; fi
          '"$1"
}
stage=configure
if [ ! -f "$bld/CMakeCache.txt" ]; then
    container "cmd=\$(rpm --define '_vpath_srcdir /source' --define '_vpath_builddir /build' \
        --define 'name kwin' --define 'version $version' --define 'release 1.fc44.ci' \
        --eval '%cmake_kf6 -G Ninja -DBUILD_TESTING=ON'); printf '%s\n' \"\$cmd\"; bash -euc \"\$cmd\"" \
        >"$out/configure.log" 2>&1 || fail
fi
printf 'configure=0\n' >>"$out/STATUS"
stage=build
container "cmake --build /build --parallel $cpus --target $targets -- -k 0" >"$out/build.log" 2>&1 || {
    grep -E "error:|FAILED:" "$out/build.log" | head -n 40 >&2
    fail
}
printf 'build=0\n' >>"$out/STATUS"
(cd "$bld/bin" && sha256sum "libkwin.so.$version" testXdgShellWindow testTiles testOffscreenQuickView testStartupDefaults testFifo testHangDetectorShutdown testWaylandSurface testWindowThumbnailSource) >"$out/binaries.sha256"
stage=inventory
run_test "ctest --test-dir /build --show-only=json-v1 -R '^(kwin-test($suites)|kwayland-testWaylandSurface)\$'" \
    >"$out/test-inventory.json" 2>"$out/test-inventory.stderr" || fail
stage=core
rc=0
junit=$(mktemp "$bld/ci-results-XXXXXX.xml")
run_test "export LD_DEBUG=libs; ctest --test-dir /build -V --no-tests=error --timeout 300 --repeat until-pass:3 --output-junit /build/${junit##*/} -R '^(kwin-test($suites)|kwayland-testWaylandSurface)\$'" \
    >"$work/core-full.log" 2>&1 || rc=$?
cp "$junit" "$out/ctest.xml"
rm -- "$junit"
# The dynamic loader's trace is only needed for the libkwin check below; keep its init lines.
grep -vE '^[0-9]+: +[0-9]+:[[:space:]]' "$work/core-full.log" >"$out/core.log" || true
grep -E 'calling init: .*libkwin\.so' "$work/core-full.log" | sed -E 's/^[0-9]+: +[0-9]+:[[:space:]]+//' | sort | uniq -c >"$out/libkwin-init.txt" || true
rm -f "$work/core-full.log"
grep -E 'tests passed|tests failed' "$out/core.log" | tail -n 1 >"$out/summary.txt" || true
# Suites that passed only after a repeat: ctest reports the failed attempt before the passing one.
grep -E 'Test +#[0-9]+: (kwin|kwayland)-test[A-Za-z0-9]+ .*\*\*\*(Failed|Timeout|Exception)' "$out/core.log" |
    grep -oE '(kwin|kwayland)-test[A-Za-z0-9]+' | sort | uniq -c >"$out/repeats.txt" || true
(( rc == 0 )) || fail
python3 "$ci/suite_results.py" --discovery "$out/test-inventory.json" --junit "$out/ctest.xml" \
    --log "$out/core.log" --libkwin-init "$out/libkwin-init.txt" --output "$out/test-validation.json" || fail
# Every suite must have loaded the built libkwin and none Fedora's.
grep -q 'calling init: /build/bin/libkwin\.so' "$out/libkwin-init.txt" || { echo "built libkwin not loaded" >&2; fail; }
if grep -qE 'calling init: /usr/lib(64)?/libkwin\.so' "$out/libkwin-init.txt"; then echo "Fedora's libkwin loaded" >&2; fail; fi
printf 'core=0\n' >>"$out/STATUS"
if [ "${MEMCHECK:-0}" = 1 ]; then
    stage=memcheck
    status=0
    # The only documented suppression: the Qt 6.11.2 startup over-read in
    # QObject::connect(const char*) during QApplication init (see the file's
    # header). It comes from this trusted helper directory, never candidate
    # source, so imported code cannot widen the weekly gate.
    supp=(-v "$ci/memcheck-qt611-known.supp:/ci-known.supp:ro,z")
    # Entries limited to what passes deterministically under valgrind on the hosted
    # software-rendering runner, chosen from three dispatch runs on this branch
    # (42622615853, 42627504561, 42632270739): the seven below passed every time;
    # testDecorationRecreationTraversal, testDecorationBridgeReentry and
    # testMaximizeRestoreRecursion each failed all retry attempts on at least one
    # runner (QTest event-loop deadlines under ~30x valgrind + llvmpipe; zero memory
    # errors every time). The excluded entries, like the fifo/tiles ones, stay
    # covered by the GPU-backed native lane and the functional CI, and must not
    # randomly turn the weekly gate red. Every attempt keeps its own log and is
    # reported; an unsuppressed memory finding is sticky and fails the gate.
    for run in "testXdgShellWindow testDecorationSettingsReentry" \
        "testXdgShellWindow testRequestedGeometryReentrantMove" \
        "testXdgShellWindow testGeometryCallbackClose" "testXdgShellWindow testGeometrySupersession" \
        "testXdgShellWindow testConfigureDiscardReentry" "testXdgShellWindow testConfigureContentHandoff" \
        "testHangDetectorShutdown"; do
        read -r binary function <<<"$run"
        code=0
        # Bounded retries, as for the functional suites, for intermittent QTest
        # timeouts under valgrind on the software-rendering runner. An
        # unsuppressed memory finding is sticky: memory errors can depend on
        # scheduling or heap layout, so any attempt reporting one fails the
        # entry immediately and is never retried away, and every attempt keeps
        # its own log.
        sticky=0
        for attempt in 1 2 3; do
            code=0
            log="$out/memcheck-$binary-$function-attempt$attempt.log"
            # shellcheck disable=SC2154 # dri is defined for test runs above
            podman run "${common[@]}" "${dri[@]}" "${supp[@]}" \
                -e HOME=/tmp/home -e XDG_CONFIG_HOME=/tmp/home/config -e XDG_CACHE_HOME=/tmp/home/cache \
                -e XDG_RUNTIME_DIR=/tmp/runtime -e LD_LIBRARY_PATH=/build/bin -e QT_QPA_PLATFORM=offscreen \
                -e QSG_RHI_BACKEND=opengl -e KWIN_FORCE_SW_CURSOR=1 -v "$work/empty-effects:/usr/share/kwin/effects:ro,z" \
                -v "$tree:/source:ro,z" -v "$bld:/build:rw,z" --entrypoint bash "$image" \
                -euc 'mkdir -p "$HOME" "$XDG_CONFIG_HOME" "$XDG_CACHE_HOME" "$XDG_RUNTIME_DIR" /tmp/.X11-unix
                  chmod 700 "$XDG_RUNTIME_DIR"
                  if [ ! -d /tmp/.X11-unix ]; then mkdir /tmp/.X11-unix; chmod 1777 /tmp/.X11-unix; fi
                  cd /build/bin
                  dbus-run-session valgrind --suppressions=/ci-known.supp --error-exitcode=91 --track-origins=yes --leak-check=no --num-callers=28 ./'"$binary $function" \
                >"$log" 2>&1 || code=$?
            if grep -qE 'ERROR SUMMARY: [1-9][0-9]* errors' "$log"; then
                sticky=1
            fi
            printf '%s %s attempt=%s exit=%s%s %s\n' "$binary" "$function" "$attempt" "$code" \
                "$([[ $sticky = 1 ]] && printf ' unsuppressed-memory-error')" \
                "$(grep -E 'ERROR SUMMARY' "$log" | tail -n 1 | sed -E 's/^==[0-9]+== //')" | tee -a "$out/memcheck.txt"
            (( sticky == 1 )) && break
            (( code == 0 )) && break
        done
        (( code == 0 && sticky == 0 )) || status=1
    done
    (( status == 0 )) || fail
    printf 'memcheck=0\n' >>"$out/STATUS"
fi
stage=identity
[ "$(git -C "$src" rev-parse HEAD 'HEAD^{tree}')" = "$(cat "$out/source-commit.txt")" ] || fail
[ "$(git -C "$tree" rev-parse HEAD 'HEAD^{tree}')" = "$(cat "$out/source-commit.txt")" ] || fail
[ -z "$(git -C "$tree" status --porcelain)" ] || fail
(cd "$bld/bin" && sha256sum --check "$out/binaries.sha256") >"$out/binaries-after.txt" || fail
printf 'done=0\n' >>"$out/STATUS"
