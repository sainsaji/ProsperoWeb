#!/usr/bin/env bash
# Runs INSIDE the pw-builder container (tools/local-build/run.sh starts it).
#
#   pw-build.sh setup      one time: copy the repo, dependencies, fetch + patch WebKit
#   pw-build.sh sync       copy edited sources (the repo, and the WebKit files patch 0022 touches)
#   pw-build.sh build      sync, then WebKit + frontend + package; engine lands in /out
#   pw-build.sh shell      a shell in the work tree
#
# /host is the working copy of the fork (read-only), /work a Docker volume
# (ext4, so ninja is fast) holding the copy that is built, /out where
# prosperoweb.elf is delivered. Build steps mirror .github/workflows/ci.yml.
set -euo pipefail

SRC=/work/src
HOST=/host
export PS5_PAYLOAD_SDK="${PS5_PAYLOAD_SDK:-/opt/ps5-payload-sdk}"
export JOBS="${JOBS:-6}"
export ICU_JOBS="${ICU_JOBS:-3}"
export ADDONS_ALLOW_UNPINNED=1
export CCACHE_DIR="${CCACHE_DIR:-/work/ccache}"
mkdir -p "${CCACHE_DIR}" "${SRC}" /out

log() { printf '[pw-build] %s\n' "$*"; }

sync_repo() {
    # The last COMMIT of the working copy, from git (LF line endings: the
    # Windows checkout has CRLF, which breaks shell scripts and patches).
    # Commit your changes before building; WebKit files are synced below.
    log "syncing the repo (git archive HEAD)"
    git config --global --add safe.directory "${HOST}"
    git -C "${HOST}" archive --format=tar HEAD | tar -x -C "${SRC}"
}

sync_webkit() {
    # The WebKit files the 0022 patch edits, from the working copy: what is
    # being iterated on. (The rest of the tree is what fetch-webkit.sh made.)
    [[ -f "${SRC}/third_party/webkit/.ps5-patches.stamp" ]] || { log "no WebKit yet: run setup"; exit 1; }
    local f n=0
    while read -r f; do
        if ! cmp -s "${HOST}/third_party/webkit/${f}" "${SRC}/third_party/webkit/${f}"; then
            tr -d '\r' < "${HOST}/third_party/webkit/${f}" > "${SRC}/third_party/webkit/${f}"
            log "  updated ${f}"
            n=$((n + 1))
        fi
    done < <(grep '^+++ b/' "${HOST}/webkit-port/patches/0022-webkit-ps5-glslim-compositing.patch" | sed 's#^+++ b/##')
    log "${n} WebKit file(s) updated"
}

case "${1:-build}" in
setup)
    sync_repo
    cd "${SRC}"
    log "dependencies"
    scripts/ps5/build-deps.sh
    rm -rf build-deps
    log "fetching and patching WebKit"
    scripts/ps5/fetch-webkit.sh
    log "setup done; now: pw-build.sh build"
    ;;
sync)
    sync_repo
    sync_webkit
    ;;
build)
    sync_repo
    sync_webkit
    cd "${SRC}"
    ccache --zero-stats >/dev/null || true
    log "WebKit"
    scripts/ps5/build-webkit.sh all
    ccache --show-stats | head -8 || true
    log "frontend"
    scripts/ps5/fetch-mesa-bundle.sh
    cmake -S . -B build-ps5 \
        -DCMAKE_TOOLCHAIN_FILE="${PS5_PAYLOAD_SDK}/toolchain/prospero.cmake" \
        -DPROSPEROWEB_FRONTEND=ps5 -DPROSPEROWEB_BUILD_TESTS=OFF -DCMAKE_BUILD_TYPE=Release
    cmake --build build-ps5 -j"${JOBS}" --target prosperoweb
    cmake --build build-ps5 -j"${JOBS}" --target prosperoweb_native prosperoweb_broker
    log "package"
    scripts/ps5/package.sh
    rm -rf /out/dist && cp -a dist /out/dist
    ls -l /out/dist/prosperoweb/prosperoweb.elf
    ;;
shell)
    cd "${SRC}"
    exec bash
    ;;
*)
    echo "usage: pw-build.sh setup|sync|build|shell" >&2
    exit 2
    ;;
esac
