/**
 * plugins/deploy/lib/audit.mjs —— **append-only 审计**：每次特权调用落一行
 *
 * 为什么要有：特权工具的危险不在"它做了什么"，而在"**事后说不清它做了什么**"。
 * 中介类设计（sudo / polkit / XDG portal）都把"留痕"当硬要求；本仓的台账纪律（`AGENTS.md` §1.4）
 * 也是同一条：**append-only 文本、一条一行、顺序不依赖文件位置**。
 *
 * 落点：`data/privileged-audit.log`（入库文本；两台机器各自追加不同行 ⇒ 三路合并干净）。
 * 行格式的**真源**是 `lib/ops.mjs` 的 `auditLine()`（本文件只负责追加与回读）。
 */
import fs from 'node:fs';
import path from 'node:path';

export const DEFAULT_AUDIT = path.join('data', 'privileged-audit.log');

/** 追加一行；返回写后的字节数与行数（**不抛**：审计写不动不该让特权动作"看起来没发生"） */
export function appendAudit(repoRoot, line, file = DEFAULT_AUDIT) {
  const abs = path.resolve(repoRoot, file);
  try {
    fs.mkdirSync(path.dirname(abs), { recursive: true });
    const before = fs.existsSync(abs) ? fs.readFileSync(abs, 'utf8') : '';
    fs.appendFileSync(abs, `${line}\n`, 'utf8'); // 追加是原子的（O_APPEND）⇒ 不会覆盖别人的行
    const after = fs.readFileSync(abs, 'utf8');
    // ★ 复验只要求"原文还在 + 这一行在里头"：append-only 日志**天然会被并发交错**
    //   （同一轮里并行两次工具调用），拿 `endsWith` 判会把正常情况误判成失败。
    if (!after.startsWith(before) || !after.includes(`${line}\n`)) {
      return { ok: false, file: abs, reason: '回读复验不过（追加后既不是"原文 + 这一行"）' };
    }
    return { ok: true, file: abs, bytes: Buffer.byteLength(after), lines: after.split('\n').filter(Boolean).length };
  } catch (err) {
    return { ok: false, file: abs, reason: `${err.code ?? ''} ${err.message}`.trim() };
  }
}
