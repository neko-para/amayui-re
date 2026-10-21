/** @env pure @kind contract @why wait_subagent 插件接线/模型可见结果坏了 */
/**
 * plugins/wait-subagent/test/plugin.test.mjs —— 插件加载与工具契约
 *
 * 用真实的 `defineTool`（DSH 的普通依赖）验证：
 *   · `apply` 只注册一个 `wait_subagent`，参数/输出 schema 能被吃下；
 *   · `execute` 在 live / 已结束 / 未运行 / 用户插话 / 取消 / 非法 child 下的返回；
 *   · 模型看到的 render 文本是**可执行**的（不是内部状态名）。
 *
 * 不测真实 agent loop —— 那要起完整 DSH；事件口径由 `wait.test.mjs` 的竞态用例守。
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';

import * as plugin from '../index.mjs';

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

/** 人发的 inbox 消息。 */
function humanMessage(id = 'm1', text = 'stop waiting') {
  return { id, source: { kind: 'user' }, content: [{ type: 'text', text }] };
}

/** 假 Agent：ctx / inbox 足够 `wait_subagent` 用。 */
function fakeAgent({ bus, nextStep = [], nextTurn = [] }) {
  return { id: 'parent', ctx: bus, inbox: { nextStep, nextTurn } };
}

/** 轮询到条件成立（让 execute 走到挂监听之后的 await）。 */
async function until(predicate, label) {
  for (let attempt = 0; attempt < 100; attempt += 1) {
    if (predicate()) return;
    await new Promise((resolve) => setImmediate(resolve));
  }
  throw new Error(`timed out waiting for ${label}`);
}

/**
 * 造一个插件上下文。
 * @param {object} options - catalog / live agent behavior.
 * @returns {{ctx: object, bus: object, tools: object[]}} fake host.
 */
function fakeHost({ children = [{ id: 'child', mode: 'continuable' }], live } = {}) {
  const bus = eventBus();
  const tools = [];
  const subagents = { listChildren: async () => children };
  const agents = {
    get: (id) => (live?.(id) ? { id } : undefined),
    isOwnedBy: () => true,
  };
  const ctx = {
    on: bus.on,
    get(serviceName) {
      if (serviceName === 'subagents') return subagents;
      if (serviceName === 'agents') return agents;
      return undefined;
    },
    tools: { register: (definition) => tools.push(definition) },
  };
  return { ctx, bus, tools };
}

test('★ 插件自我声明：只注册一个 `wait_subagent`，schema 能被 defineTool 吃下', () => {
  assert.equal(plugin.name, 'amayui-wait-subagent');
  assert.deepEqual(plugin.inject, ['tools']);
  assert.equal(typeof plugin.apply, 'function');

  const { ctx, tools } = fakeHost();
  plugin.apply(ctx);
  assert.equal(tools.length, 1);
  const tool = tools[0];
  assert.equal(tool.name, 'wait_subagent');
  assert.equal(tool.parameters.properties.agent_id.type, 'string');
  assert.equal(tool.output.schema.properties.status.enum.includes('user_interjected'), true);
  assert.equal(typeof tool.output.render, 'function');
  assert.equal(tool.isConcurrencySafe({ agent_id: 'child' }), true);
});

test('★ live child：等 end，返回 finished + 停止等待的指令', async () => {
  const { ctx, bus, tools } = fakeHost();
  plugin.apply(ctx);
  const tool = tools[0];
  const agent = fakeAgent({ bus });
  bus.emit('subagent/start', { id: 'child', runId: 'r1' });

  const controller = new AbortController();
  const pending = tool.execute({ agent_id: 'child' }, { agent, signal: controller.signal });
  await until(() => bus.count('agent/inbox/inserted') === 1, 'wait listeners');
  bus.emit('subagent/end', { id: 'child', runId: 'r1', stopReason: 'completed' });

  const value = await pending;
  assert.equal(value.status, 'finished');
  assert.equal(value.stop_reason, 'completed');
  assert.match(value.note, /do not call sleep/i);
  assert.equal(tool.output.render(undefined, value)[0].type, 'text');
});

test('★ 用户 queue 插话：返回 user_interjected + delivery=queue + 让模型结束 turn', async () => {
  const { ctx, bus, tools } = fakeHost();
  plugin.apply(ctx);
  const tool = tools[0];
  const agent = fakeAgent({ bus });
  bus.emit('subagent/start', { id: 'child', runId: 'r1' });

  const controller = new AbortController();
  let concluded = false;
  const pending = tool.execute({ agent_id: 'child' }, { agent, signal: controller.signal, concludeTurn: () => { concluded = true; } });
  await until(() => bus.count('agent/inbox/inserted') === 1, 'wait listeners');

  const message = humanMessage();
  agent.inbox.nextTurn.push(message);
  bus.emit('agent/inbox/inserted', { message });

  const value = await pending;
  assert.equal(value.status, 'user_interjected');
  assert.equal(value.delivery, 'queue');
  assert.equal(value.message_preview, 'stop waiting');
  assert.match(value.note, /closes the turn/);
  assert.equal(concluded, true, 'queue 必须标 concludesTurn，loop 才会自动关 turn');
});

test('★ 用户 steer 插话：不标 concludesTurn，消息在下一步处理', async () => {
  const { ctx, bus, tools } = fakeHost();
  plugin.apply(ctx);
  const tool = tools[0];
  const agent = fakeAgent({ bus });
  bus.emit('subagent/start', { id: 'child', runId: 'r1' });

  let concluded = false;
  const pending = tool.execute({ agent_id: 'child' }, { agent, signal: new AbortController().signal, concludeTurn: () => { concluded = true; } });
  await until(() => bus.count('agent/inbox/inserted') === 1, 'wait listeners');

  const message = humanMessage();
  agent.inbox.nextStep.push(message);
  bus.emit('agent/inbox/inserted', { message });

  const value = await pending;
  assert.equal(value.status, 'user_interjected');
  assert.equal(value.delivery, 'steer');
  assert.equal(concluded, false, 'steer 不要让 loop 关 turn');
});

test('★ catalog 读取期间用户插话：不进入等待，立即 user_interjected', async () => {
  const { ctx, bus, tools } = fakeHost();
  plugin.apply(ctx);
  const tool = tools[0];
  const agent = fakeAgent({ bus });
  bus.emit('subagent/start', { id: 'child', runId: 'r1' });

  let release;
  const gate = new Promise((resolve) => { release = resolve; });
  const subagents = ctx.get('subagents');
  subagents.listChildren = async () => {
    await gate;
    return [{ id: 'child', mode: 'continuable' }];
  };

  const pending = tool.execute({ agent_id: 'child' }, { agent, signal: new AbortController().signal });
  const message = humanMessage('m-race', 'stop');
  agent.inbox.nextTurn.push(message);
  release();

  const value = await pending;
  assert.equal(value.status, 'user_interjected');
  assert.equal(value.delivery, 'queue');
});

test('★ 已经结束：不再挂等待，直接 already_finished', async () => {
  const { ctx, bus, tools } = fakeHost();
  plugin.apply(ctx);
  const tool = tools[0];
  const agent = fakeAgent({ bus });
  bus.emit('subagent/start', { id: 'child', runId: 'r1' });
  bus.emit('subagent/end', { id: 'child', runId: 'r1', stopReason: 'completed' });

  const value = await tool.execute({ agent_id: 'child' }, { agent, signal: new AbortController().signal });
  assert.equal(value.status, 'already_finished');
  assert.equal(value.stop_reason, 'completed');
  assert.match(value.note, /Do not wait for it again/);
});

test('★ 没有 live activation：立即 not_running，不轮询', async () => {
  const { ctx, bus, tools } = fakeHost();
  plugin.apply(ctx);
  const tool = tools[0];
  const agent = fakeAgent({ bus });

  const value = await tool.execute({ agent_id: 'child' }, { agent, signal: new AbortController().signal });
  assert.equal(value.status, 'not_running');
  assert.match(value.note, /no live activation/);
});

test('★ 待处理的人消息：在调用前就入队 ⇒ 立即 user_interjected（优先于等待）', async () => {
  const { ctx, bus, tools } = fakeHost();
  plugin.apply(ctx);
  const tool = tools[0];
  const message = humanMessage('m0', 'look at this');
  const agent = fakeAgent({ bus, nextStep: [message] });

  const value = await tool.execute({ agent_id: 'child' }, { agent, signal: new AbortController().signal });
  assert.equal(value.status, 'user_interjected');
  assert.equal(value.delivery, 'steer');
});

test('★ 非法 child：不是直接子代理 ⇒ errored result，不去等一个陌生 session', async () => {
  const { ctx, bus, tools } = fakeHost({ children: [] });
  plugin.apply(ctx);
  const tool = tools[0];
  const agent = fakeAgent({ bus });

  await assert.rejects(
    () => tool.execute({ agent_id: 'stranger' }, { agent, signal: new AbortController().signal }),
    /not a direct child/,
  );
});

test('★ 调用前已取消：立即 cancelled', async () => {
  const { ctx, bus, tools } = fakeHost();
  plugin.apply(ctx);
  const tool = tools[0];
  const agent = fakeAgent({ bus });
  const controller = new AbortController();
  controller.abort(new Error('stop'));

  const value = await tool.execute({ agent_id: 'child' }, { agent, signal: controller.signal });
  assert.equal(value.status, 'cancelled');
});

test('★ 等待中取消：cancelled，且不再响应后续 end', async () => {
  const { ctx, bus, tools } = fakeHost();
  plugin.apply(ctx);
  const tool = tools[0];
  const agent = fakeAgent({ bus });
  bus.emit('subagent/start', { id: 'child', runId: 'r1' });
  const controller = new AbortController();

  const pending = tool.execute({ agent_id: 'child' }, { agent, signal: controller.signal });
  await until(() => bus.count('agent/inbox/inserted') === 1, 'wait listeners');
  controller.abort(new Error('user stop'));

  const value = await pending;
  assert.equal(value.status, 'cancelled');
  assert.equal(bus.count('agent/inbox/inserted'), 0, 'wait 的监听必须在取消时清掉');
});

test('★ timeout_seconds 越界：errored result（不让模型用 0 / 负数 / 天文数字）', async () => {
  const { ctx, bus, tools } = fakeHost();
  plugin.apply(ctx);
  const tool = tools[0];
  const agent = fakeAgent({ bus });

  await assert.rejects(
    () => tool.execute({ agent_id: 'child', timeout_seconds: 0 }, { agent, signal: new AbortController().signal }),
    /1 through 3600/,
  );
  await assert.rejects(
    () => tool.execute({ agent_id: 'child', timeout_seconds: 3601 }, { agent, signal: new AbortController().signal }),
    /1 through 3600/,
  );
});
