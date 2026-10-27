/**
 * plugins/wait-subagent/lib/wait.mjs —— 「等 subagent」的纯逻辑
 *
 * 这里不读文件、不跑子进程，只做三件事：
 *   ① **记住 live activation**：订阅 `subagent/start` / `subagent/end`，按 child id 记
 *      `running` / `settled`。这是判断“现在还有没有一个 activation 可等”的依据；
 *   ② **判断用户插话**：`agent/inbox/inserted` 里 `message.source.kind === 'user'` 的那条
 *      就是人发的话（Web 的 queue / steer 都走这里；subagent 的完成通知是
 *      `subagent-settled`，不会被误判）；
 *   ③ **竞态安全地等**：先挂监听、再复查 pending / tracker，最后才进入 await。
 *      这样“子代理在挂监听前刚好结束”“用户消息在开始等之前刚好入队”都不会丢。
 *
 * ★ 为什么要 `mode`：一次等多个 child 时，“等第一个”与“等全部”是**两种不同的需求**。
 *   `any_of`（默认）只要**任意一个** target 结束就返回（其余仍在跑，用 `pending` 交回去）；
 *   `all_of` 要等**每个** target 都结束。缺省 `any_of` 的理由：并行开了 N 个子代理时，
 *   绝大多数时候要的是“有结果就先看”，而不是被最慢的那个拖住。
 *
 * ★ 为什么用事件而不是轮询：`subagent/end` 是 managed activation 的**唯一终态边**
 *   （`finishDisposal` 在清理、通知父级之后发出），`agent/inbox/inserted` 是 inbox
 *   **每次插入**的同步通知；两者都在同一个进程里同步派发，不需要 sleep / 定时器。
 *
 * ★ 为什么 scope（`agent.ctx.on`）能收到：`subagent/*` 用 `scopeTarget(parent)` 派发，
 *   `agent/inbox/*` 用 `agentCarrier(agent)` 派发；`agent.ctx` 是用该 agent 注册的 scope，
 *   按 dsh-scope 的口径“tag 是 dispatch key 或它的祖先”才收 ⇒ 正好只收自己的。
 */

/** 人类用户消息的 source kind（`user-rpc` 也归并成这个 kind）。 */
export const USER_SOURCE_KIND = 'user';

/** 等待结果的内部 kind；由 `index.mjs` 翻译成模型可见的 status。 */
export const WAIT_KINDS = Object.freeze({
  FINISHED: 'finished',
  USER_INTERJECTED: 'user_interjected',
  TIMED_OUT: 'timed_out',
  CANCELLED: 'cancelled',
});

/** 等一组 child 的哪一类终态。 */
export const WAIT_MODES = Object.freeze({
  /** 任意一个 target 结束就返回（其余仍在跑）。 */
  ANY_OF: 'any_of',
  /** 每个 target 都结束才返回。 */
  ALL_OF: 'all_of',
});

/**
 * 一条 inbox 消息是不是“人插话”。
 * @param {unknown} message - `agent/inbox/inserted` 的 message。
 * @returns {boolean} source.kind === 'user'。
 */
export function isHumanMessage(message) {
  return message?.source?.kind === USER_SOURCE_KIND;
}

/**
 * 把一条用户消息压成一行预览（图片/文件消息没有文本时返回空串）。
 * @param {unknown} message - 用户消息。
 * @param {number} [limit] - 最大字符数。
 * @returns {string} 单行、已截断的文本。
 */
export function messagePreview(message, limit = 400) {
  if (!Array.isArray(message?.content)) return '';
  const text = message.content
    .filter((block) => block?.type === 'text' && typeof block.text === 'string')
    .map((block) => block.text)
    .join('\n')
    .trim();
  if (text.length === 0) return '';
  if (text.length <= limit) return text;
  return `${text.slice(0, Math.max(0, limit - 1))}…`;
}

/**
 * 当前 inbox 里已经排队的“人插话”。先 next-step（下一步就会看到）再 next-turn。
 * @param {object} agent - 调用方 Agent。
 * @returns {Array<object>} 人类用户消息。
 */
export function pendingHumanMessages(agent) {
  const nextStep = agent?.inbox?.nextStep ?? [];
  const nextTurn = agent?.inbox?.nextTurn ?? [];
  return [...nextStep, ...nextTurn].filter(isHumanMessage);
}

/**
 * 一条已经进 inbox 的人类消息落在哪个投递窗口。
 * @param {object} agent - 调用方 Agent。
 * @param {object} message - 人类用户消息。
 * @returns {'steer'|'queue'|'unknown'} steer 下一步可见；queue 要等本 turn 结束。
 */
export function inboxDelivery(agent, message) {
  if (agent?.inbox?.nextStep?.some((candidate) => candidate.id === message?.id)) return 'steer';
  if (agent?.inbox?.nextTurn?.some((candidate) => candidate.id === message?.id)) return 'queue';
  return 'unknown';
}

/**
 * 订阅 subagent 生命周期，维护每个 child 的最后已知状态。
 *
 * 状态不会被“结束即删除”：`settled` 留在表里，正是为了回答“wait 被调用时它是不是
 * 早就结束了”。新的 `subagent/start`（例如 cold resume）会覆盖成 `running`。
 *
 * @param {object} ctx - 插件/服务上下文，需有 `on(name, listener)`。
 * @returns {{get(childId: string): object|undefined, dispose(): void}} tracker。
 */
export function createRunTracker(ctx) {
  /** @type {Map<string, {state: 'running'|'settled', runId: string, info: object}>} */
  const runs = new Map();
  const disposers = [];

  const offStart = ctx.on('subagent/start', (info) => {
    if (typeof info?.id !== 'string') return;
    runs.set(info.id, { state: 'running', runId: info.runId, info: { id: info.id, runId: info.runId } });
  });
  if (typeof offStart === 'function') disposers.push(offStart);

  const offEnd = ctx.on('subagent/end', (info) => {
    if (typeof info?.id !== 'string') return;
    const current = runs.get(info.id);
    // 同一个 child 的旧 activation 迟到的 end 不能覆盖新 activation 的 running。
    if (current?.state === 'running' && current.runId !== info.runId) return;
    // 只保留终态需要的字段：不把 lastAssistantMessage 这类大块内容留在 tracker 里。
    runs.set(info.id, {
      state: 'settled',
      runId: info.runId,
      info: { id: info.id, runId: info.runId, stopReason: info.stopReason },
    });
  });
  if (typeof offEnd === 'function') disposers.push(offEnd);

  return {
    get(childId) {
      return runs.get(childId);
    },
    dispose() {
      for (const dispose of disposers.splice(0)) {
        try {
          dispose();
        } catch {
          /* 清理失败不改变已经观察到的状态 */
        }
      }
      runs.clear();
    },
  };
}

/**
 * 等一组 subagent 的终态，或等人插话。
 *
 * **注册顺序是契约**：
 *   1. 先挂 `subagent/end` / `agent/inbox/inserted` / abort 监听；
 *   2. 再复查 pending 人消息；
 *   3. 再复查 tracker（补上“挂监听前刚好结束”的窗口）；
 *   4. 最后才设 timeout / 进入 await。
 *
 * **返回口径**（`settled` / `pending` 都只描述本次传进来的 `targets`）：
 *   · `finished` —— `mode` 的条件在本次调用里达到；`race: true` 表示**根本不用等**
 *     （挂监听时条件已经满足）。`any_of` 只把**已经结束**的放进 `settled`，其余进 `pending`；
 *   · `user_interjected` / `timed_out` / `cancelled` —— 同样带上当时的 `settled` / `pending` 快照，
 *     这样模型知道“等到哪一步了”，不必重新猜。
 *
 * @param {object} options - 等待参数。
 * @param {object} options.agent - 调用方 Agent（`exec.agent`）。
 * @param {Array<{childId: string, runId?: string}>} options.targets - 要等的 child（至少一个）。
 * @param {'any_of'|'all_of'} [options.mode] - 等第一个还是等全部；缺省 `any_of`。
 * @param {number} [options.timeoutMs] - 可选安全上限；省略表示一直等到条件达成或人插话。
 * @param {AbortSignal} [options.signal] - 调用方取消信号（turn cancel）。
 * @param {{get(childId: string): object|undefined}} [options.tracker] - 生命周期 tracker。
 * @returns {Promise<object>} 内部结果：finished / user_interjected / timed_out / cancelled。
 */
export function waitForSubagents({ agent, targets, mode = WAIT_MODES.ANY_OF, timeoutMs, signal, tracker }) {
  const isAllOf = mode === WAIT_MODES.ALL_OF;

  return new Promise((resolve, reject) => {
    let done = false;
    /** @type {ReturnType<typeof setTimeout> | undefined} */
    let timer;
    /** @type {Array<() => void>} */
    const disposers = [];
    /** childId -> 只等这一次 activation 时的 runId；undefined 表示接受任意 end。 */
    const remaining = new Map();
    for (const target of targets) remaining.set(target.childId, target.runId);
    /**
     * 本次调用期间确认结束的 child（按确认顺序）。
     * @type {Array<{childId: string, info: object|undefined}>}
     */
    const settled = [];

    const cleanup = () => {
      if (timer !== undefined) {
        clearTimeout(timer);
        timer = undefined;
      }
      for (const dispose of disposers.splice(0)) {
        try {
          dispose();
        } catch {
          /* 单个 listener 的清理失败不影响其余清理 */
        }
      }
    };
    const finish = (outcome) => {
      if (done) return;
      done = true;
      cleanup();
      resolve(outcome);
    };
    const fail = (error) => {
      if (done) return;
      done = true;
      cleanup();
      reject(error);
    };

    /** 结果里的 settled / pending 快照（复制，避免调用方拿到内部可变数组）。 */
    const snapshot = () => ({
      settled: settled.map((entry) => ({ childId: entry.childId, info: entry.info })),
      pending: [...remaining.keys()],
    });
    const interjected = (message) =>
      finish({
        kind: WAIT_KINDS.USER_INTERJECTED,
        message,
        delivery: inboxDelivery(agent, message),
        ...snapshot(),
      });
    const finishFinished = (race) =>
      finish({ kind: WAIT_KINDS.FINISHED, ...snapshot(), ...(race === true ? { race: true } : {}) });

    /** 条件是否已经满足：all_of 要没有 remaining；any_of 只要有一个 settled。 */
    const conditionMet = () => (isAllOf ? remaining.size === 0 : settled.length > 0);
    /** 记录一个 child 结束；重复结束返回 false（不改状态）。 */
    const recordSettled = (childId, info) => {
      if (!remaining.has(childId)) return false;
      remaining.delete(childId);
      settled.push({ childId, info });
      return true;
    };

    const onEnd = (info) => {
      if (done) return;
      const childId = info?.id;
      if (typeof childId !== 'string' || !remaining.has(childId)) return;
      const expectedRunId = remaining.get(childId);
      if (expectedRunId !== undefined && info.runId !== expectedRunId) return;
      // If a human message is already pending at the same edge, prefer reporting
      // the interjection: the model must not miss that a human is waiting.
      const pending = pendingHumanMessages(agent);
      if (pending.length > 0) {
        interjected(pending[0]);
        return;
      }
      recordSettled(childId, { id: childId, runId: info.runId, stopReason: info.stopReason });
      if (conditionMet()) finishFinished(false);
    };
    const onInserted = (payload) => {
      const message = payload?.message;
      if (!isHumanMessage(message)) return;
      interjected(message);
    };
    const onAbort = () => finish({ kind: WAIT_KINDS.CANCELLED, reason: signal?.reason, ...snapshot() });

    // 空 targets 没有可等的东西：直接按“条件已满足”返回，避免挂到 timeout（调用方不该这么调）。
    if (remaining.size === 0) {
      finishFinished(true);
      return;
    }

    try {
      disposers.push(agent.ctx.on('subagent/end', onEnd));
      disposers.push(agent.ctx.on('agent/inbox/inserted', onInserted));
    } catch (error) {
      fail(error);
      return;
    }

    // 先处理已经取消：此时不需要再读 inbox / tracker。
    if (signal?.aborted) {
      onAbort();
      return;
    }
    signal?.addEventListener('abort', onAbort, { once: true });
    if (signal) disposers.push(() => signal.removeEventListener('abort', onAbort));

    // 挂上监听之后复查：这个窗口里入队的人消息不能漏。
    const pending = pendingHumanMessages(agent);
    if (pending.length > 0) {
      interjected(pending[0]);
      return;
    }

    // 挂上监听之后复查：这个窗口里结束的 activation 不能漏。
    // 若 tracker 已经换成了另一个 run，说明原来那次已经结束（或根本没有），
    // 立即算作 settled，而不是继续等一个永远不会再来的 end。
    for (const childId of [...remaining.keys()]) {
      const expectedRunId = remaining.get(childId);
      const current = tracker?.get(childId);
      if (current === undefined) {
        // 有 runId 却查不到记录：那次 activation 已经不在了（tracker 被清 / 早于插件），
        // 与原来的单 child 口径一致 —— 不挂死，按“已结束”返回，info 缺省。
        if (expectedRunId !== undefined) recordSettled(childId, undefined);
        continue;
      }
      if (expectedRunId !== undefined) {
        if (current.state === 'settled' && current.runId === expectedRunId) {
          recordSettled(childId, current.info);
          continue;
        }
        if (current.runId !== expectedRunId) {
          // 新 activation 已覆盖旧记录 ⇒ 要等的那一次已经结束。
          recordSettled(childId, current.info);
        }
        continue;
      }
      if (current.state === 'settled') recordSettled(childId, current.info);
    }
    if (conditionMet()) {
      finishFinished(true);
      return;
    }

    if (timeoutMs !== undefined) {
      timer = setTimeout(
        () => finish({ kind: WAIT_KINDS.TIMED_OUT, elapsedMs: timeoutMs, ...snapshot() }),
        timeoutMs,
      );
    }
  });
}
