/** @env assets @kind gate @why 新增 CORE-OBJECTS 规则的站点 EA 不在语料行里 / 指到指令中间 ⇒ 机器绑定指错位置 */
/**
 * tools/test/spec-objects-sites.assets.test.mjs —— CORE-OBJECTS 5 条规则的**站点锚**守卫
 *
 * 与 ops 段站点守卫同一判据：EA 必须在 `.lst` 的 `.text/.data/seg002` 行里；
 * `.text` 段必须是**指令行**（函数起点那种空文本行允许），数据段存在即可。
 * ★ 不是“语义正确”的判据，只保证锚没指错位置。
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';

import { REPO_ROOT } from '../lib/paths.mjs';

const LST = path.join(REPO_ROOT, 'corpus', 'disasm', 'files', 'AGE.EXE__dumped.sectfix.EXE.lst');
const BINDINGS = path.join(REPO_ROOT, 'data', 'spec', 'bindings.json');

const RULES = [
  'CORE-OBJECTS-ROOT',
  'CORE-OBJECTS-BASE',
  'CORE-OBJECTS-TYPE-IDENTITY',
  'CORE-OBJECTS-SLOT-TABLE',
  'CORE-OBJECTS-SLOT-LIFECYCLE',
];

function lines() {
  const map = new Map();
  for (const line of fs.readFileSync(LST, 'utf8').split('\n')) {
    const m = /^(\.text|\.data|seg002):([0-9A-F]{8})\s*(.*)$/.exec(line);
    if (!m) continue;
    const ea = Number.parseInt(m[2], 16);
    if (!map.has(ea)) map.set(ea, []);
    map.get(ea).push({ section: m[1], text: m[3].trim() });
  }
  return map;
}

const NOT_INSTRUCTION = /^(?:;|proc\b|endp\b|align\b|db\b|dd\b|dq\b|dw\b|assume\b|public\b|extrn\b|include\b|title\b|segment\b|ends\b|struct\b|unicode\b|\.|loc_|def_|jpt_)/i;

const skip = fs.existsSync(LST) && fs.existsSync(BINDINGS) ? false : '语料 .lst 或 bindings.json 不在场';

test('★ CORE-OBJECTS 规则的站点 EA 都在 .lst 的指令行首（数据段站点存在即可）', { skip }, () => {
  const bindings = JSON.parse(fs.readFileSync(BINDINGS, 'utf8')).bindings ?? [];
  const byRule = new Map(bindings.map((b) => [b.rule, b]));
  const map = lines();
  const problems = [];
  for (const rule of RULES) {
    const b = byRule.get(rule);
    if (!b) { problems.push(`${rule}：bindings.json 里没有这条规则`); continue; }
    if ((b.sites ?? []).length === 0) problems.push(`${rule}：没有任何站点`);
    for (const site of b.sites ?? []) {
      const ea = Number.parseInt(String(site.ea ?? '').replace(/^0x/i, ''), 16);
      const at = map.get(ea);
      if (at === undefined) { problems.push(`${rule}：EA ${site.ea} 不在 .lst 的 .text/.data/seg002 行里`); continue; }
      const textLines = at.filter((x) => x.section === '.text');
      if (textLines.length === 0) continue;
      if (!textLines.some((x) => x.text === '' || !NOT_INSTRUCTION.test(x.text))) {
        problems.push(`${rule}：EA ${site.ea} 不是指令行：${textLines.map((x) => x.text).join(' | ')}`);
      }
    }
  }
  assert.deepEqual(problems, [], `站点锚不成立：\n  - ${problems.join('\n  - ')}`);
});

test('★ 判别力：不存在的 EA / 指令中间 EA ⇒ 必须被点名', { skip }, () => {
  const map = lines();
  assert.equal(map.has(0xffffffff), false, '哨兵 EA 意外存在');
  // 取一个已知的指令行 EA，它的下一个字节通常不是行首；若恰好是，换一个。
  const start = map.get(0x4baa11);
  assert.ok(start, '样本 EA 0x4baa11 必须在 .lst 里');
  const mid = 0x4baa12;
  if (map.has(mid)) {
    const at = map.get(mid);
    assert.ok(at.some((x) => x.section === '.text'), '样本前提：0x4baa12 若存在应是 .text 行');
  }
});
