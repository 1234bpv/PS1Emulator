param([switch]$Final)
$ErrorActionPreference='Stop'
& "$PSScriptRoot/update-progress.ps1" | Out-Null
$summary=Get-Content "$PSScriptRoot/summary-progress.json" -Raw | ConvertFrom-Json
$games=@(Get-Content "$PSScriptRoot/games.json" -Raw | ConvertFrom-Json)
$states=@(Get-Content "$PSScriptRoot/states/manifest.json" -Raw | ConvertFrom-Json)
$repo=Resolve-Path "$PSScriptRoot/../../.."
$target=Join-Path $repo 'docs/measurements/f11-game-speed-2026-10-05.md'
$lines=[Collections.Generic.List[string]]::new()
function Line([string]$value='') {$lines.Add($value)}
function Fmt($v) {([double]$v).ToString('F3',[Globalization.CultureInfo]::InvariantCulture)}
Line '# F11: швидкість ігор alpha 1.3, 2026-10-05'
Line
Line ("Стан на {0}: {1} дійсних прогонів, {2} завершених трійок. {3}" -f (Get-Date -Format 'yyyy-MM-dd HH:mm'),$summary.acceptedRuns,$summary.completedTriples,$(if($Final){'Серії завершені; причини пропусків наведені нижче.'}else{'Тестування триває; це проміжний звіт.'}))
Line
Line '## Умови'
Line
Line 'Дата: 2026-10-05, Europe/Helsinki. Гілка measure/game-speed, без push.'
Line 'Емулятор alpha 1.3, EXE 1 431 280 байтів, SHA-256'
Line '`4CE2F9720F00FA69A8E2C262B2CFED6393E9B4197AC6B8B80E6FF3E6F0099EE4`.'
Line 'EXE обох телефонів звірені повним GET. На C5 за дорученням користувача'
Line 'замінено старий EXE готовим релізом і відповідним C:\sys\hash\ps1stage1.exe'
Line '(SHA-1 962048C437A7D6770CE095C695EFC8A0829882C6). Код емулятора не змінювали й не перезбирали.'
Line
Line 'C5: Bluetooth A8448113EA78, COM12, прямий RFCOMM до SDP-служби'
Line 'PS1 deploy daemon, канал 13. N95: WiFi 192.168.3.22:2323.'
Line 'На картках однакові дванадцять образів; назви й розміри — games.json.'
Line 'C5 штатний. N95-stock: m=86, ядро 330240 кГц, шина 165120 кГц,'
Line 'L3=4, CM_CLKSEL1_CORE=04360644. Повні read-only ocprobe-журнали'
Line 'до й після кожної завершеної трійки закінчуються == end ==.'
$clockFiles=@(Get-ChildItem $PSScriptRoot -Filter '*clock*.log' | Where-Object {$_.Name -match 'N95-stock.*clock-(before|after)'})
foreach($f in $clockFiles){Line ('- ['+$f.Name+'](f11-game-speed-2026-10-05-evidence/'+$f.Name+')')}
Line
if(-not @($summary.series | Where-Object label -like 'N95-cpu50-*').Count){Line 'N95-cpu50 ще не застосовувався. N95 залишається stock до завершення всіх штатних серій.'}
Line 'Телефони не перезавантажували. BUS GUARD увімкнений; boot.dat не змінювали.'
Line
Line 'Sound=ON, volume=0, профілі вимкнено. STAR x2 після WAITFG за UID'
Line 'e0001002 дає vol step=0 перед clocks start; це перевіряється в кожному прогоні.'
Line 'Єдина змінна в шаблоні налаштувань — шлях до потрібного образу:'
Line
Line '```ini'
foreach($s in Get-Content "$PSScriptRoot/stock-template.txt"){Line $s}
Line '```'
Line
Line '## Метод і сцени'
Line
Line 'Рівно 1200 фактичних кадрів PS1 після LoadState; початкові VBlank не включені.'
Line 'Регіон із reset у JSONL: NTSC → 60, PAL → 50 кадрів за секунду.'
Line 'Швидкість = 1200 / (gui_ms / 1000) / частоту регіону × 100%.'
Line 'Час береться з gui: frames=1200 ms=…, а не з тривалості сценарію.'
Line 'Окремий emuMs реліз без PS1_BENCH не журналює: н/д.'
Line
Line 'Показані FPS = сума пʼяти лічильників dsa rows / 240 / (gui_ms / 1000).'
Line 'Перевірений шлях: active1 gl0 pixB4 covered1 fbOk1 rowCopy0 scr320×240 fxaa0.'
Line 'Це частота блітів емулятора, включно з повторними оновленнями; унікальність'
Line 'кожного зображення не доведена. screen presented рахує кадри PS1 й не використовується як видимий FPS.'
Line 'Серія не робить SHOT, діагностичних дампів чи збережень у вимірюваному інтервалі.'
Line
Line '| Гра | Стан / логічний loadframe | SHA-256 | Доказ сцени |'
Line '|---|---|---|---|'
$fallback=@{gt='resume-c5-gt-late.png; resume-n95-gt-late.png';crash='C5-crash-verify.png; N95-stock-crash-verify.png';wipeout='N95-wipeout-driven2.png; N95-stock-wipeout-verify.png'}
foreach($s in $states){$proof=@($s.shot,$s.earlyShot,$s.navigationShot,$s.proof,$fallback[$s.game]) | Where-Object {$_}; Line ("| {0} | {1} / {2} | {3} | {4} |" -f $s.game,$s.file,$s.loadFrame,$s.sha256,($proof -join '; '))}
Line
Line 'Кожний стан прийнято після savestate rc=0 і візуального підтвердження gameplay.'
Line 'Для нових сцен зберігається також фактичний framebuffer третього кадру після load.'
Line 'Діагностика dumpat після читання малює тестовий візерунок; у Rayman він лишається'
Line 'на пізньому SHOT. Тому доказ там — ранній framebuffer і окремий SHOT без dumpat.'
Line 'C5 іноді має портретний SHOT 240×320 із обрізаним правим краєм; оригінальні BMP збережено.'
Line
Line '## Результати'
Line
Line 'У клітинках — медіана трьох прогонів: показані FPS / швидкість PS1. «—» — завершеної дійсної трійки ще немає.'
Line
Line '| Гра | Регіон | Рідні FPS | C5 | N95-stock | N95-cpu50 |'
Line '|---|---|---|---|---|---|'
$native=@{gt='30';crash='30';wipeout='30';colony='30, висновок¹'}
foreach($g in $games){$cells=@();foreach($dev in @('C5','N95-stock','N95-cpu50')){$s=@($summary.series | Where-Object {$_.label -eq "$dev-$($g.key)" -and $_.repeatCount -eq 3});$cells+=if($s.Count){"$(Fmt $s[0].fpsMedian) / $(Fmt $s[0].speedMedian)%"}else{'—'}};$reg=if(@($summary.series | Where-Object label -like "*-$($g.key)").Count){(@($summary.series | Where-Object label -like "*-$($g.key)" | Select-Object -ExpandProperty region -Unique) -join ',')}else{'ще не звірено'};$nat=if($native.ContainsKey($g.key)){$native[$g.key]}else{'не встановлено'};Line ("| {0} | {1} | {2} | {3} |" -f $g.image,$reg,$nat,($cells -join ' | '))}
Line
Line '| Серія | gui_ms, повтори 1/2/3 | Показані FPS min–max | Швидкість PS1 min–max |'
Line '|---|---|---|---|'
foreach($s in $summary.series){Line ("| {0} ({1}/3) | {2} | {3}–{4} | {5}–{6}% |" -f $s.label,$s.repeatCount,($s.ms -join '/'),(Fmt $s.fpsMin),(Fmt $s.fpsMax),(Fmt $s.speedMin),(Fmt $s.speedMax))}
Line
Line 'Різниці в межах розкиду не трактуються як виграш. Повна швидкість означає 100% PS1.'
Line
Line 'Рідні FPS: GT 30 — задано в дорученні; Crash 30 — [інтервʼю Andy Gavin](https://arstechnica.com/video/watch/war-stories-how-crash-bandicoot-hacked-the-playstation-to-run/);'
Line 'WipEout 30 — [опис оригінального коду автором його перенесення](https://phoboslab.org/log/2023/08/rewriting-wipeout).'
Line '¹ Colony Wars 30 — висновок із опису успадкованого рушія попередніх ігор, зафіксованого'
Line 'на двох відеокадрах (30 NTSC / 25 PAL), у [Julian Gold, postmortem Red Sun, Game Developer серпень 2000, с.54–55](https://media.gdcvault.com/GD_Mag_Archives/GDM_August_2000.pdf).'
Line 'Це не прямий замір оригінального диска. Пошуки інших ігор не дали чисельного первинного'
Line 'джерела; VBlank 60 і FPS пізніших портів не підмінюють рідну частоту PS1. Нотатки — native-fps-sources.txt.'
Line
Line '## Стенд, відхилені результати й залишок'
Line
Line 'Три попередні C5 Castlevania зі стану frame15000 виключені: раннє повторне'
Line 'завантаження на N95 показало вступ із вежами замку. Артефакти — rejected-cast-intro/.'
Line 'Новий стан frame17500 показує Richter із HUD на сходах і використовується для нових серій.'
Line 'Nav4 Cast не зберегла потрібний кадр; попередній bench не прийнято як новий стан.'
Line
Line 'Попередні великі GET C5 через віртуальний COM втрачали блоки по 1008 байтів'
Line 'і чекали оголошеного розміру до таймауту. Це не означало відʼєднання телефона:'
Line 'INFO відповідав. Прямий native RFCOMM дав повний початковий bench 1713024 байти'
Line 'з SHA256 8AA4F9A…62D13 і повний EXE з потрібним SHA256. Локальні PC-скрипти'
Line 'збережені; оригінальний deploy_net.ps1 не змінено.'
Line 'WAIT зависав після виходу емулятора; відновлено лише демон командою RESTART.'
Line 'Далі застосовуються SLEEP і ENDAPP/KILL/PS. WAITFG використовує UID, бо назва PS1 збігалася з демоном.'
Line
Line 'Незавершені клітинки таблиці наразі очікують замірів; вони не позначені як остаточно пропущені.'
Line 'Ліміт навігації — 10 спроб на гру; збої стенду — максимум три послідовні спроби.'
Line
Line '## Повернення файлів і артефакти'
Line
Line 'Початкові налаштування, профілі, bench, картки памʼяті та input збережені в'
Line 'c5-backup/n95-backup із SHA256. Попередню фазу повернення перевірено 32/32'
Line 'після успішного native GET C5 bench. Цей аудит не підтверджує поточних файлів:'
Line 'для поточних серій знову встановлено тимчасові стани й налаштування.'
if(-not $Final){Line '**Остаточне повернення й зворотне SHA256-читання ще попереду.**'}
Line 'C5 після роботи залишиться з оновленим EXE; N95 після заключної cpu50-серії має залишитися cpu50.'
Line
Line 'Сирі журнали, SHOT і стани: plans/runs/2026-10-05_game-speed/.'
Line 'Копія для коміту — [evidence/](f11-game-speed-2026-10-05-evidence/);'
Line '[машинне зведення](f11-game-speed-2026-10-05-evidence/summary-progress.json).'
Line 'До завершення роботи копія evidence може відставати від поточного каталогу runs.'
Line
Line '## Висновок'
Line
Line 'Жодна завершена дійсна серія поки не досягла повної швидкості PS1.'
Line 'Внесок розгону ядра ще не виміряно. Нові прийняті стани придатні для повторних замірів і відеозйомки.'
[IO.File]::WriteAllLines($target,$lines,[Text.UTF8Encoding]::new($false))
$f6=Join-Path $repo 'docs/measurements/f6-compat-matrix.md'
$text=Get-Content $f6 -Raw
$text=[regex]::Replace($text,'(?m)^Нові заміри alpha 1\.3:.*$',"Нові заміри alpha 1.3: [F11, 2026-10-05](f11-game-speed-2026-10-05.md) — тестування триває; $($summary.acceptedRuns) дійсних прогонів, транспорт C5 через прямий RFCOMM відновлено.")
[IO.File]::WriteAllText($f6,$text,[Text.UTF8Encoding]::new($false))
Write-Output $target
