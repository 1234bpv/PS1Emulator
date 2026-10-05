param([Parameter(Mandatory)][string]$Name)
$ErrorActionPreference='Stop'
$baseline=Get-Content "$PSScriptRoot/user-oc-baseline.json" -Raw | ConvertFrom-Json
$engine=Get-Content "$PSScriptRoot/$Name-engine.log" -Raw
$dsp=Get-Content "$PSScriptRoot/$Name-dsp.log" -Raw
if($engine -notmatch '== end ==' -or $dsp -notmatch '== end =='){throw "Incomplete clock logs $Name"}
foreach($key in $baseline.registers.psobject.Properties.Name){
 $text=if($key -eq 'CM_CLKSEL_DSP'){$dsp}else{$engine}
 if($text -notmatch "$key\s+@[^\r\n]*= ([0-9a-fA-F]{8})"){throw "Missing $Name/$key"}
 if($Matches[1].ToUpperInvariant() -ne $baseline.registers.$key){throw "Clock changed $Name/$key"}
}
if($dsp -notmatch 'NOT STOCK AT START: m=111 CLKSEL1_CORE=04360644 CLKSEL_MPU=00000002'){throw "DSP snapshot core differs $Name"}
if($Name -match '^(N95-useroc-.+)-clock-(before|after)$'){
 $series=$Matches[1]
 $side=$Matches[2]
 $probe=Get-Content "$PSScriptRoot/$Name-engine-start.done.json" -Raw | ConvertFrom-Json
 if($side -eq 'before'){
  $run=Get-Content "$PSScriptRoot/$series-r1-scenario.done.json" -Raw | ConvertFrom-Json
  if([datetime]$probe.finished -gt [datetime]$run.started){throw "Before probe not before run $series"}
 }else{
  $run=Get-Content "$PSScriptRoot/$series-r3-scenario.done.json" -Raw | ConvertFrom-Json
  if([datetime]$probe.started -lt [datetime]$run.finished){throw "After probe not after run $series"}
 }
}
[ordered]@{name=$Name;registersMatched=$true;multiplier=111;core='04360644';dsp='000023C3';baseline='user-oc-baseline.json'}
