/** @env pure @kind contract @why 池与区域两份数据不一致：经地址空间读到的与经池 API 读到的不是同一个值（而两条路都"看起来正常"） */
/**
 * tools/test/emulator-pool-regions.test.mjs —— **池存储迁到区域**的判据（ADR 第 ② 步）
 *
 * ## 它守的是什么
 * ADR（`REQ-01M4ARC3CPM00CC1KC4Q3HF550`）第 ② 步的要求是"池的存储换成**区域**"，判据是
 * **既有守卫全绿 + 经地址空间读到的池值 == 经池 API 读到的值**。这条文件就是后半句：
 *
 * 1. **一份数据**（不是两份）：迁到区域的池，写完之后 `Map` 必须是**空的** ——
 *    否则"值存在哪"就有两个答案，快照该信谁变成运气问题。
 * 2. **两条路给出同一个值**：`pool.read(tag, idx)` == `DEC(space.readU32(region.addressOf(idx)))`。
 * 3. **快照格式逐字不变**：把同样的写入分别喂给"区域版"与"Map 版"，两者的快照必须**逐值相同**
 *    —— 这是"迁移不改外部形状"的可执行形式（也是为什么分区表/恢复路径都不用动）。
 * 4. **增长留痕**：容量初值 0、按需增长，且每次真涨都回调（决策 `REQ-01M4B969TBWVERFCB1MXS2Q2E1`）。
 * 5. **未写过的格子仍是 `null`**（不是 0）—— 迁移不许悄悄改这条口径。
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';

import { GlobalPools, LOCAL_POOLS, LocalPools } from '../../apps/emulator/src/model/pools.ts';
import { AddressSpace, REGION_STRIDE } from '../../apps/emulator/src/model/address-space.ts';
import { encInt } from '@amayui/age-format/src/asm/value-codec.mts';
import { bitsFromFloat } from '../../apps/emulator/src/model/float-bits.ts';
import { HANDLERS } from '../../apps/emulator/src/vm/ops.ts';
import { readOperand, writeOperand } from '../../apps/emulator/src/vm/operand.ts';

const KEY = 0x5a5a1234; // ★ 用一个**非 0** 的键：键为 0 时 ENC 退化成恒等，那样守卫会失去分辨力

/** 造一个"区域版"的 LocalPools（迁移开启） */
function regionLocal(key = KEY) {
  const grew = [];
  const space = new AddressSpace({ onGrow: (e) => grew.push(e) });
  return { pools: new LocalPools(key, undefined, { space }), space, grew };
}

test('★★ 区域**不许重叠**（窗口）：容量 0 的两个区域基址必须不同；增长不许越窗（静默串数据的根因）', () => {
  const space = new AddressSpace();
  const a = space.alloc({ tag: 'a', elemBytes: 4, capacity: 0 });
  const b = space.alloc({ tag: 'b', elemBytes: 4, capacity: 0 });
  // ① 容量 0 不是"没有地址"：两个区域必须落在**不同**地址上（本仓实测踩过：同基址 ⇒ 串数据）
  assert.notEqual(a.base, b.base, '★ 两个容量 0 的区域不许共用基址');
  // ② 增长不许吞掉邻居：区域之间至少隔一个窗口
  assert.ok(b.base - a.base >= REGION_STRIDE, `区域之间至少隔一个窗口（${REGION_STRIDE}）`);
  // ③ 窗口内的容量上限：4 字节元素 ⇒ STRIDE/4 格；再多就要人决定（响亮失败，不自动重叠）
  const maxIndex = REGION_STRIDE / 4 - 1;
  assert.equal(space.ensureCapacity(a, maxIndex, '到窗口顶'), true, '窗口内可以涨到顶');
  assert.throws(() => space.ensureCapacity(a, maxIndex + 1, '越窗'), /增长会与下一个区域重叠/,
    '★ 越窗必须响亮失败 —— ⛔ 不许自动重叠（那会让"写进 A 的值从 B 的地址读到"）');
  // ④ 越窗失败**不该**改动容量（半途而废的扩容会让后续判据建立在坏状态上）
  assert.equal(a.capacity, maxIndex + 1, '失败的扩容不该改容量');
});

test('★★ `0x61 lookup-array` 的目标是指针 ⇒ 写的是**那一格的地址**（不是值）；解引用才拿到值', () => {
  const space = new AddressSpace();
  const locals = new LocalPools(0, undefined, { space });
  const globals = new GlobalPools(0, undefined, { space });
  const ctx = { locals, globals, script: {}, space };
  for (const [i, v] of [[100, 7], [101, 8], [102, 9]]) globals.write('int', i, v);

  // `lookup-array (local-ptr 0) (global-int 100) (1)` —— 取 &global:int[101]
  const ins = {
    opcode: 0x61, name: 'lookup-array', argc: 3, index: 0,
    args: [{ type: 0xc, rawData: 0 }, { type: 3, rawData: 100 }, { type: 0, rawData: 1 }],
  };
  const machine = { space, globals, notes: [], note(k, d) { this.notes.push(`${k}:${d}`); }, effect() {} };
  const frame = { locals };
  HANDLERS[0x61]({ machine, frame, script: {}, ins });

  // ★ 指针格里存的必须是**地址**
  const wantAddr = globals.regionOf('int').addressOf(101);
  assert.equal(space.readU32(locals.regionOf(0xc).addressOf(0)), wantAddr,
    '★ 写进指针格的是 `&global:int[101]`（若写成"那一格的值 8"，下一步就会去解引用 8）');
  // ★ 于是解引用才拿到 8 —— 这一条把"地址 vs 值"两种实现分开
  assert.equal(readOperand(ctx, { type: 0xc, rawData: 0 }, 0).value, 8, '解引用后 = 8');
});

test('★★ `0x64 copy-local-array`：块首 u32 = 元素个数、数据从 +4 起、逐格写 **ENC(源)**', () => {
  const space = new AddressSpace();
  const locals = new LocalPools(0, undefined, { space });
  const globals = new GlobalPools(0, undefined, { space });
  // 造一份脚本：headerLen 60，块在 raw=0 ⇒ offset 60：count=3，然后 11 / 22 / 33（小端）
  const bytes = new Uint8Array(100);
  const put = (o, v) => { bytes[o] = v & 0xff; bytes[o + 1] = (v >>> 8) & 0xff; bytes[o + 2] = (v >>> 16) & 0xff; bytes[o + 3] = (v >>> 24) & 0xff; };
  put(60, 3); put(64, 11); put(68, 22); put(72, 33);
  const script = { name: 'STUB.BIN', headerLen: 60, bytes };
  const ins = {
    opcode: 0x64, name: 'copy-local-array', argc: 2, index: 0,
    args: [{ type: 3, rawData: 1000 }, { type: 0, rawData: 0 }],
  };
  const machine = { space, globals, notes: [], note(k, d) { this.notes.push(`${k}:${d}`); }, effect() {} };
  HANDLERS[0x64]({ machine, frame: { locals }, script, ins });

  // ★ 三格写进 `&global:int[1000]`，值经 ENC 存、经池 API 读回来是原值
  for (const [i, v] of [[0, 11], [1, 22], [2, 33]]) {
    assert.equal(globals.read('int', 1000 + i), v, `第 ${i} 格`);
    assert.equal(space.readU32(globals.regionOf('int').addressOf(1000 + i)), encInt(v, 0), `第 ${i} 格：内存里是 ENC(源)`);
  }
  // ★ 第 count 格之后**不该**被写过。★ 注意：它此刻还在区域容量之外 ⇒ 直接读会**响亮失败**
  //   （这正是"越界 = UB，本层不模仿"的体现）；所以先显式长到那一格，再断言它是空的。
  const next = globals.regionOf('int').addressOf(1003);
  assert.throws(() => space.readU32(next), /不落在任何区域里/, '容量外 ⇒ 抛（⛔ 不是安静地读别处）');
  space.ensureAddress(next, '守卫：把容量长到第 count 格');
  assert.equal(space.readU32(next), null, '⛔ 只写 count 格（多写一格就是偏移口径错了）');

  // ★ `count === 0` ⇒ 引擎什么都不做（不抛）
  const bytes2 = new Uint8Array(100);
  HANDLERS[0x64]({ machine, frame: { locals }, script: { name: 'S', headerLen: 60, bytes: bytes2 }, ins });
  assert.ok(machine.notes.some((n) => n.startsWith('copy-local-array-empty')), 'count=0 要留一笔（可见），而不是静默');
});

test('★★ 决策：string 元素**不透明**（JS 字符串）—— 区域只发地址、数据在 Map、`0xe` 靠地址定位元素', () => {
  const space = new AddressSpace();
  const locals = new LocalPools(0, undefined, { space });
  const globals = new GlobalPools(0, undefined, { space });
  const ctx = { locals, globals, script: {}, space };

  locals.write(11, 3, 'こんにちは');                       // 局部字符串池第 3 格
  const addr = locals.regionOf(11).addressOf(3);
  assert.equal(addr, locals.regions.get('string').base + 28 * 3, '★ 元素步长是 **28**（与引擎一致）');
  assert.equal(space.readU32(addr), null, '★ 区域里**没有**字符串数据（格是空的）');
  assert.equal(locals.pools.get('string').get(3), 'こんにちは', '数据在 Map 里');

  // `0xe`：取地址 → 定位元素 → 那个 JS 字符串
  writeOperand(ctx, { type: 0xe, rawData: 0 }, 0, addr);
  assert.equal(readOperand(ctx, { type: 0xe, rawData: 0 }, 0).value, 'こんにちは', '经 `0xe` 读回来是同一个字符串');

  // ⛔ 两处必须**响亮失败**：指向非字符串区域、没对齐
  writeOperand(ctx, { type: 0xe, rawData: 0 }, 0, locals.regionOf(9).addressOf(0));
  assert.throws(() => readOperand(ctx, { type: 0xe, rawData: 0 }, 0), /不是字符串元素/);
  writeOperand(ctx, { type: 0xe, rawData: 0 }, 0, addr + 4);
  assert.throws(() => readOperand(ctx, { type: 0xe, rawData: 0 }, 0), /没对齐/);

  // ★ 快照口径不变：字符串池从 **Map** 取（区域里没有它的数据）
  assert.deepEqual(locals.snapshot().pools.find(([n]) => n === 'string')[1], [[3, 'こんにちは']],
    '字符串池的快照 = Map 的内容（不是区域里那堆空格子）');
});

test('★★ 数组类 opcode 的步长**按目标族取**（`0xc` ⇒ 4、`0xe` ⇒ 28）；`0x12c` 是二维索引 `op3*op4+op5`', () => {
  const space = new AddressSpace();
  const locals = new LocalPools(0, undefined, { space });
  const globals = new GlobalPools(0, undefined, { space });
  const ctx = { locals, globals, script: {}, space };
  const machine = { space, globals, notes: [], note(k, d) { this.notes.push(`${k}:${d}`); }, effect() {} };

  // ★ `0xe`（局部字符串指针）：步长必须是 **28** —— 写死 4 会算到别的元素上（语料里 674 个站点）
  globals.write('string', 2, 'ふたつめ');
  const strBase = globals.regionOf('string').addressOf(0);          // &global:string[0]（步长 28）
  HANDLERS[0x61]({
    machine, frame: { locals }, script: {},
    ins: { opcode: 0x61, name: 'lookup-array', argc: 3, index: 0,
      args: [{ type: 0xe, rawData: 0 }, { type: 5, rawData: 0 }, { type: 0, rawData: 2 }] },
  });
  assert.equal(space.readU32(locals.regionOf(0xe).addressOf(0)), strBase + 28 * 2,
    '★ 28 字节族：写进字符串指针格的是 `base + 28*idx`（⛔ 不是 `base + 4*idx`）');
  assert.equal(readOperand(ctx, { type: 0xe, rawData: 0 }, 0).value, 'ふたつめ', '解引用拿到第 2 个字符串元素');

  // ★ `0x12c` 的二维索引：`base[op3*op4 + op5]`
  for (let i = 0; i < 200; i += 1) globals.write('int', 100 + i, i);
  const intBase = globals.regionOf('int').addressOf(100);
  HANDLERS[0x12c]({
    machine, frame: { locals }, script: {},
    ins: { opcode: 0x12c, name: 'lookup-array-2d', argc: 5, index: 0,
      args: [{ type: 0xc, rawData: 0 }, { type: 3, rawData: 100 }, { type: 0, rawData: 3 },
        { type: 0, rawData: 10 }, { type: 0, rawData: 4 }] },
  });
  assert.equal(space.readU32(locals.regionOf(0xc).addressOf(0)), intBase + 4 * 34,
    '★ 索引 = 3*10 + 4 = 34（行 3、行宽 10、列 4）');
  assert.equal(readOperand(ctx, { type: 0xc, rawData: 0 }, 0).value, 34, '解引用拿到 `global:int[134]` 的值');
});

test('★ 一份数据：迁移后 Map 是空的，位模式**只在区域里**', () => {
  const { pools, space } = regionLocal();
  const bits = pools.write(9, 7, 0x1234); // type 9 = local int
  assert.equal(pools.pools.get('int').size, 0, '★ 写完之后 Map 必须为空（一份数据）');

  // ★ 经地址空间读到的**位模式**必须就是 `write` 的返回值（= `encInt(value, key)`）
  const reg = pools.regions.get('int');
  assert.ok(reg, 'int 池必须有区域');
  assert.equal(space.readU32(reg.addressOf(7)), bits, '经地址空间读到的位模式 == write 的返回值');
  assert.equal(bits, encInt(0x1234, KEY), '位模式就是 encInt(value, key)');

  // ★ 而且池 API 读回来的是**解码后**的原值
  assert.equal(pools.read(9, 7), 0x1234, '池 API 读回解码后的值');
  // 未写过的格子仍是 null（⛔ 不是 0）
  assert.equal(pools.read(9, 6), null, '未写过的格子 ⇒ null');
  assert.equal(space.readU32(reg.addressOf(6)), null, '（区域那一格也没写过 ⇒ null）');
});

test('★ 快照格式**逐字不变**：区域版与 Map 版的快照必须逐值相同', () => {
  const writes = /** @type {const} */ ([[9, 3, 11], [9, 4, 22], [12, 0, 0xdead], [14, 2, 7]]); // int / ptr / stringPtr
  const { pools: region } = regionLocal();
  const plain = new LocalPools(KEY); // 没给 space ⇒ 走 Map 路径（迁移前的形态）
  for (const [tag, idx, v] of writes) {
    region.write(tag, idx, v);
    plain.write(tag, idx, v);
  }
  assert.deepEqual(region.snapshot(), plain.snapshot(),
    '★ 迁移**不改外部形状**：同样写入 ⇒ 两种存储给出逐值相同的快照');
  // 而且没迁的池（float/string）在两边都是 Map 路径，值也一致
  region.write(10, 1, 1.5);
  plain.write(10, 1, 1.5);
  region.write(11, 1, '游ゴシック');
  plain.write(11, 1, '游ゴシック');
  assert.deepEqual(region.snapshot(), plain.snapshot(), '混着写（已迁 + 未迁）也必须一致');
});

test('★ 快照往返：从区域版快照恢复出来的实例，读回来的值相同（且数据落在区域里）', () => {
  const { pools } = regionLocal();
  pools.write(9, 5, 0xabcdef);
  pools.write(12, 1, 0x99); // ptr
  const snap = pools.snapshot();

  const space2 = new AddressSpace();
  const back = LocalPools.restore(snap, { space: space2 });
  assert.equal(back.read(9, 5), 0xabcdef, '恢复后读回同一个值');
  assert.equal(back.read(12, 1), 0x99);
  assert.equal(back.pools.get('int').size, 0, '★ 恢复时值被搬进区域 ⇒ Map 仍然是空的（一份数据）');
  assert.equal(back.snapshot().pools.length, snap.pools.length, '池的个数一致');
  assert.deepEqual(back.snapshot(), snap, '快照往返逐值相同');
});

test('★ 增长：初值 0、每次真涨都留痕；未迁移的池族（float/string）不进区域', () => {
  const { pools, space, grew } = regionLocal();
  const reg = pools.regions.get('int');
  assert.equal(reg.capacity, 0, '★ 容量初值 0（不编常数）');
  pools.write(9, 3, 1); // 会触发一次增长
  assert.ok(reg.capacity > 3, `写第 3 格 ⇒ 容量至少 4，实际 ${reg.capacity}`);
  assert.equal(grew.length, 1, '★ 每次真涨都要留痕（钩子回调一次）');
  assert.equal(grew[0].tag, 'local:int', '留痕里要能看出是哪个池');
  assert.equal(grew[0].from, 0, '从 0 涨起');

  // 同一个下标再写 ⇒ 不再增长（幂等）
  pools.write(9, 3, 2);
  assert.equal(grew.length, 1, '已经容得下 ⇒ 不涨、不留痕');

  // ★★ 现在 **6 个池都有区域** —— 连 string 也有。但两者的**职责不同**：
  //   * int/float 族：区域里**就是数据**（4 字节位模式）；
  //   * string 族：区域**只负责发地址**（`0xe` local string-ptr 要解引用到字符串元素），数据在 Map。
  assert.deepEqual([...pools.regions.keys()].sort(),
    ['float', 'floatPtr', 'int', 'ptr', 'string', 'stringPtr'],
    '每个池都进地址空间（都能取址）');
  // ★ string 的"只发地址"契约：写完字符串后，**格仍然没被写过**，而它的地址是有效的
  pools.write(11, 0, 'インライン');
  assert.equal(pools.read(11, 0), 'インライン', '数据在 Map 里');
  const strReg = pools.regions.get('string');
  assert.equal(strReg.elemBytes, 28, '★ 字符串区域的步长必须是 **28**（与引擎的元素步长一致，`base + 28*idx` 才对得上）');
  assert.equal(space.readU32(strReg.addressOf(0)), null, '★ 区域里**没有字符串数据**（格是空的）');
  assert.ok(strReg.capacity >= 1, '★ 但容量要长到那一格 —— 否则 `0xe` 解引用会响亮失败');
  // ★ float 族在区域里存的是 **float32 位模式**（不是数值）
  pools.write(10, 0, 2.5);
  assert.equal(space.readU32(pools.regions.get('float').addressOf(0)), bitsFromFloat(2.5), 'float 格 = float32 位模式');
  assert.equal(pools.read(10, 0), 2.5, '读回来是数值');
});

test('★ 全局池同形：`int`/`intRef` 迁到区域，且与 Map 版快照逐值相同', () => {
  const grew = [];
  const space = new AddressSpace({ onGrow: (e) => grew.push(e) });
  const g = new GlobalPools(KEY, undefined, { space });
  const plain = new GlobalPools(KEY);
  for (const [name, idx, v] of /** @type {const} */ ([['int', 9, 42], ['intRef', 1, 7]])) {
    g.write(name, idx, v);
    plain.write(name, idx, v);
  }
  assert.deepEqual(g.snapshot(), plain.snapshot(), '全局池：两种存储的快照逐值相同');
  // ★★ 全局池现在**六个都有区域**（与局部池同一套口径）：都能取址；但 string 族**数据仍在 Map**
  //   （区域只发地址 —— `0xe`/`0x8` 的基址 `&op2` 可能是全局字符串池的某一格）
  assert.deepEqual([...g.regions.keys()].sort(),
    ['float', 'floatRef', 'int', 'intRef', 'string', 'stringRef'],
    '每个全局池都进地址空间（都能取址）');
  assert.equal(g.regions.get('string').elemBytes, 28, '字符串族的元素步长是 **28**（与引擎一致）');
  // ★ 容量 0 的区域里那个地址**还不存在** ⇒ 直接读会响亮失败（这是正确行为，不是缺陷）
  assert.throws(() => space.readU32(g.regions.get('string').addressOf(0)), /不落在任何区域里/,
    '容量 0 ⇒ 地址还不存在 ⇒ 读它要抛（⛔ 不是安静地读别处）');
  g.write('string', 0, 'インライン');
  assert.ok(g.regions.get('string').capacity >= 1, '★ 写字符串要让它能**发地址**（容量随之长大）');
  assert.equal(space.readU32(g.regions.get('string').addressOf(0)), null, '★ 但区域里没有数据（格是空的）');
  for (const name of ['int', 'intRef']) {
    const reg = g.regions.get(name);
    assert.equal(space.readU32(reg.addressOf(name === 'int' ? 9 : 1)), encInt(name === 'int' ? 42 : 7, KEY),
      `${name}：位模式在区域里`);
  }
  assert.ok(grew.length >= 2, '两次写（两个池）各自触发增长并留痕');
  // ★ 没迁的只有 string 族
  const beforeG = space.regions.length;
  g.write('string', 0, 'インライン');
  assert.equal(g.read('string', 0), 'インライン');
  assert.equal(space.regions.length, beforeG, 'string 族仍然走 Map（不建区域）');
  // float 族：格内容是位模式
  g.write('float', 0, 1.25);
  assert.equal(g.read('float', 0), 1.25);
  assert.equal(space.readU32(g.regions.get('float').addressOf(0)), bitsFromFloat(1.25), 'float 格 = float32 位模式');
});
