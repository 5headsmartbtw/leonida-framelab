# SPDX-License-Identifier: MIT
# Runs the installed D3D12FG consumer once per color mode and render extent in separate processes:
# equal extents (the existing regression) and render extents below the 1280x720 output, for RGBA8,
# BGRA8, RGB10A2 and HDR10 color. Each run also performs the descriptor, rejection, retention and
# recreation checks built into the sample. A color mode the device cannot run must be refused by
# create; that case is reported as 'unsupported-rejected' instead of running frames.
# -DebugLayer runs every case with the D3D12 debug layer and GPU-based validation (needs the
# Windows Graphics Tools feature). A case then also fails on any corruption or error message; each
# case's messages are in d3d12-debug.log.
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Executable,
    [Parameter(Mandatory)][string]$RuntimeDirectory,
    [string]$LogRoot = (Join-Path (Get-Location) 'framelab-logs'),
    [string[]]$RenderExtents = @('', '960x540', '640x360'),
    [ValidateSet('rgba8', 'bgra8', 'rgb10a2', 'hdr10')][string[]]$ColorModes = @('rgba8', 'bgra8', 'rgb10a2', 'hdr10'),
    [switch]$DebugLayer
)
$ErrorActionPreference = 'Stop'
$exe = (Get-Item -LiteralPath $Executable).FullName
$runtime = (Get-Item -LiteralPath $RuntimeDirectory).FullName
$runId = 'd3d12-fg-extent-{0:yyyyMMdd-HHmmss}-{1}' -f (Get-Date), ([guid]::NewGuid().ToString('N').Substring(0, 8))
$logs = Join-Path ($ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($LogRoot)) $runId
New-Item -ItemType Directory -Force -Path $logs | Out-Null
Write-Output "D3D12 FG extent evidence directory: $logs"

$checks = @('v1_size=', 'extent_size=', 'partial_sizes_rejected=1', 'invalid_render_extents_rejected=1',
    'invalid_color_configs_rejected=1', 'invalid_native_state_rejected=1', 'render_extent_mismatch_rejected=1',
    'native_extent_mismatch_rejected=1', 'color_format_mismatch_rejected=1', 'fp16_frame_rejected=1',
    'native_color_format_mismatch_rejected=1', 'native_output_format_mismatch_rejected=1',
    'stale_depth_rejected=1', 'early_retire_rejected=1 pending_destroy_rejected=1', 'recreated_requires_reset=1')
$results = @()
$failed = $false
foreach ($color in $ColorModes) {
    foreach ($render in $RenderExtents) {
        $name = if ($render) { "$color-render-$render" } else { "$color-equal" }
        $caseLogs = Join-Path $logs $name
        New-Item -ItemType Directory -Path $caseLogs | Out-Null
        $stdout = Join-Path $caseLogs 'stdout.log'
        $stderr = Join-Path $caseLogs 'stderr.log'
        $arguments = @("`"$runtime`"", "`"$caseLogs`"", '-')
        if ($render) { $arguments += @('--render', $render) }
        # RGBA8 runs without --color, exactly as before the color option existed.
        if ($color -ne 'rgba8') { $arguments += @('--color', $color) }
        if ($DebugLayer) { $arguments += '--debug-layer' }
        $run = Start-Process -FilePath $exe -ArgumentList $arguments -WorkingDirectory (Split-Path $exe) -WindowStyle Hidden `
            -PassThru -RedirectStandardOutput $stdout -RedirectStandardError $stderr
        # Windows PowerShell 5.1 reports a null ExitCode unless the process handle is opened before the process exits.
        $null = $run.Handle
        if (-not $run.WaitForExit(600000)) { $run.Kill(); $run.WaitForExit(); throw "Timeout: $name" }
        $run.WaitForExit()
        if ($null -eq $run.ExitCode) { throw "Exit code unavailable: $name" }
        $text = [string](Get-Content -LiteralPath $stdout -Raw)
        $missing = @($checks | Where-Object { $text -notmatch [regex]::Escape("ABI_CHECK $_") })
        $mode = [regex]::Match($text, 'SDK_FG_COLOR mode=(\S+) format=(\d+) dxgi=(\d+) hdr10=(\d) device_support=(\d)')
        $summary = [regex]::Match($text, 'SDK_FG_SUMMARY real_frames=(\d+) verified=(\d+) final_lease_releases=(\d+) max_edge_error=(-?\d+)')
        $recreated = [regex]::Match($text, 'SDK_FG_RECREATED render=(\S+) real_frames=(\d+) verified=(\d+) final_lease_releases=(\d+) max_edge_error=(-?\d+)')
        $debug = [regex]::Match($text, 'SDK_FG_DEBUG corruption=(\d+) error=(\d+) warning=(\d+) info=(\d+) message=(\d+) discarded=(\d+)')
        $row = [ordered]@{
            name = $name
            color = $color
            render = if ($render) { $render } else { 'equal' }
            debug_layer = [bool]$DebugLayer
            exit_code = $run.ExitCode
            stderr_bytes = (Get-Item -LiteralPath $stderr).Length
            dxgi_format = if ($mode.Success) { [int]$mode.Groups[3].Value } else { $null }
            device_support = if ($mode.Success) { [int]$mode.Groups[5].Value } else { $null }
            missing_checks = $missing
            verified = if ($summary.Success) { [int]$summary.Groups[2].Value } else { $null }
            lease_releases = if ($summary.Success) { [int]$summary.Groups[3].Value } else { $null }
            max_edge_error = if ($summary.Success) { [int]$summary.Groups[4].Value } else { $null }
            recreated_render = if ($recreated.Success) { $recreated.Groups[1].Value } else { $null }
            recreated_verified = if ($recreated.Success) { [int]$recreated.Groups[3].Value } else { $null }
            recreated_lease_releases = if ($recreated.Success) { [int]$recreated.Groups[4].Value } else { $null }
            recreated_max_edge_error = if ($recreated.Success) { [int]$recreated.Groups[5].Value } else { $null }
            debug_corruption = if ($debug.Success) { [int]$debug.Groups[1].Value } else { $null }
            debug_errors = if ($debug.Success) { [int]$debug.Groups[2].Value } else { $null }
            debug_warnings = if ($debug.Success) { [int]$debug.Groups[3].Value } else { $null }
            debug_info = if ($debug.Success) { [int]$debug.Groups[4].Value } else { $null }
            debug_discarded = if ($debug.Success) { [int64]$debug.Groups[6].Value } else { $null }
        }
        # With the debug layer, a case also needs its message summary and no corruption or error.
        $validated = -not $DebugLayer -or ($debug.Success -and $row.debug_corruption -eq 0 -and
            $row.debug_errors -eq 0 -and $row.debug_discarded -eq 0)
        # Raw texels of the sixth frame's generated image in the run's color format (four bytes each).
        $readback = Join-Path $caseLogs 'generated-5.rgba'
        if (Test-Path -LiteralPath $readback) {
            if ((Get-Item -LiteralPath $readback).Length -ne 1280 * 720 * 4) { throw "Unexpected readback size: $readback" }
            $row.readback_sha256 = (Get-FileHash -LiteralPath $readback -Algorithm SHA256).Hash
        }
        if ($run.ExitCode -eq 3 -and $row.device_support -eq 0 -and $text -match 'ABI_CHECK unsupported_color_rejected=1') {
            $ok = $row.stderr_bytes -eq 0 -and $validated
            $row.verdict = if ($ok) { 'unsupported-rejected' } else { 'fail' }
        } else {
            $ok = $run.ExitCode -eq 0 -and $row.stderr_bytes -eq 0 -and $missing.Count -eq 0 -and
                $row.verified -eq 5 -and $row.lease_releases -eq 3 -and $row.recreated_verified -eq 2 -and
                $row.recreated_lease_releases -eq 3 -and $validated
            $row.verdict = if ($ok) { 'pass' } else { 'fail' }
        }
        if (-not $ok) { $failed = $true; Write-Warning "$name failed. See $stdout" }
        $results += [pscustomobject]$row
        $messages = if ($DebugLayer) { ", debug corruption/error/warning {0}/{1}/{2}" -f $row.debug_corruption, $row.debug_errors, $row.debug_warnings } else { '' }
        Write-Output ("{0}: {1} (exit {2}, max edge error {3} px{4})" -f $name, $row.verdict, $run.ExitCode, $row.max_edge_error, $messages)
    }
}

$gpu = Get-CimInstance Win32_VideoController | Where-Object { $_.AdapterCompatibility -match 'NVIDIA' } | Select-Object -First 1
$report = [ordered]@{
    run_id = $runId
    executable = $exe
    runtime_directory = $runtime
    gpu = if ($gpu) { $gpu.Name } else { $null }
    driver = if ($gpu) { $gpu.DriverVersion } else { $null }
    os = [Environment]::OSVersion.VersionString
    cases = $results
}
$report | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $logs 'd3d12-fg-extent-results.json') -Encoding utf8
if ($failed) { exit 1 }
exit 0
