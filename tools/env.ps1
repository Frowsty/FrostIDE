# SPDX-License-Identifier: Apache-2.0
#
# env.ps1: puts the pinned toolchain from tools/bootstrap.ps1 on PATH for the
# current PowerShell session only. Dot-source it:  . tools\env.ps1
#
# It also points the NuGet package folder, the vcpkg downloads and sccache at the
# SDK root and the vcpkg binary cache at <SDK drive>\vcpkg-cache, sets DOTNET_ROOT to
# the pinned .NET SDK, and turns off tool telemetry. The .NET CLI home and NuGet's
# HTTP and plugin caches are not redirected yet and still use the user profile on C:.
# Nothing is persisted machine-wide.
# TODO(M0): redirect DOTNET_CLI_HOME, NUGET_HTTP_CACHE_PATH and NUGET_PLUGINS_CACHE_PATH
# to the SDK root as well (D1: caches on E:).

param(
    # Must match the -SdkRoot given to bootstrap.ps1.
    [string]$SdkRoot = 'E:\sdk'
)

$repoRoot = Split-Path -Parent $PSScriptRoot
$lock = Get-Content -Raw -Path (Join-Path $repoRoot 'sdk\sdk.lock.json') | ConvertFrom-Json

# Each archive component lives in <SdkRoot>\<name>-<version>; its "bin" field
# names the directory that holds the executables.
$paths = @()
foreach ($name in @('llvm', 'cmake', 'ninja', 'gh', 'sccache', 'dotnet')) {
    $pin = $lock.windows.$name
    $bin = Join-Path (Join-Path $SdkRoot "$name-$($pin.version)") $pin.bin
    if (Test-Path $bin) { $paths += (Resolve-Path $bin).Path } else { Write-Warning "missing $name ($bin); run tools\bootstrap.ps1" }
}
$paths += (Join-Path $SdkRoot 'vcpkg')

# Remove earlier copies of our entries so dot-sourcing twice does not grow PATH.
$rest = @($env:PATH -split ';' | Where-Object { $_ -and ($paths -notcontains $_) })
$env:PATH = (($paths + $rest) -join ';')

$env:FROST_SDK = $SdkRoot
$env:FROST_LLVM_ROOT = Join-Path $SdkRoot "llvm-$($lock.windows.llvm.version)"
$env:FROST_QT_ROOT = Join-Path $SdkRoot "qt\$($lock.qt.version)\msvc2022_64"
$env:VCPKG_ROOT = Join-Path $SdkRoot 'vcpkg'
$env:VCPKG_DISABLE_METRICS = '1'
$env:VCPKG_DOWNLOADS = Join-Path $SdkRoot '_downloads\vcpkg'
$env:VCPKG_DEFAULT_BINARY_CACHE = Join-Path (Split-Path -Qualifier $SdkRoot) 'vcpkg-cache'
$env:DOTNET_ROOT = Join-Path $SdkRoot "dotnet-$($lock.windows.dotnet.version)"
$env:DOTNET_CLI_TELEMETRY_OPTOUT = '1'
$env:DOTNET_NOLOGO = '1'
$env:DOTNET_SKIP_FIRST_TIME_EXPERIENCE = '1'
$env:NUGET_PACKAGES = Join-Path $SdkRoot 'nuget'
$env:SCCACHE_DIR = Join-Path $SdkRoot 'sccache-cache'
$env:GH_NO_UPDATE_NOTIFIER = '1'

# vcpkg refuses to start when its binary cache directory does not exist.
foreach ($dir in @($env:VCPKG_DOWNLOADS, $env:VCPKG_DEFAULT_BINARY_CACHE, $env:NUGET_PACKAGES, $env:SCCACHE_DIR)) {
    if (-not (Test-Path $dir)) { New-Item -ItemType Directory -Force -Path $dir | Out-Null }
}
