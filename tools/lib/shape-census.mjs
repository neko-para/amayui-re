/**
 * tools/lib/shape-census.mjs —— **全表普查**：把"每个有 handler 的 opcode 以什么形状碰了 `obj0` 的哪个偏移"
 * 从"跑完就丢的一次性输出"变成**一次可复现的只读产出**。
 *
 * ## 它回答什么（以及不回答什么）
 * * 回答：底数（多少 opcode / 多少唯一 handler 符号）· 对象根寄存器分组 · 站点按**基址类别**分类 ·
 *   `obj0` 偏移聚类（每个偏移被哪些 handler 碰、样例 EA）· 带索引的簇（`abs` / `stride` / 索引来源）·
 *   未解析三类（base / index / width）的报数与样例 EA。
 * * ⛔ **不判定**：判定在 `lib/shape-claims.mjs`（模型 + 成员表）。本模块只报**观察**。
 * * ⛔ **不写任何文件**：产物落哪由调用方决定（本仓派生区是 `.tmp/`，不入库）。
 *
 * ## 三条口径（写清楚，否则数字会互相打架）
 * 1. **handler 集合来自语料的分配表**（`lib/opcodes.mjs` 的 `extractHandlers`）—— 那是"A↔B 的桥"，
 *    ⛔ 不属于汇编层、也不属于引擎语义层，所以它出现在**命令**里，不出现在 claims 产物里。
 * 2. **一个 handler 可能被多个 opcode 共用**（实测：`sub_419690` 被两个 opcode 指向）⇒ 本模块按**符号**去重，
 *    并单独报"被几个 opcode 指向"。把 opcode 数当 handler 数会**虚高**。
 * 3. **`obj0` 偏移只收"位移能用"的访问**（判据是 `shape-claims.mjs` 的 `offsetUnusable()`，**共用同一份定义**）：
 *    `lea` 取址中间量（`abs` 只是位移项）· **索引未解析**（位移少了索引那一项 ⇒ 报出来的 `absDisp` 是假的）。
 *    ★ 实测订正：handler 子集里 `index` 出问题的 310 处中，**175 处的基址是解出来的**（不是 128 那个子集）——
 *      把它们当偏移观察会编出**假观察**（轻则多一条残差，重则"正好命中某个成员"⇒ 假解释）。
 *      38 处 `abs=0x0` 的 `lea` 指针算术也属于同一类误报源（实测踩过）。
 */
import { extractHandlers, listingPath } from './opcodes.mjs';
import { analyzeLines, functionSpans, objRootRegs } from './shape-access.mjs';
import { offsetUnusable } from './shape-claims.mjs';
import fs from 'node:fs';
import path from 'node:path';

const hex = (n) => `0x${(n >>> 0).toString(16)}`;
const pad = (s, n) => String(s).padEnd(n);

/**
 * 跑一遍 handler 子集的普查（**一次载入语料**；⛔ 不要每个 handler 重载一遍 `.lst`）。
 * @param {{ lines: string[], repoRoot: string, match?: string|null }} opts
 * @returns {object} 机器可读的普查结果（调用方决定怎么打印 / 落盘）
 */
export function censusHandlers({ lines, repoRoot, match = null }) {
  const lst = listingPath(repoRoot);
  const problems = [];
  let handlerRows = 0;
  let byOpcode = new Map();
  if (!lst) problems.push('反汇编语料不在场：corpus/disasm/files/*.lst 不存在（先 `pnpm tools disasm build`）');
  else {
    const ext = extractHandlers(fs.readFileSync(lst, 'utf8'));
    byOpcode = ext.byOpcode;
    handlerRows = ext.rows;
    problems.push(...ext.problems);
  }
  /** 符号 → 指向它的 opcode 列表（按符号去重：多 opcode 共用同一 handler） */
  const opcodesOf = new Map();
  for (const [op, sym] of byOpcode) {
    if (!opcodesOf.has(sym)) opcodesOf.set(sym, []);
    opcodesOf.get(sym).push(op);
  }
  const allSyms = [...opcodesOf.keys()].sort();
  const syms = match ? allSyms.filter((s) => s.includes(match)) : allSyms;

  const roots = objRootRegs();
  const spans = functionSpans(lines);
  const spanOf = new Map(spans.map((s) => [s.sym, s]));

  const byBaseKind = { this: 0, absolute: 0, unresolved: 0, memberptr: 0, embed: 0 };
  const byAccess = { r: 0, w: 0, rw: 0, a: 0 };
  const byWidthHow = { explicit: 0, register: 0, unresolved: 0 };
  const rootRegs = new Map();
  const offsets = new Map();     // abs → { sites, handlers:Set, eas:[] }
  const indexed = new Map();     // `${abs}|${stride}|${index}` → { abs, stride, index, sites, handlers:Set }
  const unresolved = { base: { n: 0, byReg: new Map(), eas: [] }, index: { n: 0, eas: [] }, width: { n: 0, eas: [] } };
  const absoluteSyms = new Map(); // 数据段符号名 → 站点数
  const missing = [];
  const perHandler = [];
  let sitesTotal = 0;
  let partialLea = 0;
  let partialTakeAddr = 0;
  let partialIndexUnresolved = 0;
  let obj0Total = 0;

  for (const sym of syms) {
    const sp = spanOf.get(sym);
    if (!sp) { missing.push(sym); continue; }
    const root = roots.get(sym) ?? null;
    rootRegs.set(root ?? '(无)', (rootRegs.get(root ?? '(无)') ?? 0) + 1);
    const { sites } = analyzeLines(lines, sp.from, sp.to, root, {});
    const own = { obj0: 0, symbol: 0, unresolved: 0, memberptr: 0, embed: 0, partial: 0, partialIndexUnresolved: 0, deep: 0 };
    for (const s of sites) {
      sitesTotal += 1;
      const kind = s.baseKind ?? 'unresolved';
      if (kind in byBaseKind) byBaseKind[kind] += 1;
      if (s.access in byAccess) byAccess[s.access] += 1;
      const how = s.width === 'unknown' ? 'unresolved' : s.widthHow ?? 'register';
      if (how in byWidthHow) byWidthHow[how] += 1;
      if (kind === 'absolute') { own.symbol += 1; absoluteSyms.set(s.obj, (absoluteSyms.get(s.obj) ?? 0) + 1); }
      if (s.problems.includes('base')) {
        own.unresolved += 1;
        unresolved.base.n += 1;
        const r = s.baseReg ?? '(无基址寄存器)';
        unresolved.base.byReg.set(r, (unresolved.base.byReg.get(r) ?? 0) + 1);
        if (unresolved.base.eas.length < 64) unresolved.base.eas.push(s.ea);
      }
      if (kind === 'memberptr') own.memberptr += 1;
      if (kind === 'embed' && typeof s.obj === 'string' && s.obj !== 'Engine') own.embed += 1;
      if (s.problems.includes('index')) { unresolved.index.n += 1; if (unresolved.index.eas.length < 64) unresolved.index.eas.push(s.ea); }
      if (s.problems.includes('width')) { unresolved.width.n += 1; if (unresolved.width.eas.length < 64) unresolved.width.eas.push(s.ea); }
      if (s.obj === 'Engine' && s.absDisp !== null) {
        // ★ 位移可不可用是**一个**判据（`offsetUnusable`，与 claims 共用同一份定义 ⇒ 两边不会差数）：
        //   `lea` 取址中间量 · 索引未解析（位移少了索引那一项）。两类都不算偏移观察。
        const why = offsetUnusable(s);
        if (why) {
          own.partial += 1;
          partialLea += 1;
          if (why === 'index-unresolved') { own.partialIndexUnresolved += 1; partialIndexUnresolved += 1; }
          else { partialTakeAddr += 1; }
          continue;
        }
        own.obj0 += 1;
        obj0Total += 1;
        const key = s.absDisp;
        if (!offsets.has(key)) offsets.set(key, { abs: key, sites: 0, handlers: new Set(), eas: [] });
        const o = offsets.get(key);
        o.sites += 1;
        o.handlers.add(sym);
        if (o.eas.length < 16) o.eas.push(s.ea);
        if (s.stride) {
          const ikey = `${key}|${s.stride}|${s.indexSym ?? ''}`;
          if (!indexed.has(ikey)) indexed.set(ikey, { abs: key, stride: s.stride, index: s.indexSym ?? null, sites: 0, handlers: new Set() });
          const c = indexed.get(ikey);
          c.sites += 1;
          c.handlers.add(sym);
        }
      }
    }
    perHandler.push({ sym, opcodes: opcodesOf.get(sym), root, sites: sites.length, ...own });
  }

  const toRows = (m, pick) => [...m.values()].map(pick).sort((a, b) => b.sites - a.sites);
  return {
    handlerSet: {
      assignments: handlerRows,
      opcodes: byOpcode.size,
      uniqueSymbols: allSyms.length,
      scanned: syms.length,
      found: syms.length - missing.length,
      missing,
      sharedBySeveralOpcodes: [...opcodesOf].filter(([, v]) => v.length > 1).map(([sym, ops]) => ({ sym, opcodes: ops })),
    },
    roots: { byReg: Object.fromEntries([...rootRegs].sort((a, b) => b[1] - a[1])), withoutRoot: perHandler.filter((h) => !h.root).map((h) => h.sym) },
    sites: {
      total: sitesTotal, partialLea, partialTakeAddr, partialIndexUnresolved, obj0: obj0Total,
      byBaseKind, byAccess, byWidthHow,
      absoluteSyms: [...absoluteSyms].sort((a, b) => b[1] - a[1]).map(([sym, n]) => ({ sym, sites: n })),
      unresolved: {
        base: { n: unresolved.base.n, byReg: Object.fromEntries([...unresolved.base.byReg].sort((a, b) => b[1] - a[1])), sampleEAs: unresolved.base.eas },
        index: { n: unresolved.index.n, sampleEAs: unresolved.index.eas },
        width: { n: unresolved.width.n, sampleEAs: unresolved.width.eas },
      },
    },
    obj0Offsets: toRows(offsets, (o) => ({ abs: o.abs, sites: o.sites, handlers: o.handlers.size, sampleHandlers: [...o.handlers].slice(0, 5), sampleEAs: o.eas })),
    indexedClusters: toRows(indexed, (c) => ({ abs: c.abs, stride: c.stride, index: c.index, sites: c.sites, handlers: c.handlers.size })),
    perHandler,
    problems,
  };
}

/** 终端渲染（数字都能追到 EA / 符号清单；⛔ 不许"大约/大概"） */
export function renderCensus(r, { topOffsets = 12, topIndexed = 8 } = {}) {
  const L = [];
  const h = r.handlerSet;
  /** `{k: n}` → `k n · …`（只留非零项；⛔ 不把 0 混进"有多少"的叙述里） */
  const kvs = (obj) => Object.entries(obj).filter(([, v]) => v).map(([k, v]) => `${k} ${v}`).join(' · ');
  L.push('# 全表普查（**有 handler 的 opcode** 子集；判据 = 语料分派表）');
  L.push(`底数       赋值行 ${h.assignments} 条 · 有 handler 的 opcode ${h.opcodes} 个 · **唯一 handler 符号 ${h.uniqueSymbols}**`);
  L.push(`           普查 ${h.scanned} 个：语料里找到 ${h.found} · 找不到 ${h.missing.length}${h.missing.length ? `（${h.missing.slice(0, 5).join(' ')}…）` : ''}`);
  if (h.sharedBySeveralOpcodes.length) L.push(`           ★ 被多个 opcode 共用的 handler：${h.sharedBySeveralOpcodes.map((x) => `${x.sym}(${x.opcodes.map((o) => `0x${o.toString(16)}`).join('/')})`).join(' · ')}`);
  L.push(`对象根     ${Object.entries(r.roots.byReg).map(([k, v]) => `${k} ${v}`).join(' · ')}`);
  if (r.roots.withoutRoot.length) L.push(`           ⛔ 无对象根（⇒ **不可检查**）：${r.roots.withoutRoot.join(' ')}`);
  const st = r.sites;
  L.push(`站点       ${st.total} 处（` + Object.entries(st.byBaseKind).filter(([, v]) => v).map(([k, v]) => `${k} ${v}`).join(' · ') + '）');
  L.push(`           访问方向：${kvs(st.byAccess)}　（位移不可用 ${st.partialLea}：lea 取址 ${st.partialTakeAddr} · 索引未解析 ${st.partialIndexUnresolved}）`);
  L.push(`           宽度来源：${kvs(st.byWidthHow)}　⛔ unknown 不许当 4`);
  L.push(`obj0 偏移  ${r.obj0Offsets.length} 个（按站点数降序，前 ${Math.min(topOffsets, r.obj0Offsets.length)} 个）`);
  for (const o of r.obj0Offsets.slice(0, topOffsets)) {
    L.push(`  ${pad(hex(o.abs), 10)} ${String(o.sites).padStart(5)} 处 / ${String(o.handlers).padStart(4)} handler　例：${o.sampleHandlers.join(' ')}`);
  }
  L.push(`索引簇     (abs / stride / 索引来源) 共 ${r.indexedClusters.length} 簇（按站点数降序，前 ${Math.min(topIndexed, r.indexedClusters.length)} 簇）`);
  for (const c of r.indexedClusters.slice(0, topIndexed)) {
    L.push(`  abs=${pad(hex(c.abs), 10)} stride=${pad(c.stride, 5)} idx=${pad(c.index ?? '(无)', 26)} ${String(c.sites).padStart(5)} 处 / ${c.handlers} handler`);
  }
  const u = st.unresolved;
  L.push(`未解析     基址 ${u.base.n} 处（按寄存器：${Object.entries(u.base.byReg).map(([k, v]) => `${k}×${v}`).join(' · ') || '（无）'}）`);
  L.push(`           索引 ${u.index.n} 处 · 宽度 ${u.width.n} 处 · 数据段符号 ${st.absoluteSyms.reduce((a, x) => a + x.sites, 0)} 处（${st.absoluteSyms.length} 个符号）`);
  if (u.base.sampleEAs.length) L.push(`           样例 EA：base ${u.base.sampleEAs.slice(0, 5).map(hex).join(' ')}${u.index.sampleEAs.length ? ` · index ${u.index.sampleEAs.slice(0, 3).map(hex).join(' ')}` : ''}${u.width.sampleEAs.length ? ` · width ${u.width.sampleEAs.slice(0, 3).map(hex).join(' ')}` : ''}`);
  if (r.problems.length) L.push(`⚠ 分派表结构问题 ${r.problems.length} 条：${r.problems.slice(0, 4).join(' / ')}`);
  L.push('');
  L.push('★ 三类未解析**独立计数、不许相加** —— ★ 实测订正：`index` **不是** `base` 的子集');
  L.push('  （handler 子集实测：`index` 310 = 同时缺基址 128 + **基址已解出、只是索引没解出 175** + 其它基址 7）');
  L.push('  后一类正是最容易变成**假观察**的一档：位移少了索引那一项，却被当成"偏移 0/N" ⇒ 由 `offsetUnusable()` 剔出');
  L.push('★ handler 是**按符号去重**的；"opcode 数"与"handler 数"不是一回事（有共用的）');
  return `${L.join('\n')}\n`;
}
