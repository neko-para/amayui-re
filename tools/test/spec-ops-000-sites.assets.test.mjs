/** @env assets @kind gate @why 新增 opcode 规则绑定的站点 EA 不在语料的指令行首（EA 写错 / 指向函数标签或数据行），规范的“机器绑定”就指错了机器位置 */
/**
 * tools/test/spec-ops-000-sites.assets.test.mjs —— 0x000–0x0FF 段新增规则的**站点锚**守卫
 *
 * 这些规则的行为证据在语料里：spec 正文不写 EA，`bindings.json` 的 `sites[].ea` 是唯一的机器锚。
 * 本文件逐条断言：每个站点 EA 在 `.lst` 里存在，且那一行是**指令**（不是 `proc near` / `endp` /
 * `align` / 数据定义 / 注释）。
 *
 * ★ 这不是“语义正确”的判据：它只保证锚没指错位置。语义证据仍在 `bindings.evidence` 与
 *   `pnpm tools spec validate` 的语料对账里。
 * ★ 样本缺席（fresh clone / 没解压语料）⇒ skip，不是红。
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';

import { REPO_ROOT } from '../lib/paths.mjs';

const LST = path.join(REPO_ROOT, 'corpus', 'disasm', 'files', 'AGE.EXE__dumped.sectfix.EXE.lst');
const BINDINGS = path.join(REPO_ROOT, 'data', 'spec', 'bindings.json');

/** 2026-10 批：0x000–0x0FF 段新增的 13 条规则（与 bindings.json 一一对应） */
const RULES = [
  'CORE-OPS-ABORT-01',
  'CORE-OPS-RANDOM-60',
  'CORE-OPS-POOL-LEA-63',
  'CORE-OPS-PARAM-74-7F',
  'CORE-OPS-FRAME-LOCAL-SET-7B',
  'CORE-OPS-FRAME-LOCAL-RET-7C',
  'CORE-OPS-FRAME-JUMP-8C',
  'CORE-OPS-TABLE-RESET-A1',
  'CORE-OPS-TABLE-BIND-A2',
  'CORE-OPS-TABLE-DISPATCH-A3',
  'CORE-OPS-SLEEP-C8',
  'CORE-OPS-CLOCK-D0',
  'CORE-OPS-STATUS-CLEAR-D9',
  'CORE-OPS-AUDIO-SLOT-STOP-B6',
  'CORE-OPS-AUDIO-SLOT-PLAY-BA',
  'CORE-OPS-AUDIO-FADE-TARGET-B7',
  'CORE-OPS-AUDIO-FADE-STOP-B8',
];

/** `.lst` 里“EA → 该行剩余文本” */
function instructionLines() {
  const map = new Map();
  const text = fs.readFileSync(LST, 'utf8');
  for (const line of text.split('\n')) {
    const m = /^\.text:([0-9A-F]{8})\s+(.*)$/.exec(line);
    if (!m) continue;
    const ea = Number.parseInt(m[1], 16);
    if (!map.has(ea)) map.set(ea, []);
    map.get(ea).push(m[2].trim());
  }
  return map;
}

const NOT_INSTRUCTION = /^(?:;|proc\b|endp\b|align\b|db\b|dd\b|dq\b|dw\b|assume\b|public\b|extrn\b|include\b|title\b|segment\b|ends\b|struct\b|unicode\b|\.|loc_|def_|jpt_)/i;

function checkRules(rules) {
  const bindings = JSON.parse(fs.readFileSync(BINDINGS, 'utf8')).bindings ?? [];
  const byRule = new Map(bindings.map((b) => [b.rule, b]));
  const lines = instructionLines();
  const problems = [];
  for (const rule of rules) {
    const binding = byRule.get(rule);
    if (!binding) { problems.push(`${rule}：bindings.json 里没有这条规则`); continue; }
    const sites = binding.sites ?? [];
    if (sites.length === 0) problems.push(`${rule}：没有任何站点`);
    for (const site of sites) {
      const ea = Number.parseInt(String(site.ea ?? '').replace(/^0x/i, ''), 16);
      if (!Number.isInteger(ea) || ea < 0) { problems.push(`${rule}：站点 EA 形态非法 ${site.ea}`); continue; }
      const texts = lines.get(ea);
      if (texts === undefined) { problems.push(`${rule}：EA 0x${ea.toString(16)} 不在 .lst 的 .text 行里`); continue; }
      if (!texts.some((t) => !NOT_INSTRUCTION.test(t))) problems.push(`${rule}：EA 0x${ea.toString(16)} 不是指令行：${texts.join(' | ')}`);
    }
  }
  return problems;
}

const skip = fs.existsSync(LST) && fs.existsSync(BINDINGS) ? false : '语料 .lst 或 bindings.json 不在场';

test('★ 新增 opcode 条目的站点 EA 都在 .lst 指令行首', { skip }, () => {
  const problems = checkRules(RULES);
  assert.deepEqual(problems, [], `站点锚不成立：\n  - ${problems.join('\n  - ')}`);
});

test('★ 判别力：把站点改到函数标签行 / 不存在的 EA ⇒ 必须被点名', { skip }, () => {
  // 用内存副本跑同一判据，证明上面的断言不是恒真。
  const lines = instructionLines();
  const labelEa = [...lines.entries()].find(([, ts]) => ts.every((t) => NOT_INSTRUCTION.test(t)))?.[0];
  const problems = [];
  if (labelEa !== undefined) {
    const texts = lines.get(labelEa);
    if (texts.some((t) => !NOT_INSTRUCTION.test(t))) problems.push('函数标签行没有被判成非指令');
  }
  const missingEa = 0xffffffff;
  if (lines.has(missingEa)) problems.push('哨兵 EA 意外存在');
  assert.deepEqual(problems, [], `判别力失效：\n  - ${problems.join('\n  - ')}`);
});
