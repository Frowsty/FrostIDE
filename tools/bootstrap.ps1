# SPDX-License-Identifier: Apache-2.0
#
# bootstrap.ps1: installs the pinned FrostIDE development toolchain on Windows.
#
# Everything goes under -SdkRoot (default E:\sdk) so the system drive stays
# untouched: LLVM, CMake, Ninja, gh, sccache, the .NET SDK, vcpkg, a Python
# venv for tooling, a subset of Qt, LuaLS (ADR 0079) and the self-built lldb-dap. Versions, URLs and
# hashes come from sdk/sdk.lock.json; an archive whose hash differs from the pin is
# rejected. lldb-dap has no official Windows binary (spike S5), so it is built from
# the pinned LLVM sources by tools/build-lldb.ps1 (about 15 minutes) when it is missing,
# and its installed files are checked against their pinned hashes otherwise (ADR 0049).
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
    # Optional subset (llvm, cmake, ninja, gh, sccache, dotnet, conpty, luals, python, managed, qt, vcpkg, lldb).
    [string[]]$Only = @()
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 3.0

# `powershell -File` passes "-Only llvm,qt" as the single string 'llvm,qt'
# (only -Command parses comma lists), so split every element on commas.
$Only = @($Only | ForEach-Object { $_ -split ',' } | ForEach-Object { $_.Trim() } | Where-Object { $_ })

$RepoRoot = Split-Path -Parent $PSScriptRoot
$Lock = Get-Content -Raw -Path (Join-Path $RepoRoot 'sdk\sdk.lock.json') | ConvertFrom-Json
$Downloads = Join-Path $SdkRoot '_downloads'

# Free-space floors from PLAN.md section 7 (M0): stop before the system drive gets tight.
$MinFreeSystemGB = 15
$MinFreeSdkGB = 150
# The oldest Python the repository checks run on (tomllib; tools/frost_lint.py checks it too).
$MinPython = [version]'3.11'

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
# Extraction uses the in-box bsdtar (tar.exe), which reads the pinned .zip and .tar.xz archives
# (it refuses LLVM's .tar.zst, whose 2 GiB zstd window exceeds its memory limit; see the lock file).
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

# Runs a native command whose stderr is expected noise (git's "no tag" message,
# for instance) and returns its stdout. Under Windows PowerShell 5.1 with
# $ErrorActionPreference = 'Stop', any stderr line from a native command becomes
# a terminating error even when redirected, so the preference is relaxed in this
# function's scope only. Callers check $LASTEXITCODE.
function Invoke-NativeQuiet([scriptblock]$Command) {
    $ErrorActionPreference = 'Continue'
    return (& $Command 2>$null)
}

# Throws unless $VersionText, the "major.minor.micro" that $Interpreter printed for
# sys.version_info, is $MinPython or newer. Text that is not a version (an
# interpreter that failed to start, say) also throws. Both messages name $Venv to
# delete: an existing venv keeps the interpreter it was created with, and
# Install-PythonTools recreates it only when python.exe is missing, so a venv whose
# base Python was uninstalled stays broken until it is deleted.
function Assert-PythonVersion([string]$VersionText, [string]$Interpreter, [string]$Venv) {
    $text = "$VersionText".Trim()
    if ($text -notmatch '^(\d+)\.(\d+)\.(\d+)$') {
        throw ("could not read the Python version of $Interpreter (got '$text'). If its base Python was " +
               "removed or replaced, put Python $MinPython or newer on PATH, delete $Venv and run the bootstrap again.")
    }
    $version = [version]::new([int]$Matches[1], [int]$Matches[2], [int]$Matches[3])
    if ($version -lt $MinPython) {
        throw ("$Interpreter is Python $version, but the repository checks need Python $MinPython or newer " +
               "(tomllib). Put a newer Python on PATH, delete $Venv and run the bootstrap again.")
    }
}

# Creates the tooling venv ($SdkRoot\py) that runs qt-fetch.py and the repository
# checks, and stops unless its interpreter is $MinPython or newer (checked on every
# run, so an existing venv from an older Python is caught too). Every tool uses the
# standard library only, so nothing is pip-installed.
function Install-PythonTools {
    $venv = Join-Path $SdkRoot 'py'
    $python = Join-Path $venv 'Scripts\python.exe'
    if (-not (Test-Path $python)) {
        Write-Host '[python] creating venv'
        & python -m venv $venv
        if ($LASTEXITCODE -ne 0) { throw "python -m venv failed (is Python $MinPython+ on PATH?)" }
    }
    $versionText = Invoke-NativeQuiet { & $python -I -c "import sys; print('%d.%d.%d' % sys.version_info[:3])" }
    Assert-PythonVersion $versionText $python $venv
    Write-Host "[python] venv ready: Python $("$versionText".Trim())"
}

# Installs the pinned Qt subset with tools/qt-fetch.py into $SdkRoot\qt; the
# script reads the repository, archives and hashes from the lock file itself.
function Install-Qt {
    $python = Join-Path $SdkRoot 'py\Scripts\python.exe'
    & $python -I (Join-Path $PSScriptRoot 'qt-fetch.py') `
        --lock (Join-Path $RepoRoot 'sdk\sdk.lock.json') --platform windows `
        --dest (Join-Path $SdkRoot 'qt') --cache (Join-Path $Downloads 'qt')
    if ($LASTEXITCODE -ne 0) { throw 'qt-fetch.py failed' }
}

# Clones vcpkg at the pinned tag into $SdkRoot\vcpkg and builds vcpkg.exe. When
# the pin moves, the checkout follows and vcpkg.exe is rebuilt, because the tool
# version is part of the tag. The tag must be the commit vcpkg.json names as its
# builtin-baseline, so port versions and the tool always agree.
function Install-Vcpkg {
    $root = Join-Path $SdkRoot 'vcpkg'
    $exe = Join-Path $root 'vcpkg.exe'
    $tag = $Lock.vcpkg.tag
    if (-not (Test-Path (Join-Path $root '.git'))) {
        Write-Host "[vcpkg] cloning $tag"
        & git clone --quiet --depth 1 --branch $tag $Lock.vcpkg.repository $root
        if ($LASTEXITCODE -ne 0) { throw 'git clone of vcpkg failed' }
    } else {
        $current = Invoke-NativeQuiet { git -C $root describe --tags --exact-match }
        if ($current -ne $tag) {
            Write-Host "[vcpkg] moving from '$current' to $tag"
            & git -C $root fetch --quiet --depth 1 origin "refs/tags/${tag}:refs/tags/${tag}"
            if ($LASTEXITCODE -ne 0) { throw "vcpkg fetch of $tag failed" }
            & git -C $root -c advice.detachedHead=false checkout --quiet $tag
            if ($LASTEXITCODE -ne 0) { throw "vcpkg checkout of $tag failed" }
            if (Test-Path $exe) { Remove-Item -Force $exe }
        }
    }
    $head = (& git -C $root rev-parse HEAD)
    $baseline = (Get-Content -Raw -Path (Join-Path $RepoRoot 'vcpkg.json') | ConvertFrom-Json).'builtin-baseline'
    if ($head -ne $baseline) {
        throw "vcpkg tag $tag is commit $head, but vcpkg.json's builtin-baseline is $baseline; bump them together"
    }
    if (-not (Test-Path $exe)) {
        & (Join-Path $root 'bootstrap-vcpkg.bat') -disableMetrics
        if ($LASTEXITCODE -ne 0) { throw 'bootstrap-vcpkg.bat failed' }
    }
    Write-Host "[vcpkg] ready at $root ($tag, $head)"
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

# Installs or verifies the pinned managed server without replacing existing files.
function Install-ManagedServer {
    $pin = $Lock.managedServer
    $archive = Join-Path $Downloads "csharp-ls.$($pin.version).nupkg"
    $dest = Join-Path $SdkRoot 'managed'
    Get-Download $pin.url $archive
    Assert-Hash $archive $pin
    $python = Join-Path $SdkRoot 'py\Scripts\python.exe'
    if (-not (Test-Path $python)) {
        throw 'The managed server installer needs the pinned Python tools; bootstrap python first.'
    }
    $installer = Join-Path $PSScriptRoot 'managed_server.py'
    if (Test-Path $dest) {
        & $python $installer --archive $archive --dest $dest --verify-only
    } else {
        & $python $installer --archive $archive --dest $dest
    }
    if ($LASTEXITCODE -ne 0) { throw 'Managed server install or verification failed' }
    Write-Host "[managed] $($pin.version) verified in $dest"
}

# Verifies the self-built lldb-dap in $SdkRoot\lldb-<version> against windows.lldb.files in the
# lock file, or builds it from the pinned LLVM sources first when it is not installed. Runs last,
# because the build needs LLVM, CMake, Ninja and the Python venv; tools\build-lldb.ps1 does both
# (with -VerifyOnly for the check) and throws on a mismatch.
function Install-Lldb {
    $pin = $Lock.windows.lldb
    $recipe = Join-Path $PSScriptRoot 'build-lldb.ps1'
    $dest = Join-Path $SdkRoot "lldb-$($pin.version)"
    if (Test-Path (Join-Path $dest '.frost-installed')) {
        & $recipe -SdkRoot $SdkRoot -VerifyOnly
        Write-Host "[lldb] $($pin.version) verified in $dest"
        return
    }
    Write-Host "[lldb] $($pin.version) is not installed: building it from the pinned LLVM sources (about 15 minutes)"
    & $recipe -SdkRoot $SdkRoot
}

Assert-FreeSpace
Assert-Msvc
New-Item -ItemType Directory -Force -Path $Downloads | Out-Null

# luals: the pinned LuaLS release (MIT, ADR 0079) lands in <SdkRoot>\luals-<version>, where
# frost::lang::defaultLuaSearch and tools\stage.ps1 look for bin\lua-language-server.exe.
foreach ($name in @('llvm', 'cmake', 'ninja', 'gh', 'sccache', 'dotnet', 'conpty', 'luals')) {
    if (Test-Wanted $name) { Install-Archive $name }
}
if (Test-Wanted 'python') { Install-PythonTools }
if (Test-Wanted 'managed') { Install-ManagedServer }
if (Test-Wanted 'qt') { Install-Qt }
if (Test-Wanted 'vcpkg') { Install-Vcpkg }
if (Test-Wanted 'lldb') { Install-Lldb }

Write-Host 'bootstrap complete; dot-source tools\env.ps1 to use the toolchain in this shell'
