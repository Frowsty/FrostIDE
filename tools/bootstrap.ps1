# SPDX-License-Identifier: Apache-2.0
#
# bootstrap.ps1: installs the pinned FrostIDE development toolchain on Windows.
#
# Everything goes under -SdkRoot (default E:\sdk) so the system drive stays
# untouched: LLVM, CMake, Ninja, gh, sccache, the .NET SDK, vcpkg, a Python
# venv for tooling and a subset of Qt. Versions, URLs and hashes come from
# sdk/sdk.lock.json; an archive whose hash differs from the pin is rejected.
# The script is idempotent: a component whose marker file matches its pin is
# skipped, so re-running after a partial failure only redoes what is missing.
#
# Nothing here changes machine-wide state (no PATH edits, no registry writes).
# tools/env.ps1 puts the installed tools on PATH for the current session.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File tools\bootstrap.ps1 [-SdkRoot E:\sdk] [-Only llvm,qt]

[CmdletBinding()]
param(
    # Root directory for every installed component.
    [string]$SdkRoot = 'E:\sdk',
    # Optional subset of components to install (llvm, cmake, ninja, gh, sccache, dotnet, python, qt, vcpkg).
    [string[]]$Only = @()
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 3.0

$RepoRoot = Split-Path -Parent $PSScriptRoot
$Lock = Get-Content -Raw -Path (Join-Path $RepoRoot 'sdk\sdk.lock.json') | ConvertFrom-Json
$Downloads = Join-Path $SdkRoot '_downloads'

# Free-space floors from PLAN.md section 7 (M0): stop before the system drive gets tight.
$MinFreeSystemGB = 15
$MinFreeSdkGB = 150

# Returns $true when the named component should be installed in this run.
function Test-Wanted([string]$Name) {
    return ($Only.Count -eq 0) -or ($Only -contains $Name)
}

# Stops the script if either drive is below its free-space floor.
function Assert-FreeSpace {
    $systemDrive = (Get-PSDrive -Name ($env:SystemDrive.TrimEnd(':')))
    $sdkDrive = (Get-PSDrive -Name ((Split-Path -Qualifier $SdkRoot).TrimEnd(':')))
    $systemFree = [math]::Round($systemDrive.Free / 1GB, 1)
    $sdkFree = [math]::Round($sdkDrive.Free / 1GB, 1)
    Write-Host "free space: $($env:SystemDrive) $systemFree GB, $(Split-Path -Qualifier $SdkRoot) $sdkFree GB"
    if ($systemFree -lt $MinFreeSystemGB) { throw "system drive has $systemFree GB free; need at least $MinFreeSystemGB GB" }
    if ($sdkFree -lt $MinFreeSdkGB) { throw "SDK drive has $sdkFree GB free; need at least $MinFreeSdkGB GB" }
}

# Downloads $Url to $Path with curl.exe (much faster than Invoke-WebRequest in
# Windows PowerShell 5.1) through a .part file, unless $Path already exists.
function Get-Download([string]$Url, [string]$Path) {
    if (Test-Path $Path) { return }
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $Path) | Out-Null
    $part = "$Path.part"
    & "$env:SystemRoot\System32\curl.exe" --fail --location --silent --show-error --retry 3 -o $part $Url
    if ($LASTEXITCODE -ne 0) { throw "download failed ($LASTEXITCODE): $Url" }
    Move-Item -Force $part $Path
}

# Throws unless the file's hash equals the pinned SHA-256 or SHA-512 of $Pin.
function Assert-Hash([string]$Path, $Pin) {
    if ($Pin.PSObject.Properties.Name -contains 'sha256') {
        $algorithm = 'SHA256'; $expected = $Pin.sha256
    } else {
        $algorithm = 'SHA512'; $expected = $Pin.sha512
    }
    $actual = (Get-FileHash -Algorithm $algorithm -Path $Path).Hash.ToLowerInvariant()
    if ($actual -ne $expected.ToLowerInvariant()) {
        Remove-Item -Force $Path
        throw "$algorithm mismatch for $Path`n  expected $expected`n  actual   $actual (file deleted)"
    }
}

# Installs one archive component from the lock file into $SdkRoot\<name>-<version>.
# Extraction uses the in-box bsdtar (tar.exe), which reads zip, tar.gz and tar.zst.
# If the archive has a single top-level directory, its contents become the root.
function Install-Archive([string]$Name) {
    $pin = $Lock.windows.$Name
    $dest = Join-Path $SdkRoot "$Name-$($pin.version)"
    $marker = Join-Path $dest '.frost-installed'
    if ((Test-Path $marker) -and ((Get-Content -Raw $marker).Trim() -eq $pin.url)) {
        Write-Host "[$Name] $($pin.version) already installed"
        return
    }
    $archive = Join-Path $Downloads $pin.file
    Write-Host "[$Name] downloading $($pin.version)"
    Get-Download $pin.url $archive
    Assert-Hash $archive $pin

    $staging = Join-Path $SdkRoot "_staging\$Name"
    if (Test-Path $staging) { Remove-Item -Recurse -Force $staging }
    New-Item -ItemType Directory -Force -Path $staging | Out-Null
    Write-Host "[$Name] extracting"
    & "$env:SystemRoot\System32\tar.exe" -xf $archive -C $staging
    if ($LASTEXITCODE -ne 0) { throw "extraction failed for $archive" }

    $entries = @(Get-ChildItem -Force $staging)
    $source = $staging
    if ($entries.Count -eq 1 -and $entries[0].PSIsContainer) { $source = $entries[0].FullName }
    if (Test-Path $dest) { Remove-Item -Recurse -Force $dest }
    Move-Item $source $dest
    if (Test-Path $staging) { Remove-Item -Recurse -Force $staging }
    Set-Content -Path $marker -Value $pin.url -Encoding ascii
    Write-Host "[$Name] installed to $dest"
}

# Creates the tooling venv ($SdkRoot\py) with pinned packages (aqtinstall for
# listing Qt, py7zr for qt-fetch.py).
function Install-PythonTools {
    $venv = Join-Path $SdkRoot 'py'
    $python = Join-Path $venv 'Scripts\python.exe'
    if (-not (Test-Path $python)) {
        Write-Host '[python] creating venv'
        & python -m venv $venv
        if ($LASTEXITCODE -ne 0) { throw 'python -m venv failed (is Python 3.10+ on PATH?)' }
    }
    $packages = @("aqtinstall==$($Lock.python.aqtinstall)", "py7zr==$($Lock.python.py7zr)")
    & $python -m pip install --disable-pip-version-check --quiet @packages
    if ($LASTEXITCODE -ne 0) { throw 'pip install failed' }
    Write-Host "[python] venv ready: $(& $python --version)"
}

# Installs the pinned Qt subset with tools/qt-fetch.py into $SdkRoot\qt.
function Install-Qt {
    $python = Join-Path $SdkRoot 'py\Scripts\python.exe'
    $qt = $Lock.qt.windows
    $fetchArgs = @(
        (Join-Path $PSScriptRoot 'qt-fetch.py'),
        '--repository', $qt.repository,
        '--package', $qt.package,
        '--archives', ($qt.archives -join ','),
        '--dest', (Join-Path $SdkRoot 'qt'),
        '--cache', (Join-Path $Downloads 'qt')
    )
    if ($qt.updatesSha256) { $fetchArgs += @('--updates-sha256', $qt.updatesSha256) }
    & $python @fetchArgs
    if ($LASTEXITCODE -ne 0) { throw 'qt-fetch.py failed' }
}

# Clones vcpkg at the pinned tag into $SdkRoot\vcpkg and builds vcpkg.exe.
function Install-Vcpkg {
    $root = Join-Path $SdkRoot 'vcpkg'
    $tag = $Lock.vcpkg.tag
    if (-not (Test-Path (Join-Path $root '.git'))) {
        Write-Host "[vcpkg] cloning $tag"
        & git clone --quiet --depth 1 --branch $tag $Lock.vcpkg.repository $root
        if ($LASTEXITCODE -ne 0) { throw 'git clone of vcpkg failed' }
    } else {
        $current = (& git -C $root describe --tags --exact-match 2>$null)
        if ($current -ne $tag) {
            Write-Host "[vcpkg] moving from '$current' to $tag"
            & git -C $root fetch --quiet --depth 1 origin "refs/tags/${tag}:refs/tags/${tag}"
            & git -C $root checkout --quiet $tag
            if ($LASTEXITCODE -ne 0) { throw "vcpkg checkout of $tag failed" }
        }
    }
    if (-not (Test-Path (Join-Path $root 'vcpkg.exe'))) {
        & (Join-Path $root 'bootstrap-vcpkg.bat') -disableMetrics
        if ($LASTEXITCODE -ne 0) { throw 'bootstrap-vcpkg.bat failed' }
    }
    Write-Host "[vcpkg] ready at $root ($tag)"
}

# Verifies that the pinned MSVC toolset and Windows SDK exist. They come from a
# Visual Studio (Build Tools) install, which needs elevation and is therefore
# never installed by this script; clang-cl compiles against them.
function Assert-Msvc {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path $vswhere)) { throw 'vswhere.exe not found: install Visual Studio 2022 Build Tools (C++ workload)' }
    $found = $false
    foreach ($vs in @(& $vswhere -all -products * -property installationPath)) {
        if (Test-Path (Join-Path $vs "VC\Tools\MSVC\$($Lock.msvc.vcToolsVersion)")) { $found = $true }
    }
    if (-not $found) { throw "MSVC toolset $($Lock.msvc.vcToolsVersion) not found in any Visual Studio install" }
    $sdkInclude = Join-Path ${env:ProgramFiles(x86)} "Windows Kits\10\Include\$($Lock.msvc.windowsSdkVersion)"
    if (-not (Test-Path $sdkInclude)) { throw "Windows SDK $($Lock.msvc.windowsSdkVersion) not found" }
    Write-Host "[msvc] toolset $($Lock.msvc.vcToolsVersion) and Windows SDK $($Lock.msvc.windowsSdkVersion) present"
}

Assert-FreeSpace
Assert-Msvc
New-Item -ItemType Directory -Force -Path $Downloads | Out-Null

foreach ($name in @('llvm', 'cmake', 'ninja', 'gh', 'sccache', 'dotnet')) {
    if (Test-Wanted $name) { Install-Archive $name }
}
if (Test-Wanted 'python') { Install-PythonTools }
if (Test-Wanted 'qt') { Install-Qt }
if (Test-Wanted 'vcpkg') { Install-Vcpkg }

Write-Host 'bootstrap complete; dot-source tools\env.ps1 to use the toolchain in this shell'
