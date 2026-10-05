$ErrorActionPreference='Stop'
$runRoot=$PSScriptRoot
$repo=(Resolve-Path "$runRoot/../../..").Path
Copy-Item "$runRoot/stand-stop.json" "$runRoot/interrupted-cpu50-2026-10-05-2027/stand-stop.json" -Force
[ordered]@{stoppedAt=(Get-Date).ToString('o');reason='N95 unreachable again during first cpu50 Castlevania scenario after user powered on';acceptedRuns=72;completedTriples=24;cpu50AcceptedRuns=0;attempts=@('N95-cpu50-cast-r1-scenario.done.json','N95-return-connect-check2.done.json','N95-return-connect-check3-scan.done.json');lastVerifiedClock='N95-cpu50-cast-clock-before.log';lastVerifiedMultiplier=129;lastVerifiedCore='04360846';c5Restored=$true;n95CurrentFilesRestored=$false;taskComplete=$false;previousIncident='interrupted-cpu50-2026-10-05-2027';recoveredAt='2026-10-05T20:53:26+03:00';stockAfterReturn='N95-return-clock.log';recoveredFailedJsonl='N95-return-failed-cast.jsonl';nextAction='Human restores N95 access; restore original 18 files and verify actual registers before further work; no autonomous reboot or L3 decrease'} | ConvertTo-Json -Depth 5 | Set-Content "$runRoot/stand-stop.json" -Encoding utf8
& "$runRoot/audit-final.ps1" -RunsOnly
$report="$repo/docs/measurements/f11-game-speed-2026-10-05.md"
$t=Get-Content $report -Raw
$t=[regex]::Replace($t,'Стан на 2026-10-05 \d\d:\d\d:',('Стан на '+(Get-Date -Format 'yyyy-MM-dd HH:mm')+':'))
$t=$t.Replace('C:syshashps1stage1.exe','C:/sys/hash/ps1stage1.exe')
$note=@'
### Повторне відновлення і зупинка, 2026-10-05 21:05

Після повідомлення користувача про увімкнення N95 INFO о20:53:26 відповів на 192.168.3.22: tick1604352, startTick1556547, boot0, IDLE, PS ps1stage1=0. Новий uptime підтвердив перезапуск після попередньої сесії. Виконавець перезапуск не робив. [Отриманий журнал перерваного першого прогону](f11-game-speed-2026-10-05-evidence/N95-return-failed-cast.jsonl) закінчується loadstate17500 rc0 та clocks start, без shutdown, pass і exit: прогін недійсний. Перші артефакти з префіксом N95-cpu50-cast збережені в interrupted-cpu50-2026-10-05-2027/. Історичні посилання першого інциденту з такими іменами слід читати в цьому підкаталозі.

[Регістри після повернення](f11-game-speed-2026-10-05-evidence/N95-return-clock.log) штатні: m86, PLL00056400, MPU2, CORE04360644, L3=4. RESTORE STOCK не потрібний. Через ocgui повторно підготовлено m129, MPU/2, L3/6; KEEP ON EXIT = ТАК, APPLY і CONFIRM. Після перезапуску стрілки працювали у звичайній орієнтації; усі неприкладені рухи збережені знімками. [Повна перевірка після перемикання](f11-game-speed-2026-10-05-evidence/N95-return-cpu50-switch-clock.log): m129, PLL00081400, MPU2, CORE04360846, L3=6, ядро495360 кГц, шина165120 кГц. Перед серією probe3 підтвердив ті самі регістри.

Повторний Castlevania r1, proc4, знову втратив зв'язок після WAITFG та початку SLEEP86126. Три перевірки:

1. N95-cpu50-cast-r1-scenario: 21:01:43–21:05:20, сім `link lost while scripting - reconnecting ...`; GET trace завершився `ObjectDisposedException`, `System.Net.Sockets.NetworkStream`; exitCode1, timeout=false, межа220с.
2. N95-return-connect-check2: 21:05:27–21:05:29, `daemon not responding at 192.168.3.22 (explicit -Ip, not scanning; use -Ip scan to search)`; exitCode1.
3. N95-return-connect-check3-scan: 21:05:29–21:05:45, `no daemon found on the LAN - is it running and on WiFi?`; exitCode1.

Після трьох перевірок звернення зупинено. Повторення на тому самому етапі зафіксоване, але причину не встановлено: транспортні повідомлення самі не доводять аварію живлення чи процесора. Дійсних прогонів залишається72, cpu50=0. Післясерійні регістри, фінальний boot.dat та повернення18 файлів N95 недоступні. C5 не чіпали; його остаточне повернення чинне. Автономні REBOOT, зменшення L3 та запис boot.dat не виконувалися. Цей розділ описує останній стан; нижче збережена історія першої зупинки.

'@
$t=$t.Replace('## Стенд, відхилені результати й залишок',"## Стенд, відхилені результати й залишок`n`n$note")
[IO.File]::WriteAllText($report,$t,[Text.UTF8Encoding]::new($false))
Add-Content "$repo/work/state.md" "`n2026-10-05 21:05: повторний cpu50 Castlevania r1 proc4 утратив доступ після WAITFG/SLEEP. Сценарій exit1 ObjectDisposedException, INFO .22 exit1, scan exit1. Після трьох перевірок звернення зупинено. Дійсних прогонів72. C5 повернуто14/14; N9518 файлів ще не повернуто. Останні регістри m129 CORE04360846 з probe3; стан після втрати доступу невідомий. Після відновлення доступу спочатку повернути18 оригінальних файлів і перевірити регістри. Без автономного REBOOT, L3 decrease чи boot.dat write."
& "$repo/work/check-state.ps1"
