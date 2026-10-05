param([string]$Label,[string]$Kind,[string]$Address,[string]$GameKey,[int]$Attempt=1,[int]$LoadFrame=0,[int]$SaveFrame=5000,[string]$Recipe)
$ErrorActionPreference='Stop'
. "$PSScriptRoot\invoke-net.ps1"
if($Attempt -gt 10) {throw 'navigation attempt limit'}
$link=@("-$Kind",$Address)
$game=@(Get-Content "$PSScriptRoot\games.json" -Raw | ConvertFrom-Json | Where-Object key -eq $GameKey)[0]
$name="$Label-$GameKey-nav$Attempt"
$stock=(Get-Content "$PSScriptRoot\stock-template.txt" -Raw).Replace('Castlevania - Symphony of the Night (USA.pbp',$game.image)
$stock | Set-Content "$PSScriptRoot\stock-$GameKey.txt" -Encoding ascii
Invoke-BoundedNet -Name "$name-settings" -NetArgs ($link+@('-PutFile',"$PSScriptRoot\stock-$GameKey.txt",'-Remote','E:\PS1\cfg\ps1_settings.txt')) -LimitSec 30
$load=if($LoadFrame -gt 0){"loadstate$LoadFrame"}else{''}
$end=$SaveFrame+1
$steps=Get-Content $Recipe -Raw
@"
LOG $name navigation only; not a timing measurement
-START E:\sys\bin\ps1stage1.exe gui rec light blend dsa img"$($game.image)" $load savestate$SaveFrame frames$end dumpat999999 limit240
-WAITFG e0001002 20000
-KEY star press 2 100
$steps
-ENDAPP 0xE0001002
SLEEP 2000
-KILL ps1stage1
PS ps1stage1
"@ | Set-Content "$PSScriptRoot\$name.txt" -Encoding ascii
Invoke-BoundedNet -Name "$name-scenario" -NetArgs ($link+@('-Scenario',"$PSScriptRoot\$name.txt",'-ScenarioTimeoutSec','300','-OutFile',"$PSScriptRoot\$name.log")) -LimitSec 330
& "$PSScriptRoot\get-file.ps1" -Name "$name-log" -Kind $Kind -Address $Address -Remote 'E:\PS1\logs\ps1_stage1.jsonl' -Local "$PSScriptRoot\$name.jsonl" -LimitSec 45
$events=@(Get-Content "$PSScriptRoot\$name.jsonl" | ForEach-Object {$_ | ConvertFrom-Json})
$events | Where-Object { $_.event -eq 'savestate' -or $_.error -or $_.event -eq 'shutdown'} | ConvertTo-Json -Compress | Write-Output
if(-not @($events | Where-Object { $_.event -eq 'savestate' -and $_.msg -match "frame=$SaveFrame rc=0 size=[1-9]" }).Count) {throw "navigation did not save frame $SaveFrame; phone bench remains previous state"}
# Shots are downloaded independently after DONE. bench stays on the phone for next navigation attempt.

