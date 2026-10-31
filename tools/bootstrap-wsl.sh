#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# bootstrap-wsl.sh: installs the pinned FrostIDE Linux toolchain inside WSL
# (Ubuntu 24.04, the Linux floor from PLAN.md section 1.3).
#
# Two phases, because only the first needs root:
#   sudo-less root phase:  wsl -d Ubuntu-24.04 -u root -- bash tools/bootstrap-wsl.sh --system
#       apt packages: libstdc++ 14 (the floor), zstd, Qt's X11/xcb runtime
#       libraries, python3-venv, rsync, gdb.
#   user phase:            wsl -d Ubuntu-24.04 -- bash tools/bootstrap-wsl.sh
#       LLVM, CMake and Ninja from sdk/sdk.lock.json into ~/sdk (ext4, not
#       /mnt/e, because builds on the 9P mount are several times slower), Qt
#       through tools/qt-fetch.py, vcpkg at the pinned tag, and ~/sdk/env.sh.
#
# Archives are taken from the Windows download cache (/mnt/e/sdk/_downloads)
# when present, otherwise downloaded; every archive is SHA-256 checked.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LOCK="$REPO_ROOT/sdk/sdk.lock.json"
SDK="${FROST_SDK:-$HOME/sdk}"
WIN_CACHE="${FROST_WIN_DOWNLOADS:-/mnt/e/sdk/_downloads}"

# Reads a value from the lock file with python3 (always present on Ubuntu),
# e.g. lock_get linux.llvm.version
lock_get() {
    python3 - "$LOCK" "$1" <<'PY'
import json, sys
value = json.load(open(sys.argv[1]))
for key in sys.argv[2].split("."):
    value = value[key]
print(",".join(value) if isinstance(value, list) else ("" if value is None else value))
PY
}

# Installs the apt packages FrostIDE needs to build and run its tests.
system_phase() {
    if [[ $EUID -ne 0 ]]; then
        echo "error: --system must run as root (wsl -u root)" >&2
        exit 1
    fi
    export DEBIAN_FRONTEND=noninteractive
    apt-get update -q
    # libstdc++-14-dev: the C++ standard library floor for clang.
    # zstd, xz-utils: decompressors for the pinned archives.
    # libxcb*, libxkbcommon*, libfontconfig, libgl/egl: Qt's xcb and offscreen platform plugins.
    apt-get install -y -q --no-install-recommends \
        g++-14 libstdc++-14-dev zstd xz-utils zip unzip rsync gdb python3-venv pkg-config make \
        libgl-dev libegl-dev libglib2.0-0t64 libdbus-1-3 libfontconfig1 libfreetype6 \
        libxkbcommon0 libxkbcommon-x11-0 libx11-xcb1 libxcb1 libxcb-cursor0 libxcb-icccm4 \
        libxcb-image0 libxcb-keysyms1 libxcb-randr0 libxcb-render-util0 libxcb-shape0 \
        libxcb-xinerama0 libxcb-xkb1 libxcb-xinput0 libxcb-sync1 libxcb-xfixes0 libxrender1 \
        libsm6 libice6 curl ca-certificates git
    echo "system phase complete"
}

# Copies or downloads one archive from the lock file into $SDK/_downloads and
# verifies its SHA-256; prints the local path.
fetch_archive() {
    local name="$1" file url sha
    file="$(lock_get "linux.$name.file")"
    url="$(lock_get "linux.$name.url")"
    sha="$(lock_get "linux.$name.sha256")"
    mkdir -p "$SDK/_downloads"
    local dest="$SDK/_downloads/$file"
    if [[ ! -f "$dest" ]]; then
        if [[ -f "$WIN_CACHE/$file" ]]; then
            cp "$WIN_CACHE/$file" "$dest.part"
        else
            curl --fail --location --silent --show-error --retry 3 -o "$dest.part" "$url"
        fi
        mv "$dest.part" "$dest"
    fi
    if [[ "$(sha256sum "$dest" | cut -d' ' -f1)" != "$sha" ]]; then
        rm -f "$dest"
        echo "error: SHA-256 mismatch for $file (deleted)" >&2
        exit 1
    fi
    echo "$dest"
}

# Extracts one archive component into $SDK/<name>-<version>, stripping a single
# top-level directory; skipped when the marker already names the pinned URL.
install_archive() {
    local name="$1" version url archive dest staging
    version="$(lock_get "linux.$name.version")"
    url="$(lock_get "linux.$name.url")"
    dest="$SDK/$name-$version"
    if [[ -f "$dest/.frost-installed" && "$(cat "$dest/.frost-installed")" == "$url" ]]; then
        echo "[$name] $version already installed"
        return
    fi
    archive="$(fetch_archive "$name")"
    staging="$SDK/_staging/$name"
    rm -rf "$staging" && mkdir -p "$staging"
    echo "[$name] extracting $version"
    case "$archive" in
        *.tar.zst) zstd -dc --long=31 --memory=2048MB "$archive" | tar -x -C "$staging" ;;
        *.tar.xz)  tar -xJf "$archive" -C "$staging" ;;
        *.tar.gz)  tar -xzf "$archive" -C "$staging" ;;
        *.zip)     unzip -q "$archive" -d "$staging" ;;
        *) echo "error: unknown archive type $archive" >&2; exit 1 ;;
    esac
    local entries=("$staging"/*)
    local source="$staging"
    if [[ ${#entries[@]} -eq 1 && -d "${entries[0]}" ]]; then source="${entries[0]}"; fi
    rm -rf "$dest"
    mv "$source" "$dest"
    rm -rf "$staging"
    echo "$url" > "$dest/.frost-installed"
    echo "[$name] installed to $dest"
}

# Installs the pinned Qt subset for Linux with tools/qt-fetch.py.
install_qt() {
    local venv="$SDK/py"
    if [[ ! -x "$venv/bin/python" ]]; then python3 -m venv "$venv"; fi
    "$venv/bin/python" -m pip install --disable-pip-version-check --quiet "py7zr==$(lock_get python.py7zr)"
    local pin
    pin="$(lock_get qt.linux.updatesSha256)"
    "$venv/bin/python" "$REPO_ROOT/tools/qt-fetch.py" \
        --repository "$(lock_get qt.linux.repository)" \
        --package "$(lock_get qt.linux.package)" \
        --archives "$(lock_get qt.linux.archives)" \
        --dest "$SDK/qt" --cache "$SDK/_downloads/qt" \
        ${pin:+--updates-sha256 "$pin"}
}

# Clones vcpkg at the pinned tag into $SDK/vcpkg and builds the vcpkg binary.
install_vcpkg() {
    local tag root="$SDK/vcpkg"
    tag="$(lock_get vcpkg.tag)"
    if [[ ! -d "$root/.git" ]]; then
        git -c advice.detachedHead=false clone --quiet --depth 1 --branch "$tag" "$(lock_get vcpkg.repository)" "$root"
    fi
    if [[ ! -x "$root/vcpkg" ]]; then "$root/bootstrap-vcpkg.sh" -disableMetrics; fi
    # bootstrap-vcpkg.sh can report missing prerequisites and still exit 0.
    if [[ ! -x "$root/vcpkg" ]]; then
        echo "error: vcpkg did not build; run the --system phase first" >&2
        exit 1
    fi
    echo "[vcpkg] ready at $root ($tag)"
}

# Writes ~/sdk/env.sh, which puts the toolchain on PATH when sourced.
write_env() {
    local llvm cmake ninja qt
    llvm="$SDK/llvm-$(lock_get linux.llvm.version)"
    cmake="$SDK/cmake-$(lock_get linux.cmake.version)"
    ninja="$SDK/ninja-$(lock_get linux.ninja.version)"
    qt="$SDK/qt/$(lock_get qt.version)/gcc_64"
    cat > "$SDK/env.sh" <<EOF
# Generated by tools/bootstrap-wsl.sh; source it to use the pinned toolchain.
export FROST_SDK="$SDK"
export FROST_LLVM_ROOT="$llvm"
export FROST_QT_ROOT="$qt"
export VCPKG_ROOT="$SDK/vcpkg"
export VCPKG_DISABLE_METRICS=1
export VCPKG_DEFAULT_BINARY_CACHE="$SDK/vcpkg-cache"
export PATH="$llvm/bin:$cmake/bin:$ninja:$SDK/vcpkg:\$PATH"
mkdir -p "\$VCPKG_DEFAULT_BINARY_CACHE"
EOF
    echo "wrote $SDK/env.sh"
}

if [[ "${1:-}" == "--system" ]]; then
    system_phase
    exit 0
fi

mkdir -p "$SDK"
for component in llvm cmake ninja; do install_archive "$component"; done
install_qt
install_vcpkg
write_env
echo "user phase complete; source $SDK/env.sh"
