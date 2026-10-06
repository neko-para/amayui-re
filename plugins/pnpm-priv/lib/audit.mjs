/**
 * plugins/pnpm-priv/lib/audit.mjs —— 每次调用**留一行**（append-only 文本）
 *
 * 为什么与 `data/ledger/` 分开：审计记的是**特权动作**（"谁在什么时候、以什么参数、跑了 pnpm"），
 * 它不是引擎知识、也不该混进知识台账。落点固定在工作区内的 `data/` 下（`.gitignore` 会忽略 `data/` 的运行时产物）。
 * ★ append-only：只 `appendFileSync`，从不重写；写不动就如实返回原因（调用方把它带上，不抛）。
 */
import fs from 'node:fs';
import path from 'node:path';

/** 默认审计文件（相对仓库根） */
export const DEFAULT_AUDIT = 'data/privileged-audit.log';

/**
 * @param {string} repoRoot
 * @param {string} line 已经压平的一行
 * @param {string} rel 相对 repoRoot 的路径
 * @returns {{ok:boolean, file:string, lines?:number, reason?:string}}
 */
export function appendAudit(repoRoot, line, rel = DEFAULT_AUDIT) {
  const file = path.resolve(repoRoot, rel);
  try {
    fs.mkdirSync(path.dirname(file), { recursive: true });
    fs.appendFileSync(file, `${line}\n`, 'utf8');
    const n = fs.readFileSync(file, 'utf8').split('\n').filter((l) => l !== '').length;
    return { ok: true, file, lines: n };
  } catch (err) {
    return { ok: false, file, reason: `${err.code ?? ''} ${err.message}`.trim() };
  }
}
