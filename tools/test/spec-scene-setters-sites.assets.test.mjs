/** @env assets @kind gate @why 10 条场景 setter 的 handler 起点漂了 ⇒ 规则绑定的机器站点失效（正文自包含，校验器不替读者核这一条） */
/**
 * tools/test/spec-scene-setters-sites.assets.test.mjs —— 场景 10 条 setter 的 handler 起点守卫。
 *
 * 只钉“该 EA 仍然是 .lst 的指令行首”；不解释语义（语义在正文与 bindings）。
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';

import { REPO_ROOT } from '../lib/paths.mjs';

const LST = path.join(REPO_ROOT, 'corpus', 'disasm', 'files', 'AGE.EXE__dumped.sectfix.EXE.lst');
const SKIP = fs.existsSync(LST) ? false : '语料 .lst 不在场';

const HANDLERS = new Map([
  ['0x21d', 0x423c60],
  ['0x21e', 0x423ca0],
  ['0x21f', 0x423d40],
  ['0x230', 0x4243b0],
  ['0x231', 0x4243f0],
  ['0x233', 0x424510],
  ['0x234', 0x4245b0],
  ['0x235', 0x424630],
  ['0x239', 0x424900],
  ['0x242', 0x4251a0],
]);

test('★ 场景 10 条 setter 的 handler 起点都在 .lst 的指令行首', { skip: SKIP }, () => {
  const text = fs.readFileSync(LST, 'utf8');
  const lines = text.split('\n');
  const bad = [];
  for (const [op, ea] of HANDLERS) {
    const prefix = `.text:${ea.toString(16).toUpperCase().padStart(8, '0')}`;
    const idx = lines.findIndex((l) => l.startsWith(prefix));
    if (idx < 0) {
      bad.push(`${op} @0x${ea.toString(16)}：没有这一行`);
      continue;
    }
    // 函数入口可能先有几行空标号 / 注释；沿同一 EA 往下找第一条指令。
    let body = '';
    for (let k = idx; k < Math.min(idx + 12, lines.length); k += 1) {
      if (!lines[k].startsWith(prefix)) break;
      const b = lines[k].slice(prefix.length).trim();
      if (b === '' || b.startsWith(';')) continue;
      body = b;
      break;
    }
    if (body === '') bad.push(`${op} @0x${ea.toString(16)}：标号后没有指令`);
    else if (/^(?:loc_|def_|jpt_)/i.test(body)) bad.push(`${op} @0x${ea.toString(16)}：不是指令行首（${body}）`);
  }
  assert.deepEqual(bad, []);
});
