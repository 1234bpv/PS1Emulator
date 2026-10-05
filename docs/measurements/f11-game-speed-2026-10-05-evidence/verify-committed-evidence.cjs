const fs = require('fs');
const path = require('path');
const crypto = require('crypto');
const cp = require('child_process');
const repo = path.resolve(__dirname, '../../..');
const prefix = 'docs/measurements/f11-game-speed-2026-10-05-evidence/';
const manifest = JSON.parse(fs.readFileSync(path.join(repo, prefix, 'export-verification.json'), 'utf8').replace(/^\uFEFF/, ''));
const requests = manifest.copies.map(item => `HEAD:${prefix}${item.path}`).join('\n') + '\n';
const result = cp.spawnSync('git', ['cat-file', '--batch'], {cwd: repo, input: requests, maxBuffer: 1024 * 1024 * 1024});
if (result.status !== 0) throw new Error(result.stderr.toString());
let offset = 0;
const failures = [];
for (const item of manifest.copies) {
  const end = result.stdout.indexOf(10, offset);
  if (end < 0) throw new Error('Truncated git batch header');
  const header = result.stdout.subarray(offset, end).toString('utf8');
  const match = /^([a-f0-9]{40}) blob (\d+)$/.exec(header);
  if (!match) throw new Error(`Missing blob: ${item.path}: ${header}`);
  const size = Number(match[2]);
  const data = result.stdout.subarray(end + 1, end + 1 + size);
  const digest = crypto.createHash('sha256').update(data).digest('hex').toUpperCase();
  if (size !== item.bytes || digest !== item.sha256) failures.push({path: item.path, expected: item.sha256, actual: digest, bytes: size});
  offset = end + 1 + size + 1;
}
const head = cp.execFileSync('git', ['rev-parse', 'HEAD'], {cwd: repo, encoding: 'utf8'}).trim();
const proof = {verifiedAt: new Date().toISOString(), commit: head, checkedFiles: manifest.copies.length, failures, verified: failures.length === 0};
fs.writeFileSync(path.join(__dirname, 'committed-evidence-verification.json'), JSON.stringify(proof, null, 2) + '\n');
process.stdout.write(JSON.stringify({commit: head, checkedFiles: proof.checkedFiles, mismatches: failures.length, verified: proof.verified}) + '\n');
if (failures.length) process.exitCode = 1;
