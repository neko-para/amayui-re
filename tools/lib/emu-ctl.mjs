/**
 * tools/lib/emu-ctl.mjs — `emu-ctl` 的**纯函数**（argv / 端点文件 / 列表合并 / 选中 / 退出码 / 人读格式化）
 *
 * ★ 为什么单独一个文件：这些是"客户端的判断"，必须能在**不起端点、不开 socket**的 pure 测试里钉住
 *   （`tools/test/emu-ctl.test.mjs`）。传输（Node 内置 `WebSocket`）只在 `tools/emu-ctl.mjs` 里。
 * ★ 本文件**不 import node:net / node:fs**：它只处理"字符串 / 对象 → 判定"。
 *   端点文件的**读取**由 CLI 做，读到的文本交给这里的 `parseCtlFile()`。
 * ★ 协议真源是服务端代码（`apps/emulator-lift/src/runtime/control_ws.cpp`），不是设计文档：
 *   · 请求字段是**顶层的** `for_ms` / `for_frames` / `path`（⛔ 不是 `{"args":{…}}`）；
 *   · 每条 `res` 带 `nonce` 与 `id`；`bye` 带 `{event,reason,requested_by,nonce}`；
 *   · 端点文件 `ctl.<pid>.<nonce8>.json` 带 `{schema,url,pid,port,nonce,token,session,…}`。
 */
import path from 'node:path';

import { REPO_ROOT } from './paths.mjs';

/** 缺省控制根：`<repo>/.tmp/emu-ctl`（服务端要显式 `--control-root`，客户端缺省就找这里）。 */
export const DEFAULT_CONTROL_ROOT = path.join(REPO_ROOT, '.tmp', 'emu-ctl');

/** `list` 的**单条目**连接预算（设计 §2.5：逐个 1.5 s 连一次）。⛔ 不要为了"看起来稳"放大它。 */
export const CONNECT_TIMEOUT_MS = 1500;

/** 命令缺省超时（秒）；`--timeout <s>` 可调。 */
export const COMMAND_TIMEOUT_SEC = 30;

/** 端点文件里的 `schema`（握手 / 文件同一个版本号）。 */
export const CTL_SCHEMA = 'emu-ctl/2';

/** `ctl.<pid>.<nonce8>.json`（服务端 `control_ws.cpp` 的命名；`.tmp` 中间件不匹配）。 */
export const CTL_NAME_RE = /^ctl\.(\d+)\.([0-9a-f]{8})\.json$/;

/** `list` 条目的 `why` 枚举（判据 9a/9c 的客户端面）。 */
export const WHY = Object.freeze(['ok', 'refused', 'timeout', 'nonce_mismatch', 'bad_file']);

/** 退出码（判据 / DSH tool 都吃这几个数）。 */
export const EXIT = Object.freeze({
  ok: 0, // 成功（含 script.available:false）
  endpoint: 1, // 端点回 ok:false
  usage: 2, // 参数 / 协议错
  timeout: 3, // 超时（端点仍活）
  instance: 4, // 找不到 / 多个实例
});

/** 本客户端实现的操作（`click` 已解锁；⛔ `key` 仍是 M1 未实现；`gc` 只删残留端点文件）。 */
export const OPS = Object.freeze(['list', 'pause', 'resume', 'sample', 'screenshot', 'click', 'wait', 'quit', 'gc']);

/** M1 的**剩余**能力：传了就明确回"未实现"（退出码 2），⛔ 不当 `unknown_op` 静默掉。`click` 已解锁。 */
export const M1_OPS = Object.freeze(['key']);

/** 协议里 `error.code` 的枚举（服务端 `controlErrorName`；⛔ 客户端不按 `message` 分支）。 */
export const ERROR_CODES = Object.freeze([
  'bad_request',
  'unknown_op',
  'not_paused',
  'busy',
  'unavailable',
  'timeout',
  'io',
  'internal',
  'shutting_down',
]);

/** 用法 / 参数错误：`exitCode` 固定 2（CLI 捕获后打一句人话）。 */
export class UsageError extends Error {
  constructor(message) {
    super(message);
    this.name = 'UsageError';
    this.exitCode = EXIT.usage;
  }
}

function isObj(v) {
  return v !== null && typeof v === 'object' && !Array.isArray(v);
}

function nonEmptyString(v) {
  return typeof v === 'string' && v.length > 0;
}

// =====================================================================================
// argv
// =====================================================================================

/** op 的"隐藏 flag"形式（`cli.mjs` 的薄转发要求每个动作的 argv[0] 是 `--xxx`）。 */
const OP_FLAG = Object.freeze({
  '--list': 'list',
  '--pause': 'pause',
  '--resume': 'resume',
  '--sample': 'sample',
  '--screenshot': 'screenshot',
  '--wait': 'wait',
  '--quit': 'quit',
  '--click': 'click',
  '--gc': 'gc',
});

const TAKES_VALUE = Object.freeze({
  '--session': 'session',
  '--timeout': 'timeout',
  '--control-root': 'root',
});

function parsePositiveNumber(raw, flag) {
  const n = Number(raw);
  if (!Number.isFinite(n) || n <= 0) throw new UsageError(`${flag} 需要一个正数（实际：${JSON.stringify(raw)}）`);
  return n;
}

/**
 * `--for-ms` / `wait` 的毫秒值：严格正数、上限 86 400 000（服务端 `parseCommand` 同一条边界）。
 * ⛔ 传 0 不是"跑 0 ms"，而是一个**非法谓词**（服务端会回 `bad_request`）⇒ 客户端先挡住。
 */
export function parsePositiveMs(raw, flag = '--for-ms') {
  const n = parsePositiveNumber(raw, flag);
  if (n > 86_400_000) throw new UsageError(`${flag} 超出上限 86400000 ms（实际：${n}）`);
  return n;
}

/** `--for frames N`：严格正**整数**（服务端要求 `number_unsigned` 且 != 0）。 */
/** `click` 的参考坐标上界（服务端 `cursor_map.h::kReferenceFrameW/H`；⛔ 客户端不另立一套）。 */
export const REFERENCE_W = 1280;
export const REFERENCE_H = 720;

/** `click` 的单轴坐标：非负整数、闭区间 `0..limit`（服务端 `readCoord` 同一条边界；⛔ 不 clamp）。 */
export function parseClickCoord(raw, flag, limit) {
  const text = String(raw);
  if (!/^\d+$/.test(text)) {
    throw new UsageError(`${flag} 必须是非负整数（0..${limit}；实际：${JSON.stringify(raw)}）`);
  }
  const n = Number(text);
  if (!Number.isSafeInteger(n) || n < 0 || n > limit) {
    throw new UsageError(`${flag} 必须在 0..${limit} 的闭区间内（实际：${JSON.stringify(raw)}）`);
  }
  return n;
}

/** `click` 的按钮：只有 `left` / `right`（服务端同一条枚举；`middle` 本轮不支持）。 */
export function parseClickButton(raw) {
  const button = String(raw);
  if (button !== 'left' && button !== 'right') {
    throw new UsageError(`click 的 button 只能是 left / right（实际：${JSON.stringify(raw)}）`);
  }
  return button;
}

export function parsePositiveFrames(raw, flag = '--for frames') {
  if (!/^\d+$/.test(String(raw))) {
    throw new UsageError(`${flag} 需要一个正整数（实际：${JSON.stringify(raw)}）`);
  }
  const n = Number(raw);
  if (!Number.isSafeInteger(n) || n <= 0) {
    throw new UsageError(`${flag} 需要一个正整数（实际：${JSON.stringify(raw)}）`);
  }
  return n;
}

/**
 * 解析 argv（位置 op 与隐藏 flag 两种入口都接受）。
 * @returns {{op:string|null, describe:boolean, help:boolean, json:boolean, session:string|null,
 *   timeoutSec:number, root:string, screenshotPath:string|null, waitMs:number|null,
 *   forMs:number|null, forFrames:number|null, dryRun:boolean}}
 */
export function parseArgv(argv = []) {
  const out = {
    op: null,
    describe: false,
    help: false,
    json: false,
    session: null,
    timeoutSec: COMMAND_TIMEOUT_SEC,
    root: DEFAULT_CONTROL_ROOT,
    screenshotPath: null,
    click: null,
    waitMs: null,
    forMs: null,
    forFrames: null,
    dryRun: false,
  };
  const pos = [];
  const setOp = (op) => {
    if (out.op !== null && out.op !== op) throw new UsageError(`一次只能有一个操作（${out.op} / ${op}）`);
    out.op = op;
  };

  for (let i = 0; i < argv.length; i += 1) {
    const a = String(argv[i]);
    if (a === '') continue;
    if (a === '--json') {
      out.json = true;
    } else if (a === '--dry-run') {
      out.dryRun = true;
    } else if (a === '--describe') {
      out.describe = true;
    } else if (a === '--help' || a === '-h') {
      out.help = true;
    } else if (a in OP_FLAG) {
      setOp(OP_FLAG[a]);
    } else if (a in TAKES_VALUE) {
      const key = TAKES_VALUE[a];
      const value = argv[i + 1];
      if (value === undefined || value === '') throw new UsageError(`${a} 需要一个值`);
      i += 1;
      if (key === 'session') out.session = String(value);
      else if (key === 'root') out.root = String(value);
      else out.timeoutSec = parsePositiveNumber(value, a);
    } else if (a === '--for-ms') {
      const value = argv[i + 1];
      if (value === undefined) throw new UsageError('--for-ms 需要一个值');
      i += 1;
      out.forMs = parsePositiveMs(value, '--for-ms');
    } else if (a === '--for') {
      const kind = argv[i + 1];
      const value = argv[i + 2];
      if (kind !== 'ms' && kind !== 'frames') throw new UsageError('--for 后面只能是 `ms` 或 `frames`');
      if (value === undefined) throw new UsageError(`--for ${kind} 需要一个值`);
      i += 2;
      if (kind === 'ms') out.forMs = parsePositiveMs(value, '--for ms');
      else out.forFrames = parsePositiveFrames(value, '--for frames');
    } else if (a.startsWith('--')) {
      throw new UsageError(`未知参数：${a}（--help 看用法）`);
    } else {
      pos.push(a);
    }
  }

  // `--help` / `--describe` 优先：带上位置参数（如 `gc --help`）也不该被当成"多余的参数"
  if (out.help || out.describe) return out;

  // 位置形式的 op：`node tools/emu-ctl.mjs sample`
  if (!out.op && !out.describe && !out.help && pos.length > 0) {
    const name = pos.shift();
    if (M1_OPS.includes(name)) {
      throw new UsageError(`「${name}」是 M1（输入投递）的剩余能力，服务端还没有这个 op；本客户端未实现（click 已解锁）。`);
    }
    if (!OPS.includes(name)) {
      throw new UsageError(
        `未知操作：${name}。可用：${OPS.join(' / ')}（M1 的 click/key 未实现；gc 只删残留端点文件）`,
      );
    }
    out.op = name;
  }

  // 这些 op 的**位置参数**
  if (out.op === 'screenshot' && out.screenshotPath === null) {
    if (pos.length === 0) throw new UsageError('screenshot 需要一个目标路径：`screenshot <path>`');
    out.screenshotPath = pos.shift();
  }
  if (out.op === 'click' && out.click === null) {
    if (pos.length < 2) throw new UsageError('click 需要坐标：`click <x> <y> [left|right]`');
    const x = parseClickCoord(pos.shift(), 'click x', REFERENCE_W - 1);
    const y = parseClickCoord(pos.shift(), 'click y', REFERENCE_H - 1);
    const button = pos.length > 0 ? parseClickButton(pos.shift()) : 'left';
    out.click = { x, y, button };
  }
  if (out.op === 'wait' && out.waitMs === null) {
    if (pos.length === 0) throw new UsageError('wait 需要一个毫秒数：`wait <ms>`');
    out.waitMs = parsePositiveMs(pos.shift(), 'wait');
  }
  if (pos.length > 0) throw new UsageError(`多余的参数：${pos.join(' ')}（--help 看用法）`);

  // --for-* 只对 resume 有意义（且两者互斥）
  if (out.forMs !== null && out.forFrames !== null) {
    throw new UsageError('--for-ms 与 --for frames 互斥（一次只能给一条重停谓词）');
  }
  if ((out.forMs !== null || out.forFrames !== null) && out.op !== 'resume') {
    throw new UsageError('--for-ms / --for frames 只能与 resume 一起用（`wait <ms>` 是 resume --for-ms 的糖）');
  }
  if (out.dryRun && out.op !== 'gc') {
    throw new UsageError('--dry-run 只对 gc 有意义（`gc --dry-run` 只报不删）');
  }

  if (!out.op && !out.describe && !out.help) {
    throw new UsageError(`缺少操作。可用：${OPS.join(' / ')}（--help 看用法）`);
  }
  return out;
}

// =====================================================================================
// 端点文件（`ctl.<pid>.<nonce8>.json`）
// =====================================================================================

/**
 * 解析**一个**端点文件。
 * @returns {{ok:true, entry:{file:string,pid:number,port:number,nonce:string,token:string,session:string,
 *   url:string,exe_sha256:string,shape:string,started_utc:string}} | {ok:false, why:'bad_file', detail:string}}
 *
 * ★ `bad_file` 只用于"文件本身不像端点文件"：JSON 坏 / schema 不对 / 缺 pid·port·nonce·token /
 *   文件名与内容不一致。**连接失败**由探测阶段给 `refused` / `timeout` / `nonce_mismatch`。
 */
export function parseCtlFile(name, text) {
  const bad = (detail) => ({ ok: false, why: 'bad_file', detail });
  const m = CTL_NAME_RE.exec(String(name));
  if (!m) return bad('文件名不是 ctl.<pid>.<nonce8>.json');
  let doc;
  try {
    doc = JSON.parse(String(text));
  } catch (err) {
    return bad(`JSON 解析失败：${err.message}`);
  }
  if (!isObj(doc)) return bad('顶层不是对象');
  if (doc.schema !== CTL_SCHEMA) return bad(`schema 不是 ${CTL_SCHEMA}（实际 ${JSON.stringify(doc.schema)}）`);
  if (!Number.isInteger(doc.pid) || doc.pid <= 0) return bad(`pid 不是正整数（实际 ${JSON.stringify(doc.pid)}）`);
  if (!Number.isInteger(doc.port) || doc.port < 1 || doc.port > 65535) {
    return bad(`port 不在 1..65535（实际 ${JSON.stringify(doc.port)}）`);
  }
  if (!nonEmptyString(doc.nonce) || !/^[0-9a-f]{8,64}$/i.test(doc.nonce)) {
    return bad(`nonce 不是 hex（实际 ${JSON.stringify(doc.nonce)}）`);
  }
  if (!nonEmptyString(doc.token)) return bad('token 不是非空字符串');
  // 文件名与内容必须自洽（同一份来源；⛔ 不要让"读错的旧文件"被当成另一个实例）
  if (Number(m[1]) !== doc.pid) return bad(`文件名 pid=${m[1]} 与内容 pid=${doc.pid} 不一致`);
  if (m[2].toLowerCase() !== doc.nonce.slice(0, 8).toLowerCase()) {
    return bad(`文件名 nonce8=${m[2]} 与内容 nonce=${doc.nonce} 的前 8 位不一致`);
  }
  return {
    ok: true,
    entry: {
      file: String(name),
      pid: doc.pid,
      port: doc.port,
      nonce: doc.nonce,
      token: doc.token,
      session: typeof doc.session === 'string' ? doc.session : '',
      // ★ URL 由 `port + token` **派生**（设计 §2.5 的"读文件 → 建 url"）：文件里的 `url` 是同一份
      //   事实的冗余副本，⛔ 不作为连接目标 —— 判据 9c 只改 `port`，派生才会连到"被占的那个端口"。
      url: `ws://127.0.0.1:${doc.port}/?token=${doc.token}`,
      exe_sha256: typeof doc.exe_sha256 === 'string' ? doc.exe_sha256 : '',
      shape: typeof doc.shape === 'string' ? doc.shape : '',
      started_utc: typeof doc.started_utc === 'string' ? doc.started_utc : '',
    },
  };
}

/** 只保留 `ctl.<pid>.<nonce8>.json` 的文件名（`.tmp` 中间件 / 别的文件都不碰）。 */
export function listCtlFiles(names) {
  return [...names]
    .filter((n) => CTL_NAME_RE.test(String(n)))
    .sort((a, b) => String(a).localeCompare(String(b)));
}

/**
 * `hello` 与端点文件的**同一性**校验（判据 9c 的核心：端口被别的服务占着 ⇒ nonce 不匹配）。
 * @returns {string|null} null = 一致；否则一句人话（客户端会归成 `nonce_mismatch`）。
 */
export function helloMismatch(hello, entry) {
  if (!isObj(hello)) return 'hello 不是对象';
  if (hello.schema !== CTL_SCHEMA) return `hello.schema 不是 ${CTL_SCHEMA}（实际 ${JSON.stringify(hello.schema)}）`;
  if (hello.nonce !== entry.nonce) return `hello.nonce 与端点文件不一致（${JSON.stringify(hello.nonce)} ≠ ${JSON.stringify(entry.nonce)}）`;
  if (hello.pid !== entry.pid) return `hello.pid 与端点文件不一致（${hello.pid} ≠ ${entry.pid}）`;
  if (hello.port !== entry.port) return `hello.port 与端点文件不一致（${hello.port} ≠ ${entry.port}）`;
  return null;
}

/**
 * 把"端点文件 + 探测结果"合成 `list` 的一行。
 * @param {object} entry `parseCtlFile` 的 entry
 * @param {{alive:boolean, hello?:object, why?:string, detail?:string, opened?:boolean}} probe
 */
export function listRow(entry, probe) {
  const dead = { session: entry.session || null, pid: entry.pid, port: entry.port, alive: false, why: probe.why, present: null };
  if (!probe.alive) return dead;
  const hello = probe.hello ?? {};
  return {
    session: typeof hello.session === 'string' && hello.session.length > 0 ? hello.session : entry.session || null,
    pid: entry.pid,
    port: entry.port,
    alive: true,
    why: 'ok',
    present: hello.present === 'visible' || hello.present === 'hidden' ? hello.present : null,
  };
}

/**
 * 从列表里**选中**一个实例（⛔ 不猜）。
 * @returns {{ok:true, entry:object} | {ok:false, code:4, reason:'none'|'not_found'|'multiple', entries:object[], session:string|null}}
 *
 * ★ 缺省时"恰好一个 alive"才自动选中；多个 ⇒ 全部列出 + 退出码 4。
 *   `--session` 精确匹配；匹配不到 / 匹配到多个（不应该）都退出码 4。
 */
export function selectEntry(entries, session = null) {
  const all = [...(entries ?? [])];
  if (session !== null && session !== undefined && session !== '') {
    const hits = all.filter((e) => e.alive === true && e.session === session);
    if (hits.length === 1) return { ok: true, entry: hits[0] };
    return { ok: false, code: EXIT.instance, reason: hits.length === 0 ? 'not_found' : 'multiple', entries: all, session };
  }
  const alive = all.filter((e) => e.alive === true);
  if (alive.length === 1) return { ok: true, entry: alive[0] };
  return { ok: false, code: EXIT.instance, reason: alive.length === 0 ? 'none' : 'multiple', entries: all, session: null };
}

// =====================================================================================
// gc（只删**残留端点文件**；⛔ 不碰 keep.txt / 别人的文件）
// =====================================================================================

/**
 * `gc` 的单文件判定（**纯函数**；CLI 只负责把文件系统事实填进来）。
 *
 * 删除必须**同时**满足三条（`AGENTS.md` 的"删文件要能点名目标"）：
 *   ① `connect()` 判死：`why ∈ {refused,timeout}`（`nonce_mismatch` = 端口上有个**别的**服务，⛔ 不删）；
 *   ② 文件里的 `pid` **不存活**（`pidAlive === false`；pid 复用 ⇒ pid 只是辅助，主判据仍是 ①）；
 *   ③ 文件名严格匹配 `ctl.<数字>.<8hex>.json`（⛔ `keep.txt` / 目录 / 别的名字一律不删）。
 *
 * ★ `item` 形状：`{name, isFile?, alive?, why?, pid?, port?, pidAlive?}`。
 *   `isFile === false`（目录 / 符号链接）⇒ keep；`bad_file` / `alive` / `nonce_mismatch` ⇒ keep。
 * @returns {{action:'remove'|'keep', why:string}}
 */
export function gcDecision(item) {
  if (!CTL_NAME_RE.test(String(item?.name ?? ''))) return { action: 'keep', why: 'name_mismatch' };
  if (item.isFile === false) return { action: 'keep', why: 'not_regular_file' };
  if (item.alive === true) return { action: 'keep', why: 'alive' };
  if (item.why !== 'refused' && item.why !== 'timeout') return { action: 'keep', why: item.why || 'unknown' };
  if (item.pidAlive === true) return { action: 'keep', why: 'pid_alive' };
  return { action: 'remove', why: 'dead' };
}

/**
 * 把逐文件判定合成 `gc` 的输出：`{removed,kept,why}`。
 * `why` 是 `name → 理由` 的映射（人读与 `--json` 共用同一份事实）。
 */
export function gcPlan(items) {
  const removed = [];
  const kept = [];
  const why = {};
  for (const item of items ?? []) {
    const d = gcDecision(item);
    const rec = {
      name: String(item?.name ?? ''),
      pid: Number.isInteger(item?.pid) ? item.pid : null,
      port: Number.isInteger(item?.port) ? item.port : null,
      action: d.action,
      why: d.why,
    };
    if (d.action === 'remove') removed.push(rec);
    else kept.push(rec);
    why[rec.name] = d.why;
  }
  return { removed, kept, why };
}

/** `--json` 的 gc 对象（`removed/kept/why` 是契约字段，其余是上下文）。 */
export function gcJson(root, plan, dryRun) {
  return {
    op: 'gc',
    root,
    dry_run: dryRun === true,
    ok: true,
    removed: plan.removed,
    kept: plan.kept,
    why: plan.why,
  };
}

function gcRecordLine(rec) {
  const tag = rec.action === 'remove' ? 'remove' : 'keep  ';
  const pp = rec.pid !== null || rec.port !== null ? ` pid=${rec.pid ?? '-'} port=${rec.port ?? '-'}` : '';
  return `${tag} ${rec.name}${pp} why=${rec.why}`;
}

/** `gc` 的人读全文（removed 与 kept 按文件名排序，稳定可 diff）。 */
export function formatGcText(root, plan, dryRun) {
  const rows = [...plan.removed, ...plan.kept].sort((a, b) => a.name.localeCompare(b.name));
  const out = [`root ${root}`];
  for (const rec of rows) out.push(gcRecordLine(rec));
  if (dryRun === true) out.push('(--dry-run：只报告，未删任何文件)');
  out.push(`removed ${plan.removed.length} · kept ${plan.kept.length}`);
  return out;
}

// =====================================================================================
// 退出码
// =====================================================================================

/**
 * 服务端 `res` → CLI 退出码。
 * 0 = ok（含 `script.available:false`，那是数据不是错误）· 3 = 端点侧 `timeout` · 1 = 其余 `ok:false`。
 */
export function exitCodeForReply(reply) {
  if (isObj(reply) && reply.ok === true) return EXIT.ok;
  if (isObj(reply) && isObj(reply.error) && reply.error.code === 'timeout') return EXIT.timeout;
  return EXIT.endpoint;
}

/** 客户端侧错误类别 → 退出码（`usage` 与 `protocol` 都是 2；`instance` 是 4）。 */
export function exitCodeForError(kind) {
  switch (kind) {
    case 'usage':
    case 'protocol':
      return EXIT.usage;
    case 'timeout':
      return EXIT.timeout;
    case 'instance':
      return EXIT.instance;
    default:
      return EXIT.endpoint;
  }
}

/** 服务端 `error.code` → 客户端错误类别（只认枚举；未知码不猜语义）。 */
export function errorKindOf(reply) {
  const code = isObj(reply) && isObj(reply.error) ? reply.error.code : null;
  if (code === 'timeout') return 'timeout';
  return 'endpoint';
}

// =====================================================================================
// 输出（人读 + JSON）
// =====================================================================================

function scriptHead(script) {
  if (!isObj(script)) return 'script:unavailable(no-sample)';
  if (script.available === true) return `${script.script}:${script.pos_hex}`;
  return `script:unavailable(${script.why ?? 'unknown'})`;
}

/**
 * `sample` 的人读行（缺省输出；判据的肉眼读数就是这一行）。
 * 形如：`TITLE.BIN:0x039C paused_at=block frame=129`
 */
export function formatSampleLine(sample) {
  const pausedAt = isObj(sample) && nonEmptyString(sample.paused_at) ? sample.paused_at : '-';
  const frame = isObj(sample) && isObj(sample.screen) && sample.screen.frame !== undefined ? sample.screen.frame : '-';
  return `${scriptHead(isObj(sample) ? sample.script : undefined)} paused_at=${pausedAt} frame=${frame}`;
}

/** `screenshot` 的人读行：路径 / 字节 / 帧 / 是否仍是同一帧 / 来源。 */
export function formatScreenshotLine(shot) {
  if (!isObj(shot)) return 'screenshot:missing';
  return `${shot.path} bytes=${shot.bytes} frame=${shot.frame} hash_still_matches=${shot.hash_still_matches} source=${shot.source}`;
}

/** `click` 的请求帧：**顶层** `x` / `y` / `button`（服务端 `parseCommand` 只读顶层；⛔ 不包 `args`）。 */
export function clickFrame(x, y, button = 'left') {
  return { op: 'click', x, y, button };
}

/** `pause` / `resume` / `wait` 的人读行。 */
export function formatStateLine(op, reply) {
  const d = isObj(reply) && isObj(reply.data) ? reply.data : {};
  const tail = d.guest_ms !== undefined ? ` guest_ms=${d.guest_ms}` : '';
  return `${op} ok paused=${d.paused} paused_at=${nonEmptyString(d.paused_at) ? d.paused_at : '-'} state_seq=${d.state_seq}${tail}`;
}

/** `click` 的人读行：坐标 / 按钮 / **入队深度**（`queued` ≠ 已施加；施加在下一个 guest 读点）。 */
export function formatClickLine(reply) {
  const d = isObj(reply) && isObj(reply.data) ? reply.data : {};
  const c = isObj(d.click) ? d.click : {};
  return `click ok x=${c.x} y=${c.y} button=${c.button} queued=${c.queued} state_seq=${d.state_seq}`;
}

/** `list` 的一行（人读）。JSON 版是 `listRow()` 的对象。 */
export function formatListLine(row) {
  const tag = row.alive ? 'alive' : 'dead ';
  return `${tag} pid=${row.pid} port=${row.port} why=${row.why} present=${row.present ?? '-'} session=${row.session ?? '-'}`;
}

/** `list` 的人读全文。 */
export function formatListText(root, rows) {
  const out = [`root ${root}`];
  if (!rows || rows.length === 0) {
    out.push('(没有 ctl.*.json —— 端点没起，或 --control-root 不是同一个目录)');
    return out;
  }
  for (const row of rows) out.push(formatListLine(row));
  const alive = rows.filter((r) => r.alive).length;
  out.push(`共 ${rows.length} 条 · alive ${alive} · dead ${rows.length - alive}`);
  return out;
}

/** `quit` 的人读行：bye 的 `{reason,requested_by}` 是判据 14 的读数面。 */
export function formatQuitLine(reply, bye) {
  const state = isObj(reply) && isObj(reply.data) ? `state_seq=${reply.data.state_seq}` : 'state_seq=?';
  if (isObj(bye)) return `quit ok reason=${bye.reason} requested_by=${bye.requested_by} ${state}`;
  return `quit ok bye=connection-closed ${state}`;
}

/**
 * `--json` 的**单个**对象（除 `list` 外）。`data` 是服务端 `res.data` 原样（不重排字段）。
 * ★ `bye` 只在 `quit` 里出现（它属于连接生命周期，不属于命令答复）。
 */
export function resultJson({ op, entry = null, reply = null, bye = null, error = null }) {
  const out = {
    op,
    session: entry ? entry.session || null : null,
    pid: entry ? entry.pid ?? null : null,
    port: entry ? entry.port ?? null : null,
    ok: isObj(reply) ? reply.ok === true : false,
  };
  if (isObj(reply) && reply.ok === true) out.data = reply.data ?? null;
  if (isObj(reply) && reply.ok !== true && isObj(reply.error)) out.error = reply.error;
  if (error) out.error = error;
  if (op === 'quit') out.bye = isObj(bye) ? bye : null;
  return out;
}

/** `--json` 的 list 对象。 */
export function listJson(root, rows) {
  return {
    op: 'list',
    root,
    count: rows.length,
    alive: rows.filter((r) => r.alive).length,
    entries: rows,
  };
}

// =====================================================================================
// 自描述
// =====================================================================================

export const DOMAIN = {
  id: 'emu-ctl',
  title: 'AOT 控制层**客户端**（`--control-root` 下的 `ctl.*.json`；停 / 采样 / 截图 / 点击 / 继续 / 收口 / 清残留）',
  data: [
    '`<--control-root>/ctl.<pid>.<nonce8>.json`（端点写的身份文件；`list` 只读判活，`gc` **只删残留的那一个**）',
    '`screenshot <path>` 的**客户端指定路径**（BMP；写文件 ⇒ mutates）',
    '`click <x> <y> [left|right]` 的**入队回执**（参考坐标 1280×720；⛔ 入队 ≠ 施加）',
  ],
  access:
    '`list` 只读；`gc` 只删"连接判死 + pid 不存活 + 文件名严格匹配"的残留端点文件（⛔ 不碰 `keep.txt` / 目录 / 别的文件）；`screenshot` 写客户端给的路径；`click` 只向端点的立即队列投一条（⛔ 不改 guest 状态）；`quit` 只请求端点自己收口',
  tool: 'tools/emu-ctl.mjs',
};

export const OPERATIONS = Object.freeze([
  { name: 'list', argv: ['--list'], mutates: false, summary: '扫 --control-root 下的 ctl.*.json，逐个 1.5 s 连一次；输出 {session,pid,port,alive,why,present}（有死条目也算 0；⛔ 不删文件）' },
  { name: 'pause', argv: ['--pause'], mutates: true, summary: '请求暂停并阻塞到汇点' },
  { name: 'resume', argv: ['--resume'], mutates: true, summary: '放行；`--for-ms N` / `--for frames N` ⇒ 放行后按谓词重停' },
  { name: 'sample', argv: ['--sample'], mutates: true, summary: '暂停（若未停）+ 快照（停在汇点才能采样；freeze + state_seq +1 ⇒ 会写状态）；人读一行 `TITLE.BIN:0x039C paused_at=… frame=…`（`script.available:false` 仍是 0）' },
  { name: 'screenshot', argv: ['--screenshot'], mutates: true, summary: '暂停（服务端 Screenshot 分支会停在汇点；客户端先发幂等 pause 兜底）+ 写"最后呈现那帧"到指定路径（BMP）' },
  { name: 'click', argv: ['--click'], mutates: true, summary: '投一条点击到立即队列（参考坐标 1280×720：x 0..1279 / y 0..719、button ∈ {left,right} 缺省 left）；**入队 ≠ 施加**：只接受暂停态（运行态 ⇒ not_paused / 退出码 1），施加在下一个 guest 读点，队列上界 64 条/次' },
  { name: 'wait', argv: ['--wait'], mutates: true, summary: '糖：等价于 `resume --for-ms <ms>`（控制层 wait = 让 guest 跑 N ms 后重停；⛔ 与 --input-plan 的 wait 不是一回事）' },
  { name: 'quit', argv: ['--quit'], mutates: true, summary: '请求收口，等到 bye 或连接断（bye.reason / requested_by 是读数）' },
  { name: 'gc', argv: ['--gc'], mutates: true, summary: '只删残留端点文件：connect 判死（refused/timeout）+ pid 不存活 + 文件名严格匹配 ctl.<pid>.<8hex>.json；⛔ 不动 keep.txt 等其它文件；`--dry-run` 只报不删' },
]);

export function describe() {
  return {
    domain: DOMAIN,
    operations: OPERATIONS.map((o) => ({ name: o.name, mutates: o.mutates, summary: o.summary })),
    exitCodes: {
      '0': '成功（含 script.available:false）',
      '1': '端点回 ok:false',
      '2': '参数 / 协议错（含 click 坐标/按钮非法、key 仍是 M1 未实现、gc 的 root 不可读 / 删除失败）',
      '3': '超时（端点仍活）',
      '4': '找不到 / 多个实例（不猜；`--session <id>` 精确选中）',
    },
    listWhy: WHY,
    selection:
      '`--session <id>` 精确选中；缺省时**恰好一个 alive** 才自动选中，多个 ⇒ 退出码 4 并全部列出；零个 ⇒ 4。',
    gc: {
      deletes:
        '**只**删同时满足三条的残留端点文件：① `connect()` 判死（`why ∈ {refused,timeout}`；`nonce_mismatch` = 端口上有个别的服务 ⇒ 不删）② 文件里的 `pid` 不存活（`kill(pid,0)`；pid 复用 ⇒ pid 只是辅助，主判据仍是 ①）③ 文件名严格匹配 `ctl.<数字>.<8hex>.json`。',
      never:
        '⛔ 不碰 `keep.txt`、目录、符号链接、`*.json.tmp`、以及任何不匹配该模式的文件；⛔ `list` 永远只报告、不删（gc 是唯一删文件的动作）。',
      output: '`--json` = `{removed:[…],kept:[…],why:{name→理由}}`；`--dry-run` 只报不删；退出码 0（删了或没得删）· 2（参数 / root 不可读 / 删除失败）。',
    },
    click: {
      reference: '参考坐标 = **1280×720**：`x ∈ [0,1279]`、`y ∈ [0,719]` 的整数（闭区间；⛔ 不 clamp、⛔ 不猜）；`button ∈ {left,right}`，缺省 `left`；本地校验失败 ⇒ 退出码 2（省一次往返）。',
      queue:
        '**入队 ≠ 施加**：`click` 只把一条立即点击放进队列；施加点是**下一个 guest 读点**。⇒ 它**只接受暂停态**（运行态服务端回 `not_paused` ⇒ 退出码 1）；队列上界 **64 条/次**，满了服务端回 `bad_request`（⇒ 退出码 1）。回执里的 `queued` 是入队后的深度，不是"已施加"。',
      frame: '请求帧是**顶层字段** `{op:"click",x,y,button}`（服务端 `parseCommand` 只读顶层；⛔ 不包 `{"args":{…}}`）。',
    },
    clockSemantics: {
      sample: '`sample.data.sample.clock.paused_total_ms` 是**已结算**口径（`pausedCommittedMs()`，**不含**当前这段未结束的冻结区间）——判据 3b 要求同一暂停点连采两次逐字节相同。',
      pauseResume: '`pause` / `resume` 的 `res.data.paused_total_ms` 是**含当前冻结区间**口径（`pausedTotalMs()`）。同名不同口径，⛔ 不要把两者当同一个数。',
    },
    transport: {
      websocket: 'Node 内置 `WebSocket`（Node ≥ 21；⛔ 零新依赖、⛔ 不 spawn 子进程）',
      connectOrder: '连接后先读 `hello` 才发命令；每条 res 校验 `nonce == hello.nonce`，不一致 ⇒ 退出码 2',
      timeouts: `list 单条目 ${CONNECT_TIMEOUT_MS} ms；命令缺省 ${COMMAND_TIMEOUT_SEC} s（--timeout <s>）`,
      requestShape:
        '请求字段是**顶层的** `for_ms` / `for_frames` / `path` / `x` / `y` / `button`（服务端 `parseCommand` 的实现；⛔ 不是 `{"args":{…}}`）',
      origin: '客户端不发 `Origin`（服务端带 Origin 一律 403；错 token ⇒ 401）',
    },
    traps: {
      wait:
        '控制层 `wait <ms>` = 让 guest 跑 N ms 后**重停**（= `resume --for-ms`）；`--input-plan` 里的 `wait` 是配方 DSL 的一条 deadline —— 两者不是一回事。',
      criteria:
        '⛔ 验收判据不要用 wait：要能红就用 `sample` 的 script/pos、`state_seq`、`insns`、`hash_still_matches`。',
    },
  };
}

export function describeText() {
  const d = describe();
  const L = [];
  L.push(`emu-ctl — ${d.domain.title}`);
  L.push('');
  L.push('DOMAIN');
  for (const x of d.domain.data) L.push(`  数据    ${x}`);
  L.push(`  读写    ${d.domain.access}`);
  L.push('');
  L.push('OPERATIONS');
  for (const o of d.operations) L.push(`  ${o.name.padEnd(11)}${o.mutates ? '(会写)' : '(只读)'} ${o.summary}`);
  L.push('');
  L.push('退出码');
  for (const [code, text] of Object.entries(d.exitCodes)) L.push(`  ${code}  ${text}`);
  L.push('');
  L.push(`list 的 why：${d.listWhy.join(' / ')}`);
  L.push(`选中：${d.selection}`);
  L.push('');
  L.push('gc（只删残留端点文件，⛔ 不动其它文件）');
  L.push(`  删：${d.gc.deletes}`);
  L.push(`  不碰：${d.gc.never}`);
  L.push(`  输出：${d.gc.output}`);
  L.push('');
  L.push('click（入队 ≠ 施加；参考坐标 1280×720）');
  L.push(`  坐标：${d.click.reference}`);
  L.push(`  队列：${d.click.queue}`);
  L.push(`  帧：${d.click.frame}`);
  L.push('');
  L.push('时钟口径（同名不同口径）');
  L.push(`  sample：${d.clockSemantics.sample}`);
  L.push(`  pause/resume：${d.clockSemantics.pauseResume}`);
  L.push('');
  L.push(`传输：${d.transport.websocket}`);
  L.push(`      ${d.transport.connectOrder}`);
  L.push(`      ${d.transport.timeouts}`);
  L.push(`      请求形状：${d.transport.requestShape}`);
  L.push('');
  L.push('★ 同名陷阱');
  L.push(`  ${d.traps.wait}`);
  L.push(`  ${d.traps.criteria}`);
  L.push('');
  L.push('⛔ 未实现：key（M1 的剩余能力，服务端还没有；传了 ⇒ 退出码 2）· "自动挑一个"实例（多个 alive ⇒ 退出码 4）');
  return `${L.join('\n')}\n`;
}
