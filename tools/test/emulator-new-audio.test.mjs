/** @env pure @kind contract @why 音频边界两侧都会被实现错：0xb4/0xb5 被当成"整条跳过"（不发请求）、或 0xb4 的槽号被合成一个 15（凭空多出 5 个引擎格）、或 0xbf 不清第 9 位 */
/**
 * tools/test/emulator-new-audio.test.mjs —— `60-ops-voice.md` 的音频请求边界守卫
 *
 * 覆盖：
 * - `CORE-OPS-AUDIO-WAV`：0xb4 装载（发请求 + **装载成功时写该槽的「当前资源号」格**）/ 0xb5 播放（只发请求）；
 *   两条都不改门位 / 脚本变量 / 当前帧；槽号的两个界（后端 15 / 引擎表 10）分开判
 * - `CORE-OPS-AUDIO-BGM`：0xbf 清状态字第 9 位（其它位保留）+ 发 BGM 请求
 *
 * ★ 0xbf 的清位在"整字为 0 才派发"的门模型下是防御性分支；这里直接调 handler，
 *   单测"派发到它时"的引擎数据效果（与 spec 的写法一致）。
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';

import { loadOpcodeTable } from '../../apps/emulator-new/src/platform/node/spec.ts';
import { Machine } from '../../apps/emulator-new/src/core/machine.ts';
import { HANDLERS } from '../../apps/emulator-new/src/core/handlers.ts';

const table = await loadOpcodeTable();
const KEY = 0x1234abcd;

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

test('★ 音效装载与播放：0xb4 / 0xb5', async () => {
  const m = machineFor([[0xb4, [[0, 0x39a3], [0, 1]]], [0xb5, [[0, 2]]]]);
  await m.run();
  assert.deepEqual(
    m.effects.filter((e) => e.kind.startsWith('audio.')),
    [
      { kind: 'audio.wav.load', detail: { resource: 0x39a3, slot: 1 } },
      { kind: 'audio.wav.play', detail: { slot: 2 } },
    ],
    '两条都只发一次请求，参数按「资源 / 槽」与「槽」写',
  );
  assert.equal(m.seSlotResource[1], 0x39a3, '装载成功要写该槽的「当前资源号」格（规则第 2 步）');
  assert.equal(m.flags, 0, '不置门位');
  assert.equal(m.scalars.size, 0, '不写脚本标量');
  assert.equal(m.steps, 2, '各执行一次');
});

test('★ 音效槽号的两个界：后端 15 / 引擎表 10（10..14 只发请求不写表，≥15 连请求都不发）', async () => {
  const m = machineFor([
    [0xb4, [[0, 0x1111], [0, 9]]],   // 表内
    [0xb4, [[0, 0x2222], [0, 10]]],  // 表外（后端受理）
    [0xb4, [[0, 0x3333], [0, 15]]],  // 后端拒绝
    [0xb5, [[0, 14]]],               // 后端受理的最后一格
    [0xb5, [[0, 15]]],               // 后端拒绝
  ]);
  await m.run();
  assert.deepEqual(
    m.effects.filter((e) => e.kind.startsWith('audio.')),
    [
      { kind: 'audio.wav.load', detail: { resource: 0x1111, slot: 9 } },
      { kind: 'audio.wav.load', detail: { resource: 0x2222, slot: 10 } },
      { kind: 'audio.wav.play', detail: { slot: 14 } },
    ],
    '槽 10..14 后端照常受理（要发请求）；槽 15 后端拒绝（不发请求）',
  );
  assert.equal(m.seSlotResource[9], 0x1111, '表内那格要写');
  assert.equal(m.seSlotResource.length, 10, '引擎表只有 10 格 —— 不许为了"装下槽 10..14"把它撑成 15');
  assert.deepEqual(
    m.diagnostics.filter((d) => d.kind.startsWith('audio.')).map((d) => `${d.kind}:${d.index}`),
    [
      'audio.wav.load-slot-past-engine-table:10',
      'audio.wav.load-slot-out-of-range:15',
      'audio.wav.play-slot-out-of-range:15',
    ],
    '三条偏离各记一条诊断（10..14 是表外；15 是后端拒绝）',
  );
});

test('★ BGM：0xbf 清第 9 位', () => {
  const m = new Machine({ body: script([[0xbf, [[0, 0x1f]]]]), table, key: KEY });
  // 直接调 handler：门模型下位非 0 时不会派发；这里只验"派发到它"的效果。
  m.flags = 0x200 | 0x1;
  HANDLERS.get(0xbf)(m, [{ type: 0, raw: 0x1f }]);
  assert.equal(m.flags, 0x1, '只清第 9 位，睡眠位保留');
  assert.deepEqual(m.effects, [{ kind: 'audio.bgm.play', detail: { resource: 0x1f } }], '发一次 BGM 请求');
  assert.equal(m.scalars.size, 0, '不写脚本标量');
});
