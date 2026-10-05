const fs = require('node:fs');
const cp = require('node:child_process');
const env = {...process.env, PSModulePath: 'C:\\Windows\\System32\\WindowsPowerShell\\v1.0\\Modules'};
const started = new Date().toISOString();
const run = cp.spawnSync('C:\\Windows\\System32\\WindowsPowerShell\\v1.0\\powershell.exe', ['-NoLogo', '-NoProfile', '-NonInteractive', '-Command', 'Write-Output 123'], {env, cwd:'C:\\Windows', timeout:15000, windowsHide:true, encoding:'utf8'});
const result = {started,finished:new Date().toISOString(),status:run.status,signal:run.signal,error:run.error?.message,stdout:run.stdout,stderr:run.stderr,change:'child-only PSModulePath restricted to existing Windows modules'};
fs.writeFileSync(__dirname+'/shell-startup-check.json',JSON.stringify(result,null,2));
console.log(JSON.stringify(result));
