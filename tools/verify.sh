#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# verify.sh: the Linux half of FrostIDE's local verification (there is no CI;
# PLAN.md section 6.4). Run inside WSL:
#
#   wsl -d Ubuntu-24.04 --exec bash /mnt/e/dev/FrostIDE/tools/verify.sh [--quick | --full] [--no-checks]
#
# It mirrors the working tree onto ext4 (building on the /mnt/e 9P mount is
# several times slower), then configures, builds and tests:
#   --quick  (default) linux-clang Debug (clang + libstdc++ 14, the Linux floor)
#   --full   additionally ASan+UBSan (Debug) and TSan (RelWithDebInfo)
# The PowerShell spellings -Quick and -Full are accepted too.
#
# Tests labelled `stress` (long concurrency runs) are skipped in the Debug
# configuration and run in the --full configurations. Repository checks
# (clang-format, frost-lint, licence check) run unless --no-checks is given;
# verify.ps1 passes it because it has already checked the same tree.
#
# Environment:
#   FROST_MIRROR  mirror directory (a leading "~/" is expanded). Default:
#                 ~/frost-src-<hash of the source path>, so every worktree gets
#                 its own mirror; a lock file serialises runs on one mirror.
#   FROST_JOBS    build parallelism (default: nproc), to stay inside the WSL memory cap.
#
# The last line of output is the summary for the commit verification footer,
# e.g. "wsl: debug 62/63 (1 skipped), asan 63/63, tsan 63/63". Skipped tests are
# never counted as passed. Exit status is non-zero if any configuration fails to
# build, has a failing test, or a check fails.

set -euo pipefail

FULL=0
CHECKS=1
for arg in "$@"; do
    case "$arg" in
        --full | -Full | -full) FULL=1 ;;
        --quick | -Quick | -quick) FULL=0 ;;
        --no-checks) CHECKS=0 ;;
        *) echo "usage: verify.sh [--quick | --full] [--no-checks]" >&2; exit 2 ;;
    esac
done

SOURCE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
if [[ -n "${FROST_MIRROR:-}" ]]; then
    MIRROR="${FROST_MIRROR/#\~\//$HOME/}"
else
    MIRROR="$HOME/frost-src-$(printf '%s' "$SOURCE" | sha256sum | cut -c1-12)"
fi
JOBS="${FROST_JOBS:-$(nproc)}"
# shellcheck source=/dev/null
source "${FROST_SDK:-$HOME/sdk}/env.sh"

# One run per mirror at a time: a second run would rsync over the first one's
# sources mid-build and report results for a mix of both trees.
mkdir -p "$MIRROR"
exec 9> "$MIRROR.lock"
if ! flock -n 9; then
    echo "verify.sh: waiting for another run on $MIRROR"
    flock 9
fi

# Mirror the tree. build/, .git/ and .claude/ (agent worktrees) stay out: the
# mirror keeps its own build trees (protected from --delete because they are excluded).
rsync -a --delete --exclude '/build/' --exclude '/.git/' --exclude '/.claude/' "$SOURCE/" "$MIRROR/"
cd "$MIRROR"
echo "verify.sh: source $SOURCE, mirror $MIRROR, jobs $JOBS"

summary=()
status=0

# Turns ctest output (on stdin) into "passed/total" plus " (n skipped)", or nothing.
# Skipped tests are listed under "The following tests did not run:".
ctest_counts() {
    local out line total failed=0 skipped
    out="$(cat)"
    line="$(grep -E 'tests passed.* out of [0-9]+' <<< "$out" | tail -1 || true)"
    [[ -n "$line" ]] || return 0
    total="$(sed -E 's/.*out of ([0-9]+).*/\1/' <<< "$line")"
    if [[ "$line" =~ ([0-9]+)\ tests\ failed ]]; then failed="${BASH_REMATCH[1]}"; fi
    skipped="$(grep -cE '^\s*[0-9]+ - .+\((Skipped|Disabled)\)\s*$' <<< "$out" || true)"
    local text="$((total - failed - skipped))/$total"
    if ((skipped > 0)); then text+=" ($skipped skipped)"; fi
    echo "$text"
}

# Configures, builds and tests one preset pair; appends "<label> <counts>" to the summary.
# Extra arguments are passed to ctest (e.g. -LE stress).
run_config() {
    local label="$1" configure="$2" preset="$3"
    shift 3
    echo "=== $label: configure ($configure)"
    if ! cmake --preset "$configure" > "build-$label.log" 2>&1; then
        tail -30 "build-$label.log"
        summary+=("$label configure-failed"); status=1; return
    fi
    echo "=== $label: build ($preset)"
    if ! cmake --build --preset "$preset" -j "$JOBS" >> "build-$label.log" 2>&1; then
        grep -E 'error|FAILED' "build-$label.log" | head -40 || true
        summary+=("$label build-failed"); status=1; return
    fi
    echo "=== $label: test ($preset)"
    local out counts
    out="$(ctest --preset "$preset" "$@" 2>&1)" || status=1
    echo "$out" | tail -15
    counts="$(ctest_counts <<< "$out")"
    if [[ -z "$counts" ]]; then
        summary+=("$label no-results"); status=1; return
    fi
    summary+=("$label $counts")
}

run_config debug linux-clang linux-clang-debug -LE stress
if [[ $FULL -eq 1 ]]; then
    run_config asan linux-clang-asan linux-clang-asan
    run_config tsan linux-clang-tsan linux-clang-tsan
fi

if [[ $CHECKS -eq 1 ]]; then
    # clang-format on tracked C/C++ files (the banned-construct fixtures cannot be parsed).
    unformatted=0
    while IFS= read -r file; do
        if ! clang-format --dry-run --Werror "$SOURCE/$file" > /dev/null 2>&1; then
            echo "needs clang-format: $file"
            unformatted=$((unformatted + 1))
        fi
    done < <(git -C "$SOURCE" ls-files '*.cpp' '*.hpp' '*.h' '*.c' '*.hpp.in' | grep -v '^tests/toolchain_probe/banned/' || true)
    if ((unformatted == 0)); then summary+=("format ok"); else summary+=("format FAILED ($unformatted files)"); status=1; fi
    for tool in frost_lint licensecheck; do
        if [[ ! -f "$SOURCE/tools/$tool.py" ]]; then summary+=("$tool not-run"); continue; fi
        if python3 -I "$SOURCE/tools/$tool.py" --repo "$SOURCE"; then summary+=("$tool ok"); else summary+=("$tool FAILED"); status=1; fi
    done
fi

echo "wsl: $(IFS=,; echo "${summary[*]}" | sed 's/,/, /g')"
exit $status
