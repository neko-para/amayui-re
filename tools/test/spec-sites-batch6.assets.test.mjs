/** @env assets @kind gate @why 第六批新增规则的绑定站点若有一处不对着 .lst 指令行首，规则正文的机器锚就失效了 */
/**
 * tools/test/spec-sites-batch6.assets.test.mjs
 *
 * 第六批新增规则（0x199 进入本地块）的站点。
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { REPO_ROOT } from '../lib/paths.mjs';

const LST = path.join(REPO_ROOT, 'corpus', 'disasm', 'files', 'AGE.EXE__dumped.sectfix.EXE.lst');
const SKIP = fs.existsSync(LST) ? false : '语料 .lst 不在场';

const ENTER_199 = [
  0x418fc0, 0x418fcd, 0x418fe0, 0x418feb, 0x418ff3, 0x419008, 0x41901f, 0x419045,
  0x41904d, 0x41905e, 0x41906b, 0x419076, 0x4190a5, 0x4190b5, 0x4190cb, 0x4190d1,
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

test('★ CORE-OPS-STATE-199 的站点都在 .lst 指令行首', { skip: SKIP }, () => {
  const lines = fs.readFileSync(LST, 'utf8').split('\n');
  const bad = [];
  for (const ea of ENTER_199) {
    const why = check(ea, lines);
    if (why) bad.push(`0x${ea.toString(16)}：${why}`);
  }
  assert.deepEqual(bad, []);
});
