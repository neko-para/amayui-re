/**
 * tools/test/opcodes.test.mjs — 指令表**派生**的守卫
 *
 * 测三件"会坏且坏得有意义"的事：
 *   ① 派生器**真的会丢知识层字段**（拿一份带 handler/status 的合成旧表，验派生结果里没有它们）；
 *   ② 派生表**不合规时会红**（opcode 重复 / argc 越界 / 多出字段）；
 *   ③ 落盘形态**同输入同字节**（一个 opcode 一行、键序固定）。
 *
 * ❌ 不测"当前表里有什么值"（那是状态，交给 `packages/age-format/test/asm.test.mjs` 与 `pnpm tools opcodes report`）。
 * 运行：`pnpm test`
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';

import {
  FORMAT_FIELDS,
  LABEL_FIELDS,
  OBSERVED_FIELDS,
  canonicalTableString,
  deriveFormatTable,
  deriveTo,
  report,
  validateTable,
} from '../lib/opcodes.mjs';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';

import { DEFAULT_MANIFEST } from '../lib/manifest.mjs';

/** 一份"像旧仓那样"的合成表：格式层四列 + 两个知识层字段 */
const rawTable = () => [
  { opcode: 5, argc: 0, name: 'ret', handler: 'sub_400000', status: '已核对', aliases: [] },
  { opcode: 1, argc: 0, name: 'abort', handler: 'sub_418E60', status: '已核对（2026-09）', aliases: ['kill'] },
  { opcode: 470, argc: 2, name: '', handler: 'sub_42E7C0', status: '仅映射', aliases: [] },
];

test('① 派生器丢掉的是**自述标签**（handler 是观测，不算"丢"）', () => {
  const table = deriveFormatTable(rawTable());
  assert.deepEqual(Object.keys(table[0]), FORMAT_FIELDS, '派生结果只应有格式层四列');
  for (const e of table) {
    for (const k of LABEL_FIELDS) assert.ok(!(k in e), `不该出现自述标签 ${k}`);
  }
  assert.deepEqual(table.map((e) => e.opcode), [1, 5, 470], '必须按 opcode 升序');
  assert.deepEqual(table.find((e) => e.opcode === 1).aliases, ['kill'], 'aliases 要保留');
  assert.equal(validateTable(table).failures, 0);
});

test('① 对账报告**区分观测与自述标签**（handler 与 argc 同类 ⇒ 算"继承"，不算"丢弃"）', () => {
  const r = report(rawTable());
  assert.equal(r.entries, 3);
  // handler 是观测 ⇒ 落在 inherited 里，不是 dropped
  assert.ok(r.inheritedFields.includes('handler'), 'handler 必须被算作继承的观测');
  assert.ok(r.inheritedFields.includes('argc'));
  assert.deepEqual(r.droppedFields, ['status'], '唯一被丢的是人工自述标签');
  assert.equal(r.handlerCount, 3);
  assert.equal(r.distinctHandlers, 3);
  assert.ok(r.statusValues.includes('已核对'));
  assert.ok(r.statusValues.includes('仅映射'));
  assert.equal(r.entriesWithAliases, 1);
  // 分类常量本身要自洽：观测与标签不许重叠
  assert.deepEqual(OBSERVED_FIELDS.filter((f) => LABEL_FIELDS.includes(f)), []);
});

test('② 校验器红得有意义：opcode 重复 / argc 越界 / 多出字段', () => {
  const dup = deriveFormatTable([{ opcode: 1, argc: 0, name: 'a', aliases: [] }, { opcode: 1, argc: 1, name: 'b', aliases: [] }]);
  assert.ok(validateTable(dup).failures > 0, 'opcode 重复必须红');

  const badArgc = [{ opcode: 1, argc: 999, name: 'a', aliases: [] }];
  assert.ok(validateTable(badArgc).failures > 0, 'argc 越界必须红');

  const extra = [{ opcode: 1, argc: 0, name: 'a', aliases: [], handler: 'x' }];
  const rep = validateTable(extra);
  assert.ok(rep.failures > 0, '多出知识层字段必须红');
  assert.ok(rep.checks.find((c) => c.id === 1).problems.length > 0, '而且要点在 #1（非格式层字段）上');
});

test('② 派生器遇到缺 opcode/argc 的旧条目必须抛（不产出半张表）', () => {
  assert.throws(() => deriveFormatTable([{ name: 'x' }]), /缺 opcode\/argc/);
  assert.throws(() => deriveFormatTable(null), /不是数组/);
});

test('③ 落盘形态确定：同输入同字节，且一个 opcode 一行', () => {
  const a = canonicalTableString(deriveFormatTable(rawTable()));
  const b = canonicalTableString(deriveFormatTable([...rawTable()].reverse()));
  assert.equal(a, b, '输入顺序不同也必须得到同一份字节');
  // ★ `'[\n' + 3 行 + '\n]\n'` 按 `\n` split 出来是 6 段（末尾那个是空串）
  assert.equal(a.split('\n').length, 6, '三个 opcode + 首尾两行 + 末尾空段');
  assert.ok(a.endsWith(']\n'));
  for (const key of FORMAT_FIELDS) assert.ok(a.includes(`"${key}"`), `键序里必须有 ${key}`);
});

test('③ 写盘：写后回读复验，非法输入不留残file', () => {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'amayui-opcodes-'));
  const target = path.join(dir, 'opcodes.json');
  const ok = deriveTo(rawTable(), target);
  assert.equal(ok.ok, true, ok.reason);
  assert.ok(fs.existsSync(target));
  assert.equal(JSON.parse(fs.readFileSync(target, 'utf8')).length, 3);

  // 非法输入 ⇒ 拒绝，且不留下半份文件
  const badTarget = path.join(dir, 'bad.json');
  const res = deriveTo([{ opcode: 1, argc: 1, name: 'a', aliases: [] }, { opcode: 1, argc: 1, name: 'b', aliases: [] }], badTarget);
  assert.equal(res.ok, false);
  assert.equal(fs.existsSync(badTarget), false, '被拒时不得留下文件');
});

test('④ 派生链**不硬编码旧仓路径**：来源必须登记在清单条目里', () => {
  const manifest = JSON.parse(fs.readFileSync(DEFAULT_MANIFEST, 'utf8'));
  const entry = manifest.entries.find((e) => e.id === 'knowledge/opcode-table-source');
  assert.ok(entry, '清单里必须有 knowledge/opcode-table-source（没了派生链就断）');
  assert.equal(entry.storage, 'external-only', '来源不入库：只登记路径 + sha256');
  assert.equal(entry.dest, null);
  assert.ok(Array.isArray(entry.blocks) && entry.blocks.includes('K3'), '它是知识线要处理的东西（K3 前不得入库）');
  const origin = (entry.origin ?? [])[0];
  assert.ok(origin, '来源条目必须登记 origin');
  assert.ok(manifest.roots?.[origin.root], `origin.root "${origin.root}" 必须能在 roots 里解析`);
  assert.match(origin.sha256 ?? '', /^[0-9a-f]{64}$/, '不入库的文件件必须登记 sha256');

  // ★ 「要么都继承」：handler 是观测，必须有它自己的登记条目（不能只活在派生器的"丢弃"注释里）
  const handlers = manifest.entries.find((e) => e.id === 'knowledge/opcode-handlers');
  assert.ok(handlers, 'handler 是观测 ⇒ 必须登记为 knowledge/opcode-handlers（继承，不丢）');
  assert.equal(handlers.storage, 'external-only');
  assert.equal((handlers.origin ?? [])[0]?.sha256, origin.sha256, '与来源表同一份文件');
  // 而 status 是自述标签：登记条目只用于留痕（说明"为什么单单它被丢"）
  const labels = manifest.entries.find((e) => e.id === 'knowledge/opcode-status-labels');
  assert.ok(labels, 'status 被丢弃这件事也要留痕（knowledge/opcode-status-labels）');
  assert.equal((labels.origin ?? [])[0]?.sha256, origin.sha256);

  // 派生器真的按条目解析（而不是写了路径常量）
  const src = fs.readFileSync(new URL('../opcodes.mjs', import.meta.url), 'utf8');
  assert.ok(src.includes("'knowledge/opcode-table-source'"), '派生器必须按条目 id 解析来源');
  assert.ok(!/DEFAULT_SOURCE\s*=\s*'scripts\//.test(src), '派生器不得硬编码旧仓相对路径');
});

test('④ 来源在场时，它的 sha256 必须与登记一致（来源被换过 ⇒ 红）', async () => {
  const manifest = JSON.parse(fs.readFileSync(DEFAULT_MANIFEST, 'utf8'));
  const entry = manifest.entries.find((e) => e.id === 'knowledge/opcode-table-source');
  const origin = (entry.origin ?? [])[0];
  const abs = path.join(manifest.roots[origin.root], origin.path);
  if (!fs.existsSync(abs)) {
    // 旧仓是每台机器不同的绝对路径：不在场就跳过（与 age-format 的样本同一口径）
    return;
  }
  const { createHash } = await import('node:crypto');
  const got = createHash('sha256').update(fs.readFileSync(abs)).digest('hex');
  assert.equal(got, origin.sha256, '来源文件的 sha256 与登记不符 ⇒ 派生链的证据变了');
});
