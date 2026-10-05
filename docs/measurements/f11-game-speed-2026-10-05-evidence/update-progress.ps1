$ErrorActionPreference='Stop'
function Median($values) { $v=@($values | Sort-Object); if($v.Count -eq 0){return $null}; return $v[[int][Math]::Floor($v.Count/2)] }
$results=@(Get-ChildItem $PSScriptRoot -Filter '*.result.json' -File | ForEach-Object { Get-Content $_.FullName -Raw | ConvertFrom-Json } | Where-Object { $_.frames -eq 1200 -and $_.errors -eq 0 })
$series=@($results | Group-Object label | Sort-Object Name | ForEach-Object {
 $runs=@($_.Group | Sort-Object repeat)
 $regions=@($runs.region | Where-Object {$_} | Select-Object -Unique)
 if(-not $regions.Count){$events=Get-Content "$PSScriptRoot/$($_.Name)-r1.jsonl" | ConvertFrom-Json; $regionText=(@($events | Where-Object event -eq reset)[0]).msg; if($regionText -match 'type=(NTSC|PAL)'){$regions=@($Matches[1])}}
 [ordered]@{label=$_.Name;repeatCount=$runs.Count;region=($regions -join ',');ms=@($runs.ms);shownFrames=@($runs.shownFrames);fpsMedian=(Median $runs.shownFps);fpsMin=($runs.shownFps | Measure-Object -Minimum).Minimum;fpsMax=($runs.shownFps | Measure-Object -Maximum).Maximum;speedMedian=(Median $runs.speedPercent);speedMin=($runs.speedPercent | Measure-Object -Minimum).Minimum;speedMax=($runs.speedPercent | Measure-Object -Maximum).Maximum;errors=@($runs.errors);pass=@($runs.pass);loaded=@($runs.loaded)}
})
[ordered]@{updated=(Get-Date).ToString('o');acceptedRuns=$results.Count;completedTriples=@($series | Where-Object repeatCount -eq 3).Count;series=$series} | ConvertTo-Json -Depth 8 | Set-Content "$PSScriptRoot/summary-progress.json" -Encoding UTF8
$series | ForEach-Object { '{0}: {1} runs, {2} ms; median {3:F3} FPS, {4:F3}%' -f $_.label,$_.repeatCount,($_.ms -join '/'),$_.fpsMedian,$_.speedMedian }
