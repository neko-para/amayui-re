/** @env pure @kind contract @why 影片的“置位 / 播完清槽 / 循环重启 / 不释放纹理”写错时，门会永远关着或把纹理一起丢掉 */
/**
 * tools/test/emulator-new-movie.test.mjs —— 0x20f + CORE-VM-MOVIE 的核心守卫
 *
 * 用 fake MovieHost 驱动“当前位置 / 停止位置 / 已结束”，不依赖解码器 / 游戏安装。
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';

import { loadOpcodeTable } from '../../apps/emulator-new/src/platform/node/spec.ts';
import { HANDLERS } from '../../apps/emulator-new/src/core/handlers.ts';
import { Machine, MOVIE_BIT } from '../../apps/emulator-new/src/core/machine.ts';

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

function machineFor(instrs, host) {
  return new Machine({ body: script(instrs), table, key: KEY, movieHost: host });
}

function fakeHost(initial = {}) {
  const state = { currentTime: 0, stopTime: 10, ended: false, ...initial };
  const calls = { open: 0, play: 0, poll: 0, rewind: 0, close: 0, fill: 0 };
  const handle = {};
  return {
    state,
    calls,
    handle,
    open(req) { calls.open += 1; return handle; },
    play() { calls.play += 1; },
    poll() { calls.poll += 1; return { ...state }; },
    rewind() { calls.rewind += 1; state.currentTime = 0; state.ended = false; },
    close() { calls.close += 1; },
    fillTexture() { calls.fill += 1; },
  };
}

function withTexture(m, id = 42) {
  m.scene.textureObjects.set(id, { kind: 'normal', width: 1280, height: 720 });
  return m;
}

test('★ 0x20f：只建对象 / 打开 / 启动 / 置位；handler 内不 poll', async () => {
  const host = fakeHost();
  const m = withTexture(machineFor([], host));
  await HANDLERS.get(0x20f)(m, [[0, 21063], [0, 42], [0, 506]].map(([type, raw]) => ({ type, raw })));
  const obj = m.scene.objectSlots.get(42);
  assert.equal(obj.kind, 'movie');
  assert.equal(obj.resourceId, 21063);
  assert.equal(obj.opt, 506);
  assert.equal(obj.loop, false);
  assert.equal(obj.started, true);
  assert.equal(obj.handle, host.handle);
  assert.equal(m.flags & MOVIE_BIT, MOVIE_BIT);
  assert.equal(m.scene.hasObjects, 1);
  assert.deepEqual(host.calls, { open: 1, play: 1, poll: 0, rewind: 0, close: 0, fill: 0 });
});

test('★ 播完（option 506：不循环、不填纹理）：清对象槽、关闭后端、不填纹理、不释放纹理', async () => {
  const host = fakeHost();
  const m = withTexture(machineFor([[0x20f, [[0, 21063], [0, 42], [0, 506]]], [0x1a8, []]], host));
  host.state.currentTime = host.state.stopTime;
  const r = await m.run({ maxSteps: 2 });
  assert.equal(r.steps, 2, '播完后门应放行，执行下一条');
  assert.equal(m.scene.objectSlots.has(42), false);
  assert.equal(host.calls.close, 1);
  assert.equal(host.calls.fill, 0, '(506 & 3) == 2 ⇒ 不填纹理');
  assert.equal(m.scene.textureObjects.has(42), true, '只清对象槽，不释放纹理对象');
  assert.equal(m.flags & MOVIE_BIT, 0);
});

test('★ option 0：收尾时调用一次填纹理（像素效果不在本层）', async () => {
  const host = fakeHost();
  const m = withTexture(machineFor([[0x20f, [[0, 21063], [0, 42], [0]]], [0x1a8, []]], host));
  host.state.currentTime = host.state.stopTime;
  await m.run({ maxSteps: 2 });
  assert.equal(host.calls.fill, 1, '(0 & 3) != 2 ⇒ 填纹理');
});

test('★ 循环位：播完回到开头并保留对象 / 槽；不清位', async () => {
  const host = fakeHost();
  const m = withTexture(machineFor([], host));
  await HANDLERS.get(0x20f)(m, [[0, 21063], [0, 42], [0, 1]].map(([type, raw]) => ({ type, raw })));
  host.state.currentTime = host.state.stopTime;
  m.serviceMovies();
  assert.equal(m.scene.objectSlots.has(42), true);
  assert.equal(host.calls.rewind, 1);
  assert.equal(host.calls.play, 2, '打开时 play 一次 + 循环重启一次');
  assert.equal(host.calls.close, 0);
  assert.equal(m.scene.hasObjects, 1);
});

test('★ 没有影片后端：对象保留、门保持关闭（不伪造播完）', async () => {
  const m = withTexture(machineFor([[0x20f, [[0, 21063], [0, 42], [0]]], [0x1a8, []]]));
  m.scene.textureObjects.set(42, { kind: 'normal', width: 1, height: 1 });
  m.scene.objectSlots.set(42, {
    kind: 'movie', slot: 42, resourceId: 1, opt: 0, started: true,
    handle: null, loop: false, currentTime: 0, stopTime: 0,
  });
  const r = await m.run({ maxSteps: 2 });
  assert.equal(r.stop.kind, 'gate');
  assert.equal(m.flags & MOVIE_BIT, MOVIE_BIT);
});
