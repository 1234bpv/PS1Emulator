param([string]$Label,[string]$Kind,[string]$Address,[int]$NextId,[string]$Image='gt.pbp',[int]$StateFrame=7700,[int]$Runs=3,[int]$FirstRepeat=1,[int]$WaitMs=100000,[switch]$AdaptiveWait)
$ErrorActionPreference='Stop'
if($Label -like 'N95-useroc-*'){$AdaptiveWait=$true}
. "$PSScriptRoot\invoke-net.ps1"
$link=@("-$Kind",$Address)
for($repeat=$FirstRepeat;$repeat -lt $FirstRepeat+$Runs;$repeat++) {
  $name="$Label-r$repeat"
  $scene=Join-Path $PSScriptRoot "$name.txt"
  $end=$StateFrame+1200
  $mute=if($Label -like 'N95-useroc-*'){
   '-KEY star press 2 0'
  }else{'-KEY star press 2 100'}
  @"
LOG $name 1200 PS1 frames stock release volume zero
-START E:\sys\bin\ps1stage1.exe gui light blend dsa img"$Image" loadstate$StateFrame frames$end dumpat999999 limit150
-WAITFG e0001002 20000
$mute
SLEEP $WaitMs
-ENDAPP 0xE0001002
-KILL ps1stage1
PS ps1stage1
"@ | Set-Content -LiteralPath $scene -Encoding ascii
  Invoke-BoundedNet -Name "$name-scenario" -NetArgs ($link+@('-Scenario',$scene,'-ScenarioTimeoutSec','200','-OutFile',"$PSScriptRoot\$name.log")) -LimitSec 220
  $done=Get-Content "$PSScriptRoot\$name-scenario.done.json" -Raw | ConvertFrom-Json
  if($done.timeout -or $done.exitCode -ne 0) { throw "scenario client incomplete $name" }
  $trace=Get-Content "$PSScriptRoot\$name.log" -Raw
  if($trace -notmatch "--- DONE") { throw "scenario did not finish $name" }
  if($trace -notmatch "OK proc $NextId\r?\n") { throw "process ID differs $name" }
  for($getAttempt=1;$getAttempt -le 3;$getAttempt++) {
    $getName="$name-jsonl-a$getAttempt"
    Invoke-BoundedNet -Name $getName -NetArgs ($link+@('-GetFile','E:\PS1\logs\ps1_stage1.jsonl','-OutFile',"$PSScriptRoot\$name-a$getAttempt.jsonl")) -LimitSec 45
    $done=Get-Content "$PSScriptRoot\$getName.done.json" -Raw | ConvertFrom-Json
    if(-not $done.timeout -and $done.exitCode -eq 0) {Copy-Item "$PSScriptRoot\$name-a$getAttempt.jsonl" "$PSScriptRoot\$name.jsonl"; break}
  }
  if($done.timeout -or $done.exitCode -ne 0) { throw "log download incomplete after 3 attempts $name" }
  $events=@(Get-Content "$PSScriptRoot\$name.jsonl" | ForEach-Object { $_ | ConvertFrom-Json })
  $shutdown=@($events | Where-Object event -eq 'shutdown')[-1]
  $pass=@($events | Where-Object event -eq 'pass')[-1]
  $errors=@($events | Where-Object error)
  $zero=@($events | Where-Object { $_.event -eq 'vol' -and $_.msg -eq 'step=0' })[-1]
  $start=@($events | Where-Object event -eq 'clocks')[0]
  $dsa=@($events | Where-Object { $_.event -eq 'dsa' -and $_.msg -match 'rows=' })[-1]
  $loaded=@($events | Where-Object { $_.msg -like 'Loaded CD Image:*' })[-1]
  $expectedGame=@(Get-Content "$PSScriptRoot/games.json" -Raw | ConvertFrom-Json | Where-Object image -eq $Image)
  if($expectedGame.Count -ne 1 -or $StateFrame -ne $expectedGame[0].frame){throw "Image/state arguments differ from canonical scene $name"}
  if($loaded.msg -ne "Loaded CD Image: E:\PS1\games\$Image[+pbp] ($($expectedGame[0].size) bytes).`n"){throw "Loaded image differs $name"}
  if(@($events | Where-Object {$_.event -eq 'loadstate' -and $_.msg -match "frame=$StateFrame rc=0"}).Count -ne 1){throw "Canonical state not loaded $name"}
  $reset=@($events | Where-Object event -eq 'reset')[-1]
  if($reset.msg -notmatch 'type=(NTSC|PAL)') {throw "region not recorded $name"}
  $region=$Matches[1]
  $nativeHz=if($region -eq 'PAL'){50}else{60}
  if($shutdown.msg -notmatch '^gui: frames=1200 ms=(\d+)$') { throw "incorrect frame count $name $($shutdown.msg)" }
  $elapsed=[int]$Matches[1]
  if(-not $zero -or -not $start -or $zero.ms -ge $start.ms -or $errors.Count -ne 0 -or @($events | Where-Object {$_.event -eq 'exit' -and $_.msg -eq 'gui returned 0'}).Count -ne 1) { throw "run invalid $name errors=$($errors.Count)" }
  if($dsa.msg -notmatch 'active=1 gl=0 fbPlatform=1 pixB=4.*rows=(\d+)/(\d+)/(\d+)/(\d+)/(\d+) covered=1 fbOk=1 rowCopy=0.*scr=320,240') { throw "cannot derive shown frames $name" }
  $rows=0L;1..5 | ForEach-Object { $rows += [long]$Matches[$_] }
  if($rows%240) { throw "incomplete rows $name" }
  if(@($events | Where-Object {$_.event -eq 'snd_open' -and $_.msg -match 'rc=0'}).Count -ne 1 -or @($events | Where-Object {$_.event -eq 'sndstats' -and $_.msg -match 'feeds=([1-9][0-9]*) samples=([1-9][0-9]*)'}).Count -ne 1){throw "Audio emulation absent $name"}
  $result=[ordered]@{label=$Label; repeat=$repeat; region=$region; frames=1200; ms=$elapsed; shownFrames=$rows/240; shownFps=($rows/240)*1000/$elapsed; ps1Fps=1200000/$elapsed; speedPercent=120000000/($elapsed*$nativeHz); errors=$errors.Count; shutdown=$shutdown.msg; pass=$pass.msg; loaded=$loaded.msg; emuMs=$null; shownMethod='DSA direct FB pixB=4, stock fxaa=0, 240 rows per present'; volume0ms=$zero.ms; clockStartMs=$start.ms}
  $result | ConvertTo-Json -Depth 3 | Set-Content "$PSScriptRoot\$name.result.json" -Encoding utf8
  $result | ConvertTo-Json -Compress | Write-Output
  if($AdaptiveWait){$WaitMs=[Math]::Min($WaitMs,[Math]::Max(35000,$elapsed+20000))}
  $NextId++
}





