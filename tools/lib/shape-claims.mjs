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
      for (const d of [0, ...(m.views ?? []).map((v) => num(v.elementDelta))]) {
        out.push({ m, delta: d, from: m.offN + stride * d, to: m.offN + stride * d + elemSize, kind: 'array', sweep: sweepOf(m), stride, elemSize });
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
        const coveredByZero = d > 0 && m.count != null && to <= m.offN + stride * m.count;
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
    pointee: 0, pointeeOk: 0, pointeeOpaque: 0, pointeeBad: 0, pointeeMulti: 0, deep: 0,
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
        if (t.complete) red(`G2 ${c.id} @${hex(s.ea)}`, `访问 ${hex(s.absDisp)} 不落在 ${c.type} 的任何已声明成员上（且该类型声明为 complete）`);
        else residue.push({ ea: s.ea, abs: s.absDisp, kind: 'obj0', why: `访问 ${hex(s.absDisp)} 不落在 ${c.type} 的任何已声明成员上` });
        continue;
      }
      const w = pickWindow(cands, s);
      for (const cw of cands) candidateWindows.add(`${tid}|${cw.m.offN}|${cw.delta}`);
      const host = w.m;
      let leaf = host;
      if (w.kind === 'array') {              // ★ 数组（含视图窗口）：还要落到**元素类型的成员**上
        const et = types[host.type];
        const elemOff = s.absDisp - w.from;
        const em = membersOf(et).find((m) => elemOff >= m.offN && (m.ext == null ? elemOff < m.offN + 1 : elemOff < m.offN + m.ext));
        if (!em) {
          if (et?.complete) red(`G2 ${c.id} @${hex(s.ea)}`, `元素内偏移 ${hex(elemOff)} 不是 ${host.type} 的成员（${host.type} 声明为 complete）`);
          else residue.push({ ea: s.ea, abs: s.absDisp, kind: 'obj0', why: `元素内偏移 ${hex(elemOff)} 不是 ${host.type} 的成员` });
          continue;
        }
        leaf = em;
        if (s.indexSym && et?.size != null && s.stride !== et.size) { red(`G3 ${c.id} @${hex(s.ea)}`, `数组形态步长 ${s.stride} != size(${host.type})=${et.size}`); continue; }
      }
      if (leaf.width != null && s.access !== 'a' && s.width !== 'unknown' && s.width > leaf.width) { red(`G2 ${c.id} @${hex(s.ea)}`, `宽度 ${s.width} != 成员声明 ${leaf.width}`); continue; }
      explained.push({ ea: s.ea, abs: s.absDisp, member: host, leaf, delta: w.delta, viaView: w.delta !== 0 });
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
    const pointee = sites.filter((s) => (s.pointeePath?.length ?? 0) >= 1 && typeof s.pointeeOff === 'number');
    let pointeeOk = 0; let pointeeOpaque = 0; let pointeeBad = 0; let pointeeMulti = 0;
    const opaqueEAs = [];
    const multiHopSites = [];
    for (const s of pointee) {
      const path = s.pointeePath;
      // ★ **只取址（lea）** 的二级站点：值被用于地址算术，不是解引用 ⇒ 记**解释**档
      //   （真正的解引用一定另有非 lea 站点；实测踩过：整数游标被当指针时 4 条 G5' 假红）
      if (s.access === 'a') { pointeeOk += 1; continue; }
      if (path.length + 1 > 2) { pointeeMulti += 1; multiHopSites.push(s); continue; }   // 跨两级以上：不判
      const hop = path[0];
      if (!derefHosts.has(tid)) derefHosts.set(tid, new Set());
      derefHosts.get(tid).add(hop);
      const host = ms.find((m) => m.offN === hop);
      if (!host) {
        pointeeBad += 1;
        if (t.complete) red(`G5 ${c.id} @${hex(s.ea)}`, `二级对象的宿主 ${hex(hop)} 不是 ${c.type} 的成员`);
        else residue.push({ ea: s.ea, abs: hop, kind: 'pointee', why: `二级对象宿主 ${hex(hop)} 未声明为成员` });
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
    cons.pointee += pointeeOk + pointeeBad + pointeeOpaque + pointeeMulti;
    cons.pointeeOk += pointeeOk;
    cons.pointeeOpaque += pointeeOpaque;
    cons.pointeeBad += pointeeBad;
    cons.pointeeMulti += pointeeMulti;
    cons.deep += multiHopSites.length;
    cons.symbol += symbolSites.length;
    cons.unresolvedBase += unresolvedSites.length;
    cons.unresolvedIndex += indexBadSites.length;
    cons.widthUnknown += widthBadSites.length;
    // 未解析按**基址寄存器**分组（§7：报数要报到能处置的粒度，⛔ 不许只给个数字）
    const byReg = new Map();
    for (const s of unresolvedSites) byReg.set(s.baseReg ?? '(无基址寄存器)', (byReg.get(s.baseReg ?? '(无基址寄存器)') ?? 0) + 1);
    perCarrier.push({
      c, explained, residue, total: obj0.length,
      viaView: explained.filter((e) => e.viaView).length,
      viaViewEAs: explained.filter((e) => e.viaView).map((e) => ({ ea: e.ea, abs: e.abs, member: e.member.offN, delta: e.delta })),
      pointee: pointeeOk + pointeeBad + pointeeOpaque + pointeeMulti, pointeeOk, pointeeOpaque, pointeeMulti,
      multiHopEAs: multiHopSites.map((s) => s.ea),
      multiHopPaths: multiHopSites.map((s) => (s.pointeePath ?? []).map((h) => "+" + hex(h)).join(" -> ")),
      deep: pointeeMulti, deepEAs: multiHopSites.map((s) => s.ea), opaqueEAs,
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
      if (refs(end) === 0) {
        // ★ 奇数跨度（u16×17=0x22 / u16×289=0x242）的末尾按定义不是 4 对齐 ⇒ 编译器不会算出它 ⇒ 零引用是必然，不是"条数无依据"
        if (end % 4 !== 0) notes.push(`G4 ${tid}+${hex(m.offN)}: 末尾 ${hex(end)} 不对齐 4 且零引用 ⇒ 编译器按定义不会算出它（边界靠常量+写循环证明，⛔ 不是"通过"）`);
        else red(`G4 ${tid}+${hex(m.offN)}`, `数组末尾 ${hex(end)} 在语料里零引用 ⇒ count=${m.count} 无依据`);
      }
      else notes.push(`G4 ${tid}+${hex(m.offN)}: 末尾 ${hex(end)} 有 ${refs(end)} 处引用 ⇒ count=${m.count} 站得住`);
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
  if (cons.pointee !== cons.pointeeOk + cons.pointeeOpaque + cons.pointeeBad + cons.pointeeMulti) red('守恒②', `${cons.pointee} != ${cons.pointeeOk}+${cons.pointeeOpaque}+${cons.pointeeBad}+${cons.pointeeMulti}（跨两级以上）`);
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
export function renderReport(rep, { title = 'claims-check' } = {}) {
  const L = [`# ${title}`, `  类型声明 / 载体 / 角色：见模型文件；本报告只给**判定**与**残差**`, ''];
  L.push('## 逐载体');
  for (const p of rep.perCarrier) {
    const { c, explained, residue, total, pointee, pointeeOk, pointeeOpaque, deep, opaqueEAs, partial, indexUnusable } = p;
    L.push(`  ${pad(c.id, 12)} ${pad(c.fn, 12)} ${c.reg}→${pad(c.type, 6)} 访问 ${total} 处：解释 ${explained.length} · 残差 ${residue.length}`
      + `｜二级 ${pointee}（解释 ${pointeeOk} · 不透明 ${pointeeOpaque}）`
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
  const c2 = rep.cons.pointee === rep.cons.pointeeOk + rep.cons.pointeeOpaque + rep.cons.pointeeBad + rep.cons.pointeeMulti ? '✅' : '❌';
  const sum = rep.cons.obj0Acc + rep.cons.partial + rep.cons.pointee + rep.cons.local + rep.cons.symbol + rep.cons.unresolvedBase;
  const c3 = rep.cons.sites === sum ? '✅' : '❌';
  L.push('', `## 守恒①（obj0 相对访问）：${rep.cons.obj0Acc} = 解释 ${rep.cons.obj0Explained} + 残差 ${rep.cons.obj0Residue}  ${c1}`);
  L.push(`## 守恒②（二级对象）：${rep.cons.pointee} = 目标类型解释 ${rep.cons.pointeeOk} + 不透明 ${rep.cons.pointeeOpaque} + 拒绝/未声明 ${rep.cons.pointeeBad} + **跨两级以上（未判）${rep.cons.pointeeMulti}**  ${c2}`);
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
  return `${L.join('\n')}\n`;
}
