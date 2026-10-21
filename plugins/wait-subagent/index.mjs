/**
 * plugins/wait-subagent/index.mjs —— DSH 插件：注册模型工具 `wait_subagent`
 *
 * ## 它解决什么问题
 * `subagent` 工具只返回 `started subagent <id>`，不返回结果；完成结果走
 * “settlement notice”。但模型有时**必须在本 turn 里等到子代理结束**才能继续
 * （例如要基于它的结论决定下一步）。现在只能 `bash` 起一个 `sleep` —— 时长是猜的，
 * 短了要再睡、长了白等，用户插话也打断不了。
 *
 * 这个工具把等待变成**事件驱动**：
 *   · 子代理的 managed activation 结束（`subagent/end`，scope 派发到直接父 agent）；
 *   · 用户插话（`agent/inbox/inserted` 且 `message.source.kind === 'user'`）；
 *   · turn 被取消（`exec.signal`）；
 *   · 可选的安全上限（`timeout_seconds`）。
 *
 * ## 为什么必须是插件，不是网页
 * 它要订阅**正在运行的会话**的 agent/subagent 生命周期事件，并拿 `exec.agent`
 * 的 scope context —— 这些只有宿主进程里的 DSH 插件拿得到。
 *
 * ## 三条不变量
 * ① **不轮询**：没有 `setInterval`、没有 `sleep`；只在事件 / abort / 可选 timer 上 settle；
 * ② **不抢 completion notice 的活**：子代理的最终输出仍由 runtime 的 settlement notice
 *    投递；本工具只回答“它结束了没有、为什么结束”，不复制/改写结果；
 * ③ **只等直接子代理**：用 parent-owned `subagent/catalog` 验证 child 身份；不接受
 *    兄弟/任意 session id，避免等错对象。
 */
import { defineTool } from '@deepseek-ai/dsh-tools';

import {
  createRunTracker,
  inboxDelivery,
  messagePreview,
  pendingHumanMessages,
  waitForSubagent,
  WAIT_KINDS,
} from './lib/wait.mjs';

/** Stable Loader identity. */
export const name = 'amayui-wait-subagent';
/** 只需要工具注册表；subagents / agents 在调用时按需解析（缺了也能加载）。 */
export const inject = ['tools'];

const TOOL_NAME = 'wait_subagent';
const DEFAULT_MAX_TIMEOUT_SECONDS = 3600;

const STATUS_ENUM = ['finished', 'already_finished', 'user_interjected', 'not_running', 'timed_out', 'cancelled'];
const DELIVERY_ENUM = ['steer', 'queue', 'unknown'];

const DESCRIPTION = [
  'Wait for one direct subagent to settle instead of sleeping or polling.',
  'Returns as soon as the child settles, a human user interjects (queue or steer), the wait times out, or the turn is cancelled.',
  'If the child already settled or has no live activation, returns immediately with that status.',
  'Call once per child; independent waits may run in parallel.',
  'The child’s final output still arrives separately as its completion notice; this tool only confirms settlement.',
  'A queued user interjection automatically concludes the turn so the message can run; a steered message is visible at your next step. Do not call wait_subagent again for that child.',
].join(' ');

const NOTE = {
  finished: (id, stop) =>
    `subagent ${id} settled${stop === undefined ? '' : ` (${stop})`}. Its completion notice and final output are delivered separately; continue with your next step. Do not call sleep to wait for it.`,
  alreadyFinished: (id, stop) =>
    `subagent ${id} had already settled${stop === undefined ? '' : ` (${stop})`} before this call. Do not wait for it again; its completion notice is the result.`,
  userSteer: () =>
    'The user interjected while you were waiting; the message is admitted at your next step. Stop waiting and address it.',
  userQueue: () =>
    'The user queued a message while you were waiting; it runs after this turn. The wait marked this result as turn-concluding, so the loop closes the turn and starts the queued message. Do not call wait_subagent again for this child.',
  notRunning: (id) =>
    `subagent ${id} has no live activation to wait for. If it already finished, use its completion notice; if it is inactive, resume it with send_message before waiting again.`,
  timedOut: (seconds) =>
    `The wait timed out after ${seconds}s and the child is still running. Do not poll; either end this turn and rely on the completion notice, or call wait_subagent once more if you must keep this turn open.`,
  cancelled: () => 'The wait was cancelled with the turn.',
};

/**
 * Resolve an optional service without making the plugin's load depend on it.
 * @param {object} ctx - plugin context.
 * @param {string} serviceName - service name.
 * @returns {any} service or undefined.
 */
function serviceOf(ctx, serviceName) {
  if (typeof ctx?.get !== 'function') return undefined;
  try {
    return ctx.get(serviceName);
  } catch {
    return undefined;
  }
}

/**
 * Read the parent-owned catalog for one direct child.
 * @param {object} ctx - plugin context.
 * @param {object} agent - calling agent.
 * @param {string} childId - child session id.
 * @param {AbortSignal} [signal] - cancellation forwarded to the catalog read.
 * @returns {Promise<{supported: boolean, entry?: object}>} catalog outcome; unsupported means "cannot validate".
 */
async function catalogEntryOf(ctx, agent, childId, signal) {
  const subagents = serviceOf(ctx, 'subagents');
  if (subagents === undefined || typeof subagents.listChildren !== 'function') return { supported: false };
  try {
    const children = await subagents.listChildren(agent.id, signal);
    return { supported: true, entry: children.find((candidate) => candidate.id === childId) };
  } catch {
    return { supported: false };
  }
}

/**
 * Whether the child is a direct runtime child of the caller. Only available for
 * local children (external runs have no Agent).
 * @param {object} ctx - plugin context.
 * @param {object} agent - calling agent.
 * @param {string} childId - child session id.
 * @returns {boolean|undefined} true/false when the registry can answer; undefined when it cannot.
 */
function ownedByCaller(ctx, agent, childId) {
  const agents = serviceOf(ctx, 'agents');
  if (agents === undefined || typeof agents.isOwnedBy !== 'function') return undefined;
  return agents.isOwnedBy(childId, agent);
}

/**
 * Turn one internal wait outcome into the canonical tool value.
 * @param {object} agent - calling agent.
 * @param {string} childId - child session id.
 * @param {object} outcome - result from `waitForSubagent`.
 * @param {number|undefined} timeoutSeconds - requested timeout, for wording.
 * @returns {object} canonical value.
 */
function valueOfOutcome(childId, outcome, timeoutSeconds) {
  switch (outcome.kind) {
    case WAIT_KINDS.FINISHED: {
      const stop = outcome.info?.stopReason;
      return {
        agent_id: childId,
        status: outcome.race === true ? 'already_finished' : 'finished',
        ...(stop === undefined ? {} : { stop_reason: String(stop) }),
        note: outcome.race === true ? NOTE.alreadyFinished(childId, stop) : NOTE.finished(childId, stop),
      };
    }
    case WAIT_KINDS.USER_INTERJECTED: {
      const delivery = outcome.delivery ?? 'unknown';
      const preview = messagePreview(outcome.message);
      return {
        agent_id: childId,
        status: 'user_interjected',
        delivery,
        ...(preview.length === 0 ? {} : { message_preview: preview }),
        note: delivery === 'queue' ? NOTE.userQueue() : NOTE.userSteer(),
      };
    }
    case WAIT_KINDS.TIMED_OUT:
      return {
        agent_id: childId,
        status: 'timed_out',
        elapsed_seconds: Math.round((outcome.elapsedMs ?? 0) / 1000),
        note: NOTE.timedOut(timeoutSeconds ?? Math.round((outcome.elapsedMs ?? 0) / 1000)),
      };
    case WAIT_KINDS.CANCELLED:
      return { agent_id: childId, status: 'cancelled', note: NOTE.cancelled() };
    default:
      return {
        agent_id: childId,
        status: 'not_running',
        note: NOTE.notRunning(childId),
      };
  }
}

/**
 * Build the immediate "already settled" value from the lifecycle tracker.
 * @param {string} childId - child session id.
 * @param {object|undefined} tracked - tracker snapshot.
 * @returns {object} canonical value.
 */
function valueOfSettled(childId, tracked) {
  const stop = tracked?.info?.stopReason;
  return {
    agent_id: childId,
    status: 'already_finished',
    ...(stop === undefined ? {} : { stop_reason: String(stop) }),
    note: NOTE.alreadyFinished(childId, stop),
  };
}

/** Canonical value for "no live activation, and no evidence it is still starting". */
function valueOfNotRunning(childId) {
  return { agent_id: childId, status: 'not_running', note: NOTE.notRunning(childId) };
}

/** Canonical value for an already-cancelled call. */
function valueOfCancelled(childId) {
  return { agent_id: childId, status: 'cancelled', note: NOTE.cancelled() };
}

/** Canonical value for a user message already pending before the wait starts. */
function valueOfPendingUser(agent, childId) {
  const message = pendingHumanMessages(agent)[0];
  return valueOfOutcome(childId, {
    kind: WAIT_KINDS.USER_INTERJECTED,
    message,
    delivery: inboxDelivery(agent, message),
  });
}

/**
 * A queued (next-turn) user message cannot be seen in this turn. Mark the tool
 * result terminal so the loop closes the turn and starts the queued message —
 * the model is not asked to "remember to end" after an interrupted wait.
 * A steered (next-step) message is visible at the next step, so the turn stays
 * open for a normal reply.
 * @param {object} exec - ToolRunContext.
 * @param {object} value - canonical result.
 * @returns {object} the same value.
 */
function concludeIfQueued(exec, value) {
  if (value.status === 'user_interjected' && value.delivery === 'queue' && typeof exec?.concludeTurn === 'function') {
    exec.concludeTurn();
  }
  return value;
}

export function apply(ctx) {
  const tracker = createRunTracker(ctx);

  ctx.tools.register(
    defineTool({
      name: TOOL_NAME,
      description: DESCRIPTION,
      parameters: {
        agent_id: {
          type: 'string',
          required: true,
          description: 'Child session id returned by subagent/subagent_fork as `started subagent <id>`.',
        },
        timeout_seconds: {
          type: 'integer',
          description: `Optional safety limit in seconds (1-${DEFAULT_MAX_TIMEOUT_SECONDS}). Omit to wait until the child settles or the user interjects.`,
        },
      },
      output: {
        schema: {
          type: 'object',
          additionalProperties: false,
          properties: {
            agent_id: { type: 'string', required: true },
            status: { type: 'string', required: true, enum: STATUS_ENUM },
            stop_reason: { type: 'string' },
            delivery: { type: 'string', enum: DELIVERY_ENUM },
            message_preview: { type: 'string' },
            elapsed_seconds: { type: 'integer' },
            note: { type: 'string', required: true },
          },
        },
        render: (_args, value) => [{ type: 'text', text: value.note }],
      },
      // Read-only observation: overlapping waits (or a wait next to another read) are safe.
      isConcurrencySafe: () => true,
      async execute(args, exec) {
        const agent = exec?.agent;
        if (agent === undefined) {
          throw new Error('wait_subagent requires a calling Agent; it cannot run agentless.');
        }

        const childId = args.agent_id;
        if (typeof childId !== 'string' || childId.trim().length === 0) {
          throw new Error('agent_id must be a non-empty child session id.');
        }
        if (childId === agent.id) throw new Error('wait_subagent cannot wait for the calling agent itself.');

        const timeoutSeconds = args.timeout_seconds;
        if (timeoutSeconds !== undefined) {
          if (!Number.isSafeInteger(timeoutSeconds) || timeoutSeconds < 1 || timeoutSeconds > DEFAULT_MAX_TIMEOUT_SECONDS) {
            throw new Error(`timeout_seconds must be an integer from 1 through ${DEFAULT_MAX_TIMEOUT_SECONDS}.`);
          }
        }

        // A human is already waiting: do not enter a wait that would hide that fact.
        if (exec.signal?.aborted) return valueOfCancelled(childId);
        if (pendingHumanMessages(agent).length > 0) return concludeIfQueued(exec, valueOfPendingUser(agent, childId));

        const catalog = await catalogEntryOf(ctx, agent, childId, exec.signal);
        if (exec.signal?.aborted) return valueOfCancelled(childId);
        // The catalog read can yield: a human may have interjected in that window.
        if (pendingHumanMessages(agent).length > 0) return concludeIfQueued(exec, valueOfPendingUser(agent, childId));

        // The parent-owned catalog is the authority on "is this one of my direct children?".
        // A missing read (projection unavailable) is a soft failure: keep the tracker/registry checks.
        if (catalog.supported && catalog.entry === undefined) {
          throw new Error(
            `"${childId}" is not a direct child of agent "${agent.id}". Use list_agents to find a child id from \`started subagent <id>\`.`,
          );
        }

        const tracked = tracker.get(childId);
        if (tracked?.state === 'settled') return valueOfSettled(childId, tracked);

        if (tracked?.state === 'running') {
          const outcome = await waitForSubagent({
            agent,
            childId,
            runId: tracked.runId,
            timeoutMs: timeoutSeconds === undefined ? undefined : timeoutSeconds * 1000,
            signal: exec.signal,
            tracker,
          });
          return concludeIfQueued(exec, valueOfOutcome(childId, outcome, timeoutSeconds));
        }

        // Tracker missed the start (plugin reload / child started before this plugin).
        // A live local Agent is still an authoritative "running" answer.
        const agents = serviceOf(ctx, 'agents');
        const live = agents !== undefined && typeof agents.get === 'function' ? agents.get(childId) : undefined;
        if (live !== undefined) {
          if (ownedByCaller(ctx, agent, childId) === false) {
            throw new Error(`"${childId}" is a live agent but is not a direct child of agent "${agent.id}".`);
          }
          const outcome = await waitForSubagent({
            agent,
            childId,
            timeoutMs: timeoutSeconds === undefined ? undefined : timeoutSeconds * 1000,
            signal: exec.signal,
            tracker,
          });
          return concludeIfQueued(exec, valueOfOutcome(childId, outcome, timeoutSeconds));
        }

        // External children have no local Agent, but their activation still emits the
        // same `subagent/start` / `subagent/end` pair. A start is always announced
        // before the delegation tool returns, so an unknown tracker entry here means
        // the activation is not live (or predates this plugin). Do not block forever
        // on a terminal edge that may already have passed.
        return valueOfNotRunning(childId);
      },
    }),
  );
}
