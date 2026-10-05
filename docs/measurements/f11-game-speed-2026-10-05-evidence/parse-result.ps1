param([string]$Name,[string]$Label,[int]$Repeat)
$PSScriptRootRoot=$PSScriptRoot
$trace=Get-Content "$PSScriptRoot\$Name.log" -Raw
$events=@(Get-Content "$PSScriptRoot\$name.jsonl" | ForEach-Object { $_ | ConvertFrom-Json })
  $shutdown=@($events | Where-Object event -eq 'shutdown')[-1]
  $pass=@($events | Where-Object event -eq 'pass')[-1]
  $errors=@($events | Where-Object error)
  $zero=@($events | Where-Object { $_.event -eq 'vol' -and $_.msg -eq 'step=0' })[-1]
  $start=@($events | Where-Object event -eq 'clocks')[0]
  $dsa=@($events | Where-Object { $_.event -eq 'dsa' -and $_.msg -match 'rows=' })[-1]
  $loaded=@($events | Where-Object { $_.msg -like 'Loaded CD Image:*' })[-1]
  if($shutdown.msg -notmatch '^gui: frames=1200 ms=(\d+)$') { throw "incorrect frame count $name $($shutdown.msg)" }
  $elapsed=[int]$Matches[1]
  if($zero.ms -ge $start.ms -or $errors.Count -ne 0 -or @($events | Where-Object {$_.event -eq 'exit' -and $_.msg -eq 'gui returned 0'}).Count -ne 1) { throw "run invalid $name errors=$($errors.Count)" }
  if($dsa.msg -notmatch 'active=1 gl=0 fbPlatform=1 pixB=4.*rows=(\d+)/(\d+)/(\d+)/(\d+)/(\d+) covered=1 fbOk=1 rowCopy=0.*scr=320,240') { throw "cannot derive shown frames $name" }
  $rows=0L;1..5 | ForEach-Object { $rows += [long]$Matches[$_] }
  if($rows%240) { throw "incomplete rows $name" }
  $result=[ordered]@{label=$Label; repeat=$repeat; frames=1200; ms=$elapsed; shownFrames=$rows/240; shownFps=($rows/240)*1000/$elapsed; ps1Fps=1200000/$elapsed; speedPercent=2000000/$elapsed; errors=$errors.Count; shutdown=$shutdown.msg; pass=$pass.msg; loaded=$loaded.msg; emuMs=$null; shownMethod='DSA direct FB pixB=4, stock fxaa=0, 240 rows per present'; volume0ms=$zero.ms; clockStartMs=$start.ms}
  $result | ConvertTo-Json -Depth 3 | Set-Content "$PSScriptRoot\$name.result.json" -Encoding utf8
  $result | ConvertTo-Json -Compress | Write-Output

