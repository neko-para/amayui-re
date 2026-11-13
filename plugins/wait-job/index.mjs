/**
 * plugins/wait-job/index.mjs —— DSH 插件：注册模型工具 `wait_job`
 *
 * ## 它解决什么问题
 * 等一个后台任务现在只有 `job_output({ wait: true })`，而它只有三个出口：
 * 结算 / 超时 / `exec.signal`（turn 取消）。**用户插话不会叫醒它** ⇒ 模型要么
 * "停掉整个任务"，要么盲猜一个时长去 `sleep`。这个工具把等待变成可打断的：
 *   · 任务结算；· 用户插话（`agent/inbox/inserted` 且 `source.kind === 'user'`）；
 *   · turn 取消（`exec.signal`）；· 可选上限（`timeout_seconds`，缺省**不设**）；
 *   · （非缺省）`wake_on: 'output'`：任务写出新字节就返回。
 *
 * ## 为什么正文必须由这里带回（本单最重要的不变量）
 * `dsh-jobs-local` 在结算时若有 `wait` 正在等，那条 `settled` 事件的
 * `awaited` 为 true；`dsh-tool-jobs` 的完成通知遇到 `awaited` 就直接 return
 * （`dsh-tool-jobs/lib/index.js:269`）—— 而那条通知**不携带正文**（它只说
 * "Read its output with job_output"）。⇒ `wake_on: 'settle'` 结算后必须自己
 * `jobs.read` 把正文放进 tool 结果，否则正文丢失。细节见 `lib/wait-job.mjs` 的文件头。
 *
 * ## 为什么必须是插件而不是网页
 * 它要订阅**正在运行的会话**的 inbox 事件、拿 `exec.agent` 的 scope context、
 * 并调用宿主进程里的 `ctx.jobs` —— 这些只有 DSH 插件拿得到。
 *
 * ## 三条不变量
 * ① **不轮询**：没有 `setInterval`、没有 `sleep`；只在结算 / 事件 / abort / 可选 timer 上返回；
 * ② **因打断或超时返回时不动模型游标**：不调 `jobs.read`，只交回状态与已产出字节数；
 * ③ **两条路径不合并**：`wake_on:'output'` 从不持有 `jobs.wait`（否则人插话时那次结算
 *    会被标 `awaited` ⇒ 完成通知被吃，而这条路径又不读正文 ⇒ 丢结果）。
 */
import { defineTool } from '@deepseek-ai/dsh-tools';

import { AWAKE, WAKE_ON, interruptionOf, renderValue, shouldConcludeTurn, waitForJob } from './lib/wait-job.mjs';

/** Stable Loader identity. */
export const name = 'amayui-wait-job';
/** 需要工具注册表与后台任务注册表（没有 jobs 这个工具无从工作）。 */
export const inject = ['tools', 'jobs'];

const TOOL_NAME = 'wait_job';
/** `timeout_seconds` 的硬上限（24h）：再长就该让模型结束这个 turn，靠完成通知。 */
const MAX_TIMEOUT_SECONDS = 86400;

const AWAKE_ENUM = Object.freeze([AWAKE.SETTLED, AWAKE.INTERRUPTED, AWAKE.TIMEOUT, AWAKE.OUTPUT, AWAKE.NO_SUCH_JOB]);
const WAKE_ON_ENUM = Object.freeze([WAKE_ON.SETTLE, WAKE_ON.OUTPUT]);
const STATUS_ENUM = Object.freeze(['running', 'stopping', 'completed', 'killed', 'failed']);

const DESCRIPTION = [
  'Wait for one background job instead of polling it or sleeping.',
  'Returns immediately when the user interjects or the turn is cancelled, and when `timeout_seconds` expires; those returns report `awake`, the job status, the bytes produced so far and the progress line, but do NOT read the output — read it with job_output.',
  'Default `wake_on: "settle"`: waits for settlement. A settlement collected by this wait suppresses the separate completion notice (the job runtime treats an awaited settlement as already delivered), so the job output is returned by THIS call when `include_output` is true (the default); with `include_output: false` you must read it with job_output.',
  '`wake_on: "output"`: returns as soon as the job writes new bytes after this call. It never consumes output and never holds a settlement wait, so the completion notice still arrives.',
  'Omit `timeout_seconds` to wait until the job settles or a human interjects.',
  'A job id this session cannot see is reported as `awake: "no-such-job"`, not as an error.',
].join(' ');

/**
 * Validate `job_id` beyond what the parameter schema can express (non-empty, trimmed).
 * @param {unknown} value - raw argument.
 * @returns {string} the job id.
 */
function validateJobId(value) {
  if (typeof value !== 'string' || value.trim().length === 0) {
    throw new Error(`invalid job_id: expected a non-empty string, got ${JSON.stringify(value)}`);
  }
  return value.trim();
}

/**
 * 被**队列型**用户消息打断时，把这个结果标成 turn 收尾 —— 否则那条消息要等模型
 * 自己结束 turn 才跑（它在**本 turn 里看不见**）。steer 型下一步就能看见 ⇒ 不收尾。
 * 与 `plugins/wait-subagent` 对同一件事的口径一致。
 * @param {object} exec - ToolRunContext。
 * @param {object} agent - 调用方 Agent。
 * @param {object} value - 本工具的 canonical value。
 * @returns {object} 同一个 value。
 */
function concludeIfQueuedUser(exec, agent, value) {
  if (value?.awake !== AWAKE.INTERRUPTED) return value;
  if (typeof exec?.concludeTurn !== 'function') return value;
  if (shouldConcludeTurn(agent) === false) return value;
  exec.concludeTurn();
  return value;
}

export function apply(ctx) {
  ctx.tools.register(
    defineTool({
      name: TOOL_NAME,
      description: DESCRIPTION,
      parameters: {
        job_id: {
          type: 'string',
          required: true,
          description: 'Job id returned by the tool that started the background work (or by job_list).',
        },
        timeout_seconds: {
          type: 'integer',
          description: `Optional safety limit in seconds (1-${MAX_TIMEOUT_SECONDS}). Omit to wait until the job settles or a human interjects.`,
        },
        wake_on: {
          type: 'string',
          enum: WAKE_ON_ENUM,
          default: WAKE_ON.SETTLE,
          description:
            'settle (default): return when the job settles, carrying its output when include_output is true. output: return as soon as the job writes new output after this call; that output is left unread for job_output.',
        },
        include_output: {
          type: 'boolean',
          default: true,
          description:
            'Whether a settle return carries the job output (default true). Ignored for wake_on: "output", which never consumes output.',
        },
      },
      output: {
        schema: {
          type: 'object',
          additionalProperties: false,
          properties: {
            awake: { type: 'string', required: true, enum: AWAKE_ENUM },
            status: { type: 'string', enum: STATUS_ENUM },
            bytes: { type: 'integer' },
            progress: { type: 'string' },
            elapsed_seconds: { type: 'integer' },
            output: { type: 'string' },
            note: { type: 'string', required: true },
          },
        },
        render: (_args, value) => [{ type: 'text', text: renderValue(value) }],
      },
      // ★ 不声明 `isConcurrencySafe`：不过 settle 路径会 `jobs.read` —— 那是**消费模型游标**
      //   的写操作，两次并发 wait_job 会互相吞正文（`job_output` 同样不敢声称并发安全）。
      presentCall: (args) => ({ card: 'generic', title: `Wait for background job ${args.job_id}`, kind: 'read' }),
      async execute(args, exec) {
        const jobId = validateJobId(args.job_id);

        const timeoutSeconds = args.timeout_seconds;
        if (timeoutSeconds !== undefined) {
          if (!Number.isSafeInteger(timeoutSeconds) || timeoutSeconds < 1 || timeoutSeconds > MAX_TIMEOUT_SECONDS) {
            throw new Error(`timeout_seconds must be an integer from 1 through ${MAX_TIMEOUT_SECONDS}.`);
          }
        }

        const wakeOn = args.wake_on === undefined ? WAKE_ON.SETTLE : args.wake_on;
        if (!WAKE_ON_ENUM.includes(wakeOn)) throw new Error('wake_on must be "settle" or "output".');

        const agent = exec?.agent;
        // 与 `plugins/wait-subagent/index.mjs:428-430` 同形：没有调用方 Agent 就没有 session，
        // 也谈不上"这个 job 属于谁"——拿 `caller = undefined` 去查 job 只会得到一句误导性的
        // "不属于本 session"，所以这里直接报错。
        if (agent === undefined) {
          throw new Error('wait_job requires a calling Agent; it cannot run agentless.');
        }
        const value = await waitForJob({
          jobId,
          jobs: ctx.jobs,
          caller: agent.id,
          wakeOn,
          timeoutMs: timeoutSeconds === undefined ? undefined : timeoutSeconds * 1000,
          includeOutput: args.include_output !== false,
          signal: exec?.signal,
          interruption: interruptionOf(agent),
        });
        return concludeIfQueuedUser(exec, agent, value);
      },
    }),
  );
}
