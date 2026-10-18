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
 * `lea r,[r+0]` = MSVC 的 6 字节对齐**填充**，**不是**内存访问。
 * ★ 判定层（`shape-access.mjs` 的 `siteOf`）早已按同一口径剔除它；本层若仍把它算成"有内存操作数却没站点"，
 *   就会产出**假红**：实测 `sub_402330` 的唯一"未归类行"就是 `0x40235d lea ecx,[ecx+0]`
 *   ⇒ 该函数永远闭不上，而真正的原因只有两个间接调用。全表 147 个带"未归类行"的函数里有一部分就是它。
 */
const PAD_LEA = /^lea\s+([a-z][a-z0-9]*)\s*,\s*\[\s*\1\s*\+\s*0h?\s*\]$/i;
/**
 * 直接调用目标：`sub_XXXXXX` 或**符号名**（可带多个下划线前缀与 @N 后缀，如 `__CxxThrowException@8`）。
 * ⛔ 裸寄存器 / `[...]` / `dword ptr [...]` 都是**间接**。
 */
function isDirectCallee(tgt) {
  // ★ 两次实测踩到的假阳：① `jmp short loc_XXXXXX`（函数内标签 = 直接跳转）
  //   ② MSVC 装饰名 `?foo@@YAXXZ`（首字符不是字母）。⇒ 先剥限定词，再分三类放行。
  let s = (tgt ?? '').trim();
  if (!s || s.startsWith('[') || /\bptr\b/i.test(s)) return false;
  s = s.replace(/^(short|near|far)\s+/i, '').trim();
  if (!s || /\s/.test(s)) return false;
  if (REG_NAME.has(s.toLowerCase())) return false;
  if (/^(loc|def)_[0-9A-F]{6}$/i.test(s)) return true;              // 函数内标签
  if (/^sub_[0-9A-F]{6}$/i.test(s)) return true;                    // 已识别的函数
  if (/^\?*[_@$A-Za-z?][\w@?$.]*$/.test(s)) return true;            // 符号名（含 `??_U@…` 这类装饰名）
  return false;
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
    if (PAD_LEA.test(r.body)) { kinds.pureReg += 1; continue; }   // 对齐填充：不碰内存，⛔ 不许记成"未归类行"
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

  // ★ 未闭合原因**结构化**（`kind` 是闭集合，供归档/分组用；`text` 给人看）。
  //   早先只有给人看的字符串，归档要靠 `replace(/^\d+ /,'')` 反向解析 —— 那样的"分组键"是**猜**出来的。
  const reasonKinds = [];
  if (escapes.unparsed.length) reasonKinds.push({ kind: 'unparsedLine', count: escapes.unparsed.length, text: `${escapes.unparsed.length} 行有内存操作数但没站点（非栈）` });
  if (escapes.indirectCall.length) reasonKinds.push({ kind: 'indirectCall', count: escapes.indirectCall.length, text: `${escapes.indirectCall.length} 处**间接调用**（目标未知 ⇒ 副作用无界）` });
  if (escapes.indirectJmp.length) reasonKinds.push({ kind: 'indirectJmp', count: escapes.indirectJmp.length, text: `${escapes.indirectJmp.length} 处**间接跳转**（目标未知）` });
  if (siteStates.unresolvedBase) reasonKinds.push({ kind: 'unresolvedBase', count: siteStates.unresolvedBase, text: `${siteStates.unresolvedBase} 个站点**基址未解析**` });
  const reasons = reasonKinds.map((r) => r.text);

  return {
    sym, rootReg, insns: rows.length, kinds, escapes, sites: sites.length, siteStates,
    closed: reasons.length === 0,
    reasons,
    reasonKinds,
    // ★ 明确不自称的东西：调用改了 obj0 什么（L2 归台账）
    caveat: escapes.directCalls.length
      ? `${escapes.directCalls.length} 个直接调用的**效果**不在本层（L1 只点名、不判定）`
      : null,
  };
}

/** `L0` 的未闭合原因闭集合（归档/分组的**唯一**键来源） */
export const REASON_KINDS = ['unparsedLine', 'indirectCall', 'indirectJmp', 'unresolvedBase'];

/**
 * **未闭合归档**：把一批 `coverageOf()` 的结果按原因分组 ⇒ "下一层能用的函数清单"。
 *
 * 口径：
 * * `closed` 的函数只进 `functions`，不进任何分组（⛔ 不制造"零原因"的分组）；
 * * 一个函数可能**同时**有多个原因 ⇒ 它出现在多个分组里；`counts.byReason` **按原因计**，
 *   与 `unclosed`（按函数计）**不是同一个口径**，两个数都会报出来（不许相加）。
 */
export function coverageArchive(covs) {
  const groups = Object.fromEntries(REASON_KINDS.map((k) => [k, []]));
  const functions = covs.map((c) => ({ sym: c.sym, closed: !!c.closed, reasons: c.reasons ?? [], reasonKinds: (c.reasonKinds ?? []).map((r) => r.kind), sites: c.sites ?? 0 }));
  let closed = 0;
  let unclosed = 0;
  for (const c of covs) {
    if (c.closed) { closed += 1; continue; }
    unclosed += 1;
    for (const r of c.reasonKinds ?? []) {
      if (!groups[r.kind]) throw new Error(`未知的未闭合原因 kind：${r.kind}（闭集合见 REASON_KINDS）`);
      groups[r.kind].push(c.sym);
    }
  }
  return {
    total: covs.length, closed, unclosed,
    counts: { byReason: Object.fromEntries(Object.entries(groups).map(([k, v]) => [k, v.length])) },
    byReason: groups,
    functions,
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
  if (cov.escapes.unparsed.length) {
    // ★ 未归类行**必须逐条点名 EA**：否则"147 个函数有未归类行"只能当口号，没法回语料查它是不是解析器缺口。
    L.push(`  ⚠ 未归类行（有内存操作数但没站点、且不是栈）：${cov.escapes.unparsed.slice(0, 8).map((c) => `${hex(c.ea)} \`${c.text}\``).join(' · ')}${cov.escapes.unparsed.length > 8 ? ' …' : ''}`);
  }
  if (cov.escapes.throw.length) L.push(`  抛出：${cov.escapes.throw.map((c) => hex(c.ea)).join(' ')}`);
  if (cov.escapes.dsSymbols.length) L.push(`  数据段符号：${cov.escapes.dsSymbols.map((c) => `${hex(c.ea)} ${c.obj}`).join(' ')}`);
  if (cov.escapes.floatMem.length) L.push(`  ⚠ x87/SSE 内存操作数（**模型之外**）：${cov.escapes.floatMem.slice(0, 6).map((c) => hex(c.ea)).join(' ')}`);
  if (cov.escapes.stringOps.length) L.push(`  ⚠ 字符串指令（不经寄存器模型）：${cov.escapes.stringOps.slice(0, 6).map((c) => hex(c.ea)).join(' ')}`);
  if (cov.tierCompare) {
    const { conservative: c1, calibrated: c2 } = cov.tierCompare;
    L.push(`  未解析基址：保守档 ${c1} · 校准档 ${c2}`
      + (c2 < c1 ? `　⇒ 其中 ${c1 - c2} 处只是\"call 杀了 caller-saved\"，不是模型缺口（--calibrated 看另一档）` : ''));
  }
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
