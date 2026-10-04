param([string]$Name,[string]$Kind,[string]$Address,[string]$Remote,[string]$Local,[int]$LimitSec=45)
$ErrorActionPreference='Stop'
. "$PSScriptRoot\invoke-net.ps1"
for($attempt=1;$attempt -le 3;$attempt++) {
 Invoke-BoundedNet -Name "$Name-a$attempt" -NetArgs @("-$Kind",$Address,'-GetFile',$Remote,'-OutFile',$Local) -LimitSec $LimitSec
 $mark=Get-Content "$PSScriptRoot\$Name-a$attempt.done.json" -Raw | ConvertFrom-Json
 if(-not $mark.timeout -and $mark.exitCode -eq 0){break}
}
if($mark.timeout -or $mark.exitCode -ne 0){throw "download failed after 3 attempts: $Remote"}
