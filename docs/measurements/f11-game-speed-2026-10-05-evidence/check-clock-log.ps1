param([Parameter(Mandatory)][string]$Path,[int]$Multiplier=86,[string]$Core='04360644')
$ErrorActionPreference='Stop'
$log=Get-Content $Path -Raw
if($log -notmatch '== end =='){throw 'Clock log incomplete'}
if($log -notmatch 'CM_CLKSEL1_PLL\s+@[^\r\n]*= ([0-9a-fA-F]{8})'){throw 'No actual PLL register'}
$pll=[Convert]::ToUInt32($Matches[1],16)
$actualMultiplier=($pll -shr 12) -band 1023
if($log -notmatch 'CM_CLKSEL1_CORE\s+@[^\r\n]*= ([0-9a-fA-F]{8})'){throw 'No actual CORE register'}
$actualCore=$Matches[1].ToUpperInvariant()
$coreBits=[Convert]::ToUInt32($actualCore,16)
$l3=$coreBits -band 15
$dss=($coreBits -shr 8) -band 31
if($log -notmatch 'CM_CLKSEL_MPU\s+@[^\r\n]*= ([0-9a-fA-F]{8})'){throw 'No actual MPU register'}
$mpu=[Convert]::ToUInt32($Matches[1],16) -band 31
if($actualMultiplier -ne $Multiplier -or $actualCore -ne $Core.ToUpperInvariant() -or $mpu -ne 2){throw "Actual clocks differ: m=$actualMultiplier CORE=$actualCore MPUdiv=$mpu"}
$dpll=$actualMultiplier*3840
$coreKhz=$dpll*2
if($coreKhz/$l3 -ne 165120){throw 'Actual bus is not stock'}
[ordered]@{multiplier=$actualMultiplier;core=$actualCore;pll=('{0:X8}' -f $pll);mpuDivider=$mpu;l3Divider=$l3;dssDivider=$dss;dpllKhz=$dpll;coreKhz=$coreKhz;mpuKhz=$coreKhz/$mpu;busKhz=$coreKhz/$l3;dssKhz=$coreKhz/$dss;log=[IO.Path]::GetFileName($Path)}
