#!/usr/bin/env node
/**
 * tools/emu-ctl.mjs — **AOT 控制层的独立客户端**（`apps/emulator-lift-aot --control-root` 的对面）
 *
 *   经派发器：`pnpm tools emu-ctl <list|pause|resume|sample|screenshot|click|wait|quit|gc> [args…]`
 *   也可独立跑：`node tools/emu-ctl.mjs list`
 *
 * ★ 边界（这是本文件唯一做的事）：
 *   · **发现** = 读 `<--control-root>/ctl.<pid>.<nonce8>.json`（缺省 `<repo>/.tmp/emu-ctl`），
 *     逐个 `127.0.0.1:<port>` 连一次并校验 `hello.nonce` / `hello.pid`（`list` 的 1.5 s 预算）；
 *   · **传输** = Node **内置 `WebSocket`**（Node ≥ 21；⛔ 零新依赖、⛔ 不 spawn 子进程 —— §5.1）；
 *   · **命令** = 连接后**先读 `hello`** 才发；每条 `res` 校验 `nonce == hello.nonce`（不一致 ⇒ 退出码 2）。
 * ★ 协议真源是服务端代码 `apps/emulator-lift/src/runtime/control_ws.cpp`：
 *   请求的谓词/路径字段是**顶层** `for_ms` / `for_frames` / `path`（⛔ 不是 `{"args":{…}}`）。
 * ★ `click` 已解锁（顶层 `{op,x,y,button}`；参考坐标 1280×720；只接受暂停态）；⛔ `key` 仍是 M1 未实现
 *   （传了明确回"未实现" + 退出码 2）；
 *   `gc` **只**删残留端点文件（connect 判死 + pid 不存活 + 文件名严格匹配），⛔ 不碰 keep.txt 等其它文件；
 *   `list` 永远只报告、不删；⛔ 不"自动挑一个"实例（多个 alive ⇒ 要挑实例的 op 退出码 4 + 全部列出；
 *   `list` 恒 0，多实例也 0）。
 */
import fs from 'node:fs';
import net from 'node:net';
import path from 'node:path';
import process from 'node:process';
import { fileURLToPath } from 'node:url';

import * as L from './lib/emu-ctl.mjs';

export const DOMAIN = L.DOMAIN;
export const OPERATIONS = L.OPERATIONS;
export const describe = L.describe;
export const describeText = L.describeText;

const HELP = `tools/emu-ctl.mjs — AOT 控制层客户端（Node 内置 WebSocket；零新依赖）

  node tools/emu-ctl.mjs list [--json] [--control-root <dir>]
  node tools/emu-ctl.mjs pause [--json] [--session <id>] [--timeout <s>]
  node tools/emu-ctl.mjs resume [--for-ms N | --for frames N] [--json] [--session <id>] [--timeout <s>]
  node tools/emu-ctl.mjs sample [--json] [--session <id>] [--timeout <s>]
  node tools/emu-ctl.mjs screenshot <path> [--json] [--session <id>] [--timeout <s>]
  node tools/emu-ctl.mjs click <x> <y> [left|right] [--json] [--session <id>] [--timeout <s>]
  node tools/emu-ctl.mjs wait <ms> [--json] [--session <id>] [--timeout <s>]
  node tools/emu-ctl.mjs quit [--json] [--session <id>] [--timeout <s>]
  node tools/emu-ctl.mjs gc [--dry-run] [--json] [--control-root <dir>]
  node tools/emu-ctl.mjs --describe [--json] | --help
  pnpm tools emu-ctl <list|pause|resume|sample|screenshot|click|wait|quit|gc> [args…]

  --control-root <dir>   端点目录（缺省 <repo>/.tmp/emu-ctl；与服务端 --control-root 同一个）
  --session <id>         精确选中一个实例；缺省时**恰好一个 alive** 才自动选中
  --timeout <s>          命令超时（缺省 ${L.COMMAND_TIMEOUT_SEC} s；超时 ⇒ 退出码 3）
  --json                 stdout **只有一个** JSON 对象（人读行只出现在缺省模式；诊断走 stderr）
  --for-ms N             放行 guest 跑 N ms 后重停（guest 时钟；= \`wait <ms>\`）
  --for frames N         放行 guest 直到 Present 帧号 +N 后重停
                         ⚠ 帧不推进的屏上服务端会回 error.code:"timeout"（退出码 3）
  --dry-run              只对 gc 有效：只报告"会删哪些"，不删任何文件

退出码
  0  成功（含 sample.script.available:false —— 数据缺失不是错误）
  1  端点回 ok:false（或连接在答复前断开）
  2  参数 / 协议错（含 nonce 不一致、click 坐标/按钮非法、key = M1 未实现、gc 的 root 不可读 / 删除失败）
  3  超时（端点仍活）
  4  找不到 / 多个实例（**仅"要挑一个实例"的 op**：pause/resume/sample/screenshot/click/wait/quit；
     不猜，用 --session <id> 精确选中；⛔ \`list\` 恒 0，多实例也 0）

★ \`list\` 的 why ∈ {ok, refused, timeout, nonce_mismatch, bad_file}
  · kill -9 的残留文件 ⇒ alive:false, why:"refused"，**文件仍在**（list 不删；要清就用 gc）
  · 端口被别的服务占着 ⇒ alive:false, why:"nonce_mismatch"（hello 对不上）或 "bad_file"
  · 有死条目时 list **仍然退出码 0**（那是事实，不是错误）

★ click（**入队 ≠ 施加**；参考坐标 1280×720）：
  \`click <x> <y> [left|right]\`：x ∈ [0,1279]、y ∈ [0,719] 的整数（闭区间），button ∈ {left,right} 缺省 left；
  只把一条点击放进**立即队列**，施加点是**下一个 guest 读点** ⇒ **只接受暂停态**（运行态端点回
  \`not_paused\` ⇒ 退出码 1）。队列上界 **64 条/次**，满了端点回 \`bad_request\`（⇒ 退出码 1）。
  本地坐标/按钮非法 ⇒ 退出码 2（省一次往返）。请求帧是顶层 \`{op:"click",x,y,button}\`（⛔ 不包 args）。

★ gc（**只删残留端点文件**，⛔ 不动其它文件）：
  删 = ① connect 判死（refused/timeout）② 文件里的 pid 不存活（kill(pid,0)；pid 复用 ⇒ pid 只是辅助）
      ③ 文件名严格匹配 ctl.<数字>.<8hex>.json。
  ⛔ keep.txt / 目录 / 符号链接 / ctl.*.json.tmp / 别的名字一律不碰；\`--dry-run\` 只报不删。
  \`--json\` = {removed:[…],kept:[…],why:{name→理由}}；退出码 0（删了或没得删）· 2（root 不可读 / 删除失败）。

★ 时钟口径（同名**不同口径**，⛔ 别当同一个数）：
  sample 的 clock.paused_total_ms = **已结算**（不含当前这段未结束的冻结区间；判据 3b 要同一暂停点两次逐字节相同）；
  pause / resume 的 res.paused_total_ms = **含当前冻结区间**。

★ **同名陷阱**：控制层 \`wait <ms>\` = 让 guest 跑 N ms 后**重停**（等价 resume --for-ms）；
  \`--input-plan\` 里的 \`wait\` 是**配方 DSL 的一条 deadline**。两者**不是一回事**，
  ⛔ 不要把这里的 wait 实现成"往 InputPlan 里塞一条 step"。
  ⚠ \`wait\` 与 \`resume --for-ms\` 一样**要求端点已暂停**（它不自己先 pause；先 \`pause\` 或 \`sample\`）。
  ⛔ 验收判据不要用 wait：要能红就用 sample 的 script/pos、state_seq、insns。
  ★ 截图是 present 时冻结的那一帧；\`hash_still_matches\` 只回答"冻结有没有生效"，⛔ 别拿它当判据
    （判内容要判非全黑 / 与同帧 \`--dump-frame\` 对照）。

⛔ 未实现：key（M1 的剩余能力，服务端还没有这个 op）· "自动挑一个"实例（要挑实例的 op 在多个 alive 时 ⇒ 退出码 4；\`list\` 恒 0）
`;

/** 客户端侧失败（不是服务端的 `ok:false`）。`kind` 决定退出码（见 lib 的 `exitCodeForError`）。 */
class ClientError extends Error {
  constructor(kind, message) {
    super(message);
    this.name = 'ClientError';
    this.kind = kind;
  }
}

function say(text) {
  process.stdout.write(text.endsWith('\n') ? text : `${text}\n`);
}

function warn(text) {
  process.stderr.write(text.endsWith('\n') ? text : `${text}\n`);
}

function remaining(deadline) {
  return Math.max(1, deadline - Date.now());
}

async function frameText(data) {
  if (typeof data === 'string') return data;
  if (data && typeof data.text === 'function') return await data.text();
  if (data instanceof ArrayBuffer) return Buffer.from(data).toString('utf8');
  return String(data);
}

function errorDetail(ev) {
  const err = ev?.error ?? ev;
  return String(err?.message ?? err?.type ?? '连接错误');
}

// =====================================================================================
// 端点发现
// =====================================================================================

/** 只做 TCP 可达性：把 `refused` / `timeout` 与"连上了但不是我们的服务"分开（判据 9a/9c）。 */
function tcpReachable(port, timeoutMs) {
  return new Promise((resolve) => {
    let done = false;
    const sock = net.connect({ host: '127.0.0.1', port });
    const finish = (r) => {
      if (done) return;
      done = true;
      try {
        sock.destroy();
      } catch {
        /* 已经关了 */
      }
      resolve(r);
    };
    sock.setTimeout(Math.max(1, timeoutMs));
    sock.once('connect', () => finish({ ok: true }));
    sock.once('timeout', () => finish({ ok: false, why: 'timeout', detail: `TCP 连接超时（${timeoutMs} ms）` }));
    sock.once('error', (err) => {
      finish({
        ok: false,
        // 本地 loopback 上连不上就是 refused（ECONNREFUSED / EHOSTUNREACH / EACCES 都归这一格：
        // `timeout` 只留给"TCP 连接本身超时"，那样才是端点可能还活但连不上）。
        why: 'refused',
        detail: `TCP 连接失败：${err?.code ?? err?.message ?? 'unknown'}`,
      });
    });
  });
}

/**
 * `list` 的单条目探测：TCP 可达 → WS 握手 → 第一帧必须是**身份匹配**的 `hello`。
 * @returns {{alive:true, hello:object} | {alive:false, why:string, detail:string}}
 */
function wsHello(entry, timeoutMs) {
  return new Promise((resolve) => {
    let ws = null;
    let done = false;
    let opened = false;
    const finish = (r) => {
      if (done) return;
      done = true;
      clearTimeout(timer);
      try {
        ws?.close();
      } catch {
        /* 已经关了 */
      }
      resolve(r);
    };
    const timer = setTimeout(() => {
      finish(
        opened
          ? { alive: false, why: 'timeout', detail: `${timeoutMs} ms 内没收到 hello` }
          : {
              alive: false,
              why: 'nonce_mismatch',
              detail: 'TCP 连上了，但 WS 握手没有在预算内完成（像是一个不说话的陌生服务）',
            },
      );
    }, Math.max(1, timeoutMs));

    try {
      ws = new WebSocket(entry.url);
    } catch (err) {
      finish({ alive: false, why: 'nonce_mismatch', detail: `WebSocket 构造失败：${err.message}` });
      return;
    }
    ws.addEventListener('open', () => {
      opened = true;
    });
    ws.addEventListener('message', (ev) => {
      frameText(ev.data)
        .then((text) => {
          let hello;
          try {
            hello = JSON.parse(text);
          } catch {
            finish({ alive: false, why: 'nonce_mismatch', detail: '第一帧不是 JSON（不是 emu-ctl 端点？）' });
            return;
          }
          const bad = L.helloMismatch(hello, entry);
          if (bad) finish({ alive: false, why: 'nonce_mismatch', detail: bad });
          else finish({ alive: true, hello });
        })
        .catch((err) => finish({ alive: false, why: 'nonce_mismatch', detail: `读帧失败：${err.message}` }));
    });
    ws.addEventListener('error', (ev) => {
      finish({
        alive: false,
        why: 'nonce_mismatch',
        detail: `WS 握手失败：${errorDetail(ev)}（token 错 / 不是 WS 服务 / 被拒）`,
      });
    });
    ws.addEventListener('close', () => {
      finish({
        alive: false,
        why: 'nonce_mismatch',
        detail: '连接在 hello 之前关闭（不是 emu-ctl 端点？）',
      });
    });
  });
}

/** 探测一个**通过文件校验**的端点。单条目预算 `timeoutMs`（`list` 用 1.5 s）。 */
async function probeEntry(entry, timeoutMs) {
  const started = Date.now();
  const tcp = await tcpReachable(entry.port, Math.min(500, Math.max(100, timeoutMs - 200)));
  if (!tcp.ok) return { alive: false, why: tcp.why, detail: tcp.detail };
  const left = Math.max(100, timeoutMs - (Date.now() - started));
  return await wsHello(entry, left);
}

/**
 * 读 + 校验 + 探测整个 root。**串行**（设计 §2.5：逐个连一次）。
 * @returns {{records:Array<{name:string,entry:object|null,row:object}>, rows:object[]}}
 */
async function probeAll(root, timeoutMs) {
  let names;
  try {
    names = fs.readdirSync(root);
  } catch (err) {
    if (err.code === 'ENOENT') return { records: [], rows: [] };
    throw new ClientError('usage', `读不了 --control-root ${root}：${err.message}`);
  }
  const records = [];
  for (const name of L.listCtlFiles(names)) {
    const pidMatch = L.CTL_NAME_RE.exec(name);
    const pid = pidMatch ? Number(pidMatch[1]) : null;
    let text;
    try {
      text = fs.readFileSync(path.join(root, name), 'utf8');
    } catch (err) {
      const row = { session: null, pid, port: null, alive: false, why: 'bad_file', present: null };
      records.push({ name, entry: null, row });
      records[records.length - 1].detail = `读不了：${err.message}`;
      continue;
    }
    const parsed = L.parseCtlFile(name, text);
    if (!parsed.ok) {
      const row = { session: null, pid, port: null, alive: false, why: parsed.why, present: null };
      records.push({ name, entry: null, row, detail: parsed.detail });
      continue;
    }
    const probe = await probeEntry(parsed.entry, timeoutMs);
    const row = L.listRow(parsed.entry, probe);
    const rec = { name, entry: parsed.entry, row };
    if (!probe.alive && probe.detail) rec.detail = probe.detail;
    records.push(rec);
  }
  return { records, rows: records.map((r) => r.row) };
}

// =====================================================================================
// 连接 / 一问一答
// =====================================================================================

/** 一个已打开的连接：事件队列 + 等待者（消息 / close / error 都进同一条流）。 */
class WsLink {
  constructor(ws) {
    this.ws = ws;
    this.queue = [];
    this.waiters = [];
    this.closed = false;
    this.closeWaiters = [];
    this.chain = Promise.resolve();
    ws.addEventListener('message', (ev) => {
      this.chain = this.chain.then(async () => {
        const text = await frameText(ev.data);
        this.push({ type: 'message', text });
      });
    });
    ws.addEventListener('error', (ev) => this.push({ type: 'error', detail: errorDetail(ev) }));
    ws.addEventListener('close', (ev) => {
      this.closed = true;
      this.push({ type: 'close', code: ev.code, reason: ev.reason });
      for (const fn of this.closeWaiters.splice(0)) fn();
    });
  }

  push(ev) {
    const waiter = this.waiters.shift();
    if (waiter) waiter(ev);
    else this.queue.push(ev);
  }

  next(timeoutMs) {
    if (this.queue.length > 0) return Promise.resolve(this.queue.shift());
    if (this.closed) return Promise.resolve({ type: 'close', code: 1000, reason: 'closed' });
    return new Promise((resolve, reject) => {
      let settled = false;
      const waiter = (ev) => {
        if (settled) return;
        settled = true;
        clearTimeout(timer);
        resolve(ev);
      };
      const timer = setTimeout(() => {
        if (settled) return;
        settled = true;
        this.waiters = this.waiters.filter((w) => w !== waiter);
        reject(new ClientError('timeout', `等待端点答复超时（${timeoutMs} ms）`));
      }, Math.max(1, timeoutMs));
      this.waiters.push(waiter);
    });
  }

  send(obj) {
    try {
      this.ws.send(JSON.stringify(obj));
    } catch (err) {
      throw new ClientError('endpoint', `发送失败：${err.message}`);
    }
  }

  close() {
    return new Promise((resolve) => {
      if (this.closed) return resolve();
      let settled = false;
      const done = () => {
        if (settled) return;
        settled = true;
        clearTimeout(timer);
        resolve();
      };
      const timer = setTimeout(done, 300);
      this.closeWaiters.push(done);
      try {
        this.ws.close(1000, 'emu-ctl done');
      } catch {
        done();
      }
    });
  }
}

/** 建连并**先读 hello**（身份不一致 ⇒ `protocol` = 退出码 2）。 */
async function openLink(entry, timeoutMs) {
  const deadline = Date.now() + timeoutMs;
  let ws;
  try {
    ws = new WebSocket(entry.url);
  } catch (err) {
    throw new ClientError('endpoint', `连不上 ws://127.0.0.1:${entry.port}：${err.message}`);
  }
  const link = new WsLink(ws);
  for (;;) {
    let ev;
    try {
      ev = await link.next(remaining(deadline));
    } catch {
      await link.close();
      throw new ClientError('timeout', `连接 ${entry.url} 后 ${timeoutMs} ms 内没读到 hello`);
    }
    if (ev.type === 'message') {
      let hello;
      try {
        hello = JSON.parse(ev.text);
      } catch {
        await link.close();
        throw new ClientError('protocol', 'hello 不是 JSON');
      }
      const bad = L.helloMismatch(hello, entry);
      if (bad) {
        await link.close();
        throw new ClientError('protocol', `端点身份不一致：${bad}`);
      }
      return { link, hello };
    }
    if (ev.type === 'error') {
      await link.close();
      throw new ClientError('endpoint', `连接错误：${ev.detail}`);
    }
    await link.close();
    throw new ClientError('endpoint', '连接在 hello 之前关闭');
  }
}

/** 发一条命令，读到 `id` 匹配的 `res`；每条 `res` 都校验 `nonce`。 */
async function callOp(link, hello, timeoutMs, command, id = 1) {
  const deadline = Date.now() + timeoutMs;
  link.send({ id, ...command });
  for (;;) {
    const ev = await link.next(remaining(deadline));
    if (ev.type === 'message') {
      let msg;
      try {
        msg = JSON.parse(ev.text);
      } catch {
        throw new ClientError('protocol', '端点回了非 JSON 帧');
      }
      if (msg && msg.event === 'bye') {
        throw new ClientError(
          'endpoint',
          `端点在答复前发了 bye（reason=${msg.reason} requested_by=${msg.requested_by}）`,
        );
      }
      if (!msg || typeof msg !== 'object' || !Object.hasOwn(msg, 'ok')) {
        throw new ClientError('protocol', '答复没有 ok 字段（不是 emu-ctl/2 的 res）');
      }
      if (msg.nonce !== hello.nonce) {
        throw new ClientError(
          'protocol',
          `res.nonce 与 hello.nonce 不一致（${JSON.stringify(msg.nonce)} ≠ ${JSON.stringify(hello.nonce)}）`,
        );
      }
      if (msg.id !== id) continue; // 一问一答；不是我们的答复就跳过（前向兼容）
      return msg;
    }
    if (ev.type === 'close') {
      throw new ClientError('endpoint', `连接在答复前关闭（code=${ev.code}${ev.reason ? ` ${ev.reason}` : ''}）`);
    }
    throw new ClientError('endpoint', `连接错误：${ev.detail}`);
  }
}

/** `quit` 之后等 `bye`（或连接断）。返回 bye 对象，连接断返回 null；超时抛 `timeout`。 */
async function waitBye(link, hello, timeoutMs) {
  const deadline = Date.now() + timeoutMs;
  for (;;) {
    const ev = await link.next(remaining(deadline));
    if (ev.type === 'close') return null;
    if (ev.type === 'error') return null;
    let msg;
    try {
      msg = JSON.parse(ev.text);
    } catch {
      continue;
    }
    if (msg && msg.event === 'bye') {
      if (msg.nonce !== hello.nonce) {
        throw new ClientError('protocol', 'bye.nonce 与 hello.nonce 不一致');
      }
      return msg;
    }
  }
}

// =====================================================================================
// 命令面
// =====================================================================================

function buildCommand(parsed) {
  if (parsed.op === 'pause') return { op: 'pause' };
  if (parsed.op === 'sample') return { op: 'sample' };
  if (parsed.op === 'quit') return { op: 'quit' };
  if (parsed.op === 'screenshot') {
    // 相对路径按**客户端** cwd 解析：端点的 cwd 通常不是你的 cwd。
    return { op: 'screenshot', path: path.resolve(process.cwd(), parsed.screenshotPath) };
  }
  if (parsed.op === 'click') {
    // ★ 顶层字段（服务端 parseCommand 只读顶层；⛔ 不包 args）。
    return L.clickFrame(parsed.click.x, parsed.click.y, parsed.click.button);
  }
  if (parsed.op === 'wait') return { op: 'resume', for_ms: parsed.waitMs };
  if (parsed.op === 'resume') {
    if (parsed.forMs !== null) return { op: 'resume', for_ms: parsed.forMs };
    if (parsed.forFrames !== null) return { op: 'resume', for_frames: parsed.forFrames };
    return { op: 'resume' };
  }
  throw new ClientError('usage', `内部：buildCommand 不认识 ${parsed.op}`);
}

function instanceError(parsed, sel) {
  const message =
    sel.reason === 'none'
      ? `没有 alive 的端点（--control-root ${parsed.root}）`
      : sel.reason === 'not_found'
        ? `没有 session 为 ${JSON.stringify(sel.session)} 的 alive 端点`
        : `有 ${sel.entries.filter((e) => e.alive).length} 个 alive 端点，必须用 --session 选一个（⛔ 不猜）`;
  return {
    code: sel.reason === 'multiple' ? 'multiple_instances' : 'no_instance',
    message,
  };
}

// =====================================================================================
// gc：只删**残留端点文件**（⛔ 不碰 keep.txt / 目录 / 别的文件）
// =====================================================================================

/** `kill(pid,0)` 的跨平台等价（Windows 走 Node 的 process.kill；EPERM = 存在但没权限）。 */
function isPidAlive(pid) {
  if (!Number.isInteger(pid) || pid <= 0) return false;
  try {
    process.kill(pid, 0);
    return true;
  } catch (err) {
    return err != null && err.code === 'EPERM';
  }
}

/**
 * 扫 root，把**每个文件**变成 `gcDecision` 的输入。
 * ★ 目录 / 符号链接 / 不匹配文件名的东西也进来（`gcDecision` 会 keep + 理由），
 *   这样 `gc` 的输出能显示"我看过它、我决定不碰它"——判据 9b 的 keep.txt 就在这一格。
 * ★ 只对 `ctl.*.json` 真去 connect；`bad_file` / 非 ctl 文件不连接、不解析。
 */
async function scanGcItems(root) {
  let dirents;
  try {
    dirents = fs.readdirSync(root, { withFileTypes: true });
  } catch (err) {
    if (err.code === 'ENOENT') return []; // 没有这个 root = 没得删（不是错误）
    throw new ClientError('usage', `读不了 --control-root ${root}：${err.message}`);
  }
  const items = [];
  for (const d of [...dirents].sort((a, b) => a.name.localeCompare(b.name))) {
    const name = d.name;
    const isFile = d.isFile();
    if (!isFile || !L.CTL_NAME_RE.test(name)) {
      items.push({ name, isFile });
      continue;
    }
    const pid = Number(L.CTL_NAME_RE.exec(name)[1]);
    let text;
    try {
      text = fs.readFileSync(path.join(root, name), 'utf8');
    } catch {
      items.push({ name, isFile: true, pid, why: 'bad_file' });
      continue;
    }
    const parsed = L.parseCtlFile(name, text);
    if (!parsed.ok) {
      items.push({ name, isFile: true, pid, why: 'bad_file' });
      continue;
    }
    const probe = await probeEntry(parsed.entry, L.CONNECT_TIMEOUT_MS);
    const row = L.listRow(parsed.entry, probe);
    const item = {
      name,
      isFile: true,
      pid: parsed.entry.pid,
      port: parsed.entry.port,
      alive: row.alive,
      why: row.why,
    };
    // pid 只是**辅助**：只有 connect 判死时才看它（非 refused/timeout 一律不删）。
    if (row.alive !== true && (row.why === 'refused' || row.why === 'timeout')) {
      item.pidAlive = isPidAlive(parsed.entry.pid);
    }
    items.push(item);
  }
  return items;
}

async function cmdGc(parsed) {
  let plan;
  try {
    plan = L.gcPlan(await scanGcItems(parsed.root));
  } catch (err) {
    const e = err instanceof ClientError ? err : new ClientError('usage', err.message);
    if (parsed.json) {
      say(
        JSON.stringify(
          { op: 'gc', root: parsed.root, ok: false, error: { code: 'usage', message: e.message } },
          null,
          2,
        ),
      );
    } else {
      warn(`emu-ctl gc 失败：${e.message}`);
    }
    return L.EXIT.usage;
  }

  const rootAbs = path.resolve(parsed.root);
  const failures = [];
  const failedNames = new Set();
  if (parsed.dryRun !== true) {
    for (const rec of plan.removed) {
      // ★ 删之前**逐字校验目标路径**：绝对路径的父目录必须是 rootAbs、basename 必须原样、
      //   文件名必须仍严格匹配 `ctl.<pid>.<nonce8>.json`（⛔ 不删任何"计算出来的"路径）。
      const target = path.resolve(rootAbs, rec.name);
      if (
        path.dirname(target) !== rootAbs ||
        path.basename(target) !== rec.name ||
        !L.CTL_NAME_RE.test(rec.name)
      ) {
        failures.push(`${rec.name}: 拒绝删除（路径越界 / 文件名不再匹配）`);
        failedNames.add(rec.name);
        continue;
      }
      try {
        fs.unlinkSync(target);
      } catch (err) {
        failures.push(`${rec.name}: ${err.message}`);
        failedNames.add(rec.name);
      }
    }
  }

  if (failedNames.size > 0) {
    // 删除失败的文件还在 ⇒ 从 removed 移到 kept（why=delete_failed），不谎报"删了"。
    const failed = plan.removed
      .filter((r) => failedNames.has(r.name))
      .map((r) => ({ ...r, action: 'keep', why: 'delete_failed' }));
    plan = {
      removed: plan.removed.filter((r) => !failedNames.has(r.name)),
      kept: [...plan.kept, ...failed],
      why: { ...plan.why, ...Object.fromEntries(failed.map((r) => [r.name, 'delete_failed'])) },
    };
  }

  const error = failures.length
    ? { code: 'gc_failed', message: `${failures.length} 个文件删除失败：${failures.join('；')}` }
    : null;
  if (parsed.json) {
    const obj = L.gcJson(parsed.root, plan, parsed.dryRun);
    if (error) {
      obj.ok = false;
      obj.error = error;
    }
    say(JSON.stringify(obj, null, 2));
  } else {
    say(L.formatGcText(parsed.root, plan, parsed.dryRun).join('\n'));
    if (error) warn(`emu-ctl gc：${error.message}`);
  }
  return error ? L.EXIT.usage : L.EXIT.ok;
}

async function cmdList(parsed) {
  let probed;
  try {
    probed = await probeAll(parsed.root, L.CONNECT_TIMEOUT_MS);
  } catch (err) {
    const e = err instanceof ClientError ? err : new ClientError('usage', err.message);
    if (parsed.json) {
      say(JSON.stringify({ op: 'list', root: parsed.root, ok: false, error: { code: 'usage', message: e.message } }, null, 2));
    } else {
      warn(`emu-ctl list 失败：${e.message}`);
    }
    return L.exitCodeForError(e.kind);
  }
  if (parsed.json) {
    say(JSON.stringify(L.listJson(parsed.root, probed.rows), null, 2));
  } else {
    say(L.formatListText(parsed.root, probed.rows).join('\n'));
    // 人读的失败细节（为什么坏 / 为什么对不上）走 stderr，不污染 stdout 的结构
    for (const rec of probed.records) {
      if (rec.detail) warn(`  ${rec.name}: ${rec.detail}`);
    }
  }
  return L.EXIT.ok;
}

async function cmdOne(parsed) {
  let link = null;
  let entry = null;
  try {
    const probed = await probeAll(parsed.root, L.CONNECT_TIMEOUT_MS);
    const sel = L.selectEntry(probed.rows, parsed.session);
    if (!sel.ok) {
      const error = instanceError(parsed, sel);
      if (parsed.json) {
        say(
          JSON.stringify(
            { op: parsed.op, ok: false, error, entries: sel.entries, root: parsed.root },
            null,
            2,
          ),
        );
      } else {
        warn(`emu-ctl ${parsed.op} 失败：${error.message}`);
        for (const line of L.formatListText(parsed.root, sel.entries)) warn(line);
        for (const rec of probed.records) if (rec.detail) warn(`  ${rec.name}: ${rec.detail}`);
      }
      return L.EXIT.instance;
    }
    const rec = probed.records.find((r) => r.row === sel.entry);
    entry = rec ? rec.entry : null;
    if (!entry) throw new ClientError('protocol', '内部：选中的实例没有端点文件');

    const timeoutMs = Math.round(parsed.timeoutSec * 1000);
    const opened = await openLink(entry, timeoutMs);
    link = opened.link;
    // ★ `screenshot` 的语义是"**暂停** + 写最后呈现那帧"。服务端 `serve()` 的 Screenshot 分支
    //   只写文件、**不自己 pause**（`sample` 才 pause）⇒ 客户端先发一条**幂等** `pause`
    //   （服务端已暂停态再 pause 回 ok:true 且 state_seq 不动）。这条是客户端补语义，⛔ 不改服务端。
    let reply;
    if (parsed.op === 'screenshot') {
      const paused = await callOp(link, opened.hello, timeoutMs, { op: 'pause' }, 1);
      reply =
        paused.ok === true
          ? await callOp(link, opened.hello, timeoutMs, buildCommand(parsed), 2)
          : paused;
    } else {
      reply = await callOp(link, opened.hello, timeoutMs, buildCommand(parsed));
    }

    let bye = null;
    let byeTimedOut = false;
    if (parsed.op === 'quit' && reply.ok === true) {
      try {
        bye = await waitBye(link, opened.hello, timeoutMs);
      } catch (err) {
        if (!(err instanceof ClientError) || err.kind !== 'timeout') throw err;
        byeTimedOut = true;
      }
    }

    const code = byeTimedOut ? L.EXIT.timeout : L.exitCodeForReply(reply);
    if (parsed.json) {
      const obj = L.resultJson({ op: parsed.op, entry, reply, bye });
      if (byeTimedOut) {
        obj.error = { code: 'timeout', message: 'quit 已受理，但超时前没等到 bye（端点仍活？）' };
      }
      say(JSON.stringify(obj, null, 2));
    } else if (reply.ok !== true) {
      const err = reply.error ?? {};
      warn(`emu-ctl ${parsed.op} 失败：${err.code ?? 'unknown'}${err.message ? ` — ${err.message}` : ''}`);
    } else if (parsed.op === 'sample') {
      say(L.formatSampleLine(reply.data?.sample));
    } else if (parsed.op === 'click') {
      say(L.formatClickLine(reply));
    } else if (parsed.op === 'screenshot') {
      say(L.formatScreenshotLine(reply.data?.screenshot));
    } else if (parsed.op === 'quit') {
      say(L.formatQuitLine(reply, bye));
      if (byeTimedOut) warn('emu-ctl quit：已受理，但超时前没等到 bye（端点仍活？）');
    } else {
      say(L.formatStateLine(parsed.op, reply));
    }
    return code;
  } catch (err) {
    const e = err instanceof ClientError ? err : new ClientError('endpoint', err.message ?? String(err));
    if (parsed.json) {
      say(
        JSON.stringify(
          {
            op: parsed.op,
            session: entry ? entry.session || null : null,
            pid: entry ? entry.pid : null,
            port: entry ? entry.port : null,
            ok: false,
            error: {
              code:
                e.kind === 'timeout'
                  ? 'timeout'
                  : e.kind === 'protocol'
                    ? 'protocol_error'
                    : e.kind === 'usage'
                      ? 'usage'
                      : 'connection',
              message: e.message,
            },
          },
          null,
          2,
        ),
      );
    } else {
      warn(`emu-ctl ${parsed.op} 失败：${e.message}`);
    }
    return L.exitCodeForError(e.kind);
  } finally {
    if (link) await link.close();
  }
}

// =====================================================================================
// 入口
// =====================================================================================

export async function main(argv = process.argv.slice(2)) {
  // ★ 开头自检：Node ≥ 21 才有内置 WebSocket（仓库没有 pin Node ⇒ 这里给人话，⛔ 不引依赖）
  if (typeof WebSocket !== 'function') {
    warn('emu-ctl 需要 Node 内置的全局 WebSocket（Node ≥ 21）；当前版本没有 typeof WebSocket === "function"。');
    return L.EXIT.usage;
  }
  let parsed;
  try {
    parsed = L.parseArgv(argv);
  } catch (err) {
    if (err instanceof L.UsageError) {
      warn(`emu-ctl: ${err.message}`);
      return L.EXIT.usage;
    }
    throw err;
  }
  if (parsed.help) {
    say(HELP);
    return L.EXIT.ok;
  }
  if (parsed.describe) {
    say(parsed.json ? JSON.stringify(L.describe(), null, 2) : L.describeText());
    return L.EXIT.ok;
  }
  if (parsed.op === 'list') return await cmdList(parsed);
  if (parsed.op === 'gc') return await cmdGc(parsed);
  return await cmdOne(parsed);
}

const isDirectRun =
  process.argv[1] !== undefined && path.resolve(process.argv[1]) === path.resolve(fileURLToPath(import.meta.url));
if (isDirectRun) {
  main()
    .then((code) => {
      process.exitCode = code ?? 0;
      // 兜底：某个半死 peer 让 WS 的 close 握手挂住时，别把 CLI 卡死（正常路径上浏览器/端点会先关）
      const guard = setTimeout(() => process.exit(process.exitCode ?? 0), 2000);
      guard.unref();
    })
    .catch((err) => {
      warn(`ERROR: ${err.stack ?? err.message}`);
      process.exitCode = L.EXIT.usage;
    });
}
