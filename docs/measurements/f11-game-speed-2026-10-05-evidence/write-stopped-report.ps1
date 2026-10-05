$ErrorActionPreference='Stop'
& "$PSScriptRoot/write-report.ps1" | Out-Null
$repo=(Resolve-Path "$PSScriptRoot/../../..").Path
$report="$repo/docs/measurements/f11-game-speed-2026-10-05.md"
$text=[IO.File]::ReadAllText($report)
$text=$text.Replace('Тестування триває; це проміжний звіт.','Зупинено: N95 перестав відповідати. Завдання не завершено; поточні файли N95 ще не повернуто.')
$text=$text.Replace('N95-cpu50 ще не застосовувався. N95 залишається stock до завершення всіх штатних серій.','N95-cpu50 застосовано й підтверджено перед першою спробою; дійсних cpu50-прогонів немає.')
$text=$text.Replace('Після фінального зворотного читання файлів цю авторизацію потрібно повернути; PAVLO не змінювали.','Після фінального зворотного читання файлів авторизацію KOMPUTER повернуто в початковий стан: lock зник; PAVLO не змінювали. Доказ — c5-bt-authorization-restored.json і c5-bt-final-restored.png.')
$text=$text.Replace('Незавершені клітинки таблиці наразі очікують замірів; вони не позначені як остаточно пропущені.','Пропущені через втрату доступу: N95-cpu50 для всіх дванадцяти ігор. Для завершення потрібне відновлення WiFi/демона, перевірка фактичних регістрів і продовження серій.')
$stop=@'
### Зупинка N95-cpu50, 2026-10-05 20:27

Усі 12 ігор виміряні на C5 і N95-stock: 72 дійсні прогони, 24 трійки. Після успішного переходу в cpu50 перший сценарій Castlevania втратив WiFi-зʼєднання. JSONL, повний trace та післясерійні регістри не отримано; прогін не прийнято. Перевірки:

1. N95-cpu50-cast-r1-scenario, 20:22:48–20:26:23: SCRIPT запущено, після WAITFG і SLEEP 86126 клієнт сім разів написав `link lost while scripting - reconnecting ...`; отримання журналу закінчилося `ObjectDisposedException`, `System.Net.Sockets.NetworkStream`, exitCode=1. Клієнт повернувся сам до межі 220 с.
2. INFO за 192.168.3.22, 20:26:53–20:26:55: `daemon not responding at 192.168.3.22 (explicit -Ip, not scanning; use -Ip scan to search)`, exitCode=1.
3. `-Ip scan -Device n95 -Info`, 20:27:26–20:27:42: сканування всіх мереж, включно з 192.168.3.0/24, закінчилося `no daemon found on the LAN - is it running and on WiFi?`, exitCode=1.

Після цих трьох перевірок звернення до телефона зупинено за правилом завдання. Причину втрати доступу не встановлено; зависання або перезавантаження телефона не доведені. Точні stdout/stderr і маркери мають префікси трьох перевірок вище; [машинний опис зупинки](f11-game-speed-2026-10-05-evidence/stand-stop.json).

Останні підтверджені регістри — probe102 перед Castlevania: m129, PLL00081400, CORE04360846, MPU00000002. Поточний стан після втрати доступу не перевірено. RESTORE STOCK, REBOOT, повторне перемикання й запис boot.dat не виконувалися. Сцени всіх дванадцяти ігор готові; жодну гру не пропущено через ліміт навігації.

'@
$text=$text.Replace('## Стенд, відхилені результати й залишок',"## Стенд, відхилені результати й залишок`n`n$stop")
$restoration=@'
## Повернення файлів і артефакти

**C5: остаточно повернуто й звірено 14/14 початкових файлів і профілів, SHA-256 ідентичні.** input, input_prev та interp відсутні за STAT. Релізний EXE повторно прочитаний: 1431280 байтів, SHA-256 4CE2F972…99EE4. C:syshashps1stage1.exe містить потрібні 20 байтів SHA-1 962048C4…9882C6. Тимчасову авторизацію KOMPUTER скасовано останнім BT-сеансом; SHOT і GET виконані в тому самому зʼєднанні. PAVLO лишився авторизованим.

[C5, 14 перевірок](f11-game-speed-2026-10-05-evidence/c5-final2-restoration-verification.json), [BT після повернення](f11-game-speed-2026-10-05-evidence/c5-bt-final-restored.png).

**N95: поточні початкові файли ще НЕ повернуто й остаточний стан не перевірено.** Оригінали 18 файлів збережені з SHA-256. Останніми на телефон встановлено тимчасові stock-Castlevania налаштування та cast-gameplay bench; input/interp після навігації також потребують повернення. Попередній аудит відновлення стосувався попередньої фази й не підтверджує теперішнього стану N95.

boot.dat прочитано до переходу: 68 байтів, SHA-256 3522F5C7A85D48D8807EE8E776CCC73999CBA8C70611EF66FA106C7CF1003FF3. Його не записували; фінальний повторний GET недоступний. Останнє підтверджене тактування N95 — cpu50; після втрати доступу його не чіпали.

Сирі журнали, SHOT і стани: plans/runs/2026-10-05_game-speed/. Копія для коміту — [evidence/](f11-game-speed-2026-10-05-evidence/), [машинне зведення](f11-game-speed-2026-10-05-evidence/summary-progress.json), [аудит 72 прогонів](f11-game-speed-2026-10-05-evidence/current-run-audit.json). Архів оновлено й перевірено за SHA-256; код емулятора не змінено.

'@
$text=[regex]::Replace($text,'(?s)## Повернення файлів і артефакти.*?(?=## Висновок)',$restoration)
$raw=[Collections.Generic.List[string]]::new()
$raw.Add('## Окремі прогони')
$raw.Add('')
$raw.Add('У кожному посиланні на JSON збережені точні gui/pass/Loaded CD Image; сирий JSONL має ту саму назву без .result. emuMs для всіх прогонів н/д у цьому релізі. Розміри завантажених образів перевірені проти games.json.')
$raw.Add('')
$raw.Add('| Прогін / точні рядки | gui: frames | gui: ms | Показані кадри | error | Loaded bytes |')
$raw.Add('|---|---:|---:|---:|---:|---:|')
foreach($f in Get-ChildItem $PSScriptRoot -Filter '*.result.json' | Sort-Object Name){
 $r=Get-Content $f.FullName -Raw | ConvertFrom-Json
 $size=[regex]::Match($r.loaded,'\((\d+) bytes\)').Groups[1].Value
 $raw.Add("| [$($r.label)-r$($r.repeat)](f11-game-speed-2026-10-05-evidence/$($f.Name)) | $($r.frames) | $($r.ms) | $($r.shownFrames) | $($r.errors) | $size |")
}
$text+="`n"+($raw -join "`n")+"`n"
[IO.File]::WriteAllText($report,$text,[Text.UTF8Encoding]::new($false))
$f6="$repo/docs/measurements/f6-compat-matrix.md"
$matrix=[IO.File]::ReadAllText($f6)
$matrix=[regex]::Replace($matrix,'(?m)^Нові заміри alpha 1\.3:.*$','Нові заміри alpha 1.3: [F11, 2026-10-05](f11-game-speed-2026-10-05.md) — 72 штатні прогони для всіх 12 ігор; cpu50 зупинено через недосяжність N95, поточні файли N95 ще не повернуто.')
[IO.File]::WriteAllText($f6,$matrix,[Text.UTF8Encoding]::new($false))
Write-Output $report
