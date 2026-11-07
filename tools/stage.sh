#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# stage.sh: stages a Linux build of FrostIDE into a folder that runs from where it is put (the
# Linux counterpart of tools/stage.ps1, R0-close item 7): the frost executable, its crash handler
# and baked resources, the licence files, and the pinned tools frost finds beside itself first
# (FND-10): clangd with its resource directory (llvm-<version>/bin, lib/clang), lldb-dap with
# lldb-argdumper and liblldb (lldb-<version>/bin, lib), and LuaLS in the layout the locator looks
# for (luals-<version>/bin/lua-language-server, main.lua and its scripts beside bin; ADR 0079).
# A missing tool fails the stage (FND-13). Versions come from sdk/sdk.lock.json ("linux").
#
#   tools/stage.sh --dest <dir> [--config Debug|Release|RelWithDebInfo] [--build-dir build/linux-clang]
#                  [--sdk-root <dir>] [--jobs N] [--skip-build] [--force]
#
# --sdk-root defaults to $FROST_SDK (~/sdk/env.sh sets it); there is no built-in default root. The
# destination must not exist or be empty; --force replaces it. Without --skip-build the frost target
# is built first (configuring the linux-clang preset when the build tree has none).
#
# Every check on the inputs (the build tree, the bundled tools) runs before the destination is
# touched, and a failure after that removes what this run created, so a failed stage never leaves
# a partial folder behind. After copying, every staged executable is compared byte for byte with
# its source, and `ldd` must resolve every shared library frost needs.
#
# Qt. frost links the pinned Qt of the SDK through its RUNPATH (an absolute path into
# $FROST_SDK/qt), so a Linux stage runs on a machine with that SDK installed; deploying Qt into the
# stage is not done yet (TODO(M5): the portable Linux package, PLAN.md "Packaging"). The stage
# prints the RUNPATH it relies on.
#
# Exit status: 0 staged, 1 a check failed (nothing partial is left), 2 usage. Linux only; bash 4+,
# coreutils, python3 (reads the lock), cmp and ldd.

set -euo pipefail

usage() {
    sed -n '12,13p' "$0" >&2
    exit 2
}

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
dest=""
config="Release"
build_dir="$REPO/build/linux-clang"
sdk_root="${FROST_SDK:-}"
jobs=""
skip_build=0
force=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --dest) dest="${2:-}"; shift 2 ;;
        --config) config="${2:-}"; shift 2 ;;
        --build-dir) build_dir="${2:-}"; shift 2 ;;
        --sdk-root) sdk_root="${2:-}"; shift 2 ;;
        --jobs) jobs="${2:-}"; shift 2 ;;
        --skip-build) skip_build=1; shift ;;
        --force) force=1; shift ;;
        -h|--help) usage ;;
        *) echo "stage: unknown argument '$1'" >&2; usage ;;
    esac
done
[[ -n "$dest" ]] || { echo "stage: --dest is required" >&2; usage; }
case "$config" in Debug|Release|RelWithDebInfo) ;; *) echo "stage: bad --config '$config'" >&2; usage ;; esac

# What this run created; fail() removes it again, also when a command fails unexpectedly (ERR).
created=""
created_contents=""
trap 'fail "a command failed on line $LINENO"' ERR
fail() {
    trap - ERR
    echo "stage: FAIL $*"
    if [[ -n "$created" && -e "$created" ]]; then
        rm -rf -- "$created" && echo "stage: removed the partial $created"
    elif [[ -n "$created_contents" && -d "$created_contents" ]]; then
        find "$created_contents" -mindepth 1 -maxdepth 1 -exec rm -rf -- {} + &&
            echo "stage: removed the partial contents of $created_contents"
    fi
    exit 1
}

absolute() { # An absolute, normalised path (the path need not exist).
    python3 -I -c 'import os, sys; print(os.path.abspath(sys.argv[1]))' "$1"
}
dest="$(absolute "$dest")"
build_dir="$(absolute "$build_dir")"
[[ -n "$sdk_root" ]] || fail "no SDK root: pass --sdk-root or set FROST_SDK (source ~/sdk/env.sh)"
sdk_root="$(absolute "$sdk_root")"

lock_version() { # The version sdk/sdk.lock.json pins for linux.<tool>.
    python3 -I -c 'import json, sys
lock = json.load(open(sys.argv[1], encoding="utf-8"))
print(lock["linux"][sys.argv[2]]["version"])' "$REPO/sdk/sdk.lock.json" "$1"
}
llvm_rel="llvm-$(lock_version llvm)"
lldb_rel="lldb-$(lock_version lldb)"
luals_rel="luals-$(lock_version luals)"

# 1. Build frost.
if [[ $skip_build -eq 0 ]]; then
    if [[ ! -f "$build_dir/build.ninja" ]]; then
        (cd "$REPO" && cmake --preset linux-clang) || fail "configure failed"
    fi
    build_args=(--build "$build_dir" --config "$config" --target frost)
    [[ -n "$jobs" ]] && build_args+=(-j "$jobs")
    cmake "${build_args[@]}" || fail "the $config build failed"
    echo "stage: built frost ($config)"
fi

# 2. The inputs, before the destination is touched.
bin_dir="$build_dir/src/app/$config"
for name in frost crashpad_handler; do
    [[ -f "$bin_dir/$name" ]] || fail "the build tree has no $name in $bin_dir"
done
[[ -d "$bin_dir/resources" ]] || fail "the build tree has no resources in $bin_dir"
for name in LICENSE NOTICE; do
    [[ -f "$REPO/$name" ]] || fail "the repository has no $name"
done
clangd="$sdk_root/$llvm_rel/bin/clangd"
clang_lib="$sdk_root/$llvm_rel/lib/clang"
[[ -f "$clangd" ]] || fail "missing bundled clangd: $clangd"
[[ -d "$clang_lib" ]] || fail "missing bundled clangd resource directory: $clang_lib"
for name in bin/lldb-dap bin/lldb-argdumper; do
    [[ -f "$sdk_root/$lldb_rel/$name" ]] || fail "missing bundled lldb-dap file: $sdk_root/$lldb_rel/$name"
done
liblldb=("$sdk_root/$lldb_rel"/lib/liblldb.so*)
[[ -e "${liblldb[0]}" ]] || fail "missing bundled lldb-dap file: $sdk_root/$lldb_rel/lib/liblldb.so"
for name in bin/lua-language-server main.lua LICENSE; do
    [[ -f "$sdk_root/$luals_rel/$name" ]] ||
        fail "missing bundled LuaLS file: $sdk_root/$luals_rel/$name (tools/bootstrap-wsl.sh)"
done

# 3. A fresh destination.
if [[ -e "$dest" && ! -d "$dest" ]]; then
    fail "$dest exists and is not a folder"
fi
if [[ -d "$dest" ]] && [[ -n "$(ls -A "$dest")" ]]; then
    [[ $force -eq 1 ]] || fail "$dest is not empty (use --force to replace it)"
    rm -rf -- "$dest"
fi
if [[ -d "$dest" ]]; then
    created_contents="$dest" # An empty folder the caller made stays; only its contents go.
else
    mkdir -p -- "$dest"
    created="$dest"
fi

# 4. FrostIDE's files and licences (the vendored sources' LICENSE files, named as stage.ps1 does).
cp -- "$bin_dir/frost" "$bin_dir/crashpad_handler" "$dest/" || fail "cannot copy frost"
cp -R -- "$bin_dir/resources" "$dest/resources" || fail "cannot copy the resources"
cp -- "$REPO/LICENSE" "$REPO/NOTICE" "$dest/" || fail "cannot copy the licences"
mkdir -p -- "$dest/licenses"
while IFS= read -r -d '' file; do
    relative="${file#"$REPO/third_party/"}"
    target="$(dirname "$relative" | tr '/' '-')-$(basename "$relative")"
    cp -- "$file" "$dest/licenses/${target#.-}" || fail "cannot copy $file"
done < <(find "$REPO/third_party" -type f -name 'LICENSE*' -print0)
mkdir -p -- "$dest/unity"
cp -R -- "$REPO/integrations/unity/com.frostide.editor" "$dest/unity/" || fail "cannot copy the Unity package"

# 5. The bundled tools.
mkdir -p -- "$dest/$llvm_rel/bin" "$dest/$llvm_rel/lib" "$dest/$lldb_rel/bin" "$dest/$lldb_rel/lib"
cp -- "$clangd" "$dest/$llvm_rel/bin/" || fail "cannot copy clangd"
cp -R -- "$clang_lib" "$dest/$llvm_rel/lib/" || fail "cannot copy the clangd resource directory"
cp -- "$sdk_root/$lldb_rel/bin/lldb-dap" "$sdk_root/$lldb_rel/bin/lldb-argdumper" "$dest/$lldb_rel/bin/" ||
    fail "cannot copy lldb-dap"
cp -P -- "${liblldb[@]}" "$dest/$lldb_rel/lib/" || fail "cannot copy liblldb" # -P keeps the soname links.
# LuaLS as installed, without the install marker or a log a run left behind (as stage.ps1 does).
cp -R -- "$sdk_root/$luals_rel" "$dest/$luals_rel" || fail "cannot copy LuaLS"
rm -rf -- "$dest/$luals_rel/.frost-installed" "$dest/$luals_rel/log"
cp -- "$sdk_root/$luals_rel/LICENSE" "$dest/licenses/luals-LICENSE.txt"

# 6. The checks: every staged executable is its source's bytes, and frost's libraries resolve.
for pair in "frost:$bin_dir/frost" "crashpad_handler:$bin_dir/crashpad_handler" \
            "$llvm_rel/bin/clangd:$clangd" "$lldb_rel/bin/lldb-dap:$sdk_root/$lldb_rel/bin/lldb-dap" \
            "$luals_rel/bin/lua-language-server:$sdk_root/$luals_rel/bin/lua-language-server"; do
    staged="$dest/${pair%%:*}"
    [[ -x "$staged" ]] || fail "the staged $staged is missing or not executable"
    cmp -s -- "$staged" "${pair#*:}" || fail "the staged ${pair%%:*} differs from ${pair#*:}"
done
if ! libraries="$(ldd "$dest/frost" 2>&1)"; then
    fail "ldd cannot read the staged frost: $libraries"
fi
if grep -q 'not found' <<<"$libraries"; then
    fail "the staged frost needs libraries that do not resolve: $(grep 'not found' <<<"$libraries" | sed 's/^[[:space:]]*//' | paste -sd ' ' -)"
fi
runpath="$( (readelf -d "$dest/frost" 2>/dev/null || true) | sed -n 's/.*(RUNPATH).*\[\(.*\)\]/\1/p')"
[[ -z "$runpath" ]] || echo "stage: frost loads Qt through its RUNPATH $runpath (not staged)"

files="$(find "$dest" -type f | wc -l)"
bytes="$(du -sb "$dest" | cut -f1)"
echo "stage: ok $dest ($files files, $((bytes / 1048576)) MB)"
exit 0
