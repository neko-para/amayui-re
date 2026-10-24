/** @env pure @kind contract @why 存档帧记录的父帧号 / 表 1 / 表 2 下标写错（或把"下标"算成位置偏移）⇒ 读侧用错表项算出执行位置（或让"当前帧"带上可跳转的下标），恢复后从错误的指令继续，且写文件时不抛 */
/**
 * tools/test/emulator-new-save-tables.test.mjs —— `FMT-SAVE-CODEC` 的**帧记录**（写侧）：
 * `rec[0]` = 父帧号、`rec[259]` = 表 1 下标、`rec[260]` = 表 2 下标（当前帧强制 −1）。
 *
 * ## 判据为什么是这些
 * - 三张位置表是**表项数组**（每项 = 该类 opcode 的 dword 位置，严格升序），
 *   `rec[259]` / `rec[260]` 是**位置在表里的元素下标**（线性查找），⛔ 不是位置本身、⛔ 不是"位置 − 起点"：
 *   表里只收该类 opcode 的位置，相邻表项之间隔着别的指令 ⇒ 偏移量与下标必然不同。
 * - **当前帧**的 `rec[260]` 必须 −1：恢复时 `rec[260]` 优先且推进 3 字（≈"换脚本已执行完"），
 *   若给当前帧带上它，重启存档会把一条根本没执行的换脚本当成已执行。
 * - `rec[0]` = **父帧号**（不是状态码）：根帧 −1、子帧 = 建立它时的帧号 ⇒ 恢复时 `0x2` 的回退目标正确。
 * - 位置**不在表里** / 该帧**没有表** ⇒ −1（引擎的「没有这一项」哨兵），⛔ 不写 0（0 是合法下标）。
 *
 * ## 怎么读这份字节
 * `0x19e` 写出来的槽文件 = 292 B 头 + 20 B 参数块 + 未压缩信封（12 B 前缀 + 主体）。
 * 用参数块里的 seed / key 逆变换就拿到主体：`+0` 当前帧号，帧记录基址 = `1252 + 20000`（版本 1 = 3）。
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';

import { Machine, indexInTable } from '../../apps/emulator-new/src/core/machine.ts';
import { HANDLERS } from '../../apps/emulator-new/src/core/handlers.ts';
import { parseHeader } from '../../apps/emulator-new/src/core/decode.ts';
import { tablesFromHeader } from '../../apps/emulator-new/src/core/runner.ts';
import { readInt } from '../../apps/emulator-new/src/core/values.ts';
import { untransformWords } from '../../apps/emulator-new/src/core/save-codec.ts';

const KEY = 0x1234abcd;
const op = (type, raw) => ({ type, raw });

/** 直接调 handler 的用例只需空表（`Machine.run` 不用）。 */
function machine(opts = {}) {
  return new Machine({ body: Buffer.alloc(4), table: new Map(), key: KEY, ...opts });
}

/** 记下 `0x19e` 实际写出的槽文件字节（目标不存在 ⇒ 不走覆写确认）。 */
function slotHost() {
  const host = {
    written: null,
    fileForSlot: (slot, kind) => `slot${slot}.${kind}`,
    read: async () => null,
    write: async (_path, bytes) => { host.written = bytes; return true; },
    remove: async () => true,
    rename: async () => true,
    copy: async () => true,
  };
  return host;
}

/** 走 `0x19e` 写一次槽，返回**主体**的视图（`+0` = 当前帧号；帧记录从 `1252 + 20000` 起）。 */
async function saveBody(m, host) {
  const ops = [op(3, 1), op(0, 1)];
  await HANDLERS.get(0x19e)(m, ops);
  assert.equal(readInt(m, ops, 1), 0, '0x19e 写槽成功（第 1 个操作数 ← 0）');
  const file = host.written;
  assert.ok(file, '0x19e 必须真的调宿主的 write');
  const params = new DataView(file.buffer, file.byteOffset + 292, 20);
  const seed = params.getUint32(12, true);
  const key = params.getUint16(16, true);
  const cipher = file.slice(312);
  const plain = new Uint8Array(
    untransformWords(new Uint32Array(cipher.buffer, cipher.byteOffset, cipher.byteLength / 4), seed, key).buffer,
  );
  return new DataView(plain.buffer, 12); // 跳过信封的 12 字节前缀
}

/** 帧记录基址：版本 1 = 3 ⇒ 有 20000 字节专有副块。 */
const REC_BASE = 1252 + 20000;
const recAt = (index) => REC_BASE + 1044 * index;
const word = (body, off) => body.getUint32(off, true);

/**
 * 一张合成的脚本（`SYS4` + 版本串 + 6 个计数 + 子头；三组 `(条数, 偏移)`；表体写在 body 里）。
 * ★ `off` 是**表体在 body 里的 dword 下标**（body 前 4 个 dword 当"代码区"），⛔ 不是第一个表项的值。
 */
function syntheticScript(entriesPerTable) {
  const headerLen = 32 + 4 + entriesPerTable.length * 8;
  const offsets = [];
  let next = 4; // 留 4 个 dword 的代码区，表体从 body dword 4 起
  for (const entries of entriesPerTable) {
    offsets.push(next);
    next += entries.length;
  }
  const buf = Buffer.alloc(headerLen + next * 4);
  buf.write('SYS4', 0, 'latin1');
  buf.write('080 ', 4, 'latin1');
  buf.writeUInt32LE(4 + entriesPerTable.length * 8, 32);
  const body = buf.subarray(headerLen);
  entriesPerTable.forEach((entries, i) => {
    buf.writeUInt32LE(entries.length, 36 + 8 * i);
    buf.writeUInt32LE(offsets[i], 40 + 8 * i);
    entries.forEach((value, k) => body.writeUInt32LE(value, (offsets[i] + k) * 4));
  });
  return buf;
}

test('★ 表项数组 + 线性查找 ⇒ `rec[259]` / `rec[260]` 是**元素下标**（不是位置、也不是位置 − 起点）；当前帧 `rec[260]` 强制 −1', async () => {
  const host = slotHost();
  // 表里只收该类 opcode 的位置 ⇒ 表项之间有洞（7 / 10 / 15 不是连续位置）。
  const tables = { msg: [7, 10, 15], call: [7, 12, 20], sub: null };
  const m = machine({ rootTables: tables, scriptTables: (id) => (id === 5 ? tables : null), saveHost: host });
  m.frame.position = 7; // 根帧：两张表里都是第 0 项
  m.enterScript(Buffer.alloc(4), 5); // 当前帧 ← 子帧（父帧号 0）
  m.frame.position = 15; // 子帧：表 1 第 2 项 ⇒ 下标 2

  const body = await saveBody(m, host);
  assert.equal(word(body, 0), 1, '主体 +0 = 当前帧号 1（记录条数 = 当前帧号 + 1）');

  // 记录 0 = 根帧（**不是**当前帧）⇒ 位置 7 同时是两张表的表项 ⇒ 两个下标都能有。
  assert.equal(word(body, recAt(0) + 1036), 0, '★ rec[259] = 位置 7 在表 1 里的元素下标 0');
  assert.equal(word(body, recAt(0) + 1040), 0, '★ rec[260] = 位置 7 在表 2 里的元素下标 0（两者不互斥）');

  // 记录 1 = 当前帧（子帧）⇒ 表 1 有下标、表 2 强制 −1。
  assert.equal(word(body, recAt(1) + 1036), 2, '★ 子帧 rec[259] = 位置 15 在 [7, 10, 15] 里的下标 2（⛔ 不是 15、也不是 15−7=8）');
  assert.equal(word(body, recAt(1) + 1040), 0xffffffff, '★ 当前帧 rec[260] 强制 −1（位置 15 不在表 2 [7, 12, 20] 里，本就不会命中）');

  // 位置 10：下标 1，而"位置 − 起点"会是 3 ⇒ 两种算法可区分（下标不是偏移）。
  const host2 = slotHost();
  const m2 = machine({ rootTables: tables, scriptTables: () => tables, saveHost: host2 });
  m2.frame.position = 10;
  m2.loadFrameAt(1, Buffer.alloc(4), 5);
  m2.callFrameAt(1); // 根帧因此变成非当前帧
  const body2 = await saveBody(m2, host2);
  assert.equal(word(body2, recAt(0) + 1036), 1, '★ 位置 10 → 下标 1；"位置 − 起点"会给 3（⛔ 不许用那个）');
  assert.equal(word(body2, recAt(0) + 1040), 0xffffffff, '位置 10 不在表 2 [7, 12, 20] 里 ⇒ rec[260] = −1');
});

test('★ 位置落在表 2 但**当前帧**强制 −1；同一位置在非当前帧上写成下标', async () => {
  const host = slotHost();
  const tables = { msg: [7, 10, 15], call: [7, 12, 20], sub: null };
  const m = machine({ rootTables: tables, scriptTables: () => tables, saveHost: host });
  m.frame.position = 12; // 12 只在表 2 里（下标 1）；不在表 1 里 ⇒ rec[259] = −1
  m.loadFrameAt(1, Buffer.alloc(4), 7);
  m.callFrameAt(1); // 子帧成为当前帧
  m.frame.position = 12; // 同一个位置：在当前帧上必须写 −1

  const body = await saveBody(m, host);
  assert.equal(word(body, recAt(0) + 1036), 0xffffffff, '根帧：位置 12 不在表 1 里 ⇒ rec[259] = −1');
  assert.equal(word(body, recAt(0) + 1040), 1, '★ 根帧（非当前帧）：rec[260] = 位置 12 在表 2 里的下标 1');
  assert.equal(word(body, recAt(1) + 1036), 0xffffffff, '子帧：位置 12 不在表 1 里 ⇒ rec[259] = −1');
  assert.equal(word(body, recAt(1) + 1040), 0xffffffff, '★ 子帧是当前帧 ⇒ rec[260] 强制 −1（位置在表里也不写）');
});

test('★ `indexInTable`：线性查找元素（命中 = 下标、未命中 = −1；没有表 ⇒ −1）', () => {
  const entries = [7, 10, 15];
  assert.equal(indexInTable(7, entries), 0, '第一项 ⇒ 0');
  assert.equal(indexInTable(10, entries), 1, '中间项 ⇒ 1（⛔ 不是位置 − 首项 = 3）');
  assert.equal(indexInTable(15, entries), 2, '末项 ⇒ 2');
  assert.equal(indexInTable(8, entries), -1, '表项之间的洞 ⇒ −1');
  assert.equal(indexInTable(9, entries), -1, '洞 ⇒ −1');
  assert.equal(indexInTable(100, entries), -1, '末项之后 ⇒ −1');
  assert.equal(indexInTable(7, null), -1, '没有表 ⇒ −1');
  assert.equal(indexInTable(7, undefined), -1, '缺项 ⇒ −1');
  assert.equal(indexInTable(7, []), -1, '空表 ⇒ −1');
});

test('★ 位置不在表里 / 该帧没有表 ⇒ 两个下标都是 −1（⛔ 不写位置本身、也不写 0）', async () => {
  // ① 有表但位置是表项之间的洞。
  const host = slotHost();
  const tables = { msg: [7, 10, 15], call: [7, 12, 20], sub: null };
  const withTables = machine({ rootTables: tables, scriptTables: () => tables, saveHost: host });
  withTables.frame.position = 8; // 8 不是任何一张表的表项
  let body = await saveBody(withTables, host);
  assert.equal(word(body, recAt(0) + 1036), 0xffffffff, '位置 8 不在表 1 里 ⇒ rec[259] = −1');
  assert.equal(word(body, recAt(0) + 1040), 0xffffffff, '位置 8 不在表 2 里 ⇒ rec[260] = −1');

  // ② 根帧没有表（缺省 rootTables）/ 子帧的 id 查不到表 ⇒ 也是 −1。
  const host2 = slotHost();
  const noTables = machine({ scriptTables: () => null, saveHost: host2 });
  noTables.frame.position = 10;
  noTables.enterScript(Buffer.alloc(4), 9);
  noTables.frame.position = 10;
  body = await saveBody(noTables, host2);
  assert.equal(word(body, recAt(0) + 1036), 0xffffffff, '根帧没有表 ⇒ rec[259] = −1');
  assert.equal(word(body, recAt(0) + 1040), 0xffffffff, '根帧没有表 ⇒ rec[260] = −1');
  assert.equal(word(body, recAt(1) + 1036), 0xffffffff, '子帧没有表 ⇒ rec[259] = −1');
  assert.equal(word(body, recAt(1) + 1040), 0xffffffff, '子帧没有表 ⇒ rec[260]（当前帧）也是 −1');
});

test('★ `rec[0]` = 父帧号：根帧 −1、子帧 = 建立它时的帧号；空槽整条 −1', async () => {
  const host = slotHost();
  const m = machine({ saveHost: host });
  m.enterScript(Buffer.alloc(4), 5); // 槽 1：父帧号 0
  m.loadFrameAt(3, Buffer.alloc(4), 7); // 槽 3：父帧号 1（装载者 = 当前帧号 1）；槽 2 留空
  m.callFrameAt(3); // 当前帧号 ← 3

  const body = await saveBody(m, host);
  assert.equal(word(body, 0), 3, '记录条数 = 当前帧号 + 1');
  assert.equal(word(body, recAt(0)), 0xffffffff, '★ 根帧 rec[0] = −1（不是状态码 0）');
  assert.equal(word(body, recAt(1)), 0, '★ 子帧 rec[0] = 0（父帧号）');
  assert.equal(word(body, recAt(2)), 0xffffffff, '空槽：父帧号 −1');
  assert.equal(word(body, recAt(2) + 4), 0xffffffff, '空槽：脚本索引 −1');
  assert.equal(word(body, recAt(2) + 8), 0, '空槽：返回点条数 0');
  assert.equal(word(body, recAt(2) + 1036), 0xffffffff, '空槽：表 1 下标 −1');
  assert.equal(word(body, recAt(2) + 1040), 0xffffffff, '空槽：表 2 下标 −1');
  assert.equal(word(body, recAt(3)), 1, '★ 装载在 3 号槽的帧 rec[0] = 1（装载时的帧号）');
});

test('★ 头里三组 `(条数, 偏移)` + body → 三张表项数组（按声明顺序；不足 3 张 ⇒ 缺的 null）', () => {
  const three = syntheticScript([[7, 10, 15], [3, 9], [100]]);
  const header3 = parseHeader(three);
  assert.deepEqual(
    tablesFromHeader(header3, three.subarray(header3.headerLength)),
    { msg: [7, 10, 15], call: [3, 9], sub: [100] },
    '★ 顺序 = 头的声明顺序 1/2/3；off 是**表体在 body 的 dword 下标**（表项 = body[off*4 + 4k]）',
  );

  const two = syntheticScript([[7, 10], [3]]);
  const header2 = parseHeader(two);
  assert.deepEqual(
    tablesFromHeader(header2, two.subarray(header2.headerLength)),
    { msg: [7, 10], call: [3], sub: null },
    '只有两组 ⇒ 表 3 是 null（没有表 ≠ 空表）',
  );

  const none = syntheticScript([]);
  const header0 = parseHeader(none);
  assert.deepEqual(
    tablesFromHeader(header0, none.subarray(header0.headerLength)),
    { msg: null, call: null, sub: null },
    '一组都没有 ⇒ 三张表全 null',
  );
});
