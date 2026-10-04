param([string]$Name,[int]$Multiplier=86,[string]$Core='04360644')
$ErrorActionPreference='Stop'
. "$PSScriptRoot\invoke-net.ps1"
Invoke-BoundedNet -Name "$Name-start" -NetArgs @('-Ip','192.168.3.22','-Do','START E:\sys\bin\ocprobe.exe patcher engine; SLEEP 2000') -LimitSec 25
$mark=Get-Content "$PSScriptRoot\$Name-start.done.json" -Raw | ConvertFrom-Json
if($mark.timeout -or $mark.exitCode -ne 0){throw 'clock probe start incomplete'}
for($attempt=1;$attempt -le 3;$attempt++) {
 Invoke-BoundedNet -Name "$Name-a$attempt" -NetArgs @('-Ip','192.168.3.22','-GetFile','E:\ocprobe.log','-OutFile',"$PSScriptRoot\$Name.log") -LimitSec 30
 $mark=Get-Content "$PSScriptRoot\$Name-a$attempt.done.json" -Raw | ConvertFrom-Json
 if(-not $mark.timeout -and $mark.exitCode -eq 0){break}
}
if($mark.timeout -or $mark.exitCode -ne 0){throw 'clock probe download incomplete'}
$log=Get-Content "$PSScriptRoot\$Name.log" -Raw
if($log -notmatch '== end ==' -or $log -notmatch "m=$Multiplier DPLL=" -or $log -notmatch "CM_CLKSEL1_CORE[^\r\n]*= $Core") {throw "clock registers do not match $Name"}
$log | Write-Output
