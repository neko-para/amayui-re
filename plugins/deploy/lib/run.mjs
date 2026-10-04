/**
 * plugins/deploy/lib/run.mjs —— 在宿主里跑本仓自己的 CLI（`node tools/cli.mjs <域> <动作> …`）
 *
 * ★ 为什么是 `node tools/cli.mjs` 而不是 `pnpm tools …`：少一层 shell 与包管理器，
 *   参数以 **argv 数组**传（不经 shell ⇒ 拼不出注入），超时与中断都可控。
 * ★ 为什么"只转发、不重写业务"：特权面越小越好 —— 部署逻辑仍住在本仓的工具里
 *   （它们自带 dry-run / 写后回读复验 / 回滚），插件只负责"在宿主里跑 + 记账 + 结构化返回"。
 * ★ 这里**允许** `stdio: 'pipe'`：本文件跑在宿主进程里（受限沙箱下才 EPERM，见 `AGENTS.md` §5）。
 */
import { execFile } from 'node:child_process';
import path from 'node:path';

/** 输出裁剪：结果要回给模型，别把几 MB 日志塞进上下文 */
export function tail(text, maxLines = 40, maxChars = 6000) {
  const s = String(text ?? '');
  const lines = s.split(/\r?\n/).filter((l) => l.trim() !== '');
  const cut = lines.slice(-maxLines).join('\n');
  return cut.length > maxChars ? `…（已截断）\n${cut.slice(-maxChars)}` : cut;
}

/**
 * @param {{repoRoot:string, argv:string[], timeoutMs?:number, signal?:AbortSignal}} opts
 * @returns {Promise<{ok:boolean, code:number|null, out:string, err:string, ms:number, cmd:string}>}
 */
export function runRepoCli({ repoRoot, argv, timeoutMs = 15 * 60 * 1000, signal }) {
  const cli = path.join(repoRoot, 'tools', 'cli.mjs');
  const cmd = `node tools/cli.mjs ${argv.join(' ')}`;
  const t0 = Date.now();
  return new Promise((resolve) => {
    execFile(
      process.execPath,
      [cli, ...argv],
      { cwd: repoRoot, timeout: timeoutMs, maxBuffer: 64 * 1024 * 1024, windowsHide: true, signal },
      (err, stdout, stderr) => {
        const code = err && typeof err.code === 'number' ? err.code : err ? null : 0;
        resolve({
          ok: !err,
          code,
          out: String(stdout ?? ''),
          err: String(stderr ?? '') + (err && !stderr ? `${err.message}` : ''),
          ms: Date.now() - t0,
          cmd,
        });
      },
    );
  });
}

/** 直接跑一条外部命令（`relabel-medium` 用；同样是 argv 数组、不经 shell） */
export function runTool({ cwd, cmd, argv, timeoutMs = 10 * 60 * 1000, signal }) {
  const t0 = Date.now();
  return new Promise((resolve) => {
    execFile(cmd, argv, { cwd, timeout: timeoutMs, maxBuffer: 16 * 1024 * 1024, windowsHide: true, signal }, (err, stdout, stderr) => {
      const code = err && typeof err.code === 'number' ? err.code : err ? null : 0;
      resolve({ ok: !err, code, out: String(stdout ?? ''), err: String(stderr ?? '') + (err && !stderr ? `${err.message}` : ''), ms: Date.now() - t0, cmd: `${cmd} ${argv.join(' ')}` });
    });
  });
}
