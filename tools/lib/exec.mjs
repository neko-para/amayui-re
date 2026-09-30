/**
 * tools/lib/exec.mjs — **纯子进程 / git 工具**（不认识任何领域数据）
 *
 * ★ 唯一的硬约束：**不捕获管道**。`stdio: 'pipe'` 在受限沙箱里要开命名管道 ⇒ `spawn EPERM`；
 *   这里把 stdout 重定向到**文件描述符**（临时文件写在**系统临时区**，
 *   因此被调用的那个仓库里一个字节都不会被写 —— 对"只读的旧仓"尤其要紧）。
 */
import { spawnSync } from 'node:child_process';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';

let SEQ = 0;

/**
 * 跑一条外部命令并取回 stdout。**不抛**：跑不起来时回 `error`，让调用方决定是 warning 还是失败。
 * @returns {{code:number, out:string, error?:string}}
 */
export function runCapture(cwd, cmd, args) {
  const tmp = path.join(os.tmpdir(), `cap-${process.pid}-${(SEQ += 1)}.out`);
  let fd = null;
  try {
    fd = fs.openSync(tmp, 'w');
    const r = spawnSync(cmd, args, { cwd, stdio: ['ignore', fd, 'inherit'] });
    fs.closeSync(fd);
    fd = null;
    return {
      code: r.status ?? 1,
      out: fs.readFileSync(tmp, 'utf8'),
      error: r.error ? `${r.error.code ?? ''} ${r.error.message}`.trim() : undefined,
    };
  } catch (err) {
    return { code: 127, out: '', error: err.message };
  } finally {
    if (fd !== null) {
      try {
        fs.closeSync(fd);
      } catch {
        /* ignore */
      }
    }
    try {
      fs.unlinkSync(tmp);
    } catch {
      /* ignore */
    }
  }
}

/**
 * 跑一条**只读** git 命令并**要求它成功**（拿不到事实就抛 —— 绝不许"读不到就当空"）。
 * @param {{tolerant?:boolean}} opts tolerant=true 时非零退出返回 ''（用于 `git lfs ls-files` 这类可选查询）
 */
export function git(cwd, args, { tolerant = false } = {}) {
  const r = runCapture(cwd, 'git', args);
  if (r.error) throw new Error(`git ${args.join(' ')} 跑不起来：${r.error}`);
  if (r.code !== 0) {
    if (tolerant) return '';
    throw new Error(`git ${args.join(' ')} 退出 ${r.code} ⇒ 事实拿不到`);
  }
  return r.out.trim();
}

/** `git check-ignore -v`：命中 ⇒ ignored=true。**不抛**（跑不起来时回 `error`） */
export function gitCheckIgnore(repoRoot, relPath) {
  const r = runCapture(repoRoot, 'git', ['check-ignore', '-v', '--', relPath]);
  return { ignored: r.code === 0, detail: r.out.trim(), error: r.error };
}

/** `git check-attr filter -- <path>`：取 filter 属性值（LFS 判定）。**不抛** */
export function gitAttrFilter(repoRoot, relPath) {
  const r = runCapture(repoRoot, 'git', ['check-attr', 'filter', '--', relPath]);
  const m = /filter:\s*(\S+)/.exec(r.out);
  return { value: m ? m[1] : 'unspecified', detail: r.out.trim(), error: r.error };
}
