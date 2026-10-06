/** @env pure @kind gate @why "每个可变字段必须归一类"这条口径塌了：新增状态没表态（快照会悄悄漏掉或悄悄多带） */
/**
 * tools/test/emulator-state-partition.test.mjs —— **状态分区**的守卫
 *
 * ## 它守的是什么
 * 快照/恢复的真正难点不是序列化，而是**"哪些量算状态"**。今天模型里只有池这一族，
 * 所以"忘了分类"还看不出代价；等 handler 一个接一个写下去（每个都可能顺手 `this.foo = 0`），
 * "有个量没回去"就会变成默认结果 —— 而这类错**不会报错**，只会让"恢复后重跑到第 N 帧"
 * 与"自然跑到第 N 帧"在某处静静分叉。
 *
 * ⇒ 本守卫把表态变成**机械可判**的：`apps/emulator/src/model/pools.ts` 的 `STATE_PARTITION`
 *   必须**恰好**覆盖每个类的**自有字段**，且：
 *
 * 1. 每个自有字段都被归类（新增字段忘了归类 ⇒ 红）；
 * 2. 表里不许有**已不存在**的字段（过期条目 ⇒ 红）；
 * 3. 类别落在闭集合 `engine / derived / diagnostic / host` 里；
 * 4. **`engine` 类的字段恰好就是快照的顶层键** —— 这条把"进不进快照"从散文变成等式；
 * 5. `diagnostic` 类的字段**不得**出现在快照顶层键里（`oob` 就是执行这条的样本）；
 * 6. 表里点名的类名必须真的在本模块导出（类改名/删掉而表没跟着改 ⇒ 红）。
 *
 * ★ 为什么用**反射**而不是"把字段名列一遍"：列一遍等于把同一件事写两遍，
 *   而新增字段时两遍里只会改一遍 —— 那正是本守卫要消灭的失败模式。
 *
 * 运行：`pnpm test`（纯反射，不需要语料）
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';

import * as poolsModule from '../../apps/emulator/src/model/pools.ts';
import { GLOBAL_POOL_NAMES, GlobalPools, LOCAL_POOLS, LocalPools, STATE_PARTITION } from '../../apps/emulator/src/model/pools.ts';

/** 类别的**闭集合**（与 `pools.ts` 的 `StateClass` 一致；改了这里就要改那里，反之亦然） */
const STATE_CLASSES = ['engine', 'derived', 'diagnostic', 'host'];

/** 被分区的类（**新加一个带状态的类就要加进这张表** —— 否则它的字段没人管） */
const PARTITIONED = [
  ['LocalPools', () => new LocalPools(0xdeadbeef)],
  ['GlobalPools', () => new GlobalPools(0xdeadbeef)],
];

test('★ 每个自有字段都必须归类，表里也不许有过期条目（两边逐一对齐）', () => {
  for (const [cls, make] of PARTITIONED) {
    const declared = STATE_PARTITION[cls];
    assert.ok(declared, `\`STATE_PARTITION\` 里没有 ${cls} —— 它的状态字段因此没人管`);
    const own = Object.keys(make());
    const unclassified = own.filter((p) => !(p in declared));
    assert.deepEqual(unclassified, [], `★ ${cls} 这些字段没归类（新增状态必须表态：engine/derived/diagnostic/host）：${unclassified.join(' / ')}`);
    const stale = Object.keys(declared).filter((p) => !own.includes(p));
    assert.deepEqual(stale, [], `${cls} 分区表里有已不存在的字段（过期条目）：${stale.join(' / ')}`);
  }
});

test('★ 类别必须在闭集合里，而且表里点名的类必须真的存在', () => {
  const bad = [];
  for (const [cls, decl] of Object.entries(STATE_PARTITION)) {
    for (const [field, kind] of Object.entries(decl)) {
      if (!STATE_CLASSES.includes(kind)) bad.push(`${cls}.${field} = ${JSON.stringify(kind)}`);
    }
  }
  assert.deepEqual(bad, [], `类别非法（闭集合 ${STATE_CLASSES.join(' / ')}）：${bad.join(' / ')}`);
  // 反射导出：点名了一个不存在的类 ⇒ 表已经过期
  const ghost = Object.keys(STATE_PARTITION).filter((cls) => typeof poolsModule[cls] !== 'function');
  assert.deepEqual(ghost, [], `分区表点名的类在本模块里不是类（改名/删掉了？）：${ghost.join(' / ')}`);
});

test('★ `engine` 类的字段**恰好**就是快照的顶层键（把"进不进快照"变成等式）', () => {
  for (const [cls, make] of PARTITIONED) {
    const decl = STATE_PARTITION[cls];
    const engineFields = Object.entries(decl).filter(([, k]) => k === 'engine').map(([f]) => f).sort();
    const snapKeys = Object.keys(make().snapshot()).sort();
    assert.deepEqual(
      snapKeys,
      engineFields,
      `★ ${cls} 的快照顶层键与"分类为 engine 的字段"不一致：多带 = 快照里混进了不该持久化的量，少带 = 有状态会被静默丢掉`,
    );
    assert.ok(engineFields.length > 0, `${cls} 应当至少有一个 engine 字段（否则快照是空的）`);
  }
});

test('★ `diagnostic` 类的字段不得进快照（`oob` 是样本：它随执行步数涨，但不是引擎态）', () => {
  for (const [cls, make] of PARTITIONED) {
    const decl = STATE_PARTITION[cls];
    const diag = Object.entries(decl).filter(([, k]) => k === 'diagnostic').map(([f]) => f);
    const snapKeys = Object.keys(make().snapshot());
    for (const f of diag) {
      assert.ok(!snapKeys.includes(f), `★ ${cls}.${f} 是诊断类，不许出现在快照里（它不是引擎态 ⇒ 会让等价判据红得没意义）`);
    }
  }
  // 样本核对：`oob` 确实被归到 diagnostic，而且它确实是**会增长**的那个数组
  assert.equal(STATE_PARTITION.LocalPools.oob, 'diagnostic', '`LocalPools.oob` 必须是 diagnostic');
  const lp = new LocalPools(1);
  assert.ok(Array.isArray(lp.oob), '`oob` 应当是那个留痕数组');
  lp.noteOOB(9, 999, 'read-uninitialized');
  assert.equal(lp.oob.length, 1, '`oob` 会随访问增长（这正是不该进快照的理由）');
});

test('★ 池定义本身也进核对：`LOCAL_POOLS` 的池名与 `GLOBAL_POOL_NAMES` 都必须是唯一的', () => {
  const local = LOCAL_POOLS.map((p) => p.name);
  assert.equal(new Set(local).size, local.length, 'local 池名不许重复（重复会让快照恢复时"缺池/重复"判不出来）');
  assert.equal(new Set(GLOBAL_POOL_NAMES).size, GLOBAL_POOL_NAMES.length, 'global 池名不许重复');
  // 分区表覆盖的类集合与"本模块里的池视图类"一致（防止新加一个视图类而没人给它分区）
  const exported = Object.keys(poolsModule).filter((k) => typeof poolsModule[k] === 'function' && /Pools$/.test(k)).sort();
  assert.deepEqual(Object.keys(STATE_PARTITION).sort(), exported, `★ 带 Pools 的导出类（${exported.join(' / ')}）必须都被分区表覆盖`);
});
