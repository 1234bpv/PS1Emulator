param([switch]$RunsOnly)
$ErrorActionPreference='Stop'
function Assert($condition,[string]$message){if(-not $condition){throw $message}}
& "$PSScriptRoot/update-progress.ps1" | Out-Null
$summary=Get-Content "$PSScriptRoot/summary-progress.json" -Raw | ConvertFrom-Json
if(-not $RunsOnly){Assert ($summary.acceptedRuns -eq 108 -and $summary.completedTriples -eq 36) 'Expected 108 runs / 36 triples'}
$games=@(Get-Content "$PSScriptRoot/games.json" -Raw | ConvertFrom-Json)
$states=@(Get-Content "$PSScriptRoot/states/manifest.json" -Raw | ConvertFrom-Json)
if(-not $RunsOnly){Assert ($states.Count -eq 12 -and $games.Count -eq 12) 'Expected 12 canonical states and games'}
$oc=if(Test-Path "$PSScriptRoot/cpu50-clock-verified.json"){Get-Content "$PSScriptRoot/cpu50-clock-verified.json" -Raw | ConvertFrom-Json}else{$null}
$runChecks=@();$clockChecks=@();$stateChecks=@()
foreach($game in $games){
 $state=@($states | Where-Object game -eq $game.key)
 if($RunsOnly -and -not $state.Count){continue}
 Assert ($state.Count -eq 1) "Missing/duplicate canonical state $($game.key)"
 $state=$state[0]
 $local="$PSScriptRoot/states/$($state.file)"
 Assert ((Get-FileHash $local -Algorithm SHA256).Hash -eq $state.sha256 -and (Get-Item $local).Length -eq $state.bytes -and $game.frame -eq $state.loadFrame) "State hash/frame differs $($game.key)"
 $stateChecks+=[ordered]@{game=$game.key;file=$state.file;sha256=$state.sha256;bytes=$state.bytes;loadFrame=$state.loadFrame;visualProofAccepted=($state.status -eq 'accepted gameplay state')}
 Assert ($state.status -eq 'accepted gameplay state') "Gameplay not accepted $($game.key)"
 $endCycles=@()
 foreach($device in @('C5','N95-stock','N95-cpu50')){
  $name="$device-$($game.key)"
  if($RunsOnly -and -not (Test-Path "$PSScriptRoot/$name-r3.result.json")){continue}
  foreach($repeat in 1..3){
   $run="$name-r$repeat"
   $result=Get-Content "$PSScriptRoot/$run.result.json" -Raw | ConvertFrom-Json
   $events=@(Get-Content "$PSScriptRoot/$run.jsonl" | ForEach-Object {$_ | ConvertFrom-Json})
   Assert (@($events | Where-Object error).Count -eq 0) "Errors $run"
   Assert (@($events | Where-Object {$_.event -eq 'exit' -and $_.msg -eq 'gui returned 0'}).Count -eq 1) "Exit differs $run"
   $shutdown=@($events | Where-Object event -eq shutdown)[-1]
   Assert ($shutdown.msg -eq "gui: frames=1200 ms=$($result.ms)") "Timing differs $run"
   $loaded=@($events | Where-Object {$_.msg -like 'Loaded CD Image:*'})[-1]
   Assert ($loaded.msg -eq "Loaded CD Image: E:\PS1\games\$($game.image)[+pbp] ($($game.size) bytes).`n") "Loaded image differs $run"
   $load=@($events | Where-Object {$_.event -eq 'loadstate' -and $_.msg -match "frame=$($game.frame) rc=0"})
   Assert ($load.Count -eq 1) "Load frame differs $run"
   $region=@($events | Where-Object event -eq reset)[-1].msg
   Assert ($region -match 'type=(NTSC|PAL)') "No region $run"
   $dsa=@($events | Where-Object {$_.event -eq 'dsa' -and $_.msg -match 'rows='})[-1]
   Assert ($dsa.msg -match 'active=1 gl=0 fbPlatform=1 pixB=4.*rows=(\d+)/(\d+)/(\d+)/(\d+)/(\d+) covered=1 fbOk=1 rowCopy=0.*scr=320,240') "DSA invalid $run"
   $rows=0L;foreach($index in 1..5){$rows+=[long]$Matches[$index]}
   Assert ($rows/240 -eq $result.shownFrames) "Shown count differs $run"
   $zero=@($events | Where-Object {$_.event -eq 'vol' -and $_.msg -eq 'step=0'})[-1]
   $start=@($events | Where-Object event -eq clocks)[0]
   Assert ($zero -and $zero.ms -lt $start.ms) "Volume late $run"
   $pass=@($events | Where-Object event -eq pass)[-1].msg
   Assert ($pass -match 'cycle=(\d+)') "No end cycle $run"
   $endCycles+=$Matches[1]
   $runChecks+=[ordered]@{run=$run;frames=1200;guiMs=$result.ms;shownFrames=$result.shownFrames;errorCount=0;imageBytes=$game.size;endCycle=$Matches[1]}
  }
  if($device -ne 'C5'){
   $multiplier=if($device -eq 'N95-stock'){86}else{129}
   $core=if($device -eq 'N95-stock'){'04360644'}else{$oc.core}
   foreach($side in @('before','after')){
    $clockName="$name-clock-$side"
    if($name -eq 'N95-stock-gt' -and $side -eq 'before'){$clockName='resume-n95-clock-before'}
    $log=Get-Content "$PSScriptRoot/$clockName.log" -Raw
    & "$PSScriptRoot/check-clock-log.ps1" -Path "$PSScriptRoot/$clockName.log" -Multiplier $multiplier -Core $core | Out-Null
    $clockChecks+=[ordered]@{series=$name;side=$side;multiplier=$multiplier;core=$core;log="$clockName.log"}
   }
  }
 }
 Assert (@($endCycles | Select-Object -Unique).Count -eq 1) "End cycles differ across devices $($game.key)"
}
if($RunsOnly){
 [ordered]@{verifiedAt=(Get-Date).ToString('o');validRuns=$runChecks.Count;runChecks=$runChecks;stateChecks=$stateChecks;clockChecks=$clockChecks} | ConvertTo-Json -Depth 10 | Set-Content "$PSScriptRoot/current-run-audit.json" -Encoding utf8
 Write-Output "Current audit passed: $($runChecks.Count) runs, $($stateChecks.Count) states, $($clockChecks.Count) clock checks."
 return
}
$restoreChecks=@()
foreach($device in @('c5','n95')){
 $restore=@(Get-Content "$PSScriptRoot/$device-final2-restoration-verification.json" -Raw | ConvertFrom-Json)
 $expected=if($device -eq 'c5'){14}else{18}
 Assert ($restore.Count -eq $expected -and @($restore | Where-Object {-not $_.identical -or $_.originalSha256 -ne $_.readbackSha256}).Count -eq 0) "Restoration differs $device"
 $restoreChecks+=$restore
 $exe="$PSScriptRoot/final2-$device-exe.exe"
 Assert ((Get-Item $exe).Length -eq 1431280 -and (Get-FileHash $exe -Algorithm SHA256).Hash -eq '4CE2F9720F00FA69A8E2C262B2CFED6393E9B4197AC6B8B80E6FF3E6F0099EE4') "Final EXE differs $device"
 $absent=if($device -eq 'c5'){@('ps1_input.rec','ps1_input_prev.rec','ps1_interp.txt')}else{@('ps1_input_prev.rec','ps1_interp.txt')}
 foreach($leaf in $absent){
  $suffix="STAT E:\PS1\logs\$leaf" -replace '[^a-zA-Z0-9]','_'
  $stat=Get-Content "$PSScriptRoot/$device-final2-final-$suffix.stdout.txt" -Raw
  Assert ($stat -match 'ERR.*(-1|notfound|not found|notexist)') "Absence unproved $device/$leaf"
 }
}
$bt=Get-Content "$PSScriptRoot/c5-bt-authorization-restored.json" -Raw | ConvertFrom-Json
Assert ($bt.peer -eq 'KOMPUTER' -and $bt.authorized -eq $false -and $bt.visuallyVerified) 'C5 BT peer authorization not restored'
$bootBefore=(Get-FileHash "$PSScriptRoot/n95-boot-before.dat" -Algorithm SHA256).Hash
$bootAfter=(Get-FileHash "$PSScriptRoot/n95-boot-after.dat" -Algorithm SHA256).Hash
Assert ($bootBefore -eq $bootAfter) 'boot.dat differs'
$finalClock=Get-Content "$PSScriptRoot/N95-cpu50-final-clock.log" -Raw
& "$PSScriptRoot/check-clock-log.ps1" -Path "$PSScriptRoot/N95-cpu50-final-clock.log" -Multiplier 129 -Core $oc.core | Out-Null
$repo=(Resolve-Path "$PSScriptRoot/../../..").Path
$sourceDiff=@(& git -C $repo diff HEAD --name-only -- src tests tools-patches)
Assert ($sourceDiff.Count -eq 0) 'Emulator/source infrastructure changed'
[ordered]@{complete=$true;verifiedAt=(Get-Date).ToString('o');runs=$runChecks;states=$stateChecks;clockChecks=$clockChecks;restoredFiles=$restoreChecks;bootDatSha256=$bootAfter;c5PeerAuthorization=$bt;n95FinalMultiplier=129;n95FinalCore=$oc.core;emulatorSourceUnchanged=$true} | ConvertTo-Json -Depth 10 | Set-Content "$PSScriptRoot/final-audit.json" -Encoding utf8
Write-Output 'Final audit passed: 108 runs, 12 states, 48 clock checks, 32 restored files.'
