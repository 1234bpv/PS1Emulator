const fs=require('node:fs'),path=require('node:path'),crypto=require('node:crypto'),cp=require('node:child_process');
const root=path.resolve(__dirname,'../../..');
const evidence=path.join(root,'docs/measurements/f11-game-speed-2026-10-05-evidence');
const addendum=`Додаткова діагностика: два локальні читання зрештою завершилися
із затримкою приблизно 5–6 хвилин (15:51→15:56 та 15:52→15:58).
Це затримка запуску, а не доведена остаточна відмова PowerShell.
Свіжа проста команда о15:58 знову не дала відповіді за 10 с;
нормальний запуск у межах клієнтських таймаутів не відновлено.

Окремий C5 Cast proc38 відхилено через Bluetooth-діалог:
DSA covered=0 rgn=5, тому його FPS не прийнято. Повторна трійка
proc41–43 після тимчасової авторизації KOMPUTER має повну область DSA
і включена в 48 прогонів. Відхилені журнали збережено окремо.

`;
for(const file of [path.join(root,'work/state.md'),path.join(root,'docs/measurements/f11-game-speed-2026-10-05.md')]) {
 let text=fs.readFileSync(file,'utf8');
 text=text.replace('Наступна робота після відновлення локального PowerShell:',addendum+'Наступна робота після відновлення локального PowerShell:');
 fs.writeFileSync(file,text);
}
fs.appendFileSync(path.join(evidence,'pc-blocker-2026-10-05.md'),'\n'+addendum);
const hashes=[];
for(const item of JSON.parse(fs.readFileSync(path.join(__dirname,'states/manifest.json'),'utf8'))) {
 for(const base of [path.join(__dirname,'states'),path.join(evidence,'states')]) {
  const bytes=fs.readFileSync(path.join(base,item.file));
  const hash=crypto.createHash('sha256').update(bytes).digest('hex').toUpperCase();
  if(bytes.length!==item.bytes||hash!==item.sha256)throw Error('canonical state mismatch '+item.file+' '+base);
 }
 hashes.push({game:item.game,sha256:item.sha256,bytes:item.bytes});
}
const results=fs.readdirSync(__dirname).filter(n=>n.endsWith('.result.json'));
if(results.length!==48)throw Error('unexpected run count '+results.length);
const verification={verifiedAt:new Date().toISOString(),acceptedResultFiles:results.length,canonicalStateHashes:hashes,checkState:'last passed 2026-10-05 15:13, rerun unavailable due local startup delays',phoneRestoration:'pending'};
fs.writeFileSync(path.join(__dirname,'export-verification.json'),JSON.stringify(verification,null,2));
fs.copyFileSync(path.join(__dirname,'export-verification.json'),path.join(evidence,'export-verification.json'));
fs.copyFileSync(__filename,path.join(evidence,path.basename(__filename)));
function git(args){const out=cp.spawnSync('git',args,{cwd:root,encoding:'utf8',windowsHide:true,timeout:60000,maxBuffer:8*1024*1024});if(out.status!==0)throw Error(JSON.stringify({args,status:out.status,stderr:out.stderr,error:out.error?.message}));return out.stdout.trim();}
if(git(['branch','--show-current'])!=='measure/game-speed')throw Error('wrong branch');
if(git(['diff','--name-only','--','src','tests','tools-patches']))throw Error('production changes require separate review');
if(git(['diff','--cached','--name-only']))throw Error('unexpected previously staged changes');
git(['add','-f','--','docs/measurements/f11-game-speed-2026-10-05.md','docs/measurements/f11-game-speed-2026-10-05-evidence','docs/measurements/f6-compat-matrix.md','work/state.md']);
const staged=git(['diff','--cached','--name-only']).split('\n');
if(staged.some(n=>!(n==='work/state.md'||n==='docs/measurements/f6-compat-matrix.md'||n.startsWith('docs/measurements/f11-game-speed-2026-10-05'))))throw Error('unexpected staged file');
console.log(JSON.stringify({verifiedRuns:results.length,verifiedStates:hashes.length,stagedFiles:staged.length}));
console.log(git(['commit','-m','docs: record 48 gameplay runs and local PowerShell blocker']));
console.log('commit '+git(['rev-parse','--short','HEAD']));
console.log('status '+git(['status','--short']));
