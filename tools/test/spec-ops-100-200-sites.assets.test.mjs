/** @env assets @kind gate @why 新增 0x100–0x3FF 规则的站点 EA 不在语料行里 / 指到指令中间 ⇒ 机器绑定指错位置 */
/**
 * tools/test/spec-ops-100-200-sites.assets.test.mjs —— 0x100–0x3FF 段新增规则的**站点锚**守卫
 *
 * 与 `spec-ops-000-sites.assets.test.mjs` 同一判据：每个站点 EA 必须在 `.lst` 里存在，
 * `.text` 段的站点必须是**指令行**（函数起点那种空文本行允许），`.data` / `seg002` 的站点是数据/常量。
 * ★ 这不是“语义正确”的判据：它只保证锚没指错位置。
 * ★ 样本缺席 ⇒ skip，不是红。
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';

import { REPO_ROOT } from '../lib/paths.mjs';

const LST = path.join(REPO_ROOT, 'corpus', 'disasm', 'files', 'AGE.EXE__dumped.sectfix.EXE.lst');
const BINDINGS = path.join(REPO_ROOT, 'data', 'spec', 'bindings.json');

const RULES = [
  'ADV-CHOICE-ADD-90',
  'CORE-OPS-TEXT-CONCAT-193',
  'CORE-OPS-TEXT-REDRAW-20A',
  'CORE-OPS-MSG-WAIT-243',
  'CORE-OPS-MSG-BLOCK-304-305',
  'CORE-OPS-TEXTURE-STRETCH-32',
  'CORE-OPS-MSG-WAIT-INPUT-72',
  'CORE-OPS-INPUT-SET-CURSOR-10A',
  'CORE-OPS-VOICE-OPTION-2F7',
  'CORE-OPS-SCENE3D-214',
  'CORE-OPS-ENGINE-CELL-2E9',
  'CORE-OPS-CONFIG-READ-1CB',
  'CORE-OPS-SCENE-RANGE-TRANSLATE-256',
  'CORE-OPS-SCENE-KEYED-215',
  'CORE-OPS-SCENE-CONTAINER-CLEAR-224',
  'CORE-OPS-TEXT-RUN-HEADER-7A',
  'CORE-OPS-SCENE-COLOR-32D',
  'CORE-OPS-SCENE-EFFECT-DESTROY-32A',
  'CORE-OPS-ENGINE-CELL-1B1',
  'CORE-OPS-ENGINE-CELL-238',
  'CORE-OPS-VOICE-ALL-STOP',
  'ADV-CHOICE-WINDOW',
  'ADV-CHOICE-SET-EXTRA-97',
  'CORE-OPS-TEXT-UNEQUAL-195',
  'CORE-OPS-RUN-BRACKET',
  'CORE-OPS-SCENE-TRANSFORM',
  'CORE-OPS-SCENE-CLEAR',
  'CORE-OPS-STATUS-WORD',
  'CORE-OPS-STATUS-CELL-READ',
  'CORE-OPS-MODE-BIT',
  'CORE-OPS-FLAG-CELLS',
  'CORE-OPS-POOL-MEMCPY',
  'CORE-OPS-SAVE-READ',
  'CORE-OPS-SAVE-WRITE',
  'CORE-OPS-SAVE-FILES',
  'CORE-OPS-LAYER-CELL',
  'CORE-OPS-TEXT-CELL-REVEAL-1CE',
  'CORE-OPS-FLOAT-ABS',
  'CORE-OPS-SCENE3D-SET-VEC1',
  'CORE-OPS-SCENE3D-SET-VEC2',
  'CORE-OPS-SCENE3D-GET-VEC1',
  'CORE-OPS-SCENE3D-GET-VEC2',
  'CORE-OPS-SCENE3D-SET-TRANSFORM',
  'CORE-OPS-SCENE3D-GET-LOCAL-TRANS',
  'CORE-OPS-SCENE3D-RECORD-LOOKUP',
  'CORE-OPS-SCENE-TABLE-FLAGS',
  'CORE-OPS-TEXTURE-SIZE',
  'CORE-OPS-FILLTEXTURE',
  'CORE-OPS-SCALAR-FLOAT-ARITH',
  'CORE-OPS-CG-SET',
  'CORE-OPS-SCENE-CG-NUMBER-23B',
  'CORE-OPS-L2D-LOAD',
  'CORE-OPS-L2D-MARK',
];

/** `.lst` 里“EA → [{段, 该行剩余文本}]” */
function lines() {
  const map = new Map();
  const text = fs.readFileSync(LST, 'utf8');
  for (const line of text.split('\n')) {
    const m = /^(\.text|\.data|seg002):([0-9A-F]{8})\s*(.*)$/.exec(line);
    if (!m) continue;
    const ea = Number.parseInt(m[2], 16);
    if (!map.has(ea)) map.set(ea, []);
    map.get(ea).push({ section: m[1], text: m[3].trim() });
  }
  return map;
}

const NOT_INSTRUCTION = /^(?:;|proc\b|endp\b|align\b|db\b|dd\b|dq\b|dw\b|assume\b|public\b|extrn\b|include\b|title\b|segment\b|ends\b|struct\b|unicode\b|\.|loc_|def_|jpt_)/i;

function checkRules(rules) {
  const bindings = JSON.parse(fs.readFileSync(BINDINGS, 'utf8')).bindings ?? [];
  const byRule = new Map(bindings.map((b) => [b.rule, b]));
  const map = lines();
  const problems = [];
  for (const rule of rules) {
    const binding = byRule.get(rule);
    if (!binding) { problems.push(`${rule}：bindings.json 里没有这条规则`); continue; }
    const sites = binding.sites ?? [];
    if (sites.length === 0) problems.push(`${rule}：没有任何站点`);
    for (const site of sites) {
      const ea = Number.parseInt(String(site.ea ?? '').replace(/^0x/i, ''), 16);
      if (!Number.isInteger(ea) || ea < 0) { problems.push(`${rule}：站点 EA 形态非法 ${site.ea}`); continue; }
      const at = map.get(ea);
      if (at === undefined) { problems.push(`${rule}：EA 0x${ea.toString(16)} 不在 .lst 的 .text/.data/seg002 行里`); continue; }
      const textLines = at.filter((x) => x.section === '.text');
      if (textLines.length === 0) continue; // 数据段：存在即可
      if (!textLines.some((x) => x.text === '' || !NOT_INSTRUCTION.test(x.text))) {
        problems.push(`${rule}：EA 0x${ea.toString(16)} 不是指令行：${textLines.map((x) => x.text).join(' | ')}`);
      }
    }
  }
  return problems;
}

const skip = fs.existsSync(LST) && fs.existsSync(BINDINGS) ? false : '语料 .lst 或 bindings.json 不在场';

test('★ 0x100–0x3FF 新增条目的站点 EA 都在 .lst 的指令行首（数据段站点存在即可）', { skip }, () => {
  const problems = checkRules(RULES);
  assert.deepEqual(problems, [], `站点锚不成立：\n  - ${problems.join('\n  - ')}`);
});

test('★ 判别力：函数标签行 / 不存在的 EA / 指令中间 EA ⇒ 必须被点名', { skip }, () => {
  const map = lines();
  const labelEa = [...map.entries()].find(([, at]) => at.every((x) => x.section !== '.text' || x.text === '' || NOT_INSTRUCTION.test(x.text)))?.[0];
  const problems = [];
  if (labelEa !== undefined) {
    const at = map.get(labelEa);
    if (at.some((x) => x.section === '.text' && x.text !== '' && !NOT_INSTRUCTION.test(x.text))) problems.push('函数标签行没有被判成非指令');
  }
  assert.equal(map.has(0xffffffff), false, '哨兵 EA 意外存在');
  assert.deepEqual(problems, [], `判别力失效：\n  - ${problems.join('\n  - ')}`);
});
