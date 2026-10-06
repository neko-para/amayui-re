/** @env pure @kind contract @why 引擎态快照不再是"同样状态给同样字节"：顺序规范化/稀疏保真/key 必带 里有一条塌了 */
/**
 * tools/test/emulator-snapshot.test.mjs —— **池快照的接缝**（`apps/emulator/src/model/pools.ts`）
 *
 * ## 它守的是什么（不是"测快照功能"——功能还没到）
 * 今天还没有执行核心，所以**没有**文件格式、没有版本号（故意的，见 `pools.ts` 头注）。
 * 本支守的是**三条接缝口径**，它们现在塌了只是几行的事，等执行核心落地再塌就得改语义：
 *
 * 1. **同样状态 ⇒ 逐字节相同的纯数据**。反例是 `Map` 的迭代顺序 = **插入顺序**
 *    （本文件用**哨兵**先证明两条路径的原始顺序确实不同，再要求快照相同 —— 否则这条用例可能恒真）。
 * 2. **稀疏保真**：只记在场的槽。"这里没东西"与"值是 0"是两件事（int 族初值是 `enc_zero`，非 0）。
 * 3. **必带 `key`**：int 族槽存的是编码位模式，没有 key 就不可解释；`value-codec.mts` 不给默认值，快照也不许简写。
 * 4. **不可信输入要响亮失败**：未知池名 / 下标重复 / 缺池 / 值类型不符 —— 全抛，不静默跳过。
 * 5. **诊断（`oob`）不进快照**：它是"引擎不做越界检查"这件事的留痕，不是引擎态。
 *
 * ★ 为什么"缺池"也必须红：`read()` 内部是 `this.pools.get(name).get(idx)`，
 *   缺一个池就是运行期崩 —— 宁可在**恢复这一刻**就报出来。
 *
 * 运行：`pnpm test`（纯函数，不需要语料）
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';

import { GLOBAL_POOL_NAMES, GlobalPools, LOCAL_POOLS, LocalPools } from '../../apps/emulator/src/model/pools.ts';

const KEY = 0xdeadbeef;
/** 本类认得 6 个 local 池 —— 造"缺池"用例时用 */
const ALL_LOCAL = LOCAL_POOLS.map((p) => p.name);
const bytes = (v) => JSON.stringify(v);

/** 造一份**合法**的全池快照（6 个池都在场），便于逐项破坏 */
const legalLocal = (over = {}) => ({
  key: KEY,
  pools: ALL_LOCAL.map((n) => [n, []]),
  ...over,
});

test('★ 往返等价：`restore(snapshot(x))` 与 `x` 同值，且再快照**逐字节相同**', () => {
  const lp = new LocalPools(KEY);
  lp.write(9, 3, 0x12345678);
  lp.write(9, 40, -7);
  lp.write(10, 5, 1.5);
  lp.write(11, 2, 'あまゆい');
  lp.write(14, 1, 99);
  const gp = new GlobalPools(KEY);
  gp.write('int', 7, 42);
  gp.write('float', 2, 2.25);
  gp.write('string', 0, 'セリフ');

  // ── LocalPools：按 operand type tag 逐槽对读
  const lp2 = LocalPools.restore(lp.snapshot());
  assert.equal(bytes(lp2.snapshot()), bytes(lp.snapshot()), 'LocalPools：再快照必须逐字节相同');
  assert.equal(lp2.key, lp.key, 'LocalPools：key 必须一起回来（它是 int 族可解释的前提）');
  for (const def of LOCAL_POOLS) {
    for (const idx of [0, 1, 2, 3, 5, 7, 40]) {
      assert.equal(lp2.read(def.typeTag, idx), lp.read(def.typeTag, idx), `local.${def.name}[${idx}] 读值必须一致`);
    }
  }

  // ── GlobalPools：按池名逐槽对读
  const gp2 = GlobalPools.restore(gp.snapshot());
  assert.equal(bytes(gp2.snapshot()), bytes(gp.snapshot()), 'GlobalPools：再快照必须逐字节相同');
  assert.equal(gp2.key, gp.key, 'GlobalPools：key 必须一起回来');
  for (const name of GLOBAL_POOL_NAMES) {
    for (const idx of [0, 1, 2, 5, 7]) {
      assert.equal(gp2.read(name, idx), gp.read(name, idx), `global.${name}[${idx}] 读值必须一致`);
    }
  }

  // 位模式（不是解码值）也一起过去：快照里 int 槽的那个数必须**不是**原值
  const restoredBits = LocalPools.restore(lp.snapshot()).pools.get('int').get(3);
  assert.equal(restoredBits, lp.pools.get('int').get(3), '快照存的是**位模式**，恢复后仍是同一份位模式');
  assert.notEqual(restoredBits, 0x12345678, '★ 位模式不该等于原值（等于就说明快照偷偷存了解码值）');
});

test('★ 顺序无关：两条路径到达同一状态 ⇒ 快照**逐字节相同**（先证明原始顺序确实不同）', () => {
  // 路径 A：先写槽 5 再写槽 3
  const a = new LocalPools(KEY);
  a.write(9, 5, 11);
  a.write(9, 3, 22);
  a.write(9, 8, 33);
  // 路径 B：同样的三个槽，倒着写
  const b = new LocalPools(KEY);
  b.write(9, 8, 33);
  b.write(9, 3, 22);
  b.write(9, 5, 11);

  // ★ 哨兵：先证明"原始 Map 的迭代顺序**确实**不同" —— 否则下面那条断言可能恒真
  assert.notDeepEqual([...a.pools.get('int').keys()], [...b.pools.get('int').keys()],
    '哨兵失效：两条路径的原始插入顺序本应不同，这条用例才谈得上"规范化"');

  assert.equal(bytes(a.snapshot()), bytes(b.snapshot()), '★ 同样状态必须给出逐字节相同的快照（键升序）');
  // 而且快照里的下标确实是升序的
  assert.deepEqual(a.snapshot().pools.find(([n]) => n === 'int')[1].map(([i]) => i), [3, 5, 8], '快照里的下标必须升序');
  // 池名也升序（`Map` 的迭代顺序是插入顺序，不是声明的顺序）
  const names = a.snapshot().pools.map(([n]) => n);
  assert.deepEqual(names, [...names].sort(), '快照里的池名必须升序');
});

test('★ 稀疏保真：缺席 ≠ 值为 0（int 族的初值是 `enc_zero`，那**不是** 0）', () => {
  const lp = new LocalPools(KEY);
  lp.write(9, 0, 0); // 明确写一个 0 进去
  const snap = lp.snapshot();
  const int = snap.pools.find(([n]) => n === 'int')[1];
  assert.deepEqual(int.map(([i]) => i), [0], '只写了槽 0 ⇒ 快照里 int 池只有槽 0');
  assert.notEqual(int[0][1], 0, '★ 槽 0 的位模式不是 0（0 经 ENC 之后是非 0 的位模式）');
  // 其余 5 个池都在场但都是空的（快照是"全池覆盖 + 稀疏槽"）
  assert.deepEqual(snap.pools.map(([n]) => n), [...ALL_LOCAL].sort(), '6 个池都必须出现在快照里（缺池在恢复时是运行期崩）');
  assert.equal(snap.pools.reduce((s, [, pairs]) => s + pairs.length, 0), 1, '整份快照里只有一个槽在场');
  // 恢复之后"缺席"仍然缺席（不许被补成 0）
  const back = LocalPools.restore(snap);
  assert.equal(back.read(9, 0), 0);
  assert.equal(back.read(9, 1), null, '★ 未初始化的槽恢复后仍是 null（不许补 0）');
});

test('★ 必带 `key`：没有 key 的快照一律拒绝（`value-codec.mts` 不给默认值，快照也不许简写）', () => {
  assert.throws(() => LocalPools.restore({ pools: [] }), /缺 key/, '省略 key ⇒ 抛（静默用 0 = 把"我不知道 key"伪装成"key 是 0"）');
  assert.throws(() => LocalPools.restore({ key: 1.5, pools: [] }), /缺 key/, 'key 必须是整数');
  assert.throws(() => GlobalPools.restore({ key: 'deadbeef', pools: [] }), /缺 key/, 'key 必须是数（十六进制串不算）');
  // 而 key = 0 是**合法**的（引擎那份 key 是运行期算出来的，0 没有任何特殊地位）
  const zero = LocalPools.restore(legalLocal({ key: 0 }));
  assert.equal(zero.key, 0, 'key = 0 必须被接受（它不是"缺失"）');
});

test('★ 不可信输入要**响亮失败**：未知池名 / 下标重复 / 缺池 / 值类型不符', () => {
  assert.throws(() => LocalPools.restore({ key: KEY, pools: [['ptr2', []]] }), /不在本类里/, '未知池名');
  assert.throws(() => GlobalPools.restore({ key: KEY, pools: [['ptr', []]] }), /不在本类里/, '`ptr` 是 local 池，不是 global 的');
  assert.throws(() => LocalPools.restore(legalLocal({ pools: [...ALL_LOCAL.map((n) => [n, []]), ['int', []]] })), /重复/, '池名重复');
  assert.throws(() => LocalPools.restore({ key: KEY, pools: [['int', []]] }), /缺池/, '缺池（`read()` 会因此运行期崩）');
  // ★ 而且这条是**构造器的不变式**：手工塞一份半份池，也在构造这一刻就红（不必等到 read）
  assert.throws(() => new LocalPools(KEY, new Map([['int', new Map()]])), /缺池/, '手工塞半份池 ⇒ 构造器就红');
  assert.throws(() => new GlobalPools(KEY, new Map()), /缺池/, '空池表 ⇒ 构造器就红（global 侧同口径）');
  assert.throws(() => LocalPools.restore(legalLocal({ pools: ALL_LOCAL.map((n) => [n, n === 'int' ? [[3, 1], [3, 2]] : []]) })), /下标重复/, '同一槽两行 ⇒ 后写覆盖先写，静默丢一格');
  assert.throws(() => LocalPools.restore(legalLocal({ pools: ALL_LOCAL.map((n) => [n, n === 'string' ? [[0, 7]] : []]) })), /值类型不符/, 'string 池装了个数');
  assert.throws(() => LocalPools.restore(legalLocal({ pools: ALL_LOCAL.map((n) => [n, n === 'int' ? [[-1, 7]] : []]) })), /下标非法/, '负下标');
  assert.throws(() => LocalPools.restore(legalLocal({ pools: ALL_LOCAL.map((n) => [n, n === 'int' ? [[0, 'x']] : []]) })), /值类型不符/, 'int 池装了个串');
  assert.throws(() => LocalPools.restore({ key: KEY, pools: 'nope' }), /不是数组/, '`pools` 不是数组');
});

test('★ 诊断不进快照：`oob` 变化**不改变**快照字节（它是留痕，不是引擎态）', () => {
  const lp = new LocalPools(KEY);
  lp.write(9, 1, 5);
  const before = bytes(lp.snapshot());
  const beforeKeys = Object.keys(lp.snapshot()).sort();

  lp.noteOOB(9, 999, 'read-uninitialized');
  lp.noteOOB(9, 999, 'read-uninitialized');
  lp.noteOOB(11, 12345, 'read-oob');

  assert.equal(bytes(lp.snapshot()), before, '★ 记了三次越界访问，快照字节必须**一个都没变**');
  assert.deepEqual(Object.keys(lp.snapshot()).sort(), beforeKeys, '快照的顶层键集合也不该变');
  assert.ok(!beforeKeys.includes('oob'), '快照里不许有 `oob` 这个键（诊断不是引擎态）');
  // 但它必须**可被看见**：摘要里有总数、分类计数、头几条（`kind` 升序 ⇒ 摘要也是确定性的）
  const sum = lp.oobSummary(2);
  assert.equal(sum.total, 3);
  assert.deepEqual(sum.byKind, [['read-oob', 1], ['read-uninitialized', 2]], 'kind 升序统计');
  assert.equal(sum.first.length, 2, '头 2 条原始记录（便于定位，但不随快照传播）');
  // 恢复出来的实例 oob 是空的 —— 快照不含它，也就不会"假装"恢复过那些访问
  assert.equal(LocalPools.restore(lp.snapshot()).oob.length, 0, '诊断不随快照传播');
});

test('★ 快照是**纯数据**：JSON 往返后仍可恢复；而实例本身**不是**快照', () => {
  const lp = new LocalPools(KEY);
  lp.write(9, 3, 0x12345678);
  lp.write(11, 1, 'テキスト');
  const snap = lp.snapshot();

  // ① 快照必须能被 JSON 往返（否则进不了 git、也做不了"两份快照 diff"）
  const viaJson = JSON.parse(JSON.stringify(snap));
  assert.deepEqual(viaJson, snap, 'JSON 往返后逐字段相同');
  assert.equal(bytes(LocalPools.restore(viaJson).snapshot()), bytes(snap), 'JSON 往返后恢复出来的状态相同');
  // ② `structuredClone` 对**快照**成立（纯数据）
  assert.deepEqual(structuredClone(snap), snap, '快照可 `structuredClone`');
  // ③ ★ 但 `structuredClone(实例)` **不是**快照 —— 原型丢了，方法没了
  //    （实测：`structuredClone(new A())` 得到纯对象、`typeof c.f === 'undefined'`）
  //    ⇒ "存快照"只能走 `snapshot()`，别指望克隆实例。
  const cloned = structuredClone(lp);
  assert.equal(typeof cloned.read, 'undefined', '★ 克隆实例会丢原型 ⇒ 它只能是数据，不能当快照用');
  assert.equal(typeof cloned.snapshot, 'undefined', '同上');
});

test('★ `GlobalPools.read` 对未知池名**抛**（与 `write` 对称）—— 静默返回 null 会盖住"池名写错"', () => {
  const gp = new GlobalPools(KEY);
  gp.write('int', 4, 8);
  assert.equal(gp.read('int', 4), 8, '已知池名照常读');
  assert.equal(gp.read('int', 5), null, '未初始化的槽仍是 null（不是抛）');
  assert.throws(() => gp.read('int2', 0), /未知的全局池/, '★ 池名写错必须响亮失败，不许冒充"这个槽是空的"');
  assert.throws(() => gp.write('int2', 0, 1), /未知的全局池/, 'write 一直是抛的，read 现在与它对称');
});
