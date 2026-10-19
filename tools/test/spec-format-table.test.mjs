/** @env pure @kind gate @why 规范指令表形态坏掉 / 待核验状态被抹掉（读者会把未核验协议当已证实） */
/**
 * tools/test/spec-format-table.test.mjs —— **规范指令表**与协议核验状态的守卫
 *
 * `data/spec/format/opcode-table.json` 是**本规范自己的**指令表；当前内容来自现有实现，
 * 尚未逐条对引擎解码代码核验。它**不是** `age-format` 的 canonical 副本，本测试也不拿它做真源比对。
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';

import { REPO_ROOT } from '../lib/paths.mjs';

const TABLE = path.join(REPO_ROOT, 'data/spec/format/opcode-table.json');
const read = (p) => JSON.parse(fs.readFileSync(p, 'utf8'));

test('★ 规范指令表形态：数组 / opcode 唯一 / argc 是非负整数 / name 是字符串', () => {
  const table = read(TABLE);
  assert.ok(Array.isArray(table) && table.length > 0);
  const seen = new Set();
  for (const e of table) {
    assert.ok(Number.isInteger(e.opcode) && e.opcode >= 0, `opcode 非法：${JSON.stringify(e)}`);
    assert.ok(!seen.has(e.opcode), `opcode 重复：${e.opcode}`);
    seen.add(e.opcode);
    assert.ok(Number.isInteger(e.argc) && e.argc >= 0, `argc 非法：${JSON.stringify(e)}`);
    assert.equal(typeof e.name, 'string');
    if (e.aliases !== undefined) assert.ok(Array.isArray(e.aliases));
  }
});

test('★ 协议章指向规范表，并明确标出「待核验」', () => {
  const doc = fs.readFileSync(path.join(REPO_ROOT, 'data/spec/chapters/10-format.md'), 'utf8');
  assert.ok(doc.includes('opcode-table.json'), '格式章必须指向规范指令表');
  assert.ok(doc.includes('### 核验状态（部分待核验）'), '格式章必须把核验状态钉成 partial');
  assert.ok(doc.includes('待核验'), '格式章必须明确标出待核验');
  assert.ok(doc.includes('不是从 `age-format` 复制来的第二份真源'), '格式章必须声明规范表的所有权');
});

test('★ 表说明也标出待核验，且不把实现当 canonical', () => {
  const doc = fs.readFileSync(path.join(REPO_ROOT, 'data/spec/format/opcode-table.md'), 'utf8');
  assert.ok(doc.includes('待核验'), '表说明必须标出待核验');
  assert.ok(doc.includes('不是 `age-format` 的 canonical 副本'), '表说明必须声明不是实现副本');
});
