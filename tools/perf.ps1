# SPDX-License-Identifier: Apache-2.0
#
# perf.ps1: entry point of FrostIDE's performance-budget harness (PLAN.md section 4; ADR 0027).
#
#   powershell -ExecutionPolicy Bypass -File tools\perf.ps1 [-List] [-Budget B28]
#              [-Variant rebuild,noop,verify,wsl-leg] [-Runs n] [-Warmup n] [-Files core,main,sched]
#              [-Label text] [-PerfRoot E:\FrostBuilds\perf] [-Milestone m1] [-BuildDir build\win-clangcl]
#              [-LockRecord <file>] [-Restore <run dir>]
#
# A thin wrapper with no param block: it dot-sources env.ps1 (the pinned cmake, ninja, sccache and
# clang-cl on PATH, SCCACHE_DIR set) and runs tools\frost_perf.py with the SDK's Python, forwarding
# every argument unchanged. frost_perf.py's argparse is the only definition of the command line
# (options, defaults, the results layout and the exit codes are documented there); it accepts these
# PowerShell spellings and splits the comma lists that -File passes as one string.
#
# Side effects: those of frost_perf.py (results under -PerfRoot; probe edits that it restores).
# Exit status: frost_perf.py's (0 within budget, 1 over budget, 2 usage or environment error,
# 3 invalid run).

$ErrorActionPreference = 'Continue'
. (Join-Path $PSScriptRoot 'env.ps1')
$python = Join-Path $env:FROST_SDK 'py\Scripts\python.exe'
& $python -I (Join-Path $PSScriptRoot 'frost_perf.py') @args
exit $LASTEXITCODE
