param([string]$Device,[string]$Kind,[string]$Address)
$ErrorActionPreference='Stop'
. "$PSScriptRoot\invoke-net.ps1"
$link=@("-$Kind",$Address)
$client=Join-Path $PSScriptRoot 'restoration-client.ps1'
$backup=Join-Path $PSScriptRoot "$Device-backup"
$verified=Join-Path $PSScriptRoot "$Device-final-verified"
New-Item -ItemType Directory -Force -Path $verified | Out-Null
$manifest=@(Get-Content "$backup\manifest.json" -Raw | ConvertFrom-Json)
function RemotePath([string]$relative) {
 if($relative -eq 'ps1_input.rec'){return 'E:\PS1\logs\ps1_input.rec'}
 return "E:\PS1\$relative"
}
foreach($entry in $manifest | Where-Object {$_.relative -match '(?i)(ps1_settings.txt|bench.sav|card[12].mcd|^ps1_input.rec)$'}) {
 $name="$Device-resume-restore-$($entry.relative -replace '[^a-zA-Z0-9]','_')"
 for($attempt=1;$attempt -le 3;$attempt++) {
  Invoke-BoundedNet -Name "$name-a$attempt" -NetArgs ($link+@('-PutFile',"$backup\$($entry.relative)",'-Remote',(RemotePath $entry.relative))) -LimitSec 240 -ClientPath $client
  $mark=Get-Content "$PSScriptRoot\$name-a$attempt.done.json" -Raw | ConvertFrom-Json
  if(-not $mark.timeout -and $mark.exitCode -eq 0){break}
 }
 if($mark.timeout -or $mark.exitCode -ne 0){throw "restore upload incomplete $($entry.relative)"}
}
$delete=if($Device -eq 'c5'){'DEL E:\PS1\logs\ps1_input.rec; DEL E:\PS1\logs\ps1_input_prev.rec; DEL E:\PS1\logs\ps1_interp.txt'}else{'DEL E:\PS1\logs\ps1_input_prev.rec; DEL E:\PS1\logs\ps1_interp.txt'}
Invoke-BoundedNet -Name "$Device-resume-delete-temp" -NetArgs ($link+@('-Do',$delete)) -LimitSec 30 -ClientPath $client
$checks=@()
foreach($entry in $manifest) {
 $remote=RemotePath $entry.relative
 $local=Join-Path $verified $entry.relative
 $name="$Device-resume-readback-$($entry.relative -replace '[^a-zA-Z0-9]','_')"
 for($attempt=1;$attempt -le 3;$attempt++) {
  Invoke-BoundedNet -Name "$name-a$attempt" -NetArgs ($link+@('-GetFile',$remote,'-OutFile',$local)) -LimitSec 60 -ClientPath $client
  $mark=Get-Content "$PSScriptRoot\$name-a$attempt.done.json" -Raw | ConvertFrom-Json
  if(-not $mark.timeout -and $mark.exitCode -eq 0){break}
 }
 if($mark.timeout -or $mark.exitCode -ne 0){throw "restore readback incomplete $remote"}
 $hash=(Get-FileHash -LiteralPath $local -Algorithm SHA256).Hash
 $checks += [pscustomobject]@{device=$Device;path=$entry.relative;bytes=(Get-Item $local).Length;originalSha256=$entry.sha256;readbackSha256=$hash;identical=($hash -eq $entry.sha256)}
 $checks | ConvertTo-Json | Set-Content "$PSScriptRoot\$Device-resume-restoration-verification.json" -Encoding utf8
 if($hash -ne $entry.sha256){throw "restoration differs $remote"}
}
$stats=if($Device -eq 'c5'){'STAT E:\PS1\logs\ps1_input.rec; STAT E:\PS1\logs\ps1_input_prev.rec; STAT E:\PS1\logs\ps1_interp.txt; STATUS; PS ps1stage1; INFO'}else{'STAT E:\PS1\logs\ps1_input_prev.rec; STAT E:\PS1\logs\ps1_interp.txt; STATUS; PS ps1stage1; INFO'}
Invoke-BoundedNet -Name "$Device-resume-final-stat" -NetArgs ($link+@('-Do',$stats)) -LimitSec 30 -ClientPath $client
$checks | Select-Object device,path,identical | Format-Table -AutoSize
