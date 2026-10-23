/** @env assets @kind gate @why CORE-BOOT 的出口 / 清理 / 失败返回站点漂了 ⇒ 四条启动合同与语料的绑定失效 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { REPO_ROOT } from '../lib/paths.mjs';

const LST = path.join(REPO_ROOT, 'corpus', 'disasm', 'files', 'AGE.EXE__dumped.sectfix.EXE.lst');
const SKIP = fs.existsSync(LST) ? false : '语料 .lst 不在场';
const SITES = [0x413551, 0x413568, 0x413799, 0x4bb51c, 0x4bb555, 0x4ba8f5, 0x4ba927, 0x4bab63];

test('★ CORE-BOOT 四条规则的站点都在 .lst 的指令行首', { skip: SKIP }, () => {
  const lines = fs.readFileSync(LST, 'utf8').split('\n');
  const bad = [];
  for (const ea of SITES) {
    const prefix = `.text:${ea.toString(16).toUpperCase().padStart(8, '0')}`;
    const idx = lines.findIndex((l) => l.startsWith(prefix));
    if (idx < 0) { bad.push(`0x${ea.toString(16)}：没有这一行`); continue; }
    let body = '';
    for (let k = idx; k < Math.min(idx + 12, lines.length); k += 1) {
      if (!lines[k].startsWith(prefix)) break;
      const b = lines[k].slice(prefix.length).trim();
      if (b === '' || b.startsWith(';')) continue;
      body = b;
      break;
    }
    if (body === '') bad.push(`0x${ea.toString(16)}：标号后没有指令`);
  }
  assert.deepEqual(bad, []);
});
