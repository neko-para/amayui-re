/**
 * plugins/pnpm-priv/lib/run.mjs —— 在**宿主里**跑 `pnpm` 与固定任务（argv 数组、不经 shell）
 *
 * ★ 为什么不复用 `tools/cli.mjs`：要跑的是**包管理器**与 app 自己的脚本（`vite build` / Electron），
 *   它们不是本仓的工具。所以这里是"直接 argv 执行"，与 `plugins/deploy/lib/run.mjs` 的 `runTool`
 *   同一形态、同一理由：**不接任意命令**，任务名 → argv 在 `ops.mjs` 的 `TASKS` 表里拼死。
 * ★ 这里**允许** `stdio: 'pipe'`：本文件跑在宿主进程里（受限沙箱下才 EPERM，见 `AGENTS.md` §5）。
 * ★ `pnpm` 在 Windows 上是 `.CMD`：用 `execFile` 跑 `pnpm.cmd` 需要 `shell:true` 才能解析；
 *   而 `shell:true` 会把参数交给 cmd ⇒ 所以**不用 shell**，改为显式找出 `pnpm` 的可执行入口
 *   （`process.env.npm_execpath` 或 PATH 上的 `pnpm.cmd`），仍然以 argv 数组传参。
 * ★ Electron 同理：不碰 `.bin/electron.cmd`（那是 cmd 脚本），而是从 app 的 `node_modules/electron`
 *   里读它自己的 `path.txt` 拿到 `dist/electron.exe`；拿不到就退到 `node cli.js`。
 */
import { execFile, spawn } from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';

/** 输出裁剪：结果要回给模型，别把几 MB 日志塞进上下文 */
export function tail(text, maxLines = 40, maxChars = 6000) {
  const s = String(text ?? '');
  const lines = s.split(/\r?\n/).filter((l) => l.trim() !== '');
  const cut = lines.slice(-maxLines).join('\n');
  return cut.length > maxChars ? `…（已截断）\n${cut.slice(-maxChars)}` : cut;
}

/** 找出 pnpm 的可执行入口（不靠 shell） */
export function resolvePnpm() {
  // ① 正在跑我们的 pnpm 会把这个写进环境（`pnpm tools …` 时就是它）
  const exe = process.env.npm_execpath;
  if (exe && fs.existsSync(exe)) {
    // npm_execpath 在 pnpm 下通常是 `…/pnpm/bin/pnpm.cjs` ⇒ 用 node 跑它
    if (/\.(c?js|mjs)$/i.test(exe)) return { kind: 'node-script', node: process.execPath, script: exe, display: `node ${exe}` };
    return { kind: 'exe', exe, display: exe };
  }
  // ② PATH 上找 pnpm.cmd / pnpm
  // ★ 找到 `.cmd` 时必须再找**它的 JS 入口**：Node 从 v20 起拒绝 `execFile('x.cmd')`
  //   （同步 `spawn EINVAL`，CVE-2024-27980 的处置），而 `shell:true` 会把参数交给 cmd。
  //   全局安装的布局是 `<node 目录>/node_modules/pnpm/bin/pnpm.cjs`。
  const pathExt = (process.env.PATHEXT ?? '.COM;.EXE;.BAT;.CMD').split(';');
  const candidates = [];
  for (const dir of (process.env.PATH ?? '').split(path.delimiter)) {
    if (!dir) continue;
    for (const cand of ['pnpm.cmd', 'pnpm.exe', 'pnpm']) candidates.push(path.join(dir, cand));
    for (const ext of pathExt) candidates.push(path.join(dir, `pnpm${ext}`));
  }
  for (const p of candidates) {
    if (!fs.existsSync(p)) continue;
    if (/\.(cmd|bat)$/i.test(p)) {
      const js = path.join(path.dirname(p), 'node_modules', 'pnpm', 'bin', 'pnpm.cjs');
      if (fs.existsSync(js)) return { kind: 'node-script', node: process.execPath, script: js, display: `node ${js}` };
      // 退路：这个 shim 本来就是 `.cmd`（是**它**的形态，不是我们给调用方的口子）
      return { kind: 'cmd-shim', comspec: process.env.ComSpec || 'cmd.exe', shim: p, display: p };
    }
    return { kind: 'exe', exe: p, display: p };
  }
  return null;
}

/**
 * 找出 Electron 的可执行入口 —— **只从 app 自己的 `node_modules` 里找**（不靠 PATH：
 * 系统里另装的那个 Electron 不是我们在测的那个）。
 * `path.txt` 是 Electron 自己写的（`electron.exe` / `Electron.app/Contents/MacOS/Electron` …），
 * 官方 `index.js` 的解析就是 `<pkg>/dist/<path.txt>`。
 */
export function resolveElectron(repoRoot, app) {
  if (!app) return null;
  const pkgDir = path.join(repoRoot, app.replace(/[\\/]+$/, ''), 'node_modules', 'electron');
  try {
    const name = fs.readFileSync(path.join(pkgDir, 'path.txt'), 'utf8').trim();
    if (name) {
      const exe = path.join(pkgDir, 'dist', name);
      if (fs.existsSync(exe)) return { kind: 'exe', exe, display: exe };
    }
  } catch { /* 没有 path.txt ⇒ 退到 cli.js */ }
  const cli = path.join(pkgDir, 'cli.js');
  if (fs.existsSync(cli)) return { kind: 'node-script', node: process.execPath, script: cli, display: `node ${cli}` };
  return null;
}

/** 解析结果 → [可执行, 参数]（三种入口形态在这里收口） */
function toCmdArgs(r, argv) {
  if (r.kind === 'node-script') return [r.node, [r.script, ...argv]];
  if (r.kind === 'cmd-shim') return [r.comspec, ['/d', '/s', '/c', r.shim, ...argv]];
  return [r.exe, argv];
}

/** 按 runner 解析出 [可执行, 参数]（解析不到 ⇒ null） */
function resolveRunner({ runner, repoRoot, app, argv }) {
  const r = runner === 'pnpm' ? resolvePnpm() : runner === 'electron' ? resolveElectron(repoRoot, app) : null;
  if (!r) return null;
  const [cmd, args] = toCmdArgs(r, argv);
  return { ...r, cmd, args };
}

/**
 * `execFile` 的安全包装：**spawn 的失败可能是同步抛出的**（受限沙箱下 `EPERM`、
 * Windows 上 `.cmd` 的 `EINVAL`）⇒ 必须变成结构化失败，否则 `execute` 会以"未处理异常"结束，
 * 模型拿不到"为什么"，也就没法判断是不是特权没成立。
 */
function execFileSafe({ cmd, args, opts, t0, display }) {
  return new Promise((resolve) => {
    const fail = (err) =>
      resolve({
        ok: false,
        code: typeof err.code === 'number' ? err.code : null,
        out: '',
        err: `${err.code ?? ''} ${err.message}`.trim(),
        ms: Date.now() - t0,
        cmd: display,
        runner: display,
      });
    try {
      execFile(cmd, args, opts, (err, stdout, stderr) => {
        const code = err && typeof err.code === 'number' ? err.code : err ? null : 0;
        resolve({
          ok: !err,
          code,
          out: String(stdout ?? ''),
          err: String(stderr ?? '') + (err && !stderr ? `${err.message}` : ''),
          ms: Date.now() - t0,
          cmd: display,
          runner: display,
        });
      });
    } catch (err) {
      fail(err);
    }
  });
}

/**
 * 跑一次 pnpm。
 * @param {{repoRoot:string, argv:string[], timeoutMs?:number, signal?:AbortSignal}} opts
 * @returns {Promise<{ok:boolean, code:number|null, out:string, err:string, ms:number, cmd:string, runner:string|null}>}
 */
export function runPnpm({ repoRoot, argv, timeoutMs = 30 * 60 * 1000, signal }) {
  const runner = resolvePnpm();
  if (!runner) {
    return Promise.resolve({ ok: false, code: null, out: '', err: '找不到 pnpm 可执行文件（PATH / npm_execpath 都没有）', ms: 0, cmd: `pnpm ${argv.join(' ')}`, runner: null });
  }
  const [cmd, args] = toCmdArgs(runner, argv);
  return execFileSafe({
    cmd,
    args,
    opts: { cwd: repoRoot, timeout: timeoutMs, maxBuffer: 64 * 1024 * 1024, windowsHide: true, signal, env: { ...process.env, npm_config_yes: 'true' } },
    t0: Date.now(),
    display: `${runner.display} ${argv.join(' ')}`,
  });
}

/**
 * 跑一个**固定任务**（`ops.mjs` 的 `TASKS` 表）：cwd / argv / timeout 全部来自 plan。
 * @param {{runner:string, repoRoot:string, app?:string|null, argv:string[], cwd:string, timeoutMs?:number, signal?:AbortSignal}} opts
 * @returns {Promise<{ok:boolean, code:number|null, out:string, err:string, ms:number, cmd:string, runner:string|null}>}
 */
export function runTask({ runner, repoRoot, app = null, argv, cwd, timeoutMs = 30 * 60 * 1000, signal }) {
  const r = resolveRunner({ runner, repoRoot, app, argv });
  if (!r) {
    return Promise.resolve({
      ok: false,
      code: null,
      out: '',
      err: `找不到 ${runner} 的入口（${runner === 'electron' ? `看 ${app}/node_modules/electron` : 'PATH / npm_execpath 都没有 pnpm'}）—— 先跑 install？`,
      ms: 0,
      cmd: `${runner} ${argv.join(' ')}`,
      runner: null,
    });
  }
  const t0 = Date.now();
  const env = { ...process.env, ...(runner === 'pnpm' ? { npm_config_yes: 'true' } : {}) };
  return execFileSafe({
    cmd: r.cmd,
    args: r.args,
    opts: { cwd, timeout: timeoutMs, maxBuffer: 64 * 1024 * 1024, windowsHide: true, signal, env },
    t0,
    display: `${r.display} ${argv.join(' ')}`,
  });
}

/**
 * **起进程就返回**（交互式 Electron 用）：`detached` + `unref`，父进程死了它也活着。
 *
 * ★ 为什么不用 `execFile`：交互式窗口会一直在，等它退出等于把这个回合钉住。
 * ★ 为什么还要 `graceMs`：`spawn` 的失败（ENOENT / EACCES / 缺 DLL）是**异步**的，
 *   不等一下就会把"起不来"报成"已启动"。等 1.5 s 再看 `exitCode` 是不是真的还活着。
 * @returns {Promise<{ok:boolean, pid:number|null, code:number|null, err:string|null, ms:number, cmd:string, logFile:string}>}
 */
export function launchDetached({ runner, repoRoot, app = null, argv, cwd, logFile, graceMs = 1500 }) {
  const r = resolveRunner({ runner, repoRoot, app, argv });
  const cmdText = `${runner} ${argv.join(' ')}`;
  if (!r) {
    return Promise.resolve({ ok: false, pid: null, code: null, err: `找不到 ${runner} 的入口`, ms: 0, cmd: cmdText, logFile });
  }
  const t0 = Date.now();
  try {
    fs.mkdirSync(path.dirname(logFile), { recursive: true });
    const fd = fs.openSync(logFile, 'a');
    const child = spawn(r.cmd, r.args, {
      cwd,
      detached: true,
      windowsHide: true,
      stdio: ['ignore', fd, fd],
      env: { ...process.env },
    });
    fs.closeSync(fd); // 父进程这份可以关了：子进程已经 dup 走
    let spawnErr = null;
    child.on('error', (e) => { spawnErr = e; });
    child.unref();
    return new Promise((resolve) => {
      setTimeout(() => {
        resolve({
          ok: !spawnErr && child.exitCode === null,
          pid: child.pid ?? null,
          code: child.exitCode ?? null,
          err: spawnErr ? `${spawnErr.code ?? ''} ${spawnErr.message}`.trim() : null,
          ms: Date.now() - t0,
          cmd: `${r.display} ${argv.join(' ')}`,
          logFile,
        });
      }, graceMs);
    });
  } catch (err) {
    return Promise.resolve({ ok: false, pid: null, code: null, err: `${err.code ?? ''} ${err.message}`.trim(), ms: Date.now() - t0, cmd: cmdText, logFile });
  }
}
