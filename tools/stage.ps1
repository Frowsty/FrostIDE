# SPDX-License-Identifier: Apache-2.0
#
# stage.ps1: builds FrostIDE (Release or RelWithDebInfo) and stages a folder that runs on its own
# (W1, ADR 0044): frost.exe and frost_app.dll, the crash handler, the baked resources, the Qt
# runtime windeployqt picks for frost_app.dll, the .NET Framework ProjectHost (which uses installed
# Visual Studio MSBuild), and the platform plugins (including the build's offscreen
# plugin too, for --selftest), and the licence files (LICENSE, NOTICE, the vendored sources'
# LICENSE files under licenses\). Release and RelWithDebInfo (and Debug with -BundleTools) also
# bundle the pinned clangd with its resource directory, lldb-dap, LuaLS (ADR 0079), the managed
# language server and the managed debugger; a missing one fails the stage (FND-13). Next to the
# folder, <Destination>-symbols.zip holds frost.exe and frost_app.dll with the PDBs whose GUID and
# age match them (tools/symbolize.py archive; symbolize.py --symbol-archive reads it). Windows only.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\stage.ps1 -Destination E:\FrostBuilds\preview\x
#              -QtSourceArchive <pinned-qtbase-source.tar.xz>
#              [-Config Release|RelWithDebInfo] [-BuildDir build\win-clangcl] [-Jobs 6] [-SkipBuild] [-Force]
#              [-Python <python.exe>] [-BundleTools]
#
# -Python defaults to the SDK's (E:\sdk\py\Scripts\python.exe), which runs tools/symbolize.py.
# The destination and its symbol archive must not exist (or the folder be empty); -Force replaces
# them. -SkipBuild stages what the build
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
    [string]$QtSourceArchive = '',
    [string]$ManagedServerDir = 'E:\FrostBuilds\managed-server-0.28.0-reviewed',
    [string]$ManagedDebugDir = 'E:\FrostBuilds\managed-debug\installed-app-v13\FrostManagedDebug',
    [string]$Python = '',
    [switch]$BundleTools,
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

# Release staging must include source and notices. Debug remains useful for local
# fixture/preview checks; supply the archive there too when preparing a package.
if ($Config -ne 'Debug' -and -not $QtSourceArchive) {
    Fail '-QtSourceArchive is required for Release/RelWithDebInfo packaging'
}
if ($QtSourceArchive -and -not (Test-Path -LiteralPath $QtSourceArchive -PathType Leaf)) {
    Fail 'the Qt source archive does not exist'
}

$lock = Get-Content -Raw -Path (Join-Path $repoRoot 'sdk\sdk.lock.json') | ConvertFrom-Json
# The pinned Qt's windeployqt, unless one was given.
if (-not $WinDeployQt) {
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

# The bundled tools must all be there (FND-13): a stage without one would ship a feature that
# cannot work, so a missing tool fails here, before the destination is touched.
$bundle = $Config -ne 'Debug' -or $BundleTools
$llvmRel = "llvm-$($lock.windows.llvm.version)"
$clangdSource = Join-Path $SdkRoot "$llvmRel\bin\clangd.exe"
$clangLibSource = Join-Path $SdkRoot "$llvmRel\lib\clang"
$lldbRel = "lldb-$($lock.windows.lldb.version)"
$lldbSourceDir = Join-Path $SdkRoot "$lldbRel\bin"
$lldbFiles = @('lldb-dap.exe', 'liblldb.dll', 'lldb-argdumper.exe')
$lualsRel = "luals-$($lock.windows.luals.version)"
$lualsSource = Join-Path $SdkRoot $lualsRel
if ($bundle) {
    if (-not (Test-Path -LiteralPath $clangdSource -PathType Leaf)) { Fail "missing bundled clangd: $clangdSource" }
    if (-not (Test-Path -LiteralPath $clangLibSource -PathType Container)) {
        Fail "missing bundled clangd resource directory: $clangLibSource"
    }
    foreach ($lldbFile in $lldbFiles) {
        $path = Join-Path $lldbSourceDir $lldbFile
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { Fail "missing bundled lldb-dap file: $path" }
    }
    foreach ($name in @('bin\lua-language-server.exe', 'main.lua', 'LICENSE')) {
        $path = Join-Path $lualsSource $name
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { Fail "missing bundled LuaLS file: $path (tools\bootstrap.ps1 -Only luals)" }
    }
    if (-not $ManagedServerDir -or -not (Test-Path -LiteralPath (Join-Path $ManagedServerDir 'CSharpLanguageServer.dll') -PathType Leaf)) {
        Fail "missing bundled managed language server: no CSharpLanguageServer.dll in '$ManagedServerDir'"
    }
    if (-not $ManagedDebugDir -or -not (Test-Path -LiteralPath (Join-Path $ManagedDebugDir 'adapter\netcoredbg.exe') -PathType Leaf)) {
        Fail "missing bundled managed debugger: no adapter\netcoredbg.exe in '$ManagedDebugDir'"
    }
}

# The symbol archive goes next to the stage (FND-13), written by tools/symbolize.py.
if (-not $Python) { $Python = Join-Path $SdkRoot 'py\Scripts\python.exe' }
if (-not [System.IO.Path]::IsPathRooted($Python) -or -not (Test-Path -LiteralPath $Python -PathType Leaf)) {
    Fail "no Python at $Python for the symbol archive (-Python)"
}
$symbolArchive = $Destination.TrimEnd('\', '/') + '-symbols.zip'

# 2. A fresh destination (and symbol archive).
if (Test-Path -LiteralPath $Destination) {
    $existing = @(Get-ChildItem -LiteralPath $Destination -Force)
    if ($existing.Count -gt 0) {
        if (-not $Force) { Fail "$Destination is not empty (use -Force to replace it)" }
        Remove-Item -LiteralPath $Destination -Recurse -Force
    }
}
if (Test-Path -LiteralPath $symbolArchive) {
    if (-not $Force) { Fail "$symbolArchive exists (use -Force to replace it)" }
    Remove-Item -LiteralPath $symbolArchive -Force
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

# 3b. Bundled language servers and debug adapters (clangd, lldb-dap, LuaLS, csharp-ls, netcoredbg +
# PdbGuard), checked before the destination was touched (FND-13).
if ($bundle) {
    $llvmBinDest = Join-Path $Destination "$llvmRel\bin"
    New-Item -ItemType Directory -Force -Path $llvmBinDest | Out-Null
    Copy-Item -LiteralPath $clangdSource -Destination $llvmBinDest -Force
    $llvmLibDest = Join-Path $Destination "$llvmRel\lib"
    New-Item -ItemType Directory -Force -Path $llvmLibDest | Out-Null
    Copy-Item -LiteralPath $clangLibSource -Destination $llvmLibDest -Recurse -Force
    $lldbBinDest = Join-Path $Destination "$lldbRel\bin"
    New-Item -ItemType Directory -Force -Path $lldbBinDest | Out-Null
    foreach ($lldbFile in $lldbFiles) {
        Copy-Item -LiteralPath (Join-Path $lldbSourceDir $lldbFile) -Destination $lldbBinDest -Force
    }
    # LuaLS (ADR 0079) as installed, without the install marker or a log a run left behind. The
    # locator looks for <exe dir>\luals-<version>\bin first, and FrostIDE points LuaLS's own log and
    # meta output under the user profile, so the staged folder stays read-only at run time.
    $lualsDest = Join-Path $Destination $lualsRel
    Copy-Item -LiteralPath $lualsSource -Destination $lualsDest -Recurse -Force
    foreach ($generated in @('.frost-installed', 'log')) {
        $path = Join-Path $lualsDest $generated
        if (Test-Path -LiteralPath $path) { Remove-Item -LiteralPath $path -Recurse -Force }
    }
    $managedDest = Join-Path $Destination 'managed'
    New-Item -ItemType Directory -Force -Path $managedDest | Out-Null
    Copy-Item -Path (Join-Path $ManagedServerDir '*') -Destination $managedDest -Recurse -Force
    $managedDbgDest = Join-Path $Destination 'FrostManagedDebug'
    New-Item -ItemType Directory -Force -Path $managedDbgDest | Out-Null
    Copy-Item -Path (Join-Path $ManagedDebugDir '*') -Destination $managedDbgDest -Recurse -Force
}

# 4. The Qt runtime for frost_app.dll, then the build's platform plugins (offscreen for --selftest).
# --no-svg: icons are baked to raster atlases at build time by tools/icon_baker, the only QtSvg user
# (ADR 0015, "Runtime QtSvg" rejected); frost_app.dll imports no Qt6Svg, and the glacial logo is painted.
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
# LuaLS's MIT licence beside FrostIDE's notices too (it also stays in the LuaLS folder).
if ($bundle) { Copy-Item -LiteralPath (Join-Path $lualsSource 'LICENSE') -Destination (Join-Path $licenses 'luals-LICENSE.txt') }

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
if ($QtSourceArchive) {
    $python = Join-Path $SdkRoot 'py\Scripts\python.exe'
    if (-not (Test-Path -LiteralPath $python -PathType Leaf)) { Fail 'packaging requires the SDK Python' }
    $qtRoot = Split-Path -Parent (Split-Path -Parent $WinDeployQt)
    & $python (Join-Path $PSScriptRoot 'package_notices.py') --repo $repoRoot --stage $Destination `
        --installed $installedMatch.Groups[1].Value --triplet $tripletMatch.Groups[1].Value `
        --qt-root $qtRoot --qt-source $QtSourceArchive
    if ($LASTEXITCODE -ne 0) { Fail 'dependency source/notice packaging failed' }
}
else {
    Write-Host 'stage: local Debug preview; dependency source/notice packaging was not requested'
}
if ($bundle) {
    foreach ($name in @("$llvmRel\bin\clangd.exe", "$lldbRel\bin\lldb-dap.exe", "$lualsRel\bin\lua-language-server.exe",
            'managed\CSharpLanguageServer.dll', 'FrostManagedDebug\adapter\netcoredbg.exe', 'licenses\luals-LICENSE.txt')) {
        if (-not (Test-Path -LiteralPath (Join-Path $Destination $name) -PathType Leaf)) { Fail "the staged folder has no $name" }
    }
}

# 7. The symbol archive: frost.exe and frost_app.dll must each have the PDB their RSDS record names,
# with the same GUID and age, in the build tree.
$previous = $ErrorActionPreference
$ErrorActionPreference = 'Continue'
$out = & $Python -I (Join-Path $PSScriptRoot 'symbolize.py') archive --stage $Destination --pdb-dir $binDir `
    --output $symbolArchive --require frost.exe --require frost_app.dll 2>&1
$code = $LASTEXITCODE
$ErrorActionPreference = $previous
$out | ForEach-Object { Write-Host "stage: symbols: $_" }
if ($code -ne 0 -or -not (Test-Path -LiteralPath $symbolArchive -PathType Leaf)) { Fail "the symbol archive failed ($code)" }
$files = @(Get-ChildItem -LiteralPath $Destination -Recurse -File)
$bytes = ($files | Measure-Object -Property Length -Sum).Sum
Write-Host ("stage: ok {0} ({1} files, {2:N1} MB); symbols {3}" -f $Destination, $files.Count, ($bytes / 1MB), $symbolArchive)
exit 0
