param([int]$NextId,[string]$Address='192.168.3.22',[switch]$SkipCompleted)
$ErrorActionPreference='Stop'
. "$PSScriptRoot/invoke-net.ps1"
$baseline="$PSScriptRoot/user-oc-baseline.json"
if(-not (Test-Path $baseline)){throw 'User clock baseline missing'}
$games=@(Get-Content "$PSScriptRoot/games.json" -Raw | ConvertFrom-Json)
& "$PSScriptRoot/update-progress.ps1" | Out-Null
$summary=Get-Content "$PSScriptRoot/summary-progress.json" -Raw | ConvertFrom-Json
foreach($game in $games){
 $name="N95-useroc-$($game.key)"
 if($SkipCompleted -and @($summary.series | Where-Object {$_.label -eq $name -and $_.repeatCount -eq 3}).Count -eq 1){
  & "$PSScriptRoot/check-user-clock-files.ps1" -Name "$name-clock-before" | Out-Null
  & "$PSScriptRoot/check-user-clock-files.ps1" -Name "$name-clock-after" | Out-Null
  Write-Output "Verified completed series skipped: $name"
  continue
 }
 if(Test-Path "$PSScriptRoot/$name-r1.result.json"){throw "Explicit resume needed $name"}
 $stock=@($summary.series | Where-Object {$_.label -eq "N95-stock-$($game.key)" -and $_.repeatCount -eq 3})
 if($stock.Count -ne 1){throw 'Stock triple missing'}
 $settings=(Get-Content "$PSScriptRoot/stock-template.txt" -Raw).Replace('Castlevania - Symphony of the Night (USA.pbp',$game.image)
 $settings | Set-Content "$PSScriptRoot/useroc-settings-$($game.key).txt" -Encoding ascii
 foreach($upload in @(@("$PSScriptRoot/useroc-settings-$($game.key).txt",'E:\PS1\cfg\ps1_settings.txt'),@("$PSScriptRoot/states/$($game.state)",'E:\PS1\saves\bench.sav'))){
  $tag="$name-upload-"+[IO.Path]::GetFileName($upload[0])
  Invoke-BoundedNet -Name $tag -NetArgs @('-Ip',$Address,'-PutFile',$upload[0],'-Remote',$upload[1]) -LimitSec 240 | Out-Null
  $done=Get-Content "$PSScriptRoot/$tag.done.json" -Raw | ConvertFrom-Json
  if($done.timeout -or $done.exitCode -ne 0){throw "Upload failed $tag"}
 }
 & "$PSScriptRoot/read-user-clock.ps1" -Name "$name-clock-before" -NextId $NextId -Address $Address -Baseline $baseline
 $NextId+=2
 $maximum=[int](($stock[0].ms | Measure-Object -Maximum).Maximum)
 $waitMs=[Math]::Max(35000,$maximum+18000)
 & "$PSScriptRoot/measure-device.ps1" -Label $name -Kind Ip -Address $Address -NextId $NextId -Image $game.image -StateFrame $game.frame -WaitMs $waitMs -AdaptiveWait
 $NextId+=3
 & "$PSScriptRoot/read-user-clock.ps1" -Name "$name-clock-after" -NextId $NextId -Address $Address -Baseline $baseline
 $NextId+=2
 & "$PSScriptRoot/update-progress.ps1" | Out-Null
 [ordered]@{lastCompleted=$name;nextId=$NextId;updated=(Get-Date).ToString('o')} | ConvertTo-Json | Set-Content "$PSScriptRoot/useroc-progress.json" -Encoding utf8
 Add-Content "$PSScriptRoot/../../../work/state.md" ("{0}: {1} three runs completed; user-installed clock registers unchanged before/after. Next proc{2}." -f (Get-Date -Format 'yyyy-MM-dd HH:mm'),$name,$NextId)
 Write-Output "Completed $name; next proc$NextId"
}
