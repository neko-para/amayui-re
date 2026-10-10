/**
 * tools/lib/coverage.mjs —— **函数覆盖度视图**（台账 × 反汇编语料 的**派生查询**）
 *
 * ## 它回答什么
 * 语料里有 **3800 多个 `sub_XXXXXX`**。哪些**有人登记过结论**？哪些只是"被顺手当 callee 锚了一下"？
 * 哪些**看起来收了口**？哪些是"下一步最该去取证的"？—— 这些问题以前只能靠人翻台账回答。
 *
 * ## ★ 机械判据（**没有一条靠语义猜测**）
 * | 概念 | 怎么算出来的 |
 * |---|---|
 * | **aimed(F)**（一条记录"面向"哪些函数） | 它的每个 `bin` 锚（任意 `repo`）取 **EA**，用 `functionOfEa` 归属到**包含它的函数** |
 * | **observed(F)** | 存在**在场**（`effective !== 'retracted'`）的记录，其 aimed 集合含 `F` |
 * | **atStart(F)** | 上述记录里至少有一条的锚 EA **恰好是 F 的起点** ⇒ 有人**把整个函数**当对象，而不只是引用它体内某条指令 |
 * | **callees(F)** | F 体内 `call`/`jmp sub_XXXXXX`（尾跳也算）；解不出目标的调用点只**计数**（`indirectCallSites`） |
 * | **complete(F)** | **最大不动点**：初始 = observed；反复删掉"有 callee 不在 complete 里"的成员，直到稳定。★ 互递归天然成立（都在 observed 里就都留下） |
 *
 * ## 三桶（＋一个细分）
 * * `complete` —— observed ∧ complete ∧ **atStart**（自己被人整体看过，且它调用的也都收口了）
 * * `partial-rooted` —— atStart，但 callee 闭包不全 ⇒ **差在它调用的那些**
 * * `partial-spotty` —— 只有体内锚（点状事实），**没人把它当整体看过**
 * * `unobserved` —— 一条都没有
 *
 * ## ★ 必须一起报出来的"诚实项"（否则读者会把下界当事实）
 * 1. `indirectCallSites` —— `call eax` / `call dword ptr […]` 这类**解不出目标**的调用点（全语料 3000+）
 *    ⇒ 任何"调用闭包"都只是**下界**；
 * 2. `calleesNotInInventory` —— 有 `callee` 符号在函数清单里找不到（异常导出 / 别名）；
 * 3. `unattributedAnchors` —— 锚 EA 不落在任何函数里（**不许静默丢**）；
 * 4. `subjectNamed` 只是**诊断**：`subject` 里出现 `sub_XXXXXX` 字样（机械提取）—— **不参与分桶**
 *    （`subject` 是"这条是关于什么的"，里面出现的符号可能是**被调方**）。
 *
 * ## 它**不**做什么
 * * ⛔ 不判断"这条结论对不对"（那是台账的准入与守卫的事）；
 * * ⛔ 不把结果落盘（派生视图，随语料/台账现算）；
 * * ⛔ 不说"没登记 = 不重要"——只回答"有没有人登记过"。
 */
import { functionInventory, functionOfEa } from './disasm.mjs';

/** 桶名（闭集合；报告与 `--json` 共用一份） */
export const TIERS = ['complete', 'partial-rooted', 'partial-spotty', 'unobserved'];

const SUB_RE = /^sub_[0-9A-F]{6}$/i;
const SYM_IN_SUBJECT_RE = /\bsub_[0-9A-F]{6}\b/g;

/**
 * @param entries  **投影后**的台账条目（`project()` 的输出，带 `effective` / `anchor`）
 * @param o.lstFile  语料 `.lst`
 * @param o.onlySub  宇宙只算 `sub_XXXXXX`（缺省 true —— 问题是按 `sub_` 问的）；具名函数单独计数
 * @param o.top      列表长度上限（缺省 30；`--top 0` = 全给）
 */
/**
 * ## ★ 语料里的**静态链接库代码**（CRT / MSVC STL）—— 覆盖度必须先把它摘出去
 *
 * 实测（2026-10，两个取证包）：前沿榜里混着**根本不是引擎代码**的函数 ——
 * * `sub_4E73EB` = **MSVC `/GS` 栈 cookie 校验桩**（15 字节：`cmp ecx,ds:dword_559A68` / `jnz` / `rep retn` /
 *   `jmp ___report_gsfailure`）⇒ 全语料 **1144 个调用点**都只是各函数的 /GS 尾声；
 * * `sub_40C210` = **`std::basic_string<char>::assign(const char*, size_type)`**（含自重叠分支与
 *   `_Xlength_error("string too long")` 出口）—— 连同 `sub_40C120`/`sub_40B510`/`sub_40B420` 是 STL 的私有实现。
 *
 * 不分出来，"下一步该取证谁"就会把**读库代码**排在前面，而那既不产生引擎知识、也不影响保真。
 *
 * ### 判据（机械、**只作候选**，理由必须印出来 —— 这是启发式，不是判决）
 * | 理由 | 条件 |
 * |---|---|
 * | `thin-forwarder-to-library` | 体内**无内存操作数** ∧ **不调 `sub_XXXXXX`** ∧ 有**非 `sub_`** 的转移目标 ⇒ "只往库里转一手"（/GS 桩的典型形状） |
 * | `calls-stl-internal` | 转移目标里有 **MSVC STL 的内部符号**（`^\?(?:_X|__).*@std@@`，如 `?_Xlength_error@std@@YAXPBD@Z`）⇒ 这一段是 STL 的抛出/内部实现 |
 * | ~~`calls-stl-internal`~~ **被取消**（见下） | 同上，**但**体内出现 ≥ `ENGINE_SCALE_DISP` 的**非栈**内存位移 ⇒ 它在大对象（`Engine`）上干活 ⇒ **不是**库内部实现 |
 *
 * ★ **为什么不是"见到 `?` 修饰名就算库"**：游戏自身也是 C++ —— 实测那样会把 **657 个**函数判成库
 *   （`??0GameClass@@…` 这类游戏自己的符号），而"STL 内部符号"这一条只剩个位数。
 *   ⇒ 判据要**窄**：宁可漏（少摘几个），不可滥（把引擎代码摘掉就再也看不见了）。
 * ★ 它**不**判决"这不是引擎的"；它只把"看起来是库代码"的那批**单列 + 给理由**，并从分桶/前沿里摘出去。
 *   摘出去的**看得见**（`library` 名单 + 每个的理由，`--json` 全给）。
 *
 * ### ★★ 2026-10 收窄：`calls-stl-internal` 的**假阳性**（一条机械可判的收窄规则）
 *
 * **实测的假阳性**：`sub_42B4B0`（操作数**写值原语**，全语料 131 个调用方 / 223 处调用）与它的
 * float 兄弟 `sub_42BA00` 被判成库候选，理由**只有** `calls-stl-internal` —— 因为它们内联了
 * `vector<int>::operator[]` 的越界抛出口（`0x42B805 push offset aInvalidVectorT` /
 * `0x42B80A call ?_Xout_of_range@std@@YAXPBD@Z`）。⇒ 后果：**两个引擎原语整条从两张前沿榜与分桶里消失**，
 * 节点正文里"它在第二榜上"当场不成立，后人照"榜单滚动"核还会**误判它已被登记**。
 * 这正是上面那句"不可滥"说的失败模式。
 *
 * **收窄规则（机械、可判、方向保守）**：`calls-stl-internal` **只在体内没有"引擎尺度位移"时才算数** ——
 * * 「引擎尺度位移」= 某个内存操作数的位移 **≥ `ENGINE_SCALE_DISP`（0x1000）** 且**基址不是栈**（`ebp`/`esp`）。
 * * **为什么 `0x1000` 是这条判据的界**（不是随手挑的数）：`Engine` 是个**同一个基址 + 大位移**访问的巨型对象
 *   （台账/语料里的字段都在 `+0x14D30` / `+0x5D880` / `+0x5EC8C` / `+0xA30D0` / `+0xAA514` 这一量级），
 *   而 STL 的内部实现只认**自己的小布局**（`std::string` 的 SSO 是 `+0x10`/`+0x14`、`vector` 是三指针 `+0`/`+4`/`+8`）。
 *   ⇒ "在一个位移上万字节的对象上干活"就是**引擎代码**的机械指纹。
 * * 机械事实**由 `lib/disasm.mjs` 的 `functionInventory` 现算**（`maxNonStackDisp`），本文件只做判断 ——
 *   于是它可复跑、可变异、不靠任何人读一遍反汇编。
 * * ★ **方向保守**：它只**取消**库候选（= 让更多函数**可见**），**不**新增任何库候选；栈基址的大位移一律看不见
 *   （代价写在 `disasm.mjs` 的常量注释里）。
 * * ★ **取消也要看得见**：被这条规则取消的候选单列在 `honest.libraryCandidatesCancelled`
 *   （否则"收窄"就成了一次静默的改口径）。
 * * 实测：它取消 **18/107** 个候选，逐条都是**碰引擎字段**的函数（`sub_42B4B0` / `sub_42BA00` /
 *   `sub_42A420`（文本原语）/ `sub_428990`（字体名表查找）/ `sub_40EA00`（帧拆卸）/ `sub_414AC0`（全局池分配）/ …），
 *   而**真库内部实现**（`sub_40C210` = `basic_string::assign`、`sub_40C120`）位移全是 `+0x10` 这种量级 ⇒ 保留。
 *   清单本身由 `tools/test/ledger-coverage.assets.test.mjs` 钉住（语料不在场时它 skip，不假装绿）。
 */
/** MSVC STL 的内部符号（`?_Xlength_error@std@@YAXPBD@Z` / `?_Xout_of_range@…` / `?__…@std@@`） */
const STL_INTERNAL_RE = /^\?(?:_X|__)[^@]*@std@@/;

/**
 * ★ **引擎尺度位移的阈值**（见上方长注释）：`Engine` 的字段离对象基址几十万字节，
 * 而 STL 内部实现只认自己的小布局 ⇒ ≥ 这个数的非栈位移是"引擎代码"的机械指纹。
 */
export const ENGINE_SCALE_DISP = 0x1000;

/** `calls-stl-internal` 的**触发条件**（不含收窄）—— 收窄与"被取消"共用这一份判据，不许写两遍 */
const callsStlInternal = (f) => f.externalTargets.some((t) => STL_INTERNAL_RE.test(t));

/** ★ 收窄判据：体内有 ≥ `ENGINE_SCALE_DISP` 的**非栈**位移（机械事实由 `functionInventory` 现算） */
const engineScaleOperand = (f) => (f.maxNonStackDisp ?? 0) >= ENGINE_SCALE_DISP;

export function libraryLikely(f) {
  const reasons = [];
  if (f.callees.length === 0 && !f.hasMemoryOperand && f.externalTargets.length > 0) reasons.push('thin-forwarder-to-library');
  // ★ 收窄（2026-10）：`calls-stl-internal` 只在"体内没有引擎尺度位移"时才算数 —— 理由与实测见上方长注释
  if (callsStlInternal(f) && !engineScaleOperand(f)) reasons.push('calls-stl-internal');
  return reasons;
}

/** ★ 被收窄规则**取消**的库候选理由（"取消也要看得见"：这些函数回到了引擎宇宙里） */
export function libraryLikelyCancelled(f) {
  return callsStlInternal(f) && engineScaleOperand(f) ? ['calls-stl-internal'] : [];
}

export function coverage(entries, { lstFile, onlySub = true, top = 30 } = {}) {
  const inv = functionInventory(lstFile);
  const live = entries.filter((e) => e.effective !== 'retracted');

  // ── 锚 → 函数（机械归属）──
  const aimed = new Map(); // sym → { records:Set, atStart:boolean }
  const unattributed = [];
  let binAnchors = 0;
  let atStartAnchors = 0;
  let insideAnchors = 0;
  let recordsWithBin = 0;
  for (const e of live) {
    if ((e.anchor ?? []).some((a) => a.type === 'bin')) recordsWithBin += 1;
    for (const a of e.anchor ?? []) {
      if (a.type !== 'bin') continue;
      binAnchors += 1;
      const sym = functionOfEa(inv, a.ea);
      if (!sym) {
        // ★ 分清两种"归属不到"：锚在**数据/全局**上（正常，`.lst` 里那行是 `seg002:/data:` 定义）
        //   与**谁也解释不了**（可疑）。不许混成一个数 —— 那会让人以为台账里有一堆坏锚。
        unattributed.push({ id: e.id, subject: e.subject, ea: a.ea, kind: inv.dataEas.has(a.ea) ? 'data' : 'unknown' });
        continue;
      }
      const f = inv.bySym.get(sym);
      const isStart = Boolean(f && f.ea === a.ea);
      if (isStart) atStartAnchors += 1;
      else insideAnchors += 1;
      if (!aimed.has(sym)) aimed.set(sym, { records: new Set(), atStart: false });
      const slot = aimed.get(sym);
      slot.records.add(e.id);
      if (isStart) slot.atStart = true;
    }
  }

  // ── `subject` 里的符号（**只作诊断**）──
  const subjectNamed = new Set();
  for (const e of live) {
    const m = String(e.subject ?? '').match(SYM_IN_SUBJECT_RE);
    if (m) for (const s of m) subjectNamed.add(s);
  }

  // ── 宇宙 ──
  const universe = onlySub ? inv.functions.filter((f) => SUB_RE.test(f.sym)) : inv.functions;
  const known = new Set(universe.map((f) => f.sym));
  // ★ 库代码（CRT / STL）：从分桶与前沿里摘出去，但**看得见**（判据见 `libraryLikely`）
  const library = universe
    .map((f) => ({ sym: f.sym, reasons: libraryLikely(f) }))
    .filter((x) => x.reasons.length > 0);
  const librarySet = new Set(library.map((x) => x.sym));
  const engine = universe.filter((f) => !librarySet.has(f.sym));
  // ★ 被收窄规则**取消**的库候选：也要看得见（否则"收窄"成了一次静默的改口径）
  const libraryCancelled = universe
    .map((f) => ({ sym: f.sym, reasons: libraryLikelyCancelled(f), maxNonStackDisp: f.maxNonStackDisp ?? 0 }))
    .filter((x) => x.reasons.length > 0);

  // ── complete 的最大不动点 ──
  //   ★ 库代码**直接算作已满足**：引擎函数调 `/GS` 桩（1144 处）不是"它的 callee 没收口"
  const complete = new Set([...[...aimed.keys()].filter((s) => known.has(s)), ...librarySet]);
  let changed = true;
  while (changed) {
    changed = false;
    for (const sym of [...complete]) {
      const f = inv.bySym.get(sym);
      const bad = (f?.callees ?? []).some((c) => !complete.has(c));
      if (bad) {
        complete.delete(sym);
        changed = true;
      }
    }
  }

  // ── 分桶 + 逐函数行 ──
  const rows = [];
  const buckets = Object.fromEntries(TIERS.map((t) => [t, 0]));
  for (const f of engine) {
    const a = aimed.get(f.sym);
    const observed = Boolean(a);
    const isComplete = complete.has(f.sym);
    let tier;
    if (!observed) tier = 'unobserved';
    else if (isComplete && a.atStart) tier = 'complete';
    else if (a.atStart) tier = 'partial-rooted';
    else tier = 'partial-spotty';
    buckets[tier] += 1;
    const unobservedCallees = f.callees.filter((c) => !aimed.has(c));
    rows.push({
      sym: f.sym,
      ea: f.ea,
      tier,
      anchors: a ? a.records.size : 0,
      atStart: a ? a.atStart : false,
      subjectNamed: subjectNamed.has(f.sym),
      callees: f.callees.length,
      unobservedCallees: unobservedCallees.length,
      indirectCallSites: f.indirectCallSites,
    });
  }

  // ── 反向调用表：**全部**函数的调用方（不只已登记的）──
  //   ★ 为什么要它：前沿按"被多少**已登记**函数调用"排序（= 它挡了多少活），但那会**低估**一种函数 ——
  //     **叶子助手**（如 `sub_408050`：全语料 110 个调用方、205 处调用，却没有 callee）。
  //     它的 callee 方向没有前沿，可它一旦登记就能让那 5 个已登记调用方接近收口。
  //     ⇒ 两个数一起给：`calledBy`（已登记调用方 = 挡了多少活）+ `calledByAll`（全语料调用方 = 用得多广）。
  const allCallers = new Map(); // sym → Set(caller sym)
  const allSites = new Map(); // sym → 全语料的**调用处数**（同一函数调两次算两处）
  for (const f of engine) {
    for (const c of f.callees) {
      if (!allCallers.has(c)) allCallers.set(c, new Set());
      allCallers.get(c).add(f.sym);
      allSites.set(c, (allSites.get(c) ?? 0) + (f.calleeHits?.get(c) ?? 1));
    }
  }

  // ── 前沿（**下一步该取证谁**）：未被登记、但被已登记的函数**直接调用** ──
  const callers = new Map(); // sym → Set(caller sym)，只算**已登记**的调用方
  for (const f of engine) {
    if (!aimed.has(f.sym)) continue;
    for (const c of f.callees) {
      if (aimed.has(c) || librarySet.has(c)) continue;
      if (!callers.has(c)) callers.set(c, new Set());
      callers.get(c).add(f.sym);
    }
  }
  const frontierDirect = [...callers]
    .map(([sym, set]) => ({
      sym,
      calledBy: set.size,
      calledByAll: allCallers.get(sym)?.size ?? 0,
      callSitesAll: allSites.get(sym) ?? 0,
      callers: [...set].sort(),
    }))
    .sort((x, y) => y.calledBy - x.calledBy || y.calledByAll - x.calledByAll || (x.sym < y.sym ? -1 : 1));

  // ── 传递前沿：从已登记函数出发 BFS，按"距离"分层（只算未登记的那些）──
  const depth = new Map();
  let layer = [...aimed.keys()].filter((s) => known.has(s));
  for (const s of layer) depth.set(s, 0);
  let d = 0;
  const byDepth = {};
  while (layer.length && d < 50) {
    d += 1;
    const next = [];
    for (const sym of layer) {
      for (const c of inv.bySym.get(sym)?.callees ?? []) {
        if (!known.has(c) || depth.has(c)) continue;
        depth.set(c, d);
        next.push(c);
      }
    }
    if (next.length) byDepth[d] = next.length;
    layer = next;
  }
  const reachableUnobserved = [...depth].filter(([s, dd]) => dd > 0 && !aimed.has(s)).map(([s]) => s);

  const slice = (arr) => (top > 0 ? arr.slice(0, top) : arr);
  const calleesNotInInventory = new Set();
  for (const f of universe) for (const c of f.callees) if (!inv.bySym.has(c)) calleesNotInInventory.add(c);

  return {
    listing: lstFile,
    universe: {
      total: inv.functions.length,
      counted: engine.length,
      named: inv.functions.filter((f) => !SUB_RE.test(f.sym)).map((f) => f.sym),
      library: library.length,
      noCallee: engine.filter((f) => f.callees.length === 0).length,
    },
    anchors: {
      bin: binAnchors,
      attributed: binAnchors - unattributed.length,
      unattributed: unattributed.filter((x) => x.kind === 'unknown').length,
      dataAnchors: unattributed.filter((x) => x.kind === 'data').length,
      atStart: atStartAnchors,
      insideBody: insideAnchors,
      records: live.length,
      recordsWithBin,
    },
    buckets,
    observed: [...aimed.keys()].filter((s) => known.has(s)).length,
    completeList: rows.filter((r) => r.tier === 'complete').map((r) => r.sym).sort(),
    partialRootedList: rows.filter((r) => r.tier === 'partial-rooted').map((r) => r.sym),
    partialSpottyList: rows.filter((r) => r.tier === 'partial-spotty').map((r) => r.sym),
    frontier: {
      directCount: frontierDirect.length,
      direct: slice(frontierDirect),
      // ★ 另一种 triage：**全语料用得最多、而自己还没登记**的函数（叶子助手通常在这张榜上）
      mostUsedUnregistered: slice(
        [...allCallers]
          .filter(([sym]) => known.has(sym) && !aimed.has(sym) && !librarySet.has(sym))
          .map(([sym, set]) => ({ sym, calledByAll: set.size, callSitesAll: allSites.get(sym) ?? 0 }))
          .sort((x, y) => y.calledByAll - x.calledByAll || (x.sym < y.sym ? -1 : 1)),
      ),
      transitiveCount: reachableUnobserved.length,
      byDepth,
    },
    honest: {
      indirectCallSites: inv.indirectCallSites,
      // ★ 摘出去的东西必须看得见（库代码候选 + 每个的理由）
      libraryCandidates: slice(library),
      // ★ **被收窄规则取消**的那些也要看得见（它们回到了引擎宇宙：分桶 / 两张榜里都能看到）
      libraryCandidatesCancelled: slice(libraryCancelled),
      calleesNotInInventory: [...calleesNotInInventory].sort(),
      unattributedAnchors: slice(unattributed.filter((x) => x.kind === 'unknown')),
      dataAnchors: slice(unattributed.filter((x) => x.kind === 'data')),
      subjectNamedButUnanchored: [...subjectNamed].filter((s) => known.has(s) && !aimed.has(s) && !librarySet.has(s)).sort(),
    },
    rows: slice(rows.filter((r) => r.tier !== 'unobserved').sort((x, y) => (x.sym < y.sym ? -1 : 1))),
    allRows: rows,
  };
}

/** 人读的一屏（数字都由**跑一遍**得出，不手写） */
export function coverageText(r) {
  const L = [];
  const pct = (n, d) => `${((100 * n) / Math.max(d, 1)).toFixed(1)}%`;
  L.push(`语料          ${r.listing}`);
  L.push(`函数宇宙      ${r.universe.total} 个 \`proc near\`（本次计入 ${r.universe.counted} 个 \`sub_XXXXXX\`；具名 ${r.universe.named.length} 个不计：${r.universe.named.join(' / ') || '（无）'}）`);
  L.push(`无 callee     ${r.universe.noCallee} 个（叶子函数）`);
  L.push('');
  L.push(`台账          ${r.anchors.records} 条在场记录，其中 ${r.anchors.recordsWithBin} 条带 \`bin\` 锚（共 ${r.anchors.bin} 个）`);
  L.push(`锚 → 函数    归属成功 ${r.anchors.attributed} 个（锚在**函数起点** ${r.anchors.atStart} · 锚在体内 ${r.anchors.insideBody}）· 归属不到 ${r.anchors.unattributed}`);
  L.push('');
  L.push('分桶（按 `sub_XXXXXX` 计）');
  L.push(`  ✅ 完全收口   ${String(r.buckets.complete).padStart(5)}　${pct(r.buckets.complete, r.universe.counted)}　自己被人整体看过（锚在函数起点），且它调用的也都收口了`);
  L.push(`  🟡 部分·有根   ${String(r.buckets['partial-rooted']).padStart(5)}　${pct(r.buckets['partial-rooted'], r.universe.counted)}　**锚在函数起点**但 callee 闭包不全 ⇒ 差在它调用的那些`);
  L.push(`  🟠 部分·点状   ${String(r.buckets['partial-spotty']).padStart(5)}　${pct(r.buckets['partial-spotty'], r.universe.counted)}　只有体内锚（引用过它某条指令），**没人把它当整体看过**`);
  L.push(`  ⬜ 未登记     ${String(r.buckets.unobserved).padStart(5)}　${pct(r.buckets.unobserved, r.universe.counted)}　台账里一条都没指向它`);
  L.push('');
  L.push(`前沿（**下一步该取证谁**）`);
  L.push(`  直接调用前沿  ${r.frontier.directCount} 个未登记函数被**已登记**函数直接调用`);
  for (const f of r.frontier.direct.slice(0, 12)) {
    L.push(`      ${f.sym}  ← ${f.calledBy} 个已登记调用方 / 全语料 ${f.calledByAll} 个调用方 · ${f.callSitesAll} 处调用（${f.callers.slice(0, 3).join(' · ')}${f.calledBy > 3 ? ' …' : ''}）`);
  }
  L.push(`  ★ 另一种 triage：**全语料用得最多、而自己还没登记**（叶子助手通常在这张榜上）：`);
  for (const f of r.frontier.mostUsedUnregistered.slice(0, 6)) L.push(`      ${f.sym}  ← 全语料 ${f.calledByAll} 个调用方 · ${f.callSitesAll} 处调用`);
  L.push(`  传递可达      ${r.frontier.transitiveCount} 个未登记函数落在"已登记函数的调用闭包"里，按距离分层：` +
    Object.entries(r.frontier.byDepth).map(([k, v]) => `${k} 跳=${v}`).join(' · '));
  L.push('');
  L.push('诚实项（★ 不报出来就会被读成事实）');
  L.push(`  ★ **解不出目标的调用点 ${r.honest.indirectCallSites} 个**（\`call eax\` / \`call dword ptr […]\`）⇒ 上面的"调用闭包"只是**下界**`);
  L.push(`  callee 不在函数清单里：${r.honest.calleesNotInInventory.length} 个${r.honest.calleesNotInInventory.length ? `（${r.honest.calleesNotInInventory.slice(0, 5).join(' · ')}…）` : ''}`);
  L.push(`  ★ **被收窄规则取消**的库候选：${r.honest.libraryCandidatesCancelled.length} 个（体内有 ≥ 0x${ENGINE_SCALE_DISP.toString(16)} 的非栈位移 ⇒ 判回引擎代码，于是它们在分桶与两张榜里都看得见）` + (r.honest.libraryCandidatesCancelled.length ? `：${r.honest.libraryCandidatesCancelled.slice(0, 6).map((x) => x.sym).join(' · ')}${r.honest.libraryCandidatesCancelled.length > 6 ? ' …' : ''}` : ''));
  L.push(`  锚 EA 归属不到任何函数：${r.anchors.unattributed} 个（\`.text\` 里不被任何 \`proc near\` 覆盖 —— 例如 COLLAPSED 的库函数）`);
  L.push(`  锚在**数据 / 全局**上：${r.anchors.dataAnchors} 个（★ 正常：那是 \`.data\` / \`seg002\` 里的定义点，不是归属失败）`);
  L.push(`  \`subject\` 里点名、但没有任何锚落在它身上：${r.honest.subjectNamedButUnanchored.length} 个（诊断项，不参与分桶）`);
  return L.join('\n');
}
