# SPDX-License-Identifier: Apache-2.0
#
# DBG-0 spike runner (docs/adr/0081-windows-debugger-backend.md): builds tools\dbg0\dbg0_spike.cpp
# with the pinned clang-cl, then measures DbgEng (the System32 copy) and the pinned lldb-dap on the
# pinned FrostEngine clone's large-PDB programs and prints one line per run.
#
#   powershell -ExecutionPolicy Bypass -File tools\dbg0\run-dbg0.ps1 [-Runs 2] [-OutDir <dir>]
#              [-ProgramDir <corpus bin>] [-Programs FrostTests,FrostEditor,FrostRuntime] [-Fixture <exe>]
#
# -ProgramDir is the clone's build\windows-debug\bin (FrostTests.exe, FrostEditor.exe, FrostRuntime.exe when
# it is built; a program that is missing is reported, never built here: the engine is ~49 GB).
# -Programs limits the run to some of the three (for example FrostRuntime once it is built).
# -Fixture is a small cl.exe Debug build (msvc_debug_test.cpp's fixture) for DbgEng's natvis check:
# `.nvload` of the installed Visual Studio stl.natvis and `dx` of a std::vector, std::map and
# std::string in the caller's frame. Every run ends its debuggee at the breakpoint (no window opens).
# Runs are sequential so the two backends never compete for the disk cache; the first run of a
# program is the cold one. Results go to <OutDir>\dbg0-results.txt as well.

param(
    [int]$Runs = 2,
    [string]$OutDir = 'E:\FrostBuilds\r3-scratch\dbg0',
    [string]$ProgramDir = 'E:\FrostBuilds\corpora\frostengine-8237e1ecfeb9\build\windows-debug\bin',
    [string[]]$Programs = @('FrostTests', 'FrostEditor', 'FrostRuntime'),
    [string]$Fixture = ''
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
. (Join-Path $repo 'tools\env.ps1') | Out-Null
New-Item -ItemType Directory -Force $OutDir | Out-Null

# The Visual Studio instance (vswhere by its known-folder path) for the Windows SDK headers and libs.
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$install = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $install) { throw 'no Visual Studio instance with the C++ tools' }
$vcvars = Join-Path $install 'VC\Auxiliary\Build\vcvarsall.bat'
$clangCl = Join-Path $env:FROST_LLVM_ROOT 'bin\clang-cl.exe'
$spike = Join-Path $OutDir 'dbg0_spike.exe'
$source = Join-Path $PSScriptRoot 'dbg0_spike.cpp'
$build = "call `"$vcvars`" x64 >nul && `"$clangCl`" /nologo /std:c++latest /EHsc /O2 /W4 `"$source`" /Fe`"$spike`" /Fo`"$OutDir\\`" version.lib psapi.lib"
& "$env:SystemRoot\System32\cmd.exe" /d /c $build
if ($LASTEXITCODE -ne 0) { throw "dbg0_spike did not build ($LASTEXITCODE)" }

$lldbDap = Join-Path $env:FROST_SDK 'lldb-23.1.4\bin\lldb-dap.exe'
$results = Join-Path $OutDir 'dbg0-results.txt'
"DBG-0 run $(Get-Date -Format s) on $env:COMPUTERNAME" | Set-Content -Encoding utf8 $results

function Invoke-Spike([string[]]$Arguments, [string]$Label) {
    $log = Join-Path $OutDir ("{0}.log" -f $Label)
    $process = Start-Process -FilePath $spike -ArgumentList $Arguments -NoNewWindow -PassThru `
        -RedirectStandardOutput $log -RedirectStandardError "$log.err"
    $null = $process.Handle # Held from the start, so ExitCode is readable after the exit.
    if (-not $process.WaitForExit(3600000)) {
        # Only this run's tree: the spike, its lldb-dap and the debuggee.
        & "$env:SystemRoot\System32\taskkill.exe" /PID $process.Id /T /F | Out-Null
        throw "$Label did not finish in an hour"
    }
    $lines = Get-Content $log | Where-Object { $_ -like 'DBG0 *' }
    $values = @{}
    foreach ($line in $lines) { if ($line -match '^DBG0 (\w+)=(.*)$') { $values[$Matches[1]] = $Matches[2] } }
    $row = '{0,-28} exit={1} ttfb={2} ms symbols={3} ms peakWS={4} MB peakPrivate={5} MB stop={6}' -f $Label,
        $process.ExitCode, $values.ttfbMs, $values.symbolsMs, $values.peakWorkingSetMB, $values.peakPrivateMB,
        $values.stoppedOnBreakpoint
    Write-Host $row
    Add-Content -Encoding utf8 $results $row
    if ($process.ExitCode -ne 0) { Get-Content "$log.err" | ForEach-Object { Add-Content -Encoding utf8 $results "  $_" } }
}

foreach ($name in $Programs) {
    $program = Join-Path $ProgramDir "$name.exe"
    if (-not (Test-Path $program)) {
        $row = "{0,-28} not built in {1} (not built by this spike)" -f $name, $ProgramDir
        Write-Host $row; Add-Content -Encoding utf8 $results $row; continue
    }
    $pdb = Get-Item (Join-Path $ProgramDir "$name.pdb") -ErrorAction SilentlyContinue
    if ($pdb) { Add-Content -Encoding utf8 $results ("{0}.pdb {1:N0} MB" -f $name, ($pdb.Length / 1MB)) }
    for ($run = 1; $run -le $Runs; ++$run) {
        Invoke-Spike @('dbgeng', $program, 'main') "$name-dbgeng-$run"
        Invoke-Spike @('lldb', $lldbDap, $program, 'main') "$name-lldb-$run"
    }
}

if ($Fixture) {
    $natvis = Join-Path $install 'Common7\Packages\Debugger\Visualizers\stl.natvis'
    $log = Join-Path $OutDir 'fixture-dbgeng-natvis.log'
    & $spike dbgeng $Fixture callee '.frame 1' 'dx numbers' 'dx table' 'dx text' ".nvload $natvis" 'dx numbers' 'dx table' 'dx text' > $log
    Add-Content -Encoding utf8 $results "natvis check ($Fixture, $natvis): see $log"
}
# Each spike run ends its own debuggee and adapter (dbg0_spike.cpp); nothing is killed by name here,
# since other debugger sessions on the machine are not ours.
Write-Host "results: $results"
