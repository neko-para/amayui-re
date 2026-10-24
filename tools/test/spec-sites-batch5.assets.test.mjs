/** @env assets @kind gate @why 第五批新增规则的绑定站点若有一处不对着 .lst 指令行首，规则正文的机器锚就失效了 */
/**
 * tools/test/spec-sites-batch5.assets.test.mjs
 *
 * 第五批新增两条规则（场景显示适配 / 0x229 场景自身变换）的站点。
 * ★ 只钉"锚是机器站点"，不证明合同语义。
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { REPO_ROOT } from '../lib/paths.mjs';

const LST = path.join(REPO_ROOT, 'corpus', 'disasm', 'files', 'AGE.EXE__dumped.sectfix.EXE.lst');
const SKIP = fs.existsSync(LST) ? false : '语料 .lst 不在场';

const DISPLAY_FIT = [
  0x406b30, 0x406b31, 0x406b37, 0x498f32, 0x498760, 0x406982,
  0x406a09, 0x4978d5, 0x4978eb, 0x498edf, 0x498efa, 0x414104,
];

const SELF_TRANSFORM_229 = [
  0x423fe0, 0x423ff6, 0x424008, 0x424027, 0x424030, 0x42403c, 0x424048, 0x49a6f9,
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

function assertSites(sites) {
  const lines = fs.readFileSync(LST, 'utf8').split('\n');
  const bad = [];
  for (const ea of sites) {
    const why = check(ea, lines);
    if (why) bad.push(`0x${ea.toString(16)}：${why}`);
  }
  assert.deepEqual(bad, []);
}

test('★ CORE-SCENE-DISPLAY-FIT 的站点都在 .lst 指令行首', { skip: SKIP }, () => {
  assertSites(DISPLAY_FIT);
});

test('★ CORE-OPS-SCENE-SELF-TRANSFORM-229 的站点都在 .lst 指令行首', { skip: SKIP }, () => {
  assertSites(SELF_TRANSFORM_229);
});
