# SPDX-License-Identifier: MIT
# Runs the native Rust D3D12 consumers (samples/rust_d3d12) against an installed FrameLab, each in its
# own process: SR and FG without validation, SR and FG with the D3D12 debug layer and GPU-based
# validation, both with a missing library, and both with a FrameLab build that has no D3D12
# components (when -NoComponentLibrary is given). The fallback cases must exit 3 after rendering
# without FrameLab. Writes rust-d3d12-results.json with the FG readback hash.
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$TargetDirectory,
    [Parameter(Mandatory)][string]$Library,
    [Parameter(Mandatory)][string]$RuntimeDirectory,
    [string]$NoComponentLibrary,
    [string]$LogRoot = (Join-Path (Get-Location) 'framelab-logs')
)
$ErrorActionPreference = 'Stop'
$target = (Get-Item -LiteralPath $TargetDirectory).FullName
$library = (Get-Item -LiteralPath $Library).FullName
$runtime = (Get-Item -LiteralPath $RuntimeDirectory).FullName
$runId = 'rust-d3d12-{0:yyyyMMdd-HHmmss}-{1}' -f (Get-Date), ([guid]::NewGuid().ToString('N').Substring(0, 8))
$logs = Join-Path ($ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($LogRoot)) $runId
New-Item -ItemType Directory -Force -Path $logs | Out-Null
Write-Output "Rust D3D12 evidence directory: $logs"

$missing = Join-Path (Split-Path $library) 'missing\FrameLab.dll'
$cases = @(
    @{ name = 'sr'; exe = 'framelab-rust-sr'; library = $library; debug = $false; expect = 0 },
    @{ name = 'fg'; exe = 'framelab-rust-fg'; library = $library; debug = $false; expect = 0 },
    @{ name = 'sr-debug-layer'; exe = 'framelab-rust-sr'; library = $library; debug = $true; expect = 0 },
    @{ name = 'fg-debug-layer'; exe = 'framelab-rust-fg'; library = $library; debug = $true; expect = 0 },
    @{ name = 'sr-missing-library'; exe = 'framelab-rust-sr'; library = $missing; debug = $false; expect = 3; reason = 'library' },
    @{ name = 'fg-missing-library'; exe = 'framelab-rust-fg'; library = $missing; debug = $false; expect = 3; reason = 'library' }
)
if ($NoComponentLibrary) {
    $plain = (Get-Item -LiteralPath $NoComponentLibrary).FullName
    $cases += @{ name = 'sr-no-component'; exe = 'framelab-rust-sr'; library = $plain; debug = $false; expect = 3; reason = 'd3d12-sr' }
    $cases += @{ name = 'fg-no-component'; exe = 'framelab-rust-fg'; library = $plain; debug = $false; expect = 3; reason = 'd3d12-fg' }
}
$required = @{
    'framelab-rust-sr' = @('RUST_SR_CONFIGURED render=', 'RUST_SR_VERIFIED frame=0 ', 'RUST_SR_VERIFIED frame=1 ',
        'RUST_SR_FALLBACK frame=2 rejected=missing_depth ordinary_verified=1', 'RUST_SR_VERIFIED frame=3 ',
        'RUST_SR_CANCEL frame=4 releases=4 submitted=0', 'RUST_SR_VERIFIED frame=5 ',
        'RUST_SR_GATED frame=5 live_before_fence=4 releases_after_retire=4 live_after=0',
        'RUST_SR_SUMMARY evaluations=4 verified=4 fallback=1 cancelled=1 resized=1 presents=0')
    'framelab-rust-fg' = @('RUST_FG_CHECK invalid_native_state_rejected=1', 'RUST_FG_CHECK stale_depth_rejected=1',
        'RUST_FG_CHECK early_retire_rejected=1 pending_destroy_rejected=1 live_before_fence=3',
        'RUST_FG_SUMMARY real_frames=6 verified=5 final_lease_releases=3')
}
$results = @()
$failed = $false
foreach ($case in $cases) {
    $caseLogs = Join-Path $logs $case.name
    New-Item -ItemType Directory -Path $caseLogs | Out-Null
    $stdout = Join-Path $caseLogs 'stdout.log'
    $stderr = Join-Path $caseLogs 'stderr.log'
    $exe = Join-Path $target "$($case.exe).exe"
    $arguments = @("`"$($case.library)`"", "`"$runtime`"", "`"$caseLogs`"")
    if ($case.debug) { $arguments += '--debug-layer' }
    $run = Start-Process -FilePath $exe -ArgumentList $arguments -WorkingDirectory $caseLogs -WindowStyle Hidden `
        -PassThru -RedirectStandardOutput $stdout -RedirectStandardError $stderr
    # Windows PowerShell 5.1 reports a null ExitCode unless the process handle is opened before the process exits.
    $null = $run.Handle
    if (-not $run.WaitForExit(600000)) { $run.Kill(); $run.WaitForExit(); throw "Timeout: $($case.name)" }
    $run.WaitForExit()
    $text = [string](Get-Content -LiteralPath $stdout -Raw)
    $row = [ordered]@{
        name = $case.name
        executable = $case.exe
        library = $case.library
        debug_layer = $case.debug
        exit_code = $run.ExitCode
        stderr_bytes = (Get-Item -LiteralPath $stderr).Length
    }
    if ($case.expect -eq 0) {
        $row.missing_lines = @($required[$case.exe] | Where-Object { $text -notmatch [regex]::Escape($_) })
        $fg = [regex]::Match($text, 'RUST_FG_SUMMARY real_frames=(\d+) verified=(\d+) final_lease_releases=(\d+) max_edge_error=(-?\d+)')
        if ($fg.Success) { $row.fg_verified = [int]$fg.Groups[2].Value; $row.fg_max_edge_error = [int]$fg.Groups[4].Value }
        $debug = [regex]::Match($text, 'RUST_D3D12_DEBUG corruption=(\d+) error=(\d+) warning=(\d+) info=(\d+) message=(\d+) discarded=(\d+)')
        if ($debug.Success) {
            $row.debug = "corruption=$($debug.Groups[1].Value) error=$($debug.Groups[2].Value) warning=$($debug.Groups[3].Value) info=$($debug.Groups[4].Value) message=$($debug.Groups[5].Value) discarded=$($debug.Groups[6].Value)"
        }
        $readback = Join-Path $caseLogs 'generated-5.rgba'
        if (Test-Path -LiteralPath $readback) { $row.readback_sha256 = (Get-FileHash -LiteralPath $readback -Algorithm SHA256).Hash }
        $ok = $run.ExitCode -eq 0 -and $row.stderr_bytes -eq 0 -and $row.missing_lines.Count -eq 0 -and
            (-not $case.debug -or $debug.Success)
    } else {
        $ordinary = if ($case.exe -eq 'framelab-rust-sr') { 'RUST_SR_ORDINARY frames=1 verified=1' } else { 'RUST_FG_ORDINARY frames=1 verified=1' }
        $row.fallback_reason = [regex]::Match($text, 'FRAMELAB_UNAVAILABLE component=(\S+) reason=(.*)').Groups[2].Value.Trim()
        $ok = $run.ExitCode -eq 3 -and $row.stderr_bytes -eq 0 -and $text -match [regex]::Escape("FRAMELAB_UNAVAILABLE component=$($case.reason) ") -and
            $text -match [regex]::Escape($ordinary)
    }
    $row.verdict = if ($ok) { if ($case.expect -eq 0) { 'pass' } else { 'fallback-pass' } } else { 'fail' }
    if (-not $ok) { $failed = $true; Write-Warning "$($case.name) failed. See $stdout" }
    $results += [pscustomobject]$row
    Write-Output ("{0}: {1} (exit {2})" -f $case.name, $row.verdict, $run.ExitCode)
}

$gpu = Get-CimInstance Win32_VideoController | Where-Object { $_.AdapterCompatibility -match 'NVIDIA' } | Select-Object -First 1
[ordered]@{
    run_id = $runId
    target_directory = $target
    runtime_directory = $runtime
    gpu = if ($gpu) { $gpu.Name } else { $null }
    driver = if ($gpu) { $gpu.DriverVersion } else { $null }
    os = [Environment]::OSVersion.VersionString
    cases = $results
} | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $logs 'rust-d3d12-results.json') -Encoding utf8
if ($failed) { exit 1 }
exit 0
