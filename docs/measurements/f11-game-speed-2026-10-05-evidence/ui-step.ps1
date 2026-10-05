param([Parameter(Mandatory)][string]$Name,[string]$Kind='Ip',[string]$Address='192.168.3.22',[Parameter(Mandatory)][string]$Command)
$ErrorActionPreference='Stop'
. "$PSScriptRoot/invoke-net.ps1"
$remote="E:\tester\$Name.bmp"
Invoke-BoundedNet -Name $Name -NetArgs @("-$Kind",$Address,'-Do',"$Command; SHOT $remote 1") -LimitSec 60
$marker=Get-Content "$PSScriptRoot/$Name.done.json" -Raw | ConvertFrom-Json
if($marker.timeout -or $marker.exitCode -ne 0){throw 'UI step incomplete; no screenshot downloaded'}
& "$PSScriptRoot/get-file.ps1" -Name "$Name-shot" -Kind $Kind -Address $Address -Remote $remote -Local "$PSScriptRoot/$Name.bmp"
Add-Type -AssemblyName System.Drawing
$shotImage=[Drawing.Image]::FromFile("$PSScriptRoot/$Name.bmp")
try{$shotImage.Save("$PSScriptRoot/$Name.png",[Drawing.Imaging.ImageFormat]::Png)}finally{$shotImage.Dispose()}
