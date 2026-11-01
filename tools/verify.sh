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
# configuration and run in the --full configurations. Afterwards the licence
# check inspects the Linux build trees and Qt; the tree checks (clang-format,
# frost-lint, the feature-matrix check) also run unless --no-checks is given,
# which verify.ps1 passes because it has already checked the same tree.
#
# Environment:
#   FROST_MIRROR  mirror directory (a leading "~/" is expanded). Default:
#                 ~/frost-src-<hash of the source path>, so every worktree gets
#                 its own mirror; a lock file serialises runs on one mirror.
#   FROST_JOBS    build parallelism (default: nproc), to stay inside the WSL memory cap.
#   FROST_SDK     SDK root whose env.sh is sourced (default: ~/sdk).
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

# Turns ctest output (on stdin) into "passed/total" plus " (n skipped, m disabled)" (each part only when
# non-zero), or nothing when ctest printed no summary line.
# Skipped and disabled tests are listed under "The following tests did not run:".
# ctest's "out of N" includes skipped tests (never counted as passed here) but not
# disabled ones (gtest's DISABLED_ tests), which are reported on their own.
ctest_counts() {
    local out line total failed=0 skipped disabled
    out="$(cat)"
    line="$(grep -E 'tests passed.* out of [0-9]+' <<< "$out" | tail -1 || true)"
    [[ -n "$line" ]] || return 0
    total="$(sed -E 's/.*out of ([0-9]+).*/\1/' <<< "$line")"
    if [[ "$line" =~ ([0-9]+)\ tests\ failed ]]; then failed="${BASH_REMATCH[1]}"; fi
    skipped="$(grep -cE '^\s*[0-9]+ - .+\(Skipped\)\s*$' <<< "$out" || true)"
    disabled="$(grep -cE '^\s*[0-9]+ - .+\(Disabled\)\s*$' <<< "$out" || true)"
    local text="$((total - failed - skipped))/$total" notes=()
    if ((skipped > 0)); then notes+=("$skipped skipped"); fi
    if ((disabled > 0)); then notes+=("$disabled disabled"); fi
    if ((${#notes[@]} > 0)); then text+=" ($(IFS=,; echo "${notes[*]}" | sed 's/,/, /g'))"; fi
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

# Runs tools/<name>.py on the mirror (which has no .git: frost-lint then walks it,
# honouring .gitignore) and appends "<name> ok|FAILED|not-run" to the summary.
# stderr goes to stdout because verify.ps1 (PowerShell 5.1) captures both.
run_check() {
    local name="$1"
    if [[ ! -f "tools/$name.py" ]]; then summary+=("$name not-run"); return; fi
    echo "=== check: $name"
    if python3 -I "tools/$name.py" --repo "$MIRROR" 2>&1; then summary+=("$name ok"); else summary+=("$name FAILED"); status=1; fi
}

if [[ $CHECKS -eq 1 ]]; then
    # clang-format on the tracked C/C++ files (the list comes from the source tree's
    # git; the mirror has the same contents). Fixtures are excluded: they are
    # byte-exact inputs, and clang-format cannot parse the banned-construct probes.
    echo "=== check: format"
    # A git worktree's .git file names its git directory as Git for Windows wrote it
    # ("gitdir: E:/dev/FrostIDE/.git/worktrees/<name>"), which git here cannot open; point it
    # at the /mnt/<drive> form of that directory instead.
    source_git=(git -C "$SOURCE")
    if [[ -f "$SOURCE/.git" ]]; then
        gitdir="$(sed -n 's/^gitdir: //p' "$SOURCE/.git" | tr -d '\r')"
        if [[ "$gitdir" =~ ^([A-Za-z]):[/\\](.*)$ ]]; then
            drive="${BASH_REMATCH[1],,}"
            source_git=(git --git-dir="/mnt/$drive/${BASH_REMATCH[2]//\\//}" --work-tree="$SOURCE")
        fi
    fi
    # An empty list (git failing, e.g. on safe.directory) must fail, not pass vacuously.
    sources="$("${source_git[@]}" ls-files '*.cpp' '*.hpp' '*.h' '*.c' '*.hpp.in' |
        grep -Ev '^tests/toolchain_probe/banned/|^tests/[^/]+/fixtures/' || true)"
    unformatted=0
    while IFS= read -r file; do
        if ! clang-format --dry-run --Werror "$MIRROR/$file" > /dev/null 2>&1; then
            echo "needs clang-format: $file"
            unformatted=$((unformatted + 1))
        fi
    done <<< "$sources"
    if [[ -z "$sources" ]]; then summary+=("format FAILED (no files listed)"); status=1
    elif ((unformatted == 0)); then summary+=("format ok")
    else summary+=("format FAILED ($unformatted files)"); status=1; fi
    run_check frost_lint
    run_check featurecheck
fi
run_check licensecheck

echo "wsl: $(IFS=,; echo "${summary[*]}" | sed 's/,/, /g')"
exit $status
