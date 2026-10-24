/** @env assets @kind gate @why 第三批新增规则的绑定站点若有一处不对着 .lst 指令行首，规则正文的机器锚就失效了（锚只能是 EA，EA 必须真是指令） */
/**
 * tools/test/spec-sites-batch3.assets.test.mjs
 *
 * 第三批新增的四条规则（SPC-POOL-MEMFLIP · CORE-FS-EXT-INDEX-KEY ·
 * CORE-OPS-TEXTURE-DRAW-CONSUMER · CORE-OPS-SCENE-COMMAND-RECORDS）的绑定站点。
 *
 * 判据：每个 EA 在 `.lst` 里有一行标号 `.text:XXXXXXXX`，且标号后紧跟至少一条**指令**
 * （不是空行 / 注释）。★ 这条守卫只钉"锚是机器站点"，不证明合同语义；
 * 语义证据由各规则的残差与实现守卫承担。
 *
 * ★ 用例名是**字面量**：data/spec/bindings.json 的 evidence 按用例名解析，
 *   用模板拼出来的名字在文件里找不到（解析器只做文本包含）。
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { REPO_ROOT } from '../lib/paths.mjs';

const LST = path.join(REPO_ROOT, 'corpus', 'disasm', 'files', 'AGE.EXE__dumped.sectfix.EXE.lst');
const SKIP = fs.existsSync(LST) ? false : '语料 .lst 不在场';

const POOL_MEMFLIP = [
  0x41aa50, 0x41aa73, 0x41aa8d, 0x41aa96, 0x41aa98, 0x41aad4, 0x41aad9, 0x41ab66,
  0x415192, 0x415199, 0x4151a8, 0x4151fd, 0x415341, 0x41536e, 0x41540e, 0x415b8c, 0x415b94,
];

const FS_EXT_INDEX_KEY = [
  0x40116a, 0x401180, 0x401198, 0x4011ac, 0x4011de, 0x401201, 0x40126c, 0x401294,
  0x45593c, 0x455948, 0x454adc, 0x4559fb, 0x4559fe, 0x455a0b, 0x455a10, 0x455a5a,
  0x40141a, 0x40144c, 0x401480,
];

const TEX_DRAW_CONSUMER = [
  0x4aeeb7, 0x4aeec9, 0x4aeece, 0x4aeed1, 0x4aefa6, 0x4aefab, 0x4aefd8, 0x4af15f, 0x4af17a,
  0x4a2d7a, 0x4a2d87, 0x4a2d8c, 0x4a2da5, 0x4a2dac, 0x4a2dbf, 0x4a2dc8,
  0x4a2e55, 0x4a2e99, 0x4a2ed5, 0x4a2eed, 0x4a2f53, 0x4a2f5b, 0x4a3570,
  0x4a33d2, 0x4a33e7, 0x4a33fd, 0x4a3404, 0x4acf97, 0x4ad6af,
];

const SCENE_COMMAND_RECORDS = [
  0x423f71, 0x4ade19, 0x4addbb, 0x4addfa, 0x4ade05, 0x4adec5,
  0x4adf09, 0x4adfc0, 0x4adfca,
  0x4af6ac, 0x4af6b1, 0x4af6ee, 0x4af759, 0x4af81a, 0x4af845, 0x4af855,
  0x4af8e9, 0x4af93e, 0x4afa1f, 0x4afa99, 0x4afaee, 0x4afbd5,
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

test('★ SPC-POOL-MEMFLIP 的站点都在 .lst 指令行首', { skip: SKIP }, () => {
  assertSites(POOL_MEMFLIP);
});

test('★ CORE-FS-EXT-INDEX-KEY 的站点都在 .lst 指令行首', { skip: SKIP }, () => {
  assertSites(FS_EXT_INDEX_KEY);
});

test('★ CORE-OPS-TEXTURE-DRAW-CONSUMER 的站点都在 .lst 指令行首', { skip: SKIP }, () => {
  assertSites(TEX_DRAW_CONSUMER);
});

test('★ CORE-OPS-SCENE-COMMAND-RECORDS 的站点都在 .lst 指令行首', { skip: SKIP }, () => {
  assertSites(SCENE_COMMAND_RECORDS);
});
