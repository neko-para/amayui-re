/** @env pure @kind contract @suite toolchain @why wait_job 的"结算带正文 / 打断不碰游标 / 不泄漏监听器"被破坏 */
/**
 * plugins/wait-job/test/wait-job.test.mjs —— 等待逻辑的契约
 *
 * 全部用 fake registry（不依赖真宿主、不 import 任何 dsh 包）——测的是
 * "什么时候返回、返回什么、返回后还剩下什么"：
 *   · **结算 ⇒ 正文必须非空**（完成通知只带一行"去 job_output 读"的指针、从不带正文；
 *     `awaited:true` 吃掉的正是那一行 ⇒ 正文的唯一通路是本工具的 `read`）；
 *   · 人插话 / turn 取消 ⇒ `interrupted`、**不 read**、`wait` 的 signal 真的被 abort；
 *   · `timeout_seconds` 到期 ⇒ `timeout`、**不 read**；
 *   · `wake_on:'output'` ⇒ 有 output 事件就返回，且**没有**登记 `wait`、不推进游标；
 *   · 不存在的 job id ⇒ `no-such-job`、**不抛**、文案指向 `job_list`；
 *   · 块间竞态（块到期之后才结算）⇒ 仍是 `settled` 且正文非空；
 *   · 返回后**监听器归零 / timer 已清**。
 *
 * 不测真实 job 注册表与真实 agent loop —— 那要起完整 DSH（见 README 的"未验证"一节）。
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';

import {
  AWAKE,
  MAX_WAIT_MS,
  WAKE_ON,
  inboxDelivery,
  interruptionOf,
  isHumanMessage,
  isTerminalStatus,
  pendingHumanMessages,
  renderDelta,
  renderValue,
  shouldConcludeTurn,
  waitForJob,
} from '../lib/wait-job.mjs';

/** 让已经 settle 的 promise 跑到（用于断言"还没返回"）。 */
async function flush() {
  await new Promise((resolve) => setImmediate(resolve));
}

/** 一条人发的 inbox 消息。 */
function humanMessage(id = 'm1', text = 'stop waiting and do X') {
  return { id, source: { kind: 'user' }, content: [{ type: 'text', text }] };
}

/** 一条**非**人类来源的 inbox 消息（完成通知就走这条路）。 */
function noticeMessage(id = 'n1', kind = 'tool-jobs') {
  return { id, source: { kind, form: 'notice', summary: 'job finished' }, content: [{ type: 'text', text: 'done' }] };
}

/** 一个 JobView 投影。 */
function jobView({
  id = 'bash-1',
  status = 'running',
  total = 0,
  progress,
  detail,
  kind = 'bash',
  label = 'sleep 10',
} = {}) {
  return {
    id,
    kind,
    label,
    status,
    ...(progress === undefined ? {} : { progress }),
    ...(detail === undefined ? {} : { detail }),
    startedAt: 0,
    ...(isTerminalStatus(status) ? { finishedAt: 1 } : {}),
    output: { total, earliest: 0 },
  };
}

/** 一个一次性消费型读（`JobRead`）。 */
function jobRead({ text = '', channel = 'stdout', result, lossy = false } = {}) {
  return {
    chunks: text.length === 0 ? [] : [{ at: 0, text, ...(channel === undefined ? {} : { channel }) }],
    lossy,
    ...(result === undefined ? {} : { result }),
    job: jobView({ status: 'completed' }),
  };
}

/**
 * 假 registry：只实现本插件用到的那五个成员，并把每次调用记下来。
 * 没给的成员一被调用就抛 —— 于是"**不 read**"这种断言是硬失败的。
 */
function fakeRegistry({ get, wait, read, readAt, events } = {}) {
  const calls = { get: [], read: [], readAt: [], wait: [] };
  const subscribers = new Set();
  const subscribeFilters = [];
  const registry = {
    calls,
    subscribeFilters,
    subscriberCount: () => subscribers.size,
    get(jobId, caller) {
      calls.get.push({ jobId, caller });
      if (get === undefined) throw new Error('fake registry: get() was not expected');
      return get(jobId, caller);
    },
    wait(jobId, timeoutMs, caller, signal) {
      calls.wait.push({ jobId, timeoutMs, caller, signal });
      if (wait === undefined) throw new Error('fake registry: wait() was not expected');
      return wait(jobId, timeoutMs, caller, signal);
    },
    read(jobId, caller) {
      calls.read.push({ jobId, caller });
      if (read === undefined) throw new Error('fake registry: read() was not expected');
      return read(jobId, caller);
    },
    readAt(jobId, from, caller) {
      calls.readAt.push({ jobId, from, caller });
      if (readAt === undefined) throw new Error('fake registry: readAt() was not expected');
      return readAt(jobId, from, caller);
    },
    events: {
      subscribe(filter, listener) {
        subscribeFilters.push(filter);
        subscribers.add(listener);
        return () => subscribers.delete(listener);
      },
      emit(event) {
        for (const listener of [...subscribers]) listener(event);
      },
    },
  };
  return registry;
}

/** 一个「人插话源」：能查 pending、能订阅，并记得现在还有几个监听器。 */
function fakeInterruption({ pending = 0 } = {}) {
  const listeners = new Set();
  return {
    pendingCount: typeof pending === 'number' ? pending : 0,
    pending() {
      return typeof pending === 'function' ? pending() : pending;
    },
    subscribe(listener) {
      listeners.add(listener);
      return () => listeners.delete(listener);
    },
    count: () => listeners.size,
    fire() {
      for (const listener of [...listeners]) listener();
    },
  };
}

/** 一个能模拟 turn 取消的 signal（只实现本插件用到的三个成员）。 */
function fakeSignal() {
  const listeners = new Set();
  return {
    aborted: false,
    added: 0,
    removed: 0,
    addEventListener(_type, listener) {
      this.added += 1;
      listeners.add(listener);
    },
    removeEventListener(_type, listener) {
      this.removed += 1;
      listeners.delete(listener);
    },
    abort() {
      this.aborted = true;
      for (const listener of [...listeners]) listener();
    },
    count: () => listeners.size,
  };
}

/** 一个能注入的 timer：记下 armed / cleared，并能手动触发。 */
function fakeTimers() {
  const armed = [];
  let cleared = 0;
  return {
    armed,
    cleared: () => cleared,
    setTimeout(callback, ms) {
      const handle = { callback, ms };
      armed.push(handle);
      return handle;
    },
    clearTimeout(handle) {
      cleared += 1;
      const index = armed.indexOf(handle);
      if (index >= 0) armed.splice(index, 1);
    },
    fire() {
      const [handle] = armed.splice(0);
      assert.notEqual(handle, undefined, 'fake timer: nothing armed');
      handle.callback();
    },
  };
}

/** `timeout_seconds` 是绝对 deadline：第一块会略小于请求值（已过去的那几毫秒）。 */
function assertTimeoutNear(actual, expected) {
  assert.ok(actual > expected - 1000 && actual <= expected, `expected ~${expected}ms, got ${actual}ms`);
}

/**
 * `dsh-jobs-local` 的 wait：结算前一直被 signal 打断（reject）。
 *
 * ★ 这是一条**变异守卫**：如果实现忘了 abort 我们这个私有 signal，这个 promise 就永远
 *   不会 settle ⇒ 用例会"挂住"而不是变红；而 `tools/test-run.mjs:228` 起的
 *   `node --test` **不带** `--test-timeout` ⇒ `pnpm verify` 会永久卡住（不是红）。
 *   所以 `graceMs` 之后**主动** reject：让"忘了 abort"当场变成一条明确的失败
 *   （用例随后对 `signal.aborted` / `count()` 的断言），而不是等外部超时。
 *
 * ★ `graceMs` 写死 **5000**，⛔ 不读 env 覆盖（多一个开关 = 多一个"本地绿、CI 红"的分叉）：
 *   这个兜底的用途是**防挂死、不是测速度**。它是"fake wait 注册 → `interruption.fire()`"
 *   之间的**墙钟赌注**：实测正常窗口 < 1 ms（余量 ~5000×），但 GC / CPU 饥饿 / CI 重载下
 *   若停顿超过它，就会**误红**，而且文案会栽赃给实现。最坏代价 = 2 条用例 × 5 s = 10 s，
 *   仍远小于任何门禁超时。
 * @param {{graceMs?: number}} [options] - 宽限毫秒数（缺省 5000，见上）。
 * @returns {(jobId: string, timeoutMs: number, caller: unknown, signal: object) => Promise<object>} 假 wait。
 */
function waitAbortedBySignal({ graceMs = 5000 } = {}) {
  return (jobId, timeoutMs, caller, signal) =>
    new Promise((resolve, reject) => {
      const giveUp = setTimeout(() => {
        reject(
          new Error(
            `fake wait: not aborted within ${graceMs}ms — the implementation forgot to abort its own wait signal`,
          ),
        );
      }, graceMs);
      const onAbort = () => {
        clearTimeout(giveUp);
        reject(new Error('wait aborted'));
      };
      if (signal.aborted) {
        clearTimeout(giveUp);
        reject(new Error('wait aborted'));
        return;
      }
      signal.addEventListener('abort', onAbort, { once: true });
    });
}

// ─────────────────────────────── 不变量 ①：结算带正文 ───────────────────────────────

test('★ settle 路径：结算 ⇒ awake 是 settled 且正文非空（通知只带指针，且被 awaited 吃掉）', async () => {
  const registry = fakeRegistry({
    get: () => jobView({ status: 'running', total: 0 }),
    wait: async () => jobView({ status: 'completed', total: 42, detail: 'exit code 0' }),
    read: () => jobRead({ text: 'hello from the job\n', result: 'exit code 0' }),
  });

  const value = await waitForJob({ jobId: 'bash-1', jobs: registry, caller: 's1', timeoutMs: 30_000 });

  // ★ 全文件**唯一**一处字面量（其余用 `AWAKE.*`，那是自指：把枚举改坏也不红）。
  //   契约值必须被钉死在这里 —— 守卫只认"用例能红"，认不出自指。
  assert.equal(value.awake, 'settled');
  assert.equal(value.status, 'completed');
  assert.equal(value.bytes, 42);
  // ★ 这条断言就是本单最重要的不变量：正文必须由这次结果带回。
  //   理由：完成通知只带一行"去 job_output 读"的指针、从不带正文；而这次结算被
  //   `awaited:true` 吃掉 —— 吃掉的正是那一行指针 ⇒ 不 read 就彻底没有正文。
  assert.equal(typeof value.output, 'string');
  assert.ok(value.output.length > 0, 'settled 的 tool 结果必须带正文（通知只带指针且已被 awaited 吃掉）');
  assert.match(value.output, /hello from the job/);
  assert.match(value.output, /exit code 0/);
  assert.equal(registry.calls.read.length, 1, 'settle 路径必须 read 一次把正文拿出来');
  assert.deepEqual(registry.calls.wait[0].signal instanceof AbortSignal, true, 'wait 必须拿到我们自己的 signal');
  assertTimeoutNear(registry.calls.wait[0].timeoutMs, 30_000);
});

test('settle 路径：已经结算的 job 不必 wait，正文照样带回', async () => {
  const registry = fakeRegistry({
    get: () => jobView({ status: 'completed', total: 7, detail: 'exit code 0' }),
    read: () => jobRead({ text: 'already done' }),
  });

  const value = await waitForJob({ jobId: 'bash-1', jobs: registry, caller: 's1' });

  assert.equal(value.awake, AWAKE.SETTLED);
  assert.match(value.output, /already done/);
  assert.equal(registry.calls.wait.length, 0, '已经结算就不该再登记 wait');
});

test('settle 路径：include_output:false ⇒ 不 read，并明确让模型自己 job_output', async () => {
  const registry = fakeRegistry({
    get: () => jobView({ status: 'running' }),
    wait: async () => jobView({ status: 'completed', total: 3 }),
  });

  const value = await waitForJob({ jobId: 'bash-1', jobs: registry, caller: 's1', includeOutput: false });

  assert.equal(value.awake, AWAKE.SETTLED);
  assert.equal(value.output, undefined);
  assert.equal(registry.calls.read.length, 0);
  assert.match(value.note, /job_output/);
});

test('settle 路径：块到期只说明"这一段等满了"，不能当成 timeout', async () => {
  let round = 0;
  const registry = fakeRegistry({
    get: () => jobView({ status: 'running', progress: '1/3' }),
    wait: async () => {
      round += 1;
      // 第一次"提前 resolve"一个仍活着的投影（模拟块到期），第二次才结算。
      return round === 1 ? jobView({ status: 'running', total: 12 }) : jobView({ status: 'completed', total: 20 });
    },
    read: () => jobRead({ text: 'late output' }),
  });

  const value = await waitForJob({ jobId: 'bash-1', jobs: registry, caller: 's1', timeoutMs: 60_000 });

  assert.equal(value.awake, AWAKE.SETTLED);
  assert.match(value.output, /late output/);
  assert.equal(registry.calls.wait.length, 2, '第一块没到 deadline ⇒ 必须继续等');
  assertTimeoutNear(registry.calls.wait[0].timeoutMs, 60_000);
});

test('★ 块间竞态：块到期之后才结算 ⇒ 仍是 awake=settled 且正文非空', async () => {
  // 缺省「不设超时」是切块重等 ⇒ 结算可能恰好落在两块之间。真实的 `waitJob` 对
  // **已终局**的 job 会立刻返回终局投影（不再登记 waiter），所以下一块必须拿到它。
  // 断言的重点：这种竞态下**正文不能丢**（最坏只丢一行状态行）。
  let view = jobView({ status: 'running', total: 5, progress: '1/2' });
  let round = 0;
  const registry = fakeRegistry({
    get: () => view,
    wait: async () => {
      round += 1;
      if (round === 1) return view; // 第一块到期：任务仍活着
      // 两块之间 job 结算；下一块的 wait 立刻拿到终局。
      view = jobView({ status: 'completed', total: 33, detail: 'exit code 0' });
      return view;
    },
    read: () => jobRead({ text: 'output written while no chunk was waiting\n', result: 'exit code 0' }),
  });

  const value = await waitForJob({ jobId: 'bash-1', jobs: registry, caller: 's1' });

  assert.equal(value.awake, AWAKE.SETTLED);
  assert.equal(value.status, 'completed');
  assert.equal(value.bytes, 33);
  assert.equal(typeof value.output, 'string');
  assert.ok(value.output.length > 0, '块间竞态下正文也必须由本次结果带回');
  assert.match(value.output, /while no chunk was waiting/);
  assert.equal(registry.calls.wait.length, 2, '第一块之后必须再等一块（不能停在 live 投影上）');
  assert.equal(registry.calls.read.length, 1);
});

test('settle 路径：缺省不设超时会按上限切块（不是 Infinity）', async () => {
  let round = 0;
  const registry = fakeRegistry({
    get: () => jobView({ status: 'running' }),
    wait: async () => {
      round += 1;
      return round === 1 ? jobView({ status: 'running' }) : jobView({ status: 'completed', total: 1 });
    },
    read: () => jobRead({ text: 'x' }),
  });

  const value = await waitForJob({ jobId: 'bash-1', jobs: registry, caller: 's1' });

  assert.equal(value.awake, AWAKE.SETTLED);
  assert.equal(registry.calls.wait[0].timeoutMs, MAX_WAIT_MS, '不设超时 = 按 Node/`deadline()` 上限切块');
  // ★ 上限是**外部契约**、不是本模块的私有选择：`dsh-timeout/lib/index.js:27-29` 的
  //   `assertTimerDelay` 拒绝 > 2147483647 的值（Node 定时器上限）。MAX_WAIT_MS 若漂大，
  //   「缺省不设超时」的每一块都会被宿主直接拒掉 ⇒ 必须钉住这条不等式（不能只断言"等于自己"）。
  assert.ok(MAX_WAIT_MS <= 2147483647, `MAX_WAIT_MS must not exceed the host timer ceiling, got ${MAX_WAIT_MS}`);
  assert.ok(MAX_WAIT_MS > 0);
});

// ─────────────────────────── 不变量 ②：打断 / timeout 不碰游标 ───────────────────────────

test('★ 人插话打断：awake=interrupted、不 read、wait 的 signal 真的被 abort', async () => {
  const interruption = fakeInterruption();
  const registry = fakeRegistry({
    get: () => jobView({ status: 'running', total: 42, progress: '2/5' }),
    wait: waitAbortedBySignal(),
  });

  const pending = waitForJob({
    jobId: 'bash-1',
    jobs: registry,
    caller: 's1',
    timeoutMs: 30_000,
    interruption,
  });
  await flush();
  assert.equal(interruption.count(), 1, '等待期间必须挂着人插话监听');
  interruption.fire();

  const value = await pending;

  assert.equal(value.awake, AWAKE.INTERRUPTED);
  assert.equal(value.status, 'running');
  assert.equal(value.bytes, 42);
  assert.equal(value.progress, '2/5');
  // ★ 因打断返回时**不碰模型游标**：read 一次都不许调。
  assert.equal(registry.calls.read.length, 0, '打断返回时不许 jobs.read（游标留给之后那条完成通知）');
  assert.equal(registry.calls.readAt.length, 0);
  // ★ abort 真的发生了：我们传给 wait 的 signal 被 abort。
  assert.equal(registry.calls.wait.length, 1);
  assert.equal(registry.calls.wait[0].signal.aborted, true, '人插话必须 abort 掉那个私有的 wait signal');
  // 泄漏断言：监听器归零。
  assert.equal(interruption.count(), 0, '返回后不许留下人插话监听器');
});

test('★ turn 取消：同样 interrupted、不 read、abort 真的发生，且 signal 监听已撤', async () => {
  const signal = fakeSignal();
  const registry = fakeRegistry({
    get: () => jobView({ status: 'running', total: 9 }),
    wait: waitAbortedBySignal(),
  });

  const pending = waitForJob({ jobId: 'bash-1', jobs: registry, caller: 's1', signal, interruption: fakeInterruption() });
  await flush();
  assert.equal(signal.count(), 1);
  signal.abort();

  const value = await pending;

  assert.equal(value.awake, AWAKE.INTERRUPTED);
  assert.equal(value.status, 'running');
  assert.equal(registry.calls.read.length, 0, 'turn 取消返回时不许 jobs.read');
  assert.equal(registry.calls.wait[0].signal.aborted, true, 'turn 取消必须 abort 掉那个私有的 wait signal');
  assert.equal(signal.count(), 0, '返回后必须 removeEventListener');
  assert.equal(signal.added, signal.removed, 'add/remove 必须配平');
});

test('已经取消的 signal：进等待前就返回，不登记 wait', async () => {
  const signal = fakeSignal();
  signal.aborted = true;
  const registry = fakeRegistry({ get: () => jobView({ status: 'running', total: 5 }) });

  const value = await waitForJob({ jobId: 'bash-1', jobs: registry, caller: 's1', signal });

  assert.equal(value.awake, AWAKE.INTERRUPTED);
  assert.equal(value.bytes, 5);
  assert.equal(registry.calls.wait.length, 0);
  assert.equal(registry.calls.read.length, 0);
});

test('已经有 pending 的用户消息：进等待前就返回 interrupted', async () => {
  const interruption = fakeInterruption({ pending: 1 });
  const registry = fakeRegistry({ get: () => jobView({ status: 'running', total: 5 }) });

  const value = await waitForJob({ jobId: 'bash-1', jobs: registry, caller: 's1', interruption });

  assert.equal(value.awake, AWAKE.INTERRUPTED);
  assert.match(value.note, /user interjected/i);
  assert.equal(registry.calls.wait.length, 0);
  assert.equal(registry.calls.read.length, 0);
});

test('★ timeout_seconds 到期：awake=timeout、不 read', async () => {
  const registry = fakeRegistry({
    get: () => jobView({ status: 'running', total: 3, progress: '0/2' }),
    // 模拟实现：到期那一刻 resolve 一个仍活着的投影。
    wait: (_jobId, timeoutMs) => new Promise((resolve) => setTimeout(() => resolve(jobView({ status: 'running', total: 77 })), timeoutMs + 5)),
  });

  const value = await waitForJob({ jobId: 'bash-1', jobs: registry, caller: 's1', timeoutMs: 25 });

  assert.equal(value.awake, AWAKE.TIMEOUT);
  assert.equal(value.status, 'running');
  assert.equal(value.bytes, 77);
  assert.equal(registry.calls.read.length, 0, 'timeout 返回时不许 jobs.read');
  assert.match(value.note, /timed out after/);
});

// ───────────────────────── 不变量 ③：output 路径不持有 wait ─────────────────────────

test('★ wake_on:output：有 output 事件就返回，且**没有**登记 wait、不推进游标', async () => {
  const registry = fakeRegistry({
    get: () => jobView({ status: 'running', total: 100, progress: '3/9' }),
    readAt: () => ({ chunks: [{ at: 100, text: 'more' }], next: 104, lossy: false }),
  });

  const pending = waitForJob({
    jobId: 'bash-1',
    jobs: registry,
    caller: 's1',
    wakeOn: WAKE_ON.OUTPUT,
    timeoutMs: 30_000,
  });
  await flush();
  assert.equal(registry.subscriberCount(), 1, 'output 路径必须订阅事件');
  registry.events.emit({ type: 'output', id: 'bash-1', owner: 's1', total: 104 });

  const value = await pending;

  assert.equal(value.awake, AWAKE.OUTPUT);
  assert.equal(value.status, 'running');
  assert.equal(value.bytes, 104);
  // ★ 不持有 registry.wait：否则人插话时那次结算会被标 awaited（通知被吃）。
  assert.equal(registry.calls.wait.length, 0, 'output 路径⛔不许登记 wait');
  // ★ 不推进模型游标。
  assert.equal(registry.calls.read.length, 0, 'output 路径不许 jobs.read');
  assert.equal(registry.calls.readAt.length, 1);
  assert.equal(registry.calls.readAt[0].from, 100, 'readAt 必须从"本次调用开始时的字节数"起读');
  assert.match(value.note, /job_output/);
  assert.equal(registry.subscriberCount(), 0, '返回后不许留下事件监听器');
});

test('wake_on:output：readAt 的 next 没超过起点就不算"有新字节"（不返回）', async () => {
  let round = 0;
  const registry = fakeRegistry({
    get: () => jobView({ status: 'running', total: 10 }),
    readAt: () => {
      round += 1;
      // 第一发事件：total 说涨了，但 readAt.next 还是 10 ⇒ 没有新字节。
      return round === 1 ? { chunks: [], next: 10, lossy: false } : { chunks: [{ at: 10, text: 'x' }], next: 11, lossy: false };
    },
  });

  const pending = waitForJob({ jobId: 'bash-1', jobs: registry, caller: 's1', wakeOn: WAKE_ON.OUTPUT });
  await flush();
  registry.events.emit({ type: 'output', id: 'bash-1', total: 11 });
  await flush();
  assert.equal(registry.calls.readAt.length, 1);
  registry.events.emit({ type: 'output', id: 'bash-1', total: 11 });

  const value = await pending;

  assert.equal(value.awake, AWAKE.OUTPUT);
  assert.equal(value.bytes, 11);
  assert.equal(registry.calls.readAt.length, 2);
  assert.equal(registry.calls.wait.length, 0);
});

test('wake_on:output：别的 job 的 output 事件不会叫醒它', async () => {
  const registry = fakeRegistry({
    get: () => jobView({ status: 'running', total: 0 }),
    readAt: () => ({ chunks: [{ at: 0, text: 'x' }], next: 1, lossy: false }),
  });

  const pending = waitForJob({ jobId: 'bash-1', jobs: registry, caller: 's1', wakeOn: WAKE_ON.OUTPUT });
  await flush();
  registry.events.emit({ type: 'output', id: 'bash-2', owner: 's1', total: 999 });
  await flush();
  assert.equal(registry.calls.readAt.length, 0, '不是这个 job 的事件必须忽略');

  registry.events.emit({ type: 'output', id: 'bash-1', total: 1 });
  const value = await pending;
  assert.equal(value.awake, AWAKE.OUTPUT);
});

test('wake_on:output：只回"老"字节不算新输出（起点之后才算）', async () => {
  let round = 0;
  const registry = fakeRegistry({
    get: () => jobView({ status: 'running', total: 50 }),
    readAt: () => {
      round += 1;
      // 第 1 次：事件 total 涨了 1，但 readAt.next 仍是起点 ⇒ 没有新字节。
      // 第 2 次：确实涨到 52 ⇒ 返回。
      return round === 1 ? { chunks: [], next: 50, lossy: false } : { chunks: [{ at: 50, text: 'ab' }], next: 52, lossy: false };
    },
  });

  const pending = waitForJob({ jobId: 'bash-1', jobs: registry, caller: 's1', wakeOn: WAKE_ON.OUTPUT });
  await flush();
  // 事件 total 没超过起点 ⇒ 连 readAt 都不必调（快速闸门）。
  registry.events.emit({ type: 'output', id: 'bash-1', total: 50 });
  await flush();
  assert.equal(registry.calls.readAt.length, 0, '事件 total 没超过起点 ⇒ 连 readAt 都不必调');
  // 这一次 total 过了闸门，但 readAt 的 next 说明没有真新字节 ⇒ 继续等。
  registry.events.emit({ type: 'output', id: 'bash-1', total: 51 });
  await flush();
  assert.equal(registry.calls.readAt.length, 1);
  registry.events.emit({ type: 'output', id: 'bash-1', total: 52 });

  const value = await pending;
  assert.equal(value.awake, AWAKE.OUTPUT);
  assert.equal(value.bytes, 52);
  assert.equal(registry.calls.wait.length, 0);
  assert.equal(registry.calls.read.length, 0);
});

test('★ wake_on:output：timer 已被清（注入的 fake timer 归零）', async () => {
  const timers = fakeTimers();
  const registry = fakeRegistry({
    get: () => jobView({ status: 'running', total: 0 }),
    readAt: () => ({ chunks: [], next: 5, lossy: false }),
  });

  const pending = waitForJob({
    jobId: 'bash-1',
    jobs: registry,
    caller: 's1',
    wakeOn: WAKE_ON.OUTPUT,
    timeoutMs: 1_000,
    timers,
  });
  await flush();
  assert.equal(timers.armed.length, 1, '有 timeout_seconds 时必须挂一个 timer');
  registry.events.emit({ type: 'output', id: 'bash-1', total: 5 });

  const value = await pending;
  assert.equal(value.awake, AWAKE.OUTPUT);
  assert.equal(timers.armed.length, 0, '返回后 timer 必须被撤掉');
  assert.equal(timers.cleared(), 1);
});

test('wake_on:output：timeout 到期 ⇒ awake=timeout 且不读正文', async () => {
  const timers = fakeTimers();
  const registry = fakeRegistry({ get: () => jobView({ status: 'running', total: 2, progress: 'idle' }) });

  const pending = waitForJob({
    jobId: 'bash-1',
    jobs: registry,
    caller: 's1',
    wakeOn: WAKE_ON.OUTPUT,
    timeoutMs: 5_000,
    timers,
  });
  await flush();
  timers.fire();

  const value = await pending;
  assert.equal(value.awake, AWAKE.TIMEOUT);
  assert.equal(value.bytes, 2);
  assert.equal(registry.calls.read.length, 0);
  assert.equal(registry.calls.readAt.length, 0);
  assert.equal(registry.subscriberCount(), 0, 'timeout 返回后事件监听器必须归零');
});

test('wake_on:output：output 模式里 job 结算了也要返回（不挂死），且不读正文', async () => {
  const registry = fakeRegistry({ get: () => jobView({ status: 'running', total: 0 }) });

  const pending = waitForJob({ jobId: 'bash-1', jobs: registry, caller: 's1', wakeOn: WAKE_ON.OUTPUT });
  await flush();
  registry.events.emit({
    type: 'settled',
    job: jobView({ status: 'completed', total: 30 }),
    cause: 'producer',
    awaited: false,
  });

  const value = await pending;
  assert.equal(value.awake, AWAKE.SETTLED);
  assert.equal(value.bytes, 30);
  // output 路径从不读正文 ⇒ 这次结算没被本调用 awaited，通知照常到。
  assert.equal(registry.calls.read.length, 0);
  assert.equal(registry.calls.wait.length, 0);
  assert.match(value.note, /completion notice/i);
  assert.equal(registry.subscriberCount(), 0);
});

test('★ wake_on:output：人插话打断 ⇒ interrupted 且**不留监听器**（防泄露）', async () => {
  // 变异守卫：把 awaitOutput 里 `disposers.push(interruption.subscribe(...))` 的登记去掉，
  // 这两条（本条 + 下一条）必须红 —— 否则 output 路径会漏监听器而无人发现。
  const interruption = fakeInterruption();
  const registry = fakeRegistry({
    get: () => jobView({ status: 'running', total: 10, progress: '1/2' }),
  });

  const pending = waitForJob({
    jobId: 'bash-1',
    jobs: registry,
    caller: 's1',
    wakeOn: WAKE_ON.OUTPUT,
    timeoutMs: 30_000,
    interruption,
  });
  await flush();
  assert.equal(registry.subscriberCount(), 1, 'output 路径必须订阅事件');
  assert.equal(interruption.count(), 1, '等待期间必须挂着人插话监听');
  interruption.fire();

  const value = await pending;

  assert.equal(value.awake, AWAKE.INTERRUPTED);
  assert.equal(value.status, 'running');
  assert.equal(value.bytes, 10);
  assert.equal(value.progress, '1/2');
  assert.equal(registry.calls.read.length, 0, 'output 路径打断时不读正文');
  assert.equal(interruption.count(), 0, '★ 返回后不许留下人插话监听器（泄露 = bug）');
  assert.equal(registry.subscriberCount(), 0, '返回后事件监听器也必须归零');
});

test('★ wake_on:output：turn 取消 ⇒ interrupted 且 signal 的 add/remove 配平', async () => {
  const signal = fakeSignal();
  const registry = fakeRegistry({ get: () => jobView({ status: 'running', total: 4 }) });

  const pending = waitForJob({ jobId: 'bash-1', jobs: registry, caller: 's1', wakeOn: WAKE_ON.OUTPUT, signal });
  await flush();
  assert.equal(signal.count(), 1);
  signal.abort();

  const value = await pending;

  assert.equal(value.awake, AWAKE.INTERRUPTED);
  assert.equal(value.bytes, 4);
  assert.equal(registry.calls.read.length, 0);
  assert.equal(signal.count(), 0, '★ 返回后必须 removeEventListener');
  assert.equal(signal.added, signal.removed, 'add/remove 必须配平');
  assert.equal(registry.subscriberCount(), 0);
});

test('wake_on:output：订阅窗口里已经有新字节 ⇒ 立刻返回 output', async () => {
  let round = 0;
  const registry = fakeRegistry({
    // 第一次（预检）total 是 10；订阅后再 get 时已经 14（窗口里写进来了）。
    get: () => {
      round += 1;
      return jobView({ status: 'running', total: round === 1 ? 10 : 14 });
    },
    readAt: () => ({ chunks: [{ at: 10, text: 'abcd' }], next: 14, lossy: false }),
  });

  const value = await waitForJob({ jobId: 'bash-1', jobs: registry, caller: 's1', wakeOn: WAKE_ON.OUTPUT });

  assert.equal(value.awake, AWAKE.OUTPUT);
  assert.equal(value.bytes, 14);
  assert.equal(registry.calls.readAt[0].from, 10);
  assert.equal(registry.subscriberCount(), 0);
});

// ────────────────────────────────── 未知 job ──────────────────────────────────

test('★ 不存在的 job id：friendly 的 no-such-job，不抛，且文案指向 job_list', async () => {
  const registry = fakeRegistry({
    get: () => {
      throw new Error('unknown job bash-nope');
    },
  });

  const value = await waitForJob({ jobId: 'bash-nope', jobs: registry, caller: 's1', timeoutMs: 30_000 });

  assert.equal(value.awake, AWAKE.NO_SUCH_JOB);
  assert.equal(value.status, undefined);
  assert.equal(value.bytes, undefined);
  assert.match(value.note, /bash-nope/);
  assert.match(value.note, /job_list/);
  // ★ 打错 id 不能读起来像一次正常返回：文案必须说"什么也没等到"，并给出下一步。
  assert.match(value.note, /nothing was waited for/);
  assert.match(value.note, /typo|archived/);
  assert.equal(registry.calls.wait.length, 0);
  assert.equal(registry.calls.read.length, 0);
});

test('wait 失败且 get 也确认看不见 ⇒ no-such-job；否则如实抛', async () => {
  // 预检时还看得见，登记 wait 之后记录消失（被 remove / owner 被销毁）⇒ 降级成 no-such-job。
  let gets = 0;
  const vanishing = fakeRegistry({
    get: () => {
      gets += 1;
      if (gets === 1) return jobView({ status: 'running' });
      throw new Error('unknown job bash-1');
    },
    wait: async () => {
      throw new Error('unknown job bash-1');
    },
  });
  const value = await waitForJob({ jobId: 'bash-1', jobs: vanishing, caller: 's1' });
  assert.equal(value.awake, AWAKE.NO_SUCH_JOB);
  assert.equal(vanishing.calls.read.length, 0);

  // 别的失败（job 明明还在）⇒ 如实抛，不伪装成"没有这个 job"。
  const broken = fakeRegistry({
    get: () => jobView({ status: 'running' }),
    wait: async () => {
      throw new Error('registry blew up');
    },
  });
  await assert.rejects(() => waitForJob({ jobId: 'bash-1', jobs: broken, caller: 's1' }), /registry blew up/);
  assert.equal(broken.calls.read.length, 0);
});

// ────────────────────────── 人插话识别（完成通知不能误判） ──────────────────────────

test('★ 只有 source.kind=user 才算人插话；完成通知不许叫醒等待', async () => {
  assert.equal(isHumanMessage(humanMessage()), true);
  assert.equal(isHumanMessage(noticeMessage()), false, 'tool-jobs 的完成通知不是人插话');
  assert.equal(isHumanMessage(noticeMessage('n2', 'subagent-settled')), false);
  assert.equal(isHumanMessage(undefined), false);

  assert.equal(pendingHumanMessages({ inbox: { nextStep: [humanMessage()], nextTurn: [noticeMessage()] } }).length, 1);
  assert.equal(pendingHumanMessages({ inbox: { nextStep: [noticeMessage()], nextTurn: [] } }).length, 0);

  // `interruptionOf(agent)` 只把 kind=user 的插入当打断。
  const handlers = new Map();
  const agent = {
    id: 's1',
    inbox: { nextStep: [], nextTurn: [] },
    ctx: {
      on(event, listener) {
        handlers.set(event, listener);
        return () => handlers.delete(event);
      },
    },
  };
  const interruption = interruptionOf(agent);
  assert.notEqual(interruption, undefined);
  assert.equal(interruption.pending(), 0);

  let fired = 0;
  const dispose = interruption.subscribe(() => {
    fired += 1;
  });
  assert.equal(typeof handlers.get('agent/inbox/inserted'), 'function');

  handlers.get('agent/inbox/inserted')({ agent, message: noticeMessage() });
  assert.equal(fired, 0, '完成通知不能打断等待');
  handlers.get('agent/inbox/inserted')({ agent, message: humanMessage() });
  assert.equal(fired, 1);

  dispose();
  assert.equal(handlers.size, 0, 'disposer 必须真的撤掉订阅');
});

test('interruptionOf：没有 agent.ctx.on 时返回 undefined（退化但不抛）', () => {
  assert.equal(interruptionOf(undefined), undefined);
  assert.equal(interruptionOf({ id: 's1' }), undefined);
  assert.equal(interruptionOf({ id: 's1', ctx: {} }), undefined);
});

test('shouldConcludeTurn：只有队列型（next-turn）用户消息才收尾本 turn', () => {
  const message = humanMessage('m1');
  const notice = noticeMessage('n1');

  // steer（next-step）：下一步就能看见 ⇒ 不收尾。
  assert.equal(inboxDelivery({ inbox: { nextStep: [message], nextTurn: [] } }, message), 'steer');
  assert.equal(shouldConcludeTurn({ inbox: { nextStep: [message], nextTurn: [] } }), false);

  // queue（next-turn）：本 turn 里看不见 ⇒ 收尾，否则那条消息要等模型自己结束 turn。
  assert.equal(inboxDelivery({ inbox: { nextStep: [], nextTurn: [message] } }, message), 'queue');
  assert.equal(shouldConcludeTurn({ inbox: { nextStep: [], nextTurn: [message] } }), true);

  // 完成通知不算人插话 ⇒ 从不因此收尾。
  assert.equal(shouldConcludeTurn({ inbox: { nextStep: [], nextTurn: [notice] } }), false);
  // 空 inbox / 没有 inbox ⇒ 不收尾。
  assert.equal(shouldConcludeTurn({ inbox: { nextStep: [], nextTurn: [] } }), false);
  assert.equal(shouldConcludeTurn(undefined), false);
  // steer + queue 混着：有一条下一步可见就不收尾（让模型先在这一步回应）。
  assert.equal(shouldConcludeTurn({ inbox: { nextStep: [message], nextTurn: [humanMessage('m2')] } }), false);
});

// ────────────────────────────── 渲染 / 参数边界 ──────────────────────────────

test('renderDelta：log 不进模型，stderr 归一段，result 接在后面', () => {
  const text = renderDelta({
    chunks: [
      { at: 0, text: 'out1', channel: 'stdout' },
      { at: 4, text: 'narration', channel: 'log' },
      { at: 13, text: 'err1', channel: 'stderr' },
    ],
    lossy: false,
    result: 'exit code 0',
  });
  assert.equal(text, 'out1\n[stderr]\nerr1\nexit code 0');
  assert.ok(!text.includes('narration'));
});

test('renderValue：一行人话 + 关键字段（awake/status/bytes/progress）+ 正文', () => {
  const text = renderValue({
    awake: AWAKE.INTERRUPTED,
    status: 'running',
    bytes: 12,
    progress: '2/5',
    elapsed_seconds: 7,
    output: 'body text',
    note: 'The user interjected while waiting for job bash-1.',
  });
  const lines = text.split('\n');
  assert.equal(lines[0], 'The user interjected while waiting for job bash-1.');
  assert.match(lines[1], /interrupted/);
  assert.match(lines[1], /running/);
  assert.match(lines[1], /12 bytes/);
  assert.match(lines[1], /2\/5/);
  assert.match(lines[1], /7s/);
  assert.equal(lines[2], 'body text');
});

test('★ awake 闭集：5 个字面量一次钉全（模型可见契约，不是内部枚举）', () => {
  // 这 5 个值是**模型可见契约**（README §1 的出口表 / §4 的字段闭集，也就是
  // `index.mjs` 里 `output.schema.awake.enum` 的来源）：名字漂了就是对外行为漂。
  // 别处 24 处 `AWAKE.*` 是自指 —— 把枚举改坏也照样绿（实测：`INTERRUPTED` 改成
  // `'interrupted-x'` 时 29/29 全绿）。所以在这里用**字面量 + 顺序 + 个数**锚一次，
  // 全文件只此一处，⛔ 不要再散点 pin。
  assert.deepEqual(Object.values(AWAKE), ['settled', 'interrupted', 'timeout', 'output', 'no-such-job']);
});

test('参数边界：空 job id / 缺 registry / 非正 timeout 直接报错，不去碰 registry', async () => {
  await assert.rejects(() => waitForJob({ jobId: '', jobs: fakeRegistry() }), /non-empty job id/);
  await assert.rejects(() => waitForJob({ jobId: 'bash-1', jobs: {} }), /needs a job registry/);
  const registry = fakeRegistry({ get: () => jobView({ status: 'running' }) });
  await assert.rejects(() => waitForJob({ jobId: 'bash-1', jobs: registry, timeoutMs: 0 }), /positive timeout/);
  await assert.rejects(
    () => waitForJob({ jobId: 'bash-1', jobs: registry, timeoutMs: Number.POSITIVE_INFINITY }),
    /positive timeout/,
  );
  assert.equal(registry.calls.get.length, 0, '参数校验必须在碰 registry 之前');
});
