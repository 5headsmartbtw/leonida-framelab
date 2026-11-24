# SPDX-License-Identifier: MIT
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Executable,
    [ValidateSet('combined','fifo','fallback','sr-only','fg-only')][string]$Scenario='combined',
    [string]$LogRoot=(Join-Path (Get-Location) 'framelab-combined-logs'),
    [switch]$SynchronizationValidation
)
$ErrorActionPreference='Stop'
$exe=(Get-Item -LiteralPath $Executable).FullName
$logs=New-Item -ItemType Directory -Force -Path $LogRoot
$out=Join-Path $logs.FullName 'stdout.log'
$err=Join-Path $logs.FullName 'stderr.log'
$mode=@('--verify-sdk-combined','--no-vsync')
switch($Scenario){
    'fifo' {$mode=@('--verify-sdk-combined','--vsync')}
    'fallback' {$mode+=@('--sdk-sr-reject-frame','2')}
    'sr-only' {$mode=@('--verify-sdk-sr')}
    'fg-only' {$mode=@('--verify-direct-fg','--no-vsync')}
}
$previousLog=$env:FRAMELAB_LOG_DIR
$previousSync=$env:VK_VALIDATION_VALIDATE_SYNC
try {
    $env:FRAMELAB_LOG_DIR=$logs.FullName
    if($SynchronizationValidation){$env:VK_VALIDATION_VALIDATE_SYNC='1'}
    $process=Start-Process -FilePath $exe -ArgumentList ($mode+@('--frames','6','--validation')) `
        -WorkingDirectory (Split-Path $exe) -WindowStyle Hidden -PassThru -RedirectStandardOutput $out -RedirectStandardError $err
} finally {
    $env:FRAMELAB_LOG_DIR=$previousLog
    $env:VK_VALIDATION_VALIDATE_SYNC=$previousSync
}
if(-not $process.WaitForExit(60000)){
    $process.Kill()
    $process.WaitForExit()
    throw "$Scenario timed out; see $out"
}
$process.WaitForExit()
$text=Get-Content $out -Raw
Select-String $out -Pattern 'VERIFY_PUBLIC_VULKAN|VERIFY_DIRECT_FG|SAMPLE_VALIDATION_SUMMARY' |
    ForEach-Object Line
if($process.ExitCode -ne 0 -or -not $text.Contains('SAMPLE_VALIDATION_SUMMARY errors=0 warnings=0') -or
   (Get-Item $err).Length -ne 0){throw "$Scenario validation failed; see $out"}

switch($Scenario){
    {$_ -in 'combined','fifo'} {
        if(-not $text.Contains('VERIFY_PUBLIC_VULKAN_COMBO sr_evaluations=6 mapped=6 fg_evaluations=6 generated=5 passed=1') -or
           -not $text.Contains('real_presents=6 generated_presents=5 errors=0')){throw "Missing combined GPU evidence: $out"}
    }
    'fallback' {
        if(-not $text.Contains('VERIFY_PUBLIC_VULKAN_COMBO sr_evaluations=5 mapped=5 fg_evaluations=5 generated=3 passed=1') -or
           -not $text.Contains('resets=2 rejected=1 passed=1') -or
           -not $text.Contains('real_presents=6 generated_presents=3 errors=0')){throw "Missing fallback evidence: $out"}
    }
    'sr-only' {
        if(-not $text.Contains('VERIFY_PUBLIC_VULKAN_SR readbacks=2 resets=1 rejected=0 passed=1')){
            throw "Missing SR-only evidence: $out"
        }
    }
    'fg-only' {
        if(-not $text.Contains('evaluations=6 verified=5 real_presents=6 generated_presents=5 errors=0')){
            throw "Missing FG-only evidence: $out"
        }
    }
}
Get-ChildItem -LiteralPath $logs.FullName -Recurse -Filter '*.rgba16f' |
    Get-FileHash -Algorithm SHA256 | Select-Object Hash,Path
