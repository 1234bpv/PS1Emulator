param([int]$NextId,[string]$Address='192.168.3.22')
$ErrorActionPreference='Stop'
. "$PSScriptRoot/invoke-net.ps1"
& "$PSScriptRoot/audit-final.ps1" -RunsOnly -UserOverclock
$summary=Get-Content "$PSScriptRoot/summary-progress.json" -Raw | ConvertFrom-Json
if($summary.acceptedRuns -ne 108 -or $summary.completedTriples -ne 36){throw 'All measurement triples must finish first'}
& "$PSScriptRoot/restore-device.ps1" -Device n95 -Kind Ip -Address $Address -AuditTag final2
foreach($download in @(@('E:\sys\bin\ps1stage1.exe','final2-n95-exe.exe'),@('C:\Data\Overclock\boot.dat','useroc-boot-after.dat'))){
 for($attempt=1;$attempt -le 3;$attempt++){
  $tag="useroc-final-get-$($download[1])-a$attempt"
  Invoke-BoundedNet -Name $tag -NetArgs @('-Ip',$Address,'-GetFile',$download[0],'-OutFile',"$PSScriptRoot/$($download[1])") -LimitSec 45 | Out-Null
  $done=Get-Content "$PSScriptRoot/$tag.done.json" -Raw | ConvertFrom-Json
  if(-not $done.timeout -and $done.exitCode -eq 0){break}
 }
 if($done.timeout -or $done.exitCode -ne 0){throw "Final download incomplete $($download[0])"}
}
& "$PSScriptRoot/read-user-clock.ps1" -Name N95-useroc-final -NextId $NextId -Address $Address -Baseline "$PSScriptRoot/user-oc-baseline.json"
& "$PSScriptRoot/audit-final.ps1" -UserOverclock
& "$PSScriptRoot/write-user-report.ps1" -Final
$authorization=Get-Content "$PSScriptRoot/user-overclock-authorization.json" -Raw | ConvertFrom-Json
$authorization.status='complete-user-overclock-testing'
$authorization | Add-Member completedAt (Get-Date).ToString('o') -Force
$authorization | Add-Member audio 'sound=1, volume step=0 before measurement, snd_open rc0 and positive feeds/samples verified for every run' -Force
$authorization | ConvertTo-Json -Depth 5 | Set-Content "$PSScriptRoot/user-overclock-authorization.json" -Encoding utf8
Add-Content "$PSScriptRoot/../../../work/state.md" ((Get-Date -Format 'yyyy-MM-dd HH:mm')+': F11 completed with user-installed m111 CORE04360644 DSP000023C3. 108 valid runs/36 triples/12 canonical gameplay states. All runs sound ON, volume0 before clocks; positive audio feeds/samples. C5 14/14 and N95 18/18 originals restored with SHA readback; EXE release hash and boot.dat unchanged verified. User clock left unchanged, no emulator source changes, no push. Final evidence audit passed.')
& "$PSScriptRoot/export-current.ps1" -FinalUserOverclock
Write-Output 'User overclock measurements finalized; commit remains.'
