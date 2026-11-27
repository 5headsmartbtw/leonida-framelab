# SPDX-License-Identifier: MIT
# Runs the installed D3D12FG consumer in separate processes: the original readback and retirement check,
# DXGI presentation with VSync on and off, ordinary rendering without FG, rejected-input fallback with a
# history cut and a resize while work is outstanding, the same under the D3D12 debug layer, extended runs
# and timing runs without readback. Presentation cases open a visible window; keep it unoccluded.
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Executable,
    [Parameter(Mandatory)][string]$RuntimeDirectory,
    [string]$Proxy = '-',
    [string]$LogRoot = (Join-Path (Get-Location) 'framelab-logs'),
    [int]$ExtendedFrames = 1800,
    [int]$TimingFrames = 600,
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

$results = @()
$failed = $false
foreach ($case in $cases) {
    $caseLogs = Join-Path $logs $case.name
    New-Item -ItemType Directory -Path $caseLogs | Out-Null
    $stdout = Join-Path $caseLogs 'stdout.log'
    $stderr = Join-Path $caseLogs 'stderr.log'
    $arguments = @("`"$runtime`"", "`"$caseLogs`"", "`"$Proxy`"") + $case.args
    $started = Get-Date
    $run = Start-Process -FilePath $exe -ArgumentList $arguments -WorkingDirectory (Split-Path $exe) `
        -PassThru -RedirectStandardOutput $stdout -RedirectStandardError $stderr
    # Windows PowerShell 5.1 reports a null ExitCode unless the process handle is opened before the process exits.
    $null = $run.Handle
    $peak = 0
    while (-not $run.HasExited) {
        try { $run.Refresh(); $peak = [Math]::Max($peak, $run.PeakWorkingSet64) } catch { }
        if (((Get-Date) - $started).TotalSeconds -gt 900) { $run.Kill(); $run.WaitForExit(); throw "Timeout: $($case.name)" }
        Start-Sleep -Milliseconds 250
    }
    $run.WaitForExit()
    if ($null -eq $run.ExitCode) { throw "Exit code unavailable: $($case.name)" }
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
    Write-Output ("{0}: {1} (exit {2}, {3} s)" -f $case.name, $row.verdict, $run.ExitCode, $row.seconds)
}

$gpu = Get-CimInstance Win32_VideoController | Where-Object { $_.AdapterCompatibility -match 'NVIDIA' } | Select-Object -First 1
$report = [ordered]@{
    run_id = $runId
    executable = $exe
    runtime_directory = $runtime
    gpu = if ($gpu) { $gpu.Name } else { $null }
    driver = if ($gpu) { $gpu.DriverVersion } else { $null }
    os = [Environment]::OSVersion.VersionString
    scanout = 'unmeasured'
    cases = $results
}
$report | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $logs 'd3d12-fg-present-results.json') -Encoding utf8
if ($failed) { exit 1 }
exit 0
