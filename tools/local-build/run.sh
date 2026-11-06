#!/usr/bin/env bash
# Host wrapper for the local engine build (Docker). See README.md.
#
#   tools/local-build/run.sh setup    first time (image, dependencies, WebKit)
#   tools/local-build/run.sh build    build and put the engine in output/local-build/dist
#   tools/local-build/run.sh sync     only copy the edited sources
#   tools/local-build/run.sh shell
#
# From Git Bash on Windows; MSYS_NO_PATHCONV keeps the container paths intact.
set -euo pipefail
export MSYS_NO_PATHCONV=1
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${HERE}/../.." && pwd)"
OUT="${PW_OUT:-${ROOT}/output/local-build}"
mkdir -p "${OUT}"
# docker.exe wants Windows paths for the build context and the mounts.
win() { (cd "$1" && (pwd -W 2>/dev/null || pwd)); }
HERE_W="$(win "${HERE}")"; ROOT_W="$(win "${ROOT}")"; OUT_W="$(win "${OUT}")"

docker build -q -t pw-builder -f "${HERE_W}/Dockerfile" "${HERE_W}" >/dev/null
flags=(-i)
[[ -t 0 ]] && flags=(-it)
exec docker run --rm "${flags[@]}" \
    -v pw-work:/work \
    -v "${ROOT_W}:/host:ro" \
    -v "${OUT_W}:/out" \
    -e JOBS="${JOBS:-6}" \
    pw-builder bash /host/tools/local-build/pw-build.sh "${1:-build}"
