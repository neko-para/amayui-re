/** @env pure @kind contract @why wait_subagent 的等待/插话/竞态语义被破坏 */
/**
 * plugins/wait-subagent/test/wait.test.mjs —— 等待逻辑的契约
 *
 * 测的是“什么时候返回、返回什么”，不测 DSH 的真实 agent loop：
 *   · activation 的 start/end 记录与“旧 activation 迟到的 end”；
 *   · 人插话只认 `source.kind === 'user'`，subagent 完成通知不能误判；
 *   · pending / 已结束 / abort / timeout 的返回；
 *   · `any_of`（默认，等第一个）与 `all_of`（等全部）的差别；
 *   · 挂监听前的竞态（先 end 再 wait）不能挂死。
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';

import {
  createRunTracker,
  inboxDelivery,
  isHumanMessage,
  messagePreview,
  pendingHumanMessages,
  waitForSubagents,
  WAIT_MODES,
} from '../lib/wait.mjs';

/** 最小事件总线：`on` 返回 disposer，`emit` 同步派发。 */
function eventBus() {
  const handlers = new Map();
  return {
    on(name, listener) {
      let set = handlers.get(name);
      if (set === undefined) {
        set = new Set();
        handlers.set(name, set);
      }
      set.add(listener);
      return () => set.delete(listener);
    },
    emit(name, payload) {
      for (const listener of [...(handlers.get(name) ?? [])]) listener(payload);
    },
    count(name) {
      return handlers.get(name)?.size ?? 0;
    },
  };
}

/** 一条人发的 inbox 消息。 */
function humanMessage(id = 'm1', text = 'stop waiting and do X') {
  return { id, source: { kind: 'user' }, content: [{ type: 'text', text }] };
}

/** 非人类来源：runtime 的 subagent 完成通知。 */
function settlementMessage(id = 'm2') {
  return { id, source: { kind: 'subagent-settled', senderSessionId: 'child' }, content: [{ type: 'text', text: 'done' }] };
}

/** 假 Agent：只需要 ctx / inbox。 */
function fakeAgent({ bus, nextStep = [], nextTurn = [] }) {
  return { id: 'parent', ctx: bus, inbox: { nextStep, nextTurn } };
}

/** 让已经 settle 的 promise 跑到（用于断言“还没返回”）。 */
async function flush() {
  await new Promise((resolve) => setImmediate(resolve));
}

/** 一个 target。 */
function target(childId, runId) {
  return runId === undefined ? { childId } : { childId, runId };
}

test('★ tracker：start/end 记录状态，旧 run 迟到的 end 不覆盖新 run', () => {
  const bus = eventBus();
  const tracker = createRunTracker(bus);

  bus.emit('subagent/start', { id: 'child', runId: 'r1' });
  assert.equal(tracker.get('child')?.state, 'running');
  assert.equal(tracker.get('child')?.runId, 'r1');

  bus.emit('subagent/end', { id: 'child', runId: 'r1', stopReason: 'completed' });
  assert.equal(tracker.get('child')?.state, 'settled');
  assert.equal(tracker.get('child')?.info?.stopReason, 'completed');

  // cold resume：同一个 child 新开一个 activation
  bus.emit('subagent/start', { id: 'child', runId: 'r2' });
  assert.equal(tracker.get('child')?.state, 'running');
  assert.equal(tracker.get('child')?.runId, 'r2');

  // r1 的重复 end 不能把 r2 的状态改回 settled
  bus.emit('subagent/end', { id: 'child', runId: 'r1', stopReason: 'error' });
  assert.equal(tracker.get('child')?.state, 'running');
  assert.equal(tracker.get('child')?.runId, 'r2');

  tracker.dispose();
  assert.equal(bus.count('subagent/start'), 0);
  assert.equal(bus.count('subagent/end'), 0);
  assert.equal(tracker.get('child'), undefined);
});

test('★ 人插话判定：只认 source.kind === "user"（完成通知不算）', () => {
  assert.equal(isHumanMessage(humanMessage()), true);
  assert.equal(isHumanMessage(settlementMessage()), false);
  assert.equal(isHumanMessage({ source: { kind: 'runtime-context' } }), false);
  assert.equal(isHumanMessage({ source: { kind: 'agent-message' } }), false);
  assert.equal(isHumanMessage(undefined), false);
});

test('★ pending / delivery：先 next-step，再 next-turn', () => {
  const step = humanMessage('s1', 'steer me');
  const turn = humanMessage('t1', 'queue me');
  const agent = fakeAgent({ bus: eventBus(), nextStep: [step], nextTurn: [turn] });

  assert.deepEqual(pendingHumanMessages(agent), [step, turn]);
  assert.equal(inboxDelivery(agent, step), 'steer');
  assert.equal(inboxDelivery(agent, turn), 'queue');
  assert.equal(inboxDelivery(agent, humanMessage('missing')), 'unknown');
});

test('★ messagePreview：取文本、压平、截断', () => {
  assert.equal(messagePreview(humanMessage('m', '  hello\nworld  ')), 'hello\nworld');
  assert.equal(messagePreview({ content: [{ type: 'image' }] }), '');
  const long = messagePreview({ content: [{ type: 'text', text: 'x'.repeat(20) }] }, 5);
  assert.equal(long.length, 5);
  assert.ok(long.endsWith('…'));
});

test('★ 等 activation：end 到达后返回 finished，并清掉本次监听', async () => {
  const bus = eventBus();
  const tracker = createRunTracker(bus);
  const agent = fakeAgent({ bus });
  bus.emit('subagent/start', { id: 'child', runId: 'r1' });
  const before = bus.count('subagent/end');

  const pending = waitForSubagents({ agent, targets: [target('child', 'r1')], tracker });
  bus.emit('subagent/end', { id: 'child', runId: 'r1', stopReason: 'completed', lastAssistantMessage: [{ type: 'text', text: 'ok' }] });

  const outcome = await pending;
  assert.equal(outcome.kind, 'finished');
  assert.equal(outcome.race, undefined);
  assert.deepEqual(outcome.settled.map((entry) => entry.childId), ['child']);
  assert.equal(outcome.settled[0].info?.stopReason, 'completed');
  assert.deepEqual(outcome.pending, []);
  assert.equal(bus.count('subagent/end'), before, '本次 wait 的监听必须被清掉（tracker 自己的监听保留）');
});

test('★ any_of 默认：两个 child 里**先结束的那个**就返回，另一个留在 pending', async () => {
  const bus = eventBus();
  const tracker = createRunTracker(bus);
  const agent = fakeAgent({ bus });
  bus.emit('subagent/start', { id: 'a', runId: 'r1' });
  bus.emit('subagent/start', { id: 'b', runId: 'r2' });
  const before = bus.count('subagent/end');

  const waiting = waitForSubagents({ agent, targets: [target('a', 'r1'), target('b', 'r2')], tracker });
  bus.emit('subagent/end', { id: 'b', runId: 'r2', stopReason: 'completed' });

  const outcome = await waiting;
  assert.equal(outcome.kind, 'finished');
  assert.deepEqual(outcome.settled.map((entry) => entry.childId), ['b']);
  assert.deepEqual(outcome.pending, ['a'], 'any_of 不替调用方等最慢的那个');
  assert.equal(bus.count('subagent/end'), before, '返回后必须清掉本次监听的 subagent/end');
});

test('★ all_of：第一个 end 不返回，最后一个 end 才返回（带全部 stop_reason）', async () => {
  const bus = eventBus();
  const tracker = createRunTracker(bus);
  const agent = fakeAgent({ bus });
  bus.emit('subagent/start', { id: 'a', runId: 'r1' });
  bus.emit('subagent/start', { id: 'b', runId: 'r2' });

  let resolved = false;
  const waiting = waitForSubagents({
    agent,
    targets: [target('a', 'r1'), target('b', 'r2')],
    mode: WAIT_MODES.ALL_OF,
    tracker,
  }).then((outcome) => {
    resolved = true;
    return outcome;
  });

  bus.emit('subagent/end', { id: 'a', runId: 'r1', stopReason: 'completed' });
  await flush();
  assert.equal(resolved, false, 'all_of 不能因为第一个结束就返回');

  bus.emit('subagent/end', { id: 'b', runId: 'r2', stopReason: 'refusal' });
  const outcome = await waiting;
  assert.equal(outcome.kind, 'finished');
  assert.deepEqual(outcome.settled.map((entry) => [entry.childId, entry.info?.stopReason]), [
    ['a', 'completed'],
    ['b', 'refusal'],
  ]);
  assert.deepEqual(outcome.pending, []);
});

test('★ any_of：调用前已经有一个结束 ⇒ 立即返回 race（不进入 await），另一个进 pending', async () => {
  const bus = eventBus();
  const tracker = createRunTracker(bus);
  const agent = fakeAgent({ bus });
  bus.emit('subagent/start', { id: 'a', runId: 'r1' });
  bus.emit('subagent/end', { id: 'a', runId: 'r1', stopReason: 'completed' });
  bus.emit('subagent/start', { id: 'b', runId: 'r2' });

  const outcome = await waitForSubagents({ agent, targets: [target('a', 'r1'), target('b', 'r2')], tracker });
  assert.equal(outcome.kind, 'finished');
  assert.equal(outcome.race, true, 'race=true 表示“根本不用等”，模型可见 status 由 index.mjs 翻成 already_finished');
  assert.deepEqual(outcome.settled.map((entry) => entry.childId), ['a']);
  assert.deepEqual(outcome.pending, ['b']);
  assert.equal(bus.count('subagent/end'), 1, '不挂本次等待的监听（只有 tracker 那条）');
});

test('★ all_of：一个早已结束、一个仍在跑 ⇒ 只等仍在跑的那个', async () => {
  const bus = eventBus();
  const tracker = createRunTracker(bus);
  const agent = fakeAgent({ bus });
  bus.emit('subagent/start', { id: 'a', runId: 'r1' });
  bus.emit('subagent/end', { id: 'a', runId: 'r1', stopReason: 'completed' });
  bus.emit('subagent/start', { id: 'b', runId: 'r2' });

  const waiting = waitForSubagents({
    agent,
    targets: [target('a', 'r1'), target('b', 'r2')],
    mode: WAIT_MODES.ALL_OF,
    tracker,
  });
  bus.emit('subagent/end', { id: 'b', runId: 'r2', stopReason: 'completed' });

  const outcome = await waiting;
  assert.equal(outcome.kind, 'finished');
  assert.equal(outcome.race, undefined);
  assert.deepEqual(outcome.settled.map((entry) => entry.childId), ['a', 'b']);
  assert.deepEqual(outcome.pending, []);
});

test('★ runId 不匹配的 end 被忽略（cold resume 后只等新 activation）', async () => {
  const bus = eventBus();
  const tracker = createRunTracker(bus);
  const agent = fakeAgent({ bus });
  bus.emit('subagent/start', { id: 'child', runId: 'r2' });

  let resolved = false;
  const waiting = waitForSubagents({ agent, targets: [target('child', 'r2')], tracker }).then((outcome) => {
    resolved = true;
    return outcome;
  });

  bus.emit('subagent/end', { id: 'child', runId: 'r1', stopReason: 'completed' });
  await flush();
  assert.equal(resolved, false, '旧 run 的 end 不能让新 run 的等待提前返回');

  bus.emit('subagent/end', { id: 'child', runId: 'r2', stopReason: 'completed' });
  assert.equal((await waiting).kind, 'finished');
});

test('★ 不在 targets 里的 child 结束不影响等待', async () => {
  const bus = eventBus();
  const tracker = createRunTracker(bus);
  const agent = fakeAgent({ bus });
  bus.emit('subagent/start', { id: 'waited', runId: 'r1' });

  let resolved = false;
  const waiting = waitForSubagents({ agent, targets: [target('waited', 'r1')], tracker }).then((outcome) => {
    resolved = true;
    return outcome;
  });

  bus.emit('subagent/end', { id: 'sibling', runId: 'r9', stopReason: 'completed' });
  await flush();
  assert.equal(resolved, false);

  bus.emit('subagent/end', { id: 'waited', runId: 'r1', stopReason: 'completed' });
  const outcome = await waiting;
  assert.deepEqual(outcome.settled.map((entry) => entry.childId), ['waited']);
});

test('★ 用户插话：steer 立即返回，并带上投递窗口与快照', async () => {
  const bus = eventBus();
  const tracker = createRunTracker(bus);
  const agent = fakeAgent({ bus });
  bus.emit('subagent/start', { id: 'child', runId: 'r1' });

  const pending = waitForSubagents({ agent, targets: [target('child', 'r1')], tracker });
  const message = humanMessage();
  agent.inbox.nextStep.push(message);
  bus.emit('agent/inbox/inserted', { message });

  const outcome = await pending;
  assert.equal(outcome.kind, 'user_interjected');
  assert.equal(outcome.delivery, 'steer');
  assert.equal(outcome.message, message);
  assert.deepEqual(outcome.pending, ['child'], '插话时仍把“谁还在跑”交回去');
});

test('★ 用户插话：queue 立即返回，并标成 queue', async () => {
  const bus = eventBus();
  const tracker = createRunTracker(bus);
  const agent = fakeAgent({ bus });
  bus.emit('subagent/start', { id: 'child', runId: 'r1' });

  const pending = waitForSubagents({ agent, targets: [target('child', 'r1')], tracker });
  const message = humanMessage();
  agent.inbox.nextTurn.push(message);
  bus.emit('agent/inbox/inserted', { message });

  const outcome = await pending;
  assert.equal(outcome.kind, 'user_interjected');
  assert.equal(outcome.delivery, 'queue');
});

test('★ 同一时刻既有人消息又收到 end：优先报告人插话（人不能被漏掉）', async () => {
  const bus = eventBus();
  const tracker = createRunTracker(bus);
  const agent = fakeAgent({ bus });
  bus.emit('subagent/start', { id: 'child', runId: 'r1' });
  const pending = waitForSubagents({ agent, targets: [target('child', 'r1')], tracker });

  // 模拟 settlement notice 先把人消息留在 inbox，然后 end 到达
  const message = humanMessage();
  agent.inbox.nextStep.push(message);
  bus.emit('subagent/end', { id: 'child', runId: 'r1', stopReason: 'completed' });

  const outcome = await pending;
  assert.equal(outcome.kind, 'user_interjected');
  assert.equal(outcome.delivery, 'steer');
});

test('★ 非人插话不打断等待', async () => {
  const bus = eventBus();
  const tracker = createRunTracker(bus);
  const agent = fakeAgent({ bus });
  bus.emit('subagent/start', { id: 'child', runId: 'r1' });

  let resolved = false;
  const pending = waitForSubagents({ agent, targets: [target('child', 'r1')], tracker }).then((outcome) => {
    resolved = true;
    return outcome;
  });

  bus.emit('agent/inbox/inserted', { message: settlementMessage() });
  await flush();
  assert.equal(resolved, false, 'subagent-settled 不能当成用户插话');

  bus.emit('subagent/end', { id: 'child', runId: 'r1', stopReason: 'completed' });
  assert.equal((await pending).kind, 'finished');
});

test('★ 开始等待前已经入队的人消息：立即返回，不进入 await', async () => {
  const bus = eventBus();
  const tracker = createRunTracker(bus);
  const message = humanMessage();
  const agent = fakeAgent({ bus, nextTurn: [message] });
  bus.emit('subagent/start', { id: 'child', runId: 'r1' });

  const outcome = await waitForSubagents({ agent, targets: [target('child', 'r1')], tracker });
  assert.equal(outcome.kind, 'user_interjected');
  assert.equal(outcome.delivery, 'queue');
});

test('★ 开始等待前 activation 已经结束：立即返回 finished（不挂死）', async () => {
  const bus = eventBus();
  const tracker = createRunTracker(bus);
  const agent = fakeAgent({ bus });
  bus.emit('subagent/start', { id: 'child', runId: 'r1' });
  bus.emit('subagent/end', { id: 'child', runId: 'r1', stopReason: 'refusal' });

  const outcome = await waitForSubagents({ agent, targets: [target('child', 'r1')], tracker });
  assert.equal(outcome.kind, 'finished');
  assert.equal(outcome.race, true);
  assert.equal(outcome.settled[0].info?.stopReason, 'refusal');
});

test('★ abort：turn 取消时立即返回 cancelled', async () => {
  const bus = eventBus();
  const tracker = createRunTracker(bus);
  const agent = fakeAgent({ bus });
  bus.emit('subagent/start', { id: 'child', runId: 'r1' });
  const controller = new AbortController();

  const pending = waitForSubagents({ agent, targets: [target('child', 'r1')], tracker, signal: controller.signal });
  controller.abort(new Error('user stop'));

  const outcome = await pending;
  assert.equal(outcome.kind, 'cancelled');
  assert.equal(bus.count('agent/inbox/inserted'), 0, 'abort 后监听必须清掉');
});

test('★ timeout：可选安全上限到点返回 timed_out，而不是挂死', async () => {
  const bus = eventBus();
  const tracker = createRunTracker(bus);
  const agent = fakeAgent({ bus });
  bus.emit('subagent/start', { id: 'child', runId: 'r1' });

  const outcome = await waitForSubagents({ agent, targets: [target('child', 'r1')], tracker, timeoutMs: 10 });
  assert.equal(outcome.kind, 'timed_out');
  assert.equal(outcome.elapsedMs, 10);
});

test('★ all_of + timeout：到点返回时带上“已结束 / 仍在跑”的快照', async () => {
  const bus = eventBus();
  const tracker = createRunTracker(bus);
  const agent = fakeAgent({ bus });
  bus.emit('subagent/start', { id: 'a', runId: 'r1' });
  bus.emit('subagent/start', { id: 'b', runId: 'r2' });

  const waiting = waitForSubagents({
    agent,
    targets: [target('a', 'r1'), target('b', 'r2')],
    mode: WAIT_MODES.ALL_OF,
    tracker,
    timeoutMs: 10,
  });
  bus.emit('subagent/end', { id: 'a', runId: 'r1', stopReason: 'completed' });

  const outcome = await waiting;
  assert.equal(outcome.kind, 'timed_out');
  assert.deepEqual(outcome.settled.map((entry) => entry.childId), ['a']);
  assert.deepEqual(outcome.pending, ['b']);
});
