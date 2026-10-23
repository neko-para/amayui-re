/** @env pure @kind contract @why 浮点/位运算方向、0x2da 编号上限、0x23b 逐位读序、0x1a0 失败路径只写 op1、四张同键表集合任一写错都不抛，只会静默算错或读错操作数 */
/**
 * tools/test/emulator-new-ops-100-200.test.mjs —— 0x191 / 0x2d0 / 0x2d1 / 0x2d3 / 0x1c7 的合同守卫。
 *
 * ★ 判据为什么是这些：
 *   - 0x2d1 / 0x2d3 的**方向**（op2 ∘ op3）写反不会报错，只会静默算错；
 *   - 0x191 必须取绝对值，不能只搬运；
 *   - 0x1c7 只读**第 27 位**并归一成 0/1，不能把整字或别的位写出去；
 *   - 0x2da 的编号上限是 **10**（不是 999），且越界时**其余七个操作数一次都不读**；
 *   - 0x23b 的 op4 / op5 只在**每个被画出的位**上读；位数 ≤ 0 时一次都不读；
 *   - 0x1f7 / 0x1f6 / 0x23b 删 / 清的是**同一组四张同键表**（对象记录 + 网格 + 两张辅助）。
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';

import { Machine } from '../../apps/emulator-new/src/core/machine.ts';
import { HeadlessRenderHost } from '../../apps/emulator-new/src/core/render.ts';
import { HANDLERS } from '../../apps/emulator-new/src/core/handlers.ts';
import { encodeInt, readInt } from '../../apps/emulator-new/src/core/values.ts';

const KEY = 0x1234abcd;
const op = (type, raw) => ({ type, raw });
const f32bits = (v) => new Uint32Array(new Float32Array([v]).buffer)[0] >>> 0;
const f32 = (bits) => new Float32Array(new Uint32Array([bits >>> 0]).buffer)[0];

function machine(opts = {}) {
  return new Machine({ body: Buffer.alloc(4), table: new Map(), key: KEY, ...opts });
}

/**
 * `0x1f7` / `0x1f6` / `0x23b` 共用的**四张同键表**（对象记录 + 网格 + 两张辅助）。
 * ★ 守卫按这份清单逐表断言：任何一条指令少删 / 少清一张，都会在对应表上红。
 */
function tableEntriesOf(m) {
  return [
    ['drawItems', m.scene.drawItems],
    ['meshes', m.scene.meshes],
    ...m.scene.auxTables.map((table, i) => [`auxTables[${i}]`, table]),
  ];
}

/** 往四张同键表里各放同样的键（绘制项用可辨认的 `textureId`，便于判断"被替换"）。 */
function fillFourTables(m, keys) {
  for (const key of keys) {
    m.scene.drawItems.set(key, { flags: 0, textureId: 0xdead, left: 0, top: 0, right: 0, bottom: 0, aux: [0, 0, 0] });
    m.scene.meshes.set(key, {});
    for (const table of m.scene.auxTables) table.set(key, {});
  }
}

function saveHost(overrides = {}) {
  const calls = [];
  return {
    calls,
    fileForSlot: (slot, kind) => `slot${slot}.${kind}`,
    read: async (p) => { calls.push(`read:${p}`); return overrides.read?.(p) ?? null; },
    write: async (p, b) => { calls.push(`write:${p}`); return overrides.write?.(p, b) ?? true; },
    rename: async (a, b) => { calls.push(`rename:${a}->${b}`); return overrides.rename?.(a, b) ?? true; },
    remove: async (p) => { calls.push(`remove:${p}`); return overrides.remove?.(p) ?? true; },
    copy: async (a, b) => { calls.push(`copy:${a}->${b}`); return overrides.copy?.(a, b) ?? true; },
    ...(overrides.confirmOverwrite ? { confirmOverwrite: (s) => overrides.confirmOverwrite(s) } : {}),
  };
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

test('★ `0x2da`：先读编号、上限 10；越界（含负数）⇒ 错误通道且其余七个操作数一次都不读', () => {
  const m = machine();
  HANDLERS.get(0x2da)(m, [op(0, 3), ...Array.from({ length: 7 }, (_, i) => op(0, i + 10))]);
  assert.deepEqual(m.scene.cgRecords.get(3), [10, 11, 12, 13, 14, 15, 16]);

  // 10 是合法上限（引擎 `cmp edi,0Ah; ja`，不是 > 999）
  HANDLERS.get(0x2da)(m, [op(0, 10), ...Array.from({ length: 7 }, () => op(0, 7))]);
  assert.deepEqual(m.scene.cgRecords.get(10), Array(7).fill(7));

  const before = m.diagnostics.length;
  // 非法操作数类型：只要被读就抛 ⇒ 用它证明"越界时其余七个操作数一次都不读"
  const bad = op(0x63, 0);
  HANDLERS.get(0x2da)(m, [op(0, 11), bad, bad, bad, bad, bad, bad, bad]);
  assert.equal(m.scene.cgRecords.has(11), false, '越界不写记录');
  assert.equal(m.diagnostics.length, before + 1, '越界进错误通道');
  assert.equal(m.scene.errorBuffer, 'CGNumberの引数が不正です．\r\n', '错误文本与 0x23b 同一条');

  // 负数按越界处理（无符号 ja）：不抛、同一文本
  HANDLERS.get(0x2da)(m, [op(0, -1), bad, bad, bad, bad, bad, bad, bad]);
  assert.equal(m.scene.cgRecords.has(-1), false);
  assert.equal(m.diagnostics.length, before + 2);

  // 合法编号时 op2 确实会被读 ⇒ 非法类型当场抛（证明上面的"不读"不是恒真断言）
  assert.throws(
    () => HANDLERS.get(0x2da)(m, [op(0, 0), bad, op(0, 0), op(0, 0), op(0, 0), op(0, 0), op(0, 0), op(0, 0)]),
    /unsupported-operand-type/,
  );
});

test('★ `0x1f6`：清**四张**同键表（对象记录 / 网格 / 两张辅助）与脏标记，但不动“有动画”', () => {
  const m = machine();
  fillFourTables(m, [1, 2]);
  m.scene.dirty = 1;
  m.scene.animating = 1;
  HANDLERS.get(0x1f6)(m);
  assert.equal(tableEntriesOf(m).length, 4, '统一集合是四张表：对象记录 + 网格 + 两张辅助');
  for (const [name, table] of tableEntriesOf(m)) assert.equal(table.size, 0, `${name} 必须被清空`);
  assert.equal(m.scene.dirty, 0);
  assert.equal(m.scene.animating, 1, '“有动画”不在清空范围');
});

test('★ `0x23b`：位数 ≤ 0 ⇒ 不读 op4 / op5（非法类型也不会被碰到）、不建项', () => {
  const m = machine();
  m.scene.cgRecords.set(5, [1, 100, 200, 12, 15, 2, -3]);
  m.scene.drawItems.set(300, { flags: 1, textureId: 9, left: 0, top: 0, right: 0, bottom: 0, aux: [0, 0, 0] });
  m.scene.dirty = 0;
  const bad = op(0x63, 0);
  HANDLERS.get(0x23b)(m, [op(0, 300), op(0, 5), op(0, 123), bad, bad, op(0, 0), op(0, 1)]);
  assert.equal(m.scene.drawItems.get(300)?.textureId, 9, '位数 0 的清理区间为空 ⇒ 旧项不动');
  assert.equal(m.scene.drawItems.size, 1, '不建项');
  assert.equal(m.scene.dirty, 0, '没删没建 ⇒ 不置脏');

  // 位数 1 ⇒ 会进建项分支读 op4：非法类型当场抛（证明上面的"不读"不是恒真断言）
  const one = machine();
  one.scene.cgRecords.set(5, [1, 100, 200, 12, 15, 2, -3]);
  assert.throws(
    () => HANDLERS.get(0x23b)(one, [op(0, 300), op(0, 5), op(0, 123), bad, op(0, 7), op(0, 1), op(0, 0)]),
    /unsupported-operand-type/,
  );
});

test('★ 0x1f7 / 0x1f6 / 0x23b：删 / 清的**表集合相同**（对象记录 + 网格 + 两张辅助）', () => {
  // ① 0x1f7 区间删除：四张表都删 [100, 102)，102 保留
  const detach = machine();
  fillFourTables(detach, [100, 101, 102]);
  HANDLERS.get(0x1f7)(detach, [op(0, 100), op(0, 2)]);
  assert.equal(tableEntriesOf(detach).length, 4, '统一集合是四张表：对象记录 + 网格 + 两张辅助');
  for (const [name, table] of tableEntriesOf(detach)) {
    assert.equal(table.has(100), false, `0x1f7 必须删 ${name} 的 100`);
    assert.equal(table.has(101), false, `0x1f7 必须删 ${name} 的 101`);
    assert.equal(table.has(102), true, `0x1f7 不许删 ${name} 的 102（左闭右开）`);
  }

  // ② 0x23b 删旧项：同一组四张表都删 [300, 302)，302 保留；
  //    绘制项会被同键新项覆盖 ⇒ 用新项的 textureId 证明旧项确实被删过。
  const cg = machine();
  cg.scene.cgRecords.set(5, [7, 0, 0, 12, 15, 2, -3]);
  fillFourTables(cg, [300, 301, 302]);
  HANDLERS.get(0x23b)(cg, [op(0, 300), op(0, 5), op(0, 5), op(0, 0), op(0, 0), op(0, 2), op(0, 0)]);
  assert.equal(cg.scene.drawItems.get(300)?.textureId, 7, '300 上的旧绘制项被新项替换（说明清理走的是同一张表）');
  assert.equal(cg.scene.drawItems.has(302), true, '302 在清理区间外');
  for (const [name, table] of tableEntriesOf(cg)) {
    if (name === 'drawItems') continue; // 绘制项被新项重新占用，用上面的 textureId 判断
    assert.equal(table.has(300), false, `0x23b 必须删 ${name} 的 300`);
    assert.equal(table.has(301), false, `0x23b 必须删 ${name} 的 301`);
    assert.equal(table.has(302), true, `0x23b 不许删 ${name} 的 302`);
  }
  assert.equal(cg.scene.dirty, 1);
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

test('★ `0xc8`：n < 10 走宿主阻塞；n ≥ 10 置第 0 位 + 定时器；位 27 为 1 ⇒ 跳过', async () => {
  const slept = [];
  const m = machine({ sleepHost: async (ms) => { slept.push(ms); } });
  await HANDLERS.get(0xc8)(m, [op(0, 5)]);
  assert.deepEqual(slept, [5]);
  assert.equal(m.flags & 1, 0);

  const long = machine();
  long.now = 100;
  await HANDLERS.get(0xc8)(long, [op(0, 10)]);
  assert.equal(long.flags & 1, 1);
  assert.equal(long.sleepTimer, 10);
  assert.equal(long.sleepUntil, 110);

  const skip = machine({ sleepHost: async () => { throw new Error('不应睡'); } });
  skip.flags = 1 << 27;
  await HANDLERS.get(0xc8)(skip, [op(0, 5)]);
});

test('★ `0x19e`：目标不存在 / 头可读 ⇒ 直接写；头不合法 ⇒ 问宿主，拒绝写 1', async () => {
  let writes = 0;
  const none = machine({ saveHost: saveHost({ write: async () => { writes += 1; return true; } }) });
  await HANDLERS.get(0x19e)(none, [op(3, 1), op(0, 3)]);
  assert.equal(readInt(none, [op(3, 1)], 1), 0);
  assert.equal(writes, 1);

  const badBytes = Buffer.alloc(292);
  let confirmed = 0;
  const deny = machine({ saveHost: saveHost({
    read: () => badBytes, confirmOverwrite: async () => { confirmed += 1; return false; },
    write: async () => { writes += 1; return true; },
  }) });
  await HANDLERS.get(0x19e)(deny, [op(3, 1), op(0, 3)]);
  assert.equal(readInt(deny, [op(3, 1)], 1), 1);
  assert.equal(confirmed, 1);
  assert.equal(writes, 1, '拒绝 ⇒ 不再写');

  const allow = machine({ saveHost: saveHost({
    read: () => badBytes, confirmOverwrite: async () => true,
    write: async () => { writes += 1; return true; },
  }) });
  await HANDLERS.get(0x19e)(allow, [op(3, 1), op(0, 3)]);
  assert.equal(readInt(allow, [op(3, 1)], 1), 0);
  assert.equal(writes, 2);
});

test('★ `0x1a1`：无确认写槽；文件头 + 参数块 + 非压缩信封落盘', async () => {
  let written = null;
  const host = saveHost({ write: async (_p, bytes) => { written = bytes; return true; } });
  const m = machine({ saveHost: host });
  m.slots[0].id = 7;
  m.globalInt.set(0, encodeInt(5, KEY));
  m.globalFloat.set(0, f32bits(1.5));
  m.globalString.set(0, 'AB');
  await HANDLERS.get(0x1a1)(m, [op(3, 1), op(0, 4)]);
  assert.equal(readInt(m, [op(3, 1)], 1), 0);
  assert.ok(written && written.length > 340);
  assert.equal(new TextDecoder().decode(written.slice(0, 4)), 'S4SD');
  assert.equal(new DataView(written.buffer).getUint32(284, true), 3);
  assert.equal(new DataView(written.buffer).getUint32(288, true), 20);
});

test('★ `0x1a0`：读 292 字节头，7 个字段按序写 op3..9；打不开 1 / 头不合法 2', async () => {
  const buf = Buffer.alloc(292);
  buf.write('S4SD', 0, 'latin1');
  buf.writeUInt16LE(2026, 264); buf.writeUInt16LE(10, 266); buf.writeUInt16LE(23, 270);
  buf.writeUInt16LE(12, 272); buf.writeUInt16LE(34, 274); buf.writeUInt16LE(56, 276);
  buf.writeUInt32LE(789, 280);
  // 判别力：星期 / 毫秒 / 两个版本都填成与七个值不撞的数 ⇒ 读错字段会当场对不上。
  buf.writeUInt16LE(7, 268); buf.writeUInt16LE(999, 278);
  buf.writeUInt32LE(3, 284); buf.writeUInt32LE(20, 288);
  const m = machine({ saveHost: saveHost({ read: () => buf }) });
  const ops = [op(3, 0), op(0, 5), op(3, 1), op(3, 2), op(3, 3), op(3, 4), op(3, 5), op(3, 6), op(3, 7)];
  await HANDLERS.get(0x1a0)(m, ops);
  assert.equal(readInt(m, ops, 1), 0);
  assert.deepEqual([1, 2, 3, 4, 5, 6, 7].map((k) => readInt(m, ops, 2 + k)), [2026, 10, 23, 12, 34, 56, 789]);

  const miss = machine({ saveHost: saveHost({ read: () => null }) });
  // ★ 失败路径只写状态码（op1）：op3..9 必须保持原值（不许写半截字段）。
  const sentinels = [111, 222, 333, 444, 555, 666, 777];
  const seedFields = (mm) => sentinels.forEach((v, i) => mm.globalInt.set(1 + i, encodeInt(v, KEY)));
  seedFields(miss);
  await HANDLERS.get(0x1a0)(miss, ops);
  assert.equal(readInt(miss, ops, 1), 1);
  assert.deepEqual([1, 2, 3, 4, 5, 6, 7].map((k) => readInt(miss, ops, 2 + k)), sentinels, '打不开 ⇒ op3..9 保持原值');

  const bad = machine({ saveHost: saveHost({ read: () => Buffer.alloc(292) }) });
  seedFields(bad);
  await HANDLERS.get(0x1a0)(bad, ops);
  assert.equal(readInt(bad, ops, 1), 2);
  assert.deepEqual([1, 2, 3, 4, 5, 6, 7].map((k) => readInt(bad, ops, 2 + k)), sentinels, '头不合法 ⇒ op3..9 保持原值');
});

test('★ `0x1ab` / `0x1ac`：先 .DAT 后 .STH，两个都总是尝试；第二个失败覆盖第一个', async () => {
  // 都成功 ⇒ 0；两个文件都碰过。
  const ok = machine({ saveHost: saveHost() });
  await HANDLERS.get(0x1ab)(ok, [op(3, 1), op(0, 1)]);
  assert.equal(readInt(ok, [op(3, 1)], 1), 0);
  assert.deepEqual(ok.saveHost.calls, ['remove:slot1.dat', 'remove:slot1.sth']);

  // .DAT 失败、.STH 成功 ⇒ 1；★ 第一个失败也必须继续做第二个。
  const del = machine({ saveHost: saveHost({ remove: (p) => p !== 'slot1.dat' }) });
  await HANDLERS.get(0x1ab)(del, [op(3, 1), op(0, 1)]);
  assert.equal(readInt(del, [op(3, 1)], 1), 1, '.DAT 失败 + .STH 成功 ⇒ 1');
  assert.deepEqual(del.saveHost.calls, ['remove:slot1.dat', 'remove:slot1.sth'], '★ .DAT 失败 ⇒ 仍要删 .STH');

  // 两个都失败 ⇒ 2（后一步覆盖前一步），且两个都尝试过。
  const delBoth = machine({ saveHost: saveHost({ remove: () => false }) });
  await HANDLERS.get(0x1ab)(delBoth, [op(3, 1), op(0, 1)]);
  assert.equal(readInt(delBoth, [op(3, 1)], 1), 2, '★ 两个都失败 ⇒ 2（不是 1）');
  assert.deepEqual(delBoth.saveHost.calls, ['remove:slot1.dat', 'remove:slot1.sth'], '★ 两个都失败 ⇒ 两个都尝试过');

  const del2 = machine({ saveHost: saveHost({ remove: (p) => p !== 'slot2.sth' }) });
  await HANDLERS.get(0x1ab)(del2, [op(3, 1), op(0, 2)]);
  assert.equal(readInt(del2, [op(3, 1)], 1), 2, '.STH 失败 ⇒ 2');

  // 0x1ac 同形。
  const copy = machine({ saveHost: saveHost({ copy: (a) => a !== 'slot1.dat' }) });
  await HANDLERS.get(0x1ac)(copy, [op(3, 1), op(0, 1), op(0, 2)]);
  assert.equal(readInt(copy, [op(3, 1)], 1), 1);
  assert.deepEqual(copy.saveHost.calls, ['copy:slot1.dat->slot2.dat', 'copy:slot1.sth->slot2.sth']);

  const copyBoth = machine({ saveHost: saveHost({ copy: () => false }) });
  await HANDLERS.get(0x1ac)(copyBoth, [op(3, 1), op(0, 1), op(0, 2)]);
  assert.equal(readInt(copyBoth, [op(3, 1)], 1), 2, '两个都失败 ⇒ 2');
  assert.deepEqual(copyBoth.saveHost.calls, ['copy:slot1.dat->slot2.dat', 'copy:slot1.sth->slot2.sth'], '两个都尝试过');

  const copy2 = machine({ saveHost: saveHost({ copy: (a) => a !== 'slot1.sth' }) });
  await HANDLERS.get(0x1ac)(copy2, [op(3, 1), op(0, 1), op(0, 2)]);
  assert.equal(readInt(copy2, [op(3, 1)], 1), 2, '.STH 失败 ⇒ 2');
});

test('★ `0x21d`：源 → 目标整份复制；三张表都无源 ⇒ 错误通道且不改', () => {
  const m = machine();
  HANDLERS.get(0x217)(m, [op(0, 1), op(1, f32bits(1)), op(1, f32bits(2)), op(1, f32bits(3))]);
  HANDLERS.get(0x21d)(m, [op(0, 1), op(0, 2)]);
  assert.deepEqual(m.scene.drawItems.get(2).vec1, [1, 2, 3]);
  assert.notEqual(m.scene.drawItems.get(1), m.scene.drawItems.get(2));
  assert.equal(m.scene.dirty, 1);

  const before = m.diagnostics.length;
  HANDLERS.get(0x21d)(m, [op(0, 9), op(0, 10)]);
  assert.equal(m.scene.drawItems.has(10), false);
  assert.equal(m.diagnostics.length, before + 1);
});

test('★ `0x1ae` / `0x1af`：缩略图捕捉 → .STH；读 .STH → 校验 BM → 上传', async () => {
  const bmp = Buffer.alloc(64); bmp.write('BM', 0, 'latin1');
  let written = null;
  const w = machine({
    renderHost: new HeadlessRenderHost({ captureBytes: bmp }),
    saveHost: saveHost({ write: async (p, b) => { written = { p, b }; return true; } }),
  });
  await HANDLERS.get(0x1ae)(w, [op(3, 1), op(0, 4), op(0, 9)]);
  assert.equal(readInt(w, [op(3, 1)], 1), 0);
  assert.equal(written.p, 'slot4.sth');
  assert.equal(written.b[0], 0x42);

  const rh = new HeadlessRenderHost();
  const r = machine({ renderHost: rh, saveHost: saveHost({ read: () => bmp }) });
  await HANDLERS.get(0x1af)(r, [op(3, 1), op(0, 4), op(0, 9)]);
  assert.equal(readInt(r, [op(3, 1)], 1), 0);
  assert.equal(rh.requests.some((x) => x.kind === 'upload-texture-bytes'), true);

  const miss = machine({ renderHost: rh, saveHost: saveHost({ read: () => null }) });
  await HANDLERS.get(0x1af)(miss, [op(3, 1), op(0, 4), op(0, 9)]);
  assert.equal(readInt(miss, [op(3, 1)], 1), 1);

  const bad = machine({ renderHost: rh, saveHost: saveHost({ read: () => Buffer.alloc(64) }) });
  await HANDLERS.get(0x1af)(bad, [op(3, 1), op(0, 4), op(0, 9)]);
  assert.equal(readInt(bad, [op(3, 1)], 1), 2);
});

test('★ `0x20b`：颜色打包 / alpha 上夹 / 夹到对象逻辑矩形 / 走 RenderHost.fillTexture', async () => {
  const host = new HeadlessRenderHost();
  const m = machine({ renderHost: host });
  m.scene.textureObjects.set(3, { kind: 'normal', width: 320, height: 180 });
  await HANDLERS.get(0x20b)(m, [
    op(0, 3), op(0, -10), op(0, -5), op(0, 100), op(0, 100), op(0, 300), op(0, 0x112233),
  ]);
  const req = host.requests.find((r) => r.kind === 'fill-texture');
  assert.ok(req);
  assert.deepEqual(
    [req.request.left, req.request.top, req.request.right, req.request.bottom],
    [0, 0, 90, 95],
  );
  assert.equal(req.request.color, 0xff112233);
  assert.equal(req.request.alpha, 255);
  assert.equal(m.scene.textures.get(3).blank, 1);

  const empty = new HeadlessRenderHost();
  const n = machine({ renderHost: empty });
  n.scene.textureObjects.set(4, { kind: 'normal', width: 10, height: 10 });
  await HANDLERS.get(0x20b)(n, [op(0, 4), op(0, 20), op(0, 20), op(0, 5), op(0, 5), op(0, 0), op(0, 0)]);
  assert.equal(empty.requests.some((r) => r.kind === 'fill-texture'), false, '空矩形不调 fill');
});

test('★ `0x242`：同一个整数写进两张表；不标脏', () => {
  const m = machine();
  HANDLERS.get(0x242)(m, [op(0, 9), op(0, 77)]);
  assert.equal(m.scene.drawItems.get(9).fields.get(0x2d0), 77);
  assert.equal(m.scene.aux242.get(9), 77);
  assert.equal(m.scene.dirty, 0);
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
