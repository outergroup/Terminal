#!/bin/bash
# Terminal development tasks. Run `./app help` for the list.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=/dev/null
source "${ROOT}/app.env"
BUILD_DIR="${ROOT}/build"
STAGE_PARENT="${BUILD_DIR}/stage"
STAGE_ROOT="${STAGE_PARENT}/${APP_NAME}"

shell_quote() {
    local value="$1"
    printf "'%s'" "${value//\'/\'\\\'\'}"
}

load_target() {
    TARGET_KIND=local
    OUTER_TARGET_SSH=()
    if [[ -f "${ROOT}/target.env" ]]; then
        # shellcheck source=/dev/null
        source "${ROOT}/target.env"
        TARGET_KIND="${OUTER_TARGET_KIND:-local}"
    fi
    if [[ "${1:-}" == --local ]]; then
        TARGET_KIND=local
    fi
}

run_ssh() {
    "${OUTER_TARGET_SSH[@]}" "$@"
}

cmd_target() {
    if [[ "${1:-}" == local ]]; then
        printf 'OUTER_TARGET_KIND=local\n' > "${ROOT}/target.env"
    elif [[ $# -eq 1 && "$1" == ssh* ]]; then
        read -r -a words <<< "$1"
        {
            printf 'OUTER_TARGET_KIND=ssh\nOUTER_TARGET_SSH=(\n'
            for word in "${words[@]}"; do printf '    %s\n' "$(shell_quote "${word}")"; done
            printf ')\n'
        } > "${ROOT}/target.env"
    else
        echo "usage: ./app target local | ./app target \"ssh [options] host\"" >&2
        exit 2
    fi
    echo "Saved deploy target in target.env."
}

stage_common() {
    rm -rf "${STAGE_PARENT}"
    mkdir -p "${STAGE_ROOT}"
    "${ROOT}/Scripts/stage_web.sh" "${STAGE_ROOT}/web"
    cp "${ROOT}/app-icon.png" "${STAGE_ROOT}/app-icon.png"
}

stage_macos() {
    stage_common
    local bundle="${STAGE_ROOT}/${APP_NAME}.app"
    mkdir -p "${bundle}/Contents/MacOS" "${bundle}/Contents/Resources"
    echo "==> Building ${BACKEND_EXECUTABLE_NAME} for macOS"
    clang -std=gnu17 -Os -Wall -Wextra -mmacosx-version-min=12.0 \
        "${ROOT}"/Backend/*.c -o "${bundle}/Contents/MacOS/${BACKEND_EXECUTABLE_NAME}" -lpthread
    cp "${ROOT}/app-icon.png" "${bundle}/Contents/Resources/app-icon.png"
    "${ROOT}/Scripts/stage_web.sh" "${bundle}/Contents/Resources/web"
    /usr/bin/codesign --force --sign - --timestamp=none "${bundle}/Contents/MacOS/${BACKEND_EXECUTABLE_NAME}" >/dev/null
}

stage_linux() {
    local architecture="$1"
    local platform
    case "${architecture}" in
        aarch64|arm64) architecture=aarch64; platform=linux/arm64 ;;
        x86_64|amd64) architecture=x86_64; platform=linux/amd64 ;;
        *) echo "error: unsupported architecture ${architecture}" >&2; exit 1 ;;
    esac
    stage_common
    mkdir -p "${STAGE_ROOT}/RemoteLinuxBinariesMusl/${architecture}" "${STAGE_ROOT}/RemoteLinuxBinaries/${architecture}"
    echo "==> Building ${BACKEND_EXECUTABLE_NAME} for ${platform} (static musl)"
    docker run --rm --platform "${platform}" -v "${ROOT}:/src:ro" -v "${STAGE_ROOT}/RemoteLinuxBinariesMusl/${architecture}:/out" alpine:3.20 sh -c \
        "apk add -q gcc musl-dev linux-headers >/dev/null && gcc -std=gnu17 -Os -Wall -Wextra -static /src/Backend/*.c -o /out/${BACKEND_EXECUTABLE_NAME} -lpthread && strip /out/${BACKEND_EXECUTABLE_NAME}"
    # A static binary runs on glibc systems too.
    cp "${STAGE_ROOT}/RemoteLinuxBinariesMusl/${architecture}/${BACKEND_EXECUTABLE_NAME}" "${STAGE_ROOT}/RemoteLinuxBinaries/${architecture}/"
}

local_outerctl() {
    printf '%s\n' "${HOME}/Library/Application Support/outershell/bin/outerctl"
}

remote_outerctl() {
    printf '%s' 'outerctl="${XDG_STATE_HOME:-$HOME/.local/state}/outershell/bin/outerctl"; '
    printf '%s' '[ -x "$outerctl" ] || outerctl="$(command -v outerctl || true)"; '
    printf '%s' '[ -n "$outerctl" ] || { echo "error: install Outer Shell on this server first" >&2; exit 1; }; '
}

cmd_deploy() {
    load_target "${1:-}"
    if [[ "${TARGET_KIND}" == local ]]; then
        [[ "$(uname -s)" == Darwin ]] || { echo "error: local deployment requires macOS; use an SSH target" >&2; exit 1; }
        stage_macos
        echo "==> Installing ${APP_NAME} through Outer Shell"
        "$(local_outerctl)" bundled-app install --backend "${APP_ID}" --scope user --stage-root "${STAGE_ROOT}"
    else
        local os architecture
        os="$(run_ssh uname -s </dev/null)"
        architecture="$(run_ssh uname -m </dev/null)"
        [[ "${os}" == Linux ]] || { echo "error: SSH deployment supports Linux targets" >&2; exit 1; }
        stage_linux "${architecture}"
        local remote_parent=".cache/outershell-app-deploy/${APP_ID}"
        echo "==> Uploading ${APP_NAME}"
        COPYFILE_DISABLE=1 tar czf - -C "${STAGE_PARENT}" "${APP_NAME}" | run_ssh "
            set -e
            rm -rf \"\$HOME/${remote_parent}\"
            mkdir -p \"\$HOME/${remote_parent}\"
            tar xzf - -C \"\$HOME/${remote_parent}\" 2>/dev/null
        "
        echo "==> Installing ${APP_NAME} through Outer Shell"
        run_ssh "$(remote_outerctl)\"\$outerctl\" bundled-app install --backend $(shell_quote "${APP_ID}") --scope user --stage-root \"\$HOME/${remote_parent}/${APP_NAME}\"" </dev/null
    fi
    echo "Deployed ${APP_NAME}. Open the server in Outer Loop; it is in the app list."
}

cmd_uninstall() {
    load_target "${1:-}"
    if [[ "${TARGET_KIND}" == local ]]; then
        "$(local_outerctl)" bundled-app uninstall --backend "${APP_ID}" --scope user
    else
        run_ssh "$(remote_outerctl)\"\$outerctl\" bundled-app uninstall --backend $(shell_quote "${APP_ID}") --scope user" </dev/null
    fi
    echo "Uninstalled ${APP_NAME}. Terminal sessions keep running until they are closed."
}

cmd_run() {
    local web="${BUILD_DIR}/run/web"
    rm -rf "${BUILD_DIR}/run"
    "${ROOT}/Scripts/stage_web.sh" "${web}"
    cc -std=gnu17 -O1 -Wall -Wextra "${ROOT}"/Backend/*.c -o "${BUILD_DIR}/run/${BACKEND_EXECUTABLE_NAME}" -lpthread
    exec "${BUILD_DIR}/run/${BACKEND_EXECUTABLE_NAME}" --port "${PORT:-7356}" --web-root "${web}"
}

usage() {
    cat <<'EOF'
Terminal development tasks

  ./app target local        deploy to this Mac (the default)
  ./app target "ssh ..."    deploy to a Linux server over SSH
  ./app run                 run the backend on 127.0.0.1:$PORT (default 7356)
  ./app deploy [--local]    build and install through Outer Shell
  ./app uninstall [--local] uninstall the app
  ./app test                run the backend tests
  ./app clean               remove build products

Release archives for Outer Shell's app catalog come from
Scripts/build_linux_resources.sh (on each Linux architecture and libc) and
Scripts/package_release.sh. The Windows build is part of Outer Shell's.
EOF
}

case "${1:-help}" in
    target) shift; cmd_target "$@" ;;
    run) cmd_run ;;
    deploy) shift; cmd_deploy "$@" ;;
    uninstall) shift; cmd_uninstall "$@" ;;
    test) python3 "${ROOT}/Scripts/test_backend.py" ;;
    clean) rm -rf "${BUILD_DIR}" ;;
    help|-h|--help) usage ;;
    *) usage >&2; exit 2 ;;
esac
