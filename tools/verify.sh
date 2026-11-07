#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# verify.sh: the Linux half of FrostIDE's local verification (there is no CI;
# PLAN.md section 6.4). Run inside WSL:
#
#   wsl -d Ubuntu-24.04 --exec bash /mnt/e/dev/FrostIDE/tools/verify.sh [--quick | --full] [--no-checks]
#       [--tidy] [--tsan-quick]
#
# It mirrors the working tree onto ext4 (building on the /mnt/e 9P mount is
# several times slower), then configures, builds and tests:
#   --quick       (default) linux-clang Debug (clang + libstdc++ 14, the Linux floor)
#   --tsan-quick  with --quick, the TSan quick leg: the linux-clang-tsan tree's tests of the
#                 modules the changed files affect (io, rpc, sched, text and their dependents;
#                 tools/verify_support.py tsan, ADR 0027 decision 5), or "tsan-quick not-needed"
#                 when none is affected
#   --full        additionally ASan+UBSan (Debug), TSan (RelWithDebInfo, every module) and the fuzz
#                 smoke: the linux-clang-fuzz tree (libFuzzer + ASan + UBSan, RelWithDebInfo) runs
#                 each fuzz target for FROST_FUZZ_SECONDS (60) through ctest -L '^fuzz$'
# The PowerShell spellings -Quick, -Full, -Tidy and -TsanQuick are accepted too.
#
# Tests labelled `stress` (long concurrency runs) are skipped in the Debug
# configuration and run in the --full configurations. Afterwards, with --tidy, clang-tidy checks
# the changed files' translation units over the Debug compile database (every TU under --full,
# with the fuzz tree's own TUs, or when there is no changed-file list), and the licence
# check inspects the Linux build trees and Qt. Both run even with --no-checks, because they
# depend on the Linux build trees. The tree checks (clang-format, frost-lint, the
# feature-matrix check and the source-to-test line ratchet against the merge base with
# FROST_VERIFY_BASE, FND-5) run unless --no-checks is given, which verify.ps1 passes because it
# has already checked the same tree. A standalone passing run (without --no-checks) records its
# summary as a verify-stamp of the source tree for the commit-msg hook (FND-2, tools/hooks).
#
# --tidy and --tsan-quick are off by default during the feature-first phase (ADR 0031; ADR 0027
# decision 9); without them the summary says "tidy not-run (off by default)" and, in --quick,
# "tsan-quick not-run (off by default)", and no changed-file list is made.
#
# Before anything runs, the PATH loses the Windows directories (/mnt/<drive>/...) that WSL
# appends: every CMake find_* search also probes them over 9P (ADR 0027 decision 4).
#
# Environment:
#   FROST_MIRROR         mirror directory (a leading "~/" is expanded). Default:
#                        ~/frost-src-<hash of the source path>, so every worktree gets
#                        its own mirror; a lock file serialises runs on one mirror.
#   FROST_JOBS           build and tidy parallelism (default: nproc), to stay inside the WSL memory cap.
#   FROST_SDK            SDK root whose env.sh is sourced (default: ~/sdk).
#   FROST_CHANGED_FILES  the changed-file list (repository-relative paths, one per line), used
#                        only with --tidy or --tsan-quick in --quick:
#                        unset, verify.sh computes it from the source tree's git; set and empty,
#                        there is no list (verify.ps1 could not make one) and none is computed,
#                        so tidy checks every TU and the TSan quick leg covers every module;
#                        a readable file, that list (verify.ps1 passes its own).
#   FROST_VERIFY_BASE    the base whose merge base with HEAD defines the changed files when
#                        verify.sh computes the list (default: main).
#
# The last line of output is the summary for the commit verification footer,
# e.g. "wsl: debug 62/63 (1 skipped), tsan-quick 40/40, tidy ok (2 TUs), licensecheck ok" (with
# --tsan-quick and --tidy).
# Items are joined with ", " and never contain ", " outside parentheses. Skipped tests are
# never counted as passed. Exit status is non-zero if any configuration fails to build, has
# a failing test, tidy finds something, or a check fails; 2 for a usage error or a missing tool.
# Side effects: the mirror, its lock file "<mirror>.lock" and, when verify.sh computes the
# list, "<mirror>.changed-files" and its stderr "<mirror>.changed-files.err" (both overwritten by
# the next run on the mirror), and after a passing standalone run the verify-stamp in the source
# repository's <git common dir>/frost/verify-stamps.

set -euo pipefail

# Sets FULL, CHECKS, TIDY and TSAN_QUICK (0 or 1) from the command line; exits 2 with the usage
# line for anything else. No other side effects; tests/tools/test_verify_scripts.py runs it alone.
parse_args() {
    FULL=0
    CHECKS=1
    TIDY=0
    TSAN_QUICK=0
    local arg
    for arg in "$@"; do
        case "$arg" in
            --full | -Full | -full) FULL=1 ;;
            --quick | -Quick | -quick) FULL=0 ;;
            --no-checks) CHECKS=0 ;;
            --tidy | -Tidy) TIDY=1 ;;
            --tsan-quick | -TsanQuick) TSAN_QUICK=1 ;;
            *) echo "usage: verify.sh [--quick | --full] [--no-checks] [--tidy] [--tsan-quick]" >&2; exit 2 ;;
        esac
    done
}
parse_args "$@"

SOURCE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
if [[ -n "${FROST_MIRROR:-}" ]]; then
    MIRROR="${FROST_MIRROR/#\~\//$HOME/}"
else
    MIRROR="$HOME/frost-src-$(printf '%s' "$SOURCE" | sha256sum | cut -c1-12)"
fi
JOBS="${FROST_JOBS:-$(nproc)}"
# shellcheck source=/dev/null
source "${FROST_SDK:-$HOME/sdk}/env.sh"

# Budget: B28. Drop the drvfs entries WSL appends to PATH (38 to 45 on this machine, depending on
# the Windows environment that started WSL): CMake's FindOpenGL, reached through
# find_package(Qt6), searched them and their derived prefixes over 9P on every configure, about
# 0.1 s per miss. With vcpkg's toolchain that probe took 27.0 s on a first configure and 16.0 s on
# a re-run, and 0.51 s and 0.24 s without the /mnt entries (ADR 0027, decision 4). Entries under
# the SDK root are kept. Every configure, build and test of this run sees the stripped PATH; no
# Linux step runs a Windows program.
path_note="$(mktemp)"
if ! new_path="$(python3 -I "$SOURCE/tools/verify_support.py" strip-path --keep "${FROST_SDK:-$HOME/sdk}" "$PATH" \
        2> "$path_note")"; then
    cat "$path_note" >&2
    rm -f "$path_note"
    echo "verify.sh: could not filter PATH" >&2
    exit 2
fi
PATH="$new_path"
echo "verify.sh: $(cat "$path_note") Windows PATH entries (ADR 0027)"
rm -f "$path_note"
needed_tools=(cmake ninja clang-format rsync python3 git flock)
if [[ $TIDY -eq 1 ]]; then needed_tools+=(clang-tidy); fi
for tool in "${needed_tools[@]}"; do
    if ! command -v "$tool" > /dev/null; then
        echo "verify.sh: $tool is not on PATH (source ~/sdk/env.sh; tools/bootstrap-wsl.sh installs it)" >&2
        exit 2
    fi
done

# Sets SOURCE_GIT_ARGS to the git options that reach the source tree's repository from here.
# A git worktree's .git file names its git directory as Git for Windows wrote it
# ("gitdir: E:/dev/FrostIDE/.git/worktrees/<name>"), which git here cannot open; point it at the
# /mnt/<drive> form of that directory instead. Empty for a plain checkout. Used with
# `git -C "$SOURCE"`.
source_git_args() {
    SOURCE_GIT_ARGS=()
    if [[ -f "$SOURCE/.git" ]]; then
        local gitdir drive
        gitdir="$(sed -n 's/^gitdir: //p' "$SOURCE/.git" | tr -d '\r')"
        if [[ "$gitdir" =~ ^([A-Za-z]):[/\\](.*)$ ]]; then
            drive="${BASH_REMATCH[1],,}"
            SOURCE_GIT_ARGS=(--git-dir="/mnt/$drive/${BASH_REMATCH[2]//\\//}" --work-tree="$SOURCE")
        fi
    fi
}
source_git_args

# One run per mirror at a time: a second run would rsync over the first one's
# sources mid-build and report results for a mix of both trees.
mkdir -p "$MIRROR"
exec 9> "$MIRROR.lock"
if ! flock -n 9; then
    echo "verify.sh: waiting for another run on $MIRROR"
    flock 9
fi

# The changed-file list (FROST_CHANGED_FILES above), made only when --tidy or --tsan-quick use it
# (--full checks everything). The path next to the mirror belongs to this run because the mirror's
# lock is held.
changed_list=""
changed_reason=""
need_list=0
if [[ $FULL -eq 0 && ($TIDY -eq 1 || $TSAN_QUICK -eq 1) ]]; then need_list=1; fi
if [[ $need_list -eq 0 ]]; then
    :
elif [[ -n "${FROST_CHANGED_FILES+set}" ]]; then
    if [[ -z "$FROST_CHANGED_FILES" ]]; then
        changed_reason="verify.ps1 had none"
    elif [[ -r "$FROST_CHANGED_FILES" ]]; then
        changed_list="$FROST_CHANGED_FILES"
    else
        changed_reason="FROST_CHANGED_FILES is not readable: $FROST_CHANGED_FILES"
    fi
elif python3 -I "$SOURCE/tools/verify_support.py" changed --repo "$SOURCE" "${SOURCE_GIT_ARGS[@]}" \
        --base "${FROST_VERIFY_BASE:-main}" --out "$MIRROR.changed-files" 2> "$MIRROR.changed-files.err"; then
    changed_list="$MIRROR.changed-files"
else
    changed_reason="$(tail -1 "$MIRROR.changed-files.err")"
fi
if [[ -n "$changed_list" ]]; then
    echo "verify.sh: changed-file list $changed_list ($(grep -c . "$changed_list" || true) files)"
elif [[ $need_list -eq 1 ]]; then
    echo "verify.sh: no changed-file list: $changed_reason"
fi

# Copy changed content with a fresh mtime so Ninja sees it even when the source came from an older
# snapshot. Checksum comparison avoids touching identical files merely because their times differ.
# The mirror keeps its own build trees (protected from --delete because they are excluded).
mirror_source() {
    rsync -a --checksum --no-times --delete --exclude '/build/' --exclude '/.git/' --exclude '/.claude/' "$1/" "$2/"
}
# The tree hash of the source tree, taken before the copy (FND-2; tools/hooks/verify_stamp.py), for a
# standalone run only: under verify.ps1 (--no-checks) verify.ps1 stamps the combined summary itself.
stamp_tree=""
stamp_reason="a verify.ps1 leg (--no-checks)"
if [[ $CHECKS -eq 1 ]]; then
    if stamp_out="$(python3 -I "$SOURCE/tools/hooks/verify_stamp.py" tree --repo "$SOURCE" "${SOURCE_GIT_ARGS[@]}" 2>&1)"; then
        stamp_tree="$(tail -1 <<< "$stamp_out")"
    else
        stamp_reason="no tree hash: $(tail -1 <<< "$stamp_out")"
    fi
fi
mirror_source "$SOURCE" "$MIRROR"
cd "$MIRROR"
echo "verify.sh: source $SOURCE, mirror $MIRROR, jobs $JOBS"

summary=()
status=0
debug_built=0
fuzz_built=0

# Prints its arguments joined with ", " (an item's own ", " stays as it is). No side effects.
join_items() {
    local out="" item
    for item in "$@"; do out+="${out:+, }$item"; done
    printf '%s' "$out"
}

# Turns ctest output (on stdin) into "passed/total" plus " (n skipped, m disabled)" (each part only when
# non-zero), or nothing when ctest printed no summary line.
# Skipped and disabled tests are listed under "The following tests did not run:".
# ctest's "out of N" includes skipped tests (never counted as passed here) but not
# disabled ones (gtest's DISABLED_ tests), which are reported on their own. No side effects.
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
    if ((${#notes[@]} > 0)); then text+=" ($(join_items "${notes[@]}"))"; fi
    echo "$text"
}

# Configures, builds and tests one preset pair; appends "<label> <counts>" to the summary and
# sets debug_built/fuzz_built when those trees built. Extra arguments are passed to ctest
# (e.g. -LE '^stress$'). Writes build-<label>.log in the mirror.
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
    if [[ "$label" == debug ]]; then debug_built=1; fi
    if [[ "$label" == fuzz ]]; then fuzz_built=1; fi
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

# The TSan quick leg (PLAN.md 6.4 step 3; ADR 0027 decision 5), with --tsan-quick only (decision
# 9): the TSan tree's tests of the modules the changed files affect, by their mod.* labels. Without
# a list every module runs. The tree builds incrementally, so the first run in a fresh mirror builds
# all of it. A selection that matches no test fails as "no-results" (the presets set noTestsAction
# to error).
run_tsan_quick() {
    if [[ $TSAN_QUICK -ne 1 ]]; then summary+=("tsan-quick not-run (off by default)"); return; fi
    local args=(--all) out modules regex
    if [[ -n "$changed_list" ]]; then
        args=(--changed "$changed_list")
    else
        echo "verify.sh: TSan quick leg for every module (no changed-file list: $changed_reason)"
    fi
    if ! out="$(python3 -I tools/verify_support.py tsan "${args[@]}")"; then
        summary+=("tsan-quick FAILED (selection)"); status=1; return
    fi
    # sed, not read: read returns 1 at end of input, which set -e would treat as a failure.
    modules="$(sed -n 1p <<< "$out")"
    regex="$(sed -n 2p <<< "$out")"
    if [[ -z "$modules" ]]; then
        summary+=("tsan-quick not-needed"); return
    fi
    echo "verify.sh: TSan quick leg for $modules"
    run_config tsan-quick linux-clang-tsan linux-clang-tsan -LE '^stress$' -L "$regex"
}

run_config debug linux-clang linux-clang-debug -LE '^stress$'

# Symbolization of the self-test artifacts (--full only; ADR 0022). The Debug ctest run above
# leaves a crash dump, a contract-violation dump and a freeze report in
# build/linux-clang/selftest-artifacts/Debug (frost_process_tests), and tools/symbolize.py
# check-selftest must resolve each to its marked source line with the pinned llvm-symbolizer.
# Artifacts older than this run's lock file (a run whose tests did not run) count as none, and
# "no-artifacts" fails. The ASan and TSan trees write artifacts of their own; they are not
# symbolized.
if [[ $FULL -eq 1 ]]; then
    symbolize_dir="build/linux-clang/selftest-artifacts/Debug"
    echo "=== symbolize ($symbolize_dir)"
    if [[ -z "$(find "$symbolize_dir" -mindepth 2 -maxdepth 2 -name artifact.json -newer "$MIRROR.lock" 2>/dev/null)" ]]; then
        summary+=("symbolize no-artifacts"); status=1
    elif python3 -I tools/symbolize.py check-selftest --artifacts "$symbolize_dir" \
            --llvm-symbolizer "$FROST_LLVM_ROOT/bin/llvm-symbolizer" --source-root "$MIRROR" 2>&1; then
        summary+=("symbolize ok")
    else
        summary+=("symbolize FAILED"); status=1
    fi
fi

if [[ $FULL -eq 1 ]]; then
    run_config asan linux-clang-asan linux-clang-asan
    run_config tsan linux-clang-tsan linux-clang-tsan
    run_config fuzz linux-clang-fuzz linux-clang-fuzz -L '^fuzz$'
else
    run_tsan_quick
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

# clang-tidy over the Debug compile database (ADR 0027 decision 1), with --tidy only (decision 9):
# the changed files' TUs with a list (Budget: B28), every TU without one or under --full (plus the
# fuzz tree's own TUs). Appends the summary line tools/verify_support.py prints last
# ("tidy ok (2 TUs)", "tidy FAILED (...)"), or "tidy not-run (off by default)".
run_tidy() {
    if [[ $TIDY -ne 1 ]]; then summary+=("tidy not-run (off by default)"); return; fi
    echo "=== tidy"
    if [[ $debug_built -ne 1 ]]; then summary+=("tidy not-run (debug build failed)"); return; fi
    local args=(--repo "$MIRROR" --build-dir "$MIRROR/build/linux-clang" --config Debug
        --clang-tidy "$FROST_LLVM_ROOT/bin/clang-tidy" --ninja "$(command -v ninja)" --jobs "$JOBS")
    if [[ $FULL -eq 1 ]]; then
        args+=(--all)
        if [[ $fuzz_built -eq 1 ]]; then
            args+=(--extra-build-dir "$MIRROR/build/linux-clang-fuzz" --extra-config RelWithDebInfo)
        fi
    elif [[ -n "$changed_list" ]]; then
        args+=(--changed "$changed_list")
    else
        args+=(--all --reason "no changed-file list: $changed_reason")
    fi
    local out rc=0 line
    out="$(python3 -I tools/verify_support.py tidy "${args[@]}" 2>&1)" || rc=$?
    echo "$out"
    line="$(grep -E '^tidy ' <<< "$out" | tail -1 || true)"
    if [[ -z "$line" ]]; then line="tidy FAILED (exit $rc without a summary)"; fi
    summary+=("$line")
    if ((rc != 0)); then status=1; fi
}

# The source-to-test line ratchet of this slice against the merge base with FROST_VERIFY_BASE (FND-5,
# ADR 0078 decision 6; threshold in tools/frost_lint.py), measured in the source tree's git. Appends
# the "ratchet ..." line frost_lint.py prints last; "ratchet not-run (...)" does not fail.
run_ratchet() {
    echo "=== check: ratchet"
    local out rc=0 line
    out="$(python3 -I tools/frost_lint.py ratchet --repo "$SOURCE" "${SOURCE_GIT_ARGS[@]}" \
        --base "${FROST_VERIFY_BASE:-main}" 2>&1)" || rc=$?
    echo "$out"
    line="$(grep -E '^ratchet ' <<< "$out" | tail -1 || true)"
    if [[ -z "$line" ]]; then line="ratchet FAILED (exit $rc without a summary)"; rc=1; fi
    summary+=("$line")
    if ((rc != 0)); then status=1; fi
}

# Records the final summary line ($1) as a verify-stamp of the tree taken before the copy (FND-2),
# unless the source tree changed meanwhile. Prints one "verify-stamp: ..." line; never fails the run.
write_stamp() {
    if [[ -z "$stamp_tree" ]]; then echo "verify-stamp: not written ($stamp_reason)"; return 0; fi
    python3 -I "$SOURCE/tools/hooks/verify_stamp.py" write --repo "$SOURCE" "${SOURCE_GIT_ARGS[@]}" \
        --script verify.sh --summary "$1" --expect-tree "$stamp_tree" 2>&1 || true
}

if [[ $CHECKS -eq 1 ]]; then
    # clang-format on the tracked C/C++ files (the list comes from the source tree's
    # git; the mirror has the same contents). Fixtures are excluded: they are
    # byte-exact inputs, and clang-format cannot parse the banned-construct probes.
    # Vendored tree-sitter and libvterm sources stay as pinned (ADR 0023, 0051).
    echo "=== check: format"
    # An empty list (git failing, e.g. on safe.directory) must fail, not pass vacuously.
    sources="$(git -C "$SOURCE" "${SOURCE_GIT_ARGS[@]}" ls-files '*.cpp' '*.hpp' '*.h' '*.c' '*.hpp.in' |
        grep -Ev '^tests/toolchain_probe/banned/|^tests/[^/]+/fixtures/|^third_party/(tree-sitter|grammars|libvterm)/' || true)"
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
    run_ratchet
fi
run_tidy
run_check licensecheck

final="wsl: $(join_items "${summary[@]}")"
if [[ $CHECKS -eq 1 && $status -eq 0 ]]; then write_stamp "$final"; fi
echo "$final"
exit $status
