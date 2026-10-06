/**
 * plugins/pnpm-priv/lib/run.mjs —— 在**宿主里**跑 `pnpm`（argv 数组、不经 shell）
 *
 * ★ 为什么不复用 `tools/cli.mjs`：要跑的是**包管理器**本身（`pnpm install`），它不是本仓的工具。
 *   所以这里是"直接 argv 执行"，与 `plugins/deploy/lib/run.mjs` 的 `runTool` 同一形态、同一理由：
 *   **不接任意命令**，verb 与参数在 `ops.mjs` 里拼死。
 * ★ 这里**允许** `stdio: 'pipe'`：本文件跑在宿主进程里（受限沙箱下才 EPERM，见 `AGENTS.md` §5）。
 * ★ `pnpm` 在 Windows 上是 `.CMD`：用 `execFile` 跑 `pnpm.cmd` 需要 `shell:true` 才能解析；
 *   而 `shell:true` 会把参数交给 cmd ⇒ 所以**不用 shell**，改为显式找出 `pnpm` 的可执行入口
 *   （`process.env.npm_execpath` 或 PATH 上的 `pnpm.cmd`），仍然以 argv 数组传参。
 */
import { execFile } from 'node:child_process';
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
  const pathExt = (process.env.PATHEXT ?? '.COM;.EXE;.BAT;.CMD').split(';');
  for (const dir of (process.env.PATH ?? '').split(path.delimiter)) {
    if (!dir) continue;
    for (const cand of ['pnpm.cmd', 'pnpm.exe', 'pnpm']) {
      const p = path.join(dir, cand);
      if (fs.existsSync(p)) return { kind: 'exe', exe: p, display: p };
    }
    for (const ext of pathExt) {
      const p = path.join(dir, `pnpm${ext}`);
      if (fs.existsSync(p)) return { kind: 'exe', exe: p, display: p };
    }
  }
  return null;
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
  const [cmd, args] = runner.kind === 'node-script' ? [runner.node, [runner.script, ...argv]] : [runner.exe, argv];
  const t0 = Date.now();
  return new Promise((resolve) => {
    execFile(
      cmd,
      args,
      { cwd: repoRoot, timeout: timeoutMs, maxBuffer: 64 * 1024 * 1024, windowsHide: true, signal, env: { ...process.env, npm_config_yes: 'true' } },
      (err, stdout, stderr) => {
        const code = err && typeof err.code === 'number' ? err.code : err ? null : 0;
        resolve({
          ok: !err,
          code,
          out: String(stdout ?? ''),
          err: String(stderr ?? '') + (err && !stderr ? `${err.message}` : ''),
          ms: Date.now() - t0,
          cmd: `${runner.display} ${argv.join(' ')}`,
          runner: runner.display,
        });
      },
    );
  });
}
