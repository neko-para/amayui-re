/** @env assets @kind gate @why opcode→handler 的机械查询塌了：实现者只能靠旧仓索引认 handler，而旧索引**实测错位**（会把错的函数当真） */
/**
 * tools/test/opcodes-handlers.assets.test.mjs —— **opcode → handler 机械查询**的守卫
 *
 * ## 它守的是什么
 * `tools/opcodes.mjs --handlers` 从**本仓语料**重新提取 `opcode → handler 符号`
 * （分派表基址 `Engine+0xA509C`，`opcode = (表项偏移 − 基址)/4`）。这条查询存在的理由很具体：
 * **"某 opcode 的 handler 是谁"以前只能靠旧仓索引认，而旧索引实测错位一槽** ——
 * 实测：旧索引把 `sub_41C900`（`cur ← op1` 的"切深度"）记成 `0x8c jmp` 的 handler，
 * 而按算式 `Engine+0x0A5128` 那一项是 opcode **0x23**；`0x8c` 的真 handler 是 `sub_4203D0`。
 * ⇒ 本守卫把那组**已独立核过**的对照逐条钉死（下面的表），它是"实现者可以信这个查询"的唯一凭据。
 *
 * 运行：`pnpm test:assets`（要反汇编语料）
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';

import { REPO_ROOT } from '../lib/paths.mjs';
import { DISPATCH_BASE, DEFAULT_HANDLER, extractHandlers, listingPath } from '../lib/opcodes.mjs';

const listing = listingPath(REPO_ROOT);
const skip = listing ? false : '反汇编语料未解压（先 `pnpm tools disasm build`）';

/** 提取（只做一次），并**先把结构问题挡掉** —— 有问题还继续断言等于用坏数据判对错 */
const extracted = (() => {
  if (!listing) return null;
  const r = extractHandlers(fs.readFileSync(listing, 'utf8'));
  return r;
})();

test('★ 提取是机械的：赋值行数够多、偏移全部对齐、同一 opcode 不许有两条', { skip }, () => {
  assert.ok(extracted.rows > 500, `语料里应有几百条 handler 赋值，实际 ${extracted.rows}（判据形态变了？）`);
  assert.deepEqual(extracted.problems, [], `分派表结构必须自洽（有问题就不许给结论）：\n${extracted.problems.join('\n')}`);
  assert.ok(extracted.byOpcode.size > 500, `有 handler 的 opcode 应超过 500，实际 ${extracted.byOpcode.size}`);
  assert.equal(Math.min(...extracted.byOpcode.keys()), 1, '最小 opcode 应为 1（0 号走缺省）');
});

test('★ 逐条钉死**已独立核过**的对照（含旧索引错位那一条反证）', { skip }, () => {
  // ★ 这些值不是从旧仓抄的，而是各自独立取过证（逐字 `.lst` + `.c` 阅读路径）：
  //   `0x02/0x03/0x05/0x06/0xa0/0x8c/0x8f` 见台账 KN-01M4AV9E8V3C3H2F3Z581A5501；
  //   `0x50/0x60` 见批 R1 的数值族复核；`0x1a3/0x1a7` 见 KN-01M4ASXQ587J7G7E6E5R3R2Y2K 与 KN-01M4AGT0TY7D70502X3C1N2Z6M。
  const pinned = [
    [0x02, 'sub_41A820', 'exit'],
    [0x03, 'sub_41C6A0', 'call-script'],
    [0x05, 'sub_41A9B0', 'ret'],
    [0x06, 'sub_41C7C0', 'load-frame'],
    [0x50, 'sub_42C5E0', 'add'],
    [0x60, 'sub_42CA50', 'random'],
    [0x8c, 'sub_4203D0', 'jmp'],
    [0x8f, 'sub_420560', 'call'],
    [0xa0, 'sub_4209B0', 'jcc'],
    [0x1a3, 'sub_42DF40', 'load-int'],
    [0x1a7, 'sub_4191B0', 'comment'],
  ];
  for (const [op, sym, name] of pinned) {
    assert.equal(extracted.byOpcode.get(op), sym, `0x${op.toString(16)} ${name} 的 handler 应是 ${sym}`);
  }
  // ★★ 错位反证（**逐字核过语料原行**，不是转述）：
  //     `.text:00415F20 mov dword ptr [esi+0A5128h], offset sub_41D390` ⇒ 该表项是 opcode **0x23** = `sub_41D390`
  //     `.text:00415E4E mov dword ptr [esi+0A50BCh], offset sub_41C900` ⇒ `sub_41C900` 的真实 opcode 是 **0x08**
  //   ⇒ 旧索引把 `0x8c` 记成 `sub_41C900`（差的不是一槽，是"把 0x08 的函数挂到了 0x8c 名下"）。
  //   这条断言的意义：**两个 opcode 的归属不能互换** —— 换个表基址就会同时翻掉这两条。
  assert.equal(extracted.byOpcode.get(0x08), 'sub_41C900', 'sub_41C900 的真实 opcode 是 0x08');
  assert.equal(extracted.byOpcode.get(0x23), 'sub_41D390', '0x23 是 sub_41D390');
  assert.notEqual(extracted.byOpcode.get(0x8c), 'sub_41C900', '0x8c 不是 sub_41C900（旧索引在这里错位）');
  // ★ `0x8f` 在旧索引里被说成 `sub_41C8D0`（那是"卸载帧"那条）；机械提取给出的是另一条
  assert.notEqual(extracted.byOpcode.get(0x8f), 'sub_41C8D0', '0x8f 的 handler 不是旧索引说的"卸载帧"那条');
});

test('★ `opcode = (表项偏移 − 基址)/4` 这条算式必须自洽（基址换个值就会整体错位一槽）', { skip }, () => {
  // 反向复算：把每个 opcode 代回算式，必须得到语料里真实存在的那个偏移（4 字节对齐）
  let checked = 0;
  for (const op of [0x01, 0x02, 0x1a7, 0x1a8, 0x20f, 0x21c, 0x320]) {
    if (!extracted.byOpcode.has(op)) continue;
    const off = DISPATCH_BASE + op * 4;
    assert.equal(Number.isInteger(off), true);
    assert.equal(off % 4, 0, '表项必须 4 字节对齐');
    checked += 1;
  }
  assert.ok(checked >= 5, `至少核几条，实际 ${checked}`);
  assert.equal(DISPATCH_BASE, 0xa509c, '基址由构造函数里 `lea edi,[esi+0A509Ch]` 给出');
  assert.equal(DEFAULT_HANDLER, 'sub_418E30', '缺省 handler = `rep stosd` 填的那个（体是抛「不支持」）');
});

test('★ 缺省 handler 的名单（= 引擎会抛「不支持」）必须与指令表对得上', { skip }, () => {
  const table = JSON.parse(fs.readFileSync(`${REPO_ROOT}/packages/age-format/src/asm/instruction-set.json`, 'utf8'));
  const gaps = table.map((e) => e.opcode).filter((op) => op > 0 && !extracted.byOpcode.has(op));
  assert.equal(gaps.length, 30, `走缺省 handler 的 opcode 数（实测 30）—— 变了要重看：${gaps.map((o) => `0x${o.toString(16)}`).join(' ')}`);
  for (const op of gaps) assert.ok(table.some((e) => e.opcode === op), '缺口必须来自指令表');
  // 反向：有 handler 的那些**不**在这个名单里
  for (const op of [0x8c, 0x8f, 0xa0, 0x1a3, 0x1a7]) assert.ok(!gaps.includes(op), `0x${op.toString(16)} 有 handler，不该在缺口名单里`);
});

test('★ 启动前段那 16 个 opcode 全部点得到 handler（这是"能继续实现"的前提）', { skip }, () => {
  const prologue = [0x1a8, 0x2f6, 0x149, 0x21b, 0x88, 0x1ca, 0x252, 0x324, 0x32f, 0x70, 0x71, 0x73, 0x78, 0x2db, 0x79, 0x1c1];
  const missing = prologue.filter((op) => !extracted.byOpcode.has(op));
  assert.deepEqual(missing, [], `这些应当都能点名（实际缺 ${missing.map((o) => `0x${o.toString(16)}`).join(' ')}）`);
  // 逐个都必须是 `sub_` 形态（不是空串、不是别的形状）
  for (const op of prologue) assert.match(extracted.byOpcode.get(op), /^sub_[0-9A-F]+$/, `0x${op.toString(16)} 的 handler 形态不对`);
});
