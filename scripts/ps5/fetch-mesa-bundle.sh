#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Fetch the prebuilt Mesa GPU bundle for the WebProcess (src/ps5/mesa, EVO
# Player #237) into third_party/mesa-bundle/. It is one relocatable object,
# ps5_mesa_gpu.o, made by EVO Player's tools/build_mesa_bundle.sh from
# Prospero Browser's GPU stack (Mesa EGL/GLES/Zink + RADV MIT, PS5_Vulkan's
# broker client LGPL-2.1+, libps5platform GPL-3.0+; the revisions are in the
# bundle's MANIFEST-gpu-stack.txt). Pinned by SHA-256; a missing or wrong
# download leaves the WebProcess without Mesa (GLSlim and raster still build).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DEST="${MESA_BUNDLE_DIR:-${ROOT}/third_party/mesa-bundle}"
VERSION="${MESA_BUNDLE_VERSION:-mesa-bundle-1}"
FILE="ps5-mesa-bundle.tar.gz"
SHA256="${MESA_BUNDLE_SHA256:-2284ee6c53e2bb733f262cc61490781e8aa2afeceb21d2c6457e5b3e2fec4bba}"
URL="${MESA_BUNDLE_URL:-https://github.com/sainsaji/ProsperoWeb/releases/download/${VERSION}/${FILE}}"

log() { printf '[mesa-bundle] %s\n' "$*"; }

if [[ -f "${DEST}/.sha256" && "$(cat "${DEST}/.sha256")" == "${SHA256}" && -f "${DEST}/ps5_mesa_gpu.o" ]]; then
    log "already present (${VERSION})"
    exit 0
fi
tmp="$(mktemp -d)"
trap 'rm -rf "${tmp}"' EXIT
if ! curl -fsSL --retry 3 --max-time 600 -o "${tmp}/${FILE}" "${URL}"; then
    log "download failed: ${URL} (building without Mesa)"
    exit 0
fi
got="$(sha256sum "${tmp}/${FILE}" | cut -d' ' -f1)"
if [[ "${got}" != "${SHA256}" ]]; then
    log "checksum mismatch: got ${got}, want ${SHA256} (building without Mesa)"
    exit 0
fi
rm -rf "${DEST}"
mkdir -p "${DEST}"
tar xzf "${tmp}/${FILE}" -C "${DEST}"
echo "${SHA256}" > "${DEST}/.sha256"
log "${VERSION}: $(wc -c < "${DEST}/ps5_mesa_gpu.o") bytes -> ${DEST}"
