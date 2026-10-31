# SPDX-License-Identifier: Apache-2.0
#
# verify.ps1: FrostIDE's local verification gate (there is no CI; PLAN.md
# section 6.4 and the definition of done in CLAUDE.md).
#
#   powershell -ExecutionPolicy Bypass -File tools\verify.ps1 [-Full] [-WindowsOnly] [-BuildDir build\win-clangcl]
#
# Steps:
#   1. Windows: configure win-clangcl, build Debug, run ctest.
#   2. -Full adds the Release build and the ASan configuration.
#   3. Repository checks (when present): frost-lint, the licence check and the
#      feature-matrix check (featurecheck.py defaults to its `check` command).
#   4. Linux: tools/verify.sh inside WSL (unless -WindowsOnly), --full with -Full.
# Tests labelled `stress` are skipped in the Debug configuration and run in the
# -Full configurations.
# The final line is the summary for the commit footer, e.g.
#   "Verified: win debug 45/45, wsl debug 44/44, lint ok"
# and the exit status is non-zero if any step failed.

[CmdletBinding()]
param(
    [switch]$Full,
    [switch]$WindowsOnly,
    # Build directory for the win-clangcl configuration. Separate directories let
    # several work streams verify concurrently without sharing a Ninja tree.
    [string]$BuildDir = '',
    # Ext4 mirror directory inside WSL (passed to verify.sh as FROST_MIRROR).
    [string]$WslMirror = '',
    # Parallel compile jobs on both sides; kept moderate because commit memory can be scarce.
    [int]$Jobs = 8
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'env.ps1')
Set-Location $repo

$summary = New-Object System.Collections.Generic.List[string]
$failed = $false

# Runs a native command, streaming its output; returns $true when it exits with 0.
function Invoke-Step([string]$Name, [scriptblock]$Command) {
    Write-Host "=== $Name"
    & $Command
    return ($LASTEXITCODE -eq 0)
}

# Parses ctest's "N% tests passed[, F tests failed] out of T" line into "passed/total".
function Get-CtestCounts([string[]]$Output) {
    $line = $Output | Where-Object { $_ -match 'tests passed.* out of \d+' } | Select-Object -Last 1
    if (-not $line) { return $null }
    $total = [int]([regex]::Match($line, 'out of (\d+)').Groups[1].Value)
    $failedMatch = [regex]::Match($line, '(\d+) tests failed')
    $failedCount = 0
    if ($failedMatch.Success) { $failedCount = [int]$failedMatch.Groups[1].Value }
    return "$($total - $failedCount)/$total"
}

# Configures, builds and tests one Windows configuration.
# $CtestArgs are extra ctest arguments (e.g. -LE stress).
function Test-WindowsConfig([string]$Label, [string]$Preset, [string]$Config, [string]$Dir, [string[]]$CtestArgs = @()) {
    $configureArgs = @('--preset', $Preset)
    if ($Dir) { $configureArgs += @('-B', $Dir) } else { $Dir = Join-Path $repo "build\$Preset" }
    if (-not (Invoke-Step "$Label configure" { cmake @configureArgs | Out-Host })) {
        $script:summary.Add("win $Label configure-failed"); $script:failed = $true; return
    }
    if (-not (Invoke-Step "$Label build" { cmake --build $Dir --config $Config -j $Jobs | Out-Host })) {
        $script:summary.Add("win $Label build-failed"); $script:failed = $true; return
    }
    Write-Host "=== $Label test"
    $out = & ctest --test-dir $Dir -C $Config --output-on-failure @CtestArgs 2>&1 | ForEach-Object { "$_" }
    $ok = ($LASTEXITCODE -eq 0)
    $out | Select-Object -Last 15 | Out-Host
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
    Test-WindowsConfig 'asan' 'win-clangcl-asan' 'Debug' $asanDir
}

# Repository checks, run when the tools exist (they arrive during M0).
$python = Join-Path $env:FROST_SDK 'py\Scripts\python.exe'
$checks = @()
foreach ($tool in @('frost_lint.py', 'licensecheck.py', 'featurecheck.py')) {
    $path = Join-Path $PSScriptRoot $tool
    if (Test-Path $path) {
        if (Invoke-Step $tool { & $python $path --repo $repo | Out-Host }) { $checks += "$($tool -replace '\.py$','') ok" }
        else { $checks += "$($tool -replace '\.py$','') FAILED"; $failed = $true }
    }
}

if (-not $WindowsOnly) {
    $verifyArgs = @()
    if ($Full) { $verifyArgs += '--full' }
    $wslScript = '/mnt/' + $repo.Substring(0, 1).ToLowerInvariant() + ($repo.Substring(2) -replace '\\', '/') + '/tools/verify.sh'
    $envPrefix = "FROST_JOBS=$Jobs "
    if ($WslMirror) { $envPrefix += "FROST_MIRROR=$WslMirror " }
    Write-Host "=== wsl $wslScript"
    $wslOut = & wsl.exe -d Ubuntu-24.04 -- bash -c "$envPrefix bash '$wslScript' $($verifyArgs -join ' ')" 2>&1 | ForEach-Object { "$_" }
    if ($LASTEXITCODE -ne 0) { $failed = $true }
    $wslOut | Select-Object -Last 20 | Out-Host
    $wslLine = $wslOut | Where-Object { $_ -match '^wsl: ' } | Select-Object -Last 1
    if ($wslLine) { $summary.Add(($wslLine -replace '^wsl: ', 'wsl ')) } else { $summary.Add('wsl no-results'); $failed = $true }
}

$line = 'Verified: ' + (($summary + $checks) -join ', ')
if ($failed) { $line = "FAILED: $line" }
Write-Host $line
if ($failed) { exit 1 }
exit 0
