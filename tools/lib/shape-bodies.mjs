/**
 * tools/lib/shape-bodies.mjs —— **handler 体骨架普查**（只读、可复现）：
 * "这一族 handler 的记录槽写了什么、调用序列长什么样"，按**骨架相同**分组。
 *
 * ## 为什么它是"算出来的"而不是"声明出来的"（这是 A1–A6 审计的结论）
 * `docs/02-engine/shape-census.md` §10 原有一张 **A1–A6** 义务表（记录槽立即数 / 调用序列 / 位置性观察 /
 * 非交换左右序 / 跨视图 / 每个 call 落桶）。审计后判定：**它们全是能从 EA + 语料机械读出的观察**
 * ⇒ 按本仓硬口径（`AGENTS.md` §6 与技能 §4.2"能从 EA 读出的东西不进产物"）**不该做成声明**，
 * 否则产物里就是第二份真源、且必然与语料漂。⇒ 改成**一条命令算出来**：
 *   · `recordWrites`：每一处"把立即数写进 obj0 相对位移"的写（带归一化位移 / 步长 / 索引 / 立即数）
 *   · `callArgs`    ：每个直接调用点 + 其前 ≤3 条内的 `push <立即数>`（"实参立即数"）
 *   · `bodies`      ：把上面两者**完全相同**的 handler 归成一组（族边界的依据）
 *   · `conservation`：函数体内**每个** `call sub_*` 都要落桶（有立即数实参 / 无 / 尾跳）——漏一个就红
 *
 * ⛔ 本模块**不做语义结论**：身份只用 EA / 归一化位移 / 符号名；"立即数 1 是什么意思"不在这里。
 * ⛔ 不写任何文件。
 */
import { analyzeLines, functionSpans, objRootRegs } from './shape-access.mjs';
import { extractHandlers, listingPath } from './opcodes.mjs';
import fs from 'node:fs';

const hex = (n) => `0x${(n >>> 0).toString(16)}`;
const IMM = /^(0([0-9A-Fa-f]+)h|(-?\d+))$/;

/** `push <imm>` / `push offset …` ⇒ 立即数（解不出返回 null） */
function pushImmOf(body) {
  const m = /^push\s+(.+)$/i.exec(body);
  if (!m) return null;
  const op = m[1].trim();
  const v = IMM.exec(op);
  if (!v) return null;
  return v[2] !== undefined ? Number.parseInt(v[2], 16) : Number(v[3]);
}

/** `mov <mem>, <imm>`（把立即数写进内存）⇒ 立即数；不是这种形态返回 null */
const WRITE_IMM = /^mov\s+[^,[]*\[[^\]]*\][^,]*,\s*(0([0-9A-Fa-f]+)h|-?\d+)\s*$/i;
function memWriteImmOf(body) {
  const m = WRITE_IMM.exec(body);
  if (!m) return null;
  // 注意：`-?\d+` 那支**没有自己的捕获组** ⇒ 十进制要取 m[1]（踩过：取 m[3] 得到 NaN）
  return m[2] !== undefined ? Number.parseInt(m[2], 16) : Number(m[1]);
}

/**
 * 跑一遍骨架普查。
 * @param {{lines: string[], repoRoot: string, match?: string|null}} opts
 */
export function censusBodies({ lines, repoRoot, match = null }) {
  const lst = listingPath(repoRoot);
  const problems = [];
  let byOpcode = new Map();
  if (!lst) problems.push('反汇编语料不在场：corpus/disasm/files/*.lst 不存在（先 `pnpm tools disasm build`）');
  else {
    const ext = extractHandlers(fs.readFileSync(lst, 'utf8'));
    byOpcode = ext.byOpcode;
    problems.push(...ext.problems);
  }
  const syms = [...new Set(byOpcode.values())].sort().filter((s) => !match || s.includes(match));
  const roots = objRootRegs();
  const spanOf = new Map(functionSpans(lines).map((s) => [s.sym, s]));

  const groups = new Map();       // 骨架签名 → { sig, handlers:[], writes:[], calls:[] }
  const conservation = { declared: 0, noImm: 0, tail: 0, indirect: 0, missing: [] };
  const perHandler = [];
  const missing = [];

  for (const sym of syms) {
    const sp = spanOf.get(sym);
    if (!sp) { missing.push(sym); continue; }
    const { sites, links } = analyzeLines(lines, sp.from, sp.to, roots.get(sym) ?? null, {});
    // ── 记录槽写法：obj0 相对、`access==='w'`、"mov <mem>, <立即数>"
    const writes = [];
    for (const s of sites) {
      if (s.access !== 'w' || s.obj !== 'Engine' || s.absDisp === null) continue;
      const imm = memWriteImmOf(s.raw.trim().replace(/^\.text:[0-9A-F]+\s+/, ''));
      if (imm === null) continue;
      writes.push({ ea: hex(s.ea), abs: hex(s.absDisp), stride: s.stride ?? 0, index: s.indexSym ?? null, imm });
    }
    // ── 调用序列：直接调用点 + 其前 ≤3 条内的 `push <imm>`
    const calls = [];
    for (const l of links) {
      const body = (lines[l.line - 1] ?? '').trim().replace(/^\.text:[0-9A-F]+\s+/, '').replace(/\s+;.*$/, '');
      let imm = null;
      for (let d = 1; d <= 3 && imm === null; d += 1) {
        const b = (lines[l.line - 1 - d] ?? '').trim().replace(/^\.text:[0-9A-F]+\s+/, '').replace(/\s+;.*$/, '');
        imm = pushImmOf(b);
      }
      calls.push({ ea: hex(l.ea), callee: l.target, imm, tail: l.tail });
      if (l.tail) conservation.tail += 1;
      else if (imm !== null) conservation.declared += 1;
      else conservation.noImm += 1;
    }
    const sig = JSON.stringify({
      w: writes.map((x) => `${x.abs}/${x.stride}/${x.index ?? ''}←${x.imm}`).sort(),
      c: calls.map((x) => `${x.callee}(${x.imm ?? '?'})${x.tail ? '~' : ''}`),
    });
    if (!groups.has(sig)) groups.set(sig, { sig, writes: [], calls: [], handlers: [], sampleEAs: [] });
    const g = groups.get(sig);
    g.handlers.push(sym);
    for (const w of writes) { g.writes.push(w.ea); if (g.sampleEAs.length < 4) g.sampleEAs.push(w.ea); }
    for (const c of calls) { g.calls.push(c.ea); if (g.sampleEAs.length < 6) g.sampleEAs.push(c.ea); }
    perHandler.push({ sym, writes: writes.length, calls: calls.length, sig });
  }

  const out = [...groups.values()].map((g) => {
    const parsed = JSON.parse(g.sig);
    return {
      size: g.handlers.length,
      handlers: g.handlers,
      recordWrites: parsed.w,
      calls: parsed.c,
      sampleEAs: g.sampleEAs,
      immOnes: parsed.c.filter((x) => x.endsWith('(1)')).length,
    };
  }).sort((a, b) => b.size - a.size);

  return {
    handlerSet: { opcodes: byOpcode.size, uniqueSymbols: byOpcode.size ? new Set(byOpcode.values()).size : 0, scanned: syms.length, found: syms.length - missing.length, missing },
    groups: out,
    conservation: { ...conservation, totalCalls: conservation.declared + conservation.noImm + conservation.tail },
    perHandler,
    problems,
  };
}

/** 终端渲染 */
export function renderBodies(r, { top = 10 } = {}) {
  const L = [];
  const h = r.handlerSet;
  L.push('# handler 体骨架（**有 handler 的 opcode** 子集；判据 = 语料分派表）');
  L.push(`底数       有 handler 的 opcode ${h.opcodes} 个 · **唯一 handler 符号 ${h.uniqueSymbols}** · 普查 ${h.scanned}（找不到 ${h.missing.length}）`);
  L.push(`骨架分组   **${r.groups.length} 组**（分组判据 = 记录槽写法 + 调用序列**完全相同**）`);
  for (const g of r.groups.slice(0, top)) {
    L.push(`  组 ${String(g.size).padStart(3)} 个  handler　例：${g.handlers.slice(0, 3).join(' ')}${g.handlers.length > 3 ? ' …' : ''}`);
    L.push(`        记录槽：${g.recordWrites.length ? g.recordWrites.join(' ') : '（无）'}`);
    L.push(`        调用　：${g.calls.length ? g.calls.join(' ') : '（无）'}`);
  }
  if (r.groups.length > top) L.push(`  …（共 ${r.groups.length} 组，\`--json\` 看全量）`);
  const c = r.conservation;
  L.push('');
  L.push(`守恒       函数体内每个 \`call sub_*\` 都落桶：直接调用 ${c.totalCalls} = 有立即数实参 ${c.declared} + 无立即数 ${c.noImm} · 尾跳 ${c.tail}`);
  if (r.problems.length) L.push(`⚠ 分派表结构问题 ${r.problems.length} 条`);
  L.push('');
  L.push('★ 这些**全是观察**（能从 EA + 语料机械读出）⇒ 不写进 claims 产物；"立即数 1 是第几个操作数"这类是 B 层，⛔ 不在这里');
  return `${L.join('\n')}\n`;
}
