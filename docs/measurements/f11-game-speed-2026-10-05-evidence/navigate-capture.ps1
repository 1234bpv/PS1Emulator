param([string]$Label,[string]$Kind,[string]$Address,[string]$GameKey,[int]$Attempt,[int]$LoadFrame=0,[int]$SaveFrame,[string]$Recipe)
$ErrorActionPreference='Stop'
$name="$Label-$GameKey-nav$Attempt"
$failure=$null
try { & "$PSScriptRoot/navigate.ps1" @PSBoundParameters } catch {$failure=$_; Write-Output $_}
$tracePath="$PSScriptRoot/$name.log"
if(Test-Path $tracePath) {
 $trace=@(Get-Content $tracePath)
 for($i=0;$i -lt $trace.Count-1;$i++) {
  if($trace[$i] -match '\] -?SHOT (E:\\tester\\[^ ]+\.bmp)' -and $trace[$i+1] -match '^OK shot') {
   $remote=[regex]::Match($trace[$i],'E:\\tester\\[^ ]+\.bmp').Value
   $leaf=[IO.Path]::GetFileName($remote)
   & "$PSScriptRoot/get-file.ps1" -Name "$Label-$leaf-get" -Kind $Kind -Address $Address -Remote $remote -Local "$PSScriptRoot/$Label-$leaf"
  }
 }
}
if($failure){throw $failure}
& "$PSScriptRoot/get-file.ps1" -Name "$name-state" -Kind $Kind -Address $Address -Remote 'E:\PS1\saves\bench.sav' -Local "$PSScriptRoot/states/$GameKey-nav$Attempt.sav"
