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
 * ★ **一次可以等多个 child，默认只等第一个**（`mode: 'any_of'`）：并行开 N 个子代理时，
 *   要的通常只是“哪个先有结果就先看哪个”，而不是被最慢的那个拖住。要等全部才显式给
 *   `mode: 'all_of'`。两种模式的判定都只有一条：**条件达成的那条事件边**（见 `lib/wait.mjs`）。
 *
 * ## 为什么必须是插件，不是网页
 * 它要订阅**正在运行的会话**的 agent/subagent 生命周期事件，并拿 `exec.agent`
 * 的 scope context —— 这些只有宿主进程里的 DSH 插件拿得到。
 *
 * ## 三条不变量
 * ① **不轮询**：没有 `setInterval`、没有 `sleep`；只在事件 / abort / 可选 timer 上 settle；
 * ② **不抢 completion notice 的活**：子代理的最终输出仍由 runtime 的 settlement notice
 *    投递；本工具只回答“它结束了没有、为什么结束”，不复制/改写结果；
 * ③ **只等直接子代理**：用 parent-owned `subagent/catalog` 验证每个 child 身份；不接受
 *    兄弟/任意 session id，避免等错对象。
 */
import { defineTool } from '@deepseek-ai/dsh-tools';

import {
  createRunTracker,
  inboxDelivery,
  messagePreview,
  pendingHumanMessages,
  waitForSubagents,
  WAIT_KINDS,
  WAIT_MODES,
} from './lib/wait.mjs';

/** Stable Loader identity. */
export const name = 'amayui-wait-subagent';
/** 只需要工具注册表；subagents / agents 在调用时按需解析（缺了也能加载）。 */
export const inject = ['tools'];

const TOOL_NAME = 'wait_subagent';
const DEFAULT_MAX_TIMEOUT_SECONDS = 3600;
/** 一次最多等多少个 child；更多就该拆成几次调用（否则返回的 note 只会更难读）。 */
const MAX_TARGETS = 32;

const STATUS_ENUM = ['finished', 'already_finished', 'user_interjected', 'not_running', 'timed_out', 'cancelled'];
const DELIVERY_ENUM = ['steer', 'queue', 'unknown'];
const MODE_ENUM = [WAIT_MODES.ANY_OF, WAIT_MODES.ALL_OF];

const DESCRIPTION = [
  'Wait for direct subagents to settle instead of sleeping or polling.',
  'List the children in `agent_ids` (a single `agent_id` also works). `mode` decides when the call returns: `any_of` (default) returns as soon as the FIRST of them settles; `all_of` waits until every requested child has settled.',
  'Returns immediately when a human user interjects (queue or steer), when the wait times out, or when the turn is cancelled; the result then carries the settled/pending snapshot.',
  'A child that had already settled is reported as already_finished; a child with no live activation is reported as absent and is never waited for.',
  'Each child’s final output still arrives separately as its completion notice; this tool only confirms settlement.',
  'A queued user interjection automatically concludes the turn so the message can run; a steered message is visible at your next step. Do not call wait_subagent again for a child you were told settled.',
].join(' ');

/** "a, b, c" —— 用于 note 里的 id 列表。 */
function listOf(ids) {
  return ids.join(', ');
}

/** 把 settle 列表压成 "a (completed), b" 这样的短语。 */
function describeSettled(settled) {
  return settled
    .map((entry) => (entry.stop_reason === undefined ? entry.agent_id : `${entry.agent_id} (${entry.stop_reason})`))
    .join(', ');
}

/** 仍在跑的 child 子句（空列表返回空串）。 */
function stillRunningClause(pending) {
  if (pending.length === 0) return '';
  return ` ${listOf(pending)} ${pending.length === 1 ? 'is' : 'are'} still running and will deliver ${pending.length === 1 ? 'its' : 'their'} own completion notice${pending.length === 1 ? '' : 's'}.`;
}

/** 没有 live activation 的 child 子句。 */
function absentClause(absent) {
  return `${listOf(absent)} ${absent.length === 1 ? 'has' : 'have'} no live activation to wait for, so ${absent.length === 1 ? 'it was' : 'they were'} not waited for; use the completion notice, or resume an inactive child with send_message before waiting again.`;
}

const NOTE = {
  finished: ({ mode, settled, pending, absent }) => {
    if (settled.length === 0) {
      const parts = ['The wait ended without observing a settlement.'];
      if (absent.length > 0) parts.push(absentClause(absent));
      parts.push('Use the completion notices.');
      return parts.join(' ');
    }
    const parts = [];
    if (mode === WAIT_MODES.ALL_OF && settled.length > 1) {
      parts.push(`All ${settled.length} waited-for subagents settled: ${describeSettled(settled)}.`);
    } else if (mode === WAIT_MODES.ALL_OF || pending.length === 0) {
      parts.push(`${describeSettled(settled)} settled.`);
    } else {
      parts.push(`${describeSettled(settled)} settled first.`);
    }
    if (pending.length > 0) {
      parts.push(stillRunningClause(pending).trim());
      parts.push(
        `Do not sleep to wait for ${pending.length === 1 ? 'it' : 'them'}, and do not call wait_subagent again for ${listOf(settled.map((entry) => entry.agent_id))}.`,
      );
    } else {
      parts.push("Continue with your next step; each child's final output still arrives as its completion notice.");
    }
    if (absent.length > 0) parts.push(absentClause(absent));
    return parts.join(' ');
  },
  alreadyFinished: ({ settled, pending, absent }) => {
    const names = settled.map((entry) => entry.agent_id);
    const parts = [`${describeSettled(settled)} had already settled before this call.`];
    parts.push(`Do not wait for ${listOf(names)} again; the completion notice is the result.`);
    if (pending.length > 0) {
      parts.push(`${listOf(pending)} ${pending.length === 1 ? 'is' : 'are'} still running and ${pending.length === 1 ? 'was' : 'were'} not waited for.`);
    }
    if (absent.length > 0) parts.push(absentClause(absent));
    return parts.join(' ');
  },
  userSteer: ({ pending }) =>
    `The user interjected while you were waiting; the message is admitted at your next step. Stop waiting and address it.${stillRunningClause(pending)}`,
  userQueue: ({ pending }) =>
    `The user queued a message while you were waiting; it runs after this turn. The wait marked this result as turn-concluding, so the loop closes the turn and starts the queued message. Do not call wait_subagent again for ${pending.length > 1 ? 'those children' : 'that child'}.${stillRunningClause(pending)}`,
  notRunning: ({ absent }) =>
    absent.length === 1
      ? `${absent[0]} has no live activation to wait for. If it already finished, use its completion notice; if it is inactive, resume it with send_message before waiting again.`
      : `${listOf(absent)} have no live activation to wait for. If a child already finished, use its completion notice; if it is inactive, resume it with send_message before waiting again.`,
  timedOut: ({ seconds, settled, pending, absent }) => {
    const parts = [`The wait timed out after ${seconds}s.`];
    if (settled.length > 0) parts.push(`Settled: ${describeSettled(settled)}.`);
    if (pending.length > 0) parts.push(`Still running: ${listOf(pending)}.`);
    if (absent.length > 0) parts.push(absentClause(absent));
    parts.push('Do not poll; either end this turn and rely on the completion notices, or call wait_subagent once more if you must keep this turn open.');
    return parts.join(' ');
  },
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
 * Read the parent-owned catalog of direct children.
 * @param {object} ctx - plugin context.
 * @param {object} agent - calling agent.
 * @param {AbortSignal} [signal] - cancellation forwarded to the catalog read.
 * @returns {Promise<{supported: boolean, children?: object[]}>} catalog outcome; unsupported means "cannot validate".
 */
async function catalogOf(ctx, agent, signal) {
  const subagents = serviceOf(ctx, 'subagents');
  if (subagents === undefined || typeof subagents.listChildren !== 'function') return { supported: false };
  try {
    return { supported: true, children: (await subagents.listChildren(agent.id, signal)) ?? [] };
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
 * Validate and dedupe the requested child ids (`agent_id` + `agent_ids`).
 * @param {object} args - tool arguments.
 * @param {object} agent - calling agent.
 * @returns {string[]} unique child ids in request order.
 */
function requestedIds(args, agent) {
  const raw = [];
  if (args?.agent_id !== undefined) raw.push(args.agent_id);
  if (args?.agent_ids !== undefined) {
    if (!Array.isArray(args.agent_ids)) throw new Error('agent_ids must be an array of child session ids.');
    raw.push(...args.agent_ids);
  }
  if (raw.length === 0) throw new Error('wait_subagent needs `agent_id` or a non-empty `agent_ids`.');

  const ids = [];
  for (const value of raw) {
    if (typeof value !== 'string' || value.trim().length === 0) {
      throw new Error('every child id in `agent_id` / `agent_ids` must be a non-empty string.');
    }
    const id = value.trim();
    if (id === agent.id) throw new Error('wait_subagent cannot wait for the calling agent itself.');
    if (!ids.includes(id)) ids.push(id);
  }
  if (ids.length > MAX_TARGETS) {
    throw new Error(`wait_subagent accepts at most ${MAX_TARGETS} children per call; call it again for the rest.`);
  }
  return ids;
}

/** 所有返回值共有的字段。 */
function valueBase({ status, mode, agentIds, settled = [], pending = [], absent = [], note }) {
  return { status, mode, agent_ids: agentIds, settled, pending, absent, note };
}

/** 把内部 settled 条目（tracker 直读）转成模型可见的 `{agent_id, stop_reason?}`。 */
function settledOfTracker(entries) {
  return entries.map((entry) => ({
    agent_id: entry.agent_id,
    ...(entry.stop_reason === undefined ? {} : { stop_reason: String(entry.stop_reason) }),
  }));
}

/** 合并“调用前已经结束”的 child 与“等到了”的 child。 */
function mergeSettled(settledNow, outcomeSettled) {
  const merged = [...settledNow];
  for (const entry of outcomeSettled ?? []) {
    const stop = entry.info?.stopReason;
    merged.push({ agent_id: entry.childId, ...(stop === undefined ? {} : { stop_reason: String(stop) }) });
  }
  return merged;
}

/** 条件早已满足、根本不用等。 */
function valueOfAlreadySettled({ mode, agentIds, settled, pending, absent }) {
  return valueBase({
    status: 'already_finished',
    mode,
    agentIds,
    settled: settledOfTracker(settled),
    pending,
    absent,
    note: NOTE.alreadyFinished({ settled: settledOfTracker(settled), pending, absent }),
  });
}

/** 没有任何可等的 activation（既没结束记录、也没有 live agent）。 */
function valueOfNotRunning({ mode, agentIds, absent }) {
  return valueBase({ status: 'not_running', mode, agentIds, absent, note: NOTE.notRunning({ absent }) });
}

/** 调用时已经取消。 */
function valueOfCancelled({ mode, agentIds }) {
  return valueBase({ status: 'cancelled', mode, agentIds, note: NOTE.cancelled() });
}

/** 调用时就已经有人插话（还没进入等待）。 */
function valueOfPendingUser({ agent, mode, agentIds }) {
  const message = pendingHumanMessages(agent)[0];
  const delivery = inboxDelivery(agent, message);
  const preview = messagePreview(message);
  return {
    status: 'user_interjected',
    mode,
    agent_ids: agentIds,
    settled: [],
    pending: [],
    absent: [],
    delivery,
    ...(preview.length === 0 ? {} : { message_preview: preview }),
    note: delivery === 'queue' ? NOTE.userQueue({ pending: [] }) : NOTE.userSteer({ pending: [] }),
  };
}

/**
 * Turn one internal wait outcome into the canonical tool value.
 * @param {object} options - aggregation inputs.
 * @returns {object} canonical value.
 */
function valueOfWaitOutcome({ mode, agentIds, settledNow, absent, outcome, timeoutSeconds }) {
  const settled = mergeSettled(settledNow, outcome.settled);
  const pending = outcome.pending ?? [];

  switch (outcome.kind) {
    case WAIT_KINDS.FINISHED: {
      const status = outcome.race === true ? 'already_finished' : 'finished';
      return valueBase({
        status,
        mode,
        agentIds,
        settled,
        pending,
        absent,
        note:
          status === 'finished'
            ? NOTE.finished({ mode, settled, pending, absent })
            : NOTE.alreadyFinished({ settled, pending, absent }),
      });
    }
    case WAIT_KINDS.USER_INTERJECTED: {
      const delivery = outcome.delivery ?? 'unknown';
      const preview = messagePreview(outcome.message);
      return {
        status: 'user_interjected',
        mode,
        agent_ids: agentIds,
        settled,
        pending,
        absent,
        delivery,
        ...(preview.length === 0 ? {} : { message_preview: preview }),
        note: delivery === 'queue' ? NOTE.userQueue({ pending }) : NOTE.userSteer({ pending }),
      };
    }
    case WAIT_KINDS.TIMED_OUT: {
      const seconds = timeoutSeconds ?? Math.round((outcome.elapsedMs ?? 0) / 1000);
      return {
        ...valueBase({
          status: 'timed_out',
          mode,
          agentIds,
          settled,
          pending,
          absent,
          note: NOTE.timedOut({ seconds, settled, pending, absent }),
        }),
        elapsed_seconds: Math.round((outcome.elapsedMs ?? 0) / 1000),
      };
    }
    case WAIT_KINDS.CANCELLED:
      return valueBase({ status: 'cancelled', mode, agentIds, settled, pending, absent, note: NOTE.cancelled() });
    default:
      return valueOfNotRunning({ mode, agentIds: agentIds, absent });
  }
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
          description:
            'One child session id returned by subagent/subagent_fork as `started subagent <id>`. Shorthand for a single-element `agent_ids`; provide at least one of the two.',
        },
        agent_ids: {
          type: 'array',
          items: { type: 'string' },
          description: `Child session ids to wait for (1-${MAX_TARGETS}). With mode=any_of the call returns as soon as the first of them settles.`,
        },
        mode: {
          type: 'string',
          enum: MODE_ENUM,
          default: WAIT_MODES.ANY_OF,
          description:
            'any_of (default): return as soon as the first requested child settles, leaving the rest running. all_of: return only when every requested child has settled.',
        },
        timeout_seconds: {
          type: 'integer',
          description: `Optional safety limit in seconds (1-${DEFAULT_MAX_TIMEOUT_SECONDS}). Omit to wait until the mode's condition is met or the user interjects.`,
        },
      },
      output: {
        schema: {
          type: 'object',
          additionalProperties: false,
          properties: {
            status: { type: 'string', required: true, enum: STATUS_ENUM },
            mode: { type: 'string', required: true, enum: MODE_ENUM },
            agent_ids: { type: 'array', required: true, items: { type: 'string' } },
            settled: {
              type: 'array',
              required: true,
              items: {
                type: 'object',
                additionalProperties: false,
                properties: {
                  agent_id: { type: 'string', required: true },
                  stop_reason: { type: 'string' },
                },
              },
            },
            pending: { type: 'array', required: true, items: { type: 'string' } },
            absent: { type: 'array', required: true, items: { type: 'string' } },
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

        const ids = requestedIds(args, agent);
        const mode = args.mode === undefined ? WAIT_MODES.ANY_OF : args.mode;
        if (!MODE_ENUM.includes(mode)) throw new Error('mode must be "any_of" or "all_of".');

        const timeoutSeconds = args.timeout_seconds;
        if (timeoutSeconds !== undefined) {
          if (!Number.isSafeInteger(timeoutSeconds) || timeoutSeconds < 1 || timeoutSeconds > DEFAULT_MAX_TIMEOUT_SECONDS) {
            throw new Error(`timeout_seconds must be an integer from 1 through ${DEFAULT_MAX_TIMEOUT_SECONDS}.`);
          }
        }

        // A human is already waiting: do not enter a wait that would hide that fact.
        if (exec.signal?.aborted) return valueOfCancelled({ mode, agentIds: ids });
        if (pendingHumanMessages(agent).length > 0) {
          return concludeIfQueued(exec, valueOfPendingUser({ agent, mode, agentIds: ids }));
        }

        const catalog = await catalogOf(ctx, agent, exec.signal);
        if (exec.signal?.aborted) return valueOfCancelled({ mode, agentIds: ids });
        // The catalog read can yield: a human may have interjected in that window.
        if (pendingHumanMessages(agent).length > 0) {
          return concludeIfQueued(exec, valueOfPendingUser({ agent, mode, agentIds: ids }));
        }

        // The parent-owned catalog is the authority on "is this one of my direct children?".
        // A missing read (projection unavailable) is a soft failure: keep the tracker/registry checks.
        if (catalog.supported) {
          const known = new Set(catalog.children.map((entry) => entry.id));
          const strangers = ids.filter((id) => !known.has(id));
          if (strangers.length > 0) {
            throw new Error(
              `${strangers.map((id) => `"${id}"`).join(', ')} ${
                strangers.length === 1 ? 'is not a direct child' : 'are not direct children'
              } of agent "${agent.id}". Use list_agents to find a child id from \`started subagent <id>\`.`,
            );
          }
        }

        // 把每个 child 分成三类：已结束（不必等）/ 有 live activation（要等）/ 没有可等的。
        const agents = serviceOf(ctx, 'agents');
        const settledNow = [];
        const targets = [];
        const absent = [];
        for (const id of ids) {
          const tracked = tracker.get(id);
          if (tracked?.state === 'settled') {
            settledNow.push({ agent_id: id, ...(tracked.info?.stopReason === undefined ? {} : { stop_reason: String(tracked.info.stopReason) }) });
            continue;
          }
          if (tracked?.state === 'running') {
            targets.push({ childId: id, runId: tracked.runId });
            continue;
          }
          // Tracker missed the start (plugin reload / child started before this plugin).
          // A live local Agent is still an authoritative "running" answer.
          const live = agents !== undefined && typeof agents.get === 'function' ? agents.get(id) : undefined;
          if (live !== undefined) {
            if (ownedByCaller(ctx, agent, id) === false) {
              throw new Error(`"${id}" is a live agent but is not a direct child of agent "${agent.id}".`);
            }
            targets.push({ childId: id });
            continue;
          }
          absent.push(id);
        }

        // any_of 的“第一个”在调用前就已经结束 ⇒ 根本不用等。
        if (mode === WAIT_MODES.ANY_OF && settledNow.length > 0) {
          return valueOfAlreadySettled({
            mode,
            agentIds: ids,
            settled: settledNow,
            pending: targets.map((target) => target.childId),
            absent,
          });
        }
        // 没有可等的 activation：要么已经结束（不必等），要么连结束记录都没有（not_running）。
        if (targets.length === 0) {
          return settledNow.length > 0
            ? valueOfAlreadySettled({ mode, agentIds: ids, settled: settledNow, pending: [], absent })
            : valueOfNotRunning({ mode, agentIds: ids, absent });
        }

        const outcome = await waitForSubagents({
          agent,
          targets,
          mode,
          timeoutMs: timeoutSeconds === undefined ? undefined : timeoutSeconds * 1000,
          signal: exec.signal,
          tracker,
        });
        return concludeIfQueued(exec, valueOfWaitOutcome({ mode, agentIds: ids, settledNow, absent, outcome, timeoutSeconds }));
      },
    }),
  );
}
