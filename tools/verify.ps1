# SPDX-License-Identifier: Apache-2.0
#
# verify.ps1: FrostIDE's local verification gate (there is no CI; PLAN.md
# section 6.4 and the definition of done in CLAUDE.md).
#
#   powershell -ExecutionPolicy Bypass -File tools\verify.ps1 [-Quick | -Full] [-WindowsOnly]
#              [-BuildDir build\win-clangcl] [-WslMirror ~/frost-src-x] [-Jobs 8]
#
# Steps:
#   1. Windows: configure win-clangcl, build Debug, run ctest (tests labelled
#      `stress` are skipped in Debug).
#   2. -Full adds Release and the ASan configuration (RelWithDebInfo), with every label,
#      and the /MT fuzz smoke: the win-clangcl-fuzz tree (libFuzzer + ASan, RelWithDebInfo)
#      runs each fuzz target for FROST_FUZZ_SECONDS (60) through ctest -L fuzz.
#   3. Repository checks: clang-format on tracked C/C++ files, frost-lint, the
#      licence check (Windows build trees) and the feature-matrix check. A check
#      whose tool does not exist yet is reported as "not-run", never silently dropped.
#   4. Linux: tools/verify.sh inside WSL (unless -WindowsOnly), with --full under -Full.
#      The tree checks are not repeated there (same tree); the licence check is,
#      because it inspects the Linux build trees and Qt.
# -Quick is the default and only names that choice explicitly.
#
# The final line is the summary for the commit footer, e.g.
#   "Verified: win debug 63/64 (1 skipped), wsl debug 62/63 (1 skipped), format ok, frost_lint ok"
# Skipped tests are never counted as passed. Exit status: 0 only if every step
# that ran succeeded (skips and not-run checks do not fail the gate, but are visible).

[CmdletBinding()]
param(
    [switch]$Quick,
    [switch]$Full,
    [switch]$WindowsOnly,
    # Build directory for the win-clangcl configuration. Separate directories let
    # several work streams verify concurrently without sharing a Ninja tree.
    [string]$BuildDir = '',
    # Ext4 mirror directory inside WSL (verify.sh's FROST_MIRROR). The default is a
    # per-source-tree directory chosen by verify.sh, so worktrees never share one.
    [string]$WslMirror = '',
    # Parallel compile jobs on both sides; kept moderate because commit memory can be scarce.
    [int]$Jobs = 8
)

# 'Continue', not 'Stop': under Windows PowerShell 5.1 a native command's stderr
# line becomes an error record, and 'Stop' would abort the gate on the first
# warning that ctest, cmake or wsl.exe prints. Failures are detected through
# exit codes instead.
$ErrorActionPreference = 'Continue'
if ($Quick -and $Full) {
    Write-Host 'verify.ps1: -Quick and -Full are mutually exclusive'
    exit 2
}
$repo = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'env.ps1')
Set-Location $repo

$summary = New-Object System.Collections.Generic.List[string]
$failed = $false

# Runs a native command with stderr merged into stdout, echoing every line;
# returns the lines as strings. $LASTEXITCODE is the command's exit code.
function Invoke-Native([scriptblock]$Command) {
    $lines = @(& $Command 2>&1 | ForEach-Object { "$_" })
    $lines | Out-Host
    return , $lines
}

# Parses ctest output into "passed/total" plus "(n skipped, m disabled)". Both are
# listed under "The following tests did not run:". ctest's "out of N" includes
# skipped tests (never counted as passed here) but not disabled ones (gtest's
# DISABLED_ tests), which are reported on their own.
function Get-CtestCounts([string[]]$Output) {
    $line = $Output | Where-Object { $_ -match 'tests passed.* out of \d+' } | Select-Object -Last 1
    if (-not $line) { return $null }
    $total = [int]([regex]::Match($line, 'out of (\d+)').Groups[1].Value)
    $failedCount = 0
    $failedMatch = [regex]::Match($line, '(\d+) tests failed')
    if ($failedMatch.Success) { $failedCount = [int]$failedMatch.Groups[1].Value }
    $skipped = @($Output | Where-Object { $_ -match '^\s*\d+ - .+\(Skipped\)\s*$' }).Count
    $disabled = @($Output | Where-Object { $_ -match '^\s*\d+ - .+\(Disabled\)\s*$' }).Count
    $text = "$($total - $failedCount - $skipped)/$total"
    $notes = @()
    if ($skipped -gt 0) { $notes += "$skipped skipped" }
    if ($disabled -gt 0) { $notes += "$disabled disabled" }
    if ($notes.Count -gt 0) { $text += " ($($notes -join ', '))" }
    return $text
}

# Configures, builds and tests one Windows configuration. $CtestArgs are extra
# ctest arguments (e.g. -LE stress).
function Test-WindowsConfig([string]$Label, [string]$Preset, [string]$Config, [string]$Dir, [string[]]$CtestArgs = @()) {
    $configureArgs = @('--preset', $Preset)
    if ($Dir) { $configureArgs += @('-B', $Dir) } else { $Dir = Join-Path $repo "build\$Preset" }
    Write-Host "=== $Label configure"
    $null = Invoke-Native { cmake @configureArgs }
    if ($LASTEXITCODE -ne 0) { $script:summary.Add("win $Label configure-failed"); $script:failed = $true; return }
    Write-Host "=== $Label build"
    $null = Invoke-Native { cmake --build $Dir --config $Config -j $Jobs }
    if ($LASTEXITCODE -ne 0) { $script:summary.Add("win $Label build-failed"); $script:failed = $true; return }
    Write-Host "=== $Label test"
    $out = Invoke-Native { ctest --test-dir $Dir -C $Config --output-on-failure @CtestArgs }
    $ok = ($LASTEXITCODE -eq 0)
    $counts = Get-CtestCounts $out
    if (-not $counts) { $counts = 'no-results'; $ok = $false }
    $script:summary.Add("win $Label $counts")
    if (-not $ok) { $script:failed = $true }
}

Test-WindowsConfig 'debug' 'win-clangcl' 'Debug' $BuildDir @('-LE', 'stress')
if ($Full) {
    Test-WindowsConfig 'release' 'win-clangcl' 'Release' $BuildDir
    $asanDir = ''
    if ($BuildDir) { $asanDir = "$BuildDir-asan" }
    Test-WindowsConfig 'asan' 'win-clangcl-asan' 'RelWithDebInfo' $asanDir
    $fuzzDir = ''
    if ($BuildDir) { $fuzzDir = "$BuildDir-fuzz" }
    Test-WindowsConfig 'fuzz' 'win-clangcl-fuzz' 'RelWithDebInfo' $fuzzDir @('-L', 'fuzz')
}

# Symbolization of the self-test artifacts (-Full only; ADR 0022). The Debug and Release ctest
# runs above leave a crash dump, a contract-violation dump and a freeze report in
# <build>\selftest-artifacts\<Config> (frost_process_tests), and tools\symbolize.py
# check-selftest must resolve each to its marked source line with the pinned llvm-symbolizer.
# Artifacts older than this verify run (a configuration whose tests did not run) count as
# none, and "no-artifacts" fails the gate.
if ($Full) {
    $symbolizeRoot = $BuildDir
    if (-not $symbolizeRoot) { $symbolizeRoot = Join-Path $repo 'build\win-clangcl' }
    $symbolizeSince = (Get-Process -Id $PID).StartTime
    $symbolizer = Join-Path $env:FROST_LLVM_ROOT 'bin\llvm-symbolizer.exe'
    $symbolizePython = Join-Path $env:FROST_SDK 'py\Scripts\python.exe'
    foreach ($symbolizeConfig in @('Debug', 'Release')) {
        $symbolizeLabel = $symbolizeConfig.ToLowerInvariant()
        $artifacts = Join-Path $symbolizeRoot "selftest-artifacts\$symbolizeConfig"
        $fresh = @(Get-ChildItem -Path (Join-Path $artifacts '*\artifact.json') -ErrorAction SilentlyContinue |
            Where-Object { $_.LastWriteTime -ge $symbolizeSince })
        Write-Host "=== $symbolizeLabel symbolize ($artifacts)"
        if ($fresh.Count -eq 0) {
            $summary.Add("win symbolize $symbolizeLabel no-artifacts"); $failed = $true; continue
        }
        $null = Invoke-Native { & $symbolizePython -I (Join-Path $PSScriptRoot 'symbolize.py') check-selftest `
                --artifacts $artifacts --llvm-symbolizer $symbolizer --source-root $repo }
        if ($LASTEXITCODE -eq 0) { $summary.Add("win symbolize $symbolizeLabel ok") }
        else { $summary.Add("win symbolize $symbolizeLabel FAILED"); $failed = $true }
    }
}

# Repository checks.
$checks = New-Object System.Collections.Generic.List[string]

# clang-format: every tracked C/C++ file must already be formatted. Fixtures are
# excluded: they are byte-exact inputs, and clang-format cannot parse the
# banned-construct probes.
Write-Host '=== format'
$clangFormat = Join-Path $env:FROST_LLVM_ROOT 'bin\clang-format.exe'
$sources = @(git ls-files '*.cpp' '*.hpp' '*.h' '*.c' '*.hpp.in' |
    Where-Object { $_ -notlike 'tests/toolchain_probe/banned/*' -and $_ -notlike 'tests/*/fixtures/*' })
$unformatted = New-Object System.Collections.Generic.List[string]
foreach ($source in $sources) {
    $null = & $clangFormat --dry-run --Werror $source 2>&1
    if ($LASTEXITCODE -ne 0) { $unformatted.Add($source) }
}
# An empty list (git failing) must fail, not pass vacuously.
if ($sources.Count -eq 0) { $checks.Add('format FAILED (no files listed)'); $failed = $true }
elseif ($unformatted.Count -eq 0) { $checks.Add('format ok') }
else {
    $unformatted | ForEach-Object { Write-Host "needs clang-format: $_" }
    $checks.Add("format FAILED ($($unformatted.Count) files)"); $failed = $true
}

# Python checks, run when the tool exists. featurecheck.py defaults to its `check` command.
$python = Join-Path $env:FROST_SDK 'py\Scripts\python.exe'
foreach ($tool in @('frost_lint.py', 'licensecheck.py', 'featurecheck.py')) {
    $name = $tool -replace '\.py$', ''
    $path = Join-Path $PSScriptRoot $tool
    if (-not (Test-Path $path)) { $checks.Add("$name not-run"); continue }
    Write-Host "=== $name"
    $null = Invoke-Native { & $python -I $path --repo $repo }
    if ($LASTEXITCODE -eq 0) { $checks.Add("$name ok") } else { $checks.Add("$name FAILED"); $failed = $true }
}
# TODO(M1): clang-tidy on changed files (PLAN.md section 6.4).
$checks.Add('tidy not-run')

if (-not $WindowsOnly) {
    $wslScript = '/mnt/' + $repo.Substring(0, 1).ToLowerInvariant() + ($repo.Substring(2) -replace '\\', '/') + '/tools/verify.sh'
    # --exec runs env(1) directly, without a shell re-parsing the arguments, so the
    # mirror path needs no quoting. verify.sh expands a leading "~/" itself.
    $wslArgs = @('-d', 'Ubuntu-24.04', '--exec', 'env', "FROST_JOBS=$Jobs")
    if ($WslMirror) { $wslArgs += "FROST_MIRROR=$WslMirror" }
    $wslArgs += @('bash', $wslScript, '--no-checks')
    if ($Full) { $wslArgs += '--full' }
    Write-Host "=== wsl $wslScript"
    $wslOut = Invoke-Native { wsl.exe @wslArgs }
    if ($LASTEXITCODE -ne 0) { $failed = $true }
    $wslLine = $wslOut | Where-Object { $_ -match '^wsl: ' } | Select-Object -Last 1
    # "wsl: debug 63/63, asan 64/64" becomes "wsl debug 63/63, wsl asan 64/64".
    if ($wslLine) {
        foreach ($item in (($wslLine -replace '^wsl: ', '') -split ', ')) { $summary.Add("wsl $item") }
    } else { $summary.Add('wsl no-results'); $failed = $true }
}

$line = 'Verified: ' + (($summary + $checks) -join ', ')
if ($failed) { $line = "FAILED: $line" }
Write-Host $line
if ($failed) { exit 1 }
exit 0
