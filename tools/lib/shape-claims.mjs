/**
 * tools/lib/shape-claims.mjs —— **校验器功能②**：给定模型产出，检查汇编是否匹配
 *
 * ## 契约（模型只写判断；能从 EA + 语料机械读出的东西一律不进产物）
 * 模型文件（\`amayui.claims/1\`）的分节：
 *   \`types\`（**按类型 id 共享一份**：每条成员 = 偏移 + 宽度/类型+count；洞不写）
 *   \`carriers\`（机器槽 → 类型：\`arg\`(reg/栈偏移) / \`local\` / \`global\` —— **字段不是载体**，它是类型的成员）
 *   \`roles\`（段落角色：\`span\` **必须逐字等于** \`shape-regions\` 导出的一个段落）
 *   \`domains\` / \`escapes\` / \`identities\`（可选；本版只做结构检查）
 *   \`typesFrom\`（可选：引用另一个 claims 文件共享类型；**同名类型按成员增量合并**）
 *
 * ## 两道守卫（把"不耦合业务"与"不写转录"变成机械判据）
 * * **词汇守卫**：出现 \`opcode\` / \`argc\` / \`operand\` / \`lengthWord\` … ⇒ 红（那是 B 层：\`data/ledger/\`）
 * * **冗余守卫**：出现 \`mn\` / \`defines\` / \`callee\` / \`const\` … 这些字段名 ⇒ 红（能从 EA 读出）
 *
 * ## 义务（红了都要有理由；⛔ 没有一档是"静默通过"）
 * G1 类型自洽（不重叠 / 不越 size）· G2 覆盖（两级：容器的成员 + **数组成员的元素内成员**）
 * G3 步长（数组形态：观测步长 == size(元素类型)）· G4 数组边界（末尾地址必须在语料里被引用）
 * G5/G5' **二级对象下钻**（宿主是指针 + 声明了目标类型 ⇒ 用目标类型的成员表再跑一遍；
 *   目标类型为 null ⇒ **不透明桶**单列；宿主没声明 ptr ⇒ 红）
 * G6 未观察成员（用**形状层观察集**判定，它能解折叠形态；字面量扫描只是下界）
 * **G7 可证伪性**：成员声明 `kind:"ptr"` 却在**本模型载体集里零解引用** ⇒ 单列"空话声明"
 *   （⛔ **不算通过**；不判红 —— 类型是共享的，别的族文件可能才用它）
 * **G8 成员声明自洽**：成员只有两种合法形态 —— 内联值（`width`）或数组（`type`+`count`，元素类型要给 `size`）；
 *   缺一半 ⇒ 红（否则 `membersOf` 会把它**静默当成 1 字节槽**，而报告里一条红都没有）
 *   ★ `type` 在成员上有**两个互不相干的意思**，按 `kind` 分岔：`kind:"ptr"` 时它是**被指向的类型**
 *     （此时不该给 `count`，成员自己的跨度是 `width`）；否则它是**数组元素类型**（必须配 `count`）。
 * J3 角色（span == 导出段落 且 段内全 ±const/init）· 守恒①（obj0 访问）· 守恒②（二级对象三档和）
 * **超出解析深度**：**对象层级 ≥3** 的访问单列（判定器只下钻到层级 2）—— ⛔ 不许静默丢
 *
 * ## 守恒③：**站点按基址分桶**（⛔ 不许静默丢）
 * `站点 = obj0 相对 + lea 中间量 + 二级对象 + 二级对象内嵌派生 + ds: 符号 + 未解析基址`
 * ★ 踩过：守恒①② 只对 **obj0** 与**二级对象**记账 ⇒ 另外四类从访问总数里消失 ——
 *   `ds:` 段前缀符号（进程全局，没有"对象根+位移"形状）· "基址没解出来" · 以及**守恒③上线当天抓出来的**
 *   第三类：`lea eax,[<二级对象>+0x10]` 之后再 `[eax+k]`（`addr` 基址拿不到宿主偏移 ⇒ `pointeeHost=null`）
 *   —— 它既不算 obj0、也不算二级对象、也不算 deep，**从所有桶里掉出去**。
 *   表现是**数字静默变小**，不是报错。⇒ 逐类报数（带样例 EA），并用守恒③ 钉住。
 *   ★ 形状层口径（`docs/02-engine/shape-census.md` §3 / §7）：这几类**必须分开记**、且**未解析要报到能处置的粒度**
 *   （按基址寄存器分组 + 给样例 EA）；它们**不在**类型成员表的判定范围内（⛔ 不是"已通过"）。
 *
 * 真源：\`.lst\`（算术 + 锚）· \`.c\`（调用约定 / 结构第二意见）。⛔ 不读 \`apps/**\`（同层产物）。
 */
import fs from 'node:fs';
import path from 'node:path';

import { analyzeLines, functionSpans, loadListing, objRootRegs } from './shape-access.mjs';
import { deriveRegions } from './shape-regions.mjs';

/** 词汇守卫：**引擎 / 业务概念词**（B 层的东西混进汇编层 ⇒ 红） */
export const VOCAB = ['opcode', 'argc', 'operand', 'lengthWord', 'op1', 'op2', '操作数', '长度字'];
/** 冗余守卫：这些是**能从 EA + 语料机械读出**的字段名 ⇒ 不许当字段出现 */
export const REDUNDANT = ['mn', 'defines', 'callee', 'argImm', 'text', 'line', 'const'];

const num = (v) => (typeof v === 'number' ? v : Number.parseInt(String(v), 16));
const hex = (n) => (typeof n === 'number' ? `0x${n.toString(16)}` : String(n));
const pad = (s, n) => String(s).padEnd(n);

/**
 * **对象层级**（level）：1 = 对象就是 obj0；2 = 对象是"obj0 的某个成员里读出的指针"；3 = 再一跳……
 * ★ 踩过：`baseHop` 是**基址值的来路**（1 = 它的来源是 obj0 的成员），**对象层级 = baseHop + 1**。
 *   把两者混为一谈，"层级 3"整类（实测 812 处）就会漏出"超出解析深度"桶 ——
 *   而上限冻结在层级 2 时，那一类**正是必须被发现的东西**。
 */
export const objectLevel = (s) => ((s.pointeePath?.length ?? (typeof s.pointeeHost === 'number' ? 1 : 0)) + 1);
/** 下钻上限：**层级 2**（只下钻一层）。层级 ≥3 一律进"超出解析深度"桶，带 EA，⛔ 不静默丢 */
/** 报告用的阈值：**层级 ≤ 2 = 一跳可推导**；> 2 = 跨两级以上（不判，交局部类型载体）。★ 这是**报告口径**，不是判定开关 */
export const DRILL_MAX_LEVEL = 2;

/**
 * **这一处的位移能不能当"偏移观察"用**？返回 `null`（能）或不可用的原因。
 *
 * ★ 踩过（实测：handler 子集里有 55 处 `Engine + 0x0`，一度被当成"偏移 0"的观察）：
 * 1. `take-address`：`lea` **且索引已解出** ⇒ 报出来的 `absDisp` 只是"位移项"，真正的偏移随索引变
 *    （`lea ecx,[esi+edx*8+5D8F4h]` 的 `abs=0x5d8f4` 是基址，不是这一次访问的槽）⇒ 取址中间量。
 * 2. `index-unresolved`：基址解出来了、**索引没解出来** ⇒ 报出来的 `absDisp` **少了索引那一项**
 *    （`mov [esi+edx*4], eax` 报成 `abs=0x0`）。⛔ 把它当"偏移 0"的观察 = 编一条**假观察**：
 *    轻则多一条残差，重则"正好命中某个成员"⇒ **假解释**。
 * ⇒ 两种都从 `obj0` 偏移观察里剔出去，另计一档（与"取址中间量"分开报，原因不同）。
 */
export function offsetUnusable(s) {
  if (s.absDisp === null || s.absDisp === undefined) return null;   // 不是 obj0 相对：不适用
  if (s.access === 'a' && s.indexSym) return 'take-address';
  if (s.problems?.includes('index')) return 'index-unresolved';
  return null;
}

/** 内置样本：13 个函数的载体 + 两个共享类型（**数据即代码**，不新增入库 JSON ⇒ 不欠同名 .md） */
export function sampleModel() {
  const fns = [
    'sub_42C5E0', 'sub_42C620', 'sub_42CA00', 'sub_42CEC0', 'sub_430A50',
    'sub_42C660', 'sub_42C750', 'sub_42C790',
    'sub_42C6A0', 'sub_42C6E0', 'sub_42C720',
    'sub_42CA50', 'sub_40ED40',
  ];
  return {
    schema: 'amayui.claims/1',
    obj0: { isGlobal: true },
    types: {
      T0: {
        size: null,
        complete: false,
        members: [
          { off: '0x5d880', width: 4 }, { off: '0x5d884', width: 4 }, { off: '0x5d888', width: 4 },
          { off: '0x5d88c', width: 4 }, { off: '0x5d890', width: 4 },
          { off: '0x5d894', type: 'T1', count: 40 },
        ],
      },
      T1: {
        size: 120,
        complete: true,
        members: Array.from({ length: 30 }, (_, i) => ({ off: `0x${(i * 4).toString(16).padStart(2, '0')}`, width: 4 })),
      },
    },
    carriers: fns.map((fn, i) => ({ id: `c${String(i + 1).padStart(2, '0')}`, kind: 'arg', fn, reg: 'ecx', type: 'T0*' })),
  };
}

/** 词汇守卫：概念词（键与值都查）。返回违规词列表（空 = 通过） */
export function vocabViolations(model) {
  const text = JSON.stringify(model);
  return VOCAB.filter((w) => new RegExp(w, 'i').test(text));
}

/** 冗余守卫：字段名。返回违规字段名列表（空 = 通过） */
export function redundantKeys(model) {
  const keys = new Set();
  (function walk(o) {
    if (Array.isArray(o)) o.forEach(walk);
    else if (o && typeof o === 'object') for (const [k, v] of Object.entries(o)) { keys.add(k); walk(v); }
  })(model);
  return REDUNDANT.filter((k) => keys.has(k));
}

/** `typesFrom` 解析：同名类型按**成员**合并（同 off 以本文件为准）—— 增量补成员不该逼人抄整张表 */
export function resolveTypes(model, baseDir, resolve = null) {
  if (!model.typesFrom) return model;
  // ★ `resolve` 是给测试与调用方留的接缝（本仓测试不写盘 ⇒ 不能靠"临时造一份引用文件"来测守卫）
  const ref = resolve
    ? resolve(model.typesFrom)
    : JSON.parse(fs.readFileSync(path.join(baseDir, model.typesFrom), 'utf8'));
  const ids = [...new Set([...Object.keys(ref.types ?? {}), ...Object.keys(model.types ?? {})])];
  const types = Object.fromEntries(ids.map((k) => {
    const a = ref.types?.[k] ?? {};
    const b = model.types?.[k] ?? {};
    const byOff = new Map((a.members ?? []).map((m) => [String(m.off), m]));
    for (const m of b.members ?? []) byOff.set(String(m.off), m);
    return [k, { ...a, ...b, members: [...byOff.values()] }];
  }));
  return { ...ref, ...model, types, obj0: model.obj0 ?? ref.obj0 };
}

/**
 * 跑一遍全部义务。
 * @param {object} model  claims 对象（已解析 typesFrom 与否都行 —— 传 baseDir 即自行解析）
 * @param {{lines?: string[], baseDir?: string, observed?: Set<number>, roots?: Map<string,string>, resolve?: (name:string)=>object}} opts
 *   `roots` / `resolve` 是接缝：默认都从**语料**取（`.c` 的调用约定 / 磁盘上的被引用文件），
 *   传入即可在**不读语料**的纯测试里跑守卫与分桶（本仓测试不写盘）。
 * @returns {{findings: object[], notes: string[], perCarrier: object[], cons: object}}
 */
/**
 * 一个站点落在这个窗口里吗？★ 有 `sweep` 的（条数未观察）还要**索引身份 + 步长**都对上。
 * ★ 提到模块级：**局部类型载体**（G10）与 obj0 的成员匹配**共用这一份判据**（⛔ 两份实现必然差数）。
 */
export function matchesWindowGlobal(w, s) {
  if (s.absDisp < w.from || s.absDisp >= w.to) return false;
  if (w.kind !== 'array' || !w.sweep) return true;      // 内联值 / 有 count 的数组：窗口内即算
  if (s.stride === w.sweep.stride && s.indexSym === w.sweep.index) return true;   // 同一把索引扫过
  return w.delta === 0 && s.absDisp === w.m.offN && !s.indexSym;                  // 元素 0 的裸位移
}

export function checkClaims(model0, { lines = loadListing(), baseDir = process.cwd(), observed = null, roots = null, resolve = null } = {}) {
  const findings = [];
  const disputes = [];   // G12：合法争议（会被报告单列）
  const notes = [];
  const red = (where, why) => findings.push({ where, why });

  const M = resolveTypes(model0, baseDir, resolve);

  // ★★ 两道守卫跑在**解析后**的模型上（`M` ⊇ `model0`）：
  //   踩过 —— 旧写法只查原始 `model0`，于是 `typesFrom` 引用的那份类型文件里塞 `argc` / `mn`
  //   时**两道守卫一起静默失效**（实测：引用文件带两个违规字段，`claims` 仍然退出码 0）。
  //   类型现在集中在 `data/shape/types.json`、族文件一律 `typesFrom` 引用它 ⇒ 不修这一条，
  //   守卫就成了"只守族文件、不守类型表"。
  for (const w of vocabViolations(M)) red('守卫·词汇', `出现业务词「${w}」—— 汇编层不许有它（那是 data/ledger/ 的事）`);
  for (const k of redundantKeys(M)) red('守卫·冗余', `字段名「${k}」是能从 EA + 语料机械读出的转录 ⇒ 不许进产物`);

  const types = M.types ?? {};

  // ── 语料侧的两个 oracle
  const occurrences = new Map();
  // ★ 1~8 位（不是 4~8）：短位移 5F4h / 65Ch / 44h 也是被引用地，
  //   写成 {4,8} 会让所有小于 0x1000 的末尾地址都变成假零引用（实测 112704 次不可见）
  for (const m of lines.join('\n').matchAll(/\b([0-9A-F]{1,8})h\b/gi)) {
    const v = Number.parseInt(m[1], 16);
    occurrences.set(v, (occurrences.get(v) ?? 0) + 1);
  }
  const refs = (abs) => occurrences.get(abs) ?? 0;
  const spans = functionSpans(lines);
  /** EA → 指令文本（惰性建；多个守卫共用，别各扫一遍） */
  let eaTextAll = null;
  const eaTextOf = (ea) => {
    if (!eaTextAll) {
      eaTextAll = new Map();
      for (const ln of lines) {
        const m = /^[A-Za-z_.][\w.]*:([0-9A-F]{8})\s+(.*)$/.exec(ln);
        if (m) eaTextAll.set(Number.parseInt(m[1], 16), m[2]);
      }
    }
    return eaTextAll.get(ea) ?? null;
  };
  /** 符号 → 指令跨度（含结束地址；G16 要判"这条 EA 在不在函数自己的体里"） */
  const spanBySym = new Map();
  for (const s of spans) {
    const next = spans.find((x) => x.from > s.from);
    spanBySym.set(s.sym, { from: s.from, to: s.to ?? (next ? next.from - 1 : lines.length - 1), ea: s.ea, ...s });
  }
  /** 这条 EA 是否在某个函数的**指令体**里（G16 判「入口约定」用；按符号惰性建集合） */
  const eaInBody = (sp, ea) => {
    if (!sp._eas) {
      const s2 = new Set();
      for (let i = sp.from; i <= sp.to && i < lines.length; i += 1) {
        const m = /^[A-Za-z_.][\w.]*:([0-9A-F]{8})\s/.exec(lines[i]);
        if (m) s2.add(Number.parseInt(m[1], 16));
      }
      sp._eas = s2;
    }
    return sp._eas.has(ea);
  };
  const rootsMap = roots ?? objRootRegs();
  /** 观察集（形状层）：能解折叠形态 ⇒ G6 的正确判据；**必须显式传入**（本模块不写缓存文件） */
  const OBS = observed;
  if (!OBS) notes.push('G6：未传观察集（observed）⇒ 只用字面量出现次数，**折叠形态会被误报未观察**（下界）');

  /** `kind:"ptr"` 的成员：跨度就是**指针自己的宽度**（`type` 在这里是**被指向**的类型） */
  const isPtr = (m) => m.kind === 'ptr';
  /**
   * **扫描形态**（数组才可能有）：`count` 给了条数 ⇒ 由计数推；`count: null`（条数未观察）⇒ **必须**显式声明。
   * ★ 为什么 `count:null` 必须带 `sweep`：没有条数 ⇒ 窗口**无界** ⇒ 会把窗口外的偏移也"解释"掉
   *   （实测风险：`0x5ee04` 的无界窗口一路吞到 `0xa609c` ⇒ **假解释**）。有了"步长 + 索引身份"这条约束，
   *   匹配就只认"同一把索引、同一步长扫过"的站点。
   */
  const sweepOf = (m) => {
    if (m.sweep == null) return null;
    return { stride: typeof m.sweep.stride === 'number' ? m.sweep.stride : num(m.sweep.stride), index: m.sweep.index ?? null };
  };
  /** 数组的**元素步长**：numeric count ⇒ `size(元素类型)`；`count:null` ⇒ `sweep.stride`；不是数组 ⇒ null */
  const arrayStride = (m) => {
    if (isPtr(m) || m.type == null) return null;
    if (m.count == null) return sweepOf(m)?.stride ?? null;
    const et = types[m.type];
    return et?.size != null ? et.size : null;
  };
  const memberExtent = (t, mem) => {
    if (isPtr(mem)) return mem.width ?? null;
    if (mem.type != null && mem.count != null) {
      const et = types[mem.type];
      return et?.size != null ? et.size * mem.count : null;
    }
    if (mem.type != null && mem.count === null) return null;   // 条数未观察 ⇒ 跨度未知（**明说**，不当 1 字节）
    return mem.width != null ? mem.width : null;
  };
  const membersOf = (t) => (t?.members ?? []).map((m) => ({ ...m, offN: num(m.off), ext: memberExtent(t, m) })).sort((a, b) => a.offN - b.offN);
  const membersOfId = (tid) => membersOf(types[tid]);

  /**
   * 一个类型的**匹配窗口**：每个已声明成员至少有一个（`delta` 0）；数组成员还可以有**声明的视图**
   * （`views[].elementDelta ≠ 0`）—— 那是**模型的推测**："这一段字节是**同一个数组**、索引差 k"。
   * ★ 依据 `docs/02-engine/shape-census.md` §4：**原点表是声明的约定，不是语料事实**；
   *   判定器只做**算术自洽**（`member.off + stride·delta == 该段字节的位移`）与**可证伪性**（零观察就要报出来）。
   * ★ 视图**不进 G1 的分区**（它是一种**读法**，会和别的成员的范围重叠 —— 靠 `(步长, 索引身份)` 区分）。
   */
  const declaredViews = [];
  const windowsOf = (tid) => {
    const out = [];
    for (const m of membersOfId(tid)) {
      const stride = arrayStride(m);
      const elemSize = m.type != null ? (types[m.type]?.size ?? null) : null;
      if (stride == null || elemSize == null) {
        out.push({ m, delta: 0, from: m.offN, to: m.offN + (m.ext ?? 1), kind: 'inline', sweep: null, stride: null, elemSize: null });
        continue;
      }
      // ★ count 已知 ⇒ 窗口覆盖**整条数组**（elemSize*count），不是只覆盖第 1 个元素；
      //   count:null（sweep）⇒ 保持「单元素窗口 + 索引身份」的判法（无界窗口不能靠范围兜）。
      // ★ 只对 delta 0（数组本体）加宽；视图窗口保持单元素 —— 视图是「同一数组的另一种读法」，
      //   加宽会让相邻视图互相「观察」到对方的站点，把「零观察」判据搅乱（实测 ★⑥ 会红）。
      const countWide = (m.count != null && Number.isFinite(num(m.count))) ? num(m.count) * elemSize : elemSize;
      for (const d of [0, ...(m.views ?? []).map((v) => num(v.elementDelta))]) {
        const width = d === 0 ? countWide : elemSize;   // 视图窗口保持单元素（见上）
        out.push({ m, delta: d, from: m.offN + stride * d, to: m.offN + stride * d + width, kind: 'array', sweep: sweepOf(m), stride, elemSize });
      }
    }
    return out;
  };

  // ── G1 类型自洽 · G8 成员声明自洽（**声明可判定性**）
  //   ★ 为什么 G8 是红而不是提示：成员只有两种合法形态 —— **内联值**（给 `width`）或**数组**
  //     （给 `type` + `count`，且元素类型必须给 `size`）。少了任何一半，跨度就算不出来，
  //     而 `membersOf` 会把它**静默当成 1 字节槽**（实测：`{off,type}` 与 `{off,type,count}`+元素无 `size`
  //     都让帧访问变成"不落在任何已声明成员上"的残差，却一条红都没有）。
  const spanUnknown = [];
  const countUnknown = [];
  for (const [tid, t] of Object.entries(types)) {
    const ms = membersOf(t);
    for (const m of ms) {
      const label = `${tid}+${hex(m.offN)}`;
      const sw = sweepOf(m);
      // ★★ `type` 在成员上有**两个互不相干的意思**，必须按 `kind` 分岔（实测踩出来的坑）：
      //   ① `kind:"ptr"` 时：`type` = **被指向的类型**（G5' 下钻用），成员自己的跨度是 `width`；
      //   ② 否则 `type` = **数组元素类型**：`count` 给条数、`count:null` = 条数未观察（必须配 `sweep`）。
      //   ⇒ 不分支的话，"ptr 成员声明目标类型"会被误判成"类型缺 count"，把**合法声明**判红。
      //   ★ `count` **省略**（undefined）与 `count: null` 是**两件事**：前者是"声明不完整"（红），
      //     后者是"明确声明条数未观察"（要配 `sweep`，不红）—— 少了这一层，"忘了写 count"会静默退化。
      if (m.kind === 'ptr') {
        if (m.width == null) red(`G8 ${label}`, '`kind:"ptr"` 的成员必须给 width（指针本身的宽度）');
        if (m.count != null) red(`G8 ${label}`, '`kind:"ptr"` 的成员不该给 count（`type` 在这里是**被指向的类型**，不是数组元素类型）');
        if (m.sweep != null) red(`G8 ${label}`, '`kind:"ptr"` 的成员不该给 `sweep`');
      } else if (m.type != null && m.count === undefined) {
        red(`G8 ${label}`, '声明了元素类型却没给 count ⇒ 跨度算不出（会被**静默**当成 1 字节槽）；**条数未观察**要显式写 `count:null` + `sweep`');
      } else if (m.type != null && m.count === null) {
        // **条数未观察**的数组：形态可说、边界不可说
        if (m.width != null) red(`G8 ${label}`, '同时给了 width（内联值）与 type（数组）⇒ 二选一');
        if (!sw || sw.stride == null || !sw.index) {
          red(`G8 ${label}`, '`count:null`（条数未观察）必须同时给 `sweep:{stride,index}` —— 否则窗口**无界**，会把窗口外的偏移也解释掉（**假解释**）');
        }
        const et = types[m.type];
        if (et?.size == null) spanUnknown.push(`${label}（元素类型 ${m.type} 没给 size）`);
        else { countUnknown.push(`${label}（${m.type} 每条 ${et.size} 字节、索引 ${sw?.index ?? '(未声明)'}）`); if (sw?.stride != null && sw.stride !== et.size) red(`G3 ${label}`, `sweep.stride ${sw.stride} != size(${m.type}) = ${et.size}`); }
      } else if (m.type != null && m.count != null) {
        if (m.width != null) red(`G8 ${label}`, '同时声明了 width（内联值）与 type+count（数组）⇒ 二选一');
        if (m.sweep != null) red(`G8 ${label}`, '`count` 已给出条数 ⇒ 不该再给 `sweep`');
        const et = types[m.type];
        if (et?.size == null) spanUnknown.push(`${label}（元素类型 ${m.type} 没给 size）`);
        else if (m.ext == null) red(`G8 ${label}`, '数组成员的跨度算不出');
      } else if (m.count != null && m.type == null) {
        red(`G8 ${label}`, '给了 count 却没给元素类型 ⇒ 跨度算不出');
      } else if (m.width == null) {
        red(`G8 ${label}`, '既没给 width（内联值）也没给 type（数组）⇒ 这条声明没有内容');
      }
      // `kind` 的闭集合：成员的 kind 只有 `ptr` 有意义（内联值/数组都不写 kind）
      if (m.kind !== undefined && m.kind !== 'ptr') red(`G8 ${label}`, `成员 kind 只允许 "ptr"（收到 "${m.kind}"）—— 内联值不写 kind`);
      // ── G9：**视图 = 模型的推测**（"这段字节是同一个数组、索引差 k"）—— 只判算术自洽与可证伪性
      for (const v of m.views ?? []) {
        const stride = arrayStride(m);
        if (stride == null) { red(`G9 ${label}`, '声明了 views，但这条成员不是数组（拿不到元素步长）⇒ 视图窗口算不出来'); continue; }
        const elemSize = types[m.type]?.size ?? null;
        if (elemSize == null) { red(`G9 ${label}`, '声明了 views，但元素类型没给 size ⇒ 视图窗口算不出来'); continue; }
        const d = num(v.elementDelta);
        if (!Number.isInteger(d) || d === 0) { red(`G9 ${label}`, `views[].elementDelta 必须是**非零整数**（收到 ${JSON.stringify(v.elementDelta)}）`); continue; }
        // 视图是不是"多余"的：它的字节范围已被 delta 0（或另一个视图）完全覆盖 ⇒ 空话声明（单列，不算通过）
        const from = m.offN + stride * d; const to = from + elemSize;
        // ★ S2x③：**"多余"只在视图窗口与数组元素边界对齐时成立**。
        //   视图表达的是"这段字节是同一个数组、索引差 k"——若 `stride*d` 不是 `elemSize` 的整数倍，
        //   它把字节重新**分组**了（错位的另一种读法），数组自己的元素边界并不能覆盖它 ⇒ 不是多余。
        //   原先只判"范围被数组罩住"就把错位视图判成多余（实测把必需视图当成噪声）。
        const aligned = (stride * d) % elemSize === 0;
        const coveredByZero = d > 0 && m.count != null && to <= m.offN + stride * m.count && aligned;
        declaredViews.push({ tid, label: v.label ?? null, off: m.offN, delta: d, from, to, redundant: coveredByZero });
      }
    }
    for (let i = 1; i < ms.length; i += 1) {
      const prev = ms[i - 1];
      if (prev.ext == null) continue;   // 跨度未知（条数未观察）⇒ 重叠判不了 —— 由下面那条 note 明说
      if (ms[i].offN < prev.offN + prev.ext) red(`G1 ${tid}`, `成员 ${hex(ms[i].offN)} 与 ${hex(prev.offN)}(+${prev.ext}) 重叠`);
    }
    if (t.size != null) for (const m of ms) if (m.ext != null && m.offN + m.ext > t.size) red(`G1 ${tid}`, `成员 ${hex(m.offN)}+${m.ext} 超出 size ${t.size}`);
  }
  // ── G17 **`axis` 标注**（`{stride, index, why}` = "这个成员的索引语义已判定"）：
  //   它不是"能从 EA 机械读出的事实"（那是 A 层禁的），而是**判断**：哪一个寄存器是游标、步长多少。
  //   工具侧的价值：见到它就不必去猜折叠链（`lea` 的索引项在 132 处语料里会丢，见 S2ag）。
  //   但它必须是**可证伪**的：`axis.stride` 得有一个**已声明的数组**给它背书（同一个容器类型里，
  //   存在数组成员且其元素大小 == stride）—— 否则这条标注只是"我说是就是"。
  for (const [tid, ty] of Object.entries(types)) {
    const arrays = membersOf(ty).map((mm) => ({ mm, es: ((mm.type && types[mm.type]?.size) ?? null) })).filter((x) => x.es != null && (x.mm.count == null || num(x.mm.count) > 1));
    for (const mm of membersOf(ty)) {
      const ax = mm.axis;
      if (ax == null) continue;
      const label = `${tid}+${hex(mm.offN)}`;
      if (typeof ax !== 'object' || Array.isArray(ax)) { red(`G17 ${label}`, 'axis 必须是对象 `{stride, index, why}`'); continue; }
      const st = num(ax.stride);
      if (!Number.isInteger(st) || st <= 0) red(`G17 ${label}`, `axis.stride 必须是正整数（收到 ${JSON.stringify(ax.stride)}）`);
      if (typeof ax.index !== 'string' || !ax.index) red(`G17 ${label}`, 'axis.index 必须写明索引身份（例如 "var"/"i"/"arg0"）');
      if (typeof ax.why !== 'string' || ax.why.length < 8) red(`G17 ${label}`, 'axis.why 必须写清判据（EA + 指令形态），少于 8 字符视为空话');
      if (Number.isInteger(st) && st > 0 && !arrays.some((x) => x.es === st)) {
        red(`G17 ${label}`, `axis.stride=${st} 在本类型的已声明数组里**没有元素大小等于它的**（候选：${arrays.map((x) => `${hex(x.mm.offN)}=${x.es}`).join(' ') || '无'}）⇒ 这条标注没有可证伪的支撑`);
      }
    }
  }
  if (spanUnknown.length) notes.push(`G8：元素 size 未知的成员 ⇒ 跨度算不出，宿主匹配按 **1 字节** 槽处理（静默退化的入口，⛔ 不是"通过"）：${spanUnknown.join(' · ')}`);
  if (countUnknown.length) notes.push(`G8：**条数未观察**的成员（\`count:null\` + \`sweep\`）⇒ 边界 / 与相邻成员的重叠判不了，但形态（步长 + 索引身份）是声明了的：${countUnknown.join(' · ')}`);

  /** 成员的**绝对地址**（元素 0）：obj0 是全局单例时位移即地址；经数组成员到达 ⇒ 数组基址 + 元素内偏移 */
  const ABS = new Map();
  if (M.obj0?.isGlobal) {
    const walk = (tid, base) => {
      for (const m of membersOfId(tid)) {
        const abs = base + m.offN;
        ABS.set(`${tid}:${m.offN}`, abs);
        if (m.type && m.count) walk(m.type, abs);
      }
    };
    for (const tid of Object.keys(types)) if ((M.carriers ?? []).some((c) => c.type.replace(/\*|\[\]$/, '') === tid)) walk(tid, 0);
  }

  // ── 逐载体
  const perCarrier = [];
  /** typeId → Set(offN)：本模型载体集里**观察到被当基址解引用**的宿主偏移（G7 用） */
  const derefHosts = new Map();
  /** 多窗口同分 ⇒ 选不出宿主（⛔ 不许靠"谁先声明"决定） */
  const ambiguous = [];
  /** 被 G5' 解释的二级/多跳访问（报告用：层级 + 路径） */
  const explainedPointee = [];
  /** 被**任何**站点命中过的窗口（含没被选中的候选）⇒ G9 判"视图零观察"用 */
  const candidateWindows = new Set();
  const cons = {
    sites: 0, obj0Acc: 0, obj0Explained: 0, obj0ExplainedObserved: 0, obj0ExplainedViaView: 0, obj0Residue: 0, partial: 0, partialTakeAddr: 0, partialIndexUnresolved: 0,
    pointee: 0, pointeeOk: 0, pointeeOpaque: 0, pointeeBad: 0, pointeeMulti: 0, pointeeAmbiguous: 0, deep: 0,
    symbol: 0, unresolvedBase: 0, unresolvedIndex: 0, widthUnknown: 0, local: 0, localOk: 0, localResidue: 0,
  };
  // ── G10 **局部类型载体**（"这个寄存器在这一段里装的是某个类型的对象"）
  //    ★ 为什么允许它（而不是只靠路径）：路径只能表达"从 `obj0` 的某个成员读出来的指针"；
  //      指针还可能来自**参数 / 调用返回值 / 数据段** —— 那些位置上路径没有任何东西可表达。
  //    ★ 代价：它是一句**断言**（断言寄存器是 `T*`），所以三条硬要求：
  //      ① span 必须是 `regions` 机械导出的**活窗口**（`[from..liveTo]`，⛔ 不许自己写）；
  //      ② 活窗口里**被「call」打断的 caller-saved 寄存器不许用**（值可能已被抹掉 ⇒ 断言不成立）；
  //      ③ 类别必须"值来自内存/取址"（`load` / `ptr` / `other`），⛔ 常量 / 初始化不是指针。
  const localCovered = new Set();
  const localReport = [];
  const localResidues = [];
  for (const c of (M.carriers ?? []).filter((x) => x.kind === 'local')) {
    const ltid = c.type.replace(/\*|\[\]$/, '');
    const lt = types[ltid];
    if (!lt) { red(`载体 ${c.id}`, `类型 ${c.type} 未定义`); continue; }
    const lsp = spans.find((s) => s.sym === c.fn);
    if (!lsp) { red(`载体 ${c.id}`, `找不到函数 ${c.fn}`); continue; }
    const ld = deriveRegions(lines, c.fn, c.reg);
    if (!ld) { red(`载体 ${c.id}`, `找不到 ${c.fn}·${c.reg} 的段落`); continue; }
    const a = num(c.span?.[0]); const b = num(c.span?.[1]);
    const hit = ld.regions.find((r) => r.from === a && r.liveTo === b);
    if (!hit) {
      red(`G10 ${c.id}`, `span [${hex(a)},${hex(b)}] 不等于 ${c.fn}·${c.reg} 任何段落的**活窗口** [from..liveTo]（⛔ 不许自己写 span；用「--regions」现跑）`);
      continue;
    }
    if (!['load', 'ptr', 'other'].includes(hit.cat)) {
      red(`G10 ${c.id}`, `该段落类别是 ${hit.cat} ⇒ 值不是"来自内存 / 取址"的东西，不能断言它是指向 ${ltid} 的指针`);
      continue;
    }
    if (hit.brokenByCall) {
      red(`G10 ${c.id}`, `${c.reg} 是 caller-saved，而活窗口 [${hex(a)}..${hex(b)}] 里被「call」打断 ⇒ 值可能已被抹掉，断言不成立`);
      continue;
    }
    const lconv = rootsMap.get(c.fn) ?? null;
    if (!lconv) { red(`载体 ${c.id}`, `${c.fn} 的 .c 约定里查不到对象根寄存器 ⇒ 无法检查`); continue; }
    const { sites: lsites } = analyzeLines(lines, lsp.from, lsp.to, lconv, {});
    const inSpan = lsites.filter((s) => s.baseReg === c.reg && s.ea >= a && s.ea <= b);
    const lwins = windowsOf(ltid);
    // ★ 守恒③ 的计数：若这个函数**没有**对象根载体，那它被检查的只有"活窗口内的站点" ⇒ 总数要自己加上；
    //   若有（对象根载体），那些站点已经算在 rawSites 里了，再加就重复。
    if (!(M.carriers ?? []).some((x) => x.kind !== 'local' && x.fn === c.fn)) cons.sites += inSpan.length;
    let okN = 0; let badN = 0;
    for (const s of inSpan) {
      localCovered.add(s.ea);
      const off = typeof s.baseOff === 'number' ? s.baseOff : null;
      const fake = { absDisp: off ?? -1, stride: s.stride, indexSym: s.indexSym };
      const cands = lwins.filter((w) => matchesWindowGlobal(w, fake));
      if (!cands.length || off === null) {
        badN += 1;
        if (lt.complete) red(`G10 ${c.id} @${hex(s.ea)}`, `访问 ${hex(off ?? 0)} 不落在 ${ltid} 的任何已声明成员上（该类型 complete）`);
        else localResidues.push({ ea: s.ea, abs: off ?? 0, why: `访问 ${hex(off ?? 0)} 不落在 ${ltid} 的已声明成员上（局部载体 ${c.id}）` });
        continue;
      }
      const w = cands[0];
      const leaf = w.kind === 'array'
        ? membersOf(types[w.m.type]).find((m) => (off - w.from) >= m.offN && (m.ext == null ? (off - w.from) < m.offN + 1 : (off - w.from) < m.offN + m.ext))
        : w.m;
      if (leaf && leaf.width != null && s.width !== 'unknown' && s.width > leaf.width) {   // ★ 只禁"更宽"，窄访问合法
        badN += 1;
        red(`G10 ${c.id} @${hex(s.ea)}`, `宽度 ${s.width} != ${ltid} 成员 ${hex(leaf.offN)} 声明 ${leaf.width}`);
        continue;
      }
      okN += 1;
    }
    cons.local += inSpan.length; cons.localOk += okN; cons.localResidue += badN;
    localReport.push({ id: c.id, fn: c.fn, reg: c.reg, type: ltid, from: a, to: b, cat: hit.cat, sites: inSpan.length, ok: okN, bad: badN });
    // ★ 只有"路径**本来就能判**"时才算两套声明：1 跳 **且** 那一跳在某个类型里已声明成 ptr→本类型。
    //   跨两级以上的路径不判 ⇒ 局部载体是**唯一**写法，报"建议只留一套"是错的建议（实测踩过）。
    const pathAlsoJudges = (path) => (path?.length ?? 0) === 1
      && Object.values(types).some((tt) => (tt.members ?? []).some((m) => num(m.off) === path[0] && m.kind === 'ptr' && m.type === ltid));
    if (inSpan.some((s) => pathAlsoJudges(s.pointeePath))) {
      notes.push(`G10 ${c.id}: 有站点**同时**能被局部载体和"从 obj0 读出的一跳路径"解释 ⇒ 同一事实有两套声明，建议只留一套（${c.fn}·${c.reg}）`);
    }
  }

  for (const c of M.carriers ?? []) {
    if (c.kind === 'local') continue;   // 局部载体在 G10 预扫里处理（它的槽是 (寄存器, 活窗口)，不是对象根）
    if (c.kind === 'global') continue;  // 全局载体在 G13 里处理（它没有 `fn`：声明的是"全局槽里装什么"，不是某个函数的 this）
    const tid = c.type.replace(/\*|\[\]$/, '');
    const t = types[tid];
    if (!t) { red(`载体 ${c.id}`, `类型 ${c.type} 未定义`); continue; }
    const sp = spans.find((s) => s.sym === c.fn);
    if (!sp) { red(`载体 ${c.id}`, `找不到函数 ${c.fn}`); continue; }
    const conv = rootsMap.get(c.fn) ?? null;
    if (!conv) { red(`载体 ${c.id}`, `${c.fn} 的 .c 约定里查不到对象根寄存器 ⇒ 这条载体**无法检查**（不许当绿）`); continue; }
    if (c.reg !== conv) { red(`载体 ${c.id}`, `声明的载体寄存器 ${c.reg} 与 .c 约定（${conv}）不符`); continue; }

    const { sites: rawSites } = analyzeLines(lines, sp.from, sp.to, conv, {});
    // ★ 被**局部载体**覆盖的站点单独记账（G10 的桶）⇒ 从这里剔出去，否则两边都算
    const sites = rawSites.filter((s) => !localCovered.has(s.ea));
    // ★ 位移可不可用，是**一个**判据（`offsetUnusable`）：`obj0` 只收"能当偏移观察用"的站点。
    //   两类被剔出去（原因分开报）：`lea` 取址中间量 · 索引未解析（位移少了索引那一项）。
    const obj0 = sites.filter((s) => s.obj === 'Engine' && s.absDisp !== null && offsetUnusable(s) === null);
    const unusableSites = sites.filter((s) => s.obj === 'Engine' && s.absDisp !== null && offsetUnusable(s) !== null);
    const partial = unusableSites.filter((s) => offsetUnusable(s) === 'take-address');
    const indexUnusable = unusableSites.filter((s) => offsetUnusable(s) === 'index-unresolved');
    // ── ★ 守恒③ 的分桶：**这四类原先从访问总数里消失**（`ds:` 符号 / 基址没解出来 /
    //      二级对象**内嵌派生**出来的基址 —— 后者是守恒③ 上线当天抓出来的：`lea eax,[<二级对象>+0x10]`
    //      之后 `[eax+k]`，`pointeeHost` 是 null（`addr` 基址拿不到宿主偏移）⇒ 它既不算 obj0、
    //      也不算二级对象、也不算 deep，从所有桶里掉出去。⇒ 单列成独立一桶，⛔ 不许静默。
    const symbolSites = sites.filter((s) => s.baseKind === 'absolute');
    const unresolvedSites = sites.filter((s) => s.problems.includes('base'));
    // ★ S2e：其中"基址是**已声明的全局符号**"的那部分单独计数（它们与 obj0 同一偏移空间，
    //   但要有 `kind:"global"` 的事实声明才敢换算；现在只**报数**，不静默吞掉）。
    // ★ S2e：把"基址是**已声明的全局符号**"的站点，按该全局载体声明的类型去对成员表。
    //   ⛔ 只对 `kind:"global"` 且给了 `type` 的符号这么做；没声明的一律不猜。
    const globTypeOf = new Map();
    for (const cc of M.carriers ?? []) {
      if (cc.kind !== 'global' || !cc.sym || !cc.type) continue;
      globTypeOf.set(cc.sym, String(cc.type).replace(/\*$/, ''));
    }
    const globBaseSites = unresolvedSites.filter((s) => s.globSym);
    if (globBaseSites.length) {
      let explained = 0; let residue = 0; let unknownSym = 0;
      const residues = [];
      for (const s of globBaseSites) {
        const gt = globTypeOf.get(s.globSym);
        if (!gt || !types[gt]) { unknownSym += 1; continue; }
        const mem = membersOfId(gt).find((mm) => mm.offN === s.absDisp && (mm.ext == null ? true : true));
        if (mem) explained += 1;
        else { residue += 1; residues.push(`${s.globSym}+${hex(s.absDisp ?? 0)}@${hex(s.ea)}`); }
      }
      const bySym = new Map();
      for (const s of globBaseSites) bySym.set(s.globSym, (bySym.get(s.globSym) ?? 0) + 1);
      notes.push(`★ 基址是**已声明全局符号**的站点 ${globBaseSites.length} 处（${[...bySym].map(([k, v]) => `${k}×${v}`).join(' ')}）⇒ 按该全局声明类型的成员表判：解释 ${explained} · 缺成员 ${residue} · 全局未声明 ${unknownSym}`);
      if (residues.length) notes.push(`  G5-glob 缺成员（该全局类型的成员表里没有这些偏移）：${residues.slice(0, 8).join(' ')}`);
    }
    const indexBadSites = sites.filter((s) => s.problems.includes('index'));
    const widthBadSites = sites.filter((s) => s.problems.includes('width'));
    const ms = membersOf(t);
    const wins = windowsOf(tid);
    const explained = [];
    const residue = [];
    /** 一个站点落在这个窗口里吗？★ 有 `sweep` 的（条数未观察）还要**索引身份 + 步长**都对上 */
    const matchesWindow = matchesWindowGlobal;
    /**
     * 多个窗口都能解释时怎么选（**写成规则，不靠顺序**）：
     * ① 带索引/步长的站点优先**数组窗口**，裸位移优先**内联成员**；② 更窄的窗口优先；③ 视图越"近"优先。
     * ★ 选不出来（三项全同）⇒ 记进 `ambiguous`，报告里单列 —— ⛔ 不许靠"谁先声明"决定。
     */
    const pickWindow = (cands, s) => {
      const score = (w) => [
        (s.indexSym || s.stride) ? (w.kind === 'array' ? 0 : 1) : (w.kind === 'array' ? 1 : 0),
        w.to - w.from,
        Math.abs(w.delta),
      ];
      const sorted = cands.map((w) => ({ w, k: score(w) })).sort((a, b) => a.k[0] - b.k[0] || a.k[1] - b.k[1] || a.k[2] - b.k[2]);
      const [first, second] = sorted;
      if (second && first.k[0] === second.k[0] && first.k[1] === second.k[1] && first.k[2] === second.k[2] && first.w.m.offN !== second.w.m.offN) {
        ambiguous.push({ ea: s.ea, abs: s.absDisp, between: [first.w.m.offN, second.w.m.offN] });
      }
      return first.w;
    };
    for (const s of obj0) {
      const cands = wins.filter((w) => matchesWindow(w, s));
      if (!cands.length) {
        // ★ **父访问落在内嵌子对象的跨度里**（MSVC 会把子对象的初始化/字段访问内联到父的构造器里）：
        //   这个字节区间按 G1 的**归属**是子对象的 —— 所以判据去子对象的成员表里找，而不是记残差。
        //   实测来源：`T_Command+0x152a0` 在 `T_Font`（0x14d30, size 0x3967c）跨度内；`0x68ea8`/`0x796e4`
        //   同理落在 `Stack<int>` / `Queue<int>` 里。⛔ 归错就 G1 重叠，记残差就把"已解释"说成"没解释"。
        const embHost = membersOfId(tid).find((m) => m.type && m.count === 1 && m.ext != null
          && s.absDisp >= m.offN && s.absDisp < m.offN + m.ext);
        if (embHost) {
          const rel = s.absDisp - embHost.offN;
          const eb = membersOfId(embHost.type).find((m) => rel >= m.offN && (m.ext == null ? rel < m.offN + 1 : rel < m.offN + m.ext));
          if (eb && (eb.width == null || s.access === 'a' || s.width === 'unknown' || s.width <= eb.width)) {
            explained.push({ ea: s.ea, abs: s.absDisp, member: eb, leaf: eb, delta: 0, viaView: false, viaEmbedded: embHost.type });
            continue;
          }
          residue.push({ ea: s.ea, abs: s.absDisp, kind: 'obj0', why: `落在内嵌子对象 ${embHost.type}+${hex(rel)} 里，但那里没有已声明成员` });
          continue;
        }
        if (t.complete) red(`G2 ${c.id} @${hex(s.ea)}`, `访问 ${hex(s.absDisp)} 不落在 ${c.type} 的任何已声明成员上（且该类型声明为 complete）`);
        else residue.push({ ea: s.ea, abs: s.absDisp, kind: 'obj0', why: `访问 ${hex(s.absDisp)} 不落在 ${c.type} 的任何已声明成员上` });
        continue;
      }
      const w = pickWindow(cands, s);
      for (const cw of cands) candidateWindows.add(`${tid}|${cw.m.offN}|${cw.delta}`);
      const host = w.m;
      let leaf = host;
      let viaEmbedded = host.count === 1 && host.type ? host.type : null;   // 内嵌子对象的字段（父载体上的站点）
      if (w.kind === 'array') {              // ★ 数组（含视图窗口）：还要落到**元素类型的成员**上
        const et = types[host.type];
        const elemOffRaw = s.absDisp - w.from;
        // ★ 未带索引 ⇒ 这个站点可能是"第 k 个元素的第 0 个字段"；只有取模才落在元素的成员上。
        //   （带索引时差值本身就是元素内偏移：索引已经记在 indexSym/stride 里。）
        const elemOff = (!s.indexSym && typeof et?.size === 'number' && et.size > 0)
          ? (elemOffRaw % et.size)
          : elemOffRaw;
        // ★ **元素 0 的偏移 0 = 这个数组/内嵌对象自己的起点**：`lea ecx,[esi+K]`（内嵌子对象的基址）
        //   与"表第 0 槽"都落在这里 ⇒ 由**成员本身**解释，不去找"元素内的成员"。
        //   实测：`T_Command` 的 20 个内嵌子对象都是 `count:1`，子对象类型多数还没有成员 ⇒
        //   不做这一条，每一条 `lea ecx,[esi+K]` 都会变成"元素内偏移 0x0 不是成员"的残差（纯噪声）。
        if (elemOff === 0) { leaf = host; }
        else {
          const em = membersOf(et).find((m) => elemOff >= m.offN && (m.ext == null ? elemOff < m.offN + 1 : elemOff < m.offN + m.ext));
          if (!em) {
            if (et?.complete) red(`G2 ${c.id} @${hex(s.ea)}`, `元素内偏移 ${hex(elemOff)} 不是 ${host.type} 的成员（${host.type} 声明为 complete）`);
            else residue.push({ ea: s.ea, abs: s.absDisp, kind: 'obj0', why: `元素内偏移 ${hex(elemOff)} 不是 ${host.type} 的成员` });
            continue;
          }
          leaf = em;
        }
        // ★ G3 比的是**最内层那个数组的元素大小**（放在 `elemOff === 0/≠0` 两个分支**之外**：
        //   踩过 —— 只写在"元素内偏移≠0"那支里，`[esi+edi*4+2000h]`（第 0 个元素 + 真数组）就永远不校验）。
        //   · 站点落在**内嵌子对象**跨度里（`count:1`）时，`stride` 与"子对象自己的 size"无关 ——
        //     实测 `0x4116ba mov eax,[esi+eax*4+5D04h]`：`0x5d04` 落在 `T_Command+0x55d8` 那块
        //     `T_CBunki(count:1)` 的跨度里，元素内偏移 `0x72c`，而 `T_CBunki+0x72c` 本身是
        //     `T_el32×100` ⇒ 步长 4 该跟 `T_el32.size` 比，不是跟 `T_CBunki.size`(29928) 比。
        //   · `leaf` 自己是数组时，用 `leaf.type` 的元素大小。
        {
          const innerArray = leaf && leaf !== host && leaf.count != null && leaf.type ? (types[leaf.type]?.size ?? null) : null;
          const hostIsArray = host.count == null || host.count > 1;
          const strideUnit = innerArray ?? (hostIsArray ? (et?.size ?? null) : null);
          if (s.indexSym && strideUnit != null && s.stride !== strideUnit) {
            red(`G3 ${c.id} @${hex(s.ea)}`, `数组形态步长 ${s.stride} != 元素大小 ${strideUnit}${leaf !== host ? `（${host.type}+${hex(leaf.offN)}）` : `（size(${host.type})）`}`);
            residue.push({ ea: s.ea, abs: s.absDisp, kind: 'obj0', why: '数组步长与元素大小不匹配（见 G3）' });
            continue;
          }
        }
      }
      if (leaf.width != null && s.access !== 'a' && s.width !== 'unknown' && s.width > leaf.width) {
        red(`G2 ${c.id} @${hex(s.ea)}`, `宽度 ${s.width} != 成员声明 ${leaf.width}`);
        residue.push({ ea: s.ea, abs: s.absDisp, kind: 'obj0', why: `宽度 ${s.width} 超过成员声明的 ${leaf.width}（见 G2）` });
        continue;
      }
      explained.push({ ea: s.ea, abs: s.absDisp, member: host, leaf, delta: w.delta, viaView: w.delta !== 0, viaEmbedded });
    }

    // ── G15 **索引访问落在已声明成员上 ⇒ 那个成员必须是数组**（note 级，⛔ 不算通过；S2r 计划翻红）
    //   ★ 为什么需要：mov eax,[esi+eax*4+0A509Ch]（op handler 分派读）在 shape access 里是 abs=0xa509c ⚠ index
    //     ⇒ offsetUnusable()='index-unresolved' ⇒ 进「位移不可用」桶，**不比对成员、不产生残差/红**。
    //     于是「1024 项分派表被写成 544 条离散内联槽」在所有义务下都沉默。本条把它变成逐条可见。
    //   ★ 不判红的原因：当前 100+ 处命中、多数是还没建完的类型表；先可见、后翻红（否则真问题被噪声埋）。
    // ── G15 **索引访问落在哪个成员上**：收成一个分类器（原先 5 个桶的判定散在循环体里、24 处引用）
    //   verdict：array（真数组，`count>1`/`count:null` 且元素大小==步长）/ countOne（`count:1` 容器）
    //            / strideMismatch（元素大小≠步长）/ scalar（标量 ⇒ 红）/ undeclared（该偏移还没声明）
    const classifyIndexSite = (s) => {
      if (s.obj !== 'Engine' || s.absDisp === null) return null;
      const cands = wins.filter((w) => matchesWindow(w, s));
      if (!cands.length) return { verdict: 'undeclared' };
      if (s.absDisp === 0) return null;              // 位移 0 + 未解析索引 ⇒ 基址多半不是 obj0，判不了
      const arr = cands.filter((w) => w.kind === 'array' && (w.m.count == null || num(w.m.count) > 1)
        // 元素大小只在**站点落在元素边界**上时才比（落在元素内部时步长属元素里的嵌套数组）
        && (w.elemSize == null || s.memScale == null || w.elemSize === s.memScale
          || ((s.absDisp - w.from) % w.elemSize !== 0)));
      if (arr.length) return { verdict: 'array' };
      const c1 = cands.find((w) => w.kind === 'array');
      if (c1 && num(c1.m.count) === 1) return { verdict: 'countOne', off: c1.m.offN };
      if (c1) return { verdict: 'strideMismatch', off: c1.m.offN, elemSize: c1.elemSize, stride: s.memScale };
      return { verdict: 'scalar' };
    };
    let indexOverArray = 0;
    const indexOverScalarEAs = [];
    const indexOverCountOne = [];
    const indexOnUndeclared = [];
    const indexStrideMismatch = [];
    for (const s of indexBadSites) {
      const r = classifyIndexSite(s);
      if (!r) continue;
      if (r.verdict === 'array') indexOverArray += 1;
      else if (r.verdict === 'undeclared') indexOnUndeclared.push({ ea: s.ea, abs: s.absDisp });
      else if (r.verdict === 'countOne') indexOverCountOne.push({ ea: s.ea, off: r.off });
      else if (r.verdict === 'strideMismatch') indexStrideMismatch.push({ ea: s.ea, off: r.off, elemSize: r.elemSize, stride: r.stride });
      else indexOverScalarEAs.push({ ea: s.ea, why: '落在标量成员上' });
    }
    if (indexOverArray) notes.push(`G15 ${c.id}: ${indexOverArray} 处索引访问落在**数组**成员上（索引本身不可判，但形态已被数组承住；⛔ 不算通过）`);
    // ★ **翻红**：索引访问落在非数组（标量 / `count:1` 容器）成员上 ⇒ 红。
    //   判据能红是因为两道假绿通道已经堵掉（`count:1` 不算数组、元素大小必须等于观测步长）。
    if (indexOverScalarEAs.length) {
      for (const x of indexOverScalarEAs.slice(0, 20)) red(`G15 ${c.id} @${hex(x.ea)}`, `索引访问落在非数组成员上（${x.why}）⇒ 该成员应声明成数组（count 或 count:null+sweep，元素大小 == 观测步长）`);
      if (indexOverScalarEAs.length > 20) notes.push(`G15 ${c.id}: 另有 ${indexOverScalarEAs.length - 20} 处同类（只报了前 20 条 EA）`);
    }
    // ★ `count:1` 的容器**不是数组**（实测它能同样消掉 108 处索引点名 ⇒ 那是假绿通道）——
    //   但"索引落在子对象跨度里"本身**判不了**（索引可能属于子对象内部），所以记 note 并给数，⛔ 不算通过。
    if (indexOverCountOne.length) {
    if (indexStrideMismatch.length) {
      const rows = indexStrideMismatch.slice(0, 4).map((x) => `${hex(x.off)}(元素 ${x.elemSize} vs 步长 ${x.stride})`).join(' ');
      notes.push(`G15 ${c.id}: ⚠ ${indexStrideMismatch.length} 处索引访问的元素大小与观测步长不符（${rows}…）—— 要么元素类型写错、要么步长属于元素里的**嵌套数组**（后者要带元素内偏移才判得出，暂记 note）`);
    }
    if (indexOnUndeclared.length) {
      const u = [...new Set(indexOnUndeclared.map((x) => hex(x.abs)))].slice(0, 6).join(' ');
      notes.push(`G15 ${c.id}: ${indexOnUndeclared.length} 处索引访问落在**未声明**的偏移上（${u}…）—— 那是"这张表还没建"，不是"建了但类型不对"（⛔ 不算通过）；EA：${indexOnUndeclared.slice(0, 4).map((x) => hex(x.ea)).join(' ')}`);
    }
      const offs = [...new Set(indexOverCountOne.map((x) => hex(x.off)))].join(' ');
      notes.push(`G15 ${c.id}: ⚠ ${indexOverCountOne.length} 处索引访问落在 count:1 的**容器**（${offs}）跨度里 —— 容器不是数组，索引属于子对象内部还是外层**判不了**（⛔ 不算通过）；EA：${indexOverCountOne.slice(0, 6).map((x) => hex(x.ea)).join(' ')}`);
    }
    // ── G5/G5'：**一跳**的二级对象三档（解释 / 不透明 / 拒绝）
    //   ★ 为什么不再遍历多跳（结构上的简化，2026-10）：深度不是"校验"该管的东西。
    //     一跳能**推导**（1 条声明覆盖 N 处：实测 10 条 → 125 处），两跳以上请用**局部类型载体**断言
    //     （实测 2 跳只有 8 处站点；纯靠局部载体要 75 条声明 ⇒ 只在"更深"时付这个代价）。
    //   ⇒ 于是这里没有链遍历、没有下钻政策开关：**跨两级以上一律单列**（带 EA + 路径），不判。
    //   ★ 踩过（修好之前）：三跳链只带"最后一跳的偏移" ⇒ 宿主被拿去对 obj0 的成员表 = 认错层（会假解释）。
    // ★ 只取址（lea）的站点**不进 G5'**：它算地址、不解引用（真解引用另有非 lea 站点）。
    //   实测踩过：整数游标（T_a609c+0x2c24）被当指针，4 条 G5' 假红，逼人把它声明成 ptr（假绿）。
    const leaPointee = sites.filter((s) => s.access === 'a' && (s.pointeePath?.length ?? 0) >= 1);
    if (leaPointee.length) {
      notes.push(`G5' ${c.id}: ${leaPointee.length} 处**只取址**的二级站点（值被用于地址算术，未解引用）⇒ 不要求宿主声明 ptr`
        + `（${leaPointee.slice(0, 3).map((s) => hex(s.ea)).join(' ')}）`);
    }
    // ★ 带路径但 `pointeeOff === null` 的站点（access 层判为"折叠错"）**也要进这一桶**：否则它们会从守恒③ 的等式里静默消失（实测差 8）。加上 null 的站点进 pointee 桶，循环里走"判不了"档。
    const pointee = sites.filter((s) => (s.pointeePath?.length ?? 0) >= 1);
    let pointeeOk = 0; let pointeeOpaque = 0; let pointeeBad = 0; let pointeeMulti = 0; let pointeeAmbiguous = 0;
    const opaqueEAs = [];
    const ambiguousEAs = [];
    const multiHopSites = [];
    for (const s of pointee) {
      if (typeof s.pointeeOff !== 'number') { pointeeBad += 1; notes.push(`G5' ${c.id} @${hex(s.ea)}: 带二级路径但没有可用的二级偏移（负位移 ⇒ 多半是 \`new[]\` 的个数 cookie / 分配块头，不是宿主成员）⇒ **判不了**`); continue; }
      const path = s.pointeePath;
      // ★ **只取址（lea）** 的二级站点：值被用于地址算术，不是解引用 ⇒ 记**解释**档
      //   （真正的解引用一定另有非 lea 站点；实测踩过：整数游标被当指针时 4 条 G5' 假红）
      if (s.access === 'a') { pointeeOk += 1; continue; }
      // ★ **基址 / 索引分不清**：`[ecx+eax]` 两个寄存器都是"从内存读出来的值"（实测 `ecx=[esi+0x204c]`、
      //   `eax=[esi+0x2030]`）⇒ 谁是对象基址、谁是索引，**判不了**。
      //   ⛔ 不许因此把 `0x204c` 声明成 `kind:"ptr"`（反事实实测：那样 12 条红全消 = 假绿）。
      if (s.problems?.includes('index') || (s.indexSym && !/^[A-Za-z_]\w*$/.test(String(s.indexSym)))) { pointeeAmbiguous += 1; ambiguousEAs.push(s.ea); continue; }
      if (path.length + 1 > 2) { pointeeMulti += 1; multiHopSites.push(s); continue; }   // 跨两级以上：不判
      const hop = path[0];
      // ★ 负的二级偏移（`[base-K]`，MSVC `new[]` 个数 cookie）在 `shape-access` 层已折成 `null`，
      //   由下面「没有可用二级偏移」那一档统一处置 —— 这里不再留第二套措辞（死分支已删）。
      if (!derefHosts.has(tid)) derefHosts.set(tid, new Set());
      derefHosts.get(tid).add(hop);
      // ★ 宿主查找分两步（**数组元素内部**的宿主是一等情形，不是 fallback）：
      //   ① 精确 `off` 相等 —— 宿主是这条载体类型自己的成员；
      //   ② 否则看它是否落在**数组成员的元素内部**（实测 `T_Command+0x5d908` = `T1[40]` 元素内 `+0x74`，
      //      `T_Scene+0xa670` = `T2[1000]` 元素内 `+0x98`）⇒ 宿主是**元素类型里的那个成员**。
      //   ⛔ 两条都不中 ⇒ 残差（"未声明为成员"），**不许**退回"把数组成员自己当宿主"：
      //      试过那版，数组第 0 个元素的 `+0x0` 会被当宿主 ⇒ 17 条假红（`宿主 0x5d894 被当指针用…没有 kind:"ptr"`）。
      let host = null;
      let hostElemType = null;      // 宿主来自数组元素 / 内嵌子对象时，记下容器类型（vtable 判据与措辞要用）
      {
        // ★ **宿主可能藏在容器里**（数组元素内 / 内嵌子对象内，甚至容器套容器）。统一成一条递归解析：
        //   在 `tid` 的成员里找覆盖 `hop` 的**容器成员**（`type` + 元素 size）⇒ 换算成容器内偏移 `rel`，
        //   到元素/子对象类型里再找一层；命中标量就返回它。
        //   实测：`T_Command+0x4f11c` = `T_Scene+0x40c`、`T_Command+0x59380` = `T_Scene+0xa670`
        //   （后者还要再穿过 `T_Scene+0xa5d8 = T2[1000]` 才落到 `T2+0x0`）。
        // 模型的实际容器嵌套深度（obj0 起、按 `{type}` 递归；带环保护）
    const MAX_CONTAINER_DEPTH = (() => {
      let max = 0;
      const seen = new Set();
      const walk = (id0, d) => {
        if (d > max) max = d;
        const key = `${id0}:${d}`;
        if (d > 12 || seen.has(key)) return;
        seen.add(key);
        for (const mm of membersOfId(id0)) if (mm.type) walk(mm.type, d + 1);
      };
      walk(tid, 0);
      return max;
    })();
    const resolveDeep = (h, typeId, depth = 0) => {
          // ★ 深度上限不再写死 3：由**模型的实际嵌套深度**算出（obj0 起按容器成员递归取最大值）。
          //   写死 3 是拍出来的魔法数 —— 模型嵌套到 4 层时它会把第 4 层静默当成"判不了"，
          //   而模型明明已经把那条路径声明出来了。
          if (depth > MAX_CONTAINER_DEPTH) return null;
          for (const m of membersOfId(typeId)) {
            const et = m.type ? types[m.type] : null;
            if (m.kind === 'ptr') continue;      // ⛔ 指针成员不是容器：宿主就是它自己（下面走精确匹配）
            if (!et?.size || m.count === 0) continue;
            const count = m.count != null ? num(m.count) : 1;      // count:null（sweep）按单元素窗口判
            const span = et.size * count;
            if (h < m.offN || h >= m.offN + span) continue;
            const rel = (h - m.offN) % et.size;
            const inner = membersOf(et).find((x) => rel >= x.offN && (x.ext == null ? rel < x.offN + 1 : rel < x.offN + x.ext));
            if (!inner) return { host: null, hostType: m.type, rel, containerOff: m.offN };
            const innerIsContainer = inner.type != null && types[inner.type]?.size != null && inner.kind !== 'ptr';
            if (innerIsContainer && depth < 3) {
              const deeper = resolveDeep(rel + 0, m.type, depth + 1);
              // `rel` 已在元素内 ⇒ 用"元素内偏移 - 容器成员起点 + 容器成员起点"换算成容器类型内的偏移
              const deeper2 = deeper ?? resolveDeep(inner.offN + (rel - inner.offN), m.type, depth + 1);
              if (deeper2?.host) return deeper2;
            }
            return { host: inner, hostType: m.type, rel, containerOff: m.offN };
          }
          return null;
        };
        const deep = resolveDeep(hop, tid, 0);
        if (deep && deep.host) { host = deep.host; hostElemType = deep.hostType; }
        else if (deep && !deep.host) {
          pointeeBad += 1;
          residue.push({ ea: s.ea, abs: hop, kind: 'pointee', why: `二级对象宿主落在 ${deep.hostType} 的容器（+${hex(deep.containerOff)}）内偏移 ${hex(deep.rel)}，但那里没有已声明成员` });
          continue;
        }
      }
      if (!host) host = ms.find((m) => m.offN === hop) ?? null;
      // ★ **数组元素的第一双字 = 元素类型的 vtable**：与"内嵌子对象的第一双字"同一条判据
      //   （`mov eax,[…]` → `mov ecx,[eax]` → `call [ecx+槽]` 是**虚调用**）。
      //   实测来源：`T1[40]` 的元素 `+0x0`、`T2[1000]` 的元素 `+0x0`、`T_Scene+0xa5d8`（= `T2[0]` 的 `+0x0`）。
      // ★ G7 的 `derefHosts` 原先只按**载体类型 + 绝对偏移**建键 ⇒ 元素类型（`*_elem`）里的指针槽永远算
      //   "零解引用" = 空话声明（假阴）。宿主来自元素/子对象时，按**元素类型 + 元素内偏移**也记一笔。
      if (host && hostElemType) {
        if (!derefHosts.has(hostElemType)) derefHosts.set(hostElemType, new Set());
        derefHosts.get(hostElemType).add(host.offN);
      }
      if (host && hostElemType && host.offN === 0 && host.kind !== 'ptr') {
        pointeeOpaque += 1;
        opaqueEAs.push(s.ea);
        notes.push(`G5' ${c.id} @${hex(s.ea)}: 宿主 ${hex(hop)} 落在数组元素（${hostElemType}）的 **+0x0** ⇒ 解引用的是元素的 **vtable**（虚调用），不是"成员是指针"（不可判，⛔ 不算通过）`);
        continue;
      }
      // ★ 元素内**非 0 偏移**的标量被当基址用：记**残差**（不是红）——
      //   这是"元素类型的成员表还缺一条 kind:'ptr'"的缺口，不是这条载体声明错；
      //   判红的条件仍是"宿主是本类型自己的成员且没声明 ptr"（下面那条）。
      if (host && hostElemType && host.kind !== 'ptr') {
        pointeeBad += 1;
        residue.push({ ea: s.ea, abs: hop, kind: 'pointee', why: `${hostElemType}+${hex(host.offN)} 的值被当基址用，但元素类型里那条成员没声明 kind:"ptr"` });
        continue;
      }
      if (!host) {
        pointeeBad += 1;
        if (t.complete) red(`G5 ${c.id} @${hex(s.ea)}`, `二级对象的宿主 ${hex(hop)} 不是 ${c.type} 的成员`);
        else residue.push({ ea: s.ea, abs: hop, kind: 'pointee', why: `二级对象宿主 ${hex(hop)} 未声明为成员` });
        continue;
      }
      // ★ **内嵌子对象的第一双字 = 它的 vtable**：`lea ecx,[esi+K]` → `mov eax,[ecx]` → `mov eax,[eax+槽]` → `call eax`
      //   是**虚调用**（实测 `sub_412290` 的 `0x412c77`/`0x412c79`：`[this+0x1ee8]` 是内嵌 `Sys3Draw` 的 vtable）。
      //   ⇒ 宿主是**内嵌成员**（`type` + `count:1` + 非 ptr）时，解引用的是**它的 vtable**，不是"这个成员是对象指针"。
      //   ⛔ 不判红，也**不算解释**：进 `pointeeOpaque`（不可判）+ note。
      if (host.kind !== 'ptr' && host.type && host.count === 1) {
        pointeeOpaque += 1;
        opaqueEAs.push(s.ea);
        notes.push(`G5' ${c.id} @${hex(s.ea)}: 宿主 ${hex(host.offN)} 是**内嵌子对象**（${host.type}）⇒ 解引用的是它的 **vtable**（虚调用），不是"成员是指针"（不可判，⛔ 不算通过）`);
        continue;
      }
      if (host.kind !== 'ptr') { pointeeBad += 1; red(`G5' ${c.id} @${hex(s.ea)}`, `宿主 ${hex(host.offN)} 被当指针用（读它的值再以之为基址），但成员声明里没有 kind:"ptr"`); continue; }
      if (!host.type) { pointeeOpaque += 1; opaqueEAs.push(s.ea); continue; }   // 不透明：明确记账，不算通过
      const tu = types[host.type];
      if (!tu) { pointeeBad += 1; red(`G5' ${c.id} @${hex(s.ea)}`, `宿主声明的目标类型 ${host.type} 未定义`); continue; }
      const inner = membersOf(tu).find((m) => s.pointeeOff >= m.offN && (m.ext == null ? s.pointeeOff < m.offN + 1 : s.pointeeOff < m.offN + m.ext));
      if (!inner) {
        pointeeBad += 1;
        if (tu.complete) red(`G5' ${c.id} @${hex(s.ea)}`, `二级偏移 ${hex(s.pointeeOff)} 不是 ${host.type} 的成员（该类型 complete）`);
        else residue.push({ ea: s.ea, abs: s.pointeeOff, kind: 'pointee', why: `二级偏移 ${hex(s.pointeeOff)} 不是 ${host.type} 的成员` });
        continue;
      }
      if (inner.width != null && s.width !== 'unknown' && s.width > inner.width) { pointeeBad += 1; red(`G5' ${c.id} @${hex(s.ea)}`, `二级宽度 ${s.width} != ${host.type} 成员 ${hex(inner.offN)} 声明 ${inner.width}`); continue; }   // ★ 只禁更宽
      if (tu.size != null && s.pointeeOff + (s.width === 'unknown' ? 1 : s.width) > tu.size) { pointeeBad += 1; red(`G5' ${c.id} @${hex(s.ea)}`, `二级偏移 ${hex(s.pointeeOff)}+${s.width} 超出 ${host.type}.size ${tu.size}`); continue; }
      pointeeOk += 1;
      explainedPointee.push({ ea: s.ea, level: 2, path, off: s.pointeeOff, type: host.type, member: inner.offN });
    }

    // ★ 超出解析深度：**层级 > 政策上限**（默认 2）—— 单列 + 打路径 + 打 EA，⛔ 不许静默丢

    cons.sites += rawSites.length;
    cons.obj0Acc += obj0.length;
    cons.obj0Explained += explained.length;
    cons.obj0ExplainedObserved += explained.filter((e) => !e.viaView).length;
    cons.obj0ExplainedViaView += explained.filter((e) => e.viaView).length;
    cons.obj0Residue += residue.filter((r) => r.kind !== 'pointee').length;
    cons.partial += unusableSites.length;
    cons.partialTakeAddr += partial.length;
    cons.partialIndexUnresolved += indexUnusable.length;
    cons.pointee += pointeeOk + pointeeBad + pointeeOpaque + pointeeMulti + pointeeAmbiguous;
    cons.pointeeOk += pointeeOk;
    cons.pointeeOpaque += pointeeOpaque;
    cons.pointeeBad += pointeeBad;
    cons.pointeeMulti += pointeeMulti;
    cons.pointeeAmbiguous += pointeeAmbiguous;
    cons.deep += multiHopSites.length;
    cons.symbol += symbolSites.length;
    cons.unresolvedBase += unresolvedSites.length;
    cons.unresolvedIndex += indexBadSites.length;
  cons.indexOverScalar = (cons.indexOverScalar ?? 0) + indexOverScalarEAs.length;
    cons.widthUnknown += widthBadSites.length;
    // 未解析按**基址寄存器**分组（§7：报数要报到能处置的粒度，⛔ 不许只给个数字）
    const byReg = new Map();
    for (const s of unresolvedSites) byReg.set(s.baseReg ?? '(无基址寄存器)', (byReg.get(s.baseReg ?? '(无基址寄存器)') ?? 0) + 1);
    perCarrier.push({
      c, explained, residue, total: obj0.length,
      viaView: explained.filter((e) => e.viaView).length,
      viaViewEAs: explained.filter((e) => e.viaView).map((e) => ({ ea: e.ea, abs: e.abs, member: e.member.offN, delta: e.delta })),
      pointee: pointeeOk + pointeeBad + pointeeOpaque + pointeeMulti + pointeeAmbiguous, pointeeOk, pointeeOpaque, pointeeMulti, pointeeAmbiguous,
      multiHopEAs: multiHopSites.map((s) => s.ea),
      multiHopPaths: multiHopSites.map((s) => (s.pointeePath ?? []).map((h) => "+" + hex(h)).join(" -> ")),
      deep: pointeeMulti, deepEAs: multiHopSites.map((s) => s.ea), opaqueEAs, ambiguousEAs,
      partial: partial.length, partialEAs: partial.map((s) => s.ea),
      indexUnusable: indexUnusable.length, indexUnusableEAs: indexUnusable.map((s) => s.ea),
      sites: rawSites.length,
      symbol: symbolSites.length, symbolSyms: [...new Set(symbolSites.map((s) => s.obj))], symbolEAs: symbolSites.map((s) => s.ea),
      unresolvedBase: unresolvedSites.length, unresolvedByReg: [...byReg].sort((a, b) => b[1] - a[1]), unresolvedEAs: unresolvedSites.map((s) => s.ea),
      unresolvedIndex: indexBadSites.length, unresolvedIndexEAs: indexBadSites.map((s) => s.ea),
      widthUnknown: widthBadSites.length,
      // ★ `lea` 派生自**二级对象**的基址：形状层当前表达不了（拿不到宿主偏移）⇒ 单列一桶
      explainedPointeeEAs: explainedPointee.map((e) => e.ea),
    });
  }

  // ── G7 `kind:"ptr"` 的**可证伪性**：声明了指针、却在**本模型的载体集里**从未被当基址用过
  //   ★ 为什么**不红**：类型是共享的（少数定义、多数载体）—— 某个族文件里没看到解引用，
  //     只是说"这个族不这么用它"，别的族文件里可能正是唯一用它的人。
  //   ★ 为什么**必须单列**：这就是 skill 四档话里的 `unfalsifiable` —— "没有任何用法约束它" ⇒
  //     **不算通过**。实测：把 `{off:0x5d880,width:4}` 改成 `{…,kind:"ptr",type:"TB"}` 而这两个函数
  //     从不拿它当基址，旧版判定器**一片绿、连一句提示都没有**（空话声明静默通过）。
  const reachable = new Set();
  const walkReach = (tid) => {
    if (!tid || reachable.has(tid)) return;
    reachable.add(tid);
    for (const m of membersOfId(tid)) if (m.type) walkReach(m.type);
  };
  for (const c of M.carriers ?? []) walkReach(c.type.replace(/\*|\[\]$/, ''));
  const unfalsifiable = [];
  for (const tid of reachable) {
    const seen = derefHosts.get(tid) ?? new Set();
    for (const m of membersOfId(tid)) {
      if (m.kind !== 'ptr') continue;
      if (seen.has(m.offN)) continue;
      unfalsifiable.push({
        type: tid, off: m.offN, target: m.type ?? null,
        why: `声明为 ptr（目标 ${m.type ?? '未声明'}），但本模型的载体集里没有任何一处以 ${hex(m.offN)} 为基址的解引用 ⇒ 这条声明没有任何用法约束它`,
      });
    }
  }
  if (unfalsifiable.length) {
    notes.push(`G7 空话声明（\`kind:"ptr"\` 在本模型载体集里零解引用）${unfalsifiable.length} 条 —— ⛔ **不算通过**（类型共享，别的族文件可能才用它）：${unfalsifiable.map((u) => `${u.type}+${hex(u.off)}`).join(' ')}`);
  }

  // ── G9 **视图（模型的推测）的可证伪性**：声明了"这段字节是同一个数组、索引差 k"，却零观察 ⇒ 空话声明
  //   ★ 与 G7 同理：**不判红**（类型是共享的、别的族文件可能才用它），但**必须单列**（⛔ 不算通过）。
  for (const v of declaredViews) {
    const key = `${v.tid}|${v.off}|${v.delta}`;
    if (v.redundant) {
      unfalsifiable.push({ type: v.tid, off: v.off, delta: v.delta, why: '声明的视图**多余**：它的字节范围已被 delta 0 覆盖 ⇒ 不提供新信息' });
    } else if (!candidateWindows.has(key)) {
      unfalsifiable.push({ type: v.tid, off: v.off, delta: v.delta, why: `声明的视图（索引差 ${v.delta}）在本模型载体集里**零观察** ⇒ 这条推测没有被任何站点约束` });
    }
  }

  // ── G4 数组边界（末尾地址必须在语料里被引用）
  for (const [tid, t] of Object.entries(types)) {
    for (const m of membersOf(t)) {
      if (m.type == null || m.count == null) {
        if (m.type != null && m.count === null) notes.push(`G4 ${tid}+${hex(m.offN)}: **条数未观察**（sweep 形态已声明）⇒ 边界判不了 —— 这是**明说**，不是"通过"`);
        continue;
      }
      const et = types[m.type];
      if (et?.size == null) { notes.push(`G4 ${tid}+${hex(m.offN)}: 元素 size 未知 ⇒ 边界判不了`); continue; }
      const end = m.offN + et.size * m.count;
      // ★ `count === 1` = **内嵌子对象**（不是数组）：它的跨度由**元素类型的 size** 定，而"一过尾地址"(end)
      //   按定义**不会被编译器算出来**（其后要么是下一个成员、要么是填充）⇒ 用"末尾有没有被引用"来验它是错的判据。
      //   实测 `T_Font+0x18dc8`：end = 0x18dd8 全语料 **0** 处，而隔壁 0x18ddc 有 29 处。
      //   ⇒ 记 note（边界由元素类型定，不是由父级观测）；⛔ 不是"末尾有引用"那一档。
      if (m.count === 1) {
        notes.push(`G4 ${tid}+${hex(m.offN)}: count=1 = **内嵌子对象**（${m.type}，size ${hex(et.size)}）⇒ 跨度由元素类型定；一过尾地址 ${hex(end)} 按定义不会被算出，零引用是必然（⛔ 不是"末尾有引用"那一档）`);
        continue;
      }
      // ★ **标准库布局**（类型上带 `std` 标记）：`std::string` 这类 size 由 ABI 钉死（x86 MSVC = 0x18：
      //   16 字节 SSO 缓冲 + 4 长度 + 4 容量），它不是"观测出来的"，也不该靠"末尾被引用"来证明 ——
      //   它末尾之后是别的成员或填充，零引用是必然 ⇒ 记 note。
      //   ⛔ 只对**ABI 定死**的布局开这一档：`std::vector` 不行（3 指针 + 分配器是否 EBO ⇒ 12 或 16，仍要观测）。
      if (et && et.std != null) {
        notes.push(`G4 ${tid}+${hex(m.offN)}: 元素类型 ${m.type} 是**标准库布局**（${et.std}，size ${hex(et.size)} 由 ABI 定死、不是观测）⇒ 末尾 ${hex(end)} 零引用是必然（⛔ 不是"末尾有引用"那一档）`);
        continue;
      }
      // ★ S2ad：`end` 是**类型内相对偏移**，而语料里的引用是**绝对地址** —— 内嵌子对象上必须换算，
      //   否则会出现 `G4 T_inihdr+0x0: 末尾 0x4 零引用` 这种**假红**（实测绝对末尾 0xAAA1C 被引用 2 次、
      //   0xAAA20 被引用 19 次）。obj0 是全局单例时 `ABS` 有值 ⇒ 用它；否则退回相对偏移（note 说明是下界）。
      const absBase = ABS.get(`${tid}:${m.offN}`);
      const endAbs = absBase != null ? absBase + et.size * m.count : null;
      const refsOfEnd = endAbs != null ? refs(endAbs) : refs(end);
      if (endAbs == null && refs(end) === 0) {
        notes.push(`G4 ${tid}+${hex(m.offN)}: 末尾按**相对偏移** ${hex(end)} 查引用（该成员的绝对基址不可得 ⇒ 这是下界判断，⛔ 不是"末尾没被引用"）`);
      }
      if (refsOfEnd === 0) {
        // ★ 第三档：**数组正好填满对象**（末尾 == 容器 size）。这时"末地址零引用"是**必然** ——
        //   它已经在对象之外，编译器不会把它的绝对地址算出来。边界由 `size` 定界，而 `size` 是另一条
        //   独立声明（G1 又禁止任何成员越过 size）⇒ 这条约束**照样可证伪**：
        //   把 count 改大/改小 ⇒ 末尾不再等于 size ⇒ 立刻回落到红。
        //   ★ 为什么不是"放行"：只有**紧贴 size** 才成立（`end === size` 是等式，不是不等式）。
        if (t.size != null && end === t.size) {
          notes.push(`G4 ${tid}+${hex(m.offN)}: 末尾 ${hex(end)} == 容器 size ⇒ 数组正好填满对象（末地址在对象之外，零引用是必然；边界由 size 定界，⛔ 不是"末尾有引用"那一档）`);
          continue;
        }
        // ★ 奇数跨度（u16×17=0x22 / u16×289=0x242）的末尾按定义不是 4 对齐 ⇒ 编译器不会算出它 ⇒ 零引用是必然，不是"条数无依据"
        if (end % 4 !== 0) notes.push(`G4 ${tid}+${hex(m.offN)}: 末尾 ${hex(end)} 不对齐 4 且零引用 ⇒ 编译器按定义不会算出它（边界靠常量+写循环证明，⛔ 不是"通过"）`);
        else red(`G4 ${tid}+${hex(m.offN)}`, `数组末尾 ${hex(end)} 在语料里零引用 ⇒ count=${m.count} 无依据`);
      }
      else notes.push(`G4 ${tid}+${hex(m.offN)}: 末尾 ${hex(endAbs ?? end)}${endAbs != null ? '（绝对）' : '（相对下界）'} 有 ${refsOfEnd} 处引用 ⇒ count=${m.count} 站得住`);
    }
  }

  // ── G12 争议台账（**拒绝声明也要留下可检查的争议**）
  {
    const KINDS = new Set(['ptr-unknown', 'size-unknown', 'ownership-unknown']);
    for (const [i, d] of (M.disputes ?? []).entries()) {
      const where = `G12 disputes[${i}]`;
      const t2 = types[d.container];
      if (!t2) { red(where, `容器 ${d.container} 未声明`); continue; }
      const m = membersOf(t2).find((x) => x.offN === num(d.off));
      if (!m) { red(where, `${d.container}+${d.off} 不是已声明成员 ⇒ 争议没有落点（先声明宽度，再记争议）`); continue; }
      if (!Array.isArray(d.evidence) || d.evidence.length === 0) { red(where, `缺 witness EA（${d.container}+${d.off}）`); continue; }
      if (!KINDS.has(d.kind)) { red(where, `kind ${d.kind} 不在闭集合 [${[...KINDS].join(' / ')}]`); continue; }
      // ④ 不许过期
      if (d.kind === 'ptr-unknown' && m.kind === 'ptr' && m.type) { red(where, `${d.container}+${d.off} 已声明成 ptr→${m.type} ⇒ 争议已解决，删掉这条`); continue; }
      if (d.kind === 'size-unknown' && m.count != null) { red(where, `${d.container}+${d.off} 已声明 count=${m.count} ⇒ 争议已解决，删掉这条`); continue; }
      disputes.push({ container: d.container, off: d.off, kind: d.kind, why: d.why ?? '', evidence: d.evidence });
    }
  }

  // ── G13 全局载体（"这个数据段符号里装的是某个类型的指针"）
  //   ★ 为什么需要：**根对象不是参数** —— Engine（IDA 类名 `Command`）是 `_WinMain@16` 里
  //     `operator new(0xAABC8)`（`0x4baa11`）→ `call sub_415640` 构造（`0x4baa2e`）→
  //     `mov ds:dword_55E1BC, eax`（`0x4baa3a`）存进全局的。没有这一类载体，
  //     "从根正向传播"一步都走不了（只能继续从 op handler 倒着爬）。
  //   三条硬要求（缺一条这条声明就是**编出来的**）：
  //     ① 符号必须在语料里出现过（要能读到它的指令行）；
  //     ② `witness` 必须**至少有一条 EA 是写这个符号的 store**（`mov ds:<sym>, …`）——
  //        这是"它里面确实是这个指针"的唯一机械证据；对不上的 EA 单独报（stale，⛔ 不许掺进来凑数）；
  //     ③ `type` 必须已声明，且同一个符号不许两种类型。
  const globalCarriers = [];
  {
    const gcs = (M.carriers ?? []).filter((c) => c.kind === 'global');
    if (gcs.length) {
      let eaText = null;   // ★ 惰性建：只有真的声明了全局载体才扫这一遍全语料
      const lineOf = (ea) => {
        if (!eaText) {
          eaText = new Map();
          for (const ln of lines) {
            const m = /^[A-Za-z_.][\w.]*:([0-9A-F]{8})\s+(.*)$/.exec(ln);
            if (m) eaText.set(Number.parseInt(m[1], 16), m[2]);
          }
        }
        return eaText.get(ea) ?? null;
      };
      const seen = new Map();
      for (const c of gcs) {
        const where = `G13 ${c.id}`;
        if (!c.sym || typeof c.sym !== 'string') { red(where, '缺 `sym`（全局符号名）'); continue; }
        const tid = String(c.type ?? '').replace(/\*|\[\]$/, '');
        if (!types[tid]) { red(where, `类型 ${c.type} 未声明`); continue; }
        if (seen.has(c.sym) && seen.get(c.sym) !== tid) { red(where, `全局 ${c.sym} 已声明成 ${seen.get(c.sym)}，这里又声明成 ${tid}`); continue; }
        seen.set(c.sym, tid);
        const w = Array.isArray(c.witness) ? c.witness : [];
        if (!w.length) { red(where, `缺 witness EA（全局 ${c.sym}）—— "它里面装的是 ${tid} 指针"至少要有一条 store 证据`); continue; }
        const esc = c.sym.replace(/[.*+?^${}()|[\]\\]/g, '\\$&');
        const storeRe = new RegExp(`^[a-z]+\\s+(?:.*\\b)?ds:${esc}\\s*,`, 'i');
        const stores = [];
        const stale = [];
        for (const e of w) {
          const ea = num(e);
          const text = lineOf(ea);
          if (text == null) { stale.push(`${e}(语料里没有这条 EA)`); continue; }
          if (storeRe.test(text)) stores.push({ ea, text });
          else stale.push(`${e}(${text.trim().slice(0, 40)})`);
        }
        if (!stores.length) { red(where, `witness 里没有一条是**写** ds:${c.sym} 的指令 ⇒ 证明不了这个全局装的是 ${tid}（对不上的：${stale.slice(0, 3).join(' · ')}）`); continue; }
        if (stale.length) notes.push(`G13 ${c.id}: ${stale.length} 条 witness 不是 ds:${c.sym} 的 store（${stale.slice(0, 2).join(' · ')}）—— 别的用途另记，别混进 witness`);
        globalCarriers.push({ id: c.id, sym: c.sym, type: tid, stores: stores.length });
      }
    }
  }

  // ── G14 **命名棘轮**：`T_<Name>` 这个 id 本身是一条**可证伪的断言**（"我就是 <Name> 那个类"）
  //   ★ 为什么必须立：类型 id 从 `T0`/`T_a609c` 改成 `T_Command`/`T_FileDB` 之后，"名字"就成了结论，
  //     可它原先**没有任何守卫** —— 写错一个字母、换一次导出、note 过期，名字都会悄悄变成假话，
  //     而 G1..G13 一条都不会响（实测抓到 `T_RemoteDebug` 的 note 指向 0x497570 = **函数起始行**，
  //     真正的 vtable 写入是 0x4975A3）。
  //   判据（机械）：
  //     ① 把 `T_` 之后那段（如 `Scene`）拿去语料找 `??_7Scene@@6B@`：
  //        找不到 ⇒ 这是**结构名**（`T_Queue_int` / `T_Scene_460` / `T_FileDB_3028`），不要求 RTTI 证据；
  //        找得到 ⇒ 这是**RTTI 声明** ⇒ 必须有一条 `note` 把名字绑到那次 vtable 写入上；
  //     ② note 里的 `??_7<Name>@@` 必须与 id 对得上（⛔ 不许 `T_Scene` 绑 `??_7Font@@`）；
  //     ③ note 里**至少一个** EA 落在**写 vtable 的那条指令**上（该行要逐字含 `offset ??_7<Name>@@`）。
  //        ★ 为什么是"至少一个"而不是"第一个"：note 里常同时给 ctor / dtor / `_atexit` 的 EA，
  //          要求第一个会逼人把有用的地址删掉。
  const g14 = { bound: [], structural: 0 };
  {
    const rttiNames = new Set();
    for (const ln of lines) for (const m of ln.matchAll(/\?\?_7(\w+)@@6B@/g)) rttiNames.add(m[1]);
    if (rttiNames.size) {
      let eaText = null;
      const lineOf = (ea) => {
        if (!eaText) {
          eaText = new Map();
          for (const ln of lines) {
            const m = /^[A-Za-z_.][\w.]*:([0-9A-F]{8})\s+(.*)$/.exec(ln);
            if (m) eaText.set(Number.parseInt(m[1], 16), m[2]);
          }
        }
        return eaText.get(ea) ?? null;
      };
      for (const [tid, t] of Object.entries(types)) {
        if (!/^T_[A-Z]/.test(tid)) continue;              // `T1`/`T2` 这类无名占位不冒充 RTTI
        const name = tid.slice(2);
        if (!rttiNames.has(name)) { g14.structural += 1; continue; }
        const note = String(t.note ?? '');
        const m = /\?\?_7(\w+)@@/.exec(note);
        if (!m) { red(`G14 ${tid}`, `id 声称是 RTTI 类 \`${name}\`（语料里有 \`??_7${name}@@6B@\`），但 note 没有把名字绑到那次 vtable 写入上 ⇒ 名字是空话`); continue; }
        if (m[1] !== name) { red(`G14 ${tid}`, `note 绑的是 \`??_7${m[1]}@@\`，与 id 的 \`${name}\` 对不上`); continue; }
        const eas = [...note.matchAll(/0x([0-9a-f]{6,8})/gi)].map((x) => Number.parseInt(x[1], 16));
        if (!eas.length) { red(`G14 ${tid}`, 'note 里没有 vtable 写入的 EA'); continue; }
        const hit = eas.filter((ea) => { const tx = lineOf(ea); return tx != null && tx.includes(`offset ??_7${name}@@`); });
        if (!hit.length) {
          red(`G14 ${tid}`, `note 里的 EA（${eas.slice(0, 3).map(hex).join(' ')}）没有一条落在**写 vtable 的那条指令**上（要求该行逐字含 \`offset ??_7${name}@@\`）`);
          continue;
        }
        g14.bound.push({ tid, name, hit: hit.length });
      }
      if (g14.bound.length) notes.push(`G14 命名棘轮：${g14.bound.length} 个 RTTI 类名已绑到 vtable 写入点（另有 ${g14.structural} 个结构名不要求 RTTI 证据）`);
    }
  }

  // ── G16 **载体证据棘轮**：`kind:"arg"` 载体的 `witness` 必须**解析得动**
  //   ★ 为什么需要：载体是模型的地基（"`F` 的 `ecx` 装着 `T_X*`"），可它原先**完全免检** ——
  //     实测 174 条 arg 载体**一条 witness 都没有**；`witness` 这个字段此前只有 `kind:"global"` 在用（G13）。
  //   判据（机械、可证伪）：
  //     ① 有 witness ⇒ 每个 EA 在语料里必须是 **`call <该函数>` 这一行**（换一次导出/写错函数名 ⇒ 当场红）；
  //     ② 声明了 `indirectOnly:true`（"只经分派表/vtable 调用，没有直接调用点"）⇒ 语料里**必须真的没有**
  //        `call <该函数>`（反向可证伪：有就直接红 —— 这比"随便少写一条 witness"强得多）；
  //     ③ 既没有 witness 又没声明 `indirectOnly` ⇒ **note**（记 backlog），⛔ 不算通过。
  //        ★ 为什么先记 note：现有 35 条的历史欠账不该让本轮所有批次的红灯亮起来；清完再翻红。
  const g16 = { ok: 0, gap: [], indirect: 0 };
  {
    const argc = (M.carriers ?? []).filter((c) => c.kind === 'arg' && c.fn);
    if (argc.length) {
      const want = new Set(argc.map((c) => c.fn));
      const callEas = new Map();   // fn → Set(EA)：语料里 `call <fn>` 的位置
      for (const ln of lines) {
        const m = /^[A-Za-z_.][\w.]*:([0-9A-F]{8})\s+call\s+(sub_[0-9A-F]{6})\b/.exec(ln);
        if (!m || !want.has(m[2])) continue;
        if (!callEas.has(m[2])) callEas.set(m[2], new Set());
        callEas.get(m[2]).add(Number.parseInt(m[1], 16));
      }
      for (const c of argc) {
        const where = `G16 ${c.id}`;
        const calls = callEas.get(c.fn) ?? new Set();
        const sp = spanBySym.get(c.fn) ?? null;
        const w = Array.isArray(c.witness) ? c.witness : [];
        if (c.indirectOnly === true) {
          if (calls.size) { red(where, `声明了 indirectOnly（"没有直接调用点"），但语料里有 ${calls.size} 处 \`call ${c.fn}\`（如 ${[...calls].slice(0, 2).map(hex).join(' ')}）`); continue; }
          g16.indirect += 1;
          continue;
        }
        if (!w.length) { g16.gap.push(c.id); continue; }
        // ★ 两类**合法** witness（都可机械复核）：
        //   ① 调用点：那一行逐字是 `call <fn>`（证明"某个调用方把对象指针塞进 ecx"）；
        //   ② 来路/约定：该行**出现 ecx**（实测形状 `mov ebx, ecx` / `lea ecx,[esi+14D30h]` ——
        //      前者证明"这个寄存器就是对象根"，后者证明"传进去的是 obj0+K 的内嵌子对象"）。
        //   ⛔ 其它位置一律红（那正是"随便挑个 EA 凑数"的样子）。
        //   ★ 硬要求：**至少一条是调用点** —— 只有"来路"证据证明不了"这个函数真的被这样调用过"。
        const cls = w.map((e) => {
          const ea = num(e);
          if (calls.has(ea)) return { ea, kind: 'call' };
          const line = eaTextOf(ea);
          if (line && /\becx\b/i.test(line)) return { ea, kind: 'src', line };
          return { ea, kind: 'other', line };
        });
        for (const x of cls) {
          if (x.kind === 'other') red(where, `witness ${hex(x.ea)} 既不是 \`call ${c.fn}\`、也不含 ecx 来路（该行：${(x.line ?? '（语料里没有这条 EA）').trim().slice(0, 50)}）`);
        }
        if (!cls.some((x) => x.kind === 'call')) {
          red(where, `witness 里没有一条落在**调用点**上（只有入口约定证据）⇒ 证明不了"哪个调用方把 ${c.type} 塞进 ecx"${calls.size ? `（语料里该函数有 ${calls.size} 处调用点，如 ${[...calls].slice(0, 2).map(hex).join(' ')}）` : ''}`);
          continue;
        }
        if (cls.every((x) => x.kind !== 'other')) g16.ok += 1;
      }
      notes.push(`G16 载体证据：${g16.ok} 条 arg 载体的 witness 落在 \`call\` 调用点上`
        + (g16.indirect ? ` · ${g16.indirect} 条声明了 indirectOnly（语料里确无直接调用点）` : '')
        + (g16.gap.length ? ` · **${g16.gap.length} 条还没有 witness**（⛔ 不算通过，清完再翻红）：${g16.gap.slice(0, 12).join(' ')}` : ''));
    }
  }

  // ── 适配器类型（`adapter:true`）：声明的是**布局约束**（"调用方在 `+K` 上有这个成员"），
  //   不是"我是这个类"。★ 为什么必须显式标记：它是"不同调用方类在同一偏移上**相容**"这条事实的唯一落点 ——
  //   实测 `sub_4034C0`/`D0`/`E0`（`this->+0x4` 的转发器，其中 `sub_4034E0` 还是虚调用）被 `T0` 与它的
  //   内嵌子对象 `T_1ee8`/`T_4ed10`/`T_a609c` 四种类当 this 调用；不标记的话调用点对账会记成 4 处不一致。
  //   两条硬要求：① 不许给 `size` / `complete`（那会把"部分布局"说成"完整对象"）；
  //              ② 至少一个成员（空适配器什么也没约束 ⇒ 空话声明）。
  const adapterTypes = [];
  for (const [tid, t] of Object.entries(types)) {
    if (t.adapter !== true) continue;
    if (t.size != null) red(`适配器 ${tid}`, '`adapter:true` 的类型不许给 `size`（它声明的是**部分布局**，不是完整对象）');
    if (t.complete === true) red(`适配器 ${tid}`, '`adapter:true` 的类型不许声明 `complete`（它只约束被用到的那些偏移）');
    if (!(t.members ?? []).length) { red(`适配器 ${tid}`, '`adapter:true` 的类型至少要有一个成员（空适配器没有约束任何东西）'); continue; }
    adapterTypes.push({ tid, members: (t.members ?? []).length });
  }
  if (adapterTypes.length) {
    notes.push(`适配器类型 ${adapterTypes.map((a) => `${a.tid}(${a.members} 个成员)`).join(' · ')}`
      + '：不同调用方类在这些偏移上**相容**（调用点对账记 🔗 一档，⛔ 不是"不一致"）');
  }

  // ── G6 未观察成员（观察集优先；没有观察集就退成字面量下界）
  for (const [tid, t] of Object.entries(types)) {
    for (const m of membersOf(t)) {
      const abs = ABS.get(`${tid}:${m.offN}`);
      if (abs === undefined) { notes.push(`G6 ${tid}: 成员 ${hex(m.offN)} 无绝对锚 ⇒ 未观察判不了`); continue; }
      if (OBS ? !OBS.has(abs) : refs(abs) === 0) notes.push(`G6 ${tid}: 成员 ${hex(m.offN)}（绝对 ${hex(abs)}）零观察 ⇒ unobserved（不是红，是待观察）`);
    }
  }

  // ── J3 区域角色（本版最小实现：span 必须逐字等于导出段落）
  for (const r of M.roles ?? []) {
    const d = deriveRegions(lines, r.fn, r.reg);
    if (!d) { red(`J3 ${r.id}`, `找不到 ${r.fn}`); continue; }
    const a = num(r.span?.[0]); const b = num(r.span?.[1]);
    const hit = d.regions.find((x) => x.from === a && x.to === b);
    if (!hit) red(`J3 ${r.id}`, `span [${hex(a)},${hex(b)}] 不等于 ${r.fn}·${r.reg} 的任何导出段落`);
    else { notes.push(`J3 ${r.id}: 与导出段落一致（类别 ${hit.cat}，段首${hit.openedByInit ? '是' : '不是'} init）`); if (!hit.allConstOrInit) red(`J3 ${r.id}`, '该段落含非常量形态 ⇒ 不能声明成推进链'); }
  }

  if (cons.obj0Acc !== cons.obj0Explained + cons.obj0Residue) red('守恒①', `${cons.obj0Acc} != ${cons.obj0Explained}+${cons.obj0Residue}`);
  if (cons.pointee !== cons.pointeeOk + cons.pointeeOpaque + cons.pointeeBad + cons.pointeeMulti + cons.pointeeAmbiguous) red('守恒②', `${cons.pointee} != ${cons.pointeeOk}+${cons.pointeeOpaque}+${cons.pointeeBad}+${cons.pointeeMulti}（跨两级以上）+${cons.pointeeAmbiguous}（基址/索引分不清）`);
  // ★ 守恒③：**站点按基址分桶**（六类互斥且穷尽）—— 漏掉任何一类，等式立刻差数（这是"静默变小"的哨兵）
  const bucketSum = cons.obj0Acc + cons.partial + cons.pointee + cons.local + cons.symbol + cons.unresolvedBase;
  if (cons.sites !== bucketSum) red('守恒③', `${cons.sites} != obj0 ${cons.obj0Acc} + 位移不可用 ${cons.partial} + 二级对象（含多跳）${cons.pointee} + 局部载体 ${cons.local} + 数据段符号 ${cons.symbol} + 未解析基址 ${cons.unresolvedBase}`);
  if (cons.symbol + cons.unresolvedBase > 0) {
    notes.push(`★ 形状层**表达不了**的访问基线：数据段符号 ${cons.symbol} 处 · 未解析基址 ${cons.unresolvedBase} 处`
      + ` · 未解析索引 ${cons.unresolvedIndex} 处 · 推不出宽度 ${cons.widthUnknown} 处`
      + ' ⇒ 逐载体报在下面，⛔ 不是"已通过"');
  }
  cons.unfalsifiable = unfalsifiable.length;
  cons.viaView = cons.obj0ExplainedViaView;
  cons.ambiguous = ambiguous.length;
  return { findings, notes, perCarrier, cons, unfalsifiable, declaredViews, ambiguous, localCarriers: localReport, localResidues, disputes, globalCarriers };
}

/** 渲染报告（终端用；测试断言 findings / cons 即可） */
export function renderReport(rep, { title = 'claims-check', byType = false } = {}) {
  const L = [`# ${title}`, `  类型声明 / 载体 / 角色：见模型文件；本报告只给**判定**与**残差**`, ''];
  L.push('## 逐载体');
  for (const p of rep.perCarrier) {
    const { c, explained, residue, total, pointee, pointeeOk, pointeeOpaque, deep, opaqueEAs, partial, indexUnusable } = p;
    L.push(`  ${pad(c.id, 12)} ${pad(c.fn, 12)} ${c.reg}→${pad(c.type, 6)} 访问 ${total} 处：解释 ${explained.length} · 残差 ${residue.length}`
      + `｜二级 ${pointee}（解释 ${pointeeOk} · 不透明 ${pointeeOpaque}${p.pointeeAmbiguous ? ` · 基址/索引分不清 ${p.pointeeAmbiguous}` : ''}）`
      + (deep ? `｜**超出解析深度 ${deep} 处**` : '')
      + (opaqueEAs.length ? `｜不透明 EA：${opaqueEAs.slice(0, 3).map(hex).join(' ')}` : '')
      + (partial || indexUnusable ? `｜位移不可用 ${partial + indexUnusable} 处（lea 取址 ${partial} · 索引未解析 ${indexUnusable}）` : ''));
    for (const e of explained) {
      // ★ **观测档** / **推测档**分开写：视图解释的算术是**证明**过的，但"这段字节是同一个数组、索引差 k"
      //   是**模型的判断** ⇒ 报告里必须看得出来（skill 四档话里它不叫 holds）
      if (e.viaView) L.push(`        🧩 ${hex(e.ea)} abs=${hex(e.abs)} ⇒ 视图（索引差 ${e.delta}）⇒ 成员 ${hex(e.member.offN)} → 元素内 ${hex(e.leaf.offN)}　**推断**`);
      else L.push(`        ✅ ${hex(e.ea)} abs=${hex(e.abs)} ⇒ 成员 ${hex(e.member.offN)}`);
    }
    for (const e of residue) L.push(`        ⚪ ${hex(e.ea)} ${e.why}`);
    // ★ 守恒③ 的非 obj0 桶：逐类报数 + 可处置的样例 EA（⛔ 不许静默丢）
    if (p.symbol) L.push(`        ⛔ 数据段符号 ${p.symbol} 处（${p.symbolSyms.slice(0, 4).join(' ')}）EA：${p.symbolEAs.slice(0, 3).map(hex).join(' ')}`);
    if (p.pointeeMulti) L.push(`        ⚠ **跨两级以上（未判）** ${p.pointeeMulti} 处：路径 ${p.multiHopPaths.slice(0, 3).join(` / `)}　EA ${p.multiHopEAs.slice(0, 3).map(hex).join(` `)}　⇒ 请用**局部类型载体**（kind:"local"）表达`);
    if (p.unresolvedBase) L.push(`        ⛔ 未解析基址 ${p.unresolvedBase} 处（按基址寄存器：${p.unresolvedByReg.map(([r, n]) => `${r}×${n}`).join(' · ')}）EA：${p.unresolvedEAs.slice(0, 3).map(hex).join(' ')}`);
    if (p.unresolvedIndex) L.push(`        ⚠ 未解析索引 ${p.unresolvedIndex} 处 EA：${p.unresolvedIndexEAs.slice(0, 3).map(hex).join(' ')}`);
    if (p.widthUnknown) L.push(`        ⚠ 推不出宽度 ${p.widthUnknown} 处（⛔ 不许当 4）`);
  }
  L.push('', '## 提示（不算红）');
  for (const n of rep.notes) L.push(`  · ${n}`);
  const c1 = rep.cons.obj0Acc === rep.cons.obj0Explained + rep.cons.obj0Residue ? '✅' : '❌';
  const c2 = rep.cons.pointee === rep.cons.pointeeOk + rep.cons.pointeeOpaque + rep.cons.pointeeBad + rep.cons.pointeeMulti + (rep.cons.pointeeAmbiguous ?? 0) ? '✅' : '❌';
  const sum = rep.cons.obj0Acc + rep.cons.partial + rep.cons.pointee + rep.cons.local + rep.cons.symbol + rep.cons.unresolvedBase;
  const c3 = rep.cons.sites === sum ? '✅' : '❌';
  L.push('', `## 守恒①（obj0 相对访问）：${rep.cons.obj0Acc} = 解释 ${rep.cons.obj0Explained} + 残差 ${rep.cons.obj0Residue}  ${c1}`);
  L.push(`## 守恒②（二级对象）：${rep.cons.pointee} = 目标类型解释 ${rep.cons.pointeeOk} + 不透明 ${rep.cons.pointeeOpaque} + 拒绝/未声明 ${rep.cons.pointeeBad} + **跨两级以上（未判）${rep.cons.pointeeMulti}** + **基址/索引分不清（未判）${rep.cons.pointeeAmbiguous ?? 0}**  ${c2}`);
  L.push(`## 守恒③（站点按基址分桶）：${rep.cons.sites} = obj0 相对 ${rep.cons.obj0Acc} + 位移不可用 ${rep.cons.partial}`
    + `（lea 取址 ${rep.cons.partialTakeAddr} · 索引未解析 ${rep.cons.partialIndexUnresolved}）`
    + ` + 二级对象（含多跳）${rep.cons.pointee} + **局部载体 ${rep.cons.local}**（解释 ${rep.cons.localOk} · 残差 ${rep.cons.localResidue}）`
    + ` + 数据段符号 ${rep.cons.symbol} + 未解析基址 ${rep.cons.unresolvedBase}  ${c3}`);
  L.push(`## 跨两级以上的指针链（**未判** —— 请用局部类型载体 kind:"local" 表达）：${rep.cons.deep} 处  ${rep.cons.deep ? '⚠ 已单列（带 EA + 路径）' : '✅ 无'}`);
  // ★ 守恒④：**解释分两档**（观测档 = 站点形状直接落在成员窗口里；推测档 = 靠**声明的视图**——
  //   算术是证明过的，但"这段字节是同一个数组、索引差 k"是模型的判断）。分不开就没法审"哪条结论是猜的"。
  const c4 = rep.cons.obj0Explained === rep.cons.obj0ExplainedObserved + rep.cons.obj0ExplainedViaView ? '✅' : '❌';
  if (rep.cons.obj0ExplainedViaView || (rep.declaredViews ?? []).length) {
    L.push(`## 守恒④（解释分档）：${rep.cons.obj0Explained} = 观测档 ${rep.cons.obj0ExplainedObserved} + **视图推测档 ${rep.cons.obj0ExplainedViaView}**  ${c4}`);
  }
  if ((rep.declaredViews ?? []).length) {
    L.push(`## 声明的视图（**模型的推测**：同数组、索引差 k；算术自洽由工具校验，语义不由本层背书）`);
    for (const v of rep.declaredViews) L.push(`  · ${v.tid}+${hex(v.off)} 索引差 ${v.delta} ⇒ 覆盖 [${hex(v.from)},${hex(v.to)})${v.label ? `　label：${v.label}` : ''}`);
  }
  if (rep.ambiguous?.length) {
    L.push(`## ⚠ 多窗口同分 ⇒ 选不出宿主（⛔ 不许靠"谁先声明"决定）：${rep.ambiguous.length} 处`);
    for (const a of rep.ambiguous.slice(0, 5)) L.push(`  · ${hex(a.ea)} abs=${hex(a.abs)} 落在 ${a.between.map(hex).join(' 与 ')} 之间`);
  }
  if (rep.localCarriers?.length) {
    L.push('## 局部类型载体（寄存器 → 类型的**断言**；span 必须等于 `regions` 的活窗口 [from..liveTo]）');
    for (const l of rep.localCarriers) L.push(`  · ${l.id}  ${l.fn}·${l.reg} → ${l.type}*  活窗口 [${hex(l.from)}..${hex(l.to)}] 类别 ${l.cat}：站点 ${l.sites}（解释 ${l.ok} · 残差 ${l.bad}）`);
  }
  if (rep.globalCarriers?.length) {
    L.push('## 全局载体（数据段符号 → 类型；**根对象**只能这么声明 —— 它不是任何函数的参数）');
    for (const g of rep.globalCarriers) L.push(`  · ${g.id}  ds:${g.sym} → ${g.type}*（store 证据 ${g.stores} 条）`);
  }
  if (rep.disputes?.length) {
    L.push('## 争议台账（**已知有争议、但下不了结论** —— 这些地方判据覆盖不到，⛔ 不能读成"已解决"）');
    for (const d of rep.disputes) L.push(`  · ${d.container}+${d.off} [${d.kind}] ${d.why}（${d.evidence.slice(0, 3).join(' ')}）`);
  }
  if (rep.unfalsifiable?.length) {
    L.push(`## 空话声明（**没有任何用法约束它** ⇒ ⛔ **不算通过**；不是红：类型/视图是共享的，别的族文件可能才用它）：${rep.unfalsifiable.length} 条`);
    for (const u of rep.unfalsifiable) L.push(`  · ${u.type}+${hex(u.off)}${u.delta !== undefined ? `（视图 索引差 ${u.delta}）` : ''} → ${u.why}`);
  }
  L.push('', rep.findings.length ? `❌ ${rep.findings.length} 条红：\n  - ${rep.findings.map((x) => `${x.where}：${x.why}`).join('\n  - ')}` : '✅ 全部义务通过（守卫 / G1..G9 / J3 / 守恒①②③④）');
  if (byType) L.push('', ...renderByType(rep));
  return `${L.join('\n')}\n`;
}

/**
 * **残差按类型聚合**（`--by-type`）—— "下一批该补哪个类型的成员表"的机械依据。
 *
 * ★ 三条口径（都是踩出来的）：
 *   ① **类型从残差行自己取**，不从"当前处理到哪个载体"的状态取 —— `carriers.json` 的 id 不只有 `cNN`，
 *      还有按批发的 `b10cNN`/`b12cNN`；用 `^c\d+` 认块头会把那些块漏掉、把它们的残差记到上一个载体名下
 *      （实测把 `T_Input` 8/8 与 `T_Sound` 4/5 算成了 `T_Font`，`T_CBunki` 7/14 与 `T_ReadTextDB` 1/4 整块没被派出去）。
 *   ② **宿主可能是第二跳的类型**：`[this+K] → 指针 → [eax+M]` 的残差落在 `T_Input`/`T_Sound`，
 *      而不是发起访问的 `T_Font` ⇒ 要按**被访问的那个类型**去做，否则会把 `T_Input+0x490` 写进 `T_Font`（假声明）。
 *   ③ 只统计 `⚪ 不落在…` 这一类残差；`位移不可用` / `未解析基址` / `数据段符号` 各是别的桶，⛔ 不许混进来。
 */
export function renderByType(rep) {
  const agg = new Map();   // type -> off -> {n, who:Set}
  const bump = (ty, off, who) => {
    if (!agg.has(ty)) agg.set(ty, new Map());
    const inner = agg.get(ty);
    const e = inner.get(off) ?? { n: 0, who: new Set() };
    e.n += 1; e.who.add(who);
    inner.set(off, e);
  };
  for (const p of rep.perCarrier ?? []) {
    const carrierType = String(p.c?.type ?? '?').replace(/\*$/, '');
    for (const r of p.residue ?? []) {
      const w = String(r.why ?? '');
      // `… 不是 T_X 的成员` / `落在内嵌子对象 T_X+0x… 里` ⇒ 宿主就是 T_X；否则算这个载体的类型
      const m = /不是\s+(\w+)\s+的成员/.exec(w) ?? /落在内嵌子对象\s+(\w+)\+/.exec(w);
      bump(m ? m[1] : carrierType, r.abs ?? 0, p.c?.id ?? '?');
    }
  }
  if (!agg.size) return ['## 残差按类型（`--by-type`）：0 处（没有 `⚪ 不落在…` 这一类残差）'];
  const rows = [...agg.entries()].map(([ty, inner]) => ({ ty, offsets: inner.size, n: [...inner.values()].reduce((s, v) => s + v.n, 0), inner }))
    .sort((a, b) => b.n - a.n);
  const out = [`## 残差按类型（\`--by-type\`：**下一批该补哪个类型的成员表**；⛔ 不含位移不可用 / 未解析基址 / 数据段符号 —— `
    + `含 obj0 残差与**二级对象**残差，两者都表示"该类型的成员表缺这个偏移"）：${rows.reduce((s, r) => s + r.n, 0)} 处 / ${rows.length} 个类型`];
  for (const r of rows) {
    const hot = [...r.inner.entries()].sort((a, b) => b[1].n - a[1].n).slice(0, 6)
      .map(([off, e]) => `${hex(off)}(${e.n})`).join(' ');
    out.push(`  ${pad(r.ty, 30)} ${String(r.offsets).padStart(3)} 个偏移 / ${String(r.n).padStart(4)} 次访问　热点 ${hot}`);
  }
  out.push('  ★ 判活按**被访问的那个类型**去做（第二跳的宿主）：`[this+K] → 指针 → [eax+M]` 的残差属于 `M` 的类型。');
  return out;
}
