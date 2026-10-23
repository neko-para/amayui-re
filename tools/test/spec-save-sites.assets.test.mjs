/** @env assets @kind gate @why 新增 FMT-SAVE 规则的站点 EA 不在语料行里 / 指到指令中间 ⇒ 机器绑定指错位置 */
/**
 * tools/test/spec-save-sites.assets.test.mjs —— FMT-SAVE 规则的**站点锚**守卫
 * 判据同 `spec-ops-000-sites`：`.text` 段必须是指令行（函数起点空文本行允许），数据段存在即可。
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';

import { REPO_ROOT } from '../lib/paths.mjs';

const LST = path.join(REPO_ROOT, 'corpus', 'disasm', 'files', 'AGE.EXE__dumped.sectfix.EXE.lst');
const BINDINGS = path.join(REPO_ROOT, 'data', 'spec', 'bindings.json');
const RULES = ['FMT-SAVE-SLOT', 'FMT-SAVE-STH', 'FMT-SAVE-HEADER', 'FMT-SAVE-HEADER-FIELDS', 'FMT-SAVE-ENVELOPE', 'FMT-SAVE-BODY', 'FMT-SAVE-RESTORE'];

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

test('★ FMT-SAVE 规则的站点 EA 都在 .lst 的指令行首（数据段站点存在即可）', { skip }, () => {
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
