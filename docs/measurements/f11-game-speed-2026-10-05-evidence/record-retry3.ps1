$ErrorActionPreference='Stop'
$repo=(Resolve-Path "$PSScriptRoot/../../..").Path
$stock=& "$PSScriptRoot/check-clock-log.ps1" -Path "$PSScriptRoot/N95-retry3-clock.log"
$applied=& "$PSScriptRoot/check-clock-log.ps1" -Path "$PSScriptRoot/N95-retry3-switch-clock.log" -Multiplier 129 -Core 04690C46
$dsp=Get-Content "$PSScriptRoot/N95-retry3-dsp6.log" -Raw
if($dsp -notmatch 'CM_CLKSEL_DSP\s+@[^\r\n]*= 000023c6' -or $dsp -notmatch '== end =='){throw 'DSP6 register not verified'}
$restore=@(Get-Content "$PSScriptRoot/n95-final2-restoration-verification.json" -Raw | ConvertFrom-Json)
if($restore.Count -ne 18 -or @($restore | Where-Object {-not $_.identical}).Count){throw 'N95 restoration incomplete'}
[ordered]@{verifiedAt=(Get-Date).ToString('o');userChoice='non-CPU clocks must not exceed stock; lower clocks allowed';stockBefore=$stock;applied=$applied;cmClkselDsp='000023C6';dspKhz=165120;ssiKhz=165120;vlynqKhz=55040;l4Khz=82560;usbInterfaceKhz=82560;ivaDivider=3;ivaCompensated=$false;allNonCpuClocksVerifiedStockOrLower=$false;guiHasIvaControl=$false;ocguiExitedKeepOnExit=$true;gameStarted=$false;acceptedCpu50Runs=0;restoredN95Files=18;status='stopped-unreachable-before-game';finalClockAfterLossUnknown=$true} | ConvertTo-Json -Depth 6 | Set-Content "$PSScriptRoot/retry3-configuration-audit.json" -Encoding utf8
Copy-Item "$PSScriptRoot/stand-stop.json" "$PSScriptRoot/stand-stop-before-retry3.json" -Force
[ordered]@{stoppedAt=(Get-Date).ToString('o');reason='N95 unreachable after full GUI peripheral compensation, before any emulator run';acceptedRuns=72;completedTriples=24;cpu50AcceptedRuns=0;attempts=@('N95-retry3-exe-get.done.json','N95-retry3-boot-after-get.done.json','N95-retry3-connect-check3-scan.done.json');lastVerifiedClock='N95-retry3-switch-clock.log';lastVerifiedMultiplier=129;lastVerifiedCore='04690C46';lastVerifiedDsp='000023C6';c5Restored=$true;n95CurrentFilesRestored=$true;taskComplete=$false;nextAction='Restore access and read clocks; IVA cannot be compensated by installed GUI; do not repeat game launches under unverified peripheral condition; no L3 decrease or autonomous reboot'} | ConvertTo-Json -Depth 5 | Set-Content "$PSScriptRoot/stand-stop.json" -Encoding utf8
$note=@'
## Остання перевірка N95, 2026-10-05 22:20

**Початкові файли обох апаратів повернуто: C5 14/14, N95 18/18; SHA-256 збігаються.** [Зворотне читання N95](f11-game-speed-2026-10-05-evidence/n95-final2-restoration-verification.json) завершено о22:15:36, тимчасові input_prev та interp відсутні за STAT. Нижче збережено історичні повідомлення про попереднє неповернення; вони більше не описують стан файлів.

За новим дорученням користувача знову перевірено доступ. INFO .22 спершу не відповів, але scan знайшов N95 о22:13:50 з tick178401/startTick64815. Новий uptime свідчить про черговий перезапуск; виконавець його не робив. Read-only stock-регістри: PLL00056400, MPU2, CORE04360644; DSP000023C3. Усі початкові файли повернуто до наступних дій із тактуванням.

Повна перевірка виявила, що попередній CORE04360846 компенсував L3, але DSS1 був +12,5%, SSI і VLYNQ +50%; DSP /3 при фактичному CORE990720 також давав330240 замість220160 кГц. Ці попередні cpu50 спроби не відповідали новій вимозі користувача. Жодна не мала дійсного виміряного результату.

Користувач обрав: не перевищувати штатні частоти інших блоків; нижчі допустимі. Через ocgui, з покроковими SHOT, підготовлено MANUAL, m129, MPU/2, L3/6, L4/2, DSS1/12, DSS2=0, VLYNQ/18, SSI/6, USB=2, DSP/6, KEEP ON EXIT=ТАК. DSP застосовано окремо до зміни ЦП, потім APPLY/CONFIRM. ocgui штатно закрито з KEEP: його ShutdownL скасовує таймер DSP і зберігає частоти. L3 лише4→6; boot.dat не записували.

Підтверджені регістри — [CORE/PLL/MPU](f11-game-speed-2026-10-05-evidence/N95-retry3-switch-clock.log) і [окремий DSP після виходу з ocgui](f11-game-speed-2026-10-05-evidence/N95-retry3-dsp6.log): PLL00081400, MPU00000002, CORE04690C46, DSP000023C6. Історичний рядок `firmware math` у DSP-пробі використовує stock CORE660480, а фактичні частоти нижче розраховані з реального CORE990720.

| Блок | Штатно, МГц | Після зміни, МГц | Дільник |
|---|---:|---:|---:|
| ЦП |330,24|495,36|2|
| L3/SDRAM |165,12|165,12|6|
| L4 |82,56|82,56|2 від L3|
| DSS1 |110,08|82,56|12|
| DSP |220,16|165,12|6|
| SSI |220,16|165,12|6|
| VLYNQ |55,04|55,04|18|
| USB interface |82,56|82,56|2 від L3|
| IVA1 domain |220,16|330,24|3, не компенсований|

**Умову для всіх інших блоків повністю не виконано:** DSP-регістр містить окремий IVA[12:8]=3; встановлений ocgui не має керування цим полем. IVA1 є окремим ARM7-блоком, його джерело CORE, а подальший MPU-дільник2 також зберігає пропорційне перевищення. USB interface походить від L3, функціональний USB — від окремого48MHz джерела. Це підтверджує [первинний код дерева тактування OMAP2420, TI/Nokia](https://android.googlesource.com/kernel/common/+/320f5ea0cedc08ef65d67e056bcb9d181386ef2c/arch/arm/mach-omap2/clock2420_data.c). Повного аудиту всіх інших периферійних доменів не отримано. [Машинний опис](f11-game-speed-2026-10-05-evidence/retry3-configuration-audit.json) явно має allNonCpuClocksVerifiedStockOrLower=false. Гру не запускали.

Після зміни телефон утратив доступ ще під час підготовки, без запуску емулятора:

1. GET EXE 22:18:58–22:19:43: timeout=true, межа45с, лише38608 із1431280 байтів; фінальний EXE не звірений, частковий файл не є доказом.
2. GET boot.dat 22:19:43–22:19:45: `daemon not responding at 192.168.3.22`, exitCode1; фінальний boot.dat не отримано.
3. Scan 22:20:03–22:20:19: `no daemon found on the LAN - is it running and on WiFi?`, exitCode1.

Після трьох невдач звернення зупинено. Причина не встановлена; тепер збій зафіксовано без запуску гри, тому приписувати його Castlevania не можна. Поточні регістри після втрати доступу невідомі. Дані залишаються72 штатні прогони, cpu50=0. Нові параметри не підміняють попередню конфігурацію у звіті. Код емулятора не змінювали, REBOOT і зменшення L3 не виконували. Наступні дії потребують відновлення доступу та способу перевірити/компенсувати IVA через дозволений UI.

'@
$report="$repo/docs/measurements/f11-game-speed-2026-10-05.md"
$text=Get-Content $report -Raw
$text=[regex]::Replace($text,'Стан на 2026-10-05 \d\d:\d\d:.*?(\r?\n)',('Стан на '+(Get-Date -Format 'yyyy-MM-dd HH:mm')+': 72 дійсні штатні прогони. N95 знову недоступний; файли обох апаратів повернуто. Завдання не завершено.'+"`n"))
$text=$text.Replace('## Умови',($note+"`n## Умови"))
[IO.File]::WriteAllText($report,$text,[Text.UTF8Encoding]::new($false))
$f6="$repo/docs/measurements/f6-compat-matrix.md"
$matrix=Get-Content $f6 -Raw
$matrix=[regex]::Replace($matrix,'(?m)^Нові заміри alpha 1\.3:.*$','Нові заміри alpha 1.3: [F11, 2026-10-05](f11-game-speed-2026-10-05.md) — 72 штатні прогони; файли C5 і N95 повернуто; cpu50 не виміряно, N95 знову недоступний.')
[IO.File]::WriteAllText($f6,$matrix,[Text.UTF8Encoding]::new($false))
Add-Content "$repo/work/state.md" "`n2026-10-05 22:20: N95 знайдено scan, stock-перезапуск підтверджено. Повернуто18/18 оригінальних файлів і звіреноSHA256, input_prev/interp відсутні. Користувач дозволив нижчі периферійні частоти, заборонив перевищення stock. ЧерезUI виставленоm129 CORE04690C46 DSP000023C6, KEEP true, UIзакрито. DSP/SSI165120, DSS82560, VLYNQ55040, L3штатний165120. IVA3 окремий, GUIнепідтримуєкомпенсацію: всііншіблокиштатніабонижчі НЕпідтверджено. ГруНЕзапускали. GET EXEtimeout45s(38608bytes), GETbootdaemonunreachable, scanunreachable: після3стоп. Файлиповернуті, поточнірегістрипіслявтратидоступуневідомі;72valid,cpu50=0. БезREBOOT,L3decrease,bootwrite,emucodechanges."
& "$PSScriptRoot/audit-final.ps1" -RunsOnly
& "$repo/work/check-state.ps1"
