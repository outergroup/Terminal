#!/bin/bash
# Packages Terminal's release archives for Outer Shell's app catalog:
# build/release/Terminal/<variant>.tar.gz. Run Scripts/build_linux_resources.sh
# on each Linux architecture and libc first.
set -euo pipefail
export COPYFILE_DISABLE=1

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
PACKAGE_ROOT="${PACKAGE_ROOT:-${REPO_ROOT}/build/linux-package}"
OUTPUT_ROOT="${OUTPUT_ROOT:-${REPO_ROOT}/build/release}"
MACOS_BUILD_ROOT="${MACOS_BUILD_ROOT:-${REPO_ROOT}/build/macos}"
APP_CODESIGN_IDENTITY="${APP_CODESIGN_IDENTITY:-${OUTER_SHELL_CODESIGN_IDENTITY:-${CODE_SIGN_IDENTITY:--}}}"
APP_NOTARY_PROFILE="${APP_NOTARY_PROFILE:-${OUTER_SHELL_NOTARY_PROFILE:-}}"
WEB_ROOT="${REPO_ROOT}/Resources/TerminalWeb"

require_file() {
    if [[ ! -f "$1" ]]; then
        echo "error: missing $1" >&2
        exit 1
    fi
}

require_file "${REPO_ROOT}/VERSION"
APP_VERSION="$(tr -d '[:space:]' < "${REPO_ROOT}/VERSION")"
if [[ ! "${APP_VERSION}" =~ ^[0-9A-Za-z.+_-]+$ ]]; then
    echo "error: ${REPO_ROOT}/VERSION must hold a version such as 1.1.0" >&2
    exit 1
fi

for variant in RemoteLinuxBinaries/aarch64 RemoteLinuxBinaries/x86_64 RemoteLinuxBinariesMusl/aarch64 RemoteLinuxBinariesMusl/x86_64; do
    require_file "${PACKAGE_ROOT}/${variant}/TerminalBackend"
done
require_file "${REPO_ROOT}/app-icon.png"
require_file "${WEB_ROOT}/index.html"

sign_macos_app() {
    local path="$1"
    if [[ "${APP_CODESIGN_IDENTITY}" == "-" ]]; then
        /usr/bin/codesign --force --sign - --timestamp=none "$path" >/dev/null
    else
        /usr/bin/codesign --force --options runtime --timestamp --sign "${APP_CODESIGN_IDENTITY}" "$path" >/dev/null
    fi
}

notarize_macos_app_if_requested() {
    local path="$1"
    [[ -n "${APP_NOTARY_PROFILE}" ]] || return 0
    if [[ "${APP_CODESIGN_IDENTITY}" == "-" ]]; then
        echo "error: APP_NOTARY_PROFILE requires APP_CODESIGN_IDENTITY" >&2
        exit 1
    fi
    local archive="${STAGING_ROOT}/Terminal-notarize.zip"
    /usr/bin/ditto -c -k --norsrc --keepParent "$path" "$archive"
    xcrun notarytool submit "$archive" --keychain-profile "${APP_NOTARY_PROFILE}" --wait
    xcrun stapler staple "$path"
    rm -f "$archive"
}

echo "==> Building TerminalBackend for macOS"
mkdir -p "${MACOS_BUILD_ROOT}"
clang -std=gnu17 -Os -Wall -Wextra -arch arm64 -arch x86_64 -mmacosx-version-min=12.0 \
    "${REPO_ROOT}"/Backend/*.c -o "${MACOS_BUILD_ROOT}/TerminalBackend" -lpthread

STAGING_ROOT="$(mktemp -d)"
trap 'rm -rf "${STAGING_ROOT}"' EXIT
OUTPUT_APP_ROOT="${OUTPUT_ROOT}/Terminal"
rm -rf "${OUTPUT_APP_ROOT}"
mkdir -p "${OUTPUT_APP_ROOT}"

install_web() {
    local destination="$1"
    mkdir -p "${destination}"
    for file in index.html app.js style.css xterm.js xterm.css addon-fit.js LICENSE-xterm LICENSE-addon-fit; do
        install -m 0644 "${WEB_ROOT}/${file}" "${destination}/${file}"
    done
    install -m 0644 "${REPO_ROOT}/app-icon.png" "${destination}/app-icon.png"
}

package_linux_variant() {
    local arch="$1" libc="$2" output_name="$3"
    local binary_directory=RemoteLinuxBinaries
    [[ "${libc}" == musl ]] && binary_directory=RemoteLinuxBinariesMusl
    local app_root="${STAGING_ROOT}/Terminal"
    rm -rf "${app_root}"
    mkdir -p "${app_root}/${binary_directory}/${arch}"
    install_web "${app_root}/web"
    install -m 0644 "${REPO_ROOT}/app-icon.png" "${app_root}/app-icon.png"
    install -m 0755 "${PACKAGE_ROOT}/${binary_directory}/${arch}/TerminalBackend" "${app_root}/${binary_directory}/${arch}/TerminalBackend"
    printf '%s\n' "${APP_VERSION}" > "${app_root}/version"
    tar --format ustar --no-xattrs -C "${STAGING_ROOT}" -czf "${OUTPUT_APP_ROOT}/${output_name}.tar.gz" Terminal
    echo "Packaged ${OUTPUT_APP_ROOT}/${output_name}.tar.gz"
}

package_macos_variant() {
    local arch="$1" output_name="$2"
    local app_root="${STAGING_ROOT}/Terminal"
    local bundle="${app_root}/Terminal.app"
    rm -rf "${app_root}"
    mkdir -p "${bundle}/Contents/MacOS" "${bundle}/Contents/Resources"
    install_web "${app_root}/web"
    install -m 0644 "${REPO_ROOT}/app-icon.png" "${app_root}/app-icon.png"
    /usr/bin/lipo "${MACOS_BUILD_ROOT}/TerminalBackend" -thin "${arch}" -output "${bundle}/Contents/MacOS/TerminalBackend"
    chmod 0755 "${bundle}/Contents/MacOS/TerminalBackend"
    install -m 0644 "${REPO_ROOT}/app-icon.png" "${bundle}/Contents/Resources/app-icon.png"
    install_web "${bundle}/Contents/Resources/web"
    cat > "${bundle}/Contents/Info.plist" <<__TERMINAL_INFO_PLIST__
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleExecutable</key>
    <string>TerminalBackend</string>
    <key>CFBundleIdentifier</key>
    <string>org.outershell.Terminal</string>
    <key>CFBundleName</key>
    <string>Terminal</string>
    <key>CFBundlePackageType</key>
    <string>APPL</string>
    <key>CFBundleShortVersionString</key>
    <string>${APP_VERSION}</string>
    <key>CFBundleVersion</key>
    <string>${APP_VERSION}</string>
    <key>LSBackgroundOnly</key>
    <true/>
</dict>
</plist>
__TERMINAL_INFO_PLIST__
    sign_macos_app "${bundle}"
    notarize_macos_app_if_requested "${bundle}"
    printf '%s\n' "${APP_VERSION}" > "${app_root}/version"
    tar --format ustar --no-xattrs -C "${STAGING_ROOT}" -czf "${OUTPUT_APP_ROOT}/${output_name}.tar.gz" Terminal
    echo "Packaged ${OUTPUT_APP_ROOT}/${output_name}.tar.gz"
}

package_linux_variant aarch64 glibc linux-aarch64
package_linux_variant x86_64 glibc linux-x86_64
package_linux_variant aarch64 musl linux-aarch64-musl
package_linux_variant x86_64 musl linux-x86_64-musl
package_macos_variant arm64 macos-arm64
package_macos_variant x86_64 macos-x86_64
