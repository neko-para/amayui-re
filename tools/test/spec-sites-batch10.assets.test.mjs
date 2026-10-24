/** @env assets @kind gate @why 第十批新增的图案/技法绑定站点若有一处不对着 .lst 指令行首，规则正文的机器锚就失效了 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { REPO_ROOT } from '../lib/paths.mjs';
const LST = path.join(REPO_ROOT, 'corpus', 'disasm', 'files', 'AGE.EXE__dumped.sectfix.EXE.lst');
const SKIP = fs.existsSync(LST) ? false : '语料 .lst 不在场';
const SITES = [
  0x4b1829,
  0x4b1839,
  0x4b1a2b,
  0x4b1c08,
  0x4b1db9,
  0x4b1f99,
  0x4b21db,
  0x4b2402,
  0x4b263d,
  0x4b286f,
  0x4b2ab5,
  0x4b2cf5,
  0x4b2f35,
  0x4b3f4d,
  0x4b3f50,
  0x4b1872,
  0x4b187b,
  0x4b1fde,
  0x4b1ff5,
  0x4b288b,
  0x4b2893,
  0x4b1a7f,
  0x4b1e0d,
  0x4b206d,
   0x4b3cf4,
  0x4b3d1b,
  0x4b3d38,
  0x4b3d62,
  0x4b3d8c,
  0x4b351a,
  0x4b359b,
  0x4b3792,
  0x4a62a0,
];
function check(ea, lines) {
  const prefix = `.text:${ea.toString(16).toUpperCase().padStart(8, '0')}`;
  const idx = lines.findIndex((l) => l.startsWith(prefix));
  if (idx < 0) return '没有这一行';
  for (let k = idx; k < Math.min(idx + 12, lines.length); k += 1) {
    if (!lines[k].startsWith(prefix)) break;
    const body = lines[k].slice(prefix.length).trim();
    if (body === '' || body.startsWith(';')) continue;
    return null;
  }
  return '标号后没有指令';
}
test('★ CORE-OPS-SCENE-COMMAND-RECORDS 的图案/技法站点都在 .lst 指令行首', { skip: SKIP }, () => {
  const lines = fs.readFileSync(LST, 'utf8').split('\n');
  const bad = [];
  for (const ea of SITES) { const why = check(ea, lines); if (why) bad.push(`0x${ea.toString(16)}：${why}`); }
  assert.deepEqual(bad, []);
});
