/** @env pure @kind contract @why 地址空间或随机源的口径塌了：指针解引用会读到错的格子、或"可复现"变成假象（都不报错） */
/**
 * tools/test/emulator-address-space.test.mjs —— **地址空间与随机源**的契约
 *
 * ## 它守的是什么（每一条都对应一类**不会报错**的坏结果）
 * 1. **地址是合成的、且与分配顺序一一对应**：真地址会让两次运行不可比（快照不可 diff）。
 * 2. **`base + elemBytes*index` 必须往返**：这正是引擎数组遍历的算术；错一格 ⇒ 读旁边的池。
 * 3. **解引用链**（`池[idx]` 装的是地址、值在 `[地址]`）—— 这是 `global-ptr` 那族操作数的**全部内容**；
 *    它错了，读出来的值"类型对、值错"，而且**不报错**。
 * 4. **三种失败必须分得开**：未映射 / 未对齐 / 未写过。前两种是 UB（响亮失败），
 *    第三种是"这里没东西"（`null`，**不是 0**）。
 * 5. **随机源：同种子同序列、快照能接着走** —— 这两条塌了，"两次跑一样"就只是声称。
 *
 * 运行：`pnpm test`（纯函数，不需要语料）
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';

import { AddressSpace, ORIGIN, Region } from '../../apps/emulator/src/model/address-space.ts';
import { SeededRandom } from '../../apps/emulator/src/host/random.ts';
import { HANDLERS } from '../../apps/emulator/src/vm/ops.ts';

const u32buf = (v) => {
  const b = new Uint8Array(4);
  new DataView(b.buffer).setUint32(0, v >>> 0, true);
  return b;
};

test('★ 地址是合成的：同一分配序列 ⇒ 同一批基址；顺序变了基址就变（不依赖宿主）', () => {
  const mk = (order) => {
    const sp = new AddressSpace();
    for (const tag of order) sp.alloc({ tag, elemBytes: 4, capacity: 8 });
    return sp.regions.map((r) => [r.tag, r.base]);
  };
  const a = mk(['local-int', 'local-float', 'local-ptr']);
  const b = mk(['local-int', 'local-float', 'local-ptr']);
  assert.deepEqual(a, b, '同顺序必须给同一批基址');
  assert.equal(a[0][1], ORIGIN, '第一个区域落在 ORIGIN（8 对齐，ORIGIN 本身是 8 的倍数）');
  assert.ok(a[1][1] > a[0][1] && a[2][1] > a[1][1], '基址按分配顺序递增');
  assert.notDeepEqual(mk(['local-float', 'local-int', 'local-ptr']), a, '换了顺序，tag→base 必须跟着变');
});

test('★ `base + elemBytes*index` 往返：addressOf → resolve 必须回到同一格', () => {
  const sp = new AddressSpace();
  const r = sp.alloc({ tag: 'local-int@3', elemBytes: 4, capacity: 8 });
  for (let i = 0; i < 8; i += 1) {
    const hit = sp.resolve(r.addressOf(i));
    assert.ok(hit, `第 ${i} 格必须可解析`);
    assert.equal(hit.region, r);
    assert.equal(hit.offset, i * 4);
  }
  // ★ 这正是引擎的数组遍历算术：`base + 4*i`（取证：create-mesh 的 `.text:0043223E mov eax,[ecx]` + `add ecx,4`）
  assert.equal(r.addressOf(3) - r.addressOf(0), 12);
  // 区域**末地址之后** ⇒ 不是本区域（引擎会安静地读到相邻内存 —— 那是 UB，本层不模仿）
  assert.equal(sp.resolve(r.addressOf(8)), null);
  assert.equal(sp.resolve(r.base - 1), null);
});

test('★ 解引用链（`global-ptr` 的形态）：池格里装地址，值在 `[地址]` 里', () => {
  const sp = new AddressSpace();
  const target = sp.alloc({ tag: 'global-int', elemBytes: 4, capacity: 4 });
  const ptrPool = sp.alloc({ tag: 'global-ptr', elemBytes: 4, capacity: 4 });

  // 目标池第 2 格 = 0xDEADBEEF
  const cell = target.addressOf(2);
  sp.writeU32(cell, 0xdeadbeef);
  // 指针池第 0 格 = 「指向目标池第 2 格的地址」
  const ptrCell = ptrPool.addressOf(0);
  sp.writeU32(ptrCell, cell);

  // 读：取指针 → 解引用 → 值（这就是 sub_41BF50 case 6 的三条指令）
  const addr = sp.readU32(ptrCell);
  assert.equal(addr, cell, '指针格里装的就是地址');
  assert.equal(sp.readU32(addr), 0xdeadbeef, '解引用之后拿到的才是值');

  // ★ 反证：把"指针格的值"当值用（即**不**解引用）会得到地址本身 —— 类型对、值错
  assert.notEqual(addr, 0xdeadbeef);
});

test('★ 三种失败分得开：未映射 / 未对齐 ⇒ 抛（带地址）；未写过 ⇒ null（不是 0）', () => {
  const sp = new AddressSpace();
  const r = sp.alloc({ tag: 'local-int', elemBytes: 4, capacity: 4 });

  assert.throws(() => sp.readU32(0x0), /不落在任何区域里/, '未映射必须抛');
  assert.throws(() => sp.readU32(r.base + 2), /未对齐/, '未对齐必须抛，且与未映射可区分');
  assert.equal(sp.readU32(r.addressOf(1)), null, '★ 未写过 ⇒ null（"这里没东西" ≠ "这里是 0"）');
  assert.equal(r.has(4), false);

  sp.writeU32(r.addressOf(1), 0);
  assert.equal(sp.readU32(r.addressOf(1)), 0, '写过 0 之后读出来才是 0');
  assert.equal(r.has(4), true);

  const kinds = new Map(sp.diagnosticsSorted().map((d) => [d.kind, d.count]));
  assert.equal(kinds.get('unmapped'), 1);
  assert.equal(kinds.get('misaligned'), 1);
  assert.equal(kinds.get('unwritten'), 1);
});

test('★ 非 4 字节的格子（字符串 SSO = 28）也走同一条路，且不能被 writeU32 误用', () => {
  const sp = new AddressSpace();
  const s = sp.alloc({ tag: 'local-string', elemBytes: 28, capacity: 4 });
  const cell = s.addressOf(2);
  assert.equal(s.offsetOf(cell), 56);
  const bytes = new Uint8Array(28);
  bytes[0] = 0xaa;
  bytes[27] = 0xbb;
  sp.writeCell(cell, bytes);
  assert.deepEqual([...sp.readCell(cell)], [...bytes], '整格进出必须逐字节相同');
  assert.throws(() => sp.writeU32(cell, 1), /格子宽 28/);
  assert.throws(() => sp.writeCell(cell, new Uint8Array(4)), /格子宽 28/);
});

test('★ `readBytes` 可跨格、但跨区域 ⇒ 抛（那说明这段数据不在同一个块里）', () => {
  const sp = new AddressSpace();
  const r = sp.alloc({ tag: 'blob', elemBytes: 4, capacity: 4 });
  for (let i = 0; i < 4; i += 1) sp.writeU32(r.addressOf(i), i + 1);
  const got = sp.readBytes(r.base, 16);
  assert.equal(got.length, 16);
  assert.deepEqual([...got], [1, 0, 0, 0, 2, 0, 0, 0, 3, 0, 0, 0, 4, 0, 0, 0], '小端、逐字节');
  assert.throws(() => sp.readBytes(r.addressOf(3), 8), /越出区域/);
});

test('★ 快照：同状态 ⇒ 逐值相同；恢复后**不许重发**已用过的地址', () => {
  const sp = new AddressSpace();
  const r = sp.alloc({ tag: 'local-int', elemBytes: 4, capacity: 4 });
  sp.writeU32(r.addressOf(3), 0x12345678);
  sp.writeU32(r.addressOf(0), 0);

  const snap = sp.snapshot();
  assert.deepEqual(Object.keys(snap).sort(), ['baseCursor', 'regions'], '快照顶层键 = engine 类字段');
  const again = AddressSpace.restore(snap).snapshot();
  assert.deepEqual(again, snap, '往返必须逐值相同（含"哪几格在场"）');

  // 恢复出来的空间继续分配 ⇒ 基址必须落在旧区域之后
  const sp2 = AddressSpace.restore(snap);
  const r2 = sp2.alloc({ tag: 'next', elemBytes: 4, capacity: 4 });
  assert.ok(r2.base >= r.base + r.byteLength, `新区域的基址 ${r2.base} 不许覆盖已用过的地址`);

  // 恶意快照：baseCursor 落在已有区域里面 ⇒ 必须抛（否则会重发地址 = 两块数据别名）
  const bad = { ...snap, baseCursor: r.base };
  assert.throws(() => AddressSpace.restore(bad), /会重发已用过的地址/);
});

test('★ 区域身份不许含糊：`regionByTag` 遇同名多个必须抛（取第一个会掩盖命名错误）', () => {
  const sp = new AddressSpace();
  sp.alloc({ tag: 'dup', elemBytes: 4, capacity: 1 });
  assert.equal(sp.regionByTag('dup').tag, 'dup');
  sp.alloc({ tag: 'dup', elemBytes: 4, capacity: 1 });
  assert.throws(() => sp.regionByTag('dup'), /tag 不唯一/);
  assert.throws(() => sp.regionByTag('nope'), /没有这个 tag/);
  // capability 必须是正整数（本层不编默认值）
  assert.throws(() => new Region('x', 0, 4, 0), /capacity 必须是正整数/);
  assert.throws(() => new Region('x', 0, 0, 1), /elemBytes 必须是正整数/);
});

test('★ 随机源：同种子同序列（逐值钉死）；不同种子不同；取数次数可数', () => {
  const a = new SeededRandom(1);
  assert.deepEqual([a.nextU32(), a.nextU32(), a.nextU32()], [2693262067, 11749833, 2265367787],
    '同种子必须给同一串（这条就是"可复现"的全部内容）');
  assert.equal(a.draws(), 3, '取数次数是产物的一部分');

  const b = new SeededRandom(1);
  const c = new SeededRandom(2);
  assert.equal(b.nextU32(), 2693262067, '另一个实例、同种子 ⇒ 同一个数');
  assert.notEqual(c.nextU32(), 2693262067, '换种子必须换序列');
});

test('★ 随机源快照：恢复之后**接着走**（不是从头来）—— 这是"存档后重跑"的前提', () => {
  const a = new SeededRandom(1);
  a.nextU32(); a.nextU32(); a.nextU32();
  const snap = a.snapshot();
  assert.deepEqual(Object.keys(snap), ['state'], '快照顶层键 = engine 类字段（count 是诊断，不进快照）');
  const restored = SeededRandom.restore(snap);
  assert.equal(restored.nextU32(), a.nextU32(), '恢复后的下一个数必须与自然跑的下一个数相同');
  assert.equal(restored.draws(), 1, '恢复出来的实例只数"这一趟"的取数');
});

test('★ `0x60 random` 已进 handler 表（它不再是"故意不实现"：语料里 22 处）', () => {
  assert.equal(typeof HANDLERS[0x60], 'function');
  // 没有随机源时它必须**响亮失败**（不许退回 Math.random —— 那会让"同种子同日志"变成假象）
  const ctx = {
    machine: { instance: { random: null }, note() {}, effect() {} },
    frame: { locals: { read: () => 7, write() {} } },
    script: {},
    ins: { opcode: 0x60, name: 'random', argc: 2, index: 0, args: [{ type: 0, rawData: 10 }, { type: 0, rawData: 100 }] },
  };
  assert.throws(() => HANDLERS[0x60](ctx), /没有随机源/);
});
