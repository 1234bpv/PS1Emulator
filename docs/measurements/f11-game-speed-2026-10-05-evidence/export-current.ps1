param([switch]$FinalUserOverclock)
$ErrorActionPreference='Stop'
if($FinalUserOverclock){
 $finalAudit=Get-Content "$PSScriptRoot/final-audit-useroc.json" -Raw | ConvertFrom-Json
 if(-not $finalAudit.complete -or $finalAudit.runs.Count -ne 108){throw 'Final user clock audit required'}
}
$repo=(Resolve-Path "$PSScriptRoot/../../..").Path
$destination="$repo/docs/measurements/f11-game-speed-2026-10-05-evidence"
New-Item -ItemType Directory -Force $destination | Out-Null
$copies=[Collections.Generic.List[object]]::new()
function Copy-Proof([string]$source,[string]$relative){
 $relative=$relative.Replace('\','/')
 $target=Join-Path $destination $relative
 New-Item -ItemType Directory -Force ([IO.Path]::GetDirectoryName($target)) | Out-Null
 [IO.File]::Copy($source,$target,$true)
 $sourceHash=(Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash
 $targetHash=(Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash
 if($sourceHash -ne $targetHash){throw "Export differs $relative"}
 $copies.Add([ordered]@{path=$relative;bytes=(Get-Item -LiteralPath $source).Length;sha256=$sourceHash})
}
foreach($entry in Get-ChildItem $PSScriptRoot -File){
 if($entry.Name -in @('export-verification.json','export-progress.json')){continue}
 if($entry.Name -match '\.(json|jsonl|txt|log|bmp|png|ps1|cs|cjs|rec)$' -or $entry.Name -match '-early\.bin$' -or $entry.Name -in @('final2-c5-sys-hash.bin','n95-boot-before.dat','useroc-boot-before.dat','useroc-boot-after.dat','report-before-useroc.md')){
  Copy-Proof $entry.FullName $entry.Name
 }
}
$states=@(Get-Content "$PSScriptRoot/states/manifest.json" -Raw | ConvertFrom-Json)
Copy-Proof "$PSScriptRoot/evidence.gitattributes" '.gitattributes'
Copy-Proof "$PSScriptRoot/states/manifest.json" 'states/manifest.json'
foreach($state in $states){
 $source="$PSScriptRoot/states/$($state.file)"
 if((Get-Item $source).Length -ne $state.bytes -or (Get-FileHash $source -Algorithm SHA256).Hash -ne $state.sha256){throw "Canonical state differs $($state.file)"}
 Copy-Proof $source "states/$($state.file)"
}
foreach($folder in @('rejected-cast-intro','rejected-cast-dsa-overlay','interrupted-cpu50-2026-10-05-2027','rejected-useroc-crash-late-volume','rejected-useroc-crash-volume-one','rejected-useroc-gt-late-volume','rejected-useroc-crash-wrong-image')){
 if(-not (Test-Path "$PSScriptRoot/$folder")){continue}
 foreach($file in Get-ChildItem "$PSScriptRoot/$folder" -File -Recurse){
  Copy-Proof $file.FullName ([IO.Path]::GetRelativePath($PSScriptRoot,$file.FullName))
 }
}
foreach($device in @('c5','n95')){Copy-Proof "$PSScriptRoot/$device-backup/manifest.json" "$device-backup/manifest.json"}
$summary=Get-Content "$PSScriptRoot/summary-progress.json" -Raw | ConvertFrom-Json
$restore=@(Get-Content "$PSScriptRoot/c5-final2-restoration-verification.json" -Raw | ConvertFrom-Json)
if($restore.Count -ne 14 -or @($restore | Where-Object {-not $_.identical}).Count){throw 'C5 restoration incomplete'}
$n95Restore=@(Get-Content "$PSScriptRoot/n95-final2-restoration-verification.json" -Raw | ConvertFrom-Json)
if($n95Restore.Count -ne 18 -or @($n95Restore | Where-Object {-not $_.identical}).Count){throw 'N95 restoration incomplete'}
$latestUpload=@(Get-ChildItem $PSScriptRoot -Filter 'N95-useroc-*-upload-*.done.json' | ForEach-Object {Get-Content $_.FullName -Raw | ConvertFrom-Json} | Where-Object exitCode -eq 0 | Sort-Object finished | Select-Object -Last 1)
$restoredAt=($n95Restore | Sort-Object verifiedAt | Select-Object -Last 1).verifiedAt
$pending=($latestUpload.Count -gt 0 -and [datetime]$latestUpload[0].finished -gt [datetime]$restoredAt)
if($FinalUserOverclock -and $pending){throw 'N95 uploaded measurement files after restoration'}
$status=if($FinalUserOverclock){'complete-user-installed-overclock'}elseif(Test-Path "$PSScriptRoot/user-oc-baseline.json"){'measuring-user-installed-overclock'}else{'stopped-n95-unreachable-before-game'}
$userRuns=@($summary.series | Where-Object label -like 'N95-useroc-*' | ForEach-Object repeatCount | Measure-Object -Sum).Sum
$proof=[ordered]@{verifiedAt=(Get-Date).ToString('o');acceptedResultFiles=$summary.acceptedRuns;completedTriples=$summary.completedTriples;canonicalStateHashes=$states;copiedFiles=$copies.Count;copies=$copies;c5RestoredFiles=14;c5BtAuthorizationRestored=$true;n95RestorationProvedFiles=18;n95RestorationPending=$pending;cpu50AcceptedRuns=0;userOverclockAcceptedRuns=$userRuns;taskComplete=$FinalUserOverclock.IsPresent;status=$status;emulatorSourceChanged=$false}
$proof | ConvertTo-Json -Depth 10 | Set-Content "$destination/export-verification.json" -Encoding utf8
[ordered]@{exportedAt=(Get-Date).ToString('o');acceptedRuns=$summary.acceptedRuns;triples=$summary.completedTriples;states=$states.Count;copiedFiles=$copies.Count;status=$status;c5Restored=$true;n95Restored=(-not $pending);n95RestorationPending=$pending} | ConvertTo-Json | Set-Content "$destination/export-progress.json" -Encoding utf8
@'
# Докази F11, стан на 2026-10-05 22:20

Останній стан: N95 знову знайшовся, початкові файли повернуто й звірено18/18. C5 також повернуто14/14. За новим вибором користувача периферійні частоти не повинні перевищувати stock, нижчі дозволені. Через UI перевірено m129 CORE04690C46 DSP000023C6: шина штатна, DSP/SSI/DSS нижчі, VLYNQ штатний. Окремий IVA3 не компенсований; цей ocgui не має такого керування. Умова всіх блоків не підтверджена, гру не запускали. Після перемикання GET EXE обірвався після38608байтів, GETboot іscan не знайшли демона. Після3 невдач звернення припинено. Поточні регістри після втрати доступу невідомі. retry3-configuration-audit.json і stand-stop.json описують поточний стан. Нових дійсних результатів немає. Нижче історія попередніх спроб; її повідомлення про неповернуті файли більше не чинні.

Після увімкнення N95 користувачем доступ повернувся, регістри були stock. Попередній неповний JSONL отримано; він не містить завершення прогону. Через ocgui повторно встановлено й перевірено cpu50. Повторний перший Castlevania знову втратив зв'язок на тому самому етапі; сценарій, INFO та scan завершилися помилками. Після трьох перевірок звернення зупинено. Нових дійсних результатів немає. Артефакти першого інциденту — interrupted-cpu50-2026-10-05-2027/, поточного — кореневі N95-cpu50-cast-* і N95-return-*. Відновлення18 файлів N95 досі недоступне. Наступний крок після повернення доступу — повернення оригінальних файлів і перевірка фактичних регістрів.

72 дійсні прогони, 24 трійки: усі 12 ігор на C5 і N95-stock. 12 прийнятих станів для повторення сцени й відеозйомки. N95 переведений у cpu50 (m129, CORE04360846, MPU2, L3=6, BUS165120), але перший сценарій утратив WiFi; жодного cpu50-прогону не прийнято. Після невдалих сценарію, INFO та сканування звернення до телефона припинено.

C5: 14/14 початкових файлів повернуто й звірено; релізний EXE та sys/hash перевірено; тимчасову авторизацію KOMPUTER скасовано. N95: оригінали 18 файлів збережені, поточне повернення і фінальне тактування після втрати доступу ще не перевірені. Завдання не завершене.

Основний звіт: ../f11-game-speed-2026-10-05.md. Поточні докази: summary-progress.json, current-run-audit.json, stand-stop.json, cpu50-clock-verified.json, c5-final2-restoration-verification.json, c5-bt-authorization-restored.json. export-verification.json містить SHA-256 усіх файлів поточного експорту. Історичні pc-blocker та проміжні журнали збережені для відтворення; їхній статус не описує поточний стан.

Оригінальні EXE, образи ігор, картки памʼяті та повні резервні копії не включені в архів; manifests резервних копій включені. Кандидати навігації не підмінюють прийняті states/manifest.json. Посібник Wing Commander для внутрішньої навігації не перевидається.
'@ | Set-Content "$destination/README.md" -Encoding utf8
Write-Output "Export verified: $($copies.Count) files, $($summary.acceptedRuns) runs, $($states.Count) canonical states."
if(Test-Path "$PSScriptRoot/user-oc-baseline.json"){
 $description=if($FinalUserOverclock){'Серії завершені:108 дійсних прогонів,36 трійок,12 ігор на C5/N95-stock/N95-useroc. Оригінальні файли повернуто32/32. Фінальні регістри користувача m111/CORE04360644/DSP000023C3, EXE та boot.dat перевірені.'}else{"Тестування N95-useroc триває: $($summary.acceptedRuns) прогонів, $($summary.completedTriples) трійок. Поточне повернення файлів N95 ще потрібне: $pending."}
 @"
# Докази F11 — параметри розгону користувача

$description

Останнє доручення користувача замінило початковий cpu50 на параметри, які він установив сам: m111, CPU426240kHz, CORE852480kHz, BUS213120kHz, DSS142080kHz, DSP284160kHz. Виконавець не змінював розгін під час цих серій. user-oc-baseline.json і N95-useroc-*-clock-* містять повні read-only перевірки регістрів до/після трійок. Це не режим cpu50 з початкового доручення; його невдалі спроби збережені окремо й не включені в accepted results.

Основний звіт: ../f11-game-speed-2026-10-05.md. Підсумковий аудит: final-audit-useroc.json після завершення. summary-progress.json містить медіани й розкид; states/manifest.json —12 перевірених reusable gameplay-станів. export-verification.json містить SHA-256 усіх файлів поточного експорту. report-before-useroc.md, stand-stop snapshots, retry3 та completion-audit-current — історія попередніх спроб; їхні статуси не описують нові серії.

Оригінальні EXE, образи ігор і повні резервні копії не перевидаються. Маніфести резервних копій включені. C5 BT-авторизація повернута; доказ c5-bt-final-restored.png. Кандидати навігації не підміняють прийняті стани. Посібник Wing Commander не перевидається.
"@ | Set-Content "$destination/README.md" -Encoding utf8
}


