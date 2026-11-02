#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# build-lldb.sh: builds the pinned lldb-dap for Linux (with lldb, lldb-server and liblldb, which it
# needs) from the pinned LLVM sources inside WSL and installs it into ~/sdk/lldb-<version>.
#
# The official LLVM 23.1.4 Linux package's lldb-dap links libpython3.14, which Ubuntu 24.04 lacks
# (spike S5), so U1's debugger engine is built locally, without Python (PLAN.md section 2,
# Debuggers; section 6.3, the SDK leg; ADR 0029 decision 12; ADR 0049). A local recipe, never a CI
# job (D24). The Windows counterpart is tools/build-lldb.ps1; both read sdk/sdk.lock.json.
#
# Steps, each logged to $FROST_SDK/_staging/lldb-<version>/<step>.log:
#   1. The source tarball (sources.llvm in the lock file) into $FROST_SDK/_downloads (kept there),
#      copied from the Windows download cache when it has it, otherwise downloaded; its SHA-256
#      must match the pin.
#   2. cmake, llvm, clang, lldb, third-party and libc without their test and unittest trees,
#      unpacked into $FROST_SDK/_staging/lldb-<version>/src on ext4.
#   3. CMake + Ninja, Release, with the pinned clang and clang++ against libstdc++ 14 (the
#      --gcc-install-dir of cmake/toolchains/linux-clang.cmake, passed directly: that file sets
#      CMAKE_SYSTEM_NAME, which would make LLVM configure a second NATIVE tree), ld.lld, llvm-ar
#      and llvm-ranlib from the pinned LLVM; clang and lldb, the X86 target; no Python, Lua,
#      libedit, curses, LZMA, libxml2, zlib, zstd, curl, telemetry or MCP protocol servers; no
#      tests, docs, examples or benchmarks.
#   4. `ninja distribution` (lldb, liblldb, lldb-dap, lldb-server, which LLDB uses to debug a local
#      process on Linux, and lldb-argdumper), with --jobs compile jobs (default 4) and --link-jobs
#      concurrent links (default 1), at nice 10: the WSL VM has 6 GB and is shared.
#   5. `install-distribution-stripped` into a staging prefix, then moved to
#      $FROST_SDK/lldb-<version> with a .frost-installed marker.
#   6. Every installed file's SHA-256 (and every symbolic link's target) is compared with
#      linux.lldb.files in the lock file; a difference fails the run and keeps the build tree.
#      With --print-pin the manifest is printed as the lock file's JSON instead.
#   7. The source and build trees are deleted unless --keep-build-tree is given.
# --verify-only runs step 6 alone on an existing install (tools/bootstrap-wsl.sh uses it).
#
# Usage:
#   bash tools/build-lldb.sh [--jobs 4] [--link-jobs 1] [--keep-build-tree] [--print-pin] [--verify-only]
# Environment:
#   FROST_SDK            install root (default ~/sdk)
#   FROST_WIN_DOWNLOADS  Windows download cache to copy the tarball from (default /mnt/e/sdk/_downloads)

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LOCK="$REPO_ROOT/sdk/sdk.lock.json"
SDK="${FROST_SDK:-$HOME/sdk}"
WIN_CACHE="${FROST_WIN_DOWNLOADS:-/mnt/e/sdk/_downloads}"
JOBS=4
LINK_JOBS=1
KEEP=0
PRINT_PIN=0
VERIFY_ONLY=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --jobs) JOBS="$2"; shift 2 ;;
        --link-jobs) LINK_JOBS="$2"; shift 2 ;;
        --keep-build-tree) KEEP=1; shift ;;
        --print-pin) PRINT_PIN=1; shift ;;
        --verify-only) VERIFY_ONLY=1; shift ;;
        *) echo "usage: build-lldb.sh [--jobs N] [--link-jobs N] [--keep-build-tree] [--print-pin] [--verify-only]" >&2
           exit 2 ;;
    esac
done

# Reads a value from the lock file, e.g. lock_get linux.lldb.version.
lock_get() {
    python3 - "$LOCK" "$1" <<'PY'
import json, sys
value = json.load(open(sys.argv[1]))
for key in sys.argv[2].split("."):
    value = value[key]
print(value)
PY
}

step() { printf '[lldb] %(%H:%M:%S)T %s\n' -1 "$*"; }

VERSION="$(lock_get linux.lldb.version)"
if [[ "$(lock_get sources.llvm.version)" != "$VERSION" ]]; then
    echo "error: sources.llvm and linux.lldb name different versions" >&2
    exit 1
fi
FILE="$(lock_get sources.llvm.file)"
URL="$(lock_get sources.llvm.url)"
SHA="$(lock_get sources.llvm.sha256)"
LLVM="$SDK/llvm-$(lock_get linux.llvm.version)"
CMAKE="$SDK/cmake-$(lock_get linux.cmake.version)/bin/cmake"
NINJA="$SDK/ninja-$(lock_get linux.ninja.version)/ninja"
GCC_DIR=/usr/lib/gcc/x86_64-linux-gnu/14
WORK="$SDK/_staging/lldb-$VERSION"
SRC="$WORK/src"
BUILD="$WORK/build"
PREFIX="$WORK/install"
DEST="$SDK/lldb-$VERSION"
COMPONENTS="lldb;liblldb;lldb-dap;lldb-server;lldb-argdumper"

# Step 6: compares every file and symbolic link under $DEST with linux.lldb.files (python3 reads the
# lock file and walks the tree) and fails on a difference; with --print-pin prints the manifest.
assert_pin() {
python3 - "$LOCK" "$DEST" "$PRINT_PIN" "$WORK" <<'PY'
import hashlib, json, os, sys
lock, root, print_pin, work = sys.argv[1], sys.argv[2], sys.argv[3] == "1", sys.argv[4]
found = {}
for base, dirs, files in os.walk(root):
    for name in dirs + files:
        path = os.path.join(base, name)
        rel = os.path.relpath(path, root)
        if os.path.islink(path):
            found[rel] = "symlink:" + os.readlink(path)
        elif os.path.isfile(path) and name != ".frost-installed":
            found[rel] = hashlib.sha256(open(path, "rb").read()).hexdigest()
found = dict(sorted(found.items()))
if print_pin:
    print('"files": ' + json.dumps(found, indent=2))
    sys.exit(0)
pinned = json.load(open(lock))["linux"]["lldb"].get("files", {})
wrong = [f"differs: {p} {v}" for p, v in found.items() if p in pinned and pinned[p] != v]
wrong += [f"not pinned: {p} {v}" for p, v in found.items() if p not in pinned]
wrong += [f"missing: {p}" for p in pinned if p not in found]
if wrong:
    print(f"error: {root} differs from linux.lldb.files (a build keeps its tree in {work}):", *wrong, sep="\n  ",
          file=sys.stderr)
    sys.exit(1)
print(f"[lldb] all {len(found)} files match the pin")
PY
}

if [[ $VERIFY_ONLY -eq 1 ]]; then
    [[ -f "$DEST/.frost-installed" ]] || { echo "error: lldb $VERSION is not installed in $DEST" >&2; exit 1; }
    assert_pin
    exit 0
fi

for tool in "$LLVM/bin/clang++" "$LLVM/bin/ld.lld" "$CMAKE" "$NINJA"; do
    [[ -x "$tool" ]] || { echo "error: $tool not found; run tools/bootstrap-wsl.sh first" >&2; exit 1; }
done
[[ -d "$GCC_DIR" ]] || { echo "error: $GCC_DIR missing; run bootstrap-wsl.sh --system" >&2; exit 1; }
free_gb=$(( $(df --output=avail -k "$SDK" | tail -1) / 1048576 ))
if (( free_gb < 20 )); then echo "error: $SDK has $free_gb GB free; need 20 GB" >&2; exit 1; fi
mkdir -p "$SDK/_downloads" "$WORK"
started=$SECONDS

# 1. Sources.
tarball="$SDK/_downloads/$FILE"
step "sources: $FILE"
if [[ ! -f "$tarball" ]]; then
    if [[ -f "$WIN_CACHE/$FILE" ]]; then cp "$WIN_CACHE/$FILE" "$tarball.part"
    else curl --fail --location --silent --show-error --retry 3 -o "$tarball.part" "$URL"; fi
    mv "$tarball.part" "$tarball"
fi
if [[ "$(sha256sum "$tarball" | cut -d' ' -f1)" != "$SHA" ]]; then
    rm -f "$tarball"
    echo "error: SHA-256 mismatch for $tarball (deleted)" >&2
    exit 1
fi

# 2. Unpack what the build reads.
top="llvm-project-$VERSION.src"
rm -rf "$SRC" "${WORK:?}/$top"
step "unpacking cmake, llvm, clang, lldb, third-party and libc (no tests)"
excludes=()
for dir in llvm clang lldb libc; do excludes+=(--exclude="$top/$dir/test" --exclude="$top/$dir/unittests"); done
tar -xJf "$tarball" -C "$WORK" "${excludes[@]}" \
    "$top/cmake" "$top/llvm" "$top/clang" "$top/lldb" "$top/third-party" "$top/libc" > "$WORK/unpack.log" 2>&1
mv "$WORK/$top" "$SRC"

# 3. Configure.
rm -rf "$BUILD"
step "configuring (log $WORK/configure.log)"
flags="--gcc-install-dir=$GCC_DIR"
nice -n 10 "$CMAKE" -G Ninja -S "$SRC/llvm" -B "$BUILD" \
    -DCMAKE_MAKE_PROGRAM="$NINJA" \
    -DCMAKE_C_COMPILER="$LLVM/bin/clang" -DCMAKE_CXX_COMPILER="$LLVM/bin/clang++" \
    -DCMAKE_AR="$LLVM/bin/llvm-ar" -DCMAKE_RANLIB="$LLVM/bin/llvm-ranlib" -DLLVM_USE_LINKER=lld \
    -DCMAKE_C_FLAGS="$flags" -DCMAKE_CXX_FLAGS="$flags" -DCMAKE_EXE_LINKER_FLAGS="$flags" \
    -DCMAKE_SHARED_LINKER_FLAGS="$flags" -DCMAKE_MODULE_LINKER_FLAGS="$flags" \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PREFIX" \
    -DPython3_EXECUTABLE=/usr/bin/python3 \
    -DLLVM_ENABLE_PROJECTS="clang;lldb" -DLLVM_TARGETS_TO_BUILD=X86 \
    -DLLVM_DISTRIBUTION_COMPONENTS="$COMPONENTS" -DLLVM_PARALLEL_LINK_JOBS="$LINK_JOBS" \
    -DLLVM_ENABLE_ASSERTIONS=OFF \
    -DLLVM_ENABLE_LIBXML2=OFF -DLLVM_ENABLE_ZLIB=OFF -DLLVM_ENABLE_ZSTD=OFF \
    -DLLVM_ENABLE_CURL=OFF -DLLVM_ENABLE_HTTPLIB=OFF -DLLVM_ENABLE_LIBEDIT=OFF -DLLVM_ENABLE_LIBPFM=OFF \
    -DLLVM_ENABLE_TELEMETRY=OFF -DLLVM_ENABLE_BINDINGS=OFF \
    -DLLVM_INCLUDE_TESTS=OFF -DLLVM_INCLUDE_BENCHMARKS=OFF -DLLVM_INCLUDE_EXAMPLES=OFF \
    -DLLVM_INCLUDE_DOCS=OFF -DCLANG_INCLUDE_TESTS=OFF -DCLANG_INCLUDE_DOCS=OFF \
    -DCLANG_ENABLE_STATIC_ANALYZER=OFF -DCLANG_PLUGIN_SUPPORT=OFF \
    -DLLDB_INCLUDE_TESTS=OFF -DLLDB_ENABLE_PYTHON=OFF -DLLDB_ENABLE_LUA=OFF \
    -DLLDB_ENABLE_SWIG=OFF -DLLDB_ENABLE_LIBEDIT=OFF -DLLDB_ENABLE_CURSES=OFF \
    -DLLDB_ENABLE_LZMA=OFF -DLLDB_ENABLE_LIBXML2=OFF -DLLDB_ENABLE_TREESITTER=OFF \
    -DLLDB_ENABLE_PROTOCOL_SERVERS=OFF > "$WORK/configure.log" 2>&1 \
    || { tail -40 "$WORK/configure.log" >&2; exit 1; }

# 4. Build.
step "building the distribution with -j$JOBS (log $WORK/build.log)"
nice -n 10 "$NINJA" -C "$BUILD" -j "$JOBS" distribution > "$WORK/build.log" 2>&1 \
    || { grep -A20 FAILED "$WORK/build.log" | head -60 >&2; exit 1; }

# 5. Install.
step "installing"
rm -rf "$PREFIX"
"$NINJA" -C "$BUILD" install-distribution-stripped > "$WORK/install.log" 2>&1 \
    || { tail -40 "$WORK/install.log" >&2; exit 1; }
for required in bin/lldb-dap bin/lldb bin/lldb-server; do
    [[ -x "$PREFIX/$required" ]] || { echo "error: the install has no $required" >&2; exit 1; }
done
rm -rf "$DEST"
mv "$PREFIX" "$DEST"
echo "$URL sha256:$SHA recipe:tools/build-lldb.sh" > "$DEST/.frost-installed"
step "installed to $DEST"

# 6. Compare with the pin.
assert_pin

# 7. Clean up.
if [[ $KEEP -eq 0 ]]; then
    step "deleting the source and build trees"
    rm -rf "$SRC" "$BUILD"
fi
step "done in $(( (SECONDS - started) / 60 )) min"
