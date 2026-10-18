#!/usr/bin/env node
/**
 * tools/shape.mjs —— **CLI**：对象 shape 的「访问记录 / 独立复核 / 段落导出 / claims 校验」
 *
 * 经派发器：`pnpm tools shape <access|verify|regions|claims|describe> [args…]`
 * 也可独立跑：`node tools/shape.mjs --regions sub_40ED40 edi`
 *
 * ★ 本域**不认识任何具体家族**（别指望它知道"算数指令"是什么）：输入只是**若干 `sub_xxxxxx`**。
 *   命令 / 口径 / 义务在 `lib/shape-*.mjs`（那边是 schema 的真源）；本文件只做"参数 → 模型 → 打印"。
 *
 * ## 三个落点（都是"判定"，不是"结论"）
 * * `access <sym>`  —— 形状层**助手**的逐 site 访问记录（EA 锚；含 obj / 位移 / 步长 / 索引 / 宽度 / 跳数）
 * * `verify <EA>`   —— **独立复核器**：按 EA 回语料重解一条指令（verified / drifted / unverifiable）
 * * `regions <sym> [reg]` —— **校验器功能①**：机械导出拆分范围（段落 / 类别 / 段首 / 是否被 `call` 打断）
 * * `claims <file>|--sample` —— **校验器功能②**：校验 claims（类型 / 载体 / 角色 / 域 / 逃逸）
 *
 * 读写：**r**（只读语料与 `.c`；本域不写任何文件 —— claims 文件由人 / 模型自己给）。
 */
import fs from 'node:fs';
import path from 'node:path';
import process from 'node:process';

import { analyzeLines, functionSpans, loadListing, objRootRegs } from './lib/shape-access.mjs';
import { censusBodies, renderBodies } from './lib/shape-bodies.mjs';
import { censusHandlers, renderCensus } from './lib/shape-census.mjs';
import { checkClaims, DRILL_MAX_LEVEL, objectLevel, renderReport, sampleModel, VOCAB, REDUNDANT } from './lib/shape-claims.mjs';
import { deriveRegions, insnsOf } from './lib/shape-regions.mjs';
import { checkRow, describeEa, loadRaw } from './lib/shape-verify.mjs';
import { coverageOf, renderCoverage } from './lib/shape-coverage.mjs';
import { auditCalls, renderCalls } from './lib/shape-calls.mjs';
import { propagate, nextCandidates } from './lib/shape-propagate.mjs';
import { fileURLToPath } from 'node:url';

/** 本仓根（本域要拿语料分派表 ⇒ 与 `lib/opcodes.mjs` 的口径一致；不 import `cli.mjs`，免得转出循环依赖） */
const REPO_ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');

export const DOMAIN = {
  id: 'shape',
  title: '对象 shape：访问记录 / 独立复核 / 段落导出 / claims 校验（EA 锚；不写任何文件）',
  data: [
    '`corpus/disasm/files/*.{lst,c}`（**只读**语料；解压产物，gitignore）',
    'claims 文件（**由人 / 模型给**，本域只读它；⛔ 不读 `apps/**` —— 那是同层产物）',
  ],
  access: 'r（只读；⛔ 本域没有任何写操作）',
  tool: 'tools/shape.mjs',
};

export const OPERATIONS = [
  { name: 'access', argv: ['--access'], mutates: false, summary: '一个函数的逐 site 访问记录（EA 锚；含 obj / 位移 / 步长 / 索引来源 / 宽度 / 跳数）' },
  { name: 'verify', argv: ['--verify'], mutates: false, summary: '★ **独立复核器**：按 witness EA 回语料重解（verified / drifted / unverifiable；证明 vs 推断按字段）' },
  { name: 'regions', argv: ['--regions'], mutates: false, summary: '★ **功能①**：机械导出某函数某寄存器的拆分范围（段落 / 类别 / 段首形态 / 是否被 call 打断）' },
  { name: 'depth', argv: ['--depth'], mutates: false, summary: '**对象层级分布**（level 1 / 2 / 3+）：`[<sym>]`；用来现跑"下钻上限冻结在层级 2"这个判决（层级 ≥3 单列，不静默丢）' },
  { name: 'census', argv: ['--census'], mutates: false, summary: '★ **全表普查**：每个**有 handler 的 opcode**（判据 = 语料分派表）的站点分类 / `obj0` 偏移聚类 / 索引簇 / 未解析三类报数（`--match <sym>` 收窄 · `--json` 全量）' },
  { name: 'bodies', argv: ['--bodies'], mutates: false, summary: '★ **handler 体骨架普查**：记录槽写法（哪个立即数写进哪个位移）+ 调用序列（每个 callee 的实参立即数）⇒ 按骨架分组（`--match` · `--json`）—— 这是 A1–A6 的**可算版**' },
  { name: 'claims', argv: ['--claims'], mutates: false, summary: '★ **功能②**：校验 claims 文件（守卫 / G1..G14 / J3 / 四守恒）；`--sample` 用内置样本；`--by-type` 加一节**残差按类型聚合**（"下一批该补哪个类型的成员表"的机械依据）' },
  { name: 'propagate', argv: ['--propagate'], mutates: false, summary: '★ **类型传播（全图 BFS）**：从已声明载体沿调用图推导每个函数根寄存器是什么类 ⇒ 批次顺序 + "类判定"的**唯一机械依据**（⛔ 不按偏移命中率猜类）' },
  { name: 'calls', argv: ['--calls'], mutates: false, summary: '★ 调用点类型对账（跨函数）：调用方实参来源 → 被调方的载体声明 ⇒ 一致 / 不一致 / 单侧缺声明（不拿同偏移顶替类型；栈上传参本版不覆盖）' },
  { name: 'coverage', argv: ['--coverage'], mutates: false, summary: '★ **闭合度**：指令级记账 —— 未归类行 / 间接调用 / 间接跳转 / 抛异常 / x87·SSE 内存操作数 / 基址未解析 ⇒ 判 `L0 形状闭合`（**回答"这个函数分析完了吗"**；`--all [--limit N]`）' },
  { name: 'describe', argv: ['--describe'], mutates: false, summary: '自描述：claims 分节 / 义务 / 两道守卫 / 诚实项（schema 的真源）' },
];

const hex = (n) => (typeof n === 'number' ? `0x${n.toString(16)}` : String(n));

export function describeText() {
  return `shape —— 对象 shape 的观察与校验（汇编层；锚只有 EA）

数据    corpus/disasm/files/*.{lst,c}（只读语料）· claims 文件（人/模型给；本域只读）
读写    r（本域没有任何写操作）
操作    access · verify · regions · depth · census · claims · describe

分节（claims 文件 = 模型的**判断**，不是转录）
  types      每个类型**按 id 共享一份**：成员 = { off(必填) · width | type+count · kind · name(非键) }；洞不写
             ★ 成员有**三种合法形态**（缺一半 ⇒ G8 红，否则跨度算不出、判定器会把它**静默当 1 字节槽**）：
               ① 内联值：\`width\`；
               ② 数组：\`type\` + \`count\`（元素类型必须给 \`size\`）；
               ③ **条数未观察的数组**：\`type\` + \`count: null\` + \`sweep:{stride,index}\` ——
                  形态（步长 + 索引身份）说得出来、边界说不出来；⛔ 缺 \`sweep\` 会让窗口**无界**（吞掉别的偏移）
             ★ \`kind\` 只有 \`"ptr"\` 有意义 —— 意思是"这个成员的值是**另一个对象的基址**"（会被解引用）。
               ⛔ 不是"标量"的标记：不写 kind 就只是内联值/数组。标量**再被当基址用** ⇒ G5' 红
             ★ \`views\`（可选，数组成员上）：\`[{ elementDelta, label? }]\` = **模型的推测**——
               "这一段字节是**同一个数组**、索引差 k"（先例：\`0x5d894 + 0x78·(索引−1)\` ≡ \`0x5d81c + 0x78·索引\`）。
               依据 shape-census §4：**原点表是声明的约定，不是语料事实** ⇒ 判定器只做**算术自洽** +
               **可证伪性**（零观察 / 多余的视图单列），语义**不由本层背书**
             ★ \`type\` 在成员上有**两个意思**，按 \`kind\` 分岔：\`kind:"ptr"\` 时是**被指向的类型**（不该配 \`count\`）；
               否则是**数组元素类型**（必须配 \`count\`——写 \`null\` 就得配 \`sweep\`）
             ★ **RTTI 基类数组 \`??_R2<Class>@@8\` 是"有没有中间基类"的判据**：第一个 dword 是条目数，其后每项指向一个
   \`RTTIBaseClassDescriptor\`。实测 \`SE\`/\`Voice\` 都是 **2**（自己 + \`BaseObject\`）而 \`Sound\` 是 **3**（含 \`SoundError\`）
   ⇒ 中间基类**会**出现在数组里；这条把 sub_4BB810 判成"布局适配器"（T_408_trio，adapter:true），而不是硬记成某一类。
★ \`adapter:true\`：这个类型是**布局约束**（"调用方在 +K 上有这个成员"），不是"我是这个类" ——
               用于 \`this->成员\` 的转发器（实测 \`sub_4034C0\`/\`D0\`/\`E0\` 都做 \`mov ecx,[ecx+4]\` 之后转发，
               其中 \`sub_4034E0\` 是 \`mov eax,[ecx]; mov eax,[eax+8]; jmp eax\` = **虚调用**），
               被 \`T0\` 与内嵌子对象 \`T_1ee8\`/\`T_4ed10\`/\`T_a609c\` 四种类当 this 调用。
               硬要求：⛔ 不许 \`size\` · ⛔ 不许 \`complete\` · 至少一个成员；调用点对账记 🔗 一档（⛔ 不是"不一致"）。
             ★ 类型**集中在自己的文件里**（本仓落点 data/shape/types.json）—— 族文件只放**载体**，
               一律 typesFrom 引用它；⛔ 不许把类型写进某个族文件（那会逼别的族文件去引用它）
  carriers   机器槽 → 类型：kind ∈ arg{fn,reg|off} / local{fn,off} / global{sym}
             ★ 字段**不是**载体 —— 它是容器的成员（写在 types 里）
             ★ **局部类型载体**（G10）：\`{kind:"local", fn, reg, span:[from,to], type:"T*"}\` = "这个寄存器在这一段里
               装的是 T 的对象"。⛔ 不许自己写 span：它必须逐字等于 \`regions\` 的**活窗口** \`[from..liveTo]\`；
               活窗口里被 call 打断的 caller-saved 寄存器不许用（值可能已被抹掉）；段落类别必须是 load/ptr/other。
               ★ 用途：指针来源**不可表达**时（参数 / 调用返回值 / 数据段），路径没有任何东西可写 —— 而且它
               **跨得过下钻上限**（实测：用它判掉了层级 3 的站点）。两者对同一处并存 ⇒ G10 报 note（同一事实两套声明）
  roles      { fn, reg, span, role, bound, carryFrom, uses }；span **必须逐字等于** \`regions\` 导出的一个段落
  domains / escapes / identities   可选（本版只做结构检查）
  typesFrom  引用**类型真源**；**同名类型按成员增量合并**（同 off 本地优先）

义务（红了都要有理由；⛔ 没有一档是"静默通过"）
  G1 类型自洽（成员不重叠 / 不越 size）
  G2 覆盖（两级：容器的成员 + **数组成员的元素内成员**）· G3 步长（数组形态 == size(元素类型)）
  G4 数组边界（末尾地址必须在语料里被引用；**条数未观察**则明说"判不了"）
  G5/G5' **二级对象（只管一跳）**（宿主必须是已声明的 \`kind:"ptr"\` + 目标类型 ⇒ 用目标类型的成员表跑一遍；
     \`type:null\` ⇒ **不透明桶**单列；没声明 ptr ⇒ 红）
     ⛔ **不遍历多跳**：一跳 = **推导**（一次成员查找）；跨两级以上一律进"跨两级以上（未判）"档
     （带 EA + 路径），要判就用 **局部类型载体**（\`kind:"local"\`）断言 —— 深度不是校验该管的东西
  G6 未观察成员（用**形状层观察集**判定：它能解折叠形态；字面量扫描只是下界）
  G7 **可证伪性**（声明了 \`kind:"ptr"\` 却在**本模型的载体集里零解引用** ⇒ 单列"空话声明"：
     ⛔ **不算通过**，但**不判红** —— 类型是共享的，别的族文件可能才用它）
  G8 **成员声明自洽**（内联值 / 数组 / 条数未观察 三选一且给全；\`kind\` 闭集合 = {"ptr"}）
  G9 **视图自洽与可证伪性**（算术：\`成员 off + 步长·elementDelta == 被解释的位移\`；
     零观察 / 多余的视图 ⇒ 单列"空话声明"，⛔ 不算通过、不判红）
  J3 角色（span == 导出段落，且段内全 ±const/init）
  守恒①（obj0 访问）· 守恒②（二级对象 = 解释 + 不透明 + 拒绝）
  守恒③（**站点按基址分桶**：obj0 相对 + **位移不可用**（lea 取址 / 索引未解析）+ 二级对象 +
     **二级对象内嵌派生** + **数据段符号** + **未解析基址**）
     ★ 没有这一条时，这几类会**从访问总数里静默消失**（数字变小，不报错）
     ★ 未解析**按载体**报数并给样例 EA（按基址寄存器分组）；⛔ 那不是"已通过"，是"形状层表达不了"
     ★ **基址/索引分不清**（\`[ecx+eax]\` 两个寄存器都是"从内存读出来的值"）⇒ 谁是指针、谁是游标**判不了**，
       单列一档（⛔ 不算通过）。实测 \`sub_447810\` 的 \`0x448069 cmp byte ptr [ecx+eax], 0\`：
       \`ecx=[esi+0x204c]\`、\`eax=[esi+0x2030]\` ⇒ 0x2030 才是图基址、0x204c 是整数游标；
       ⛔ 反事实：把 0x204c 写成 \`kind:"ptr"\` 会让 12 条红全消 —— 那是**假绿**。
  守恒④（**解释分档**：解释 = **观测档** + **视图推测档**）—— 分不开就审不出"哪条结论是猜的"
  G13 **全局载体**（\`{kind:"global", sym, type, witness}\`）= "这个数据段符号里装的是 T 的指针"。
     ★ 为什么必须有：**根对象不是参数** —— Engine（IDA 类名 \`Command\`）由 \`_WinMain@16\` 里
       \`push 0AABC8h\` + \`operator new\`(0x4baa11) → \`call sub_415640\`(0x4baa2e) → \`mov ds:dword_55E1BC, eax\`(0x4baa3a)
       构造之后才进全局。没有这一类，从根**正向**传播一步都走不了（只能从 op handler 倒着爬）。
     三条硬要求：① \`sym\` 在语料里真实出现；② \`witness\` 里**至少一条 EA 是写该符号的 store**（\`mov ds:sym, …\`）
     —— 这是"它装的确实是那个指针"的唯一机械证据（对不上的 EA 单列，⛔ 不许凑数）；③ \`type\` 已声明，
     且同一个符号不许两种类型（否则下游传播会随声明顺序漂）。
     ★ 传播（--propagate）：调用点上 \`ecx\` 的来源是 \`k:'global'\` 且该符号有声明 ⇒ 被调方根类型 = 该类型；
       没声明的全局**不传播**；被调方**必须真的收 this**（\`__cdecl\` 里的 ecx 是残留值 ⇒ 记上去是假断言 / 假冲突）。
     ★ **同偏移不同类 = 基类相容，不是冲突**：\`sub_4034C0\`/\`sub_4034D0\`/\`sub_4034E0\` 是 \`this->+0x4\` 的**转发器**
       （0x4034c3 \`mov ecx,[ecx+4]\` + \`jmp sub_497620\`；0x4034e6 \`mov eax,[ecx]; mov eax,[eax+8]; jmp eax\` = **虚调用**），
       被 4 个类当 this 调用（T0 与其内嵌子对象 T_1ee8/T_4ed10/T_a609c）—— 真相是**通用基类 \`BaseObject\`**
       （RTTI 名 \`??_7BaseObject@@6B@\`）：构造器 \`sub_4034a0\` 在 0x4034a2 写 vtable、在 0x4034a8 写
       \`[this+4] = offset dword_55BF70\`（**全局输出对象**的地址），而它有 26 个调用方（全是各类的构造器，
       含 0x415640 的 \`sub_415640\`=Command）⇒ 子类在**偏移 0** 处就是 \`BaseObject\`，\`this\` 无需调整。
       ⇒ 声明成 \`T_BaseObject\`（\`adapter:true\` = 基类前缀：\`+0x0\` vtable · \`+0x4\` ptr→\`T_RemoteDebug\`）。
       ★ 传播规则因此是 **"声明 > 推断"**：被调方已有载体时，推断出的另一类只记 note（"调用方类是它的子类"），
       ⛔ 不记冲突；冲突只发生在**两侧都是推断**时。
两道守卫
  词汇  出现 ${VOCAB.join(' / ')} ⇒ 红（那是 B 层：data/ledger/，走 AGENTS §6 准入门）
  冗余  字段名出现 ${REDUNDANT.join(' / ')} ⇒ 红（这些都能从 EA + 语料机械读出）
  ★ 两道守卫跑在 **typesFrom 解析之后**的模型上 ⇒ 被引用的类型文件同样受守（否则守卫只守族文件、不守类型表）

诚实项
  · 二级对象**只下钻到层级 2**；层级 ≥3 单列"超出解析深度"（带 EA，不静默丢）
  · 守恒②里"目标类型解释"的那一档只覆盖**声明了 type 的指针成员**；不透明档是不可检查，不是通过
  · 观察集必须由调用方算（本模块不落缓存文件）⇒ 不传就退成字面量下界并**明说**
  · **视图（\`views\`）是模型的推测**：算术自洽由工具校验，**"它是同一个数组"这个判断不由本层背书** ——
    它只保证"要么被站点约束、要么被单列成空话声明"（可证伪性）
  · **多窗口同分**时选不出宿主 ⇒ 单列"ambiguous"（⛔ 不许靠"谁先声明"决定）
  · J4/J5/J6 三节本版只做结构检查；J3 只做"span == 导出段落"
  · **调用链一致性**（--calls）：A 传给 B 的 reg 与 B 的载体声明是否同一个类型 —— 三态 一致/不一致/单侧；
    不拿偏移相同顶替类型；两侧都解不出时不入账；栈上传参与间接调用不覆盖
  · **闭合度**（--coverage）：站点全绿**不等于**函数分析完了 —— L0 判据 = 没有未归类的内存行 ·
    没有目标未知的间接调用/跳转 · 没有基址未解析站点；L1 只点名直接调用与抛出，L2 归台账
  · **值传播的两个已知边界**（助手侧；改它要连带改守卫 ★⑪/★⑫）：
    ① \`noreturn\` 调用（闭集合只有 \`__CxxThrowException@8\`，语料里 288 处）之后**没有** fall-through；
       否则抛出路径上的写会污染汇合点后面的块（实测：\`sub_41C6A0\` 的 9 处访问整类丢掉）
    ② **栈槽只做值传播、不做站点**（\`mov [ebp+x],reg\` → 之后 \`mov reg,[ebp+x]\` 拿回同一个值）；
       ⛔ 栈槽本身永远不进任何桶（"栈槽不是本单的对象形状"这条不变）
    ⇒ 但 **SEH catch 块没有静态入边**（唯一引用在 \`.data\` 的 SEH 表里）⇒ 块内寄存器状态**解不出**，
    那里的访问只能靠 \`kind:"local"\` + \`type:"T0*"\` 表达（⛔ 不许编"匿名类型"把 obj0 变成假的）
`;
}

/** 形状层的 obj0 相对观察集（G6 的正确判据；约 1~2 s） */
function observedSet(lines) {
  const set = new Set();
  const roots = objRootRegs();
  for (const sp of functionSpans(lines)) {
    const { sites } = analyzeLines(lines, sp.from, sp.to, roots.get(sp.sym) ?? null, {});
    for (const s of sites) if (s.obj === 'Engine' && s.absDisp !== null) set.add(s.absDisp);
  }
  return set;
}

function runAccess(sym, opts = {}) {
  const lines = loadListing();
  const roots = objRootRegs();
  const sp = functionSpans(lines).find((s) => s.sym === sym);
  if (!sp) { process.stderr.write(`找不到函数 ${sym}\n`); return 2; }
  const { sites } = analyzeLines(lines, sp.from, sp.to, roots.get(sym) ?? null, opts);
  process.stdout.write(`# ${sym}（行 ${sp.from + 1}-${sp.to + 1}）  对象根寄存器=${roots.get(sym) ?? '（.c 约定里没有）'}  访问 ${sites.length} 处\n`);
  for (const s of sites) {
    process.stdout.write(`  ${hex(s.ea)}  ${String(s.mn).padEnd(6)} ${s.access} w=${String(s.width).padEnd(7)} base=${String(s.baseKind).padEnd(10)}`
      + ` obj=${String(s.obj ?? '?').padEnd(16)} abs=${s.absDisp === null ? '-' : hex(s.absDisp)}`
      + `${s.stride ? ` stride=${s.stride}` : ''}${s.indexSym ? ` idx=${s.indexSym}` : ''}`
      + `${s.pointeeHost !== null && s.pointeeHost !== undefined ? ` pointee=+${hex(s.pointeeHost)}+${hex(s.pointeeOff)}` : ''}`
      + ` level=${objectLevel(s)}`
      + `${s.problems?.length ? `  ⚠ ${s.problems.join(',')}` : ''}\n`);
  }
  return 0;
}

function runRegions(sym, reg) {
  const lines = loadListing();
  if (!insnsOf(lines, sym)) { process.stderr.write(`找不到函数 ${sym}\n`); return 2; }
  const f = insnsOf(lines, sym);
  const regs = reg
    ? [reg.toLowerCase()]
    : [...new Set(f.out.filter((x) => /^e[a-z]{2}$/.test((x.ops[0] ?? '').toLowerCase())).map((x) => x.ops[0].toLowerCase()))];
  process.stdout.write(`# ${sym}：**机械导出的拆分范围**（模型只能引用，不能改）\n`);
  for (const r of regs) {
    const d = deriveRegions(lines, sym, r);
    if (!d || d.writes.length === 0) continue;
    process.stdout.write(`\n## ${r}（写点 ${d.writes.length} 处 ⇒ ${d.regions.length} 个段落）\n`);
    for (const [i, g] of d.regions.entries()) {
      process.stdout.write(`  S${i + 1} [${hex(g.from)}..${hex(g.to)}] 活着到 ${hex(g.liveTo)} 类别=${String(g.cat).padEnd(5)}`
        + ` ${g.allConstOrInit ? '全 ±const/init' : '含非常量形态'}${g.openedByInit ? ' 段首是 init' : ' **段首不是 init**'}`
        + `${g.brokenByCall ? ' ⚠被调用打断' : ''}\n`);
      for (const w of g.writes) process.stdout.write(`        ${hex(w.ea)} 行${w.line}  ${String(w.kind).padEnd(7)} ${w.text}\n`);
    }
  }
  return 0;
}

/**
 * **对象层级分布**：level=1 对象就是 obj0；level=2 对象是"obj0 的成员里读出的指针"；3+ = 再往下。
 * ★ 层级 = `baseHop + 1`（`baseHop` 是**基址值的来路**，不是层级）—— 差这一层会让"层级 3"整类
 *   （实测 812 处）从"超出解析深度"里漏掉，而上限冻结在层级 2 时那正是必须被发现的东西。
 */
function runDepth(sym) {
  const lines = loadListing();
  const roots = objRootRegs();
  const all = functionSpans(lines);
  const fns = sym ? all.filter((s) => s.sym === sym) : all;
  if (!fns.length) { process.stderr.write(`找不到函数 ${sym}\n`); return 2; }
  const hist = new Map();
  let total = 0;
  for (const sp of fns) {
    const { sites } = analyzeLines(lines, sp.from, sp.to, roots.get(sp.sym) ?? null, {});
    for (const s of sites) {
      if (s.access === 'a') continue;                                   // 地址计算不算访问
      if (s.obj !== 'Engine' && !String(s.obj).startsWith('Engine+0x')) continue;
      const lvl = objectLevel(s);
      hist.set(lvl, (hist.get(lvl) ?? 0) + 1);
      total += 1;
    }
  }
  const withinCap = [...hist].filter(([k]) => k <= DRILL_MAX_LEVEL).reduce((a, [, v]) => a + v, 0);
  process.stdout.write(`# 对象层级分布${sym ? `（${sym}）` : '（全语料）'}：obj0 族访问 ${total} 处\n`);
  process.stdout.write('  层级含义：1 = 对象就是 obj0；2 = 对象是"obj0 的成员里读出的指针"；3+ = 指针的指针\n');
  for (const [k, v] of [...hist].sort((a, b) => a[0] - b[0])) process.stdout.write(`  level ${String(k).padEnd(3)} ${String(v).padStart(6)}  (${total ? ((v / total) * 100).toFixed(1) : '0'}%)\n`);
  process.stdout.write(`  ★ 下钻上限冻结在 level=${DRILL_MAX_LEVEL}：覆盖 ${withinCap}/${total}；层级 >${DRILL_MAX_LEVEL} 的由 \`claims\` 单列"超出解析深度"（带 EA），⛔ 不静默丢\n`);
  return 0;
}

/**
 * **全表普查**（只读）：handler 集合来自**语料的分派表**（桥），站点分类 / 偏移聚类来自**助手**。
 * ★ 为什么要它：单族 claims 只能回答"这一族碰了什么" —— **族边界本身要有依据**，
 *   而"哪些偏移被碰、被谁碰、多少处没解析"只有全表口径才答得出来（`docs/02-engine/shape-census.md` §7）。
 */
function runCensus(args) {
  const opt = (name) => { const i = args.indexOf(name); return i >= 0 ? (args[i + 1] ?? null) : null; };
  const lines = loadListing();
  const r = censusHandlers({ lines, repoRoot: REPO_ROOT, match: opt('--match') });
  process.stdout.write(args.includes('--json') ? `${JSON.stringify(r, null, 2)}\n` : renderCensus(r));
  return r.problems.length ? 1 : 0;
}

/**
 * **handler 体骨架普查**（只读）：记录槽写法 + 调用序列 ⇒ 按骨架分组。
 * ★ 这一段原来在 `shape-census.md` §10 里是一张 **A1–A6 义务表**（声明 + 校验）。
 *   审计结论：六条**全是能从 EA + 语料机械读出的观察** ⇒ 按硬口径不该做成声明（产物会变第二份真源），
 *   改成在这里**算出来**；真正需要判断的只剩"族边界"（= 这里的分组）。
 */
function runBodies(args) {
  const opt = (name) => { const i = args.indexOf(name); return i >= 0 ? (args[i + 1] ?? null) : null; };
  const r = censusBodies({ lines: loadListing(), repoRoot: REPO_ROOT, match: opt('--match') });
  process.stdout.write(args.includes('--json') ? `${JSON.stringify(r, null, 2)}\n` : renderBodies(r));
  return r.problems.length ? 1 : 0;
}

/** ★ 闭合度：一个/多个函数的指令级记账（回答"这函数分析完了吗"） */
function runCoverage(args) {
  const lines = loadListing();
  const spanOf = new Map(functionSpans(lines).map((s) => [s.sym, s]));
  const roots = objRootRegs();
  const all = args.includes('--all');
  const li = args.indexOf('--limit');
  const limit = li >= 0 ? Number(args[li + 1] ?? 40) : 40;
  const targets = all
    ? functionSpans(lines).map((s) => s.sym)
    : args.filter((a) => /^sub_[0-9A-F]{6}$/i.test(a));
  if (!targets.length) { process.stderr.write('用法：shape coverage <sub_XXXXXX>… | --all [--limit N]\n'); return 2; }
  let closed = 0; let unclosed = 0;
  const tally = new Map();
  for (const sym of targets.slice(0, all ? limit : targets.length)) {
    const sp = spanOf.get(sym);
    if (!sp) { process.stderr.write(`找不到函数 ${sym}\n`); continue; }
    const { sites } = analyzeLines(lines, sp.from, sp.to, roots.get(sym) ?? null, {});
    const cov = coverageOf(lines, sym, sp, roots.get(sym) ?? null, sites);
    // ★ 两档对照：把"保守档杀 call 后的 caller-saved"与"真的解不出"分开（§9 口径）
    {
      const cal = analyzeLines(lines, sp.from, sp.to, roots.get(sym) ?? null, { killCallerSavedOnCall: false });
      const covCal = coverageOf(lines, sym, sp, roots.get(sym) ?? null, cal.sites);
      cov.tierCompare = { conservative: cov.siteStates.unresolvedBase, calibrated: covCal.siteStates.unresolvedBase };
    }
    if (cov.closed) closed += 1;
    else {
      unclosed += 1;
      for (const r of cov.reasons) {
        const k = r.replace(/^\d+ /, '').replace(/：.*$/, '');
        tally.set(k, (tally.get(k) ?? 0) + 1);
      }
    }
    process.stdout.write(renderCoverage(cov) + '\n\n');
  }
  process.stdout.write(`## 合计：L0 闭合 ${closed} · 未闭合 ${unclosed}\n`);
  for (const [k, v] of [...tally].sort((a, b) => b[1] - a[1])) process.stdout.write(`  ${v} 个函数：${k}\n`);
  return 0;
}

/** ★ 调用点类型对账（跨族文件；回答"两侧引用的是不是同一个类型"） */
function runCalls(args) {
  const dir = 'data/shape';
  const models = [];
  let types = { types: {} };
  for (const f of fs.readdirSync(dir).filter((x) => x.endsWith('.json')).sort()) {
    const m = JSON.parse(fs.readFileSync(path.join(dir, f), 'utf8'));
    if (f === 'types.json' || m.types) { types = m.types ? m : types; continue; }
    models.push({ file: f, model: m });
  }
  const carrierOf = new Map();
  for (const { model } of models) {
    for (const c of model.carriers ?? []) {
      if (c.kind === 'arg' && c.reg) carrierOf.set(c.fn, (c.type ?? '').replace(/\*$/, ''));
    }
  }
  const fns = [...carrierOf.keys()];
  const lines = loadListing();
  const r = auditCalls({ lines, fns, models, types: types.types, selfTypeOf: (fn) => carrierOf.get(fn) ?? null });
  process.stdout.write(args.includes('--json') ? `${JSON.stringify(r, null, 2)}\n` : `${renderCalls(r)}\n`);
  return r.rows.some((x) => x.verdict === 'mismatch') ? 1 : 0;
}

/** ★ 全图类型传播：谁是什么类、下一批是谁 */
function runPropagate(args) {
  const lines = loadListing();
  const spanOf = new Map(functionSpans(lines).map((s) => [s.sym, s]));
  const roots = objRootRegs();
  const dir = 'data/shape';
  let types = {};
  const carriers = [];
  for (const f of fs.readdirSync(dir).filter((x) => x.endsWith('.json')).sort()) {
    const m = JSON.parse(fs.readFileSync(path.join(dir, f), 'utf8'));
    if (m.types) { types = m.types; continue; }
    for (const c of m.carriers ?? []) carriers.push(c);
  }
  const cache = new Map();
  const analyzeFn = (sym) => {
    if (cache.has(sym)) return cache.get(sym);
    const sp = spanOf.get(sym);
    const r = sp ? analyzeLines(lines, sp.from, sp.to, roots.get(sym) ?? null, {}) : null;
    cache.set(sym, r);
    return r;
  };
  // ★ **全局根**（`kind:"global"`）：根对象（Command / Engine）不是任何函数的参数 ——
  //   `_WinMain@16` 里 `operator new(0xAABC8)` → `call sub_415640` → `mov ds:dword_55E1BC, eax`（0x4baa11/2e/3a）。
  //   ⇒ "体内碰过这个全局"的函数是唯一能定出 `this=Command` 的入口；它们是文本级定位（机械），
  //     类型判定仍旧只交给 `propagate` 里那一条 `global` 规则（⛔ 这里不判类）。
  const globals = new Map();
  for (const c of carriers) if (c.kind === 'global' && c.sym && c.type) globals.set(c.sym, c.type.replace(/\*$/, ''));
  const extraCallers = [];
  if (globals.size) {
    const esc = [...globals.keys()].map((s) => s.replace(/[.*+?^${}()|[\]\\]/g, '\\$&'));
    const symRe = new RegExp(`ds:(?:${esc.join('|')})\\b`, 'i');
    const spans = [...spanOf.values()].sort((a, b) => a.from - b.from);
    const seen = new Set();
    let si = 0;
    for (let i = 0; i < lines.length && si < spans.length; i += 1) {
      while (si < spans.length && spans[si].to < i) si += 1;
      if (si >= spans.length) break;
      if (i < spans[si].from) continue;
      if (symRe.test(lines[i]) && !seen.has(spans[si].sym)) { seen.add(spans[si].sym); extraCallers.push(spans[si].sym); }
    }
  }
  const { inferred, linksSeen, conflicts, declaredMismatch } = propagate({
    carriers, types, analyzeFn, globals, extraCallers,
    // ★ 只往"根寄存器就是 ecx"的被调方传播：`__cdecl` 函数里的 ecx 是残留值，不是 this
    acceptTarget: (sym) => roots.get(sym) === 'ecx',
  });
  const next = nextCandidates({ inferred, carriers });
  if (args.includes('--json')) {
    process.stdout.write(`${JSON.stringify({ inferred: Object.fromEntries(inferred), next, linksSeen, conflicts, declaredMismatch }, null, 2)}\n`);
    return conflicts.length ? 1 : 0;
  }
  const byType = new Map();
  for (const [fn, v] of inferred) {
    if (!byType.has(v.type)) byType.set(v.type, []);
    byType.get(v.type).push(fn);
  }
  process.stdout.write(`# 类型传播（全图 BFS）：${inferred.size} 个函数的根类型可推导 · 扫过 ${linksSeen} 个调用点\n`);
  if (globals.size) {
    process.stdout.write(`  全局根 ${globals.size} 个（${[...globals].map(([s, t]) => `${s}: ${t}*`).join(' · ')}）`
      + ` ⇒ 根锚定扫描种子 ${extraCallers.length} 个函数${extraCallers.length ? `（${extraCallers.slice(0, 6).join(' ')}${extraCallers.length > 6 ? ' …' : ''}）` : ''}\n`);
  }
  for (const [ty, fns] of [...byType].sort((a, b) => b[1].length - a[1].length)) {
    process.stdout.write(`  ${ty}: ${fns.length} 个函数${fns.length <= 8 ? '  ' + fns.join(' ') : '  ' + fns.slice(0, 8).join(' ') + ' …'}\n`);
  }
  process.stdout.write(`\n★ 下一批候选（已被传播确定类型、尚无载体）：${next.length} 个\n`);
  for (const n of next.slice(0, args.includes('--all') ? next.length : 24)) {
    process.stdout.write(`  ${n.fn} → ${n.type}（${n.hops} 跳，来自 ${n.via}）\n`);
  }
  if (conflicts.length) {
    // ★ **按被调方分组报**：一个函数被推出两种类，只有两类解释 ——
    //   ① 它是**通用/转发助手**（`sub_4034C0`：`mov ecx,[ecx+4]; jmp sub_497620`，任何类的 `+4` 都能喂它）
    //      ⇒ 结论是"**不许给它声明载体**"，不是"某处声明错了"；
    //   ② 真的有一处声明错（那要具体到 EA 才能判）。
    //   只按单条 EA 平铺会让人以为有 84 处错误；按目标分组才能看出是 5 个通用助手。
    const byTarget = new Map();
    for (const c of conflicts) {
      const g = byTarget.get(c.target) ?? { classes: new Set(), sites: [] };
      g.classes.add(c.had); g.classes.add(c.now); g.sites.push(c);
      byTarget.set(c.target, g);
    }
    process.stdout.write(`\n⚠ ${conflicts.length} 处**传播冲突**，分布在 ${byTarget.size} 个被调方上`
      + `（同一函数被推出两种类，且它**没有**载体声明 ⇒ 要么某处声明错，要么它是适配器/基类，该给它一条声明）：\n`);
    const ranked = [...byTarget].sort((a, b) => b[1].sites.length - a[1].sites.length);
    for (const [target, g] of ranked.slice(0, 12)) {
      process.stdout.write(`  ${target}：${g.classes.size} 种类 [${[...g.classes].join(' / ')}]，${g.sites.length} 处`
        + `　例：${g.sites.slice(0, 3).map((c) => `${c.from}@0x${c.ea.toString(16)}`).join(' ')}\n`);
    }
    if (ranked.length > 12) process.stdout.write(`  …（另有 ${ranked.length - 12} 个被调方；--json 看全量）\n`);
  }
  if (declaredMismatch?.length) {
    // ★ **声明 > 推断**：被调方已有载体，推断出的另一类不算冲突 —— 它是"这个被调方不是调用方那一类的实例方法"，
    //   最典型的是 `this->成员` 的转发器（`mov ecx,[ecx+4]; jmp …`，甚至是虚调用）。
    //   ⛔ 这里不红、也不许悄悄吞掉：逐被调方点名，让人看见"有多少调用方类在这一偏移上相容"。
    const byT = new Map();
    for (const d of declaredMismatch) {
      const g = byT.get(d.target) ?? { declared: d.declared, implied: new Set(), sites: [] };
      g.implied.add(d.implied); g.sites.push(d);
      byT.set(d.target, g);
    }
    process.stdout.write(`\n· ${declaredMismatch.length} 处"推断与**已声明载体**不一致"，分布在 ${byT.size} 个被调方上`
      + `（⇒ 声明优先，不是冲突；这些被调方是**适配器/基类**，调用方类在它的偏移上相容）：\n`);
    for (const [target, g] of [...byT].sort((a, b) => b[1].sites.length - a[1].sites.length).slice(0, 8)) {
      process.stdout.write(`  ${target}：声明 ${g.declared} ← 被这些类当 ${[...g.implied].join(' / ')} 调用（${g.sites.length} 处）\n`);
    }
  }
  // ★ 传播冲突是**诊断**（同一被调方收到两种类 ⇒ 通用函数 / 某处声明错），不是验收判据：默认 exit 0
  return args.includes('--strict') && conflicts.length ? 1 : 0;
}

function runClaims(target, opts = {}) {
  const lines = loadListing();
  let model;
  let baseDir = process.cwd();
  let title = 'claims-check';
  if (target === '--sample' || target === undefined) {
    model = sampleModel();
    title = 'claims-check（内置样本）';
  } else {
    const file = path.resolve(target);
    model = JSON.parse(fs.readFileSync(file, 'utf8'));
    baseDir = path.dirname(file);
    title = `claims-check：${path.basename(file)}`;
  }
  const rep = checkClaims(model, { lines, baseDir, observed: observedSet(lines) });
  process.stdout.write(renderReport(rep, { title, byType: opts.byType === true }));
  // ★ **闭合度**：站点全绿 ≠ 函数分析完了 —— 载体函数逐个给 L0 判据，未闭合的点名原因
  {
    const spanOf = new Map(functionSpans(lines).map((s) => [s.sym, s]));
    const roots = objRootRegs();
    const fns = [...new Set((model.carriers ?? []).filter((c) => c.kind !== 'global').map((c) => c.fn))];
    const bad = [];
    let closedN = 0;
    for (const fn of fns) {
      const sp = spanOf.get(fn);
      if (!sp) continue;
      const { sites: ss } = analyzeLines(lines, sp.from, sp.to, roots.get(fn) ?? null, {});
      const cov = coverageOf(lines, fn, sp, roots.get(fn) ?? null, ss);
      if (cov.closed) { closedN += 1; continue; }
      bad.push(`  ❌ ${fn}：${cov.reasons.join('；')}`);
    }
    process.stdout.write(`\n## 闭合度（**站点全绿 ≠ 函数分析完了**）：${fns.length} 个载体函数，L0 闭合 ${closedN} · 未闭合 ${fns.length - closedN}\n`);
    for (const l of bad.slice(0, 12)) process.stdout.write(l + '\n');
    if (bad.length > 12) process.stdout.write(`  …（另有 ${bad.length - 12} 个；跑「pnpm tools shape coverage --all」看全量）\n`);
  }
  return rep.findings.length ? 1 : 0;
}

export async function main(argv = process.argv.slice(2)) {
  const [flag, ...rest] = argv;
  const pos = rest.filter((a) => !a.startsWith('--'));
  switch (flag) {
    case '--access': return runAccess(pos[0], argv.includes('--calibrated') ? { killCallerSavedOnCall: false } : {});
    case '--verify': {
      const ea = Number.parseInt(pos[0] ?? '', 16);
      const raw = loadRaw();
      const d = ea ? describeEa(raw, ea) : null;
      process.stdout.write(`${JSON.stringify(d ?? checkRow(raw, {}), null, 2)}\n`);
      return d?.ok === false ? 1 : 0;
    }
    case '--regions': return runRegions(pos[0], pos[1]);
    case '--depth': return runDepth(pos[0]);
    case '--census': return runCensus(argv);
    case '--coverage': return runCoverage(argv);
    case '--calls': return runCalls(argv);
    case '--propagate': return runPropagate(argv);
    case '--bodies': return runBodies(argv);
    case '--claims': return runClaims(pos[0], { byType: argv.includes('--by-type') });
    case '--describe': process.stdout.write(describeText()); return 0;
    default:
      process.stderr.write(`未知参数：${argv.join(' ')}\n可用：--access <sym> · --verify <EA> · --regions <sym> [reg] · --depth [sym] · --census [--match <sym>] [--json] · --claims <file>|--sample · --describe\n`);
      return 2;
  }
}

const isDirectRun = process.argv[1] && path.resolve(process.argv[1]) === path.resolve(new URL(import.meta.url).pathname.replace(/^\/([A-Za-z]:)/, '$1'));
if (isDirectRun) {
  main()
    .then((code) => { process.exitCode = code ?? 0; })
    .catch((err) => { process.stderr.write(`ERROR: ${err.stack ?? err.message}\n`); process.exitCode = 2; });
}
