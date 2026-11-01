# SPDX-License-Identifier: Apache-2.0
#
# env.ps1: puts the pinned toolchain from tools/bootstrap.ps1 on PATH for the
# current PowerShell session only. Dot-source it:  . tools\env.ps1
#
# It also points every tool cache that would otherwise land in the user profile on
# the system drive at the SDK root (D1): the NuGet package folder and its HTTP and
# plugin caches, the .NET CLI home, the vcpkg downloads and sccache; the vcpkg binary
# cache goes to <SDK drive>\vcpkg-cache. It sets DOTNET_ROOT to the pinned .NET SDK
# and turns off tool telemetry. Nothing is persisted machine-wide.
#
# Moving the .NET CLI home (DOTNET_CLI_HOME) has side effects in these sessions:
#  - Global tools: `dotnet tool install -g` installs into <SDK root>\dotnet-home\.dotnet\tools,
#    and global tools installed earlier under %USERPROFILE%\.dotnet\tools are not
#    listed or updated by `dotnet tool ... -g` here. Local tool manifests are unaffected.
#  - First use: the CLI keeps its first-use sentinels and user-level caches in
#    <SDK root>\dotnet-home\.dotnet, so the first dotnet command against a new home
#    repeats the first-use steps. Two of them persist outside the session (adding the
#    global-tools directory to the user PATH, creating the HTTPS developer certificate),
#    so DOTNET_ADD_GLOBAL_TOOLS_TO_PATH and DOTNET_GENERATE_ASPNET_CERTIFICATE turn them off.

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
$env:DOTNET_CLI_HOME = Join-Path $SdkRoot 'dotnet-home'
$env:DOTNET_ADD_GLOBAL_TOOLS_TO_PATH = '0'
$env:DOTNET_GENERATE_ASPNET_CERTIFICATE = '0'
$env:NUGET_PACKAGES = Join-Path $SdkRoot 'nuget'
$env:NUGET_HTTP_CACHE_PATH = Join-Path $SdkRoot 'nuget-http-cache'
$env:NUGET_PLUGINS_CACHE_PATH = Join-Path $SdkRoot 'nuget-plugins-cache'
$env:SCCACHE_DIR = Join-Path $SdkRoot 'sccache-cache'
$env:GH_NO_UPDATE_NOTIFIER = '1'

# Create the cache directories up front: vcpkg refuses to start when its binary
# cache directory does not exist, and the others then exist before any tool runs.
$cacheDirs = @($env:VCPKG_DOWNLOADS, $env:VCPKG_DEFAULT_BINARY_CACHE, $env:DOTNET_CLI_HOME, $env:NUGET_PACKAGES,
    $env:NUGET_HTTP_CACHE_PATH, $env:NUGET_PLUGINS_CACHE_PATH, $env:SCCACHE_DIR)
foreach ($dir in $cacheDirs) {
    if (-not (Test-Path $dir)) { New-Item -ItemType Directory -Force -Path $dir | Out-Null }
}
