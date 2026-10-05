$ErrorActionPreference='Stop'
$repo=(Resolve-Path "$PSScriptRoot/../../..").Path
& "$PSScriptRoot/audit-final.ps1" -RunsOnly | Out-Null
$runs=Get-Content "$PSScriptRoot/current-run-audit.json" -Raw | ConvertFrom-Json
$restored=@()
foreach($device in @('c5','n95')){
 $proof=@(Get-Content "$PSScriptRoot/$device-final2-restoration-verification.json" -Raw | ConvertFrom-Json)
 $expected=if($device -eq 'c5'){14}else{18}
 if($proof.Count -ne $expected){throw "Restoration proof missing $device"}
 foreach($entry in $proof){
  $path=Join-Path "$PSScriptRoot/$device-final-verified" $entry.path
  $sha=(Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash
  if($sha -ne $entry.originalSha256 -or (Get-Item $path).Length -ne $entry.bytes){throw "Readback proof differs $path"}
  $restored+=@{device=$device;path=$entry.path;sha256=$sha}
 }
}
$report=Get-Content "$repo/docs/measurements/f11-game-speed-2026-10-05.md" -Raw
$stateRows=@($report -split '\r?\n' | Where-Object {$_ -match '^\| \w+ \| [^|]+\.sav / '})
if($stateRows.Count -ne 12){throw 'State proof rows differ'}
$shots=@()
foreach($row in $stateRows){
 $names=@([regex]::Matches($row,'[A-Za-z0-9_-]+\.png') | ForEach-Object Value | Select-Object -Unique)
 if(-not $names.Count){throw 'Missing gameplay screenshot reference'}
 foreach($name in $names){
  $path=Join-Path $PSScriptRoot $name
  if(-not (Test-Path $path)){throw "Screenshot missing $name"}
  $shots+=@{file=$name;sha256=(Get-FileHash $path -Algorithm SHA256).Hash}
 }
}
$release='4CE2F9720F00FA69A8E2C262B2CFED6393E9B4197AC6B8B80E6FF3E6F0099EE4'
$exeProofs=@()
foreach($name in @('final2-c5-exe.exe','continue2-n95-exe.exe')){
 $path=Join-Path $PSScriptRoot $name
 $hash=(Get-FileHash $path -Algorithm SHA256).Hash
 if($hash -ne $release -or (Get-Item $path).Length -ne 1431280){throw "Release proof differs $name"}
 $exeProofs+=@{file=$name;sha256=$hash;bytes=1431280}
}
$source=Join-Path $PSScriptRoot 'reference/clock2420_data.c'
$code=Get-Content $source -Raw
$blocks=[regex]::Matches($code,'(?s)static struct clk (\w+)\s*=\s*\{(.*?)\n\};')
$domains=@(foreach($block in $blocks){if($block.Groups[2].Value -match '\.parent\s*=\s*&(core_ck|dsp_fck|iva1_ifck|core_l3_ck)\b'){@{clock=$block.Groups[1].Value;parent=$Matches[1]}}})
$checks=@(
 @{requirement='Twelve common game images and canonical gameplay states';status='proved';evidence='games.json; states/manifest.json; current-run-audit.json'},
 @{requirement='Release alpha1.3 verified on both devices before runs';status='proved';evidence=$exeProofs},
 @{requirement='All C5 and N95-stock runs: 3x1200 per game, correct image, zero errors, volume0, DSA';status='proved';evidence='current-run-audit.json:72 runs'},
 @{requirement='N95-stock registers before and after each series';status='proved';evidence='current-run-audit.json:24 clock checks'},
 @{requirement='All cpu50 game runs under latest peripheral constraint';status='incomplete';evidence='No accepted cpu50 runs. stand-stop.json: device unreachable after3 attempts'},
 @{requirement='No non-CPU peripheral above stock';status='contradicted for last known applied configuration';evidence='retry3-configuration-audit.json:IVA/3 remains; installed ocgui lacks IVA field'},
 @{requirement='Original files restored and checked on both devices';status='proved';evidence='c5-final2-restoration-verification.json:14; n95-final2-restoration-verification.json:18; 32 local readbacks rehashed'},
 @{requirement='Latest actual clock after loss and final N95 EXE/boot.dat readback';status='missing';evidence='Final EXE GET incomplete38608bytes; final boot.dat GET failed; stand stop means no actual clocks after loss'},
 @{requirement='Report with stock medians/spreads, source notes, config, skip reasons and raw per-run data';status='proved for completed stock measurements';evidence='f11-game-speed-2026-10-05.md; summary-progress.json'},
 @{requirement='Raw logs, screenshots and reusable states preserved';status='proved';evidence='current-run-audit.json; states hashes; screenshot files checked by this audit'},
 @{requirement='One current F6 link, measure/game-speed commit, no push';status='proved for previous commit5a5abc9; this audit pending commit';evidence='f6-compat-matrix.md; git branch/log'},
 @{requirement='No emulator code/build changes, autonomous reboot, boot.dat writes or L3 decrease';status='proved by preserved command logs and unchanged source diff';evidence='All clock actions through UI; logs show L3only4->6. Final boot.dat equality unavailable; no write commands issued'}
)
[ordered]@{verifiedAt=(Get-Date).ToString('o');complete=$false;acceptedRuns=$runs.validRuns;canonicalStates=$runs.stateChecks.Count;stockClockChecks=$runs.clockChecks.Count;restoredReadbacks=$restored;screenshotFiles=$shots;releaseProofs=$exeProofs;clockTreeSource='https://android.googlesource.com/kernel/common/+/320f5ea0cedc08ef65d67e056bcb9d181386ef2c/arch/arm/mach-omap2/clock2420_data.c';clockTreeSourceSha256=(Get-FileHash $source -Algorithm SHA256).Hash;staticDirectParents=$domains;staticClockTreeIsNotLiveRegisterProof=$true;requirements=$checks;remaining=@('Restore external access to N95','Verify all non-CPU domains, including IVA, at stock or lower via supported UI','Complete remaining valid cpu50 triples and before/after clocks','Verify final clock, full N95 EXE and unchanged boot.dat')} | ConvertTo-Json -Depth 9 | Set-Content "$PSScriptRoot/completion-audit-current.json" -Encoding utf8
Write-Output "Completion unproven: $($runs.validRuns) runs, $($runs.stateChecks.Count) states, $($restored.Count) restored readbacks, $($shots.Count) screenshot references checked."
