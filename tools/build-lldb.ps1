# SPDX-License-Identifier: Apache-2.0
#
# build-lldb.ps1: builds the pinned lldb-dap for Windows (with lldb.exe and liblldb.dll, which it
# needs) from the pinned LLVM sources and installs it into <SdkRoot>\lldb-<version>.
#
# The official LLVM 23.1.4 Windows package has no lldb or lldb-dap (spike S5), so U1's debugger
# engine is built locally on REF-W1 (PLAN.md section 2, Debuggers; section 6.3, the SDK leg;
# ADR 0029 decision 12; ADR 0049). This is a local recipe, never a CI job (D24).
#
# Steps, each logged to <SdkRoot>\_staging\lldb-<version>\<step>.out/.err:
#   1. The source tarball llvm-project-<version>.src.tar.xz from the official GitHub release into
#      <SdkRoot>\_downloads (kept there), checked against the SHA-256 in sdk/sdk.lock.json
#      (sources.llvm). With -VerifyAttestation, also its GitHub artifact attestation (gh.exe from
#      the SDK, the release-sources.yml workflow at the release tag; needs the network).
#   2. Only the directories the build reads (cmake, llvm, clang, lldb, third-party and libc, whose
#      shared headers LLVM's Support library includes), without their test and unittest trees and
#      the three symbolic links in llvm/utils/mlgo-utils, unpacked into
#      <SdkRoot>\_staging\lldb-<version>\src.
#   3. CMake + Ninja, Release, with the pinned clang-cl, lld-link, llvm-lib, llvm-rc and llvm-mt
#      against the pinned MSVC STL and Windows SDK, passed with the same -vctoolsdir, -winsdkdir and
#      -winsdkversion options as cmake/toolchains/windows-clangcl.cmake (that file itself is not
#      used: it sets CMAKE_SYSTEM_NAME, which makes LLVM treat the build as a cross build and
#      configure a second, toolchain-less NATIVE tree for its tablegens): clang and lldb only,
#      the X86 target, the static CRT
#      (/MT), so the runtime set needs no VC runtime DLLs; no Python, Lua, libedit, curses, LZMA,
#      libxml2, zlib, zstd, curl, telemetry or MCP protocol servers; no DIA SDK, so LLDB reads
#      PDBs with its native reader; no assembly files (BLAKE3's would need ml64.exe); no tests,
#      docs, examples or benchmarks.
#   4. `ninja distribution` (lldb, liblldb, lldb-dap and lldb-argdumper and what they depend on),
#      with -Jobs compile jobs (default 6) and -LinkJobs concurrent links, the whole tree at
#      BelowNormal priority, because the user works on this machine while it builds.
#   5. `install-distribution` into a staging prefix; the import library is dropped (nothing links
#      liblldb), and the rest moves to <SdkRoot>\lldb-<version> with a .frost-installed marker.
#   6. Every installed file's SHA-256 is compared with windows.lldb.files in the lock file; a file
#      that differs, is missing or is not pinned fails the run (the build tree is kept then).
#      With -PrintPin the hashes are printed as the lock file's JSON instead of being compared.
#   7. The source and build trees are deleted unless -KeepBuildTree is given.
# A full build takes about an hour or more at -Jobs 6 and needs roughly 10 GB under _staging.
# -VerifyOnly runs step 6 alone on an existing install (tools/bootstrap.ps1 uses it).
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File tools\build-lldb.ps1 [-SdkRoot E:\sdk] [-Jobs 6]
#       [-LinkJobs 2] [-KeepBuildTree] [-VerifyAttestation] [-PrintPin] [-VerifyOnly]

[CmdletBinding()]
param(
    # Root of the SDK: the download cache, the staging area and the install all live under it.
    [string]$SdkRoot = 'E:\sdk',
    # Ninja's compile parallelism. Several agents share this machine, so keep it at 6 or below.
    [ValidateRange(1, 64)][int]$Jobs = 6,
    # How many links run at once (LLVM_PARALLEL_LINK_JOBS); linking liblldb.dll takes a few GB.
    [ValidateRange(1, 16)][int]$LinkJobs = 2,
    # Keep the unpacked sources and the build tree after a successful install.
    [switch]$KeepBuildTree,
    # Also check the tarball's GitHub artifact attestation with the SDK's gh.exe (network).
    [switch]$VerifyAttestation,
    # Print the installed files' hashes as the lock file's "files" object instead of comparing.
    [switch]$PrintPin,
    # Only compare the existing install with the pin (no download, no build).
    [switch]$VerifyOnly
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 3.0

$RepoRoot = Split-Path -Parent $PSScriptRoot
$Lock = Get-Content -Raw -Path (Join-Path $RepoRoot 'sdk\sdk.lock.json') | ConvertFrom-Json
$Source = $Lock.sources.llvm
$Pin = $Lock.windows.lldb
$Version = $Pin.version
if ($Source.version -ne $Version) { throw "sources.llvm is $($Source.version) but windows.lldb is $Version" }

$Downloads = Join-Path $SdkRoot '_downloads'
$Work = Join-Path $SdkRoot "_staging\lldb-$Version"
$SrcDir = Join-Path $Work 'src'
$BuildDir = Join-Path $Work 'build'
$Prefix = Join-Path $Work 'install'
$Dest = Join-Path $SdkRoot "lldb-$Version"
$LlvmBin = Join-Path $SdkRoot "llvm-$($Lock.windows.llvm.version)\bin"
$CMake = Join-Path $SdkRoot "cmake-$($Lock.windows.cmake.version)\bin\cmake.exe"
$Ninja = Join-Path $SdkRoot "ninja-$($Lock.windows.ninja.version)\ninja.exe"
$Python = Join-Path $SdkRoot 'py\Scripts\python.exe'
# The LLVM_DISTRIBUTION_COMPONENTS: lldb-dap and what it needs at run time.
$Components = @('lldb', 'liblldb', 'lldb-dap', 'lldb-argdumper')
# Free space the build needs under _staging (sources about 1.5 GB, the Release tree about 6 GB).
$MinFreeGB = 20

foreach ($tool in @($CMake, $Ninja, $Python, (Join-Path $LlvmBin 'clang-cl.exe'))) {
    if (-not $VerifyOnly -and -not (Test-Path $tool)) { throw "$tool not found; run tools\bootstrap.ps1 first" }
}

# Prints a timestamped progress line.
function Write-Step([string]$Text) {
    Write-Host ("[lldb] {0:HH:mm:ss} {1}" -f (Get-Date), $Text)
}

# Quotes one argument for a Windows command line by the CRT's parsing rules.
function ConvertTo-Argument([string]$Value) {
    if ($Value -ne '' -and $Value -notmatch '[\s"]') { return $Value }
    $escaped = $Value -replace '(\\*)"', '$1$1\"'
    $escaped = $escaped -replace '(\\+)$', '$1$1'
    return '"' + $escaped + '"'
}

# Runs $Exe with $Arguments in $Directory, stdout and stderr to <Log>.out and <Log>.err, and
# throws with the log's tail if it exits non-zero. Start-Process keeps the child's own bytes
# (PowerShell 5.1 redirection would re-encode them as UTF-16).
function Invoke-Logged([string]$Exe, [string[]]$Arguments, [string]$Log, [string]$Directory = $Work) {
    $line = ($Arguments | ForEach-Object { ConvertTo-Argument $_ }) -join ' '
    $process = Start-Process -FilePath $Exe -ArgumentList $line -WorkingDirectory $Directory -NoNewWindow `
        -PassThru -RedirectStandardOutput "$Log.out" -RedirectStandardError "$Log.err"
    $null = $process.Handle # Caches the handle so ExitCode is readable after the wait.
    $process.WaitForExit()
    if ($process.ExitCode -ne 0) {
        $tail = @(Get-Content -Tail 30 "$Log.out") + @(Get-Content -Tail 30 "$Log.err") -join "`n"
        throw "$(Split-Path -Leaf $Exe) failed with exit code $($process.ExitCode) (logs: $Log.out/.err)`n$tail"
    }
}

# Downloads $Url to $Path through a .part file unless $Path exists (curl.exe, as bootstrap.ps1).
function Get-Download([string]$Url, [string]$Path) {
    if (Test-Path $Path) { return }
    & "$env:SystemRoot\System32\curl.exe" --fail --location --silent --show-error --retry 3 -o "$Path.part" $Url
    if ($LASTEXITCODE -ne 0) { throw "download failed ($LASTEXITCODE): $Url" }
    Move-Item -Force "$Path.part" $Path
}

# The installed files as a sorted map of '/'-separated relative path to lowercase SHA-256,
# without the .frost-installed marker.
function Get-TreeHashes([string]$Root) {
    $map = [ordered]@{}
    $files = Get-ChildItem -Recurse -File -Force $Root | Where-Object { $_.Name -ne '.frost-installed' }
    $relative = $files | ForEach-Object { $_.FullName.Substring($Root.Length + 1).Replace('\', '/') } | Sort-Object
    foreach ($path in $relative) {
        $map[$path] = (Get-FileHash -Algorithm SHA256 -Path (Join-Path $Root $path)).Hash.ToLowerInvariant()
    }
    return $map
}

# The pinned MSVC toolset directory (from any Visual Studio install that has it, through vswhere) and
# the Windows SDK root (the registry's KitsRoot10), found as cmake/toolchains/windows-clangcl.cmake
# finds them, with '/' separators.
function Get-MsvcPins {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    $tools = $null
    foreach ($vs in @(& $vswhere -all -products * -property installationPath)) {
        $candidate = Join-Path $vs "VC\Tools\MSVC\$($Lock.msvc.vcToolsVersion)"
        if (Test-Path (Join-Path $candidate 'include')) { $tools = $candidate; break }
    }
    if (-not $tools) { throw "MSVC toolset $($Lock.msvc.vcToolsVersion) not found in any Visual Studio install" }
    $key = 'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows Kits\Installed Roots'
    if (-not (Test-Path $key)) { $key = 'HKLM:\SOFTWARE\Microsoft\Windows Kits\Installed Roots' }
    $sdk = (Get-ItemProperty -Path $key).KitsRoot10.TrimEnd('\')
    if (-not (Test-Path (Join-Path $sdk "Include\$($Lock.msvc.windowsSdkVersion)\ucrt"))) {
        throw "Windows SDK $($Lock.msvc.windowsSdkVersion) not found under '$sdk'"
    }
    return @{ Tools = $tools.Replace('\', '/'); Sdk = $sdk.Replace('\', '/'); SdkVersion = $Lock.msvc.windowsSdkVersion }
}

# Step 6: compares every file under $Dest with windows.lldb.files and throws on a file that
# differs, is missing or is not pinned; with -PrintPin prints the hashes as JSON instead.
function Assert-Pin {
    $hashes = Get-TreeHashes $Dest
    if ($PrintPin) {
        $entries = @($hashes.Keys | ForEach-Object { "  `"$_`": `"$($hashes[$_])`"" })
        Write-Host ("`"files`": {`n" + ($entries -join ",`n") + "`n}")
        return
    }
    $pinned = @{}
    if ($Pin.PSObject.Properties.Name -contains 'files') {
        foreach ($property in $Pin.files.PSObject.Properties) { $pinned[$property.Name] = $property.Value }
    }
    $wrong = @()
    foreach ($path in $hashes.Keys) {
        if (-not $pinned.ContainsKey($path)) { $wrong += "not pinned: $path $($hashes[$path])" }
        elseif ($pinned[$path] -ne $hashes[$path]) { $wrong += "differs: $path $($hashes[$path])" }
    }
    foreach ($path in $pinned.Keys) { if (-not $hashes.Contains($path)) { $wrong += "missing: $path" } }
    if ($wrong.Count -gt 0) {
        throw ("$Dest differs from windows.lldb.files in sdk/sdk.lock.json (a build keeps its tree in $Work):`n  " +
               ($wrong -join "`n  ") + "`nA new pin needs an ADR (PLAN.md section 2.3); -PrintPin prints it.")
    }
    Write-Step "all $($hashes.Count) files in $Dest match the pin"
}

if ($VerifyOnly) {
    if (-not (Test-Path (Join-Path $Dest '.frost-installed'))) { throw "lldb $Version is not installed in $Dest" }
    Assert-Pin
    return
}

# Every child (cmake, ninja, clang-cl, lld-link) inherits this priority class.
(Get-Process -Id $PID).PriorityClass = 'BelowNormal'

$drive = Get-PSDrive -Name ((Split-Path -Qualifier $SdkRoot).TrimEnd(':'))
if ($drive.Free / 1GB -lt $MinFreeGB) { throw "$SdkRoot has $([math]::Round($drive.Free / 1GB)) GB free; need $MinFreeGB GB" }
New-Item -ItemType Directory -Force -Path $Downloads, $Work | Out-Null
$started = Get-Date

# 1. Sources.
$tarball = Join-Path $Downloads $Source.file
Write-Step "sources: $($Source.file)"
Get-Download $Source.url $tarball
$actual = (Get-FileHash -Algorithm SHA256 -Path $tarball).Hash.ToLowerInvariant()
if ($actual -ne $Source.sha256) {
    Remove-Item -Force $tarball
    throw "SHA-256 mismatch for $tarball (deleted)`n  expected $($Source.sha256)`n  actual   $actual"
}
if ($VerifyAttestation) {
    $bundle = Join-Path $Downloads $Source.attestation.file
    Get-Download $Source.attestation.url $bundle
    $gh = Join-Path $SdkRoot "gh-$($Lock.windows.gh.version)\bin\gh.exe"
    $json = & $gh attestation verify $tarball --bundle $bundle --repo $Source.attestation.repository --format json
    if ($LASTEXITCODE -ne 0) { throw "attestation check failed for $tarball" }
    $signer = (($json | ConvertFrom-Json)[0]).verificationResult.signature.certificate.buildSignerURI
    if ($signer -ne $Source.attestation.signer) { throw "attestation signed by '$signer', expected '$($Source.attestation.signer)'" }
    Write-Step "attestation ok ($signer)"
}

# 2. Unpack what the build reads. The tarball's single top directory becomes src.
$top = "llvm-project-$Version.src"
foreach ($stale in @($SrcDir, (Join-Path $Work $top))) {
    if (Test-Path $stale) { Remove-Item -Recurse -Force $stale }
}
Write-Step 'unpacking cmake, llvm, clang, lldb, third-party and libc (no tests)'
$members = @('cmake', 'llvm', 'clang', 'lldb', 'third-party', 'libc') | ForEach-Object { "$top/$_" }
$excludes = @('llvm', 'clang', 'lldb', 'libc') | ForEach-Object { '--exclude', "$top/$_/test", '--exclude', "$top/$_/unittests" }
# The only symbolic links in these trees, which tar.exe cannot create without the symlink
# privilege; the build never reads them (they point at the same scripts under mlgo/corpus).
$excludes += @('combine_training_corpus.py', 'extract_ir.py', 'make_corpus.py') |
    ForEach-Object { '--exclude', "$top/llvm/utils/mlgo-utils/$_" }
Invoke-Logged "$env:SystemRoot\System32\tar.exe" (@('-xf', $tarball, '-C', $Work) + $excludes + $members) (Join-Path $Work 'unpack')
Move-Item (Join-Path $Work $top) $SrcDir

# 3. Configure.
if (Test-Path $BuildDir) { Remove-Item -Recurse -Force $BuildDir }
Write-Step "configuring (log $Work\configure.out)"
$msvc = Get-MsvcPins
$llvm = $LlvmBin.Replace('\', '/')
# clang-cl takes the pins joined to their values; lld-link spells them /name:value (as the toolchain file).
$clPin = "`"-vctoolsdir$($msvc.Tools)`" `"-winsdkdir$($msvc.Sdk)`" -winsdkversion$($msvc.SdkVersion)"
$linkPin = "/vctoolsdir:`"$($msvc.Tools)`" /winsdkdir:`"$($msvc.Sdk)`" /winsdkversion:$($msvc.SdkVersion)"
$sdkInclude = "$($msvc.Sdk)/Include/$($msvc.SdkVersion)"
$rcIncludes = "/I `"$sdkInclude/um`" /I `"$sdkInclude/shared`""
$configure = @(
    '-G', 'Ninja', '-S', (Join-Path $SrcDir 'llvm'), '-B', $BuildDir,
    "-DCMAKE_MAKE_PROGRAM=$Ninja",
    "-DCMAKE_C_COMPILER=$llvm/clang-cl.exe", "-DCMAKE_CXX_COMPILER=$llvm/clang-cl.exe",
    "-DCMAKE_LINKER=$llvm/lld-link.exe", "-DCMAKE_AR=$llvm/llvm-lib.exe",
    "-DCMAKE_RC_COMPILER=$llvm/llvm-rc.exe", "-DCMAKE_MT=$llvm/llvm-mt.exe", '-DCMAKE_LINKER_TYPE=LLD',
    "-DCMAKE_C_FLAGS=$clPin", "-DCMAKE_CXX_FLAGS=$clPin", "-DCMAKE_RC_FLAGS=$rcIncludes",
    "-DCMAKE_EXE_LINKER_FLAGS=$linkPin", "-DCMAKE_SHARED_LINKER_FLAGS=$linkPin",
    "-DCMAKE_MODULE_LINKER_FLAGS=$linkPin",
    '-DCMAKE_BUILD_TYPE=Release',
    "-DCMAKE_INSTALL_PREFIX=$Prefix",
    '-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded',
    "-DPython3_EXECUTABLE=$Python",
    '-DLLVM_ENABLE_PROJECTS=clang;lldb',
    '-DLLVM_TARGETS_TO_BUILD=X86',
    "-DLLVM_DISTRIBUTION_COMPONENTS=$($Components -join ';')",
    "-DLLVM_PARALLEL_LINK_JOBS=$LinkJobs",
    '-DLLVM_ENABLE_ASSERTIONS=OFF',
    '-DLLVM_ENABLE_DIA_SDK=OFF',
    # BLAKE3's MASM files would need ml64.exe, which is not on PATH without vcvarsall; its C code is used.
    '-DLLVM_DISABLE_ASSEMBLY_FILES=ON',
    '-DLLVM_ENABLE_LIBXML2=OFF', '-DLLVM_ENABLE_ZLIB=OFF', '-DLLVM_ENABLE_ZSTD=OFF',
    '-DLLVM_ENABLE_CURL=OFF', '-DLLVM_ENABLE_HTTPLIB=OFF', '-DLLVM_ENABLE_LIBEDIT=OFF',
    '-DLLVM_ENABLE_TELEMETRY=OFF', '-DLLVM_ENABLE_BINDINGS=OFF',
    '-DLLVM_INCLUDE_TESTS=OFF', '-DLLVM_INCLUDE_BENCHMARKS=OFF', '-DLLVM_INCLUDE_EXAMPLES=OFF',
    '-DLLVM_INCLUDE_DOCS=OFF', '-DCLANG_INCLUDE_TESTS=OFF', '-DCLANG_INCLUDE_DOCS=OFF',
    '-DCLANG_ENABLE_STATIC_ANALYZER=OFF', '-DCLANG_PLUGIN_SUPPORT=OFF',
    '-DLLDB_INCLUDE_TESTS=OFF', '-DLLDB_ENABLE_PYTHON=OFF', '-DLLDB_ENABLE_LUA=OFF',
    '-DLLDB_ENABLE_SWIG=OFF', '-DLLDB_ENABLE_LIBEDIT=OFF', '-DLLDB_ENABLE_CURSES=OFF',
    '-DLLDB_ENABLE_LZMA=OFF', '-DLLDB_ENABLE_LIBXML2=OFF', '-DLLDB_ENABLE_TREESITTER=OFF',
    '-DLLDB_ENABLE_PROTOCOL_SERVERS=OFF'
)
Invoke-Logged $CMake $configure (Join-Path $Work 'configure')

# 4. Build.
Write-Step "building the distribution with -j$Jobs (log $Work\build.out)"
Invoke-Logged $Ninja @('-C', $BuildDir, "-j$Jobs", 'distribution') (Join-Path $Work 'build')

# 5. Install into the staging prefix, then into the SDK.
Write-Step 'installing'
if (Test-Path $Prefix) { Remove-Item -Recurse -Force $Prefix }
Invoke-Logged $Ninja @('-C', $BuildDir, 'install-distribution') (Join-Path $Work 'install')
Get-ChildItem -Recurse -File (Join-Path $Prefix 'lib') -Filter '*.lib' -ErrorAction SilentlyContinue | Remove-Item -Force
Get-ChildItem -Recurse -Directory $Prefix | Sort-Object { $_.FullName.Length } -Descending |
    Where-Object { @(Get-ChildItem -Force $_.FullName).Count -eq 0 } | Remove-Item -Force
foreach ($required in @('bin\lldb-dap.exe', 'bin\lldb.exe', 'bin\liblldb.dll')) {
    if (-not (Test-Path (Join-Path $Prefix $required))) { throw "the install has no $required" }
}
if (Test-Path $Dest) { Remove-Item -Recurse -Force $Dest }
Move-Item $Prefix $Dest
Set-Content -Path (Join-Path $Dest '.frost-installed') -Encoding ascii `
    -Value "$($Source.url) sha256:$($Source.sha256) recipe:tools/build-lldb.ps1"
Write-Step "installed to $Dest"

# 6. Compare with the pin.
Assert-Pin

# 7. Clean up.
if (-not $KeepBuildTree) {
    Write-Step 'deleting the source and build trees'
    Remove-Item -Recurse -Force $SrcDir, $BuildDir
}
Write-Step ("done in {0:N0} min" -f ((Get-Date) - $started).TotalMinutes)
