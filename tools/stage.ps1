# SPDX-License-Identifier: Apache-2.0
#
# stage.ps1: builds FrostIDE (Release or RelWithDebInfo) and stages a folder that runs on its own
# (W1, ADR 0044): frost.exe and frost_app.dll, the crash handler, the baked resources, the Qt
# runtime windeployqt picks for frost_app.dll, the .NET Framework ProjectHost (which uses installed
# Visual Studio MSBuild), and the platform plugins (including the build's offscreen
# plugin too, for --selftest), and the licence files (LICENSE, NOTICE, the vendored sources'
# LICENSE files under licenses\). Windows only.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\stage.ps1 -Destination E:\FrostBuilds\preview\x
#              [-Config Release|RelWithDebInfo] [-BuildDir build\win-clangcl] [-Jobs 6] [-SkipBuild] [-Force]
#
# The destination must not exist or be empty; -Force replaces it. -SkipBuild stages what the build
# tree already holds (also -Config Debug, which the script's test uses). -WinDeployQt overrides the
# windeployqt.exe of the pinned Qt (E:\sdk\qt\<version>\msvc2022_64\bin, sdk\sdk.lock.json). The
# script then checks that the folder holds frost.exe, frost_app.dll and Qt's Windows platform
# plugin, and exits with 1 if anything failed. Smoke test of a staged folder: frost.exe --selftest
# (offscreen; exit status 0 when every check passed).

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Destination,
    [ValidateSet('Release', 'RelWithDebInfo', 'Debug')][string]$Config = 'Release',
    [string]$BuildDir = '',
    [int]$Jobs = 0,
    [string]$SdkRoot = 'E:\sdk',
    [string]$WinDeployQt = '',
    [switch]$SkipBuild,
    [switch]$Force
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
if (-not $BuildDir) { $BuildDir = Join-Path $repoRoot 'build\win-clangcl' }
$BuildDir = [System.IO.Path]::GetFullPath($BuildDir)
$Destination = [System.IO.Path]::GetFullPath($Destination)

function Fail([string]$message) {
    Write-Host "stage: FAIL $message"
    exit 1
}

# The pinned Qt's windeployqt, unless one was given.
if (-not $WinDeployQt) {
    $lock = Get-Content -Raw -Path (Join-Path $repoRoot 'sdk\sdk.lock.json') | ConvertFrom-Json
    $WinDeployQt = Join-Path $SdkRoot "qt\$($lock.qt.version)\msvc2022_64\bin\windeployqt.exe"
}
if (-not [System.IO.Path]::IsPathRooted($WinDeployQt) -or -not (Test-Path -LiteralPath $WinDeployQt)) {
    Fail "no windeployqt at $WinDeployQt"
}

# 1. Build the program (frost.exe builds frost_app.dll and copies the handler and plugins).
if (-not $SkipBuild) {
    if ($Config -eq 'Debug') { Fail '-Config Debug is for -SkipBuild only; stage Release or RelWithDebInfo' }
    . (Join-Path $PSScriptRoot 'env.ps1') -SdkRoot $SdkRoot | Out-Null
    $previous = $ErrorActionPreference
    $ErrorActionPreference = 'Continue' # cmake writes warnings to stderr.
    if (-not (Test-Path -LiteralPath (Join-Path $BuildDir 'build.ninja'))) {
        Push-Location $repoRoot
        try { $out = & cmake --preset win-clangcl 2>&1; $code = $LASTEXITCODE } finally { Pop-Location }
        if ($code -ne 0) { $out | Select-Object -Last 30 | ForEach-Object { Write-Host $_ }; Fail 'configure failed' }
    }
    $buildArgs = @('--build', $BuildDir, '--config', $Config, '--target', 'frost')
    if ($Jobs -gt 0) { $buildArgs += @('-j', "$Jobs") }
    $out = & cmake @buildArgs 2>&1
    $code = $LASTEXITCODE
    $ErrorActionPreference = $previous
    if ($code -ne 0) { $out | Select-Object -Last 40 | ForEach-Object { Write-Host $_ }; Fail "the $Config build failed" }
    Write-Host "stage: built frost ($Config)"
}

$binDir = Join-Path $BuildDir "src\app\$Config"
foreach ($name in @('frost.exe', 'frost_app.dll', 'crashpad_handler.exe', 'conpty.dll', 'OpenConsole.exe')) {
    if (-not (Test-Path -LiteralPath (Join-Path $binDir $name))) { Fail "the build tree has no $name in $binDir" }
}

# 2. A fresh destination.
if (Test-Path -LiteralPath $Destination) {
    $existing = @(Get-ChildItem -LiteralPath $Destination -Force)
    if ($existing.Count -gt 0) {
        if (-not $Force) { Fail "$Destination is not empty (use -Force to replace it)" }
        Remove-Item -LiteralPath $Destination -Recurse -Force
    }
}
New-Item -ItemType Directory -Force -Path $Destination | Out-Null

# 3. FrostIDE's own files.
foreach ($name in @('frost.exe', 'frost_app.dll', 'crashpad_handler.exe', 'conpty.dll', 'OpenConsole.exe')) {
    Copy-Item -LiteralPath (Join-Path $binDir $name) -Destination $Destination
}
$projectHost = Join-Path $binDir 'project-host'
foreach ($name in @('ProjectHostVsProbe.exe', 'ProjectHostVsProbe.exe.config')) {
    if (-not (Test-Path -LiteralPath (Join-Path $projectHost $name))) { Fail "the build tree has no ProjectHost $name" }
}
$projectHostDestination = Join-Path $Destination 'project-host'
New-Item -ItemType Directory -Force -Path $projectHostDestination | Out-Null
foreach ($name in @('ProjectHostVsProbe.exe', 'ProjectHostVsProbe.exe.config')) {
    Copy-Item -LiteralPath (Join-Path $projectHost $name) -Destination $projectHostDestination
}
foreach ($name in @('ProjectHostVsProbe.exe', 'ProjectHostVsProbe.exe.config')) {
    $sourceHash = (Get-FileHash -LiteralPath (Join-Path $projectHost $name) -Algorithm SHA256).Hash
    $stagedHash = (Get-FileHash -LiteralPath (Join-Path (Join-Path $Destination 'project-host') $name) -Algorithm SHA256).Hash
    if ($sourceHash -ne $stagedHash) { Fail "the staged ProjectHost $name differs from the build output" }
}
if ($Config -ne 'Release') {
    foreach ($name in @('frost.pdb', 'frost_app.pdb')) {
        $pdb = Join-Path $binDir $name
        if (Test-Path -LiteralPath $pdb) { Copy-Item -LiteralPath $pdb -Destination $Destination }
    }
}
$resources = Join-Path $binDir 'resources'
if (-not (Test-Path -LiteralPath $resources)) { Fail "the build tree has no resources in $binDir" }
Copy-Item -LiteralPath $resources -Destination $Destination -Recurse
# Distribute the original Unity package for explicit installation in a Unity project.
$unityPackage = Join-Path $repoRoot 'integrations\unity\com.frostide.editor'
$unityDestination = Join-Path $Destination 'unity'
New-Item -ItemType Directory -Force -Path $unityDestination | Out-Null
Copy-Item -LiteralPath $unityPackage -Destination $unityDestination -Recurse

# 4. The Qt runtime for frost_app.dll, then the build's platform plugins (offscreen for --selftest).
$mode = if ($Config -eq 'Debug') { '--debug' } else { '--release' }
$deployArgs = @($mode, '--no-translations', '--no-compiler-runtime', '--no-system-d3d-compiler',
    '--no-system-dxc-compiler', '--no-opengl-sw', '--no-ffmpeg',
    '--no-plugins', '--no-svg', '--dir', $Destination,
    (Join-Path $Destination 'frost_app.dll'))
$previous = $ErrorActionPreference
$ErrorActionPreference = 'Continue'
$out = & $WinDeployQt @deployArgs 2>&1
$code = $LASTEXITCODE
$ErrorActionPreference = $previous
if ($code -ne 0) { $out | Select-Object -Last 30 | ForEach-Object { Write-Host $_ }; Fail "windeployqt failed ($code)" }
$platforms = Join-Path $binDir 'platforms'
if (Test-Path -LiteralPath $platforms) {
    New-Item -ItemType Directory -Force -Path (Join-Path $Destination 'platforms') | Out-Null
    Copy-Item -Path (Join-Path $platforms '*') -Destination (Join-Path $Destination 'platforms') -Force
}
# Qt's default plugin selection adds optional image formats, styles and network
# backends. Plugins are disabled above; copy only the reviewed platform and system
# TLS backends from our build output.
$tlsPlugin = if ($Config -eq 'Debug') { 'qschannelbackendd.dll' } else { 'qschannelbackend.dll' }
$tlsSource = Join-Path (Join-Path $binDir 'tls') $tlsPlugin
if (-not (Test-Path -LiteralPath $tlsSource)) { Fail "the build has no HTTPS backend: $tlsSource" }
New-Item -ItemType Directory -Force -Path (Join-Path $Destination 'tls') | Out-Null
Copy-Item -LiteralPath $tlsSource -Destination (Join-Path $Destination 'tls') -Force

# 5. Licences: FrostIDE's, and the vendored sources' (NOTICE names them).
foreach ($name in @('LICENSE', 'NOTICE')) {
    Copy-Item -LiteralPath (Join-Path $repoRoot $name) -Destination $Destination
}
$licenses = Join-Path $Destination 'licenses'
New-Item -ItemType Directory -Force -Path $licenses | Out-Null
# The overlay's Microsoft MIT notice covers the port recipe, not the linked sodium
# library. Preserve the actual upstream ISC notice from this build's installed prefix.
$cachePath = Join-Path $BuildDir 'CMakeCache.txt'
if (-not (Test-Path -LiteralPath $cachePath)) { Fail 'no CMake cache for dependency notice provenance' }
$cacheText = Get-Content -Raw -LiteralPath $cachePath
$installedMatch = [regex]::Match($cacheText, '(?m)^VCPKG_INSTALLED_DIR:[^=]+=([^\r\n]+)')
$tripletMatch = [regex]::Match($cacheText, '(?m)^VCPKG_TARGET_TRIPLET:[^=]+=([a-zA-Z0-9_-]+)\r?$')
if (-not $installedMatch.Success -or -not $tripletMatch.Success -or
    -not [System.IO.Path]::IsPathRooted($installedMatch.Groups[1].Value)) {
    Fail 'no absolute installed prefix and target triplet for dependency notices'
}
$sodiumNotice = Join-Path $installedMatch.Groups[1].Value ($tripletMatch.Groups[1].Value + '/share/libsodium/copyright')
if (-not (Test-Path -LiteralPath $sodiumNotice -PathType Leaf)) { Fail 'missing upstream libsodium copyright notice' }
$stagedSodiumNotice = Join-Path $licenses 'libsodium-copyright.txt'
Copy-Item -LiteralPath $sodiumNotice -Destination $stagedSodiumNotice
if ((Get-FileHash -LiteralPath $sodiumNotice -Algorithm SHA256).Hash -ne
    (Get-FileHash -LiteralPath $stagedSodiumNotice -Algorithm SHA256).Hash) {
    Fail 'the staged libsodium notice differs from the installed upstream notice'
}
$thirdParty = Join-Path $repoRoot 'third_party'
foreach ($file in @(Get-ChildItem -LiteralPath $thirdParty -Recurse -File -Filter 'LICENSE*')) {
    $relative = $file.DirectoryName.Substring($thirdParty.Length).TrimStart('\') -replace '\\', '-'
    $target = Join-Path $licenses ("$relative-$($file.Name)".TrimStart('-'))
    Copy-Item -LiteralPath $file.FullName -Destination $target
}

# 6. The checks.
$plugin = if ($Config -eq 'Debug') { 'platforms\qwindowsd.dll' } else { 'platforms\qwindows.dll' }
foreach ($name in @('frost.exe', 'frost_app.dll', 'crashpad_handler.exe', 'conpty.dll', 'OpenConsole.exe', 'LICENSE', 'NOTICE', $plugin)) {
    if (-not (Test-Path -LiteralPath (Join-Path $Destination $name))) { Fail "the staged folder has no $name" }
}
if ((Get-FileHash -LiteralPath $tlsSource -Algorithm SHA256).Hash -ne
    (Get-FileHash -LiteralPath (Join-Path (Join-Path $Destination 'tls') $tlsPlugin) -Algorithm SHA256).Hash) {
    Fail 'the staged HTTPS backend differs from the build output'
}
# The staged ConPTY pair must be byte-identical to the pair the build deployed.
foreach ($name in @('conpty.dll', 'OpenConsole.exe')) {
    $sourceHash = (Get-FileHash -LiteralPath (Join-Path $binDir $name) -Algorithm SHA256).Hash
    $stagedHash = (Get-FileHash -LiteralPath (Join-Path $Destination $name) -Algorithm SHA256).Hash
    if ($sourceHash -ne $stagedHash) { Fail "the staged $name differs from the build output" }
}
$qtCore = if ($Config -eq 'Debug') { 'Qt6Cored.dll' } else { 'Qt6Core.dll' }
if (-not (Test-Path -LiteralPath (Join-Path $Destination $qtCore))) { Fail "the staged folder has no $qtCore" }
$files = @(Get-ChildItem -LiteralPath $Destination -Recurse -File)
$bytes = ($files | Measure-Object -Property Length -Sum).Sum
Write-Host ("stage: ok {0} ({1} files, {2:N1} MB)" -f $Destination, $files.Count, ($bytes / 1MB))
exit 0
