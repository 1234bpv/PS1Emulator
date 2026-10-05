$ErrorActionPreference='Stop'
function Median($values) { $v=@($values | Sort-Object); if($v.Count -eq 0){return $null}; return $v[[int][Math]::Floor($v.Count/2)] }
$clockAccepted=@{}
$results=@(Get-ChildItem $PSScriptRoot -Filter '*.result.json' -File | ForEach-Object {
 $candidate=Get-Content $_.FullName -Raw | ConvertFrom-Json
 if($candidate.label -like 'N95-useroc-*'){
  if($null -eq $candidate.volume0ms -or $null -eq $candidate.clockStartMs -or $candidate.volume0ms -ge $candidate.clockStartMs){return}
  if(-not $clockAccepted.ContainsKey($candidate.label)){
   $clockAccepted[$candidate.label]=$false
   if((Test-Path "$PSScriptRoot/$($candidate.label)-clock-after-engine.log") -and (Test-Path "$PSScriptRoot/$($candidate.label)-clock-after-dsp.log")){
    try{
     & "$PSScriptRoot/check-user-clock-files.ps1" -Name "$($candidate.label)-clock-before" | Out-Null
     & "$PSScriptRoot/check-user-clock-files.ps1" -Name "$($candidate.label)-clock-after" | Out-Null
     $clockAccepted[$candidate.label]=$true
    }catch{Write-Warning "User clock series excluded: $($candidate.label): $_"}
   }
  }
  if(-not $clockAccepted[$candidate.label]){return}
 }
 $candidate
} | Where-Object { $_.frames -eq 1200 -and $_.errors -eq 0 })
$series=@($results | Group-Object label | Sort-Object Name | ForEach-Object {
 $runs=@($_.Group | Sort-Object repeat)
 $regions=@($runs.region | Where-Object {$_} | Select-Object -Unique)
 if(-not $regions.Count){$events=Get-Content "$PSScriptRoot/$($_.Name)-r1.jsonl" | ConvertFrom-Json; $regionText=(@($events | Where-Object event -eq reset)[0]).msg; if($regionText -match 'type=(NTSC|PAL)'){$regions=@($Matches[1])}}
 [ordered]@{label=$_.Name;repeatCount=$runs.Count;region=($regions -join ',');ms=@($runs.ms);shownFrames=@($runs.shownFrames);fpsMedian=(Median $runs.shownFps);fpsMin=($runs.shownFps | Measure-Object -Minimum).Minimum;fpsMax=($runs.shownFps | Measure-Object -Maximum).Maximum;speedMedian=(Median $runs.speedPercent);speedMin=($runs.speedPercent | Measure-Object -Minimum).Minimum;speedMax=($runs.speedPercent | Measure-Object -Maximum).Maximum;errors=@($runs.errors);pass=@($runs.pass);loaded=@($runs.loaded)}
})
[ordered]@{updated=(Get-Date).ToString('o');acceptedRuns=$results.Count;completedTriples=@($series | Where-Object repeatCount -eq 3).Count;series=$series} | ConvertTo-Json -Depth 8 | Set-Content "$PSScriptRoot/summary-progress.json" -Encoding UTF8
$series | ForEach-Object { '{0}: {1} runs, {2} ms; median {3:F3} FPS, {4:F3}%' -f $_.label,$_.repeatCount,($_.ms -join '/'),$_.fpsMedian,$_.speedMedian }

