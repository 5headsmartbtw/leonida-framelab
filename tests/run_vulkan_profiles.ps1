# SPDX-License-Identifier: MIT
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Executable,
    [Parameter(Mandatory)][string]$LogRoot,
    [ValidateSet('benchmark','soak')][string]$Mode,
    [ValidateRange(2,1000000)][int]$Frames,
    [string[]]$Profiles,
    [string]$MonitorDevice='primary'
)
$ErrorActionPreference='Stop'
if(-not $Profiles){$Profiles=if($Mode -eq 'benchmark'){@('native','sr','fg','combined')}else{@('sr','fg','combined')}}
$exe=(Get-Item -LiteralPath $Executable).FullName
$root=$ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($LogRoot)
$priorLog=$env:FRAMELAB_LOG_DIR
$priorSync=$env:VK_VALIDATION_VALIDATE_SYNC
$priorCadence=$env:FRAMELAB_FG_READBACK_EVERY
$modes=@{native=@('--benchmark-native');sr=@('--verify-sdk-sr');fg=@('--verify-direct-fg');combined=@('--verify-sdk-combined')}
$failed=$false
$profileDisplay=$null
try {
    $env:VK_VALIDATION_VALIDATE_SYNC=$null
    $env:FRAMELAB_FG_READBACK_EVERY=if($Mode -eq 'soak'){'100'}else{$null}
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
            if($Mode -eq 'benchmark'){$options+=@('--benchmark','--present-monitor',$MonitorDevice)}
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
                if($Mode -eq 'benchmark') {
                    $display=Select-String -LiteralPath $stdout -Pattern 'FRAME_METRICS .*refresh_hz=(\d+).* monitor=(\S+)' | Select-Object -First 1
                    if(!$display){throw "$name did not report a verified display"}
                    $signature=$display.Matches[0].Groups[1].Value+' '+$display.Matches[0].Groups[2].Value
                    if($profileDisplay -and $profileDisplay -ne $signature){throw "$name used a different display or refresh rate"}
                    $profileDisplay=$signature
                }
                if($Mode -eq 'soak') {
                    $runtimeLog=Join-Path $dir.FullName 'streamline/sl.log'
                    $runtimeFailure=(Test-Path -LiteralPath $runtimeLog) -and
                        (Select-String -LiteralPath $runtimeLog -Pattern 'Exception detected|leaked objects that have not been destroyed' -Quiet)
                    if($runtimeFailure -or (Get-Item -LiteralPath $stderr).Length -ne 0 -or
                       -not (Select-String -LiteralPath $stdout -Pattern 'SAMPLE_VALIDATION_SUMMARY errors=0 warnings=0' -Quiet)){
                        $failed=$true
                    }
                }
            } finally {
                if(-not $p.HasExited){$p.Kill();$p.WaitForExit()}
            }
        }
    }
} finally {
    $env:FRAMELAB_LOG_DIR=$priorLog
    $env:VK_VALIDATION_VALIDATE_SYNC=$priorSync
    $env:FRAMELAB_FG_READBACK_EVERY=$priorCadence
}
if($failed){throw "At least one $Mode failed. See $root"}
