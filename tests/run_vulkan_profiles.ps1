# SPDX-License-Identifier: MIT
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Executable,
    [Parameter(Mandatory)][string]$LogRoot,
    [ValidateSet('benchmark','soak')][string]$Mode,
    [ValidateRange(2,1000000)][int]$Frames,
    [string[]]$Profiles
)
$ErrorActionPreference='Stop'
if(-not $Profiles){$Profiles=if($Mode -eq 'benchmark'){@('native','sr','fg','combined')}else{@('sr','fg','combined')}}
$exe=(Get-Item -LiteralPath $Executable).FullName
$root=$ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($LogRoot)
$priorLog=$env:FRAMELAB_LOG_DIR
$priorSync=$env:VK_VALIDATION_VALIDATE_SYNC
$modes=@{native=@('--benchmark-native');sr=@('--verify-sdk-sr');fg=@('--verify-direct-fg');combined=@('--verify-sdk-combined')}
$failed=$false
try {
    $env:VK_VALIDATION_VALIDATE_SYNC=$null
    foreach($profile in $Profiles) {
        if(-not $modes.ContainsKey($profile)){throw "Unknown profile $profile"}
        $vsyncValues=if($Mode -eq 'benchmark'){@('off','on')}else{@('off')}
        foreach($vsync in $vsyncValues) {
            $name="$profile-$vsync"
            $dir=New-Item -ItemType Directory -Force -Path (Join-Path $root $name)
            $env:FRAMELAB_LOG_DIR=$dir.FullName
            $csv=Join-Path $dir.FullName 'frames.csv'
            $stdout=Join-Path $dir.FullName 'stdout.log'
            $stderr=Join-Path $dir.FullName 'stderr.log'
            $syncFlag=if($vsync -eq 'on'){'--vsync'}else{'--no-vsync'}
            $options=$modes[$profile]+@($syncFlag,'--frames',"$Frames",'--metrics',('"{0}"' -f $csv))
            if($Mode -eq 'benchmark'){$options+='--benchmark'}
            else{$options+=@('--validation','--resize-every',"$([Math]::Max(2,[Math]::Floor($Frames/4)))")}
            $p=Start-Process -FilePath $exe -ArgumentList $options -WorkingDirectory (Split-Path $exe) `
                -WindowStyle Hidden -PassThru -RedirectStandardOutput $stdout -RedirectStandardError $stderr
            try {
                $clock=[Diagnostics.Stopwatch]::StartNew()
                Write-Output "START $Mode $name frames=$Frames"
                while(-not $p.WaitForExit(30000)) {
                    if($clock.Elapsed.TotalMinutes -ge 60){throw "$name exceeded 60 minutes"}
                    if(Test-Path $csv){Write-Output "$name last frame $((Get-Content -LiteralPath $csv -Tail 1).Split(',')[0])"}
                }
                $p.WaitForExit()
                Write-Output "END $Mode $name exit=$($p.ExitCode) elapsed=$([Math]::Round($clock.Elapsed.TotalSeconds,1))s"
                if($p.ExitCode -ne 0){$failed=$true}
                if($Mode -eq 'soak' -and -not (Select-String $stdout 'SAMPLE_VALIDATION_SUMMARY errors=0 warnings=0' -Quiet)){
                    $failed=$true
                }
            } finally {
                if(-not $p.HasExited){$p.Kill();$p.WaitForExit()}
            }
        }
    }
} finally {
    $env:FRAMELAB_LOG_DIR=$priorLog
    $env:VK_VALIDATION_VALIDATE_SYNC=$priorSync
}
if($failed){throw "At least one $Mode failed. See $root"}
