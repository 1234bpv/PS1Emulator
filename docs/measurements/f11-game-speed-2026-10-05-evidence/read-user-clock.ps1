param([Parameter(Mandatory)][string]$Name,[int]$NextId=0,[string]$Address='192.168.3.22',[string]$Baseline='')
$ErrorActionPreference='Stop'
. "$PSScriptRoot/invoke-net.ps1"
foreach($mode in @('engine','dsp')){
 $args=if($mode -eq 'dsp'){'patcher dsp 1'}else{'patcher engine'}
 Invoke-BoundedNet -Name "$Name-$mode-start" -NetArgs @('-Ip',$Address,'-Do',"START E:\sys\bin\ocprobe.exe $args; SLEEP 2000") -LimitSec 25 | Out-Null
 $done=Get-Content "$PSScriptRoot/$Name-$mode-start.done.json" -Raw | ConvertFrom-Json
 if($done.timeout -or $done.exitCode -ne 0){throw "Clock probe start failed $Name/$mode"}
 if($NextId){
  $out=Get-Content "$PSScriptRoot/$Name-$mode-start.stdout.txt" -Raw
  if($out -notmatch "OK proc $NextId\b"){throw "Clock process differs $Name/$mode"}
  $NextId++
 }
 Invoke-BoundedNet -Name "$Name-$mode-get" -NetArgs @('-Ip',$Address,'-GetFile','E:\ocprobe.log','-OutFile',"$PSScriptRoot/$Name-$mode.log") -LimitSec 30 | Out-Null
 $done=Get-Content "$PSScriptRoot/$Name-$mode-get.done.json" -Raw | ConvertFrom-Json
 if($done.timeout -or $done.exitCode -ne 0){throw "Clock download failed $Name/$mode"}
}
$engine=Get-Content "$PSScriptRoot/$Name-engine.log" -Raw
$dsp=Get-Content "$PSScriptRoot/$Name-dsp.log" -Raw
if($engine -notmatch '== end ==' -or $dsp -notmatch '== end =='){throw 'Clock logs incomplete'}
$registers=[ordered]@{}
foreach($key in @('CM_CLKSEL1_PLL','CM_CLKSEL2_PLL','CM_CLKSEL_MPU','CM_CLKSEL1_CORE','SDRC_RFR_CTRL0','SDRC_ACTIM_A_0')){
 if($engine -notmatch "$key\s+@[^\r\n]*= ([0-9a-fA-F]{8})"){throw "Register absent $key"}
 $registers[$key]=$Matches[1].ToUpperInvariant()
}
if($dsp -notmatch 'CM_CLKSEL_DSP\s+@[^\r\n]*= ([0-9a-fA-F]{8})'){throw 'DSP register absent'}
$registers['CM_CLKSEL_DSP']=$Matches[1].ToUpperInvariant()
$pll=[Convert]::ToUInt32($registers.CM_CLKSEL1_PLL,16)
$coreReg=[Convert]::ToUInt32($registers.CM_CLKSEL1_CORE,16)
$mult=($pll -shr 12) -band 1023
$div=(($pll -shr 8) -band 15)+1
$sysSel=($pll -shr 23) -band 7
$sys=if($sysSel -eq 0){19200}elseif($sysSel -eq 2){13000}else{12000}
$coreSel=[Convert]::ToUInt32($registers.CM_CLKSEL2_PLL,16) -band 3
$coreKhz=$sys*$mult/$div*$coreSel
$mpu=[Convert]::ToUInt32($registers.CM_CLKSEL_MPU,16) -band 31
$l3=$coreReg -band 31
$dss=($coreReg -shr 8) -band 31
$dspDiv=[Convert]::ToUInt32($registers.CM_CLKSEL_DSP,16) -band 15
if(-not $mpu -or -not $l3 -or -not $dss -or -not $dspDiv){throw 'Zero clock divider'}
if($Baseline){
 $expected=Get-Content $Baseline -Raw | ConvertFrom-Json
 foreach($key in $registers.Keys){if($registers[$key] -ne $expected.registers.$key){throw "User clock changed $key actual=$($registers[$key]) expected=$($expected.registers.$key)"}}
}
$result=[ordered]@{verifiedAt=(Get-Date).ToString('o');registers=$registers;multiplier=$mult;coreKhz=$coreKhz;mpuKhz=$coreKhz/$mpu;busKhz=$coreKhz/$l3;dssKhz=$coreKhz/$dss;dspKhz=$coreKhz/$dspDiv;engineLog="$Name-engine.log";dspLog="$Name-dsp.log";matchedBaseline=[bool]$Baseline;clockWritesPerformed=$false;nextId=$NextId}
$result | ConvertTo-Json -Depth 5 | Set-Content "$PSScriptRoot/$Name.json" -Encoding utf8
$result | ConvertTo-Json -Compress -Depth 5
