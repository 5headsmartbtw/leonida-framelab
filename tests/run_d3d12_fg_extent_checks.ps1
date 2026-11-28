# SPDX-License-Identifier: MIT
# Runs the installed D3D12FG consumer once per render extent in separate processes: equal extents
# (the existing regression) and render extents below the 1280x720 output. Each run also performs
# the descriptor, rejection, retention and recreation checks built into the sample.
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Executable,
    [Parameter(Mandatory)][string]$RuntimeDirectory,
    [string]$AppLocalProxy = '-',
    [string]$LogRoot = (Join-Path (Get-Location) 'framelab-logs'),
    [string[]]$RenderExtents = @('', '960x540', '640x360')
)
$ErrorActionPreference = 'Stop'
$exe = (Get-Item -LiteralPath $Executable).FullName
$runtime = (Get-Item -LiteralPath $RuntimeDirectory).FullName
$proxy = if ($AppLocalProxy -eq '-') { '-' } else { (Get-Item -LiteralPath $AppLocalProxy).FullName }
$runId = 'd3d12-fg-extent-{0:yyyyMMdd-HHmmss}-{1}' -f (Get-Date), ([guid]::NewGuid().ToString('N').Substring(0, 8))
$logs = Join-Path ($ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($LogRoot)) $runId
New-Item -ItemType Directory -Force -Path $logs | Out-Null
Write-Output "D3D12 FG extent evidence directory: $logs"

$checks = @('v1_size=', 'partial_sizes_rejected=1', 'invalid_render_extents_rejected=1',
    'invalid_native_state_rejected=1', 'render_extent_mismatch_rejected=1', 'native_extent_mismatch_rejected=1',
    'stale_depth_rejected=1', 'early_retire_rejected=1 pending_destroy_rejected=1', 'recreated_requires_reset=1')
$results = @()
$failed = $false
foreach ($render in $RenderExtents) {
    $name = if ($render) { "render-$render" } else { 'equal' }
    $caseLogs = Join-Path $logs $name
    New-Item -ItemType Directory -Path $caseLogs | Out-Null
    $stdout = Join-Path $caseLogs 'stdout.log'
    $stderr = Join-Path $caseLogs 'stderr.log'
    $arguments = @("`"$runtime`"", "`"$caseLogs`"", "`"$proxy`"")
    if ($render) { $arguments += @('--render', $render) }
    $run = Start-Process -FilePath $exe -ArgumentList $arguments -WorkingDirectory (Split-Path $exe) -WindowStyle Hidden `
        -PassThru -RedirectStandardOutput $stdout -RedirectStandardError $stderr
    # Windows PowerShell 5.1 reports a null ExitCode unless the process handle is opened before the process exits.
    $null = $run.Handle
    if (-not $run.WaitForExit(600000)) { $run.Kill(); $run.WaitForExit(); throw "Timeout: $name" }
    $run.WaitForExit()
    if ($null -eq $run.ExitCode) { throw "Exit code unavailable: $name" }
    $text = [string](Get-Content -LiteralPath $stdout -Raw)
    $missing = @($checks | Where-Object { $text -notmatch [regex]::Escape("ABI_CHECK $_") })
    $summary = [regex]::Match($text, 'SDK_FG_SUMMARY real_frames=(\d+) verified=(\d+) final_lease_releases=(\d+) max_edge_error=(-?\d+)')
    $recreated = [regex]::Match($text, 'SDK_FG_RECREATED render=(\S+) real_frames=(\d+) verified=(\d+) final_lease_releases=(\d+) max_edge_error=(-?\d+)')
    $row = [ordered]@{
        name = $name
        exit_code = $run.ExitCode
        stderr_bytes = (Get-Item -LiteralPath $stderr).Length
        missing_checks = $missing
        verified = if ($summary.Success) { [int]$summary.Groups[2].Value } else { $null }
        lease_releases = if ($summary.Success) { [int]$summary.Groups[3].Value } else { $null }
        max_edge_error = if ($summary.Success) { [int]$summary.Groups[4].Value } else { $null }
        recreated_render = if ($recreated.Success) { $recreated.Groups[1].Value } else { $null }
        recreated_verified = if ($recreated.Success) { [int]$recreated.Groups[3].Value } else { $null }
        recreated_lease_releases = if ($recreated.Success) { [int]$recreated.Groups[4].Value } else { $null }
        recreated_max_edge_error = if ($recreated.Success) { [int]$recreated.Groups[5].Value } else { $null }
    }
    $readback = Join-Path $caseLogs 'generated-5.rgba'
    $row.readback_sha256 = $null
    if (Test-Path -LiteralPath $readback) {
        if ((Get-Item -LiteralPath $readback).Length -ne 1280 * 720 * 4) { throw "Unexpected readback size: $readback" }
        $row.readback_sha256 = (Get-FileHash -LiteralPath $readback -Algorithm SHA256).Hash
    }
    $ok = $run.ExitCode -eq 0 -and $row.stderr_bytes -eq 0 -and $missing.Count -eq 0 -and $row.readback_sha256 -and
        $row.verified -eq 5 -and $row.lease_releases -eq 3 -and $row.recreated_verified -eq 2 -and
        $row.recreated_lease_releases -eq 3
    $row.verdict = if ($ok) { 'pass' } else { 'fail' }
    if (-not $ok) { $failed = $true; Write-Warning "$name failed. See $stdout" }
    $results += [pscustomobject]$row
    Write-Output ("{0}: {1} (exit {2}, max edge error {3} px)" -f $name, $row.verdict, $run.ExitCode, $row.max_edge_error)
}

$gpu = Get-CimInstance Win32_VideoController -ErrorAction SilentlyContinue |
    Where-Object { $_.AdapterCompatibility -match 'NVIDIA' } | Select-Object -First 1
$report = [ordered]@{
    run_id = $runId
    executable = $exe
    runtime_directory = $runtime
    app_local_proxy = $proxy
    gpu = if ($gpu) { $gpu.Name } else { $null }
    driver = if ($gpu) { $gpu.DriverVersion } else { $null }
    os = [Environment]::OSVersion.VersionString
    cases = $results
}
$report | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $logs 'd3d12-fg-extent-results.json') -Encoding utf8
if ($failed) { exit 1 }
exit 0
