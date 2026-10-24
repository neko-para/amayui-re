/** @env pure @kind contract @why v1=3 的 20000 B 纹理记录表写错（五格错位 / 少写 / 门不落地 / 越界到别的块）⇒ 恢复时纹理全不重建或从错的文件号重建，且写文件时不抛 */
/**
 * tools/test/emulator-new-save-textures.test.mjs —— `FMT-SAVE-CODEC` 的**纹理记录表**（写侧）。
 *
 * ## 它守的是什么
 * 1. **布局**：`版本字段 1 == 3` ⇒ 主体 `+1252` 起 20000 B = 1000 项 × 20 B（5 个 u32 小端：
 *    来源文件号 / 颜色键 / 恢复重建门 / 零读者标志 / 空白标记）；`!= 3` ⇒ 这 20000 B 不存在，
 *    帧记录基点仍是 1252。**别的块偏移不因记录内容而移动**（内容只改这 20000 B 内的字节）。
 * 2. **五项逐个落盘**（顺错一格 = 门变成颜色键 / 颜色键变成门 ⇒ 恢复行为整条错）。
 * 3. **缺省 / 不足 1000 项 ⇒ 缺的项留 0**：门 0 = 一条都不重建，但长度与位置不变（合法文件）。
 * 4. **`0x19e` 的写路径真的把场景状态带进去**：`file` / `color` / `blank` 取纹理记录，
 *    门 / 零读者标志取 `0x258` / `0x259` 写的两张 1000 项表（`flagTableA` / `flagTableB`）；
 *    没建过记录的编号 ⇒ 三格 0（⛔ 不是 −1：−1 是"有记录但无来源"）。
 *    ★ 这条是**唯一**能证明"`buildSlotFile` 把表传进去了"的判据 —— 只测 `buildSaveBody` 盖不住。
 *
 * ## 怎么看这份字节
 * 主体 = 解压后负载跳过信封 12 B 前缀；`+0` 当前帧号，`+1252` 起纹理记录表，帧记录在 `1252 + 20000`。
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';

import { buildSaveBody, untransformWords } from '../../apps/emulator-new/src/core/save-codec.ts';
import { Machine } from '../../apps/emulator-new/src/core/machine.ts';
import { HANDLERS } from '../../apps/emulator-new/src/core/handlers.ts';
import { readInt } from '../../apps/emulator-new/src/core/values.ts';

const KEY = 0x1234abcd;
const TEX_BASE = 1252;
const TEX_ITEMS = 1000;
const TEX_BYTES = TEX_ITEMS * 20;
const op = (type, raw) => ({ type, raw });

/** 只有一条根帧、池全空的快照（纹理表是唯一变量）。 */
const snapshot = (over = {}) => ({
  frame: 0,
  records: [{ parent: -1, scriptIndex: -1, ret: [], tableA: -1, tableB: -1 }],
  ints: [],
  floats: [],
  strings: [],
  version1: 3,
  version2: 20,
  ...over,
});

/** 一项 = 20 B = 5 个 u32（顺序：file / color / flag0 / flag1 / blank）。 */
const texAt = (body, id) =>
  Array.from({ length: 5 }, (_, k) => new DataView(body.buffer, body.byteOffset).getUint32(TEX_BASE + 20 * id + 4 * k, true));

const allZero = (bytes) => bytes.every((b) => b === 0);

test('★ v1=3：+1252 起 1000×20 B，五格逐项按序落盘（file / color / 门 / 零读者标志 / blank）', () => {
  const records = [
    { file: 100, color: 0xff102030, flag0: 1, flag1: 0, blank: 0 },
    { file: -1, color: 0, flag0: 0, flag1: 1, blank: 1 },
  ];
  const snap = snapshot({ textureRecords: records });
  const body = buildSaveBody(snap, KEY);

  assert.deepEqual(texAt(body, 0), [100, 0xff102030, 1, 0, 0], '★ 第 0 项五格按序：来源文件号 / 颜色键 / 门 / 零读者标志 / 空白标记');
  assert.deepEqual(texAt(body, 1), [0xffffffff, 0, 0, 1, 1], '★ 第 1 项：file = −1 写成 0xFFFFFFFF（有记录但无来源）；门 0 / 标志 1 不许串格');

  // 其余 998 项全 0：逐项（不是抽一格）—— 少写一项就是"恢复时那一条纹理被清成门 0"。
  const rest = body.slice(TEX_BASE + 40, TEX_BASE + TEX_BYTES);
  assert.equal(rest.length, 998 * 20);
  assert.ok(allZero(rest), '★ 未给出的 998 项必须逐字节全 0（缺项 = 门 0 = 不重建）');

  // 20000 B 之后紧接着帧记录 ⇒ 记录条数与父帧号就在表尾之后（偏移没被记录内容挪动）。
  const frameBase = TEX_BASE + TEX_BYTES;
  const body2 = new DataView(body.buffer);
  assert.equal(body2.getUint32(frameBase, true), 0xffffffff, '★ 帧记录基址 = 1252 + 20000（表尾之后就是 rec[0] 的父帧号 −1）');
  assert.equal(body2.getUint32(0, true), 0, '主体 +0 仍是当前帧号');
});

test('★ v1=3：记录内容只改这 20000 B —— 有记录 / 无记录的字节长与其它块完全相同', () => {
  const withRecords = buildSaveBody(snapshot({ textureRecords: [{ file: 7, color: 9, flag0: 1, flag1: 1, blank: 0 }] }), KEY);
  const without = buildSaveBody(snapshot(), KEY);
  assert.deepEqual([...withRecords.slice(0, TEX_BASE)], [...without.slice(0, TEX_BASE)], '表之前的字节不变');
  assert.deepEqual(
    [...withRecords.slice(TEX_BASE + TEX_BYTES)],
    [...without.slice(TEX_BASE + TEX_BYTES)],
    '★ 表之后的字节（帧记录 / 计数 / 块 / 尾段）位置与内容都不变',
  );
  assert.equal(withRecords.length, without.length, '长度只由版本与池决定，与记录内容无关');

  // 缺省 = 全 0（合法文件：门全 0 ⇒ 一条都不重建），长度/位置与"给了记录"完全一致。
  assert.ok(allZero(without.slice(TEX_BASE, TEX_BASE + TEX_BYTES)), '★ 缺省 textureRecords ⇒ 20000 B 全 0');
  assert.equal(without.length, withRecords.length);
});

test('★ v1=2：没有 20000 B 纹理记录表（recBase 仍是 1252），给了记录也不写一个字节', () => {
  const records = [{ file: 100, color: 0xff102030, flag0: 1, flag1: 1, blank: 0 }];
  const v3 = buildSaveBody(snapshot({ textureRecords: records }), KEY);
  const v2 = buildSaveBody(snapshot({ version1: 2, textureRecords: records }), KEY);
  assert.equal(v3.length - v2.length, TEX_BYTES, '★ 两版长度差恰是 20000 B（其它块不因此变形）');

  const dv = new DataView(v2.buffer);
  assert.equal(dv.getUint32(TEX_BASE, true), 0xffffffff, '★ v1=2 的 +1252 就是帧记录 rec[0] 的父帧号 −1（不是纹理表）');
  assert.deepEqual([...v2.slice(TEX_BASE, TEX_BASE + 20)], [...v3.slice(TEX_BASE + TEX_BYTES, TEX_BASE + TEX_BYTES + 20)], '★ v1=2 的 +1252 起的字节 = v1=3 去掉 20000 B 后的同一段（偏移整体前移）');
});

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

/** 走 `0x19e` 写一次槽，返回**主体**的视图（`+0` = 当前帧号）。 */
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
  return new Uint8Array(plain.buffer, 12); // 跳过信封的 12 字节前缀
}

test('★ `0x19e` 写路径：表带场景的纹理记录 + 两张标志表（file/color/blank 取记录，门/零读者标志取 flagTableA/B）', async () => {
  const host = slotHost();
  const m = new Machine({ body: Buffer.alloc(4), table: new Map(), key: KEY, saveHost: host });
  m.scene.textures.set(1, { file: 100, color: 0xff102030, blank: 0 });
  m.scene.textures.set(2, { file: -1, color: 0x80402010, blank: 1 });
  m.scene.flagTableA[1] = 1; // 门 = 1
  m.scene.flagTableB[1] = 0;
  m.scene.flagTableA[2] = 0; // 门 = 0（有记录、但恢复时不重建）
  m.scene.flagTableB[2] = 1; // 零读者标志 = 1
  m.scene.flagTableA[500] = 1; // 没有纹理记录的编号：门 1，其它格 0

  const body = await saveBody(m, host);
  assert.deepEqual(texAt(body, 1), [100, 0xff102030, 1, 0, 0], '★ 第 1 项 = 记录 + 表 A 门 1 + 表 B 0');
  assert.deepEqual(texAt(body, 2), [0xffffffff, 0x80402010, 0, 1, 1], '★ 第 2 项：门取表 A（0）、零读者标志取表 B（1），两者不许互换');
  assert.deepEqual(texAt(body, 500), [0, 0, 1, 0, 0], '★ 没建过记录的编号 ⇒ 三格 0（⛔ 不写 −1），门仍来自表 A');
  assert.deepEqual(texAt(body, 0), [0, 0, 0, 0, 0], '没碰过的编号全 0');
  assert.deepEqual(texAt(body, 999), [0, 0, 0, 0, 0], '表的最后一项存在且为 0（1000 项整段写出）');
  assert.deepEqual(texAt(body, 3), [0, 0, 0, 0, 0]);
});
