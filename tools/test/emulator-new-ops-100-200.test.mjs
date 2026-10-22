/** @env pure @kind contract @why 浮点绝对值 / 四则运算 / 位 27 读回写错方向或漏解码，都不会抛，只会让后续分支静默走错 */
/**
 * tools/test/emulator-new-ops-100-200.test.mjs —— 0x191 / 0x2d0 / 0x2d1 / 0x2d3 / 0x1c7 的合同守卫。
 *
 * ★ 判据为什么是这些：
 *   - 0x2d1 / 0x2d3 的**方向**（op2 ∘ op3）写反不会报错，只会静默算错；
 *   - 0x191 必须取绝对值，不能只搬运；
 *   - 0x1c7 只读**第 27 位**并归一成 0/1，不能把整字或别的位写出去。
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';

import { Machine } from '../../apps/emulator-new/src/core/machine.ts';
import { HANDLERS } from '../../apps/emulator-new/src/core/handlers.ts';
import { readInt } from '../../apps/emulator-new/src/core/values.ts';

const KEY = 0x1234abcd;
const op = (type, raw) => ({ type, raw });
const f32bits = (v) => new Uint32Array(new Float32Array([v]).buffer)[0] >>> 0;
const f32 = (bits) => new Float32Array(new Uint32Array([bits >>> 0]).buffer)[0];

function machine() {
  return new Machine({ body: Buffer.alloc(4), table: new Map(), key: KEY });
}

test('★ `0x191`：op1 ← |读浮点(op2)|（不是原样搬运）', () => {
  const m = machine();
  m.globalFloat.set(1, f32bits(-3.5));
  HANDLERS.get(0x191)(m, [op(4, 2), op(4, 1)]);
  assert.equal(f32(m.globalFloat.get(2)), 3.5);
});

test('★ `0x2d0` / `0x2d1` / `0x2d3`：op1 ← op2 ∘ op3，方向不可交换', () => {
  const m = machine();
  const call = (opcode, a, b) => {
    HANDLERS.get(opcode)(m, [op(4, 0), op(1, f32bits(a)), op(1, f32bits(b))]);
    return f32(m.globalFloat.get(0));
  };
  assert.equal(call(0x2d0, 2.5, 4), 6.5);
  assert.equal(call(0x2d1, 7, 3), 4);
  assert.equal(call(0x2d1, 3, 7), -4, '★ 反向会得到 +4，必须抓住');
  assert.equal(call(0x2d3, 8, 2), 4);
  assert.equal(call(0x2d3, 2, 8), 0.25, '★ 反向会得到 4，必须抓住');
});

test('★ `0x1b0` memcpy：源 = 第 1 个、目标 = 第 2 个，按 4×n 字节原样复制', () => {
  const m = machine();
  m.frame.localInt.set(0, 0x11223344);
  m.frame.localInt.set(1, 0xaabbccdd);
  HANDLERS.get(0x1b0)(m, [op(9, 0), op(4, 0), op(0, 2)]);
  assert.equal(m.globalFloat.get(0), 0x11223344);
  assert.equal(m.globalFloat.get(1), 0xaabbccdd);

  const r = machine();
  r.frame.localInt.set(0, 0x11223344);
  HANDLERS.get(0x1b0)(r, [op(9, 0), op(4, 0), op(0, 0)]);
  assert.equal(r.globalFloat.size, 0, 'n = 0 ⇒ 一格都不写');
});

test('★ `0x208` 纹理尺寸：有对象 ⇒ 逻辑宽高；槽空 ⇒ 0/0；>999 ⇒ 错误通道 + 0/0', () => {
  const m = machine();
  m.scene.textureObjects.set(3, { kind: 'normal', width: 320, height: 180 });
  HANDLERS.get(0x208)(m, [op(0, 3), op(3, 1), op(3, 2)]);
  assert.equal(readInt(m, [op(3, 1)], 1), 320);
  assert.equal(readInt(m, [op(3, 2)], 1), 180);

  HANDLERS.get(0x208)(m, [op(0, 5), op(3, 1), op(3, 2)]);
  assert.equal(readInt(m, [op(3, 1)], 1), 0);
  assert.equal(readInt(m, [op(3, 2)], 1), 0);

  const before = m.diagnostics.length;
  HANDLERS.get(0x208)(m, [op(0, 1000), op(3, 1), op(3, 2)]);
  assert.equal(readInt(m, [op(3, 1)], 1), 0);
  assert.equal(m.diagnostics.length, before + 1, '越界必须进错误通道');
});

test('★ `0x1cf` / `0x1bf`：第 16 位见位整字清 0，随后第 0 位为 0 ⇒ 相邻格写 1', () => {
  const m = machine();
  HANDLERS.get(0x1cf)(m, [op(0, 0x10001)]); // 第 16 位 + 第 0 位
  HANDLERS.get(0x1bf)(m);
  assert.equal(m.statusWord, 0, '第 16 位 ⇒ 整字清 0（不是只清该位）');
  assert.equal(m.statusNeighbor, 1, '清完之后第 0 位为 0 ⇒ 相邻格写 1');

  const n = machine();
  HANDLERS.get(0x1cf)(n, [op(0, 1)]); // 第 0 位为 1、第 16 位为 0
  HANDLERS.get(0x1bf)(n);
  assert.equal(n.statusWord, 1);
  assert.equal(n.statusNeighbor, 0, '第 0 位为 1 ⇒ 不动相邻格');
});

test('★ `0x1cc` / `0x19a` / `0x1b6` / `0x1b7`：状态格原值、格 X 原值、格 Y 归一', () => {
  const m = machine();
  m.statusCell = -7;
  HANDLERS.get(0x1cc)(m, [op(3, 1)]);
  assert.equal(readInt(m, [op(3, 1)], 1), -7, '状态格原样搬（不归一）');

  m.flagX = 123;
  HANDLERS.get(0x19a)(m, [op(3, 2)]);
  assert.equal(readInt(m, [op(3, 2)], 1), 123, '格 X 原样搬');

  HANDLERS.get(0x1b7)(m, [op(0, -1)]);
  assert.equal(m.flagY, 1, '0x1b7 先归一成 0/1 再存');
  HANDLERS.get(0x1b6)(m, [op(3, 3)]);
  assert.equal(readInt(m, [op(3, 3)], 1), 1);
  HANDLERS.get(0x1b7)(m, [op(0, 0)]);
  HANDLERS.get(0x1b6)(m, [op(3, 3)]);
  assert.equal(readInt(m, [op(3, 3)], 1), 0);
});

test('★ `0x258` / `0x259`：掩码只取低两位，两张表同值；`0x259` 全清 1000 项', () => {
  const m = machine();
  HANDLERS.get(0x258)(m, [op(0, 5), op(0, 3)]);
  assert.equal(m.scene.flagTableA[5], 1);
  assert.equal(m.scene.flagTableB[5], 1);
  HANDLERS.get(0x258)(m, [op(0, 6), op(0, 2)]);
  assert.equal(m.scene.flagTableA[6], 0);
  assert.equal(m.scene.flagTableB[6], 1);
  assert.throws(() => HANDLERS.get(0x258)(m, [op(0, 1000), op(0, 1)]), /越界/);
  HANDLERS.get(0x259)(m);
  assert.equal(m.scene.flagTableA.every((x) => x === 0), true);
  assert.equal(m.scene.flagTableB.every((x) => x === 0), true);
});

test('★ `0x2da`：编号 > 999 ⇒ 错误通道且一个字段都不写；否则七个整数格', () => {
  const m = machine();
  HANDLERS.get(0x2da)(m, [op(0, 3), ...Array.from({ length: 7 }, (_, i) => op(0, i + 10))]);
  assert.deepEqual(m.scene.cgRecords.get(3), [10, 11, 12, 13, 14, 15, 16]);
  const before = m.diagnostics.length;
  HANDLERS.get(0x2da)(m, [op(0, 1000), ...Array.from({ length: 7 }, (_, i) => op(0, i))]);
  assert.equal(m.scene.cgRecords.has(1000), false);
  assert.equal(m.diagnostics.length, before + 1);
});

test('★ `0x1f6`：清记录 / 网格 / 辅助表与脏标记，但不动“有动画”', () => {
  const m = machine();
  m.scene.drawItems.set(1, {});
  m.scene.meshes.set(2, {});
  m.scene.auxTables[0].set(3, {});
  m.scene.dirty = 1;
  m.scene.animating = 1;
  HANDLERS.get(0x1f6)(m);
  assert.equal(m.scene.drawItems.size, 0);
  assert.equal(m.scene.meshes.size, 0);
  assert.equal(m.scene.auxTables[0].size, 0);
  assert.equal(m.scene.dirty, 0);
  assert.equal(m.scene.animating, 1, '“有动画”不在清空范围');
});

test('★ `0x19b` / `0x19c`：位 27 的清 / 条件置，C 支置 D、E 支不碰 D、F 支不动', () => {
  const clear = machine();
  clear.flags = (1 << 27) | (1 << 28);
  clear.modeA = clear.modeB = clear.modeD = 1;
  HANDLERS.get(0x19b)(clear);
  assert.equal((clear.flags >>> 27) & 1, 0);
  assert.equal((clear.flags >>> 28) & 1, 1, '其它位必须保留');
  assert.deepEqual([clear.modeD, clear.modeA, clear.modeB], [0, 0, 0]);

  const c = machine();
  c.modeC = 1;
  c.flags = 1 << 29;
  HANDLERS.get(0x19c)(c);
  assert.equal(c.modeA, 1);
  assert.equal(c.modeB, 0);
  assert.equal(c.modeD, 1);
  assert.equal(c.modeG, 1);
  assert.equal((c.flags >>> 27) & 1, 1);
  assert.equal((c.flags >>> 29) & 1, 0, '置位支要清第 29 位');

  const e = machine();
  e.modeE = 1;
  e.modeD = 5;
  HANDLERS.get(0x19c)(e);
  assert.equal(e.modeD, 5, 'E 支不碰 D');
  assert.equal((e.flags >>> 27) & 1, 1);

  const f = machine();
  f.modeF = 1;
  f.modeD = 5;
  HANDLERS.get(0x19c)(f);
  assert.equal(f.modeD, 5, 'F 支什么都不改');
  assert.equal((f.flags >>> 27) & 1, 0);

  const none = machine();
  none.modeD = 5;
  HANDLERS.get(0x19c)(none);
  assert.equal(none.modeD, 0);
  assert.equal((none.flags >>> 27) & 1, 0);
});

test('★ `0x217` / `0x218` / `0x219` / `0x21a`：两组三元向量，写侧缺记录先建、读侧不建', () => {
  const m = machine();
  HANDLERS.get(0x217)(m, [op(0, 3), op(1, f32bits(1.5)), op(1, f32bits(2.5)), op(1, f32bits(3.5))]);
  assert.deepEqual(m.scene.drawItems.get(3).vec1, [1.5, 2.5, 3.5]);
  assert.equal(m.scene.dirty, 1);

  HANDLERS.get(0x219)(m, [op(0, 3), op(1, f32bits(4)), op(1, f32bits(5)), op(1, f32bits(6))]);
  assert.deepEqual(m.scene.drawItems.get(3).vec2, [4, 5, 6]);

  HANDLERS.get(0x218)(m, [op(0, 3), op(4, 1), op(4, 2), op(4, 3)]);
  assert.equal(f32(m.globalFloat.get(1)), 1.5);
  assert.equal(f32(m.globalFloat.get(2)), 2.5);
  assert.equal(f32(m.globalFloat.get(3)), 3.5);
  HANDLERS.get(0x21a)(m, [op(0, 3), op(4, 1), op(4, 2), op(4, 3)]);
  assert.equal(f32(m.globalFloat.get(3)), 6);

  HANDLERS.get(0x218)(m, [op(0, 9), op(4, 1), op(4, 2), op(4, 3)]);
  assert.equal(f32(m.globalFloat.get(1)), 0);
  assert.equal(m.scene.drawItems.has(9), false, '读侧不建记录');
});

test('★ `0x1fd` / `0x1ff` / `0x1fe`：缩放 ÷100、平移原值、旋转矩阵落点', () => {
  const m = machine();
  HANDLERS.get(0x1fd)(m, [op(0, 3), op(1, f32bits(100)), op(1, f32bits(200)), op(1, f32bits(300))]);
  assert.deepEqual(m.scene.drawItems.get(3).matrices[0].slice(0, 1), [1]);
  assert.equal(m.scene.drawItems.get(3).matrices[0][5], 2);
  assert.equal(m.scene.drawItems.get(3).matrices[0][10], 3);
  assert.equal(m.scene.drawItems.get(3).config, 1);

  HANDLERS.get(0x1ff)(m, [op(0, 3), op(1, f32bits(7)), op(1, f32bits(8)), op(1, f32bits(9))]);
  assert.deepEqual(m.scene.drawItems.get(3).matrices[4].slice(12, 15), [7, 8, 9]);

  HANDLERS.get(0x1fe)(m, [op(0, 3), op(0, 0), op(0, 0), op(0, 1), op(1, f32bits(90))]);
  const rot = m.scene.drawItems.get(3).matrices[2];
  assert.ok(Math.abs(rot[0]) < 1e-6);
  assert.ok(Math.abs(rot[5]) < 1e-6, '绕 z 轴 90° ⇒ m[5] = cos = 0');
  assert.ok(Math.abs(rot[10] - 1) < 1e-6, '绕 z 轴 ⇒ m[10] = 1');
  assert.deepEqual(m.scene.drawItems.get(3).vec2, [0, 0, 1]);
});

test('★ `0x220`：新记录位 0 = 0 ⇒ 只留记录；位 0 = 1 ⇒ G0..G3 + 第四个平移 + 脏/动画', () => {
  const m = machine();
  HANDLERS.get(0x220)(m, [op(0, 4), op(0, 11), op(0, 22), op(1, f32bits(1)), op(1, f32bits(2)), op(1, f32bits(3))]);
  assert.ok(m.scene.drawItems.has(4), '建默认记录');
  assert.equal(m.scene.drawItems.get(4).config, 0, '位 0 = 0 ⇒ 不写字段');

  const item = m.scene.drawItems.get(4);
  item.flags |= 1;
  HANDLERS.get(0x220)(m, [op(0, 4), op(0, 11), op(0, 22), op(1, f32bits(1)), op(1, f32bits(2)), op(1, f32bits(3))]);
  assert.equal(item.flags & 2, 2);
  assert.equal(item.g0, 0);
  assert.equal(item.g1, 11);
  assert.equal(item.g2, 22);
  assert.equal(item.config, 1);
  assert.deepEqual(item.matrices[5].slice(12, 15), [1, 2, 3]);
  assert.equal(m.scene.dirty, 1);
  assert.equal(m.scene.animating, 1);
});

test('★ `0x228`：读 0x1ff 的平移矩阵；缺记录 ⇒ 状态 1 且不写浮点', () => {
  const m = machine();
  HANDLERS.get(0x1ff)(m, [op(0, 3), op(1, f32bits(7)), op(1, f32bits(8)), op(1, f32bits(9))]);
  HANDLERS.get(0x228)(m, [op(3, 1), op(0, 3), op(4, 1), op(4, 2), op(4, 3)]);
  assert.equal(readInt(m, [op(3, 1)], 1), 0);
  assert.deepEqual([f32(m.globalFloat.get(1)), f32(m.globalFloat.get(2)), f32(m.globalFloat.get(3))], [7, 8, 9]);

  HANDLERS.get(0x228)(m, [op(3, 1), op(0, 99), op(4, 1), op(4, 2), op(4, 3)]);
  assert.equal(readInt(m, [op(3, 1)], 1), 1);
});

test('★ `0x01`：走退出通道（码 1），不写推进格数', () => {
  const m = machine();
  HANDLERS.get(0x01)(m, []);
  assert.equal(m.halted, true);
  assert.equal(m.exitCode, 1);
});

test('★ `0x0d9`：清状态选择字第 12 位；d9Cell 非 0 时再清 d9Word 第 12 位', () => {
  const m = machine();
  m.flags = (1 << 12) | (1 << 13);
  m.d9Cell = 1;
  m.d9Word = (1 << 12) | (1 << 14);
  HANDLERS.get(0x0d9)(m);
  assert.equal((m.flags >>> 12) & 1, 0);
  assert.equal((m.flags >>> 13) & 1, 1, '其它位保留');
  assert.equal((m.d9Word >>> 12) & 1, 0);
  assert.equal((m.d9Word >>> 14) & 1, 1);

  const n = machine();
  n.d9Cell = 0;
  n.d9Word = 1 << 12;
  HANDLERS.get(0x0d9)(n);
  assert.equal((n.d9Word >>> 12) & 1, 1, 'd9Cell = 0 ⇒ 不动 d9Word');
});

test('★ `0x1f4` / `0x1f5`：忙时只加嵌套计数；离开按深度 / 忙标记分支', () => {
  const m = machine();
  m.now = 100;
  m.bracketNow = 50;
  HANDLERS.get(0x1f4)(m);
  assert.equal(m.bracketBusy, 1);
  assert.equal(m.bracketPrev, 50);
  assert.equal(m.bracketNow, 100);
  HANDLERS.get(0x1f4)(m);
  assert.equal(m.bracketDepth, 1, '忙时只加计数，不动时间戳');

  HANDLERS.get(0x1f5)(m);
  assert.equal(m.bracketDepth, 0);
  assert.equal(m.bracketBusy, 1, '深度 > 0 ⇒ 不清忙');
  HANDLERS.get(0x1f5)(m);
  assert.equal(m.bracketBusy, 0);
});

test('★ `0x1ad`：把当前层号写进引擎格（默认 −1）', () => {
  const m = machine();
  assert.equal(m.layerCell, -1);
  m.currentLayer = 5;
  HANDLERS.get(0x1ad)(m, []);
  assert.equal(m.layerCell, 5);
});

test('★ `0x1ce`：非 0 启用（置第 30 位 + 播种）；0 且已置 ⇒ 清位', () => {
  const m = machine();
  m.now = 1234;
  HANDLERS.get(0x1ce)(m, [op(0, 7)]);
  assert.equal(m.rngEnabled, 1);
  assert.equal(m.rngSwitch, 7);
  assert.equal(m.rngSeed, 1234);
  assert.equal((m.flags >>> 30) & 1, 1);
  HANDLERS.get(0x1ce)(m, [op(0, 0)]);
  assert.equal(m.rngEnabled, 0);
  assert.equal((m.flags >>> 30) & 1, 0);
  HANDLERS.get(0x1ce)(m, [op(0, 0)]);
  assert.equal((m.flags >>> 30) & 1, 0, '未置位时再停用是空操作');
});

test('★ `0x7b` / `0x7c`：两个进入目标按帧存；0x7c 的身份 / 深度判据与整字覆盖', () => {
  const m = machine();
  HANDLERS.get(0x7b)(m, [op(0, 11), op(0, 22)]);
  assert.equal(m.frame.localMain, 11);
  assert.equal(m.frame.localSub, 22);

  const bad = machine();
  assert.throws(() => HANDLERS.get(0x7c)(bad), /不在本地块里/);
  assert.equal(bad.operandCount, 1, '失败支写推进格数 1');

  const depth = machine();
  depth.localSaveWord = 1 << 25;
  depth.frame.localDepth = 1;
  depth.localSaveDepth = 2;
  assert.throws(() => HANDLERS.get(0x7c)(depth), /深度不符/);

  const ok = machine();
  ok.localSaveWord = (1 << 25) | 0x400;
  ok.localSavePos = 7;
  ok.frame.localDepth = 3;
  ok.localSaveDepth = 3;
  ok.localCellA = ok.localCellB = ok.localCellC = ok.localCellD = 1;
  ok.localPending = 1;
  ok.flags = 0xffffffff;
  HANDLERS.get(0x7c)(ok);
  assert.equal(ok.ip, 28);
  assert.equal(ok.operandCount, 0);
  assert.equal(ok.flags, 0x400, '整字覆盖为存档字 & ~第 25 位（第 26 位保留）');
  assert.equal(ok.localSaveWord, 0);
  assert.deepEqual([ok.localCellA, ok.localCellB, ok.localCellC, ok.localCellD], [0, 0, 0, 0]);
  assert.equal(ok.localPending, 0);
});

test('★ `0x21e` / `0x21f`：带位 0 门；浮点 ÷100 / 轴角另存 + 矩阵落点', () => {
  const m = machine();
  const base = [op(0, 3), op(0, 11), op(0, 22), op(1, f32bits(100)), op(1, f32bits(200)), op(1, f32bits(300))];
  HANDLERS.get(0x21e)(m, base);
  assert.equal(m.scene.drawItems.get(3).fields.size, 0, '位 0 = 0 ⇒ 只建记录、不写字段');
  m.scene.drawItems.get(3).flags |= 1;
  HANDLERS.get(0x21e)(m, base);
  const a = m.scene.drawItems.get(3);
  assert.equal(a.fields.get(0x3c), 11);
  assert.equal(a.fields.get(0x50), 22);
  assert.equal(a.extraMatrices.get(0xac)[0], 1);
  assert.equal(a.extraMatrices.get(0xac)[5], 2);
  assert.equal(a.config, 1);
  assert.equal(m.scene.dirty, 1);
  assert.equal(m.scene.animating, 1);

  const r = machine();
  r.scene.drawItems.set(4, { ...m.scene.drawItems.get(3), flags: 1, fields: new Map() });
  HANDLERS.get(0x21f)(r, [op(0, 4), op(0, 1), op(0, 2), op(0, 0), op(0, 0), op(0, 1), op(1, f32bits(90))]);
  const b = r.scene.drawItems.get(4);
  assert.equal(b.fields.get(0x1f8), 0);
  assert.equal(b.fields.get(0x208), 90);
  assert.ok(Math.abs(b.extraMatrices.get(0x12c)[5]) < 1e-6);
});

test('★ `0x230` / `0x231`：附加组位 2 的清 / 置与六格复位', () => {
  const m = machine();
  const item = { ...m.scene.drawItems.get(0) };
  HANDLERS.get(0x231)(m, [op(0, 5), op(0, 11), op(0, 22), op(0, 33)]);
  const a = m.scene.drawItems.get(5);
  assert.equal(a.flags & 4, 4);
  assert.equal(a.fields.get(0x230), 11);
  assert.equal(a.fields.get(0x238), 22);
  assert.equal(a.fields.get(0x23c), 33);
  HANDLERS.get(0x230)(m, [op(0, 5)]);
  assert.equal(a.flags & 4, 0);
  assert.equal(a.fields.get(0x230), 0);
  assert.equal(a.fields.get(0x238), 22, '后两个值格不清');
});

test('★ `0x233` / `0x234` / `0x235`：附加组三类写入；`0x239` 带位 0 门', () => {
  const m = machine();
  HANDLERS.get(0x233)(m, [op(0, 6), op(0, 1), op(1, f32bits(100)), op(1, f32bits(200)), op(1, f32bits(300))]);
  const a = m.scene.drawItems.get(6);
  assert.equal(a.fields.get(0x224), 1);
  assert.equal(a.extraMatrices.get(0x250)[0], 1);
  assert.equal(a.extraMatrices.get(0x250)[5], 2);

  HANDLERS.get(0x234)(m, [op(0, 6), op(0, 2), op(1, f32bits(1)), op(1, f32bits(2)), op(1, f32bits(3))]);
  assert.equal(a.fields.get(0x228), 2);
  assert.equal(a.fields.get(0x244), 1);
  assert.equal(a.fields.get(0x24c), 3);

  HANDLERS.get(0x235)(m, [op(0, 6), op(0, 3), op(1, f32bits(4)), op(1, f32bits(5)), op(1, f32bits(6))]);
  assert.equal(a.fields.get(0x22c), 3);
  assert.deepEqual(a.extraMatrices.get(0x290).slice(12, 15), [4, 5, 6]);

  const g = machine();
  g.scene.drawItems.set(7, { flags: 0, config: 0, fields: new Map(), extraMatrices: new Map() });
  HANDLERS.get(0x239)(g, [op(0, 7), op(0, 1), op(0, 2), op(0, 3), op(0, 4), op(0, 5)]);
  assert.equal(g.scene.drawItems.get(7).config, 0, '位 0 = 0 ⇒ 不写字段 / 不写 G3');
});

test('★ `0x1c7`：只读状态选择字第 27 位，并归一成 0 / 1', () => {
  const m = machine();
  m.flags = 1 << 27;
  HANDLERS.get(0x1c7)(m, [op(3, 1)]);
  assert.equal(readInt(m, [op(3, 1)], 1), 1);
  m.flags = 0;
  HANDLERS.get(0x1c7)(m, [op(3, 1)]);
  assert.equal(readInt(m, [op(3, 1)], 1), 0);
  m.flags = 0xffffffff;
  HANDLERS.get(0x1c7)(m, [op(3, 1)]);
  assert.equal(readInt(m, [op(3, 1)], 1), 1, '其它位全置也必须只输出 1');
});
