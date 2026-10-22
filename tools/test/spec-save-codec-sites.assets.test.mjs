/** @env assets @kind gate @why 存档编码 / 校验 / 布局的 handler 起点漂了 ⇒ FMT-SAVE-CODEC 的机器映射失效 */
/**
 * tools/test/spec-save-codec-sites.assets.test.mjs —— FMT-SAVE-CODEC 的站点守卫。
 * 只钉“EA 仍是 .lst 的指令行首”，不解释算法。
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';

import { REPO_ROOT } from '../lib/paths.mjs';

const LST = path.join(REPO_ROOT, 'corpus', 'disasm', 'files', 'AGE.EXE__dumped.sectfix.EXE.lst');
const SKIP = fs.existsSync(LST) ? false : '语料 .lst 不在场';
const SITES = [0x437480, 0x437980, 0x436d00, 0x436d50, 0x436e90, 0x436a80];

test('★ FMT-SAVE-CODEC 的读写 / 校验 / 变换 / 压缩站点都在 .lst 的指令行首', { skip: SKIP }, () => {
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
