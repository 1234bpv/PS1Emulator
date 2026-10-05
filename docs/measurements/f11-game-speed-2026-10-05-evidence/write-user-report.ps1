param([switch]$Final)
$ErrorActionPreference='Stop'
& "$PSScriptRoot/update-progress.ps1" | Out-Null
$summary=Get-Content "$PSScriptRoot/summary-progress.json" -Raw | ConvertFrom-Json
$baseline=Get-Content "$PSScriptRoot/user-oc-baseline.json" -Raw | ConvertFrom-Json
$games=@(Get-Content "$PSScriptRoot/games.json" -Raw | ConvertFrom-Json)
$states=@(Get-Content "$PSScriptRoot/states/manifest.json" -Raw | ConvertFrom-Json)
$repo=(Resolve-Path "$PSScriptRoot/../../..").Path
if($Final){
 $audit=Get-Content "$PSScriptRoot/final-audit-useroc.json" -Raw | ConvertFrom-Json
 if(-not $audit.complete -or $summary.acceptedRuns -ne 108 -or $summary.completedTriples -ne 36){throw 'Complete audit required for final report'}
}
$lines=[Collections.Generic.List[string]]::new()
function Line([string]$value=''){$lines.Add($value)}
function Fmt($v){([double]$v).ToString('F3',[Globalization.CultureInfo]::InvariantCulture)}
function Evidence([string]$file){return "[${file}](f11-game-speed-2026-10-05-evidence/$file)"}
Line '# F11: швидкість ігор alpha 1.3, серії від 2026-10-05'
Line
$status=if($Final){'Завершено з конфігурацією розгону, встановленою користувачем за останнім дорученням.'}else{'Триває тестування конфігурації розгону, встановленої користувачем.'}
Line ("Стан на {0}: {1} дійсних прогонів, {2} завершених трійок. {3}" -f (Get-Date -Format 'yyyy-MM-dd HH:mm'),$summary.acceptedRuns,$summary.completedTriples,$status)
Line
Line '## Умови'
Line
Line 'Початок серій: 2026-10-05, Europe/Helsinki. Гілка measure/game-speed; без push. Емулятор alpha 1.3, EXE 1 431 280 байтів, SHA-256 `4CE2F9720F00FA69A8E2C262B2CFED6393E9B4197AC6B8B80E6FF3E6F0099EE4`. EXE обох апаратів перевірено повним GET. Код емулятора не змінювали й не перезбирали.'
Line 'C5: Bluetooth A8448113EA78, SDP-канал13, прямий RFCOMM через локальний клієнт COM12. N95: WiFi 192.168.3.22:2323, AP Huawei. Обидві картки мають ті самі 12 образів; назви та розміри зафіксовані в games.json.'
Line 'C5 працює штатно. N95-stock: m86, ЦП 330,24 МГц, шина 165,12 МГц, MPU/2, L3/4, CORE04360644. Усі 12 штатних серій завершені перед розгоном.'
Line 'За останнім дорученням користувача третя конфігурація — його власні встановлені параметри, а не початковий cpu50. Їх виконавець не змінював: m111, ЦП 426,24 МГц, CORE 852,48 МГц, L3/4, шина 213,12 МГц, DSS1/6 = 142,08 МГц, DSP/3 = 284,16 МГц. Вищі частоти шини й периферії є частиною цієї обраної конфігурації. Попередню вимогу не перевищувати периферійні частоти замінило пряме доручення тестувати параметри користувача.'
Line ('Повний початковий набір: '+(Evidence 'user-oc-baseline.json')+'. Регістри: `CM_CLKSEL1_PLL=0006F400`, `CM_CLKSEL2_PLL=00000002`, `CM_CLKSEL_MPU=00000002`, `CM_CLKSEL1_CORE=04360644`, `CM_CLKSEL_DSP=000023C3`, `SDRC_RFR_CTRL0=0004DA01`, `SDRC_ACTIM_A_0=BADDC4C6`.')
Line 'Третя колонка позначена N95-useroc (m111). Початковий cpu50 m129 зі штатною шиною не виміряний: попередні спроби втрачали звʼязок і не мають прийнятих результатів. Дані нового режиму не підміняють cpu50.'
Line 'Тактування перевірене перед і після кожної трійки: stock — повними read-only engine-журналами; useroc — engine та DSP-журналами, з побітовим порівнянням семи регістрів проти початкового набору. HAL ECPUSpeed і довідкові stock-рядки ocprobe не використовувалися як поточні частоти. Під час цих нових серій виконувалися лише read-only clock probes; REBOOT і запис boot.dat не виконувалися.'
Line
Line 'Sound=ON, гучність 0, профілі ігор вимкнені. Після WAITFG e0001002 дві події зниження гучності дають vol step0 перед clocks start, що перевіряється в кожному JSONL. Початкові прогони використовували STAR×2 з паузою100мс; після пізнього другого натискання в Crash паузу прибрано. Варіант STAR/VOLDOWN не забезпечив нульову гучність до заміру, його прогони Crash і GT виключені. Окрема повторна спроба Crash із помилковим імʼям образу теж виключена; для повтору повернено канонічний образ і стан. Усі прийняті прогони підтверджені сирими журналами. Стокові налаштування; змінюється лише шлях до образу:'
Line '```ini'
foreach($row in Get-Content "$PSScriptRoot/stock-template.txt"){Line $row}
Line '```'
Line
Line '## Метод і сцени'
Line
Line 'Для кожної гри — один і той самий прийнятий gameplay-стан на всіх конфігураціях, по3 прогони рівно1200 фактичних кадрів PS1 після loadstate. Регіон береться з reset у журналі: NTSC60, PAL50. ШвидкістьPS1 =1200/(gui_ms/1000)/частоту регіону×100%. Час сценарію не є часом гри. emuMs у цьому релізі без PS1_BENCH — н/д.'
Line 'Показані FPS = сума пʼяти DSA rows/240/(gui_ms/1000): active1, gl0, pixB4, covered1, fbOk1, rowCopy0, scr320×240. Це частота блітів, унікальність кожного зображення не доведена. Лічильник screen presented не підміняє показаніFPS. Під час1200 кадрів немає SHOT, dump чи savestate. Очікування в сценарії має запас; для useroc після першого виміряного прогону воно скорочується з запасом20с, не змінюючи1200 кадрів.'
Line '| Гра | Стан / loadframe | SHA-256 | Доказ gameplay |'
Line '|---|---|---|---|'
$fallback=@{gt='resume-c5-gt-late.png; resume-n95-gt-late.png';crash='C5-crash-verify.png; N95-stock-crash-verify.png';wipeout='N95-wipeout-driven2.png; N95-stock-wipeout-verify.png'}
foreach($state in $states){$proof=@($state.shot,$state.earlyShot,$state.navigationShot,$state.proof,$fallback[$state.game]) | Where-Object {$_};Line ("| {0} | {1} / {2} | {3} | {4} |" -f $state.game,$state.file,$state.loadFrame,$state.sha256,($proof -join '; '))}
Line 'Стани прийняті після savestate rc0 та візуальної перевірки. Ранні framebuffers і окремі SHOT без dump використовуються там, де діагностичний dump малював тестовий візерунок. Частина C5 SHOT портретна з обрізаним краєм; оригінальніBMP збережені. Стани придатні для відеозйомки.'
Line
Line '## Результати'
Line
Line 'Медіани трьох прогонів: показаніFPS / швидкістьPS1. «—» означає, що трійка ще не завершена.'
Line '| Гра | Регіон | Рідні FPS | C5 | N95-stock | N95-useroc (m111) |'
Line '|---|---|---|---|---|---|'
$native=@{gt='30';crash='30';wipeout='30';colony='30, висновок¹'}
foreach($game in $games){
 $cells=@(foreach($device in @('C5','N95-stock','N95-useroc')){$series=@($summary.series | Where-Object {$_.label -eq "$device-$($game.key)" -and $_.repeatCount -eq 3});if($series.Count){"$(Fmt $series[0].fpsMedian) / $(Fmt $series[0].speedMedian)%"}else{'—'}})
 $nat=if($native.ContainsKey($game.key)){$native[$game.key]}else{'не встановлено'}
 $reg=(@($summary.series | Where-Object label -like "*-$($game.key)" | Select-Object -ExpandProperty region -Unique) -join ',')
 Line ("| {0} | {1} | {2} | {3} |" -f $game.image,$reg,$nat,($cells -join ' | '))
}
Line
Line '| Серія | gui_ms, повтори1/2/3 | Показані FPS min–max | ШвидкістьPS1 min–max |'
Line '|---|---|---|---|'
foreach($series in $summary.series){Line ("| {0} ({1}/3) | {2} | {3}–{4} | {5}–{6}% |" -f $series.label,$series.repeatCount,($series.ms -join '/'),(Fmt $series.fpsMin),(Fmt $series.fpsMax),(Fmt $series.speedMin),(Fmt $series.speedMax))}
Line 'Різниці в межах розкиду не трактуються як виграш. Повна швидкість означає100% PS1.'
Line
Line 'Рідні FPS: GT30 задано в дорученні; Crash30 — [інтервʼю Andy Gavin](https://arstechnica.com/video/watch/war-stories-how-crash-bandicoot-hacked-the-playstation-to-run/); WipEout30 — [опис оригінального коду автором перенесення](https://phoboslab.org/log/2023/08/rewriting-wipeout). ¹ Colony Wars30 — висновок з опису успадкованого рушія, два відеокадри на оновлення, у [Julian Gold, postmortem Red Sun, Game Developer2000, с.54–55](https://media.gdcvault.com/GD_Mag_Archives/GDM_August_2000.pdf); це непряме свідчення. Для інших ігор чисельних первинних джерел не знайдено; VBlank60 і частоти пізніших портів не підміняють рідніFPS. Пошуки збережені в native-fps-sources.txt.'
Line
Line '## Пропуски й відхилені спроби'
Line
if($Final){Line 'Для трьох підсумкових конфігурацій C5, N95-stock і N95-useroc пропущених ігор немає:12 × 3 × 3 = 108 дійсних прогонів. Третю конфігурацію змінив користувач. Початковий cpu50 не отримав дійсних результатів через втрату доступу; він не входить у підсумкові 108.'}else{Line 'Трійки N95-useroc послідовно доповнюються; незавершені клітинки очікують тестування.'}
Line 'Попередні Castlevania зі стану frame15000 виключені як вступ, а окремий C5-прогін з covered0/rgn5 — через BT-діалог. Прийнятий стан frame17500 і повторні трійки мають gameplay та covered1. Докази: rejected-cast-intro/ і rejected-cast-dsa-overlay/.'
Line 'Попередні m129/CORE04360846 і пізніший04690C46/DSP6 втрачали доступ. Останній збій виник до запуску гри; причину не встановлено. Після3 перевірок звернення зупиняли. Ці режими не мають accepted result. У наступному дорученні користувач установив власні параметри та попросив тестувати їх без зміни. Артефакти старих інцидентів і stand-stop snapshots збережено.'
Line 'Навігація мала ліміт10 спроб на гру; всі12 сцени отримано в межах ліміту. Великі C5 GET через віртуальнийCOM втрачали блоки; повний native RFCOMM дав потрібні SHA. Затримки запуску локальногоPowerShell і WAIT із мертвим process handle не включені у час емуляції. Оригінальний deploy_net.ps1 не змінено.'
Line
Line '## Регістри та повернення файлів'
Line
foreach($game in $games){foreach($side in @('before','after')){
 $stockName=if($game.key -eq 'gt' -and $side -eq 'before'){'resume-n95-clock-before.log'}else{"N95-stock-$($game.key)-clock-$side.log"}
 Line ('- '+(Evidence $stockName))
 $ocName="N95-useroc-$($game.key)-clock-$side"
 if(Test-Path "$PSScriptRoot/$ocName.json"){Line ('- '+(Evidence "$ocName-engine.log")+'; '+(Evidence "$ocName-dsp.log"))}
}}
if($Final){
 Line 'Початкові файли й профілі повернуто та звірено зворотним SHA-256-читанням: C5 14/14, N95 18/18. Початково відсутні input_prev/interp та C5 input відсутні заSTAT. C5 залишено з оновленим релізним EXE; тимчасову авторизацію KOMPUTER скасовано, PAVLO не змінювали.'
 Line ((Evidence 'c5-final2-restoration-verification.json')+'; '+(Evidence 'n95-final2-restoration-verification.json')+'; '+(Evidence 'c5-bt-final-restored.png'))
 Line 'EXE повторно прочитані й мають релізнийSHA-256. N95 залишено з параметрами користувача: m111, CORE04360644, DSP000023C3. Фінальні регістри перевірені після повернення файлів. boot.dat збігається заSHA-256 до й після цієї серії; його виконавець не записував.'
 Line ((Evidence 'N95-useroc-final-engine.log')+'; '+(Evidence 'N95-useroc-final-dsp.log')+'; '+(Evidence 'final-audit-useroc.json'))
}else{Line 'C5 уже повернуто 14/14. N95 буде повторно повернуто 18/18 після завершення нових серій; попередній аудит повернення не підтверджує тимчасові файли під час нових тестів.'}
Line 'Сирі журнали, знімки й стани — plans/runs/2026-10-05_game-speed/. Копія для коміту — evidence/; початкові EXE, образи ігор і повні резервні копії не перевидаються. Маніфести оригіналів включені.'
Line
Line '## Висновок'
Line
$full=@($summary.series | Where-Object {$_.repeatCount -eq 3 -and $_.speedMedian -ge 100})
Line ('За медіаною 100% досягли: '+$(if($full.Count){($full.label -join ', ')}else{'жодна завершена серія'})+'. Перевищення100% в межах розкиду не відрізняється від повної швидкості.')
Line '| Гра | Приріст швидкості N95-useroc до N95-stock | Розкид |'
Line '|---|---|---|'
foreach($game in $games){
 $stock=@($summary.series | Where-Object {$_.label -eq "N95-stock-$($game.key)" -and $_.repeatCount -eq 3})
 $oc=@($summary.series | Where-Object {$_.label -eq "N95-useroc-$($game.key)" -and $_.repeatCount -eq 3})
 if($stock.Count -and $oc.Count){$gain=100*($oc[0].speedMedian/$stock[0].speedMedian-1);$range=if($oc[0].speedMin -gt $stock[0].speedMax){'виграш перевищує розкид'}else{'діапазони перекриваються'};Line ("| {0} | +{1}% | {2} |" -f $game.key,(Fmt $gain),$range)}
}
Line 'Це приріст усієї встановленої конфігурації: ЦП і шина підняті на 29,07%, разом з іншими залежними доменами. Окремий внесок лише ядра цими даними не виділяється. Штатні серії зняті до ручних перезапусків, useroc — після них; вплив стану системи також не відокремлений від тактування.'
Line
Line '## Окремі прогони'
Line
Line 'У JSON за посиланням — точні gui/pass/Loaded CD Image; відповідний сирийJSONL має ту саму назву без .result. emuMs=н/д.'
Line '| Прогін | gui frames | gui ms | Показані кадри | error | Loaded bytes |'
Line '|---|---:|---:|---:|---:|---:|'
foreach($file in Get-ChildItem $PSScriptRoot -Filter '*.result.json' | Sort-Object Name){$r=Get-Content $file.FullName -Raw | ConvertFrom-Json;$size=[regex]::Match($r.loaded,'\((\d+) bytes\)').Groups[1].Value;Line ("| {0} | {1} | {2} | {3} | {4} | {5} |" -f (Evidence $file.Name),$r.frames,$r.ms,$r.shownFrames,$r.errors,$size)}
[IO.File]::WriteAllLines("$repo/docs/measurements/f11-game-speed-2026-10-05.md",$lines,[Text.UTF8Encoding]::new($false))
$f6="$repo/docs/measurements/f6-compat-matrix.md"
$text=Get-Content $f6 -Raw
$statusText=if($Final){'завершено108 прогонів:12 ігор на C5, N95-stock і встановленому користувачем N95-useroc(m111); файли повернуто'}else{"тестування N95-useroc(m111) триває; $($summary.acceptedRuns) дійсних прогонів"}
$text=[regex]::Replace($text,'(?m)^Нові заміри alpha 1\.3:.*$',"Нові заміри alpha 1.3: [F11, 2026-10-05](f11-game-speed-2026-10-05.md) — $statusText.")
[IO.File]::WriteAllText($f6,$text,[Text.UTF8Encoding]::new($false))
Write-Output 'User overclock report updated.'



