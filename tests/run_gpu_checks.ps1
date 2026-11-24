# SPDX-License-Identifier: MIT
# Copyright (c) 2026 5headsmartbtw
param([string]$TaskRoot='E:\Dev\GTA6')
$ErrorActionPreference='Stop'
$exe=Join-Path $TaskRoot 'bin_x64\Release\vk_streamline.exe'
$logs=Join-Path $TaskRoot 'logs\fg\phase3'
New-Item -ItemType Directory -Force -Path $logs | Out-Null
$cases=@(
    @{name='off';args=@('--verify-direct-fg','--frames','6','--validation','--no-vsync');evaluations=6;generated=5;rejected=0;mode='Immediate'},
    @{name='on';args=@('--verify-direct-fg','--frames','6','--validation','--vsync');evaluations=6;generated=5;rejected=0;mode='FIFO'},
    @{name='missing-depth';args=@('--verify-direct-fg','--frames','6','--validation','--no-vsync','--test-missing-depth-frame','2');evaluations=5;generated=3;rejected=1;mode='Immediate'}
)
$results=@()
foreach($case in $cases){
    $stdout=Join-Path $logs ($case.name+'.log');$stderr=Join-Path $logs ($case.name+'-error.log')
    $run=Start-Process -FilePath $exe -ArgumentList $case.args -WorkingDirectory (Split-Path $exe) -WindowStyle Hidden -PassThru -RedirectStandardOutput $stdout -RedirectStandardError $stderr
    if(-not $run.WaitForExit(60000)){$run.Kill();$run.WaitForExit();throw "Timeout: $($case.name)"}
    $run.WaitForExit() # Drain redirected output after the bounded process wait.
    $text=Get-Content -LiteralPath $stdout -Raw
    for($attempt=0;$attempt -lt 20 -and -not $text.Contains('SAMPLE_VALIDATION_SUMMARY');++$attempt){
        Start-Sleep -Milliseconds 50
        $text=Get-Content -LiteralPath $stdout -Raw
    }
    if($run.ExitCode -ne 0){throw "Nonzero exit: $($case.name). See $stdout"}
    if((Get-Item -LiteralPath $stderr).Length -ne 0){throw "Validation stderr was not empty: $stderr"}
    if(-not $text.Contains('SAMPLE_VALIDATION_SUMMARY errors=0 warnings=0')){throw "Validation failed: $stdout"}
    if(-not $text.Contains("evaluations=$($case.evaluations) verified=$($case.generated) real_presents=6 generated_presents=$($case.generated) errors=0")){throw "Unexpected frame counters: $stdout"}
    if(-not $text.Contains("VERIFY_FRAMEINPUTS rejected=$($case.rejected) expected_rejections=$($case.rejected) in_flight=0")){throw "Unexpected contract result: $stdout"}
    if(-not $text.Contains("present_mode=$($case.mode)")){throw "Unexpected presentation mode: $stdout"}
    if($case.name -eq 'missing-depth'){
        if(-not $text.Contains('FRAMEINPUTS_REJECTED source_frame=2')){throw 'Missing-depth input was not rejected'}
        if($text.Contains('FRAMEINPUTS_CONSUMED source_frame=2 ')){throw 'Missing-depth frame incorrectly reached NGX'}
        if(-not $text.Contains('FRAMEINPUTS_CONSUMED source_frame=3 generation=1 reset=1')){throw 'No history reset after fallback'}
    }
    $row=[ordered]@{name=$case.name;exit_code=$run.ExitCode;evaluations=$case.evaluations;verified=$case.generated;real_presents=6;generated_presents=$case.generated;input_rejections=$case.rejected;validation_errors=0;validation_warnings=0;stderr_bytes=0}
    if($case.name -eq 'off'){
        $baseline=Join-Path $TaskRoot 'phase3\baseline\generated-5.rgba16f'
        $current=Join-Path $TaskRoot 'logs\fg\sample-direct\generated-5.rgba16f'
        $originalHash=(Get-FileHash -LiteralPath $baseline -Algorithm SHA256).Hash
        $currentHash=(Get-FileHash -LiteralPath $current -Algorithm SHA256).Hash
        $row.baseline_sha256=$originalHash;$row.frameinputs_sha256=$currentHash;$row.baseline_identical=$originalHash -eq $currentHash
        if(-not $row.baseline_identical){throw "Readback differs from preserved baseline: $currentHash vs $originalHash"}
    }
    $results+=$row
    $results | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $logs 'gpu-results.json') -Encoding utf8
    Write-Output "PASS $($case.name): Evaluate=$($case.evaluations), verified=$($case.generated), rejected=$($case.rejected), validation=0/0"
}
$results | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $logs 'gpu-results.json') -Encoding utf8
