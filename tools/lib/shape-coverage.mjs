/**
 * tools/lib/shape-coverage.mjs —— **闭合度记账**：这个函数"分析完成了吗？"
 *
 * ★ 为什么需要它（实测教训）：`claims` 只数**站点**（带可解基址的内存操作数）。
 *   于是"12/12 站点已判"会被读成"函数分析完了"——而 `sub_41C7C0` 的真实语义在
 *   **8 个调用**（其中 `0x41c7ec call eax` 目标未知）与 **3 次抛异常**里。
 *   站点全绿 + 一堆逃逸副作用 = **未闭合**，报告必须说得出来。
 *
 * 口径（**全部机械可判**，不猜语义）：
 *   · 每条指令归一类：内存站点 / 栈 / 控制流 / 纯寄存器 / **未解析行**（解析不出来的行 —— 这是"漏分析"的探测器）
 *   · 逃逸通道逐个点名：直接调用（带目标）、**间接调用**（目标未知 ⇒ ⚠）、**间接跳转**、抛异常、
 *     数据段符号、字符串指令（`rep movs*` 这类不经寄存器模型的）、x87/SSE 内存操作数
 *   · 判据分档（**只有第一档可以由本层宣称**）：
 *       L0 形状闭合 = 没有未解析行 · 每个非栈内存操作数都有站点 · 每个站点都已判或**明确进桶**
 *                     · 没有"目标未知"的间接调用/跳转
 *       L1 副作用边界可数 = 直接调用逐个点名（它们改了 obj0 什么，**不在本层**）
 *       L2 语义 = 台账（AGENTS.md §6），本层永不自称
 */
import { analyzeLines } from './shape-access.mjs';

const PREFIX = /^([A-Za-z_.][\w.]*):([0-9A-F]{8})\s+([a-z][a-z0-9]*)\s*(.*)$/;
/** IDA 的栈变量声明行（`var_4= dword ptr -4`）—— 不是指令 */
const DECL = /^[A-Za-z_][\w ]*=\s*(dword|byte|word|qword|xmmword|tbyte|oword)\s+ptr\s+-?[0-9A-F]+h$/i;
const STRING_MN = /^(rep\s+)?(movs|stos|lods|scas|cmps)[bwd]?$/i;
/** x87 / SSE 内存操作数（它们**在**寄存器模型之外，是已知覆盖缺口） */
const FP_MN = /^(fld|fst|fstp|fild|fist|fistp|fadd|fsub|fmul|fdiv|fcom|fcomp|fucom|fucomp|movss|movsd|movaps|movapd|movdqa|movdqu|addss|addsd|mulss|mulsd|subss|subsd|divss|divsd|cvtss2sd|cvtsd2ss)$/i;
/** 寄存器名（`call eax` 是**间接**调用，不是"调用名叫 eax"） */
const REG_NAME = new Set(['eax', 'ebx', 'ecx', 'edx', 'esi', 'edi', 'ebp', 'esp', 'ax', 'bx', 'cx', 'dx', 'al', 'ah', 'bl', 'bh', 'cl', 'ch', 'dl', 'dh']);
/**
 * 直接调用目标：`sub_XXXXXX` 或**符号名**（可带多个下划线前缀与 @N 后缀，如 `__CxxThrowException@8`）。
 * ⛔ 裸寄存器 / `[...]` / `dword ptr [...]` 都是**间接**。
 */
function isDirectCallee(tgt) {
  const s = (tgt ?? '').trim();
  if (!s || s.startsWith('[') || /\bptr\b/i.test(s) || /\s/.test(s)) return false;
  if (REG_NAME.has(s.toLowerCase())) return false;
  return /^(sub_[0-9A-F]{6}|_*[A-Za-z][\w@?$.]*)$/.test(s);
}

/** 某函数的闭合度记账（`sites` 传 `analyzeLines` 的结果） */
export function coverageOf(lines, sym, span, rootReg, sites) {
  const rows = [];
  for (let i = span.from; i <= span.to; i += 1) {
    const ln = (lines[i] ?? '').trim();
    const m = PREFIX.exec(ln);
    if (!m) continue;
    const ops = m[4].replace(/\s+;.*$/, '').trim();
    if (!ops && !m[3]) continue;
    const text = `${m[3].toLowerCase()} ${ops}`.trim();   // ★ 完整指令文本（判据要用它，不是只用操作数）
    if (!text || text.startsWith(';') || DECL.test(text)) continue;
    rows.push({ ea: Number.parseInt(m[2], 16), line: i + 1, mn: m[3].toLowerCase(), text, body: text });
  }
  const siteAt = new Map();
  for (const s of sites) {
    if (!siteAt.has(s.ea)) siteAt.set(s.ea, []);
    siteAt.get(s.ea).push(s);
  }

  const kinds = { memorySite: 0, stack: 0, control: 0, pureReg: 0, stringOp: 0, float: 0, unparsed: 0 };
  const escapes = { indirectCall: [], indirectJmp: [], throw: [], directCalls: [], dsSymbols: [], stringOps: [], floatMem: [], unparsed: [] };

  for (const r of rows) {
    const hasMem = /\[[^\]]*\]/.test(r.body);
    const isCtl = /^(call|jmp|j[a-z]+|ret|retn)\b/.test(r.body);
    if (/^(call|jmp)\b/.test(r.body)) {
      const tgt = (r.body.match(/^(?:call|jmp)\s+(.+)$/) ?? [])[1] ?? '';
      const indirect = !isDirectCallee(tgt);
      if (r.body.startsWith('call')) {
        if (indirect) escapes.indirectCall.push({ ea: r.ea, text: r.body });
        else {
          escapes.directCalls.push({ ea: r.ea, target: tgt.trim() });
          if (/CxxThrowException/i.test(tgt)) escapes.throw.push({ ea: r.ea, target: tgt.trim() });
        }
      } else if (indirect) escapes.indirectJmp.push({ ea: r.ea, text: r.body });
    }
    if (STRING_MN.test(r.text)) { kinds.stringOp += 1; escapes.stringOps.push({ ea: r.ea, text: r.body }); continue; }
    if (FP_MN.test(r.mn) && hasMem) { kinds.float += 1; escapes.floatMem.push({ ea: r.ea, text: r.body }); continue; }
    const own = siteAt.get(r.ea);
    if (own && own.length) {
      kinds.memorySite += 1;
      if (own.some((s) => s.baseKind === 'absolute')) escapes.dsSymbols.push({ ea: r.ea, obj: own[0].obj });
      continue;
    }
    if (hasMem) {
      // 没有站点却有内存操作数：**判一眼是不是栈**（栈按口径不算站点，不是缺口）
      const stackOnly = (r.body.match(/\[[^\]]*\]/g) ?? []).every((t) => /ebp|esp/.test(t) || /pExceptionObject|var_|arg_/i.test(t));
      if (stackOnly) { kinds.stack += 1; continue; }
      kinds.unparsed += 1;
      escapes.unparsed.push({ ea: r.ea, text: r.body, why: '有内存操作数但没产生站点（且不是栈）' });
      continue;
    }
    if (isCtl) { kinds.control += 1; continue; }
    kinds.pureReg += 1;
  }

  const siteStates = { explained: 0, obj0Residue: 0, unresolvedBase: 0, noLiteral: 0, partial: 0, symbol: 0 };
  // ★ 已经作为"模型之外"点过名的指令（x87/SSE、字符串）不再重复记"基址未解析"——
  //   否则同一个缺口被记两遍，判据会显得比实际更糟（也会把"修好一处"算成修好两处）
  const outsideModel = new Set([...escapes.floatMem, ...escapes.stringOps].map((x) => x.ea));
  for (const s of sites) {
    if (outsideModel.has(s.ea)) continue;
    if (s.baseKind === 'absolute') { siteStates.symbol += 1; continue; }
    if (s.problems.includes('base')) siteStates.unresolvedBase += 1;
    else if (s.absDisp === null && !s.pointeePath?.length) siteStates.partial += 1;
    else if (s.absDisp === null && !(s.pointeePath?.length)) siteStates.noLiteral += 1;
  }

  const reasons = [];
  if (escapes.unparsed.length) reasons.push(`${escapes.unparsed.length} 行有内存操作数但没站点（非栈）`);
  if (escapes.indirectCall.length) reasons.push(`${escapes.indirectCall.length} 处**间接调用**（目标未知 ⇒ 副作用无界）`);
  if (escapes.indirectJmp.length) reasons.push(`${escapes.indirectJmp.length} 处**间接跳转**（目标未知）`);
  if (siteStates.unresolvedBase) reasons.push(`${siteStates.unresolvedBase} 个站点**基址未解析**`);

  return {
    sym, rootReg, insns: rows.length, kinds, escapes, sites: sites.length, siteStates,
    closed: reasons.length === 0,
    reasons,
    // ★ 明确不自称的东西：调用改了 obj0 什么（L2 归台账）
    caveat: escapes.directCalls.length
      ? `${escapes.directCalls.length} 个直接调用的**效果**不在本层（L1 只点名、不判定）`
      : null,
  };
}

/** 一屏渲染 */
export function renderCoverage(cov, hex = (n) => `0x${n.toString(16)}`) {
  const L = [];
  const k = cov.kinds;
  L.push(`# ${cov.sym}（对象根 ${cov.rootReg ?? '?'}）指令 ${cov.insns} 条 · 站点 ${cov.sites} 个`);
  L.push(`  指令分类：内存站点 ${k.memorySite} · 栈 ${k.stack} · 控制流 ${k.control} · 纯寄存器 ${k.pureReg}`
    + `${k.stringOp ? ` · 字符串 ${k.stringOp}` : ''}${k.float ? ` · x87/SSE ${k.float}` : ''}`
    + `${k.unparsed ? ` · **未归类 ${k.unparsed}**` : ''}`);
  L.push(`  判据 L0（形状闭合）：${cov.closed ? '✅ 闭合' : `❌ **未闭合** —— ${cov.reasons.join('；')}`}`);
  if (cov.escapes.directCalls.length) {
    const t = cov.escapes.directCalls.slice(0, 8).map((c) => `${hex(c.ea)}→${c.target}`).join(' ');
    L.push(`  L1（副作用边界）：直接调用 ${cov.escapes.directCalls.length} 个：${t}${cov.escapes.directCalls.length > 8 ? ' …' : ''}`);
  }
  if (cov.escapes.indirectCall.length) {
    L.push(`  ⚠ 间接调用：${cov.escapes.indirectCall.map((c) => `${hex(c.ea)} \`${c.text}\``).join(' · ')}`);
  }
  if (cov.escapes.throw.length) L.push(`  抛出：${cov.escapes.throw.map((c) => hex(c.ea)).join(' ')}`);
  if (cov.escapes.dsSymbols.length) L.push(`  数据段符号：${cov.escapes.dsSymbols.map((c) => `${hex(c.ea)} ${c.obj}`).join(' ')}`);
  if (cov.escapes.floatMem.length) L.push(`  ⚠ x87/SSE 内存操作数（**模型之外**）：${cov.escapes.floatMem.slice(0, 6).map((c) => hex(c.ea)).join(' ')}`);
  if (cov.escapes.stringOps.length) L.push(`  ⚠ 字符串指令（不经寄存器模型）：${cov.escapes.stringOps.slice(0, 6).map((c) => hex(c.ea)).join(' ')}`);
  if (cov.caveat) L.push(`  ${cov.caveat}`);
  return L.join('\n');
}

/** 便捷：给定语料 + 符号，直接算 */
export function coverageFor(lines, sym, spanOf, roots, analyze) {
  const sp = spanOf.get(sym);
  if (!sp) return null;
  const { sites } = analyze(lines, sp.from, sp.to, roots.get(sym) ?? null, {});
  return coverageOf(lines, sym, sp, roots.get(sym) ?? null, sites);
}
