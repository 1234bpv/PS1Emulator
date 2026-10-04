param([string]$Label,[string]$Kind,[string]$Address,[string]$GameKey,[switch]$NoUpload)
$ErrorActionPreference='Stop'
. "$PSScriptRoot\invoke-net.ps1"
$game=@(Get-Content "$PSScriptRoot\games.json" -Raw | ConvertFrom-Json | Where-Object key -eq $GameKey)[0]
$link=@("-$Kind",$Address)
$name="$Label-$GameKey-verify"
$settings=(Get-Content "$PSScriptRoot\stock-template.txt" -Raw).Replace('Castlevania - Symphony of the Night (USA.pbp',$game.image)
$settings | Set-Content "$PSScriptRoot\stock-$GameKey.txt" -Encoding ascii
Invoke-BoundedNet -Name "$name-settings" -NetArgs ($link+@('-PutFile',"$PSScriptRoot\stock-$GameKey.txt",'-Remote','E:\PS1\cfg\ps1_settings.txt')) -LimitSec 30
if(-not $NoUpload) {
 Invoke-BoundedNet -Name "$name-put-state" -NetArgs ($link+@('-PutFile',"$PSScriptRoot\states\$($game.state)",'-Remote','E:\PS1\saves\bench.sav')) -LimitSec 240
 $mark=Get-Content "$PSScriptRoot\$name-put-state.done.json" -Raw | ConvertFrom-Json
 if($mark.timeout -or $mark.exitCode -ne 0) {throw 'state upload incomplete'}
}
@"
LOG $name gameplay verification release alpha13
-START E:\sys\bin\ps1stage1.exe gui light blend dsa img"$($game.image)" loadstate$($game.frame) frames20000 dumpat999999 limit90
-WAITFG e0001002 20000
-KEY star press 2 100
SLEEP 25000
-SHOT E:\tester\$name.bmp 2
-ENDAPP 0xE0001002
SLEEP 2000
-KILL ps1stage1
PS ps1stage1
"@ | Set-Content "$PSScriptRoot\$name.txt" -Encoding ascii
Invoke-BoundedNet -Name "$name-scenario" -NetArgs ($link+@('-Scenario',"$PSScriptRoot\$name.txt",'-ScenarioTimeoutSec','110','-OutFile',"$PSScriptRoot\$name.log")) -LimitSec 140
foreach($item in @(@("E:\tester\$name.bmp","$name.bmp"),@('E:\PS1\logs\ps1_stage1.jsonl',"$name.jsonl"))) {
 for($attempt=1;$attempt -le 3;$attempt++) {
  $markName="$name-get-$($item[1])-a$attempt"
  Invoke-BoundedNet -Name $markName -NetArgs ($link+@('-GetFile',$item[0],'-OutFile',"$PSScriptRoot\$($item[1])")) -LimitSec 45
  $mark=Get-Content "$PSScriptRoot\$markName.done.json" -Raw | ConvertFrom-Json
  if(-not $mark.timeout -and $mark.exitCode -eq 0) {break}
 }
 if($mark.timeout -or $mark.exitCode -ne 0) {throw "download incomplete after 3 attempts $($item[0])"}
}

