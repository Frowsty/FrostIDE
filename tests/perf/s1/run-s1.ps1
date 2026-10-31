# SPDX-License-Identifier: Apache-2.0
#
# Runs spike S1 (ADR 0007) on REF-W1 in the interactive session: the probe
# validation, the editor fling and typing runs for both backends, and frost.exe's
# first frame warm and cold-proxy. Each step keeps its window on screen for 20 s
# or less, never uses exclusive fullscreen and closes what it opens; the whole
# suite is about two minutes on screen. A step that reports an invalid session
# (exit code 3: locked, monitor off, duplication lost) stops the suite, because
# later steps would be invalid too.
#
#   powershell -ExecutionPolicy Bypass -File tests\perf\s1\run-s1.ps1 -BuildDir build\s1 -OutDir <dir>
#
# The Release configuration of s1_probe_validate, s1_editor, s1_first_frame and
# frost must be built first. Reports are JSON files in -OutDir.

param(
    [Parameter(Mandatory = $true)] [string] $BuildDir,
    [Parameter(Mandatory = $true)] [string] $OutDir,
    [string] $Config = 'Release',
    [switch] $SkipFirstFrame
)

$ErrorActionPreference = 'Stop'
$build = (Resolve-Path $BuildDir).Path
New-Item -ItemType Directory -Force $OutDir | Out-Null
$out = (Resolve-Path $OutDir).Path
$s1 = Join-Path $build "tests\perf\s1\$Config"
$frost = Join-Path $build "src\app\$Config\frost.exe"

# Records Defender real-time protection and the Dev Drive state of the build
# volume, which the cold numbers depend on (budgets.json "cold" default).
$environment = [ordered]@{}
try {
    $mp = Get-MpComputerStatus -ErrorAction Stop
    $environment.defenderRealTime = $mp.RealTimeProtectionEnabled
} catch {
    $environment.defenderRealTime = 'unreadable'
}
$volume = (Split-Path -Qualifier $build)
$devDrive = (& fsutil devdrv query $volume 2>&1 | Out-String).Trim()
$environment.devDrive = $devDrive
$environment | ConvertTo-Json | Set-Content -Encoding utf8 (Join-Path $out 'environment.json')

function Invoke-Step([string] $name, [string] $exe, [string[]] $arguments) {
    $report = Join-Path $out "$name.json"
    Write-Host "== $name"
    & $exe @arguments --out $report
    $code = $LASTEXITCODE
    if ($code -eq 3) {
        Write-Host "$name reported an invalid session; stopping (later runs would be invalid too)."
        exit 3
    }
    if ($code -ne 0 -and $code -ne 1) {
        throw "$name failed with exit code $code"
    }
}

Invoke-Step 'validate' (Join-Path $s1 's1_probe_validate.exe') @()
$editor = Join-Path $s1 's1_editor.exe'
Invoke-Step 'raster-text' $editor @('--backend', 'raster')
Invoke-Step 'raster-text-blit' $editor @('--backend', 'raster', '--scroll-blit')
Invoke-Step 'raster-fill' $editor @('--backend', 'raster', '--content', 'fill')
Invoke-Step 'rhi-text' $editor @('--backend', 'rhi')
Invoke-Step 'rhi-fill' $editor @('--backend', 'rhi', '--content', 'fill')
if (-not $SkipFirstFrame) {
    $first = Join-Path $s1 's1_first_frame.exe'
    Invoke-Step 'first-frame-warm' $first @('--exe', $frost, '--mode', 'warm', '--runs', '8')
    Invoke-Step 'first-frame-cold' $first @('--exe', $frost, '--mode', 'cold', '--runs', '5')
}
Write-Host "S1 reports written to $out"
