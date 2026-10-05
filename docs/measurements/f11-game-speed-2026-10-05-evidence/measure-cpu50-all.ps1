param([Parameter(Mandatory)][int]$NextId,[Parameter(Mandatory)][string]$Core)
$ErrorActionPreference='Stop'
. "$PSScriptRoot/invoke-net.ps1"
& "$PSScriptRoot/update-progress.ps1" | Out-Null
$summary=Get-Content "$PSScriptRoot/summary-progress.json" -Raw | ConvertFrom-Json
$games=@(Get-Content "$PSScriptRoot/games.json" -Raw | ConvertFrom-Json)
foreach($game in $games) {
 if(@($summary.series | Where-Object {$_.label -eq "N95-stock-$($game.key)" -and $_.repeatCount -eq 3}).Count -ne 1){throw 'All stock games must finish before cpu50'}
 if(-not $game.state){throw 'Canonical gameplay state missing'}
}
foreach($game in $games) {
 $name="N95-cpu50-$($game.key)"
 if(Test-Path "$PSScriptRoot/$name-r1.result.json"){throw "Existing series requires explicit resume: $name"}
 $settings=(Get-Content "$PSScriptRoot/stock-template.txt" -Raw).Replace('Castlevania - Symphony of the Night (USA.pbp',$game.image)
 $settings | Set-Content "$PSScriptRoot/stock-$($game.key).txt" -Encoding ascii
 foreach($upload in @(@("$PSScriptRoot/stock-$($game.key).txt",'E:\PS1\cfg\ps1_settings.txt'),@("$PSScriptRoot/states/$($game.state)",'E:\PS1\saves\bench.sav'))) {
  $tag="$name-upload-"+[IO.Path]::GetFileName($upload[0])
  Invoke-BoundedNet -Name $tag -NetArgs @('-Ip','192.168.3.22','-PutFile',$upload[0],'-Remote',$upload[1]) -LimitSec 240
  $mark=Get-Content "$PSScriptRoot/$tag.done.json" -Raw | ConvertFrom-Json
  if($mark.timeout -or $mark.exitCode -ne 0){throw "Upload incomplete $tag"}
 }
 & "$PSScriptRoot/read-clock.ps1" -Name "$name-clock-before" -Multiplier 129 -Core $Core
 $probeText=Get-Content "$PSScriptRoot/$name-clock-before-start.stdout.txt" -Raw
 if($probeText -notmatch "OK proc $NextId\b"){throw "Unexpected probe process ID $name"}
 $NextId++
 $stock=@($summary.series | Where-Object label -eq "N95-stock-$($game.key)")[0]
 $waitMs=[int](($stock.ms | Measure-Object -Maximum).Maximum+22000)
 & "$PSScriptRoot/measure-device.ps1" -Label $name -Kind Ip -Address 192.168.3.22 -NextId $NextId -Image $game.image -StateFrame $game.frame -WaitMs $waitMs
 $NextId+=3
 & "$PSScriptRoot/read-clock.ps1" -Name "$name-clock-after" -Multiplier 129 -Core $Core
 $probeText=Get-Content "$PSScriptRoot/$name-clock-after-start.stdout.txt" -Raw
 if($probeText -notmatch "OK proc $NextId\b"){throw "Unexpected after-probe process ID $name"}
 $NextId++
 & "$PSScriptRoot/write-report.ps1" | Out-Null
 Add-Content "$PSScriptRoot/../../../work/state.md" ("{0}: {1} completed; clock before/after m129 CORE{2}. Next daemon process {3}." -f (Get-Date -Format 'yyyy-MM-dd HH:mm'),$name,$Core,$NextId)
 [ordered]@{lastCompleted=$name;nextId=$NextId;core=$Core;updated=(Get-Date).ToString('o')} | ConvertTo-Json | Set-Content "$PSScriptRoot/cpu50-progress.json" -Encoding utf8
}
