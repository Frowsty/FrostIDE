# SPDX-License-Identifier: Apache-2.0
#
# verify.ps1: FrostIDE's local verification gate (there is no CI; PLAN.md
# section 6.4 and the definition of done in CLAUDE.md).
#
#   powershell -ExecutionPolicy Bypass -File tools\verify.ps1 [-Quick | -Full] [-WindowsOnly]
#              [-BuildDir build\win-clangcl] [-WslMirror ~/frost-src-x] [-Jobs 8] [-TestJobs 8]
#              [-Base main]
#
# Steps:
#   1. The changed-file list: the files that differ from the merge base of HEAD and -Base (default
#      main), untracked files included (tools/verify_support.py changed; ADR 0027 decision 2). It
#      selects what clang-tidy checks here and in WSL, and whether WSL runs its TSan quick leg.
#   2. Windows: configure win-clangcl, build Debug, run ctest in parallel (-j -TestJobs; tests
#      labelled `stress` are skipped in Debug). Tests that must not overlap hold ctest resource
#      locks, RUN_SERIAL or PROCESSORS (frost_add_test, cmake/FrostLayers.cmake).
#   3. -Full adds Release and the ASan configuration (RelWithDebInfo), with every label, also in
#      parallel, and the /MT fuzz smoke: the win-clangcl-fuzz tree (libFuzzer + ASan,
#      RelWithDebInfo) runs each fuzz target for FROST_FUZZ_SECONDS (60) through ctest -L fuzz,
#      serially so each target gets its full CPU time.
#   4. Repository checks: clang-format on tracked C/C++ files, frost-lint, the
#      licence check (Windows build trees) and the feature-matrix check. A check
#      whose tool does not exist yet is reported as "not-run", never silently dropped.
#   5. clang-tidy (pinned LLVM, the checked-in .clang-tidy files) over the Debug compile
#      database: on the changed files' translation units, a changed header through one TU that
#      includes it; on every TU when the list could not be made (the reason is printed) or under
#      -Full, where the fuzz tree's own TUs join. "tidy not-run (debug build failed)" when there
#      is no database to use.
#   6. Linux: tools/verify.sh inside WSL (unless -WindowsOnly), with --full under -Full. It gets
#      the same changed-file list (FROST_CHANGED_FILES; empty when step 1 failed, so it does not
#      recompute one) and -Base (FROST_VERIFY_BASE). The tree checks are not repeated there (same
#      tree); the licence check and tidy are, because they inspect the Linux build trees.
# -Quick is the default and only names that choice explicitly.
#
# The final line is the summary for the commit footer, e.g.
#   "Verified: win debug 63/64 (1 skipped), wsl debug 62/63 (1 skipped), wsl tidy ok (2 TUs), format ok, ..."
# Skipped tests are never counted as passed. Exit status: 0 only if every step
# that ran succeeded (skips and not-run checks do not fail the gate, but are visible).
# Side effects: build trees under build\, the changed-file list build\verify-changed-<pid>.txt
# (deleted at the end), and the frost-tidy compile database in the Debug tree.

[CmdletBinding()]
param(
    [switch]$Quick,
    [switch]$Full,
    [switch]$WindowsOnly,
    # Build directory for the win-clangcl configuration. Separate directories let
    # several work streams verify concurrently without sharing a Ninja tree.
    [string]$BuildDir = '',
    # Ext4 mirror directory inside WSL (verify.sh's FROST_MIRROR). The default is a
    # per-source-tree directory chosen by verify.sh, so worktrees never share one.
    [string]$WslMirror = '',
    # Parallel compile jobs on both sides; kept moderate because commit memory can be scarce.
    [int]$Jobs = 8,
    # Parallel ctest jobs on Windows; 0 means -Jobs.
    [int]$TestJobs = 0,
    # The branch or commit whose merge base with HEAD defines the changed files (resolved as
    # <Base>, then origin/<Base>).
    [string]$Base = 'main'
)

# Splits verify.sh's summary ("debug 3/3 (1 skipped, 1 disabled), tidy ok (2 TUs)", without its
# "wsl: " prefix) into items at each ", " outside parentheses, so an item's own note survives.
# No side effects; tests/tools/test_verify_scripts.py takes it from this script's syntax tree.
function Split-WslSummary([string]$Line) {
    $items = New-Object System.Collections.Generic.List[string]
    $current = New-Object System.Text.StringBuilder
    $depth = 0
    for ($i = 0; $i -lt $Line.Length; $i++) {
        $c = $Line[$i]
        if ($c -eq '(') { $depth++ } elseif ($c -eq ')' -and $depth -gt 0) { $depth-- }
        if ($depth -eq 0 -and $c -eq ',' -and ($i + 1) -lt $Line.Length -and $Line[$i + 1] -eq ' ') {
            $items.Add($current.ToString())
            $null = $current.Clear()
            $i++
            continue
        }
        $null = $current.Append($c)
    }
    if ($current.Length -gt 0) { $items.Add($current.ToString()) }
    return , $items.ToArray()
}

# The WSL (drvfs) path of a Windows drive path: E:\dev\x becomes /mnt/e/dev/x. No side effects;
# tools/frost_perf.py's to_wsl_path is a copy, and both are tested on one table.
function ConvertTo-WslPath([string]$Path) {
    return '/mnt/' + $Path.Substring(0, 1).ToLowerInvariant() + ($Path.Substring(2) -replace '\\', '/')
}

# Parses ctest output into "passed/total" plus "(n skipped, m disabled)". Both are
# listed under "The following tests did not run:". ctest's "out of N" includes
# skipped tests (never counted as passed here) but not disabled ones (gtest's
# DISABLED_ tests), which are reported on their own. No side effects.
function Get-CtestCounts([string[]]$Output) {
    $line = $Output | Where-Object { $_ -match 'tests passed.* out of \d+' } | Select-Object -Last 1
    if (-not $line) { return $null }
    $total = [int]([regex]::Match($line, 'out of (\d+)').Groups[1].Value)
    $failedCount = 0
    $failedMatch = [regex]::Match($line, '(\d+) tests failed')
    if ($failedMatch.Success) { $failedCount = [int]$failedMatch.Groups[1].Value }
    $skipped = @($Output | Where-Object { $_ -match '^\s*\d+ - .+\(Skipped\)\s*$' }).Count
    $disabled = @($Output | Where-Object { $_ -match '^\s*\d+ - .+\(Disabled\)\s*$' }).Count
    $text = "$($total - $failedCount - $skipped)/$total"
    $notes = @()
    if ($skipped -gt 0) { $notes += "$skipped skipped" }
    if ($disabled -gt 0) { $notes += "$disabled disabled" }
    if ($notes.Count -gt 0) { $text += " ($($notes -join ', '))" }
    return $text
}

# 'Continue', not 'Stop': under Windows PowerShell 5.1 a native command's stderr
# line becomes an error record, and 'Stop' would abort the gate on the first
# warning that ctest, cmake or wsl.exe prints. Failures are detected through
# exit codes instead.
$ErrorActionPreference = 'Continue'
if ($Quick -and $Full) {
    Write-Host 'verify.ps1: -Quick and -Full are mutually exclusive'
    exit 2
}
$repo = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'env.ps1')
Set-Location $repo
# Budget: B28. ctest runs -TestJobs tests at once (default: -Jobs); the locks, RUN_SERIAL and
# PROCESSORS of frost_add_test keep the tests that must not overlap apart (ADR 0027 decision 3).
if ($TestJobs -le 0) { $TestJobs = $Jobs }
$python = Join-Path $env:FROST_SDK 'py\Scripts\python.exe'
$verifySupport = Join-Path $PSScriptRoot 'verify_support.py'

$summary = New-Object System.Collections.Generic.List[string]
$failed = $false
# Build state shared with the tidy step: the Debug tree, and which trees built.
$script:debugDir = ''
$script:debugBuilt = $false
$script:fuzzDir = ''
$script:fuzzBuilt = $false

# Runs a native command with stderr merged into stdout, echoing every line;
# returns the lines as strings. $LASTEXITCODE is the command's exit code.
function Invoke-Native([scriptblock]$Command) {
    $lines = @(& $Command 2>&1 | ForEach-Object { "$_" })
    $lines | Out-Host
    return , $lines
}

# Configures, builds and tests one Windows configuration. $CtestArgs are extra
# ctest arguments (e.g. -LE '^stress$'). Records a debug or fuzz tree that built in
# $script:debugDir/$script:debugBuilt and $script:fuzzDir/$script:fuzzBuilt.
function Test-WindowsConfig([string]$Label, [string]$Preset, [string]$Config, [string]$Dir, [string[]]$CtestArgs = @()) {
    $configureArgs = @('--preset', $Preset)
    if ($Dir) { $configureArgs += @('-B', $Dir) } else { $Dir = Join-Path $repo "build\$Preset" }
    if ($Label -eq 'debug') { $script:debugDir = $Dir }
    if ($Label -eq 'fuzz') { $script:fuzzDir = $Dir }
    Write-Host "=== $Label configure"
    $null = Invoke-Native { cmake @configureArgs }
    if ($LASTEXITCODE -ne 0) { $script:summary.Add("win $Label configure-failed"); $script:failed = $true; return }
    Write-Host "=== $Label build"
    $null = Invoke-Native { cmake --build $Dir --config $Config -j $Jobs }
    if ($LASTEXITCODE -ne 0) { $script:summary.Add("win $Label build-failed"); $script:failed = $true; return }
    if ($Label -eq 'debug') { $script:debugBuilt = $true }
    if ($Label -eq 'fuzz') { $script:fuzzBuilt = $true }
    Write-Host "=== $Label test"
    $out = Invoke-Native { ctest --test-dir $Dir -C $Config --output-on-failure @CtestArgs }
    $ok = ($LASTEXITCODE -eq 0)
    $counts = Get-CtestCounts $out
    if (-not $counts) { $counts = 'no-results'; $ok = $false }
    $script:summary.Add("win $Label $counts")
    if (-not $ok) { $script:failed = $true }
}

$changedList = Join-Path $repo "build\verify-changed-$PID.txt"
try {
    # 1. The changed-file list. A failure is not fatal: tidy then checks every TU and WSL runs
    # all four TSan modules, with the reason in the log.
    Write-Host '=== changed files'
    $changedReason = ''
    $out = Invoke-Native { & $python -I $verifySupport changed --repo $repo --base $Base --out $changedList }
    if ($LASTEXITCODE -eq 0 -and (Test-Path -LiteralPath $changedList)) {
        $count = @(Get-Content -LiteralPath $changedList -Encoding UTF8 | Where-Object { $_ }).Count
        Write-Host "verify.ps1: $count changed files against the merge base with $Base"
        $changedOk = $true
    } else {
        $changedReason = ($out | Where-Object { $_ } | Select-Object -Last 1)
        if (-not $changedReason) { $changedReason = "exit $LASTEXITCODE" }
        Write-Host "verify.ps1: no changed-file list: $changedReason"
        $changedOk = $false
    }

    # 2. and 3. The Windows configurations.
    $parallel = @('-j', $TestJobs)
    Test-WindowsConfig 'debug' 'win-clangcl' 'Debug' $BuildDir (@('-LE', '^stress$') + $parallel)
    if ($Full) {
        Test-WindowsConfig 'release' 'win-clangcl' 'Release' $BuildDir $parallel
        $asanDir = ''
        if ($BuildDir) { $asanDir = "$BuildDir-asan" }
        Test-WindowsConfig 'asan' 'win-clangcl-asan' 'RelWithDebInfo' $asanDir $parallel
        $fuzzDir = ''
        if ($BuildDir) { $fuzzDir = "$BuildDir-fuzz" }
        Test-WindowsConfig 'fuzz' 'win-clangcl-fuzz' 'RelWithDebInfo' $fuzzDir @('-L', '^fuzz$')
    }

    # 4. Repository checks.
    $checks = New-Object System.Collections.Generic.List[string]

    # clang-format: every tracked C/C++ file must already be formatted. Fixtures are
    # excluded: they are byte-exact inputs, and clang-format cannot parse the
    # banned-construct probes.
    Write-Host '=== format'
    $clangFormat = Join-Path $env:FROST_LLVM_ROOT 'bin\clang-format.exe'
    $sources = @(git ls-files '*.cpp' '*.hpp' '*.h' '*.c' '*.hpp.in' |
        Where-Object { $_ -notlike 'tests/toolchain_probe/banned/*' -and $_ -notlike 'tests/*/fixtures/*' })
    $unformatted = New-Object System.Collections.Generic.List[string]
    foreach ($source in $sources) {
        $null = & $clangFormat --dry-run --Werror $source 2>&1
        if ($LASTEXITCODE -ne 0) { $unformatted.Add($source) }
    }
    # An empty list (git failing) must fail, not pass vacuously.
    if ($sources.Count -eq 0) { $checks.Add('format FAILED (no files listed)'); $failed = $true }
    elseif ($unformatted.Count -eq 0) { $checks.Add('format ok') }
    else {
        $unformatted | ForEach-Object { Write-Host "needs clang-format: $_" }
        $checks.Add("format FAILED ($($unformatted.Count) files)"); $failed = $true
    }

    # Python checks, run when the tool exists. featurecheck.py defaults to its `check` command.
    foreach ($tool in @('frost_lint.py', 'licensecheck.py', 'featurecheck.py')) {
        $name = $tool -replace '\.py$', ''
        $path = Join-Path $PSScriptRoot $tool
        if (-not (Test-Path $path)) { $checks.Add("$name not-run"); continue }
        Write-Host "=== $name"
        $null = Invoke-Native { & $python -I $path --repo $repo }
        if ($LASTEXITCODE -eq 0) { $checks.Add("$name ok") } else { $checks.Add("$name FAILED"); $failed = $true }
    }

    # 5. clang-tidy over the Debug compile database (PLAN.md 6.4 step 2; ADR 0027 decision 1).
    # Budget: B28. -Quick checks only the changed files' TUs; every TU runs only without a list.
    Write-Host '=== tidy'
    if (-not $script:debugBuilt) {
        $checks.Add('tidy not-run (debug build failed)')
    } else {
        $ninja = (Get-Command ninja.exe -CommandType Application | Select-Object -First 1).Source
        $tidyArgs = @('--repo', $repo, '--build-dir', $script:debugDir, '--config', 'Debug',
            '--clang-tidy', (Join-Path $env:FROST_LLVM_ROOT 'bin\clang-tidy.exe'), '--ninja', $ninja, '--jobs', $Jobs)
        if ($Full) {
            $tidyArgs += '--all'
            if ($script:fuzzBuilt) { $tidyArgs += @('--extra-build-dir', $script:fuzzDir, '--extra-config', 'RelWithDebInfo') }
        } elseif ($changedOk) {
            $tidyArgs += @('--changed', $changedList)
        } else {
            $tidyArgs += @('--all', '--reason', "no changed-file list: $changedReason")
        }
        $out = Invoke-Native { & $python -I $verifySupport tidy @tidyArgs }
        $tidyCode = $LASTEXITCODE
        $tidyLine = $out | Where-Object { $_ -match '^tidy ' } | Select-Object -Last 1
        if (-not $tidyLine) { $tidyLine = "tidy FAILED (exit $tidyCode without a summary)" }
        $checks.Add($tidyLine)
        if ($tidyCode -ne 0) { $failed = $true }
    }

    # 6. and 7. The Linux leg.
    if (-not $WindowsOnly) {
        $wslScript = (ConvertTo-WslPath $repo) + '/tools/verify.sh'
        # --exec runs env(1) directly, without a shell re-parsing the arguments, so the
        # mirror path needs no quoting. verify.sh expands a leading "~/" itself. An empty
        # FROST_CHANGED_FILES tells verify.sh that there is no list and not to make one.
        $wslArgs = @('-d', 'Ubuntu-24.04', '--exec', 'env', "FROST_JOBS=$Jobs", "FROST_VERIFY_BASE=$Base")
        if ($changedOk) { $wslArgs += "FROST_CHANGED_FILES=$(ConvertTo-WslPath $changedList)" }
        else { $wslArgs += 'FROST_CHANGED_FILES=' }
        if ($WslMirror) { $wslArgs += "FROST_MIRROR=$WslMirror" }
        $wslArgs += @('bash', $wslScript, '--no-checks')
        if ($Full) { $wslArgs += '--full' }
        Write-Host "=== wsl $wslScript"
        $wslOut = Invoke-Native { wsl.exe @wslArgs }
        if ($LASTEXITCODE -ne 0) { $failed = $true }
        $wslLine = $wslOut | Where-Object { $_ -match '^wsl: ' } | Select-Object -Last 1
        # "wsl: debug 63/63 (1 skipped, 1 disabled), asan 64/64" becomes
        # "wsl debug 63/63 (1 skipped, 1 disabled), wsl asan 64/64".
        if ($wslLine) {
            foreach ($item in (Split-WslSummary ($wslLine -replace '^wsl: ', ''))) { $summary.Add("wsl $item") }
        } else { $summary.Add('wsl no-results'); $failed = $true }
    }
} finally {
    # 8. The list belongs to this run only.
    Remove-Item -LiteralPath $changedList -Force -ErrorAction SilentlyContinue
}

$line = 'Verified: ' + (($summary + $checks) -join ', ')
if ($failed) { $line = "FAILED: $line" }
Write-Host $line
if ($failed) { exit 1 }
exit 0
