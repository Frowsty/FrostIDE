# SPDX-License-Identifier: Apache-2.0
#
# Runs the whole spike S1 measurement suite (ADR 0007) on REF-W1 in one quiet
# session, then writes a summary. The suite is, in this order:
#   1. s1_probe_validate (GDI window: injected delays, stalls, cadence, and a
#      trivial window's missed refreshes in a stall-free phase, recorded as
#      context, not as a floor; ADR 0007, Decision 7);
#   2. probe validation through each editor backend: s1_editor --validate-delays
#      (delays spun inside the paint, so QRhi's flip-model swap chain is covered)
#      and --inject-cpu-ms 2 (the UI-CPU probe must see the spin within 10 %);
#   3. -Repeats rounds of the seven editor configurations (five at the full work
#      area with a 10 s fling and 50 keys, two at 1280x720 with a 3 s fling and no
#      typing), interleaved so backends alternate and text and fill runs alternate
#      as far as two fill configurations allow; even rounds run in reverse order;
#   4. first frame (B1), warm, between the rounds: frost.exe, s1_editor's QRhi
#      window and s1_editor's raster window, from process start; frost.exe
#      cold-proxy last;
#   5. s1_probe_validate again, for drift and a second trivial-window sample.
# -Suite floors (run 4 in ADR 0007) interleaves trivial windows with QRhi runs, to
# see whether a trivial window is a floor under the editor's missed refreshes (it
# was not: trivial windows missed more), and measures the whole output:
#   1. s1_probe_validate, and the QRhi delay validation in a window covering the
#      whole output (--full-output), where DWM may flip the swap chain directly;
#   2. -Repeats rounds of four runs: a trivial QRhi window (fill only, 320x48, 5 s,
#      flip-model swap chain), QRhi with text, the same trivial window on raster
#      (GDI flush), and QRhi fill only; even rounds run in reverse order;
#   3. after round 3 (or the last round, if fewer), one round at the whole output
#      (5120x1440 on REF-W1, covering the taskbar): QRhi text, raster fill only,
#      QRhi fill only;
#   4. s1_probe_validate again.
# The step names rhi-floor and gdi-floor are kept for the reports of run 4.
# Every step keeps its window on screen for 20 s or less, never uses exclusive
# fullscreen, never takes focus and closes what it opens. The estimated total is
# printed before anything starts; -DryRun prints the plan and the estimate only.
#
#   powershell -ExecutionPolicy Bypass -File tests\perf\s1\run-s1.ps1 -BuildDir build\s1 -OutDir <dir>
#              [-Config Release] [-Suite main|floors] [-Repeats 5] [-ContinueUnvalidated]
#              [-SkipFirstFrame] [-DryRun] [-SummaryOnly]
#
# Validation policy: if the first s1_probe_validate does not pass (exit code 1, or
# an invalid run the suite survives) the suite stops, because the probe itself is
# not trustworthy, unless -ContinueUnvalidated is given; then every later row is
# tagged "unvalidated". A backend validation (delays or CPU injection) that does
# not pass tags that backend's rows and the suite goes on. An invalid run (exit
# code 3) stops the suite when the session itself became invalid (locked, remote,
# monitor off) or after three invalid runs; a setup error (exit code 2) stops it
# at once.
#
# The -Config configuration (Release by default) of s1_probe_validate, s1_editor,
# s1_first_frame and frost must be built first. Reports are JSON files in -OutDir; summary.json and
# the printed table give each configuration's min / median / max over the rounds.
# -SummaryOnly rebuilds the summary from the reports already in -OutDir.

param(
    [Parameter(Mandatory = $true)] [string] $BuildDir,
    [Parameter(Mandatory = $true)] [string] $OutDir,
    [string] $Config = 'Release',
    [ValidateSet('main', 'floors')] [string] $Suite = 'main',
    [ValidateRange(1, 10)] [int] $Repeats = 5,
    [switch] $ContinueUnvalidated,
    [switch] $SkipFirstFrame,
    [switch] $DryRun,
    [switch] $SummaryOnly
)

$ErrorActionPreference = 'Stop'
$build = (Resolve-Path $BuildDir).Path
New-Item -ItemType Directory -Force $OutDir | Out-Null
$out = (Resolve-Path $OutDir).Path
$s1 = Join-Path $build "tests\perf\s1\$Config"
$frost = Join-Path $build "src\app\$Config\frost.exe"
$validateExe = Join-Path $s1 's1_probe_validate.exe'
$editorExe = Join-Path $s1 's1_editor.exe'
$firstExe = Join-Path $s1 's1_first_frame.exe'
$maxInvalidRuns = 3

# Writes UTF-8 without a byte-order mark (Set-Content -Encoding utf8 in
# PowerShell 5.1 writes one, which strict JSON readers reject).
function Write-Utf8([string] $path, [string] $text) {
    [IO.File]::WriteAllText($path, $text, (New-Object Text.UTF8Encoding($false)))
}

# --- The plan -----------------------------------------------------------------

# Each step: Name (report file stem), Kind (validate, delays, inject, editor,
# first-frame), Group (the configuration its summary row pools), Backend (raster
# or rhi, for validation tags), the executable, its arguments (--out is added),
# and the estimated seconds from launch to exit, most of it on screen.
$plan = New-Object System.Collections.Generic.List[object]
function Add-Step([string] $name, [string] $kind, [string] $group, [string] $backend, [string] $exe,
                  [string[]] $arguments, [double] $seconds) {
    $plan.Add([pscustomobject]@{
            Name = $name; Kind = $kind; Group = $group; Backend = $backend; Exe = $exe
            Arguments = $arguments; Seconds = $seconds
        })
}

# The editor configurations in interleaved order (backends alternate R Q R Q R Q R).
# Estimates: about 1.5 s start-up (200,000-line corpus), 0.6 s show, the fling,
# 50 keys at 23 ms, 0.2 s close.
$editorConfigs = @(
    @{ Name = 'raster-text'; Backend = 'raster'; Seconds = 13.5; Args = @('--backend', 'raster') },
    @{ Name = 'rhi-fill'; Backend = 'rhi'; Seconds = 13.5; Args = @('--backend', 'rhi', '--content', 'fill') },
    @{ Name = 'raster-text-blit'; Backend = 'raster'; Seconds = 13.5; Args = @('--backend', 'raster', '--scroll-blit') },
    @{ Name = 'rhi-text'; Backend = 'rhi'; Seconds = 13.5; Args = @('--backend', 'rhi') },
    @{ Name = 'raster-fill'; Backend = 'raster'; Seconds = 13.5; Args = @('--backend', 'raster', '--content', 'fill') },
    @{ Name = 'rhi-text-1280'; Backend = 'rhi'; Seconds = 5.5
        Args = @('--backend', 'rhi', '--size', '1280x720', '--seconds', '3', '--keys', '0') },
    @{ Name = 'raster-text-1280'; Backend = 'raster'; Seconds = 5.5
        Args = @('--backend', 'raster', '--size', '1280x720', '--seconds', '3', '--keys', '0') }
)
# -Suite floors: trivial windows alternate with QRhi at the work area, plus one
# round covering the whole output.
$fullConfigs = @()
if ($Suite -eq 'floors') {
    $editorConfigs = @(
        @{ Name = 'rhi-floor'; Backend = 'rhi'; Seconds = 7.5
            Args = @('--backend', 'rhi', '--content', 'fill', '--size', '320x48', '--seconds', '5', '--keys', '0') },
        @{ Name = 'rhi-text'; Backend = 'rhi'; Seconds = 13.5; Args = @('--backend', 'rhi') },
        @{ Name = 'gdi-floor'; Backend = 'raster'; Seconds = 7.5
            Args = @('--backend', 'raster', '--content', 'fill', '--size', '320x48', '--seconds', '5', '--keys', '0') },
        @{ Name = 'rhi-fill'; Backend = 'rhi'; Seconds = 13.5; Args = @('--backend', 'rhi', '--content', 'fill') }
    )
    $fullConfigs = @(
        @{ Name = 'rhi-text-full'; Backend = 'rhi'; Seconds = 13.5; Args = @('--backend', 'rhi', '--full-output') },
        @{ Name = 'raster-fill-full'; Backend = 'raster'; Seconds = 13.5
            Args = @('--backend', 'raster', '--content', 'fill', '--full-output') },
        @{ Name = 'rhi-fill-full'; Backend = 'rhi'; Seconds = 13.5
            Args = @('--backend', 'rhi', '--content', 'fill', '--full-output') }
    )
    $SkipFirstFrame = $true
}
$summaryConfigs = @($editorConfigs) + @($fullConfigs)

# The first-frame blocks: 10 warm launches each (plus one priming launch) and one
# cold-proxy block of 5. Add-FirstFrame appends one s1_first_frame step to $plan
# (name, group and backend as in Add-Step): `target` launched `runs` times in `mode` (warm or cold), `childArguments` joined
# into --args, and an estimate from the per-launch figures below (warm counts the
# priming launch).
$inProcess = Join-Path $out 'ff-inprocess.jsonl'
$childOut = @()
if ($out -notmatch '\s') {
    $childOut = @('--out', $inProcess) # s1_first_frame passes --args verbatim; no quoting needed without spaces.
}
function Add-FirstFrame([string] $name, [string] $group, [string] $backend, [string] $target, [string] $mode,
                        [int] $runs, [string[]] $childArguments) {
    $arguments = @('--exe', $target, '--mode', $mode, '--runs', "$runs")
    if ($childArguments.Count -gt 0) {
        $arguments += @('--args', ($childArguments -join ' '))
    }
    # Per launch: about 0.3 s to the window, 0.7 s settle, 0.2 s close, 0.4 s gap;
    # a cold launch adds about 0.8 s of unbuffered copying.
    $perLaunch = 1.6
    if ($mode -eq 'cold') {
        $perLaunch = 2.6
    } else {
        $runs += 1
    }
    Add-Step $name 'first-frame' $group $backend $firstExe $arguments (1.0 + $runs * $perLaunch)
}

Add-Step 'validate-start' 'validate' 'validate' 'gdi' $validateExe @() 21
if ($Suite -eq 'floors') {
    Add-Step 'delays-rhi-full' 'delays' 'delays-rhi-full' 'rhi' $editorExe @(
        '--backend', 'rhi', '--validate-delays', '--full-output') 9
} else {
    foreach ($backend in 'raster', 'rhi') {
        Add-Step "delays-$backend" 'delays' "delays-$backend" $backend $editorExe @(
            '--backend', $backend, '--validate-delays', '--size', '1280x720') 9
    }
    foreach ($backend in 'rhi', 'raster') {
        Add-Step "cpu-inject-$backend" 'inject' "cpu-inject-$backend" $backend $editorExe @(
            '--backend', $backend, '--size', '1280x720', '--seconds', '5', '--keys', '0', '--inject-cpu-ms', '2') 7.5
    }
}
for ($round = 1; $round -le $Repeats; $round++) {
    $order = $editorConfigs
    if ($round % 2 -eq 0) {
        $order = @($editorConfigs[($editorConfigs.Count - 1)..0])
    }
    foreach ($c in $order) {
        Add-Step "$($c.Name)-r$round" 'editor' $c.Name $c.Backend $editorExe $c.Args $c.Seconds
    }
    if ($round -eq [math]::Min(3, $Repeats)) {
        foreach ($c in $fullConfigs) {
            Add-Step "$($c.Name)-r1" 'editor' $c.Name $c.Backend $editorExe $c.Args $c.Seconds
        }
    }
    if ($SkipFirstFrame) {
        continue
    }
    $rhiChild = @('--first-frame', '--backend', 'rhi') + $childOut
    if ($round -eq 1) {
        Add-FirstFrame 'ff-frost-warm-1' 'ff-frost-warm' 'raster' $frost 'warm' 10 @()
        Add-FirstFrame 'ff-rhi-warm-1' 'ff-rhi-warm' 'rhi' $editorExe 'warm' 10 $rhiChild
    } elseif ($round -eq 3) {
        Add-FirstFrame 'ff-rhi-warm-2' 'ff-rhi-warm' 'rhi' $editorExe 'warm' 10 $rhiChild
        Add-FirstFrame 'ff-frost-warm-2' 'ff-frost-warm' 'raster' $frost 'warm' 10 @()
    }
    if ($round -eq $Repeats) {
        Add-FirstFrame 'ff-raster-warm' 'ff-raster-warm' 'raster' $editorExe 'warm' 10 (
            @('--first-frame', '--backend', 'raster') + $childOut)
        Add-FirstFrame 'ff-frost-cold' 'ff-frost-cold' 'raster' $frost 'cold' 5 @()
    }
}
Add-Step 'validate-end' 'validate' 'validate' 'gdi' $validateExe @() 21

$estimate = ($plan | Measure-Object -Property Seconds -Sum).Sum
$estimateText = '{0}:{1:00} (min:s)' -f [int][math]::Floor($estimate / 60), [int]($estimate % 60)

# --- The summary --------------------------------------------------------------

# The nearest-rank percentile of `values`, as tests/perf/probes/src/stats.cpp
# computes it: the ceil(p * n)-th smallest. $null for no values.
function Get-Rank([double[]] $values, [double] $p) {
    if ($null -eq $values -or $values.Count -eq 0) {
        return $null
    }
    $sorted = @($values | Sort-Object)
    $rank = [math]::Max(1, [int][math]::Ceiling($p * $sorted.Count))
    return $sorted[$rank - 1]
}

# n, min, median and max of the non-null `values`, or $null when there are none.
function Get-Spread($values) {
    $v = @($values | Where-Object { $null -ne $_ } | ForEach-Object { [double] $_ })
    if ($v.Count -eq 0) {
        return $null
    }
    $sorted = @($v | Sort-Object)
    return [ordered]@{ n = $sorted.Count; min = $sorted[0]; median = (Get-Rank $sorted 0.5); max = $sorted[-1] }
}

# "min / median / max" with `digits` decimals, or "-".
function Format-Spread($spread, [int] $digits = 2) {
    if ($null -eq $spread) {
        return '-'
    }
    $f = "{0:F$digits}"
    return '{0} / {1} / {2}' -f ($f -f $spread.min), ($f -f $spread.median), ($f -f $spread.max)
}

# The parsed report of step `name`, or $null when it is missing or unreadable.
function Read-Report([string] $name) {
    $path = Join-Path $out "$name.json"
    if (-not (Test-Path $path)) {
        return $null
    }
    try {
        return Get-Content -Raw $path | ConvertFrom-Json
    } catch {
        return $null
    }
}

# How many of `verdicts` are true, out of those measured (non-null): "3/5".
function Format-Verdicts($verdicts) {
    $measured = @($verdicts | Where-Object { $null -ne $_ })
    if ($measured.Count -eq 0) {
        return 'not measured'
    }
    return '{0}/{1}' -f @($measured | Where-Object { $_ -eq $true }).Count, $measured.Count
}

# Builds summary.json from the reports in $out and prints the tables. Side
# effects: overwrites <OutDir>\summary.json.
function Write-Summary {
    $summary = [ordered]@{}
    $summary.generated = (Get-Date).ToString('o')
    $summary.repeats = $Repeats
    $summary.estimatedSeconds = $estimate
    $summary.unvalidated = $script:unvalidated.ToArray()
    $summary.steps = $script:timings.ToArray() # @() of a List[object] fails in PowerShell 5.1.

    $validations = [ordered]@{}
    foreach ($step in @($plan | Where-Object { $_.Kind -in 'validate', 'delays', 'inject' })) {
        $r = Read-Report $step.Name
        if ($null -eq $r) {
            continue
        }
        $row = [ordered]@{ status = $r.status; reason = $r.reason }
        if ($step.Kind -in 'validate', 'delays') {
            $row.passed = $(if ($step.Kind -eq 'validate') { $r.status -eq 'pass' } else { $r.passed })
            $row.shiftErrorMs = @($r.delays | ForEach-Object { $_.shiftErrorMs })
            $row.residualMaxMs = @($r.delays | ForEach-Object { $_.residualMs.max })
            $row.lost = @($r.delays | ForEach-Object { $_.lost })
            $row.problems = @($r.problems)
        }
        if ($step.Kind -eq 'validate') {
            $row.residualBoundMs = $r.residualBoundMs
            $row.stallsMeasured = @($r.stalls | ForEach-Object { '{0} ms: {1}/{2}' -f $_.stallMs, $_.measured, $_.injected })
            $row.cadence = $r.cadence.consistentFraction
            $row.floorMissedPercent = 100.0 * $r.environmentFloor.missedFraction
            $row.floorMaxIntervalMs = $r.environmentFloor.maxIntervalMs
        }
        if ($step.Kind -eq 'inject') {
            $row.passed = $r.cpuInjection.pass
            $row.injectedMs = $r.cpuInjection.injectedMs
            $row.measuredDeltaMs = $r.cpuInjection.measuredDeltaMs
        }
        $validations[$step.Name] = $row
    }
    $summary.validations = $validations

    $editor = [ordered]@{}
    $tableRows = @()
    foreach ($c in $summaryConfigs) {
        $reports = @()
        $invalid = 0
        foreach ($step in @($plan | Where-Object { $_.Kind -eq 'editor' -and $_.Group -eq $c.Name })) {
            $r = Read-Report $step.Name
            if ($null -eq $r) {
                continue
            }
            if ($r.status -eq 'valid') {
                $reports += $r
            } else {
                $invalid++
            }
        }
        $coalesced = @($reports | Where-Object { $_.flingTicks -gt 0 } |
                ForEach-Object { 100.0 * $_.flingCoalesced / $_.flingTicks })
        $typed = @($reports | Where-Object { $_.typing.keys -gt 0 })
        $row = [ordered]@{
            valid = $reports.Count
            invalid = $invalid
            unvalidated = ($script:unvalidated -contains 'all') -or ($script:unvalidated -contains $c.Backend)
            arguments = ($c.Args -join ' ')
            uiCpuP50Ms = Get-Spread ($reports | ForEach-Object { $_.uiCpuPerFrameMs.p50 })
            uiCpuP99Ms = Get-Spread ($reports | ForEach-Object { $_.uiCpuPerFrameMs.p99 })
            paintP50Ms = Get-Spread ($reports | ForEach-Object { $_.paintMs.p50 })
            missedPercent = Get-Spread ($reports | ForEach-Object { 100.0 * $_.content.missedFraction })
            maxIntervalMs = Get-Spread ($reports | ForEach-Object { $_.content.maxIntervalMs })
            vblankToScreenP50Ms = Get-Spread ($reports | ForEach-Object { $_.flingLatency.inputToScreenMs.p50 })
            readyToScreenP50Ms = Get-Spread ($reports | ForEach-Object { $_.flingLatency.readyToScreenMs.p50 })
            coalescedPercent = Get-Spread $coalesced
            # Updates whose capture merged several presents: missed and max interval are upper bounds.
            mergedCaptures = Get-Spread ($reports | ForEach-Object { $_.content.mergedCaptures })
            b5P50Ms = Get-Spread ($typed | ForEach-Object { $_.typing.inputToPaintEndMs.p50 })
            b5P99Ms = Get-Spread ($typed | ForEach-Object { $_.typing.inputToPaintEndMs.p99 })
            typingToScreenP50Ms = Get-Spread ($typed | ForEach-Object { $_.typing.latency.inputToScreenMs.p50 })
            b6CpuPass = Format-Verdicts ($reports | ForEach-Object { $_.b6.cpuP99Pass })
            b6MissedPass = Format-Verdicts ($reports | ForEach-Object { $_.b6.missedPass })
            b6IntervalPass = Format-Verdicts ($reports | ForEach-Object { $_.b6.intervalPass })
            b5Pass = Format-Verdicts ($reports | ForEach-Object { $_.b5Pass })
        }
        $editor[$c.Name] = $row
        $tableRows += [pscustomobject]@{
            Config = $c.Name + $(if ($row.unvalidated) { ' (unvalidated)' } else { '' })
            'n' = '{0}{1}' -f $row.valid, $(if ($invalid -gt 0) { " (+$invalid invalid)" } else { '' })
            'CPU p99 ms' = Format-Spread $row.uiCpuP99Ms
            'Missed %' = Format-Spread $row.missedPercent 1
            'Max int ms' = Format-Spread $row.maxIntervalMs 1
            'Merged' = Format-Spread $row.mergedCaptures 0
            'Vblank-screen p50' = Format-Spread $row.vblankToScreenP50Ms 1
            'B5 p99 ms' = Format-Spread $row.b5P99Ms
        }
    }
    $summary.editor = $editor

    $firstFrame = [ordered]@{}
    $ffRows = @()
    foreach ($group in 'ff-frost-warm', 'ff-rhi-warm', 'ff-raster-warm', 'ff-frost-cold') {
        $first = @()
        $settled = @()
        $seen = @()
        $blocks = @()
        foreach ($step in @($plan | Where-Object { $_.Kind -eq 'first-frame' -and $_.Group -eq $group })) {
            $r = Read-Report $step.Name
            if ($null -eq $r -or $r.status -ne 'valid') {
                continue
            }
            $launches = @($r.launches | Where-Object { $_.valid -and -not $_.priming })
            $first += @($launches | ForEach-Object { [double] $_.firstFrameMs })
            $settled += @($launches | Where-Object { $null -ne $_.settledMs } | ForEach-Object { [double] $_.settledMs })
            $seen += @($launches | ForEach-Object { [double] $_.windowSeenMs })
            $blocks += [ordered]@{ step = $step.Name; valid = $launches.Count; p50 = $r.firstFrameMs.p50 }
        }
        if ($first.Count -eq 0) {
            continue
        }
        $row = [ordered]@{
            launches = $first.Count
            firstFrameMs = [ordered]@{
                min = (Get-Rank $first 0.0); p50 = (Get-Rank $first 0.5); p90 = (Get-Rank $first 0.9)
                p99 = (Get-Rank $first 0.99); max = (Get-Rank $first 1.0)
            }
            settledP50Ms = Get-Rank $settled 0.5
            windowSeenP50Ms = Get-Rank $seen 0.5
            blocks = $blocks
        }
        $firstFrame[$group] = $row
        $ffRows += [pscustomobject]@{
            Target = $group
            n = $first.Count
            'First frame min / p50 / p90 / max ms' = '{0:F1} / {1:F1} / {2:F1} / {3:F1}' -f $row.firstFrameMs.min,
            $row.firstFrameMs.p50, $row.firstFrameMs.p90, $row.firstFrameMs.max
            'Settled p50' = '{0:F1}' -f $row.settledP50Ms
            'Seen p50' = '{0:F1}' -f $row.windowSeenP50Ms
        }
    }
    if (Test-Path $inProcess) {
        $lines = @(Get-Content $inProcess | Where-Object { $_.Trim() } | ForEach-Object { $_ | ConvertFrom-Json })
        foreach ($backend in 'rhi', 'raster') {
            $mine = @($lines | Where-Object { $_.backend -eq $backend })
            if ($mine.Count -gt 0) {
                $firstFrame["inprocess-$backend"] = [ordered]@{
                    launches = $mine.Count
                    creationToMainP50Ms = Get-Rank @($mine | ForEach-Object { $_.creationToMainMs }) 0.5
                    mainToFirstPaintEndP50Ms = Get-Rank @($mine | Where-Object { $null -ne $_.mainToFirstPaintEndMs } |
                            ForEach-Object { $_.mainToFirstPaintEndMs }) 0.5
                    pipelineP50Ms = Get-Rank @($mine | Where-Object { $null -ne $_.pipelineMs } |
                            ForEach-Object { $_.pipelineMs }) 0.5
                }
            }
        }
    }
    $summary.firstFrame = $firstFrame

    Write-Utf8 (Join-Path $out 'summary.json') ($summary | ConvertTo-Json -Depth 8)
    Write-Host ''
    Write-Host 'Validations:'
    foreach ($name in $validations.Keys) {
        $v = $validations[$name]
        Write-Host ('  {0,-18} {1,-8} passed={2} {3}' -f $name, $v.status, $v.passed, $v.reason)
    }
    Write-Host 'Editor configurations (min / median / max over the rounds):'
    $tableRows | Format-Table -AutoSize | Out-Host
    if ($ffRows.Count -gt 0) {
        Write-Host 'First frame (B1), pooled over blocks:'
        $ffRows | Format-Table -AutoSize | Out-Host
    }
    Write-Host "Summary written to $(Join-Path $out 'summary.json')"
}

# --- Running ------------------------------------------------------------------

$script:timings = New-Object System.Collections.Generic.List[object]
$script:unvalidated = New-Object System.Collections.Generic.List[string]
$script:invalidRuns = 0

if ($SummaryOnly) {
    Write-Summary
    exit 0
}

Write-Host ("S1 suite: {0} steps, {1} rounds; estimated screen time {2}." -f $plan.Count, $Repeats, $estimateText)
if ($DryRun) {
    $plan | ForEach-Object {
        [pscustomobject]@{ Step = $_.Name; Seconds = $_.Seconds; Arguments = ($_.Arguments -join ' ') }
    } | Format-Table -AutoSize | Out-Host
    exit 0
}
foreach ($exe in $validateExe, $editorExe, $firstExe, $frost) {
    if (-not (Test-Path $exe)) {
        throw "$exe is missing; build the $Config configuration first"
    }
}

# Records the machine and its state, which every number depends on: CPU and GPU
# models, Windows build, power scheme, total CPU load and free memory before the
# first step, Defender real-time protection and the Dev Drive state of the build
# volume (budgets.json "cold" default).
$environment = [ordered]@{}
$environment.started = (Get-Date).ToString('o')
$cpu = Get-CimInstance Win32_Processor | Select-Object -First 1
$environment.cpu = [ordered]@{
    name = $cpu.Name.Trim(); cores = $cpu.NumberOfCores; logicalProcessors = $cpu.NumberOfLogicalProcessors
    maxClockMHz = $cpu.MaxClockSpeed
}
$environment.gpus = @(Get-CimInstance Win32_VideoController | ForEach-Object {
        [ordered]@{ name = $_.Name; driver = $_.DriverVersion; refreshHz = $_.CurrentRefreshRate
            width = $_.CurrentHorizontalResolution; height = $_.CurrentVerticalResolution }
    })
$os = Get-CimInstance Win32_OperatingSystem
$environment.windows = '{0} build {1}' -f $os.Caption, $os.BuildNumber
$environment.freeCommitMB = [int]($os.FreeVirtualMemory / 1024)
$environment.powerScheme = (& powercfg /getactivescheme 2>&1 | Out-String).Trim()
$environment.cpuLoadPercentAtStart = (Get-CimInstance Win32_PerfFormattedData_PerfOS_Processor -Filter "Name='_Total'").PercentProcessorTime
try {
    $mp = Get-MpComputerStatus -ErrorAction Stop
    $environment.defenderRealTime = $mp.RealTimeProtectionEnabled
} catch {
    $environment.defenderRealTime = 'unreadable'
}
$volume = (Split-Path -Qualifier $build)
$environment.devDrive = (& fsutil devdrv query $volume 2>&1 | Out-String).Trim()
Write-Utf8 (Join-Path $out 'environment.json') ($environment | ConvertTo-Json -Depth 4)
if (Test-Path $inProcess) {
    Remove-Item $inProcess
}

# Runs one plan step: `$step.Exe` with its arguments and `--out <OutDir>\<Name>.json`,
# its output shown on the console. Returns the exit code: 0 valid, 1 a validation
# failed, or 3 an invalid run that did not stop the suite. Side effects: records the step's duration in $script:timings and
# counts invalid runs. Stops the suite (after writing the summary) on a setup
# error (2) or any unknown code, and on an invalid run (3) when the report says
# the session itself ended invalid or after $maxInvalidRuns invalid runs.
function Invoke-Step($step) {
    $report = Join-Path $out "$($step.Name).json"
    Write-Host ("== {0}: {1}" -f $step.Name, ($step.Arguments -join ' '))
    $started = Get-Date
    & $step.Exe @($step.Arguments) --out $report | Out-Host
    $code = $LASTEXITCODE
    $script:timings.Add([pscustomobject]@{
            name = $step.Name; exitCode = $code; seconds = [math]::Round(((Get-Date) - $started).TotalSeconds, 1)
            estimate = $step.Seconds
        })
    if ($code -eq 3) {
        $script:invalidRuns++
        $r = Read-Report $step.Name
        $sessionEnded = ($null -eq $r) -or ($null -eq $r.sessionEnd) -or (-not $r.sessionEnd.valid)
        if ($sessionEnded -or $script:invalidRuns -ge $maxInvalidRuns) {
            Write-Host "$($step.Name) was invalid ($($r.reason)); stopping, later runs would be invalid too."
            Write-Summary
            exit 3
        }
        Write-Host "$($step.Name) was invalid ($($r.reason)); continuing ($($script:invalidRuns) of $maxInvalidRuns)."
    } elseif ($code -ne 0 -and $code -ne 1) {
        Write-Summary
        throw "$($step.Name) failed with exit code $code"
    }
    return $code
}

$suiteStart = Get-Date
foreach ($step in $plan) {
    $code = Invoke-Step $step
    # A validation step that failed (1) or was invalid but let the suite go on (3)
    # validated nothing.
    if ($code -eq 0 -or $step.Kind -notin 'validate', 'delays', 'inject') {
        continue
    }
    if ($step.Name -eq 'validate-start') {
        if (-not $ContinueUnvalidated) {
            Write-Host 'The probe validation did not pass; stopping (use -ContinueUnvalidated to measure anyway).'
            Write-Summary
            exit 1
        }
        $script:unvalidated.Add('all')
    } elseif ($step.Kind -in 'delays', 'inject') {
        Write-Host "The $($step.Backend) backend's probe validation did not pass; its rows are tagged unvalidated."
        $script:unvalidated.Add($step.Backend)
    } elseif ($step.Kind -eq 'validate') {
        $script:unvalidated.Add('end-validation-failed')
    }
}
$elapsed = ((Get-Date) - $suiteStart).TotalSeconds
Write-Host ('S1 suite finished in {0}:{1:00} (estimate {2}).' -f [int][math]::Floor($elapsed / 60), [int]($elapsed % 60),
    $estimateText)
$environment.finished = (Get-Date).ToString('o')
$environment.cpuLoadPercentAtEnd = (Get-CimInstance Win32_PerfFormattedData_PerfOS_Processor -Filter "Name='_Total'").PercentProcessorTime
Write-Utf8 (Join-Path $out 'environment.json') ($environment | ConvertTo-Json -Depth 4)
Write-Summary
