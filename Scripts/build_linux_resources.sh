#!/bin/bash
# Builds TerminalBackend for this Linux machine's architecture and libc into
# build/linux-package, where Scripts/package_release.sh expects it.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
# shellcheck source=/dev/null
source "${REPO_ROOT}/app.env"

case "$(uname -m)" in
    aarch64|arm64) ARCH=aarch64 ;;
    x86_64|amd64) ARCH=x86_64 ;;
    *) echo "error: unsupported Linux architecture: $(uname -m)" >&2; exit 1 ;;
esac

LIBC="${OUTER_APP_LINUX_LIBC:-}"
if [[ -z "${LIBC}" ]]; then
    if [[ -e "/lib/ld-musl-$(uname -m).so.1" ]] || (command -v ldd >/dev/null 2>&1 && ldd --version 2>&1 | grep -qi musl); then
        LIBC=musl
    else
        LIBC=glibc
    fi
fi
case "${LIBC}" in
    glibc) BINARY_DIRECTORY=RemoteLinuxBinaries; LINK_FLAGS="-lpthread" ;;
    musl) BINARY_DIRECTORY=RemoteLinuxBinariesMusl; LINK_FLAGS="-static -lpthread" ;;
    *) echo "error: OUTER_APP_LINUX_LIBC must be glibc or musl" >&2; exit 1 ;;
esac

OUTPUT_DIR="${REPO_ROOT}/build/linux-package/${BINARY_DIRECTORY}/${ARCH}"
mkdir -p "${OUTPUT_DIR}"
# shellcheck disable=SC2086
cc -std=gnu17 -Os -Wall -Wextra -o "${OUTPUT_DIR}/${BACKEND_EXECUTABLE_NAME}" "${REPO_ROOT}"/Backend/*.c ${LINK_FLAGS}
if command -v strip >/dev/null 2>&1; then
    strip --strip-unneeded "${OUTPUT_DIR}/${BACKEND_EXECUTABLE_NAME}" || true
fi
echo "Built ${BACKEND_EXECUTABLE_NAME} Linux/${ARCH}/${LIBC}"
