/** @env pure @kind gate @suite toolchain @why 足迹三桶判定被写反（静态命中也算路径外 / 写者闭包空集不判路径外 / 有残差落不进桶） */
/**
 * tools/test/spec-residual-footprint.test.mjs —— `spec residuals --footprint` 的**判别力**守卫
 *
 * 守四件事（每条都有一条"改坏就红"的路径）：
 * ① **opcode 三桶**：动态命中 ⇒ 路径内；只有静态命中 ⇒ **判不了**（数据依赖分支）；都不命中 ⇒ 路径外。
 *    ★ 变异清单里有一条把"静态命中"写成路径外 —— 本文件的第 ① 组就是抓它的。
 * ② **位 / 格类**：动态 `gate` / `cell` 命中 ⇒ 路径内；没命中时由**写者闭包**决定
 *    （闭包里没有动态 opcode，含空集 ⇒ 路径外）；闭包数据缺席 ⇒ 判不了（"没取证"≠"路径外"）。
 * ③ **每条残差都落桶**：用真模型 + 空足迹跑一遍，`path + offpath + unknown == rows.length`，未分类 = 0。
 * ④ **触发者只从声明处取**：opcode 只取 `covers.ops` / 标题里的 `0x…`（且只认 opcode 域 0..0x3FF，
 *    标题里的类型标签 `0x8005` 不算 opcode）。
 *
 * ★ 只读仓库内文本 / 纯函数：不跑播放器、不读游戏安装（真实足迹的确定性由
 *   `tools/test/emulator-cxx-footprint.assets.test.mjs` 守）。
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';

import { loadSpec, residualInventory } from '../lib/spec.mjs';
import {
  FOOTPRINT_SCHEMA,
  SCOPES,
  classifyResidual,
  entryOpcodeTriggers,
  joinResiduals,
  parseFootprint,
  residualBitTriggers,
  residualCellTriggers,
} from '../lib/residual-footprint.mjs';

const { model } = loadSpec();

/** 合成足迹：只写需要的表，其它为空。`writers[]` = `{target, opcode, dir}`（两个写统计）。 */
function footprint({ ops = [], staticOps = [], gates = [], cells = [], writers = [] } = {}) {
  const lines = [`{"schema":${FOOTPRINT_SCHEMA},"kind":"meta","steps":0,"scripts":[]}`];
  for (const opcode of ops) lines.push(`{"schema":1,"kind":"op","opcode":${opcode},"count":1,"firstScript":0}`);
  for (const opcode of staticOps) lines.push(`{"schema":1,"kind":"static-op","opcode":${opcode},"count":1}`);
  for (const bit of gates) lines.push(`{"schema":1,"kind":"gate","bit":${bit},"count":1}`);
  for (const [address, read = 1, write = 0] of cells) {
    lines.push(`{"schema":1,"kind":"cell","addr":"0x${address.toString(16)}","read":${read},"write":${write}}`);
  }
  for (const writer of writers) {
    lines.push(JSON.stringify({ schema: 1, kind: 'writer', count: 1, ...writer }));
  }
  return parseFootprint(`${lines.join('\n')}\n`, { file: 'synthetic.jsonl' });
}

const row = (what, next = '') => ({ rule: 'R', id: 'RES-X', channel: 'vm', what, next });

test('① opcode 三桶：动态命中 / 仅静态命中 / 都不命中', () => {
  const entry = { id: 'R', title: '一条规则', covers: [{ ops: ['0x100', '0x200'] }] };
  // 动态命中 ⇒ 路径内
  assert.equal(classifyResidual(row('细节'), entry, footprint({ ops: [0x100] })).scope, 'path');
  // 从动态足迹删掉 ⇒ 仍静态命中 ⇒ **判不了**（★ 变异：把这条写成 offpath ⇒ 本断言红）
  assert.equal(
    classifyResidual(row('细节'), entry, footprint({ staticOps: [0x100] })).scope,
    'unknown',
    '静态足迹命中的 opcode 残差必须是"判不了"（数据依赖分支），不是"路径外"',
  );
  // 静态也没有 ⇒ 路径外
  assert.equal(classifyResidual(row('细节'), entry, footprint({})).scope, 'offpath');
  // 标题里的 opcode 也能当触发者（没有 covers.ops 时）
  const titled = { id: 'R', title: '某个动作：opcode 0x100' };
  assert.deepEqual(entryOpcodeTriggers(titled), [0x100]);
  assert.equal(classifyResidual(row('细节'), titled, footprint({ ops: [0x100] })).scope, 'path');
  // 类型标签 0x8005 不是 opcode（标题里出现也不能当触发者）
  assert.deepEqual(entryOpcodeTriggers({ title: '落点 0x8005' }), []);
});

test('② 位：动态 gate 命中 ⇒ 路径内；写者闭包决定"路径外"或"判不了"', () => {
  const r = row('服务链的第 27 位置起后……');
  assert.deepEqual(residualBitTriggers(r), [27]);
  // 动态服务过该位
  assert.equal(classifyResidual(r, { title: '无关' }, footprint({ gates: [27] })).scope, 'path');
  // 没服务过 + 闭包数据缺席 ⇒ 判不了
  assert.equal(classifyResidual(r, { title: '无关' }, footprint({})).scope, 'unknown');
  // 闭包为空集 ⇒ 路径外
  const emptyClosure = { writers: { bit: new Map([[27, []]]) } };
  assert.equal(classifyResidual(r, { title: '无关' }, footprint({}), emptyClosure).scope, 'offpath');
  // 闭包里的 opcode 不在动态足迹 ⇒ 路径外
  const coldClosure = { writers: { bit: new Map([[27, [0x2f6]]]) } };
  assert.equal(classifyResidual(r, { title: '无关' }, footprint({ ops: [0x100] }), coldClosure).scope, 'offpath');
  // 闭包里的 opcode 在动态足迹 ⇒ 路径内
  const warmClosure = { writers: { bit: new Map([[27, [0x2f6]]]) } };
  assert.equal(classifyResidual(r, { title: '无关' }, footprint({ ops: [0x2f6] }), warmClosure).scope, 'path');
});

test('② 写统计 → 写者闭包：writer 记录让位 / 格残差进路径内', () => {
  // 位：写者 opcode 在动态足迹里 ⇒ 路径内（走闭包，不是 gate）
  const bitRow = row('服务链的第 27 位置起后……');
  const bitFp = footprint({
    ops: [0x2f6],
    writers: [{ target: 'bit:27', opcode: 0x2f6, dir: 'set' }],
  });
  const bitVerdict = classifyResidual(bitRow, { title: '无关' }, bitFp);
  assert.equal(bitVerdict.scope, 'path');
  assert.equal(bitVerdict.by, 'gate-writer-closure');
  // 没有 writer 记录 ⇒ 仍判不了（"没统计到"不是"路径外"）
  assert.equal(classifyResidual(bitRow, { title: '无关' }, footprint({ ops: [0x2f6] })).scope, 'unknown');

  // 格：不在 `cell` 表里，但 writer 记录在 ⇒ 路径内
  const cellRow = row('该状态字在 +0xA5D8');
  const cellFp = footprint({
    ops: [0x1f9],
    writers: [{ target: 'cell:0xa5d8', opcode: 0x1f9, dir: 'write' }],
  });
  const cellVerdict = classifyResidual(cellRow, { title: '无关' }, cellFp);
  assert.equal(cellVerdict.scope, 'path');
  assert.equal(cellVerdict.by, 'cell-writer-closure');

  // ★ join 缺省消费足迹里的 writer 记录（不是只认调用方注入的 writers）
  const joined = joinResiduals(
    [{ rule: 'R', id: 'RES-X', channel: 'vm', cls: 'C', what: '第 27 位置起后……', next: '' }],
    [{ id: 'R', title: '无关' }],
    bitFp,
  );
  assert.equal(joined.rows[0].scope, 'path', 'joinResiduals 没有消费足迹里的 writer 记录');
});

test('② 格：动态 cell 命中 ⇒ 路径内；否则判不了（VM 池格之外没有插桩）', () => {
  const r = row('该状态字在 +0xA5D8', '读 0x4A066A 的写者');
  const cells = residualCellTriggers(r);
  assert.ok(cells.includes(0xa5d8), `应点出 +0xA5D8：${cells.map((x) => x.toString(16)).join(',')}`);
  const hit = classifyResidual(r, { title: '无关' }, footprint({ cells: [[0xa5d8]] }));
  assert.equal(hit.scope, 'path');
  assert.equal(classifyResidual(r, { title: '无关' }, footprint({})).scope, 'unknown');
  // 4 位以下的裸 0x 不当地址（多半是 opcode / 类型标签）
  assert.deepEqual(residualCellTriggers(row('opcode 0x192 的落点')), []);
});

test('③ 真模型：每条残差都落桶（未分类 = 0）', () => {
  const rows = residualInventory(model);
  const empty = footprint({});
  const joined = joinResiduals(rows, model.entries, empty);
  assert.ok(rows.length > 0);
  assert.equal(joined.rows.length, rows.length);
  for (const r of joined.rows) assert.ok(SCOPES.includes(r.scope), `${r.id} 没有落桶：${r.scope}`);
  assert.equal(joined.counts.path + joined.counts.offpath + joined.counts.unknown, rows.length);
  assert.equal(joined.rows.filter((r) => !SCOPES.includes(r.scope)).length, 0);
  // 空足迹下"路径内"必须是 0（没有任何触发者被执行过）——否则说明判定没看足迹
  assert.equal(joined.counts.path, 0, '空足迹不可能有"路径内"');
});

test('④ 足迹文件解析：坏行 / 坏 kind / 坏 schema 都必须响亮失败', () => {
  assert.throws(() => parseFootprint('{"schema":2,"kind":"meta"}\n'), /schema/);
  assert.throws(() => parseFootprint('{"schema":1,"kind":"nope"}\n'), /kind/);
  assert.throws(() => parseFootprint('not json\n'), /JSON/);
  assert.throws(() => parseFootprint('{"schema":1,"kind":"gate","bit":32,"count":1}\n'), /0\.\.31/);
  assert.throws(() => parseFootprint('{"schema":1,"kind":"writer","target":"bit:32","opcode":1,"dir":"set","count":1}\n'), /0\.\.31/);
  assert.throws(() => parseFootprint('{"schema":1,"kind":"writer","target":"bit:1","opcode":1,"dir":"write","count":1}\n'), /set \/ clear/);
  assert.throws(() => parseFootprint('{"schema":1,"kind":"writer","target":"cell:0x10","opcode":1,"dir":"read","count":1}\n'), /write/);
  assert.throws(() => parseFootprint('{"schema":1,"kind":"writer","target":"nope","opcode":1,"dir":"write","count":1}\n'), /target/);
  const fp = footprint({ ops: [1], gates: [2], cells: [[0x1000]], staticOps: [3] });
  assert.equal(fp.ops.size, 1);
  assert.equal(fp.gates.size, 1);
  assert.equal(fp.cells.size, 1);
  assert.equal(fp.staticOps.size, 1);
  assert.equal(fp.writers.bit.size, 0);
  assert.equal(fp.writers.cell.size, 0);
  const withWriters = footprint({ writers: [{ target: 'bit:27', opcode: 0x2f6, dir: 'set' }, { target: 'cell:0x10', opcode: 0x2f6, dir: 'write' }] });
  assert.deepEqual([...withWriters.writers.bit.get(27)], [0x2f6]);
  assert.deepEqual([...withWriters.writers.cell.get(0x10)], [0x2f6]);
});
