/** @env pure @kind contract @why wait_subagent 插件接线/模型可见结果坏了 */
/**
 * plugins/wait-subagent/test/plugin.test.mjs —— 插件加载与工具契约
 *
 * 用真实的 `defineTool`（DSH 的普通依赖）验证：
 *   · `apply` 只注册一个 `wait_subagent`，参数/输出 schema 能被吃下；
 *   · `execute` 在 live / 已结束 / 未运行 / 用户插话 / 取消 / 非法 child 下的返回；
 *   · `mode` 的两种口径（any_of 默认 / all_of）与多 child 的聚合结果；
 *   · 模型看到的 render 文本是**可执行**的（不是内部状态名）。
 *
 * 不测真实 agent loop —— 那要起完整 DSH；事件口径由 `wait.test.mjs` 的竞态用例守。
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';

import { validateJsonSchemaValue } from '@deepseek-ai/dsh-tools';

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

/** 让已经 settle 的 promise 跑到（用于断言“还没返回”）。 */
async function flush() {
  await new Promise((resolve) => setImmediate(resolve));
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

/** 装好插件并取出工具定义；每个 execute 的返回值都顺手过一遍 output schema 校验。 */
function loadTool(options) {
  const host = fakeHost(options);
  plugin.apply(host.ctx);
  const tool = host.tools[0];
  const execute = tool.execute;
  tool.execute = async (args, exec) => {
    const value = await execute(args, exec);
    assertMatchesOutputSchema(tool, value);
    return value;
  };
  return { ...host, tool };
}

/**
 * 真实 registry 会用 `validateJsonSchemaValue` 校验工具返回值；这里提前跑一遍，
 * 让“值 ↔ 声明的 output schema 漂了”当场变红，而不是等真机调用时 ToolOutputError。
 * @param {object} tool - 工具定义。
 * @param {object} value - 一次 execute 的返回值。
 */
function assertMatchesOutputSchema(tool, value) {
  const violations = validateJsonSchemaValue(tool.output.schema, value, 'value');
  assert.deepEqual(violations, [], `返回值与声明的 output schema 不一致：${violations.join('; ')}`);
}

test('★ 插件自我声明：只注册一个 `wait_subagent`，schema 能被 defineTool 吃下', () => {
  assert.equal(plugin.name, 'amayui-wait-subagent');
  assert.deepEqual(plugin.inject, ['tools']);
  assert.equal(typeof plugin.apply, 'function');

  const { tools, tool } = loadTool();
  assert.equal(tools.length, 1);
  assert.equal(tool.name, 'wait_subagent');

  const params = tool.parameters.properties;
  assert.equal(params.agent_id.type, 'string', '单 child 的写法必须继续可用');
  assert.equal(params.agent_ids.type, 'array');
  assert.equal(params.agent_ids.items.type, 'string');
  assert.deepEqual(params.mode.enum, ['any_of', 'all_of']);
  assert.equal(params.mode.default, 'any_of', '★ 默认 any_of：先有结果就先返回');
  assert.equal(params.timeout_seconds.type, 'integer');

  const output = tool.output.schema.properties;
  assert.equal(output.status.enum.includes('user_interjected'), true);
  assert.equal(output.mode.enum.includes('any_of'), true);
  assert.deepEqual(output.settled.items.properties.agent_id.type, 'string');
  assert.equal(typeof tool.output.render, 'function');
  assert.equal(tool.isConcurrencySafe({ agent_ids: ['child'] }), true);
});

test('★ 单个 child：等 end，返回 finished + “继续下一步”的指令', async () => {
  const { tool, bus } = loadTool();
  const agent = fakeAgent({ bus });
  bus.emit('subagent/start', { id: 'child', runId: 'r1' });

  const pending = tool.execute({ agent_id: 'child' }, { agent, signal: new AbortController().signal });
  await until(() => bus.count('agent/inbox/inserted') === 1, 'wait listeners');
  bus.emit('subagent/end', { id: 'child', runId: 'r1', stopReason: 'completed' });

  const value = await pending;
  assert.equal(value.status, 'finished');
  assert.equal(value.mode, 'any_of');
  assert.deepEqual(value.agent_ids, ['child']);
  assert.deepEqual(value.settled, [{ agent_id: 'child', stop_reason: 'completed' }]);
  assert.deepEqual(value.pending, []);
  assert.match(value.note, /Continue with your next step/);
  assert.equal(tool.output.render(undefined, value)[0].type, 'text');
});

test('★ any_of（默认）：两个 child 谁先结束就返回，另一个如实留在 pending', async () => {
  const { tool, bus } = loadTool({ children: [{ id: 'a' }, { id: 'b' }] });
  const agent = fakeAgent({ bus });
  bus.emit('subagent/start', { id: 'a', runId: 'r1' });
  bus.emit('subagent/start', { id: 'b', runId: 'r2' });

  const pending = tool.execute({ agent_ids: ['a', 'b'] }, { agent, signal: new AbortController().signal });
  await until(() => bus.count('agent/inbox/inserted') === 1, 'wait listeners');
  bus.emit('subagent/end', { id: 'b', runId: 'r2', stopReason: 'completed' });

  const value = await pending;
  assert.equal(value.status, 'finished');
  assert.deepEqual(value.settled.map((entry) => entry.agent_id), ['b']);
  assert.deepEqual(value.pending, ['a'], 'any_of 不许暗中等最慢的那个');
  assert.match(value.note, /b \(completed\) settled first/);
  assert.match(value.note, /a is still running/);
  assert.match(value.note, /Do not sleep to wait for it/);
});

test('★ all_of：第一个 end 不返回，最后一个 end 才返回（note 给全部 stop_reason）', async () => {
  const { tool, bus } = loadTool({ children: [{ id: 'a' }, { id: 'b' }] });
  const agent = fakeAgent({ bus });
  bus.emit('subagent/start', { id: 'a', runId: 'r1' });
  bus.emit('subagent/start', { id: 'b', runId: 'r2' });

  let resolved = false;
  const pending = tool
    .execute({ agent_ids: ['a', 'b'], mode: 'all_of' }, { agent, signal: new AbortController().signal })
    .then((value) => {
      resolved = true;
      return value;
    });
  await until(() => bus.count('agent/inbox/inserted') === 1, 'wait listeners');

  bus.emit('subagent/end', { id: 'a', runId: 'r1', stopReason: 'completed' });
  await flush();
  assert.equal(resolved, false, 'all_of 要等每一个');

  bus.emit('subagent/end', { id: 'b', runId: 'r2', stopReason: 'refusal' });
  const value = await pending;
  assert.equal(value.status, 'finished');
  assert.equal(value.mode, 'all_of');
  assert.deepEqual(value.settled, [
    { agent_id: 'a', stop_reason: 'completed' },
    { agent_id: 'b', stop_reason: 'refusal' },
  ]);
  assert.match(value.note, /All 2 waited-for subagents settled/);
});

test('★ any_of：调用前已有一个结束 ⇒ 立即 already_finished，不再等另一个', async () => {
  const { tool, bus } = loadTool({ children: [{ id: 'a' }, { id: 'b' }] });
  const agent = fakeAgent({ bus });
  bus.emit('subagent/start', { id: 'a', runId: 'r1' });
  bus.emit('subagent/end', { id: 'a', runId: 'r1', stopReason: 'completed' });
  bus.emit('subagent/start', { id: 'b', runId: 'r2' });

  const value = await tool.execute({ agent_ids: ['a', 'b'] }, { agent, signal: new AbortController().signal });
  assert.equal(value.status, 'already_finished');
  assert.deepEqual(value.settled.map((entry) => entry.agent_id), ['a']);
  assert.deepEqual(value.pending, ['b']);
  assert.match(value.note, /had already settled before this call/);
  assert.match(value.note, /b is still running/);
  assert.equal(bus.count('agent/inbox/inserted'), 0, 'already_finished 不该挂等待监听');
});

test('★ all_of：全部早已结束 ⇒ already_finished，不挂等待', async () => {
  const { tool, bus } = loadTool({ children: [{ id: 'a' }, { id: 'b' }] });
  const agent = fakeAgent({ bus });
  bus.emit('subagent/start', { id: 'a', runId: 'r1' });
  bus.emit('subagent/end', { id: 'a', runId: 'r1', stopReason: 'completed' });
  bus.emit('subagent/start', { id: 'b', runId: 'r2' });
  bus.emit('subagent/end', { id: 'b', runId: 'r2', stopReason: 'error' });

  const value = await tool.execute({ agent_ids: ['a', 'b'], mode: 'all_of' }, { agent, signal: new AbortController().signal });
  assert.equal(value.status, 'already_finished');
  assert.deepEqual(value.settled, [
    { agent_id: 'a', stop_reason: 'completed' },
    { agent_id: 'b', stop_reason: 'error' },
  ]);
  assert.deepEqual(value.pending, []);
  assert.equal(bus.count('agent/inbox/inserted'), 0);
});

test('★ 没有 live activation：立即 not_running（absent 列出来），不轮询', async () => {
  const { tool, bus } = loadTool();
  const agent = fakeAgent({ bus });

  const value = await tool.execute({ agent_id: 'child' }, { agent, signal: new AbortController().signal });
  assert.equal(value.status, 'not_running');
  assert.deepEqual(value.absent, ['child']);
  assert.match(value.note, /no live activation/);
});

test('★ all_of 里有 child 没有 live activation：等可等的那些，absent 如实交回', async () => {
  const { tool, bus } = loadTool({ children: [{ id: 'a' }, { id: 'b' }] });
  const agent = fakeAgent({ bus });
  bus.emit('subagent/start', { id: 'a', runId: 'r1' });

  const pending = tool.execute({ agent_ids: ['a', 'b'], mode: 'all_of' }, { agent, signal: new AbortController().signal });
  await until(() => bus.count('agent/inbox/inserted') === 1, 'wait listeners');
  bus.emit('subagent/end', { id: 'a', runId: 'r1', stopReason: 'completed' });

  const value = await pending;
  assert.equal(value.status, 'finished');
  assert.deepEqual(value.settled.map((entry) => entry.agent_id), ['a']);
  assert.deepEqual(value.absent, ['b'], '等不到的东西要明说，不能假装它 settled');
  assert.match(value.note, /b has no live activation/);
});

test('★ 用户 queue 插话：返回 user_interjected + delivery=queue + 让模型结束 turn', async () => {
  const { tool, bus } = loadTool();
  const agent = fakeAgent({ bus });
  bus.emit('subagent/start', { id: 'child', runId: 'r1' });

  let concluded = false;
  const pending = tool.execute(
    { agent_id: 'child' },
    { agent, signal: new AbortController().signal, concludeTurn: () => { concluded = true; } },
  );
  await until(() => bus.count('agent/inbox/inserted') === 1, 'wait listeners');

  const message = humanMessage();
  agent.inbox.nextTurn.push(message);
  bus.emit('agent/inbox/inserted', { message });

  const value = await pending;
  assert.equal(value.status, 'user_interjected');
  assert.equal(value.delivery, 'queue');
  assert.equal(value.message_preview, 'stop waiting');
  assert.deepEqual(value.pending, ['child']);
  assert.match(value.note, /closes the turn/);
  assert.equal(concluded, true, 'queue 必须标 concludesTurn，loop 才会自动关 turn');
});

test('★ 用户 steer 插话：不标 concludesTurn，消息在下一步处理', async () => {
  const { tool, bus } = loadTool();
  const agent = fakeAgent({ bus });
  bus.emit('subagent/start', { id: 'child', runId: 'r1' });

  let concluded = false;
  const pending = tool.execute(
    { agent_id: 'child' },
    { agent, signal: new AbortController().signal, concludeTurn: () => { concluded = true; } },
  );
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
  const { tool, ctx, bus } = loadTool();
  const agent = fakeAgent({ bus });
  bus.emit('subagent/start', { id: 'child', runId: 'r1' });

  let release;
  const gate = new Promise((resolve) => { release = resolve; });
  ctx.get('subagents').listChildren = async () => {
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

test('★ 待处理的人消息：在调用前就入队 ⇒ 立即 user_interjected（优先于等待）', async () => {
  const { tool, bus } = loadTool();
  const message = humanMessage('m0', 'look at this');
  const agent = fakeAgent({ bus, nextStep: [message] });

  const value = await tool.execute({ agent_id: 'child' }, { agent, signal: new AbortController().signal });
  assert.equal(value.status, 'user_interjected');
  assert.equal(value.delivery, 'steer');
});

test('★ 非法 child：不是直接子代理 ⇒ errored result，不去等一个陌生 session', async () => {
  const { tool, bus } = loadTool({ children: [] });
  const agent = fakeAgent({ bus });

  await assert.rejects(
    () => tool.execute({ agent_id: 'stranger' }, { agent, signal: new AbortController().signal }),
    /not a direct child/,
  );
});

test('★ 多个 child 里混进陌生人：报错要点名是哪一个', async () => {
  const { tool, bus } = loadTool({ children: [{ id: 'a' }] });
  const agent = fakeAgent({ bus });

  await assert.rejects(
    () => tool.execute({ agent_ids: ['a', 'stranger'] }, { agent, signal: new AbortController().signal }),
    /"stranger" is not a direct child/,
  );
});

test('★ 参数校验：缺 id / 等自己 / id 数量上限', async () => {
  const { tool, bus } = loadTool();
  const agent = fakeAgent({ bus });
  const signal = new AbortController().signal;

  await assert.rejects(() => tool.execute({}, { agent, signal }), /needs `agent_id`/);
  await assert.rejects(() => tool.execute({ agent_ids: [] }, { agent, signal }), /needs `agent_id`/);
  await assert.rejects(() => tool.execute({ agent_id: 'parent' }, { agent, signal }), /cannot wait for the calling agent itself/);
  await assert.rejects(
    () => tool.execute({ agent_ids: Array.from({ length: 33 }, (_, index) => `c${index}`) }, { agent, signal }),
    /at most 32 children/,
  );
});

test('★ 调用前已取消：立即 cancelled', async () => {
  const { tool, bus } = loadTool();
  const agent = fakeAgent({ bus });
  const controller = new AbortController();
  controller.abort(new Error('stop'));

  const value = await tool.execute({ agent_id: 'child' }, { agent, signal: controller.signal });
  assert.equal(value.status, 'cancelled');
});

test('★ 等待中取消：cancelled，且不再响应后续 end', async () => {
  const { tool, bus } = loadTool();
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
  const { tool, bus } = loadTool();
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
