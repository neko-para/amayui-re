/** @env pure @kind contract @why 0x2f6 的组号映射 / 清零范围 / 后端门 / "是否还有语音在播"的跳过判据写错都不会报错，只会静默留下另一组的状态或算反标志 */
/**
 * tools/test/emulator-new-voice.test.mjs —— Voice 族（`60-ops-voice.md` 的 CORE-OPS-VOICE-STOP）守卫
 *
 * ★ 判据为什么是这些（每条都能被"写错"弄红）：
 *   - **清零范围**：8 格（语音对象内 4 格 + 引擎级 4 格）漏两格、或清错组，都不会报错 ⇒ 先把三组都填成非 0，
 *     再断言"目标组全 0、别的组一字不动"；
 *   - **后端门**：后端不存在时引擎**不清**语音对象里那 4 格、也**不问**播放 —— 把两步合并成"一起清"会多写 4 格；
 *   - **释放只在该通道有对象时发生**：引擎里那条调用被"槽非空"包着 ⇒ 没有对象**不许**留下释放请求；
 *   - **先停再清最后重算**：被停的那组的跳过期（低 2 位 = 3）在重算前已被清 0 ⇒ 它的通道**必须**被重新询问；
 *   - **组号 0..2**：引擎不检查上界（越界踩相邻数组 = 未定义）⇒ 本层对越界响亮失败，不静默收下。
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';

import { loadOpcodeTable } from '../../apps/emulator-new/src/platform/node/spec.ts';
import { Machine } from '../../apps/emulator-new/src/core/machine.ts';

const table = await loadOpcodeTable();
const KEY = 0x1234abcd;

/** 把 [opcode, [[type, raw], ...]] 拼成一段 body（不含头） */
function script(instrs) {
  const words = [];
  for (const [opcode, ops] of instrs) {
    words.push(opcode);
    for (const [type, raw] of ops) words.push(type, raw);
  }
  const buf = Buffer.alloc(words.length * 4);
  words.forEach((w, i) => buf.writeUInt32LE(w >>> 0, i * 4));
  return buf;
}

function machineFor(instrs) {
  return new Machine({ body: script(instrs), table, key: KEY });
}

/** 0x2f6，立即数操作数 = 组号 */
function stopGroup(group) {
  return [0x2f6, [[0, group]]];
}

/** 给一组预置 8 个非 0 字段（模拟"这组之前被用过"） */
function seedGroup(m, group, value) {
  m.voice.groups.set(group, {
    subsystem: [value, value + 1, value + 2, value + 3],
    engine: [value + 4, value + 5, value + 6, value + 7],
  });
}

test('★ 0x2f6 按组号清 8 格：目标组全 0，别的组一字不动', async () => {
  const m = machineFor([stopGroup(1)]);
  seedGroup(m, 0, 10);
  seedGroup(m, 1, 20);
  seedGroup(m, 2, 30);

  await m.run();

  assert.deepEqual(m.voice.groups.get(1), { subsystem: [0, 0, 0, 0], engine: [0, 0, 0, 0] },
    '目标组的 4 个语音对象字段 + 4 个引擎级字段都必须为 0');
  assert.deepEqual(m.voice.groups.get(0), {
    subsystem: [10, 11, 12, 13], engine: [14, 15, 16, 17],
  }, '组 0 不许被动到（组号映射错了这条会红）');
  assert.deepEqual(m.voice.groups.get(2), {
    subsystem: [30, 31, 32, 33], engine: [34, 35, 36, 37],
  }, '组 2 不许被动到（"清所有组"式的实现这条会红）');
});

test('★ 0x2f6 后端不存在：语音对象那 4 格不许清、不许发查询，标志直接 0（引擎级 4 格仍清）', async () => {
  const m = machineFor([stopGroup(1)]);
  m.sound.backend = 'absent';
  seedGroup(m, 1, 20);
  await m.run();

  assert.deepEqual(m.voice.groups.get(1).subsystem, [20, 21, 22, 23],
    '没有后端 ⇒ `sub_4BB9F0` 里那 4 条清零被整段跳过（两步合并成"一起清"这条会红）');
  assert.deepEqual(m.voice.groups.get(1).engine, [0, 0, 0, 0], '引擎级 4 格无条件清');
  assert.deepEqual(m.effects.filter((e) => e.kind === 'audio.playing.query'), [],
    '没有后端 ⇒ 1 次播放查询都不发');
  assert.equal(m.scalars.get('voice.stillPlaying'), 0);
});

test('★ 0x2f6 释放「组号 + 固定偏移」那条通道；没有对象就不许留下释放请求', async () => {
  const withObject = machineFor([stopGroup(1)]);
  withObject.sound.buffers.set(13, { playing: 1 });
  await withObject.run();
  assert.deepEqual(withObject.effects.filter((e) => e.kind === 'audio.release.request'),
    [{ kind: 'audio.release.request', detail: { channel: 13 } }],
    '组 1 的通道 = 13（偏移写错会指向 12 / 14）');
  assert.equal(withObject.sound.buffers.has(13), false, '释放之后这个通道不许还挂着对象');

  const empty = machineFor([stopGroup(1)]);
  await empty.run();
  assert.deepEqual(empty.effects.filter((e) => e.kind === 'audio.release.request'), [],
    '引擎里"释放"被"槽非空"包着 ⇒ 没有对象时**不许**发请求（发了就是编造外部调用）');
});

test('★ 0x2f6 重算标志：先停再清最后重算（被停的组也必须被重新询问）', async () => {
  const m = machineFor([stopGroup(0)]);
  // 组 0 的"跳过期"旧值是 3；先清后判的实现会跳过通道 12
  m.voice.groups.set(0, { subsystem: [0, 0, 0, 3], engine: [0, 0, 0, 0] });
  await m.run();
  assert.deepEqual(m.effects.filter((e) => e.kind === 'audio.playing.query').map((e) => e.detail.channel),
    [12, 13, 14], '清零发生在重算之前 ⇒ 组 0 的通道 12 不能因为旧值 3 被跳过');
});

test('★ 0x2f6 重算标志：有通道在播', async () => {
  const busy = machineFor([stopGroup(0)]);
  busy.sound.buffers.set(13, { playing: 1 });
  await busy.run();
  assert.equal(busy.scalars.get('voice.stillPlaying'), 1, '通道 13 在播 ⇒ 标志 1');
  assert.deepEqual(busy.effects.filter((e) => e.kind === 'audio.playing.query').map((e) => e.detail.channel),
    [12, 13], '问到"在播"就停（13 之后的通道不许再问）');

  const silent = machineFor([stopGroup(0)]);
  await silent.run();
  assert.equal(silent.scalars.get('voice.stillPlaying'), 0, '没有任何通道在播 ⇒ 标志 0');

  const atTwelve = machineFor([stopGroup(2)]);
  atTwelve.sound.buffers.set(12, { playing: 1 });
  await atTwelve.run();
  assert.equal(atTwelve.scalars.get('voice.stillPlaying'), 1,
    '停组 2（通道 14）时，通道 12 在播也必须算进标志（偏移写成 13 的话这条会红）');
});

test('★ 0x2f6 组号只认 0..2：越界响亮失败（引擎不检查上界 ⇒ 会踩相邻数组 = 未定义）', async () => {
  for (const bad of [3, 7, -1]) {
    const m = machineFor([stopGroup(bad)]);
    await assert.rejects(m.run(), /voice-group-out-of-domain/, `组号 ${bad} 必须响亮失败`);
  }
});

test('★ 前提钉住：本层没有任何指令会把声音通道置成"在播"（推导的标志值不是猜的）', async () => {
  // 唯一允许出现 playing 的地方是"读"：本文件与 handlers.ts 里不许有写入点。
  const m = machineFor([stopGroup(0), stopGroup(1), stopGroup(2)]);
  await m.run();
  assert.equal(m.sound.buffers.size, 0, '跑完三条停止指令后仍不许出现任何通道对象');
  assert.equal(m.scalars.get('voice.stillPlaying'), 0);
});


test('★ 0x2f8 设音量：读序 = 先 op2 后 op1；夹到 ±10000；通道 = 组号 + 12', async () => {
  const m = machineFor([[0x2f8, [[0, 1], [0, 12345]]]]);
  await m.run();
  assert.equal(m.sound.volumes.get(13), 10000, '组 1 ⇒ 通道 13；12345 夹到 10000');
  assert.deepEqual(m.effects.filter((e) => e.kind === 'audio.volume.request'),
    [{ kind: 'audio.volume.request', detail: { channel: 13, volume: 10000 } }]);

  const neg = machineFor([[0x2f8, [[0, 0], [0, -20000]]]]);
  await neg.run();
  assert.equal(neg.sound.volumes.get(12), -10000, '-20000 夹到 -10000');
});

test('★ 0x2f8 通道号 ≥ 15：只写错误文本，不许改音量、不许发请求', async () => {
  const m = machineFor([[0x2f8, [[0, 3], [0, 500]]]]);
  await m.run();
  assert.match(m.sound.errorBuffer, /dsSetVolume/);
  assert.equal(m.sound.volumes.has(15), false, '组 3 ⇒ 通道 15，越界 ⇒ 不许落音量');
  assert.deepEqual(m.effects.filter((e) => e.kind === 'audio.volume.request'), [],
    '越界时引擎只写错误文本 + 打日志，不发设置请求');
});

test('★ 0x2f8 后端不存在：音量格照写（引擎在调后端之前就写），但不发设置请求', async () => {
  const m = machineFor([[0x2f8, [[0, 0], [0, 500]]]]);
  m.sound.backend = 'absent';
  await m.run();
  assert.equal(m.sound.volumes.get(12), 500, '`this[a2+375] = v5` 在调用后端之前 ⇒ 与后端无关');
  assert.deepEqual(m.effects.filter((e) => e.kind === 'audio.volume.request'), []);
  assert.equal(m.sound.errorBuffer, '');
});

