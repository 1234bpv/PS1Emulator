const fs = require('node:fs');
const path = require('node:path');
const cp = require('node:child_process');
const crypto = require('node:crypto');
const root = path.resolve(__dirname, '../../..');
const evidence = path.join(root,'docs/measurements/f11-game-speed-2026-10-05-evidence');
const note = `### Зупинка продовження: локальний PowerShell, 2026-10-05 15:56

Прийнято 48 прогонів у 16 трійках для восьми ігор на C5/N95-stock.
Нових виміряних рядків після цього немає. N95 лишається stock; cpu50
не застосовано. Оригінальні файли після поточного тестування ще НЕ
повернено; попередні аудити відновлення стосуються попередньої фази.
Тимчасову авторизацію KOMPUTER у Bluetooth C5 також ще не повернено.

Зупинка за правилом трьох спроб усунення збою стенду: локальний запуск
pwsh без профілю, Windows PowerShell без профілю (звичайна/encoded проста
команда Write-Output 123) і окремий дочірній Windows PowerShell із
PSModulePath лише на наявний каталог Windows не дали stdout/stderr.
Остання перевірка обмежена 15 с: ETIMEDOUT, SIGTERM; точний маркер
shell-startup-check.json. cmd, Node та читання D: працюють. Причину
затримки PowerShell не встановлено; вона не доводить відключення C5.
Нові підключення до телефонів не виконувались після діагностики.

C5 Trek nav2/proc49 зберегла frame14000, ранній FB verification/proc50
показав корабель/HUD та навчання R2. Nav3/proc51 зберегла frame23000
rc=0 size1835418; GET повний, shutdown frames3001 ms74490, exit0.
Клієнт сценарію перевищив 330 с; локального повного scenario.log немає.
Цей стан НЕ прийнятий у manifest без раннього FB/SHOT після завантаження.
Після відновлення ПК перевірити живі процеси C5 перед запуском proc52.

N95 Colin nav6/proc65 зберегла frame42500, nav7/proc66 — frame52000
rc=0 size1173050. Обидва SHOT nav7 показують карту/підготовку до етапу,
а не gameplay; стан не прийнятий. Наступна навігація — спроба8/10,
proc67: перейти з Information на вкладку Race, потім почати етап.
Trek3/10, Colin7/10; інші лічильники попереднього розділу незмінні.

Наступна робота після відновлення локального PowerShell: INFO/PS обох
телефонів, перевірка Trek23000 та Colin навігація8; завершити stock,
лише тоді cpu50. Перед остаточним завершенням повернути файли і звірити
SHA256 зворотним читанням, повернути C5 KOMPUTER до неавторизованого
стану останньою дією, N95 після cpu50 не повертати до stock.

`;
let state=fs.readFileSync(path.join(root,'work/state.md'),'utf8');
state=state.replace('### Продовження F11, 2026-10-05 — тестування триває',note+'### Продовження F11, 2026-10-05 — журнал до локального збою');
fs.writeFileSync(path.join(root,'work/state.md'),state);
const reportFile=path.join(root,'docs/measurements/f11-game-speed-2026-10-05.md');
let report=fs.readFileSync(reportFile,'utf8');
const firstBreak=report.indexOf('\n');
report=report.slice(0,firstBreak+1)+'\n'+note+report.slice(firstBreak+1);
fs.writeFileSync(reportFile,report);
const f6=path.join(root,'docs/measurements/f6-compat-matrix.md');
let matrix=fs.readFileSync(f6,'utf8');
matrix=matrix.split('\n').map(line=>line.includes('f11-game-speed-2026-10-05.md')?line.replace(/тестування триває/g,'продовження зупинене через локальний збій PowerShell; відновлення файлів ще потрібне'):line).join('\n');
fs.writeFileSync(f6,matrix);
fs.mkdirSync(evidence,{recursive:true});
let copied=0;
for(const entry of fs.readdirSync(__dirname,{withFileTypes:true})) {
 if(entry.isFile() && (/\.(json|jsonl|txt|log|bmp|png|ps1|cs|cjs|rec)$/i.test(entry.name)||/-early\.bin$/i.test(entry.name))) {
  fs.copyFileSync(path.join(__dirname,entry.name),path.join(evidence,entry.name));copied++;
 }
}
const manifest=JSON.parse(fs.readFileSync(path.join(__dirname,'states/manifest.json'),'utf8'));
console.log('manifest shape',Array.isArray(manifest)?'array':Object.keys(manifest));
fs.writeFileSync(path.join(evidence,'pc-blocker-2026-10-05.md'),note);
fs.writeFileSync(path.join(evidence,'README.md'),`# Проміжні докази F11\n\n48 дійсних прогонів, 16 трійок, 8 прийнятих станів. Продовження зупинено через локальний збій PowerShell. N95-cpu50 не виміряно; початкові файли та Bluetooth-авторизацію C5 після поточної фази ще потрібно повернути.\n\nДив. pc-blocker-2026-10-05.md та основний звіт. Нові кандидати Trek/Colin не прийняті як gameplay.\n`);
fs.writeFileSync(path.join(evidence,'export-progress.json'),JSON.stringify({exportedAt:new Date().toISOString(),acceptedRuns:48,triples:16,states:8,rootFilesCopied:copied,status:'blocked-local-powershell',restorationPending:true},null,2));
// Only terminate exact diagnostic processes launched by this continuation.
const processRows=cp.spawnSync('wmic.exe',['process','get','ProcessId,CommandLine','/format:csv'],{encoding:'utf8',windowsHide:true}).stdout;
const terminated=[];
for(const pid of [12872,18096,15824,9432,21796]) {
 const row=processRows.split(/\r?\n/).find(line=>line.trim().endsWith(','+pid));
 if(row && /pwsh\.exe|powershell\.exe/i.test(row) && /Get-Date; Get-Process|publication.*TotalCount|Write-Output 123|VwByAGkAdABlAC0ATwB1AHQAcAB1AHQAIAAxADIAMwA=/.test(row)) {
  try{process.kill(pid);terminated.push(pid);}catch(e){console.log('diagnostic termination',pid,e.message);}
 }
}
fs.writeFileSync(path.join(__dirname,'pc-blocker-cleanup.json'),JSON.stringify({at:new Date().toISOString(),terminated,phonesTouched:false},null,2));
fs.copyFileSync(path.join(__dirname,'pc-blocker-cleanup.json'),path.join(evidence,'pc-blocker-cleanup.json'));
console.log(JSON.stringify({copied,terminated,status:'saved-local-powershell-blocker'}));
