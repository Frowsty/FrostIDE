# SPDX-License-Identifier: Apache-2.0
#
# verify.ps1: FrostIDE's local verification gate (there is no CI; PLAN.md
# section 6.4 and the definition of done in CLAUDE.md).
#
#   powershell -ExecutionPolicy Bypass -File tools\verify.ps1 [-Quick | -Full] [-WindowsOnly]
#              [-BuildDir build\win-clangcl] [-WslMirror ~/frost-src-x] [-Jobs 8] [-TestJobs 8]
#              [-Tidy] [-TsanQuick] [-Corpus] [-Base main]
#
# Steps:
#   1. When tidy or targeted TSan Quick is enabled (and without -Full): the files that differ
#      from the merge base of HEAD and -Base (default main), untracked files included
#      (tools/verify_support.py changed; ADR 0027 decision 2). It selects what clang-tidy checks
#      here and in WSL, and which modules WSL's TSan quick leg runs.
#   2. Windows: configure win-clangcl, build Debug, run ctest in parallel (-j -TestJobs; tests
#      labelled `stress` are skipped in Debug, tests labelled `corpus` everywhere but step 3b).
#      Tests that must not overlap hold ctest resource locks, RUN_SERIAL or PROCESSORS
#      (frost_add_test, cmake/FrostLayers.cmake: integration suites share the frost_desktop lock,
#      perf suites run serially). ctest runs in
#      non-interactive mode, so no test can raise an error box (see Test-WindowsConfig). After
#      each test leg, `frost_lint.py skips` lists every skipped test with its reason from ctest's
#      log; a skip whose reason names a pinned tool fails the gate (FND-6: a missing pinned tool
#      is a failure, never a skip). Each test leg's failed tests are printed, kept in
#      <build>\Testing\Temporary\FailedTests-<label>.log and listed again above the summary line
#      (the corpus leg reruns ctest in the Debug tree, which rewrites ctest's LastTestsFailed.log).
#   3. -Full adds Release and the ASan configuration (RelWithDebInfo), every label but corpus, also in
#      parallel, and the /MT fuzz smoke: the win-clangcl-fuzz tree (libFuzzer + ASan,
#      RelWithDebInfo) runs each fuzz target for FROST_FUZZ_SECONDS (60) through ctest -L fuzz,
#      serially so each target gets its full CPU time.
#   3b. -Corpus adds the corpus tier (ADR 0077 decision 7, ADR 0078): tools\corpora.py verify must
#      find every pinned corpus of tests\corpus\corpora.lock.json (a missing or changed corpus
#      fails the tier, it is never skipped), `corpora.py engine --configure` must reuse or configure
#      the pinned engine's preset (FND-6b; it fails the tier when it cannot), then the Debug tree's
#      tests labelled `corpus` (the corpus tests and the j1 journey, tests/journeys) run alone
#      and serially with a 3600 s per-test limit (they configure and index a whole engine). No
#      corpus test, or a skipped one, fails the tier ("win corpus no-tests", "... (1 skipped)").
#      Without -Corpus no corpus test runs, in -Quick or -Full.
#   0. The tree hash of the working tree (tools/hooks/verify_stamp.py tree; FND-2).
#   4. Repository checks: clang-format on tracked C/C++ files, frost-lint, the
#      licence check (Windows build trees), the feature-matrix check and the source-to-test
#      line ratchet against the merge base with -Base (frost_lint.py ratchet; FND-5). A check
#      whose tool does not exist yet is reported as "not-run", never silently dropped.
#   5. Tidy (enabled by default): pinned clang-tidy and the checked-in .clang-tidy over the Debug
#      compile database: on the changed files' translation units, a changed header through one
#      TU that includes it; on every TU when the list could not be made (the reason is printed)
#      or under -Full, where the fuzz tree's own TUs join. "tidy not-run (debug build failed)"
#      when there is no database to use; explicit -Tidy:$false reports "tidy not-run (disabled by request)".
#   6. Linux: tools/verify.sh inside WSL (unless -WindowsOnly), with --full under -Full, --tidy
#      or --no-tidy for the effective -Tidy value, and --tsan-quick/--no-tsan-quick for -TsanQuick.
#      It gets the same changed-file list
#      (FROST_CHANGED_FILES; empty when step 1 failed, so it does not recompute one) and -Base
#      (FROST_VERIFY_BASE), and --corpus under -Corpus. The tree checks are not repeated there
#      (same tree); the licence check and tidy are, because they inspect the Linux build trees.
# -Quick is the default and only names that choice explicitly. Tidy and targeted TSan Quick are
# enabled by default (ADR 0078 decision 10). Explicit -Tidy:$false or -TsanQuick:$false in a
# PowerShell invocation disables that check and is forwarded to WSL. Full still runs full TSan.
#
# The final line is the summary for the commit footer, e.g.
#   "Verified: win debug 63/64 (1 skipped), wsl debug 62/63 (1 skipped), format ok, ..."
# Skipped tests are never counted as passed. Exit status: 0 only if every step
# that ran succeeded (skips and not-run checks do not fail the gate, but are visible).
# Side effects: build trees under build\, the changed-file list build\verify-changed-<pid>.txt
# (deleted at the end), with -Tidy the frost-tidy compile database in the Debug tree, and after a
# passing run whose tree did not change meanwhile the verify-stamp <git common dir>\frost\verify-stamps\
# <tree>.json that the commit-msg hook checks (step 9; install: git config core.hooksPath tools/hooks).

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
    # clang-tidy on both sides; explicit -Tidy:$false disables it.
    [switch]$Tidy = $true,
    # WSL's targeted TSan quick leg; explicit -TsanQuick:$false disables only the quick leg.
    [switch]$TsanQuick = $true,
    # The corpus tier (step 3b): the pinned FrostEngine corpora's tests, here and in WSL.
    [switch]$Corpus,
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

# The wsl.exe arguments of the Linux leg. $ChangedList is the WSL path of the changed-file list, ''
# when it could not be made (verify.sh then neither has nor computes one), or $null when no list was
# asked for (FROST_CHANGED_FILES stays unset). --exec runs env(1) directly, without a shell
# re-parsing the arguments, so the mirror path needs no quoting; verify.sh expands a leading "~/"
# itself. No side effects; tests/tools/test_verify_scripts.py takes it from this script's syntax tree.
function Get-WslArgs($Script, [int]$Jobs, [string]$Base, $ChangedList, [string]$Mirror, [bool]$Full,
        [bool]$Tidy, [bool]$TsanQuick, [bool]$Corpus = $false) {
    $wslArgs = @('-d', 'Ubuntu-24.04', '--exec', 'env', "FROST_JOBS=$Jobs", "FROST_VERIFY_BASE=$Base")
    if ($null -ne $ChangedList) { $wslArgs += "FROST_CHANGED_FILES=$ChangedList" }
    if ($Mirror) { $wslArgs += "FROST_MIRROR=$Mirror" }
    $wslArgs += @('bash', $Script, '--no-checks')
    if ($Full) { $wslArgs += '--full' }
    if ($Tidy) { $wslArgs += '--tidy' } else { $wslArgs += '--no-tidy' }
    if ($TsanQuick) { $wslArgs += '--tsan-quick' } else { $wslArgs += '--no-tsan-quick' }
    if ($Corpus) { $wslArgs += '--corpus' }
    return , $wslArgs
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

# The names of the tests ctest listed under "The following tests FAILED:" (failed, timed out,
# crashed or not run) after its last "tests passed ... out of N" line, in ctest's order; an empty
# array when it listed none. No side effects; tests/tools/test_verify_scripts.py takes it from this
# script's syntax tree.
function Get-CtestFailures([string[]]$Output) {
    $names = New-Object System.Collections.Generic.List[string]
    $inList = $false
    foreach ($line in $Output) {
        if ($line -match 'tests passed.* out of \d+') { $names.Clear(); $inList = $false; continue }
        if ($line -match '^The following tests FAILED:') { $inList = $true; continue }
        if (-not $inList) { continue }
        $match = [regex]::Match($line, '^\s*\d+ - (.+?) \([^)]*\)')
        if ($match.Success) { $names.Add($match.Groups[1].Value) } else { $inList = $false }
    }
    return , $names.ToArray()
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

# Every ctest stage's failed tests, "win <label>: a, b", printed again just above the summary line.
$script:failedTests = New-Object System.Collections.Generic.List[string]

# Prints the failed tests of one ctest stage and keeps them in <Dir>\Testing\Temporary\
# FailedTests-<Label>.log (rewritten every run, empty when none failed). ctest's own
# LastTestsFailed.log names only the last stage's, because the corpus stage reruns ctest in the Debug
# tree. Side effects: that file, console output and $script:failedTests.
function Write-CtestFailures([string]$Label, [string]$Dir, [string[]]$Output) {
    $names = Get-CtestFailures $Output
    $temporary = Join-Path $Dir 'Testing\Temporary'
    if (Test-Path -LiteralPath $temporary) {
        $file = Join-Path $temporary "FailedTests-$Label.log"
        [System.IO.File]::WriteAllLines($file, [string[]]$names, (New-Object System.Text.UTF8Encoding $false))
    }
    if ($names.Count -eq 0) { return }
    Write-Host "verify.ps1: win $Label failed tests ($($names.Count)):"
    $names | ForEach-Object { Write-Host "  $_" }
    $script:failedTests.Add("win ${Label}: $($names -join ', ')")
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
    # --interactive-debug-mode 0: ctest's default (1) starts every test with the default error mode
    # (0), so a test that fails to start (0xC0000142, 0xC0000135) or crashes raises a hard-error or
    # WER box on the user's desktop, whatever desktop the test runs on. In mode 0 the tests inherit
    # SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX and just fail with their status (ADR 0027).
    $skipJunit = Join-Path (Resolve-Path -LiteralPath $Dir).ProviderPath 'skip-audit.xml'
    if (Test-Path -LiteralPath $skipJunit) { Remove-Item -LiteralPath $skipJunit }
    $out = Invoke-Native { ctest --test-dir $Dir -C $Config --output-on-failure --interactive-debug-mode 0 --output-junit $skipJunit @CtestArgs }
    $ok = ($LASTEXITCODE -eq 0)
    $counts = Get-CtestCounts $out
    if (-not $counts) { $counts = 'no-results'; $ok = $false }
    Write-CtestFailures $Label $Dir $out
    $script:summary.Add("win $Label $counts")
    if (-not $ok) { $script:failed = $true }
    # The skip audit (FND-6): every skipped test with its reason; a skip for a missing pinned tool
    # fails the gate ("win <label> skips FAILED (...)"), environment skips are only listed.
    $audit = Invoke-Native { & $python -I (Join-Path $PSScriptRoot 'frost_lint.py') skips --log (Join-Path $Dir 'Testing\Temporary\LastTest.log') --junit $skipJunit }
    if ($LASTEXITCODE -ne 0) {
        $auditLine = $audit | Where-Object { $_ -match '^skips ' } | Select-Object -Last 1
        if (-not $auditLine) { $auditLine = "skips FAILED (exit $LASTEXITCODE)" }
        $script:summary.Add("win $Label $auditLine"); $script:failed = $true
    }
}

$changedList = Join-Path $repo "build\verify-changed-$PID.txt"
# The verify-stamp tool (FND-2, ADR 0078 decision 5) and the tree this run verifies, taken now so a
# file changed mid-run leaves the tree unstamped (step 9).
$stampScript = Join-Path $PSScriptRoot 'hooks\verify_stamp.py'
$stampTree = ''
$stampReason = ''
try {
    # 0. The tree hash of the working tree (tracked and untracked files, through a copy of the index).
    Write-Host '=== verify-stamp tree'
    $out = Invoke-Native { & $python -I $stampScript tree --repo $repo }
    if ($LASTEXITCODE -eq 0) { $stampTree = ($out | Where-Object { $_ -match '^[0-9a-f]{40,64}$' } | Select-Object -Last 1) }
    if (-not $stampTree) { $stampReason = "no tree hash: $($out | Select-Object -Last 1)" }

    # 1. The changed-file list, only for the steps that use it (-Tidy, -TsanQuick; -Full checks
    # everything). A failure is not fatal: tidy then checks every TU and WSL runs all four TSan
    # modules, with the reason in the log.
    $needList = (-not $Full) -and ($Tidy -or $TsanQuick)
    $changedOk = $false
    $changedReason = ''
    if ($needList) {
        Write-Host '=== changed files'
        $out = Invoke-Native { & $python -I $verifySupport changed --repo $repo --base $Base --out $changedList }
        if ($LASTEXITCODE -eq 0 -and (Test-Path -LiteralPath $changedList)) {
            $count = @(Get-Content -LiteralPath $changedList -Encoding UTF8 | Where-Object { $_ }).Count
            Write-Host "verify.ps1: $count changed files against the merge base with $Base"
            $changedOk = $true
        } else {
            $changedReason = ($out | Where-Object { $_ } | Select-Object -Last 1)
            if (-not $changedReason) { $changedReason = "exit $LASTEXITCODE" }
            Write-Host "verify.ps1: no changed-file list: $changedReason"
        }
    }

    # 2. and 3. The Windows configurations.
    $parallel = @('-j', $TestJobs)
    Test-WindowsConfig 'debug' 'win-clangcl' 'Debug' $BuildDir (@('-LE', '^(stress|corpus)$') + $parallel)
    if ($Full) {
        Test-WindowsConfig 'release' 'win-clangcl' 'Release' $BuildDir (@('-LE', '^corpus$') + $parallel)
        $asanDir = ''
        if ($BuildDir) { $asanDir = "$BuildDir-asan" }
        Test-WindowsConfig 'asan' 'win-clangcl-asan' 'RelWithDebInfo' $asanDir (@('-LE', '^corpus$') + $parallel)
        $fuzzDir = ''
        if ($BuildDir) { $fuzzDir = "$BuildDir-fuzz" }
        Test-WindowsConfig 'fuzz' 'win-clangcl-fuzz' 'RelWithDebInfo' $fuzzDir @('-L', '^fuzz$')
    }

    # 3b. The corpus tier (-Corpus only; see the header). The corpora are checked first, so a missing
    # corpus is reported as such and fails, rather than surfacing as a test's failure.
    if ($Corpus) {
        Write-Host '=== corpus verify'
        $null = Invoke-Native { & $python -I (Join-Path $PSScriptRoot 'corpora.py') --repo $repo verify }
        $corpusOk = ($LASTEXITCODE -eq 0)
        if ($corpusOk) {
            # The one configured engine per pinned sha and preset (FND-6b): reused when it is there,
            # configured (never built) under the build lock when it is not; a failure fails the tier.
            Write-Host '=== corpus engine'
            $null = Invoke-Native { & $python -I (Join-Path $PSScriptRoot 'corpora.py') --repo $repo engine --configure }
            $engineOk = ($LASTEXITCODE -eq 0)
        }
        if (-not $corpusOk) {
            $summary.Add('win corpus FAILED (corpus missing or not at its pin; tools\corpora.py fetch)'); $failed = $true
        } elseif (-not $engineOk) {
            $summary.Add('win corpus FAILED (pinned engine not configured; tools\corpora.py engine --configure)'); $failed = $true
        } elseif (-not $script:debugBuilt) {
            $summary.Add('win corpus not-run (debug build failed)'); $failed = $true
        } else {
            Write-Host '=== corpus test'
            # The corpus tests read the corpora root from here, so they name no machine path (FND-10).
            $env:FROST_CORPORA_ROOT = (& $python -I (Join-Path $PSScriptRoot 'corpora.py') --repo $repo root | Select-Object -Last 1)
            $out = Invoke-Native { ctest --test-dir $script:debugDir -C Debug --output-on-failure --interactive-debug-mode 0 -L '^corpus$' --timeout 3600 }
            $ok = ($LASTEXITCODE -eq 0)
            $counts = Get-CtestCounts $out
            Write-CtestFailures 'corpus' $script:debugDir $out
            if (-not $counts) { $summary.Add('win corpus no-tests'); $failed = $true }
            else {
                $summary.Add("win corpus $counts")
                # In the corpus tier a skipped test is a failure (PLAN.md section 7, Principles).
                if (-not $ok -or $counts -match 'skipped') { $failed = $true }
            }
        }
    }

    # Symbolization of the self-test artifacts (-Full only; ADR 0022). The Debug and Release ctest
    # runs above leave a crash dump, a contract-violation dump and a freeze report in
    # <build>\selftest-artifacts\<Config> (frost_process_tests), and tools\symbolize.py
    # check-selftest must resolve each to its marked source line with the pinned llvm-symbolizer.
    # Artifacts older than this verify run (a configuration whose tests did not run) count as
    # none, and "no-artifacts" fails the gate.
    if ($Full) {
        $symbolizeRoot = $BuildDir
        if (-not $symbolizeRoot) { $symbolizeRoot = Join-Path $repo 'build\win-clangcl' }
        $symbolizeSince = (Get-Process -Id $PID).StartTime
        $symbolizer = Join-Path $env:FROST_LLVM_ROOT 'bin\llvm-symbolizer.exe'
        $symbolizePython = Join-Path $env:FROST_SDK 'py\Scripts\python.exe'
        foreach ($symbolizeConfig in @('Debug', 'Release')) {
            $symbolizeLabel = $symbolizeConfig.ToLowerInvariant()
            $artifacts = Join-Path $symbolizeRoot "selftest-artifacts\$symbolizeConfig"
            $fresh = @(Get-ChildItem -Path (Join-Path $artifacts '*\artifact.json') -ErrorAction SilentlyContinue |
                Where-Object { $_.LastWriteTime -ge $symbolizeSince })
            Write-Host "=== $symbolizeLabel symbolize ($artifacts)"
            if ($fresh.Count -eq 0) {
                $summary.Add("win symbolize $symbolizeLabel no-artifacts"); $failed = $true; continue
            }
            $null = Invoke-Native { & $symbolizePython -I (Join-Path $PSScriptRoot 'symbolize.py') check-selftest `
                    --artifacts $artifacts --llvm-symbolizer $symbolizer --source-root $repo }
            if ($LASTEXITCODE -eq 0) { $summary.Add("win symbolize $symbolizeLabel ok") }
            else { $summary.Add("win symbolize $symbolizeLabel FAILED"); $failed = $true }
        }
    }

    # 4. Repository checks.
    $checks = New-Object System.Collections.Generic.List[string]

    # clang-format: every tracked C/C++ file must already be formatted. Fixtures are
    # excluded: they are byte-exact inputs, and clang-format cannot parse the
    # banned-construct probes. Vendored tree-sitter and libvterm sources stay as pinned (ADR 0023, 0051).
    Write-Host '=== format'
    $clangFormat = Join-Path $env:FROST_LLVM_ROOT 'bin\clang-format.exe'
    $sources = @(git ls-files '*.cpp' '*.hpp' '*.h' '*.c' '*.hpp.in' |
        Where-Object { $_ -notlike 'tests/toolchain_probe/banned/*' -and $_ -notlike 'tests/*/fixtures/*' -and
            $_ -notlike 'third_party/tree-sitter/*' -and $_ -notlike 'third_party/grammars/*' -and
            $_ -notlike 'third_party/libvterm/*' })
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

    # The source-to-test line ratchet of this slice, against the merge base with -Base (FND-5, ADR 0078
    # decision 6; the threshold is in tools/frost_lint.py). "ratchet not-run (...)" when the base does
    # not resolve, which does not fail the gate.
    Write-Host '=== ratchet'
    $out = Invoke-Native { & $python -I (Join-Path $PSScriptRoot 'frost_lint.py') ratchet --repo $repo --base $Base }
    $ratchetCode = $LASTEXITCODE
    $ratchetLine = $out | Where-Object { $_ -match '^ratchet ' } | Select-Object -Last 1
    if (-not $ratchetLine) { $ratchetLine = "ratchet FAILED (exit $ratchetCode without a summary)"; $ratchetCode = 1 }
    $checks.Add($ratchetLine)
    if ($ratchetCode -ne 0) { $failed = $true }

    # 5. clang-tidy over the Debug compile database (PLAN.md 6.4 step 2; ADR 0027 decision 1),
    # enabled by default (ADR 0078 decision 10). Budget: B28. -Quick checks the changed files' TUs; every
    # TU runs only without a list.
    if (-not $Tidy) {
        $checks.Add('tidy not-run (disabled by request)')
    } elseif (-not $script:debugBuilt) {
        $checks.Add('tidy not-run (debug build failed)')
    } else {
        Write-Host '=== tidy'
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
        if ($tidyCode -ne 0 -or $tidyLine -match '^tidy FAILED(?:\s|$)') { $failed = $true }
    }

    # 6. and 7. The Linux leg.
    if (-not $WindowsOnly) {
        $wslScript = (ConvertTo-WslPath $repo) + '/tools/verify.sh'
        $wslList = $null
        if ($needList) { $wslList = '' }
        if ($changedOk) { $wslList = ConvertTo-WslPath $changedList }
        $wslArgs = Get-WslArgs -Script $wslScript -Jobs $Jobs -Base $Base -ChangedList $wslList -Mirror $WslMirror `
            -Full ([bool]$Full) -Tidy ([bool]$Tidy) -TsanQuick ([bool]$TsanQuick) -Corpus ([bool]$Corpus)
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
# The failed tests of every Windows ctest stage, so a later stage's output cannot hide them.
if ($script:failedTests.Count -gt 0) {
    Write-Host '=== failed tests'
    $script:failedTests | ForEach-Object { Write-Host $_ }
}
if ($failed) {
    $line = "FAILED: $line"
} elseif ($stampTree) {
    # 9. The verify-stamp: this summary, recorded under the verified tree's hash, which the commit-msg
    # hook (tools/hooks/commit-msg) matches against a footer on main. Not written when the tree changed
    # during the run; a missing stamp does not fail the gate, it only blocks the commit on main.
    Write-Host '=== verify-stamp'
    $null = Invoke-Native { & $python -I $stampScript write --repo $repo --script verify.ps1 --summary $line `
            --expect-tree $stampTree }
} else {
    Write-Host "verify-stamp: not written ($stampReason)"
}
Write-Host $line
if ($failed) { exit 1 }
exit 0
