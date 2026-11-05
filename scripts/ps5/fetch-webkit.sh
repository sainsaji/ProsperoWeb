#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Fetch upstream WebKit at the pinned commit into third_party/webkit (sparse,
# blobless) and apply the PS5 port patches from webkit-port/patches/.
#
# Idempotent: re-running with an already patched tree does nothing. If the
# patch set changed since it was applied, set WEBKIT_RESET=1 to discard local
# changes in third_party/webkit and re-apply.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
WEBKIT_DIR="${WEBKIT_DIR:-${ROOT}/third_party/webkit}"
WEBKIT_URL="${WEBKIT_URL:-https://github.com/WebKit/WebKit.git}"
WEBKIT_COMMIT="${WEBKIT_COMMIT:-e049cc7f923ea377af4d92877f9eedaf4ed5a1c0}"
PATCH_DIR="${ROOT}/webkit-port/patches"
STAMP="${WEBKIT_DIR}/.ps5-patches.stamp"

log() { printf '[fetch-webkit] %s\n' "$*"; }

patches_hash() {
    (cd "${PATCH_DIR}" && cat -- *.patch 2>/dev/null) | sha256sum | cut -d' ' -f1
}

clone() {
    log "cloning ${WEBKIT_URL} @ ${WEBKIT_COMMIT}"
    mkdir -p "${WEBKIT_DIR}"
    git -C "${WEBKIT_DIR}" init -q
    git -C "${WEBKIT_DIR}" remote add origin "${WEBKIT_URL}"
    git -C "${WEBKIT_DIR}" config remote.origin.promisor true
    git -C "${WEBKIT_DIR}" config remote.origin.partialclonefilter blob:none
    git -C "${WEBKIT_DIR}" sparse-checkout set --no-cone \
        '/Source/' '/Tools/' '/CMakeLists.txt' '/*.*' '!/Source/ThirdParty/libwebrtc/'
    git -C "${WEBKIT_DIR}" fetch -q --depth 1 --filter=blob:none origin "${WEBKIT_COMMIT}"
    git -C "${WEBKIT_DIR}" checkout -q --detach FETCH_HEAD
}

if [[ ! -d "${WEBKIT_DIR}/.git" ]]; then
    clone
fi

head="$(git -C "${WEBKIT_DIR}" rev-parse HEAD)"
if [[ "${head}" != "${WEBKIT_COMMIT}" ]]; then
    log "error: ${WEBKIT_DIR} is at ${head}, expected ${WEBKIT_COMMIT}" >&2
    exit 1
fi

want="$(patches_hash)"
if [[ -f "${STAMP}" && "$(cat "${STAMP}")" == "${want}" ]]; then
    log "patches already applied (${want:0:12})"
    exit 0
fi

if [[ -n "$(git -C "${WEBKIT_DIR}" status --porcelain --untracked-files=normal -- Source Tools CMakeLists.txt)" ]]; then
    if [[ "${WEBKIT_RESET:-0}" != "1" ]]; then
        log "error: ${WEBKIT_DIR} has local changes and the patch stamp does not match." >&2
        log "       Re-run with WEBKIT_RESET=1 to discard them and re-apply the patch set." >&2
        exit 1
    fi
    log "resetting local changes in ${WEBKIT_DIR}"
    git -C "${WEBKIT_DIR}" checkout -q -f -- .
    git -C "${WEBKIT_DIR}" clean -fdq -- Source Tools
fi

shopt -s nullglob
patches=("${PATCH_DIR}"/*.patch)
# In order, each checked against the tree the earlier ones left: a later patch
# may build on an earlier one's lines (0022 on 0013). A failure leaves no
# stamp, so the next run asks for WEBKIT_RESET=1.
for p in "${patches[@]}"; do
    log "applying $(basename "${p}")"
    git -C "${WEBKIT_DIR}" apply --check "${p}"
    git -C "${WEBKIT_DIR}" apply "${p}"
done
echo "${want}" > "${STAMP}"
log "done (${#patches[@]} patches, ${want:0:12})"
