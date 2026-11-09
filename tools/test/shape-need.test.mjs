/** @env pure @kind contract @suite toolchain @why 按需补齐的提案把"已声明窗口里的偏移"又写进草稿（会与 G1/G2 打架、且看起来像补过了）· 把没观察宽度的偏移编成成员（假声明）· 或漏掉传播候选（白补一轮） */
/**
 * tools/test/shape-need.test.mjs —— `shape need` 的**纯逻辑**守卫（合成 types/carriers/inferred/sites）
 *
 * 判据（红了意味着什么）：
 *  ① 草稿里的成员**只**来自"观测到、且不在任何已声明窗口里"的偏移（含数组窗口、`count:null` 窗口）；
 *  ② 宽度没观察到的偏移**不进草稿**（否则会写出一条看起来成立的假成员）；
 *  ③ 候选载体**只**来自 `propagate` 推断、且**尚无载体**的函数，`witness` 用**调用点 EA**；
 *  ④ `@off` 过滤：已声明 ⇒ 明说"已声明"而不是给空草稿；未声明 ⇒ 只报那一格。
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';

import { memberAt, memberWindows, needProposal, renderNeed } from '../lib/shape-need.mjs';

const types = {
  T: { size: 0x20, members: [{ off: '0x0', width: 4 }, { off: '0x8', type: 'EL', count: 4 }] },
  EL: { size: 4, members: [{ off: '0x0', width: 4 }] },
};
const carriers = [
  { id: 'c1', kind: 'arg', fn: 'sub_A', reg: 'ecx', type: 'T*', witness: ['0x1000'] },
  { id: 'c2', kind: 'arg', fn: 'sub_C', reg: 'ecx', type: 'T*', witness: ['0x3001'] },
];
const inferred = new Map([
  ['sub_B', { type: 'T', viaEa: 0x2000, viaFrom: 'sub_A', hops: 1 }],
  ['sub_C', { type: 'T', viaEa: 0x3000, viaFrom: 'sub_A', hops: 1 }],   // 已有载体 ⇒ 不是候选
  ['sub_D', { type: 'OTHER', viaEa: 0x4000, viaFrom: 'sub_A', hops: 1 }],
]);
const sites = [
  { fn: 'sub_A', ea: 0x1001, off: 0x0, width: 4 },        // 已声明（标量）
  { fn: 'sub_A', ea: 0x1002, off: 0x4, width: 4 },        // 未声明（与下一处的宽度不一致）
  { fn: 'sub_B', ea: 0x2001, off: 0x4, width: 1 },
  { fn: 'sub_B', ea: 0x2002, off: 0x8, width: 4 },        // 数组首元素 ⇒ 已声明
  { fn: 'sub_B', ea: 0x2003, off: 0xc, width: 4 },        // 数组元素内 ⇒ 已声明
  { fn: 'sub_B', ea: 0x2005, off: 0x20, width: 4 },       // 越界 ⇒ 未声明
  { fn: 'sub_B', ea: 0x2006, off: 0x24, width: 'unknown' }, // 没宽度 ⇒ 不进草稿
];

test('★need-01 草稿只含"已观测且未声明"的偏移；宽度没观察的不进草稿', () => {
  const p = needProposal({ types, carriers, inferred, sites, type: 'T' });
  assert.deepEqual(p.declared.map((d) => d.fn), ['sub_A', 'sub_C']);
  assert.deepEqual(p.candidates.map((c) => c.fn), ['sub_B'], '已声明的 sub_C 与别的类型的 sub_D 都不许进候选');
  assert.equal(p.candidates[0].viaEa, 0x2000, '候选必须带调用点 EA（G16 的 witness）');
  assert.deepEqual(p.undeclared.map((e) => e.off), [0x4, 0x20, 0x24]);
  assert.deepEqual(p.noWidth.map((e) => e.off), [0x24]);
  const offs = (p.stub.members ?? []).map((m) => m.off);
  assert.deepEqual(offs, ['0x4', '0x20'], '已声明的 0x0 / 数组窗口内的 0x8·0xc 不许进草稿，0x24 没宽度也不许进');
  assert.equal(p.stub.members.find((m) => m.off === '0x4').width, 4, '宽度不一致 ⇒ 取最大并在 note 里如实列出');
  assert.match(p.stub.members.find((m) => m.off === '0x4').note ?? '', /宽度不一致/);
  assert.deepEqual(p.stub.carriers[0].witness, ['0x2000'], 'witness 必须是调用点 EA');
});

test('★need-02 `@off`：已声明 ⇒ 报"已声明"且不给草稿；未声明 ⇒ 只报那一格', () => {
  const win = memberWindows(types, 'T');
  assert.ok(memberAt(win, 0x8), '0x8 必须落在数组窗口里');
  const declared = needProposal({ types, carriers, inferred, sites, type: 'T', off: 0x8 });
  assert.ok(declared.declaredAt, '@0x8 落在已声明窗口 ⇒ 必须报 declaredAt');
  assert.equal(declared.stub.members, undefined, '已声明 ⇒ 不许给成员草稿（那会与 G1 打架）');
  const one = needProposal({ types, carriers, inferred, sites, type: 'T', off: 0x4 });
  assert.deepEqual(one.undeclared.map((e) => e.off), [0x4]);
  assert.equal(one.stub.members.length, 1);
});

test('★need-03 渲染：两份草稿（types / carriers 分开）+ 明说"不落盘"', () => {
  const p = needProposal({ types, carriers, inferred, sites, type: 'T' });
  const text = renderNeed(p);
  assert.match(text, /草稿①/);
  assert.match(text, /草稿②/);
  assert.match(text, /不落盘/);
  assert.match(text, /宽度是\*\*观测值\*\*/);
});
