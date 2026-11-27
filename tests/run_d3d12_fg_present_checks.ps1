# SPDX-License-Identifier: MIT
# Runs the installed D3D12FG consumer in separate processes: the original readback and retirement check,
# DXGI presentation with VSync on and off, ordinary rendering without FG, rejected-input fallback with a
# history cut and a resize while work is outstanding, the same under the D3D12 debug layer, extended runs
# and timing runs without readback. Presentation cases open a visible window; keep it unoccluded.
# -PresentMon <PresentMon 2.x exe> also records every presentation case with PresentMon (ETW; needs
# elevation or Performance Log Users membership). The capture keeps only the client's own rows
# (presentmon.csv) and the report adds the present mode, tearing, displayed and dropped frames and
# the present and display-change intervals measured by DXGI/DWM events.
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Executable,
    [Parameter(Mandatory)][string]$RuntimeDirectory,
    [string]$Proxy = '-',
    [string]$LogRoot = (Join-Path (Get-Location) 'framelab-logs'),
    [int]$ExtendedFrames = 1800,
    [int]$TimingFrames = 600,
    [string]$PresentMon,
    [switch]$SkipDebug,
    [switch]$SkipExtended
)
$ErrorActionPreference = 'Stop'
$exe = (Get-Item -LiteralPath $Executable).FullName
$runtime = (Get-Item -LiteralPath $RuntimeDirectory).FullName
$runId = 'd3d12-fg-present-{0:yyyyMMdd-HHmmss}-{1}' -f (Get-Date), ([guid]::NewGuid().ToString('N').Substring(0, 8))
$logs = Join-Path ($ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($LogRoot)) $runId
New-Item -ItemType Directory -Force -Path $logs | Out-Null
Write-Output "D3D12 FG presentation evidence directory: $logs"

$recovery = @('--reject-frame', '--cut-at', '4', '--resize')
$cases = @(
    @{ name = 'readback'; args = @() },
    @{ name = 'vsync'; args = @('--vsync') },
    @{ name = 'no-vsync'; args = @('--no-vsync') },
    @{ name = 'no-fg'; args = @('--vsync', '--no-fg') },
    @{ name = 'recovery'; args = @('--no-vsync') + $recovery }
)
if (-not $SkipDebug) { $cases += @{ name = 'debug'; args = @('--vsync', '--debug') + $recovery } }
if (-not $SkipExtended) {
    $cases += @{ name = 'extended-no-vsync'; args = @('--no-vsync', '--frames', "$ExtendedFrames") }
    $cases += @{ name = 'extended-vsync'; args = @('--vsync', '--frames', "$ExtendedFrames") }
}
$cases += @{ name = 'timing-no-vsync'; args = @('--no-vsync', '--timing', '--frames', "$TimingFrames") }
$cases += @{ name = 'timing-vsync'; args = @('--vsync', '--timing', '--frames', "$TimingFrames") }

$monitor = if ($PresentMon) { (Get-Item -LiteralPath $PresentMon).FullName } else { $null }
$session = 'FrameLabD3D12FgPresent'

function Start-Capture([string]$Directory) {
    $raw = Join-Path $Directory 'presentmon-raw.csv'
    $out = Join-Path $Directory 'presentmon-stdout.log'
    $err = Join-Path $Directory 'presentmon-stderr.log'
    $capture = Start-Process -FilePath $monitor -WindowStyle Hidden -PassThru -RedirectStandardOutput $out -RedirectStandardError $err `
        -ArgumentList @('--v1_metrics', '--output_file', "`"$raw`"", '--session_name', $session, '--stop_existing_session', '--no_console_stats')
    $null = $capture.Handle
    # PresentMon creates the CSV once its trace session records; it prints nothing in this mode.
    $deadline = (Get-Date).AddSeconds(15)
    while (-not (Test-Path -LiteralPath $raw)) {
        if ($capture.HasExited -or (Get-Date) -gt $deadline) {
            Stop-Session $capture
            throw "PresentMon did not start recording; see $err"
        }
        Start-Sleep -Milliseconds 100
    }
    Start-Sleep -Milliseconds 500
    $capture
}

function Stop-Session($Capture) {
    $null = & $monitor --terminate_existing_session --session_name $session 2>&1
    if (-not $Capture.WaitForExit(15000)) { $Capture.Kill(); $Capture.WaitForExit() }
}

function Stop-Capture($Capture, [string]$Directory, [int]$ProcessId) {
    # Display events for the last frames arrive after the client's final Present.
    Start-Sleep -Seconds 1
    Stop-Session $Capture
    $raw = Join-Path $Directory 'presentmon-raw.csv'
    $rows = if (Test-Path -LiteralPath $raw) { @(Import-Csv -LiteralPath $raw | Where-Object { [int]$_.ProcessID -eq $ProcessId }) } else { @() }
    $csv = @{ LiteralPath = (Join-Path $Directory 'presentmon.csv'); Encoding = 'utf8' }
    # Windows PowerShell 5.1 writes a #TYPE line unless told not to; PowerShell 7 never does.
    if ($PSVersionTable.PSVersion.Major -lt 6) { $csv.NoTypeInformation = $true }
    $rows | Export-Csv @csv
    Remove-Item -LiteralPath $raw -ErrorAction SilentlyContinue
    $stats = {
        param([double[]]$values)
        if (-not $values.Count) { return $null }
        $sorted = $values | Sort-Object
        [ordered]@{
            count = $sorted.Count
            p50 = [Math]::Round($sorted[[int][Math]::Floor(0.50 * ($sorted.Count - 1))], 3)
            p95 = [Math]::Round($sorted[[int][Math]::Floor(0.95 * ($sorted.Count - 1))], 3)
            min = [Math]::Round($sorted[0], 3)
            max = [Math]::Round($sorted[-1], 3)
        }
    }
    $displayed = @($rows | Where-Object { $_.Dropped -eq '0' })
    $between = @($rows | Select-Object -Skip 1 | ForEach-Object { [double]$_.msBetweenPresents })
    $changes = @($displayed | ForEach-Object { [double]$_.msBetweenDisplayChange } | Where-Object { $_ -gt 0 })
    $modes = [ordered]@{}
    foreach ($group in @($rows | Group-Object PresentMode)) { $modes[$group.Name] = $group.Count }
    [ordered]@{
        presents = $rows.Count
        displayed = $displayed.Count
        dropped = $rows.Count - $displayed.Count
        present_modes = $modes
        allows_tearing = @($rows | Select-Object -ExpandProperty AllowsTearing -Unique)
        sync_intervals = @($rows | Select-Object -ExpandProperty SyncInterval -Unique)
        runtime = @($rows | Select-Object -ExpandProperty Runtime -Unique)
        ms_between_presents = & $stats $between
        ms_between_display_change = & $stats $changes
        ms_until_displayed = & $stats @($displayed | ForEach-Object { [double]$_.msUntilDisplayed })
    }
}

$results = @()
$failed = $false
foreach ($case in $cases) {
    $caseLogs = Join-Path $logs $case.name
    New-Item -ItemType Directory -Path $caseLogs | Out-Null
    $stdout = Join-Path $caseLogs 'stdout.log'
    $stderr = Join-Path $caseLogs 'stderr.log'
    $arguments = @("`"$runtime`"", "`"$caseLogs`"", "`"$Proxy`"") + $case.args
    $capture = if ($monitor -and $case.name -ne 'readback') { Start-Capture $caseLogs } else { $null }
    $started = Get-Date
    $run = Start-Process -FilePath $exe -ArgumentList $arguments -WorkingDirectory (Split-Path $exe) `
        -PassThru -RedirectStandardOutput $stdout -RedirectStandardError $stderr
    # Windows PowerShell 5.1 reports a null ExitCode unless the process handle is opened before the process exits.
    $null = $run.Handle
    $peak = 0
    while (-not $run.HasExited) {
        try { $run.Refresh(); $peak = [Math]::Max($peak, $run.PeakWorkingSet64) } catch { }
        if (((Get-Date) - $started).TotalSeconds -gt 900) {
            $run.Kill(); $run.WaitForExit()
            if ($capture) { Stop-Session $capture }
            throw "Timeout: $($case.name)"
        }
        Start-Sleep -Milliseconds 250
    }
    $run.WaitForExit()
    if ($null -eq $run.ExitCode) { throw "Exit code unavailable: $($case.name)" }
    $captured = if ($capture) { Stop-Capture $capture $caseLogs $run.Id } else { $null }
    $text = Get-Content -LiteralPath $stdout -Raw
    $summaryPath = Join-Path $caseLogs 'd3d12-fg-summary.json'
    $summary = if (Test-Path -LiteralPath $summaryPath) { Get-Content -LiteralPath $summaryPath -Raw | ConvertFrom-Json } else { $null }
    $row = [ordered]@{
        name = $case.name
        arguments = ($case.args -join ' ')
        exit_code = $run.ExitCode
        seconds = [Math]::Round(((Get-Date) - $started).TotalSeconds, 1)
        stderr_bytes = (Get-Item -LiteralPath $stderr).Length
        peak_working_set_mb = [Math]::Round($peak / 1MB, 1)
        summary = $summary
        presentmon = $captured
    }
    if ($case.name -eq 'readback') {
        $readback = Join-Path $caseLogs 'generated-5.rgba'
        $ok = $run.ExitCode -eq 0 -and $text -match 'SDK_FG_SUMMARY real_frames=6 verified=5 final_lease_releases=3' -and
            $text -match 'early_retire_rejected=1 pending_destroy_rejected=1' -and (Test-Path -LiteralPath $readback)
        if (Test-Path -LiteralPath $readback) { $row.readback_sha256 = (Get-FileHash -LiteralPath $readback -Algorithm SHA256).Hash }
    } else {
        $ok = $run.ExitCode -eq 0 -and $summary -and $summary.result -eq 'pass' -and $text -match 'FL15_RESULT pass'
        if ($summary -and $summary.counters.occluded_presents -gt 0) {
            Write-Warning "$($case.name): $($summary.counters.occluded_presents) Present calls reported DXGI_STATUS_OCCLUDED"
        }
    }
    $ok = $ok -and $row.stderr_bytes -eq 0
    $row.verdict = if ($ok) { 'pass' } else { 'fail' }
    if (-not $ok) { $failed = $true; Write-Warning "$($case.name) failed. See $stdout" }
    $results += [pscustomobject]$row
    $measured = if ($captured) {
        ", PresentMon {0} presents, {1} displayed, modes {2}" -f $captured.presents, $captured.displayed,
            (($captured.present_modes.GetEnumerator() | ForEach-Object { "$($_.Key)=$($_.Value)" }) -join '; ')
    } else { '' }
    Write-Output ("{0}: {1} (exit {2}, {3} s{4})" -f $case.name, $row.verdict, $run.ExitCode, $row.seconds, $measured)
}

$gpu = Get-CimInstance Win32_VideoController | Where-Object { $_.AdapterCompatibility -match 'NVIDIA' } | Select-Object -First 1
$report = [ordered]@{
    run_id = $runId
    executable = $exe
    runtime_directory = $runtime
    gpu = if ($gpu) { $gpu.Name } else { $null }
    driver = if ($gpu) { $gpu.DriverVersion } else { $null }
    os = [Environment]::OSVersion.VersionString
    # PresentMon reports how each Present reached the display (DXGI and DWM ETW events); physical
    # scanout itself is not measured.
    scanout = if ($monitor) { 'PresentMon ETW present mode and display-change timing; physical scanout unmeasured' } else { 'unmeasured' }
    presentmon = if ($monitor) { [ordered]@{ path = $monitor; sha256 = (Get-FileHash -LiteralPath $monitor -Algorithm SHA256).Hash } } else { $null }
    cases = $results
}
$report | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $logs 'd3d12-fg-present-results.json') -Encoding utf8
if ($failed) { exit 1 }
exit 0
