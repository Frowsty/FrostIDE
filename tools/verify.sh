#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# verify.sh: the Linux half of FrostIDE's local verification (there is no CI;
# PLAN.md section 6.4). Run inside WSL:
#
#   wsl -d Ubuntu-24.04 -- bash /mnt/e/dev/FrostIDE/tools/verify.sh [--full]
#
# It mirrors the working tree onto ext4 (~/frost-src; building on the /mnt/e
# 9P mount is several times slower), then configures, builds and tests:
#   default  linux-clang Debug (clang + libstdc++ 14, the Linux floor)
#   --full   additionally ASan+UBSan (Debug) and TSan (RelWithDebInfo)
#
# The last line of output is a summary for the commit verification footer,
# e.g. "wsl: debug 45/45, asan 45/45, tsan 45/45". Exit status is non-zero if
# any configuration fails to build or has a failing test.

set -euo pipefail

FULL=0
for arg in "$@"; do
    case "$arg" in
        --full) FULL=1 ;;
        *) echo "usage: verify.sh [--full]" >&2; exit 2 ;;
    esac
done

SOURCE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MIRROR="${FROST_MIRROR:-$HOME/frost-src}"
# shellcheck source=/dev/null
source "${FROST_SDK:-$HOME/sdk}/env.sh"

# Mirror the tree. build/ and .git/ stay out: the mirror keeps its own build
# trees (protected from --delete because they are excluded).
mkdir -p "$MIRROR"
rsync -a --delete --exclude '/build/' --exclude '/.git/' "$SOURCE/" "$MIRROR/"
cd "$MIRROR"

summary=()
status=0

# Configures, builds and tests one preset pair; appends "<label> passed/total" to the summary.
run_config() {
    local label="$1" configure="$2" preset="$3"
    echo "=== $label: configure ($configure)"
    if ! cmake --preset "$configure" > "build-$label.log" 2>&1; then
        tail -30 "build-$label.log"
        summary+=("$label configure-failed"); status=1; return
    fi
    echo "=== $label: build ($preset)"
    if ! cmake --build --preset "$preset" -j "$(nproc)" >> "build-$label.log" 2>&1; then
        grep -E 'error|FAILED' "build-$label.log" | head -40
        summary+=("$label build-failed"); status=1; return
    fi
    echo "=== $label: test ($preset)"
    local out
    out="$(ctest --preset "$preset" 2>&1)" || status=1
    echo "$out" | tail -15
    # ctest prints "100% tests passed out of 45" or "98% tests passed, 1 tests failed out of 45".
    local line total failed=0
    line="$(echo "$out" | grep -E 'tests passed.* out of [0-9]+' | tail -1)"
    if [[ -z "$line" ]]; then
        summary+=("$label no-results"); status=1; return
    fi
    total="$(echo "$line" | sed -E 's/.*out of ([0-9]+).*/\1/')"
    if [[ "$line" =~ ([0-9]+)\ tests\ failed ]]; then failed="${BASH_REMATCH[1]}"; fi
    summary+=("$label $((total - failed))/$total")
}

run_config debug linux-clang linux-clang-debug
if [[ $FULL -eq 1 ]]; then
    run_config asan linux-clang-asan linux-clang-asan
    run_config tsan linux-clang-tsan linux-clang-tsan
fi

echo "wsl: $(IFS=,; echo "${summary[*]}" | sed 's/,/, /g')"
exit $status
