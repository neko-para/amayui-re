/**
 * plugins/wait-job/lib/wait-job.mjs —— 「可被打断地等一个后台任务」的纯逻辑
 *
 * 这里**不 import 任何 dsh 包**：registry 的 `{ get, read, readAt, wait, events }`
 * 与「人插话」的订阅口都由调用方注入（见 `index.mjs`）⇒ `node --test` 零安装直接跑。
 *
 * ## 两条路径，**别揉成一条**（本文件最重要的不变量）
 *
 * `wake_on: 'settle'`（缺省）：用 `registry.wait(id, timeoutMs, caller, signal)`。
 *   `dsh-jobs-local` 在结算时，若**有 wait 正在等**，那条 `settled` 事件的
 *   `awaited === true`；而 `dsh-tool-jobs` 的完成通知在 `awaited === true` 时**直接
 *   return**（`dsh-tool-jobs/lib/index.js:269`）。
 *   ★ 通知本身**只有一行状态指针、从不带正文**（`dsh-tool-jobs/lib/index.js:167`：
 *   `` `${prefix}${detail}. Read its output with job_output.` ``）—— `awaited` 吃掉的
 *   只是那一行"去 job_output 读"的指针。
 *   ⇒ **正文唯一的通路是结算后我们自己 `read`** ⇒ `include_output` 缺省必须是 `true`；
 *   否则这次结算既没有通知、也没有正文 = 结果真的丢了。
 *   （这才是"结算不丢结果"的真理由：不是通知会带正文，而是通知只带指针。）
 *
 * `wake_on: 'output'`：**⛔ 不许持有 `registry.wait`**。否则用户插话时 job 恰好结算，
 *   那次结算也会被标 `awaited` ⇒ 完成通知被吃；而这条路径按规矩**不读正文**
 *   （不推进模型游标）⇒ 正文丢失。所以改用 `events.subscribe` 收
 *   `{type:'output', id}`，用 `readAt(id, from).next` 判断是否真有新字节。
 *
 * ## 因打断 / timeout 返回时**一律不碰模型游标**
 * 只回 `awake` 原因 + `status` / `bytes` / `progress`，`read` 一次都不调；
 * 之后那条完成通知（此时 `awaited === false`，它只带一行状态）照常到达，模型再
 * `job_output` 取正文。
 *
 * ## 缺省「不设超时」怎么表达
 * `dsh-jobs-local` 的 `waitJob` 要求 `timeoutMs` 是**正有限数**，它的 `deadline()`
 * 还拒绝 > 2^31-1 的值（Node 定时器上限）⇒ `Infinity` 会被直接拒掉。
 * 所以「不设超时」= 按 `MAX_WAIT_MS` 切块、逐块重等，直到结算 / 被打断。
 * 块到期只说明"这一段等满了"，**不等于**调用方要的 timeout（见 `awaitSettle` 的循环）。
 * ★ 块间竞态：结算恰好落在两块之间时，下一块 `wait` 会**立刻**拿到终局（真实实现如此），
 *   即便真漏了一次通知，损失的也只是一行状态行、**不是正文**（正文由我们自己 `read`）。
 *   用例：`chunk 到期之后结算 ⇒ 仍是 awake:'settled' 且正文非空`。
 *
 * ## 释放纪律
 * 无论从哪条路返回：`interruption` 的监听器、`signal` 上的 abort 监听、自己的 timer
 * 全部撤掉（`finally` / Promise 的 finish）—— 泄漏一条监听器就是 bug（test 有断言）。
 */

/** 工具可见的 `awake` 原因（**闭集**，`index.mjs` 的 output schema 用它）。 */
export const AWAKE = Object.freeze({
  /** 结算了（`include_output` 为真时正文已随本次结果带回）。 */
  SETTLED: 'settled',
  /** 人插话 / turn 取消把等待打断；游标未动。 */
  INTERRUPTED: 'interrupted',
  /** `timeout_seconds` 到期；游标未动。 */
  TIMEOUT: 'timeout',
  /** `wake_on:'output'`：有新字节；游标未动。 */
  OUTPUT: 'output',
  /** 这个 session 看不见这个 id（未知，或属于别的 session）。 */
  NO_SUCH_JOB: 'no-such-job',
});

/** 等什么。 */
export const WAKE_ON = Object.freeze({
  /** 等到结算（缺省）。 */
  SETTLE: 'settle',
  /** 等到**新的输出字节**（不等到结算）。 */
  OUTPUT: 'output',
});

/**
 * 单次 `registry.wait` 的最长跨度（ms）：Node 定时器上限，也是 `dsh-timeout` 的
 * `deadline()` 上限。见文件头「缺省不设超时怎么表达」。
 */
export const MAX_WAIT_MS = 2147483647;

/** 终态状态值（`JobView.status`）。 */
const TERMINAL_STATUSES = Object.freeze(['completed', 'killed', 'failed']);

/**
 * 这个状态是不是终态。
 * @param {unknown} status - `JobView.status`。
 * @returns {boolean} completed / killed / failed。
 */
export function isTerminalStatus(status) {
  return TERMINAL_STATUSES.includes(status);
}

/** 人类用户消息的 source kind（Web 的 queue / steer 都走这里）。 */
const USER_SOURCE_KIND = 'user';

/**
 * 一条 inbox 消息是不是「人插话」。
 * ★ 完成通知（`source.kind === 'tool-jobs'` / `'subagent-settled'` …）**不是**人插话，
 *   不能被误判成打断 —— 否则等 job 时会被别的 job 的通知叫醒。
 * @param {unknown} message - `agent/inbox/inserted` 的 message。
 * @returns {boolean} source.kind === 'user'。
 */
export function isHumanMessage(message) {
  return message?.source?.kind === USER_SOURCE_KIND;
}

/**
 * 当前 inbox 里已经排队的「人插话」。先 next-step（下一步就会看到）再 next-turn。
 * @param {object} agent - 调用方 Agent（只需要 `inbox.nextStep` / `inbox.nextTurn`）。
 * @returns {Array<object>} 人类用户消息。
 */
export function pendingHumanMessages(agent) {
  const nextStep = agent?.inbox?.nextStep ?? [];
  const nextTurn = agent?.inbox?.nextTurn ?? [];
  return [...nextStep, ...nextTurn].filter(isHumanMessage);
}

/**
 * 把调用方 Agent 收窄成本文件认识的那个「打断源」：查 pending + 订阅 inbox 插入。
 * ★ 只用 `agent.ctx.on` 的**事件总线契约**（返回 disposer），不认识 dsh 的类型。
 * @param {object} agent - 调用方 Agent（`exec.agent`）。
 * @returns {{pending(): number, subscribe(listener: () => void): () => void}|undefined} 打断源。
 */
export function interruptionOf(agent) {
  const scoped = agent?.ctx;
  if (scoped === undefined || scoped === null || typeof scoped.on !== 'function') return undefined;
  return {
    pending() {
      return pendingHumanMessages(agent).length;
    },
    subscribe(listener) {
      const dispose = scoped.on('agent/inbox/inserted', (payload) => {
        if (isHumanMessage(payload?.message)) listener();
      });
      return typeof dispose === 'function' ? dispose : () => {};
    },
  };
}

/**
 * 一条已经进 inbox 的人类消息落在哪个投递窗口。
 * @param {object} agent - 调用方 Agent（只需要 `inbox`）。
 * @param {object} message - 人类用户消息。
 * @returns {'steer'|'queue'|'unknown'} steer = 下一步就能看见；queue = 要等本 turn 结束。
 */
export function inboxDelivery(agent, message) {
  if (agent?.inbox?.nextStep?.some((candidate) => candidate.id === message?.id) === true) return 'steer';
  if (agent?.inbox?.nextTurn?.some((candidate) => candidate.id === message?.id) === true) return 'queue';
  return 'unknown';
}

/**
 * 这次「被打断」要不要顺带收尾本 turn。
 *
 * ★ 队列型（next-turn）用户消息在**本 turn 里看不见** —— 不收尾的话，模型拿着
 *   `interrupted` 的结果继续干活，那条消息要等它自己结束 turn 才轮得到。
 *   steer 型下一步就能看见 ⇒ 不收尾，让模型在这一步直接回应用户。
 *   （先例 `plugins/wait-subagent` 对同一件事的口径相同。）
 * @param {object} agent - 调用方 Agent。
 * @returns {boolean} true = 该调 `exec.concludeTurn()`。
 */
export function shouldConcludeTurn(agent) {
  const pending = pendingHumanMessages(agent);
  return pending.length > 0 && pending.every((message) => inboxDelivery(agent, message) === 'queue');
}

/**
 * 一次消费型读（`registry.read`）→ 模型可见正文。
 * 与 `dsh-tool-jobs` 的渲染口径一致：stdout 与无 channel 的块按序，stderr 收进一个
 * `[stderr]` 段，`log`（只给观察者的旁白）不进模型；有丢字就补一条提示；
 * 最后接上生产者的 `result`（只在结算后的第一次读出现一次）。
 * @param {{chunks?: readonly object[], lossy?: boolean, result?: string}} read - `JobRead`。
 * @returns {string} 正文（可能是空串）。
 */
export function renderDelta(read) {
  const chunks = Array.isArray(read?.chunks) ? read.chunks : [];
  const visible = chunks.filter((chunk) => chunk?.channel !== 'log');
  const out = visible.filter((chunk) => chunk?.channel !== 'stderr').map((chunk) => String(chunk?.text ?? '')).join('');
  const err = visible.filter((chunk) => chunk?.channel === 'stderr').map((chunk) => String(chunk?.text ?? '')).join('');
  const separator = out.length > 0 && !out.endsWith('\n') ? '\n' : '';
  const body = out + (err.length > 0 ? `${separator}[stderr]\n${err}` : '');
  const dropped = read?.lossy === true || visible.some((chunk) => chunk?.gapBefore === true);
  const text = dropped
    ? `${body}${body.length > 0 && !body.endsWith('\n') ? '\n' : ''}[some output was dropped from memory]`
    : body;
  const result = typeof read?.result === 'string' ? read.result : '';
  if (result.length === 0) return text;
  return text.length > 0 && !text.endsWith('\n') ? `${text}\n${result}` : `${text}${result}`;
}

/**
 * 模型看到的文本：一行人话 + 一行关键字段（`awake` / `status` / `bytes` / `progress`）
 * +（settled 且带正文时的）正文。
 * @param {object} value - 本函数的返回值（tool 的 canonical value）。
 * @returns {string} 模型可见文本。
 */
export function renderValue(value) {
  const fields = [value.awake];
  if (value.status !== undefined) fields.push(String(value.status));
  if (value.bytes !== undefined) fields.push(`${value.bytes} bytes`);
  if (value.progress !== undefined) fields.push(String(value.progress));
  if (value.elapsed_seconds !== undefined) fields.push(`${value.elapsed_seconds}s`);
  const lines = [value.note, `[${fields.join(' · ')}]`];
  if (typeof value.output === 'string' && value.output.length > 0) lines.push(value.output);
  return lines.join('\n');
}

/** 默认 timer（能注入是为了让 test 断言"timer 已清"）。 */
const DEFAULT_TIMERS = Object.freeze({
  setTimeout: (callback, ms) => setTimeout(callback, ms),
  clearTimeout: (handle) => clearTimeout(handle),
});

/**
 * 活跃条目的字节总数（`JobView.output.total`）。
 * @param {object|undefined} view - 投影。
 * @returns {number|undefined} total；投影不可读时 undefined。
 */
function byteTotal(view) {
  const total = view?.output?.total;
  return typeof total === 'number' ? total : undefined;
}

/** 距今秒数（向下取整，非负）。 */
function elapsedSeconds(startedAt) {
  return Math.max(0, Math.floor((Date.now() - startedAt) / 1000));
}

/** 状态短语：`completed` / `completed, exit code 0`。 */
function statusPhrase(view) {
  const status = view?.status === undefined ? 'unknown' : String(view.status);
  return view?.detail === undefined ? status : `${status}, ${view.detail}`;
}

/**
 * 组装 canonical value（`output` schema 认的字段，一个不多）。
 * @param {object} options - awake / view / note / output / startedAt / totalBytes。
 * @returns {object} canonical value。
 */
function valueOf({ awake, view, note, output, startedAt, totalBytes }) {
  const value = { awake, note, elapsed_seconds: elapsedSeconds(startedAt) };
  if (view !== undefined && view !== null) {
    if (view.status !== undefined) value.status = String(view.status);
    if (typeof view.progress === 'string' && view.progress.length > 0) value.progress = view.progress;
  }
  const total = totalBytes ?? byteTotal(view);
  if (total !== undefined) value.bytes = total;
  if (typeof output === 'string' && output.length > 0) value.output = output;
  return value;
}

/**
 * 这个 id 现在还看得见吗（`get` 是"存在 + 属于本 session"的权威判定）。
 * @param {object} jobs - registry（只需 `get`）。
 * @param {string} jobId - job id。
 * @param {unknown} caller - 调用方 session id。
 * @returns {boolean} true = 看得见（那这次失败另有原因，该如实报错）。
 */
function jobStillVisible(jobs, jobId, caller) {
  try {
    jobs.get(jobId, caller);
    return true;
  } catch {
    return false;
  }
}

/**
 * 重新投影一次（失败就退回上一次已知的投影 —— 报告状态不值得把整个调用变成错误）。
 * @param {object} jobs - registry。
 * @param {string} jobId - job id。
 * @param {unknown} caller - 调用方 session id。
 * @param {object|undefined} fallback - 上一次已知投影。
 * @returns {object|undefined} 最新投影。
 */
function refresh(jobs, jobId, caller, fallback) {
  try {
    return jobs.get(jobId, caller) ?? fallback;
  } catch {
    return fallback;
  }
}

/**
 * 看不见这个 job 时的结果（**不抛**：模型的工具调用不该因此变成异常）。
 * ★ 文案必须让"打错 id"读起来**不像一次正常返回**：明确说这次没等到任何东西，
 *   并指向 `job_list`。
 */
function noSuchJobValue(jobId, startedAt) {
  return valueOf({
    awake: AWAKE.NO_SUCH_JOB,
    view: undefined,
    output: undefined,
    startedAt,
    note:
      `No background job "${jobId}" is visible to this session, so nothing was waited for — ` +
      'the id is unknown, was archived (a settled record drops off), belongs to another session, ' +
      'or is a typo. Use job_list to see the jobs you can actually wait for.',
  });
}

/** 被打断（人插话 / turn 取消）时的结果：**不 read**。 */
function interruptedValue({ jobId, view, cause, startedAt }) {
  const lead =
    cause === 'user'
      ? `The user interjected while waiting for job ${jobId}`
      : `The wait for job ${jobId} was cancelled with the turn`;
  const note =
    `${lead}; it is still ${statusPhrase(view)}. This result did not consume any output — ` +
    (cause === 'user'
      ? 'read it with job_output, then address the user message.'
      : 'read it with job_output when you continue.');
  return valueOf({ awake: AWAKE.INTERRUPTED, view, output: undefined, startedAt, note });
}

/** `timeout_seconds` 到期时的结果：**不 read**。 */
function timeoutValue({ jobId, view, timeoutMs, startedAt }) {
  const seconds = Math.max(1, Math.round(timeoutMs / 1000));
  const note =
    `The wait for job ${jobId} timed out after ${seconds}s; it is still ${statusPhrase(view)}. ` +
    'This result did not consume any output — the completion notice arrives when it settles, ' +
    'or read the output now with job_output.';
  return valueOf({ awake: AWAKE.TIMEOUT, view, output: undefined, startedAt, note });
}

/** `wake_on:'output'` 醒来时的结果：**不 read**（不推进模型游标）。 */
function outputValue({ jobId, view, newBytes, totalBytes, startedAt }) {
  const note =
    `Job ${jobId} produced ${newBytes} new ${newBytes === 1 ? 'byte' : 'bytes'} and is still ${statusPhrase(view)}. ` +
    'This result did not consume the output — read it with job_output.';
  return valueOf({ awake: AWAKE.OUTPUT, view, output: undefined, startedAt, note, totalBytes });
}

/**
 * 结算时的结果。
 *
 * `includeOutput` 为真就 `read` 一次把正文带回：settle 路径上这次结算会被标
 * `awaited:true` ⇒ 完成通知不会再来；而那条通知**只带一行"去 job_output 读"的指针、
 * 从不带正文** ⇒ **正文唯一的通路就是这里**。这正是 `include_output` 缺省为 `true`
 * 的理由（关掉它 = 这次结算既没有通知也没有正文）。
 * （"已经结算"的那种走 `alreadySettled`：那次结算的事件早就发过了，本调用没可能让它
 * 变成 awaited，读正文只是顺手把没读过的字节交给模型。）
 *
 * @param {object} options - jobId / jobs / caller / view / includeOutput / alreadySettled / startedAt。
 * @returns {object} canonical value。
 */
function settledValue({ jobId, jobs, caller, view, includeOutput, alreadySettled, startedAt }) {
  let output = '';
  let readFailure;
  if (includeOutput === true) {
    try {
      output = renderDelta(jobs.read(jobId, caller));
    } catch (error) {
      readFailure = error;
    }
  }

  let note;
  if (readFailure !== undefined) {
    note =
      `Job ${jobId} settled (${statusPhrase(view)}), but reading its output failed: ` +
      `${readFailure instanceof Error ? readFailure.message : String(readFailure)}. ` +
      'Do not wait on it again; try job_output.';
  } else if (alreadySettled === true) {
    note =
      `Job ${jobId} had already settled (${statusPhrase(view)}) before this call; ` +
      (includeOutput !== true
        ? 'include_output was false, so read its output with job_output if needed.'
        : output.length > 0
          ? 'its unread output is below.'
          : 'it had no unread output.');
  } else if (includeOutput !== true) {
    note =
      `Job ${jobId} settled (${statusPhrase(view)}); include_output was false so its output was not read. ` +
      'Do not wait on it again — read the output with job_output.';
  } else if (output.length === 0) {
    note = `Job ${jobId} settled (${statusPhrase(view)}); it produced no output. Do not wait on it again.`;
  } else {
    note =
      `Job ${jobId} settled (${statusPhrase(view)}). Its output is below; this wait collected the settlement ` +
      'so do not wait on it again.';
  }
  return valueOf({ awake: AWAKE.SETTLED, view, output, startedAt, note });
}

/**
 * settle 路径：`registry.wait` + 私有 abort 控制器。
 *
 * 注册顺序是契约（与 wait-subagent 同形）：
 *   ① 先挂 `interruption` / `signal` 监听；
 *   ② 再复查"这个窗口里人已经插话 / turn 已经取消"；
 *   ③ 才进入 `wait` 循环。
 *
 * @param {object} options - 见 `waitForJob`。
 * @returns {Promise<object>} canonical value。
 */
async function awaitSettle({ jobId, jobs, caller, timeoutMs, includeOutput, signal, interruption, startedAt, view }) {
  const own = new AbortController();
  /** 谁打断了这次等待：'user' | 'turn' | undefined。**先记因，再 abort**。 */
  let cause;
  const disposers = [];

  const cleanup = () => {
    for (const dispose of disposers.splice(0)) {
      try {
        dispose();
      } catch {
        /* 单个 listener 的清理失败不影响其余清理 */
      }
    }
  };
  const interrupt = (by) => {
    if (cause !== undefined) return;
    cause = by;
    own.abort(by);
  };

  let lastView = view;
  try {
    if (interruption !== undefined) disposers.push(interruption.subscribe(() => interrupt('user')));

    const onTurnAbort = () => interrupt('turn');
    if (signal !== undefined && signal !== null && typeof signal.addEventListener === 'function') {
      signal.addEventListener('abort', onTurnAbort, { once: true });
      disposers.push(() => signal.removeEventListener('abort', onTurnAbort));
    }

    // 挂上监听之后再复查：这个窗口里插话 / 取消不能漏
    // （`addEventListener` 不会为"已经 abort 过的 signal"补发事件，所以这一查是必须的）。
    if (cause === undefined) {
      if (interruption !== undefined && interruption.pending() > 0) interrupt('user');
      else if (signal !== undefined && signal !== null && signal.aborted === true) interrupt('turn');
    }
    if (cause !== undefined) {
      return interruptedValue({ jobId, view: refresh(jobs, jobId, caller, lastView), cause, startedAt });
    }

    const deadlineAt = timeoutMs === undefined ? undefined : startedAt + timeoutMs;

    for (;;) {
      const chunkMs =
        deadlineAt === undefined ? MAX_WAIT_MS : Math.max(1, Math.min(MAX_WAIT_MS, deadlineAt - Date.now()));

      let waited;
      try {
        waited = await jobs.wait(jobId, chunkMs, caller, own.signal);
      } catch (error) {
        // 我们自己的 abort（人插话 / turn 取消）⇒ 打断，不碰游标。
        if (cause !== undefined) {
          return interruptedValue({ jobId, view: refresh(jobs, jobId, caller, lastView), cause, startedAt });
        }
        // 别的失败：只有"确实看不见这个 job"才降级成 no-such-job，其余如实抛出。
        if (jobStillVisible(jobs, jobId, caller) === false) return noSuchJobValue(jobId, startedAt);
        throw error;
      }

      // ★ 结算优先：即使同一瞬间我们被 abort，这次结算也很可能已经标了 awaited=true
      //   （通知被吃掉了）⇒ 正文必须由这次结果带回。
      if (isTerminalStatus(waited?.status)) {
        return settledValue({
          jobId,
          jobs,
          caller,
          view: waited,
          includeOutput,
          alreadySettled: false,
          startedAt,
        });
      }
      lastView = waited ?? lastView;

      if (cause !== undefined) {
        return interruptedValue({ jobId, view: refresh(jobs, jobId, caller, lastView), cause, startedAt });
      }
      if (deadlineAt !== undefined && Date.now() >= deadlineAt) {
        return timeoutValue({ jobId, view: lastView, timeoutMs, startedAt });
      }
      // 还有余量：块到期 / 实现提前 resolve 都只是"这一段等完了"，继续等。
    }
  } finally {
    cleanup();
  }
}

/** `events.subscribe` 的 filter：owned job 按 owner 收，无 caller 时退回 scope。 */
function filterOf(caller) {
  return caller === undefined || caller === null ? { owners: 'scope' } : { owner: String(caller) };
}

/**
 * output 路径：`events.subscribe` + `readAt`，**不持有 `wait`**（见文件头）。
 *
 * 注册顺序同样是契约：先订阅，再复查"订阅前就发生的 output / settled"，最后才等。
 *
 * @param {object} options - 见 `waitForJob`。
 * @returns {Promise<object>} canonical value。
 */
function awaitOutput({ jobId, jobs, caller, timeoutMs, signal, interruption, timers, startedAt, view }) {
  const timerApi = timers ?? DEFAULT_TIMERS;
  /** 本调用开始时的字节总数：只有**这之后**的新字节才算"有新输出"。 */
  const from = byteTotal(view) ?? 0;
  /** 一次读 `readAt` 就够（快速闸门用事件自带的 total，确认用 next）。 */
  const filter = filterOf(caller);

  return new Promise((resolve, reject) => {
    let done = false;
    /** @type {unknown} */
    let timer;
    const disposers = [];
    const cleanup = () => {
      if (timer !== undefined) {
        try {
          timerApi.clearTimeout(timer);
        } catch {
          /* timer 已消失也无所谓 */
        }
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
    const finish = (value) => {
      if (done) return;
      done = true;
      cleanup();
      resolve(value);
    };
    const fail = (error) => {
      if (done) return;
      done = true;
      cleanup();
      reject(error);
    };
    const finishNoSuchJob = () => finish(noSuchJobValue(jobId, startedAt));

    /** 用 `readAt` 的 `next` 确认真有新字节；是 ⇒ 返回 output。 */
    const finishIfNewBytes = (referenceView) => {
      let read;
      try {
        read = jobs.readAt(jobId, from, caller);
      } catch (error) {
        if (jobStillVisible(jobs, jobId, caller) === false) {
          finishNoSuchJob();
          return;
        }
        fail(error);
        return;
      }
      if (typeof read?.next !== 'number' || read.next <= from) return;
      finish(
        outputValue({
          jobId,
          view: refresh(jobs, jobId, caller, referenceView),
          newBytes: read.next - from,
          // `readAt` 的 `next` 就是环的当前总量（权威），投影可能比它旧。
          totalBytes: read.next,
          startedAt,
        }),
      );
    };

    const onEvent = (event) => {
      if (done || event === undefined || event === null) return;
      if (event.type === 'output') {
        if (event.id !== jobId) return;
        // 快速闸门：事件自带 total；不"真有新字节"就不必 readAt。
        if (typeof event.total === 'number' && event.total <= from) return;
        finishIfNewBytes(undefined);
        return;
      }
      if (event.type === 'settled') {
        if (event.job?.id !== jobId) return;
        // ★ 这条路径从不持有 wait ⇒ 通常 awaited=false、通知照常到；但**别的** waiter
        //   （例如另一次 job_output(wait:true)）可能已经把它收走了，那通知就不会来。
        const note =
          event.awaited === true
            ? `Job ${jobId} settled (${statusPhrase(event.job)}) while waiting for new output; ` +
              'another waiter collected the settlement, so no completion notice will arrive — read its output with job_output.'
            : `Job ${jobId} settled (${statusPhrase(event.job)}) while waiting for new output. ` +
              'Its completion notice still arrives; read the output with job_output.';
        finish(
          valueOf({
            awake: AWAKE.SETTLED,
            view: event.job,
            output: undefined,
            startedAt,
            note,
          }),
        );
        return;
      }
      if (event.type === 'removed') {
        if (event.job?.id !== jobId) return;
        // 记录被移除 ⇒ 不会再有 output / settled 事件了。
        finishNoSuchJob();
      }
    };

    // ① 先订阅
    try {
      const dispose = jobs.events.subscribe(filter, onEvent);
      if (typeof dispose === 'function') disposers.push(dispose);
    } catch (error) {
      fail(error);
      return;
    }

    // ② 挂上之后复查：订阅窗口里已经发生的 output / settled 不能漏。
    let now;
    try {
      now = jobs.get(jobId, caller);
    } catch {
      finishNoSuchJob();
      return;
    }
    if (isTerminalStatus(now?.status)) {
      finish(
        valueOf({
          awake: AWAKE.SETTLED,
          view: now,
          output: undefined,
          startedAt,
          note:
            `Job ${jobId} had already settled (${statusPhrase(now)}) before this call; wake_on was 'output', ` +
            'so its output was not read — read it with job_output.',
        }),
      );
      return;
    }
    if ((byteTotal(now) ?? 0) > from) {
      finishIfNewBytes(now);
      if (done) return;
    }

    // ③ 人插话 / turn 取消
    if (interruption !== undefined) {
      disposers.push(
        interruption.subscribe(() =>
          finish(interruptedValue({ jobId, view: refresh(jobs, jobId, caller, now), cause: 'user', startedAt })),
        ),
      );
    }
    const onTurnAbort = () =>
      finish(interruptedValue({ jobId, view: refresh(jobs, jobId, caller, now), cause: 'turn', startedAt }));
    if (signal !== undefined && signal !== null && typeof signal.addEventListener === 'function') {
      signal.addEventListener('abort', onTurnAbort, { once: true });
      disposers.push(() => signal.removeEventListener('abort', onTurnAbort));
      if (signal.aborted === true) {
        onTurnAbort();
        return;
      }
    }
    if (interruption !== undefined && interruption.pending() > 0) {
      finish(interruptedValue({ jobId, view: now, cause: 'user', startedAt }));
      return;
    }

    // ④ 才设 timer（缺省不设 ⇒ 一直等到 output / settled / 打断）
    if (timeoutMs !== undefined) {
      timer = timerApi.setTimeout(() => {
        timer = undefined;
        finish(
          timeoutValue({ jobId, view: refresh(jobs, jobId, caller, now), timeoutMs, startedAt }),
        );
      }, timeoutMs);
    }
  });
}

/**
 * 可被打断地等一个后台任务。
 *
 * @param {object} options - 等待参数。
 * @param {string} options.jobId - 要等的 job id（非空）。
 * @param {object} options.jobs - registry 面：`{ get, read, readAt, wait, events }`。
 * @param {unknown} [options.caller] - 调用方 session id（`exec.agent.id`）；缺省表示无主视图。
 * @param {'settle'|'output'} [options.wakeOn] - 等结算（缺省）还是等新输出。
 * @param {number} [options.timeoutMs] - 可选上限（正数）；缺省**不设超时**。
 * @param {boolean} [options.includeOutput] - 结算时是否把正文带回（缺省 true）。
 * @param {AbortSignal} [options.signal] - 调用方取消信号（turn cancel）。
 * @param {{pending(): number, subscribe(listener: () => void): () => void}} [options.interruption] - 人插话源。
 * @param {{setTimeout: Function, clearTimeout: Function}} [options.timers] - 可注入的 timer（测试用）。
 * @returns {Promise<object>} canonical value：`{ awake, status?, bytes?, progress?, elapsed_seconds, output?, note }`。
 */
export async function waitForJob(options) {
  const {
    jobId,
    jobs,
    caller,
    wakeOn = WAKE_ON.SETTLE,
    timeoutMs,
    includeOutput = true,
    signal,
    interruption,
    timers,
  } = options ?? {};

  if (typeof jobId !== 'string' || jobId.length === 0) {
    throw new Error('waitForJob needs a non-empty job id.');
  }
  if (jobs === undefined || jobs === null || typeof jobs.get !== 'function' || typeof jobs.wait !== 'function') {
    throw new Error('waitForJob needs a job registry ({ get, read, readAt, wait, events }).');
  }
  if (timeoutMs !== undefined && (!Number.isFinite(timeoutMs) || timeoutMs <= 0)) {
    throw new Error('waitForJob needs a positive timeout in milliseconds, or none at all.');
  }

  const startedAt = Date.now();

  // 1. 预检（**不抛**）：未知 / 别人的 job ⇒ no-such-job。
  let initial;
  try {
    initial = jobs.get(jobId, caller);
  } catch {
    return noSuchJobValue(jobId, startedAt);
  }

  // 2. 已经结算：没有可等的东西了（返回正文，不再调用 wait）。
  if (isTerminalStatus(initial?.status)) {
    return settledValue({
      jobId,
      jobs,
      caller,
      view: initial,
      // output 路径不推进模型游标（见文件头），所以它从不读正文。
      includeOutput: wakeOn === WAKE_ON.OUTPUT ? false : includeOutput,
      alreadySettled: true,
      startedAt,
    });
  }

  // 3. turn 已经取消 / 人已经在等：不要进入一个把这两件事藏起来的等待。
  if (signal !== undefined && signal !== null && signal.aborted === true) {
    return interruptedValue({ jobId, view: initial, cause: 'turn', startedAt });
  }
  if (interruption !== undefined && interruption.pending() > 0) {
    return interruptedValue({ jobId, view: initial, cause: 'user', startedAt });
  }

  // 4. 两条路径分工（**不要**合并）。
  if (wakeOn === WAKE_ON.OUTPUT) {
    return awaitOutput({ jobId, jobs, caller, timeoutMs, signal, interruption, timers, startedAt, view: initial });
  }
  return awaitSettle({ jobId, jobs, caller, timeoutMs, includeOutput, signal, interruption, startedAt, view: initial });
}
