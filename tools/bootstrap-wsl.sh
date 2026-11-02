#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# bootstrap-wsl.sh: installs the pinned FrostIDE Linux toolchain inside WSL
# (Ubuntu 24.04, the Linux floor from PLAN.md section 1.3).
#
# Two phases, because only the first needs root:
#   sudo-less root phase:  wsl -d Ubuntu-24.04 -u root -- bash tools/bootstrap-wsl.sh --system
#       apt packages: libstdc++ 14 (the floor), zstd, bsdtar, Qt's X11/xcb
#       runtime libraries, python3-venv, rsync, gdb.
#   user phase:            wsl -d Ubuntu-24.04 -- bash tools/bootstrap-wsl.sh
#       LLVM, CMake and Ninja from sdk/sdk.lock.json into ~/sdk (ext4, not
#       /mnt/e, because builds on the 9P mount are several times slower), Qt
#       through tools/qt-fetch.py, vcpkg at the pinned tag, ~/sdk/env.sh, and the
#       self-built lldb-dap: the official package's links a libpython Ubuntu 24.04
#       lacks (spike S5), so tools/build-lldb.sh builds it from the pinned LLVM
#       sources when it is missing (a long build) and checks its pinned hashes
#       otherwise (ADR 0049). FROST_SKIP_LLDB=1 skips that last step.
#
# Archives are taken from the Windows download cache (/mnt/e/sdk/_downloads)
# when present, otherwise downloaded. Every archive, Qt's included, is checked
# against the digest pinned in the lock file (SHA-512 for .NET, SHA-256 otherwise).
#
# Environment:
#   FROST_SDK            install root (default ~/sdk)
#   FROST_WIN_DOWNLOADS  Windows download cache to copy archives from (default /mnt/e/sdk/_downloads)
#   FROST_SKIP_LLDB      1 skips building or verifying lldb-dap

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
    # libarchive-tools: bsdtar, which tools/qt-fetch.py uses to extract Qt's .7z archives.
    # libxcb*, libxkbcommon*, libfontconfig, libgl/egl: Qt's xcb and offscreen platform plugins.
    apt-get install -y -q --no-install-recommends \
        g++-14 libstdc++-14-dev zstd xz-utils libarchive-tools zip unzip rsync gdb python3-venv pkg-config make \
        libgl-dev libegl-dev libglib2.0-0t64 libdbus-1-3 libfontconfig1 libfreetype6 \
        libxkbcommon0 libxkbcommon-x11-0 libx11-xcb1 libxcb1 libxcb-cursor0 libxcb-icccm4 \
        libxcb-image0 libxcb-keysyms1 libxcb-randr0 libxcb-render-util0 libxcb-shape0 \
        libxcb-xinerama0 libxcb-xkb1 libxcb-xinput0 libxcb-sync1 libxcb-xfixes0 libxrender1 \
        libsm6 libice6 curl ca-certificates git
    echo "system phase complete"
}

# Copies or downloads one archive from the lock file into $SDK/_downloads and
# verifies its pinned digest; prints the local path.
fetch_archive() {
    local name="$1" file url sha
    file="$(lock_get "linux.$name.file")"
    url="$(lock_get "linux.$name.url")"
    if [[ "$name" == dotnet ]]; then
        sha="$(lock_get "linux.$name.sha512")"
    else
        sha="$(lock_get "linux.$name.sha256")"
    fi
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
    local actual
    if [[ "$name" == dotnet ]]; then actual="$(sha512sum "$dest" | cut -d' ' -f1)";
    else actual="$(sha256sum "$dest" | cut -d' ' -f1)"; fi
    if [[ "$actual" != "$sha" ]]; then
        rm -f "$dest"
        echo "error: hash mismatch for $file (deleted)" >&2
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

# Verifies the server archive and all staged files before using the managed SDK tool.
install_managed() {
    local version url archive dest
    version="$(lock_get managedServer.version)"
    url="$(lock_get managedServer.url)"
    archive="$SDK/_downloads/csharp-ls.$version.nupkg"
    dest="$SDK/managed"
    mkdir -p "$SDK/_downloads"
    if [[ ! -f "$archive" ]]; then
        curl --fail --location --silent --show-error --retry 3 -o "$archive.part" "$url"
        mv "$archive.part" "$archive"
    fi
    if [[ -e "$dest" ]]; then
        python3 "$REPO_ROOT/tools/managed_server.py" --archive "$archive" --dest "$dest" --verify-only
    else
        python3 "$REPO_ROOT/tools/managed_server.py" --archive "$archive" --dest "$dest"
    fi
    echo "[managed] $version verified in $dest"
}

# Installs the pinned Qt subset for Linux with tools/qt-fetch.py, which reads
# the repository, archives and hashes from the lock file and extracts with bsdtar.
install_qt() {
    python3 -I "$REPO_ROOT/tools/qt-fetch.py" --lock "$LOCK" --platform linux \
        --dest "$SDK/qt" --cache "$SDK/_downloads/qt"
}

# Clones vcpkg at the pinned tag into $SDK/vcpkg and builds the vcpkg binary.
# When the pin moves, the checkout follows and the binary is rebuilt (the tool
# version is part of the tag). The tag must be vcpkg.json's builtin-baseline.
install_vcpkg() {
    local tag root="$SDK/vcpkg" current head baseline
    tag="$(lock_get vcpkg.tag)"
    if [[ ! -d "$root/.git" ]]; then
        git -c advice.detachedHead=false clone --quiet --depth 1 --branch "$tag" "$(lock_get vcpkg.repository)" "$root"
    else
        current="$(git -C "$root" describe --tags --exact-match 2> /dev/null || true)"
        if [[ "$current" != "$tag" ]]; then
            echo "[vcpkg] moving from '$current' to $tag"
            git -C "$root" fetch --quiet --depth 1 origin "refs/tags/$tag:refs/tags/$tag"
            git -C "$root" -c advice.detachedHead=false checkout --quiet "$tag"
            rm -f "$root/vcpkg"
        fi
    fi
    head="$(git -C "$root" rev-parse HEAD)"
    baseline="$(python3 -c 'import json, sys; print(json.load(open(sys.argv[1]))["builtin-baseline"])' "$REPO_ROOT/vcpkg.json")"
    if [[ "$head" != "$baseline" ]]; then
        echo "error: vcpkg tag $tag is commit $head, but vcpkg.json's builtin-baseline is $baseline; bump them together" >&2
        exit 1
    fi
    if [[ ! -x "$root/vcpkg" ]]; then "$root/bootstrap-vcpkg.sh" -disableMetrics; fi
    # bootstrap-vcpkg.sh can report missing prerequisites and still exit 0.
    if [[ ! -x "$root/vcpkg" ]]; then
        echo "error: vcpkg did not build; run the --system phase first" >&2
        exit 1
    fi
    echo "[vcpkg] ready at $root ($tag)"
}

# Verifies the self-built lldb-dap in $SDK/lldb-<version> against linux.lldb.files in the lock
# file, or builds it from the pinned LLVM sources first when it is not installed; both through
# tools/build-lldb.sh, which fails on a mismatch. Needs LLVM, CMake and Ninja, so it runs last.
install_lldb() {
    local version dest
    version="$(lock_get linux.lldb.version)"
    dest="$SDK/lldb-$version"
    if [[ -f "$dest/.frost-installed" ]]; then
        bash "$REPO_ROOT/tools/build-lldb.sh" --verify-only
        echo "[lldb] $version verified in $dest"
        return
    fi
    echo "[lldb] $version is not installed: building it from the pinned LLVM sources (a long build)"
    bash "$REPO_ROOT/tools/build-lldb.sh"
}

# Writes ~/sdk/env.sh, which puts the toolchain on PATH when sourced.
write_env() {
    local llvm cmake ninja qt dotnet
    llvm="$SDK/llvm-$(lock_get linux.llvm.version)"
    cmake="$SDK/cmake-$(lock_get linux.cmake.version)"
    ninja="$SDK/ninja-$(lock_get linux.ninja.version)"
    qt="$SDK/qt/$(lock_get qt.version)/gcc_64"
    dotnet="$SDK/dotnet-$(lock_get linux.dotnet.version)"
    cat > "$SDK/env.sh" <<EOF
# Generated by tools/bootstrap-wsl.sh; source it to use the pinned toolchain.
export FROST_SDK="$SDK"
export FROST_LLVM_ROOT="$llvm"
export FROST_QT_ROOT="$qt"
export DOTNET_ROOT="$dotnet"
export DOTNET_CLI_TELEMETRY_OPTOUT=1
export DOTNET_NOLOGO=1
export DOTNET_SKIP_FIRST_TIME_EXPERIENCE=1
export DOTNET_GENERATE_ASPNET_CERTIFICATE=0
export DOTNET_ADD_GLOBAL_TOOLS_TO_PATH=0
export DOTNET_CLI_HOME="$SDK/dotnet-home"
export NUGET_PACKAGES="$SDK/nuget"
export VCPKG_ROOT="$SDK/vcpkg"
export VCPKG_DISABLE_METRICS=1
export VCPKG_DEFAULT_BINARY_CACHE="$SDK/vcpkg-cache"
export PATH="$llvm/bin:$cmake/bin:$ninja:$dotnet:$SDK/vcpkg:\$PATH"
mkdir -p "\$VCPKG_DEFAULT_BINARY_CACHE"
EOF
    echo "wrote $SDK/env.sh"
}

if [[ "${1:-}" == "--system" ]]; then
    system_phase
    exit 0
fi

mkdir -p "$SDK"
for component in llvm cmake ninja dotnet; do install_archive "$component"; done
install_managed
install_qt
install_vcpkg
write_env
if [[ "${FROST_SKIP_LLDB:-0}" != "1" ]]; then install_lldb; fi
echo "user phase complete; source $SDK/env.sh"
