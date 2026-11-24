# SPDX-License-Identifier: MIT
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$InstalledExecutable,
    [Parameter(Mandatory)][string]$LegacyExecutable,
    [string]$LogRoot = (Join-Path (Get-Location) 'framelab-ngx-trace')
)
$ErrorActionPreference='Stop'
$root=$ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($LogRoot)
$previousLog=$env:FRAMELAB_LOG_DIR
$previousTrace=$env:FRAMELAB_TRACE_NGX_FG
$previousSync=$env:VK_VALIDATION_VALIDATE_SYNC
$previousLimit=$env:VK_LAYER_DUPLICATE_MESSAGE_LIMIT
$failed=$false
try {
    $env:FRAMELAB_TRACE_NGX_FG='1'
    $env:VK_VALIDATION_VALIDATE_SYNC='1'
    $env:VK_LAYER_DUPLICATE_MESSAGE_LIMIT='10000'
    foreach($variant in @(@{name='installed';exe=$InstalledExecutable},@{name='legacy';exe=$LegacyExecutable})) {
        $exe=(Get-Item -LiteralPath $variant.exe).FullName
        $logs=New-Item -ItemType Directory -Force -Path (Join-Path $root $variant.name)
        $env:FRAMELAB_LOG_DIR=$logs.FullName
        $stdout=Join-Path $logs.FullName 'stdout.log'
        $stderr=Join-Path $logs.FullName 'stderr.log'
        $process=Start-Process -FilePath $exe -WorkingDirectory (Split-Path $exe) -WindowStyle Hidden -PassThru `
            -ArgumentList @('--validation','--no-vsync','--frames','2') -RedirectStandardOutput $stdout -RedirectStandardError $stderr
        if(-not $process.WaitForExit(60000)) {
            $process.Kill()
            $process.WaitForExit()
            throw "NGX trace timed out: $($variant.name)"
        }
        $process.WaitForExit()
        $inCall=$false; $calls=0; $inside=0; $outside=0
        foreach($line in Get-Content -LiteralPath $stdout) {
            if($line.StartsWith('NGX_FG_NATIVE_BEGIN ')) { $inCall=$true; ++$calls }
            if($line.StartsWith('NGX_FG_NATIVE_END ')) { $inCall=$false }
            if($line.StartsWith('SAMPLE_VALIDATION severity=')) {
                if($inCall) { ++$inside } else { ++$outside }
            }
            if($line.StartsWith('SAMPLE_VALIDATION_SUMMARY ')) { Write-Output $line }
        }
        if($calls -ne 2 -or $inCall) { throw "Incomplete NGX trace: $stdout" }
        Write-Output "$($variant.name): calls=$calls inside=$inside outside=$outside exit=$($process.ExitCode)"
        if($process.ExitCode -ne 0 -or $inside -ne 0 -or $outside -ne 0 -or (Get-Item $stderr).Length -ne 0) {
            $failed=$true
        }
    }
} finally {
    $env:FRAMELAB_LOG_DIR=$previousLog
    $env:FRAMELAB_TRACE_NGX_FG=$previousTrace
    $env:VK_VALIDATION_VALIDATE_SYNC=$previousSync
    $env:VK_LAYER_DUPLICATE_MESSAGE_LIMIT=$previousLimit
}
if($failed) { throw "NGX trace failed validation; see $root" }
