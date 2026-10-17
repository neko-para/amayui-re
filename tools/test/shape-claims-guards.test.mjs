/** @env pure @kind gate @why 类型引用的守卫被绕过（typesFrom 里的违规字段静默通过）· 站点分桶静默丢类 · 成员声明退化成 1 字节 · 空话声明被当通过 */
/**
 * tools/test/shape-claims-guards.test.mjs —— claims 校验器的**四类判据**在**合成 `.lst`** 上的守卫
 *
 * 为什么是 `pure`：`checkClaims` 的两处语料接缝（`roots` = `.c` 的调用约定、`observed` = 形状层观察集）
 * 都可以**显式传入** ⇒ 本文件不读语料、不写盘，能在默认 `pnpm test` 里跑。
 * （真语料上的义务检查仍在 `shape-claims.assets.test.mjs`。）
 *
 * 钉住的四件事（每一件都是实测踩出来的）：
 *  ① **两道守卫必须跑在 `typesFrom` 解析之后的模型上** —— 否则被引用的类型文件里塞 `argc` / `mn` 时
 *     守卫**一起静默失效**（实测：引用文件带两个违规字段，`claims` 仍然退出码 0）；
 *  ② **守恒③：站点按基址分桶** —— `ds:` 符号与"基址没解出来"的站点曾经从访问总数里**静默消失**；
 *  ③ **成员声明自洽（G8）** —— `{off,type}`（缺 count）或数组成员缺元素 `size` 时，跨度算不出，
 *     判定器会把它**静默当成 1 字节槽**（实测：帧数组访问变成"不落在任何已声明成员上"的残差，却一条红都没有）；
 *  ④ **可证伪性（G7）** —— 声明 `kind:"ptr"` 却在载体集里零解引用 ⇒ 必须**单列**（⛔ 不算通过），
 *     而不是静默绿；反过来"标量被当基址解引用"必须红（G5'）。
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';

import { checkClaims } from '../lib/shape-claims.mjs';
import { deriveRegions } from '../lib/shape-regions.mjs';

/** 合成一份最小 `.lst`：与真语料的文法一致（`段:EA 助记符 操作数`） */
const L = [
  '.text:00401000 sub_401000 proc near',
  '.text:00401000 mov     esi, ecx',
  '.text:00401003 mov     eax, [esi+5D818h]',   // ← 成员 0x5d818（读）
  '.text:00401009 mov     edx, [eax+4]',        // ← 以它的值为基址 ⇒ **二级对象**（G5'/G7 的素材）
  '.text:0040100F mov     edi, [esi+5D880h]',   // ← 成员 0x5d880（G7 的空话声明素材）
  '.text:00401015 mov     eax, [esi+5D8F4h]',   // ← 数组元素内 0x60（= 0x5d8f4 − 0x5d894）
  '.text:0040101B mov     eax, [esi+5EB54h]',   // ← 数组末尾字面量（G4 的依据）
  '.text:00401021 mov     eax, ds:dword_559A68',// ← **数据段符号**（守恒③ 的数据段符号桶）
  '.text:00401027 mov     eax, [ebx+4]',        // ← `ebx` 从未定义 ⇒ **未解析基址**桶
  '.text:0040102D mov     [esi+5D818h], eax',   // ← 0x5d818（写）
  '.text:00401033 retn',
  '.text:00401033 sub_401000 endp',
];
const OBS = new Set([0x5d818, 0x5d880, 0x5d8f4, 0x5eb54]);
const ROOTS = new Map([['sub_401000', 'ecx'], ['sub_402000', 'ecx'], ['sub_403000', 'ecx'], ['sub_404000', 'ecx']]);

const base = (t0members, extraTypes = {}) => ({
  schema: 'amayui.claims/1',
  obj0: { isGlobal: true },
  types: {
    T0: { size: null, complete: false, members: t0members },
    T1: { size: 120, complete: false, members: [{ off: '0x60', width: 4 }] },
    ...extraTypes,
  },
  carriers: [{ id: 'g01', kind: 'arg', fn: 'sub_401000', reg: 'ecx', type: 'T0*' }],
});
/** 默认模型：0x5d818 声明成 ptr→TB，二级偏移 0x4 由 TB 解释 */
const TB = { size: 8, complete: false, members: [{ off: '0x0', width: 4 }, { off: '0x4', width: 4 }] };
const DEF = () => base([
  { off: '0x5d818', width: 4, kind: 'ptr', type: 'TB' },
  { off: '0x5d880', width: 4 },
  { off: '0x5d894', type: 'T1', count: 40 },
], { TB });
const run = (m) => checkClaims(m, { lines: L, baseDir: '.', observed: OBS, roots: ROOTS });

test('前提：默认模型与合成语料自洽（解释 / 二级 / 两条桶都在）', () => {
  const rep = run(DEF());
  assert.deepEqual(rep.findings, [], `不该有红：${JSON.stringify(rep.findings)}`);
  assert.equal(rep.perCarrier[0].pointeeOk, 1, '0x5d818 的二级访问应由 TB+0x4 解释');
  assert.equal(rep.cons.symbol, 1, '`ds:dword_559A68` 必须进数据段符号桶');
  assert.equal(rep.cons.unresolvedBase, 1, '`[ebx+4]`（ebx 未定义）必须进未解析基址桶');
});

test('★① 两道守卫必须覆盖 `typesFrom` 引用的那份文件（接缝注入 ⇒ 不写盘也能测）', () => {
  const model = { schema: 'amayui.claims/1', typesFrom: 'X.json', carriers: [{ id: 'g01', kind: 'arg', fn: 'sub_401000', reg: 'ecx', type: 'T0*' }] };
  const ref = { schema: 'amayui.claims/1', obj0: { isGlobal: true }, types: { T0: { size: null, complete: false, members: [{ off: '0x5d818', width: 4, argc: 3, mn: 'add' }] } } };
  const rep = checkClaims(model, { lines: L, baseDir: '.', observed: OBS, roots: ROOTS, resolve: () => ref });
  assert.ok(rep.findings.some((f) => f.where === '守卫·词汇' && f.why.includes('argc')), `引用文件里的 argc 必须被报出：${JSON.stringify(rep.findings)}`);
  assert.ok(rep.findings.some((f) => f.where === '守卫·冗余' && f.why.includes('mn')), `引用文件里的 mn 必须被报出：${JSON.stringify(rep.findings)}`);
});

test('★② 守恒③：站点按基址分桶 —— `ds:` 符号与未解析基址不许从总数里消失', () => {
  const { cons } = run(DEF());
  const sum = cons.obj0Acc + cons.partial + cons.pointee + cons.local + cons.symbol + cons.unresolvedBase;
  assert.equal(cons.sites, sum, `站点总数必须等于六桶之和（${cons.sites} vs ${sum}）`);
  assert.ok(cons.symbol >= 1 && cons.unresolvedBase >= 1, '两类都必须真的入桶（否则等式恒真、哨兵失效）');
});

test('★② 守恒③ 抓得住"从所有桶里掉出去"的第三类：`lea [<二级对象>+k]` 之后再解引用', () => {
  // ★ 实测：守恒③ 上线**当天**就因为这一条对不上（128 ≠ 127）——`addr` 基址拿不到宿主偏移 ⇒ `pointeeHost=null`
  //   ⇒ 它既不算 obj0、也不算二级对象、也不算 deep。这里把它钉成独立一桶。
  const L2 = [
    '.text:00402000 sub_402000 proc near',
    '.text:00402000 mov     esi, ecx',
    '.text:00402003 mov     eax, [esi+5D818h]',  // eax = 二级对象指针（路径 [0x5d818]）
    '.text:00402009 lea     edx, [eax]',         // ← 取址：**路径必须跟着走**
    '.text:0040200D mov     ebx, [edx+4]',       // 访问 = 二级对象 + 0x4
    '.text:00402013 retn',
    '.text:00402013 sub_402000 endp',
  ];
  const TBm = { size: 8, complete: false, members: [{ off: '0x0', width: 4 }, { off: '0x4', width: 4 }] };
  const m = base([{ off: '0x5d818', width: 4, kind: 'ptr', type: 'TB' }, { off: '0x5d880', width: 4 }], { TB: TBm });
  m.carriers[0].fn = 'sub_402000';
  const rep = checkClaims(m, { lines: L2, baseDir: '.', observed: OBS, roots: ROOTS });
  const p = rep.perCarrier[0];
  assert.deepEqual(rep.findings, [], `不该有红：${JSON.stringify(rep.findings)}`);
  assert.equal(p.pointee, 2, '取址一处 + 解引用一处，都要算进"二级对象"');
  assert.equal(p.pointeeOk, 2, '两处都要被 TB 的成员表解释');
  assert.equal(p.pointeeMulti, 0, '都是**一跳**，不许进"跨两级以上"档');
  const sum = rep.cons.obj0Acc + rep.cons.partial + rep.cons.pointee + rep.cons.local + rep.cons.symbol + rep.cons.unresolvedBase;
  assert.equal(rep.cons.sites, sum, `守恒③ 必须闭合（${rep.cons.sites} vs ${sum}）`);
});

test('★② 三跳链**不判**：进"跨两级以上（未判）"档（带路径 + EA），⛔ 不许认错层去硬判', () => {
  // 结构简化（2026-10）：路径只管**一跳**（推导）。跨两级以上不遍历 —— 要判就用**局部类型载体**断言。
  // 早先的坏行为：三跳链只带"最后一跳的偏移" ⇒ 宿主被拿去对 obj0 的成员表 = **认错层**（可能假解释）。
  const L3 = [
    '.text:00402500 sub_402500 proc near',
    '.text:00402500 mov     esi, ecx',
    '.text:00402503 mov     eax, [esi+5D818h]',  // 一层：指针 → TB
    '.text:00402509 mov     edx, [eax+0h]',      // 二层：TB+0x0 是指针 → TC
    '.text:0040250C mov     ebx, [edx+30h]',     // 三层：TC+0x30（**跨两级以上**）
    '.text:0040250F retn',
    '.text:0040250F sub_402500 endp',
  ];
  const TC = { size: 0x40, complete: false, members: [{ off: '0x30', width: 4 }] };
  const TBm = { size: 8, complete: false, members: [{ off: '0x0', width: 4, kind: 'ptr', type: 'TC' }] };
  const m = base([{ off: '0x5d818', width: 4, kind: 'ptr', type: 'TB' }, { off: '0x5d880', width: 4 }], { TB: TBm, TC });
  m.carriers[0].fn = 'sub_402500';
  const roots = new Map([['sub_402500', 'ecx']]);
  const rep = checkClaims(m, { lines: L3, baseDir: '.', observed: OBS, roots });
  const p = rep.perCarrier[0];
  assert.deepEqual(rep.findings, [], `不判就不该有红：${JSON.stringify(rep.findings)}`);
  assert.equal(rep.cons.pointeeMulti, 1, '跨两级以上的那一处进专门一档');
  assert.equal(p.pointeeMulti, 1);
  assert.deepEqual(p.multiHopPaths, ['+0x5d818 -> +0x0'], '必须报得出**整条路径**（⛔ 不许只给最后一跳）');
  assert.equal(rep.cons.pointeeOk, 1, '一跳那一处（`mov edx,[eax]`）照常判');
  assert.equal(rep.cons.pointeeBad, 0, '⛔ 不许把"跨两级以上"算成"声明错了"');
});

test('★② 未解析要报到能处置的粒度：按基址寄存器分组 + 给样例 EA', () => {
  const p = run(DEF()).perCarrier[0];
  assert.deepEqual(p.unresolvedByReg, [['ebx', 1]], `未解析基址必须按寄存器分组：${JSON.stringify(p.unresolvedByReg)}`);
  assert.deepEqual(p.unresolvedEAs.map((e) => `0x${e.toString(16)}`), ['0x401027'], '未解析基址要给得出 EA');
  assert.deepEqual(p.symbolSyms, ['dword_559A68'], '数据段符号要给得出符号名');
});

test('★③ G8：`type` 缺 `count` ⇒ 红（否则静默退化成 1 字节槽）', () => {
  const rep = run(base([{ off: '0x5d818', width: 4, kind: 'ptr', type: 'TB' }, { off: '0x5d880', width: 4 }, { off: '0x5d894', type: 'T1' }], { TB }));
  assert.ok(rep.findings.some((f) => f.where.startsWith('G8') && f.why.includes('count')), `应报 G8，实际 ${JSON.stringify(rep.findings.map((f) => f.where))}`);
});

test('★③ G8：数组成员给了 count 但元素类型没给 size ⇒ **明说**"按 1 字节处理"（不静默）', () => {
  const m = DEF();
  m.types.T1.size = null;
  const rep = run(m);
  assert.ok(rep.notes.some((n) => n.startsWith('G8') && n.includes('1 字节')), `必须明说退化成 1 字节：${JSON.stringify(rep.notes)}`);
  assert.ok(rep.notes.some((n) => n.startsWith('G4') && n.includes('边界判不了')), 'G4 也必须说判不了');
  // 跨度未知 ⇒ 数组整片解释不了：0x5d8f4（元素内 0x60）与 0x5eb54 都落不进去 —— 这正是"静默退化"的证据
  assert.deepEqual(rep.perCarrier[0].residue.map((r) => r.abs), [0x5d8f4, 0x5eb54], '跨度未知 ⇒ 帧访问变成残差');
});

test('★③ G8：`kind` 是闭集合（只有 "ptr" 有意义）', () => {
  const m = DEF();
  m.types.T0.members[1].kind = 'scalar';
  const rep = run(m);
  assert.ok(rep.findings.some((f) => f.where.startsWith('G8') && f.why.includes('kind')), '未知 kind 必须红');
});

test('★④ G5\'：**没声明 ptr** 的成员被当基址解引用 ⇒ 红（"标量不许再被解引用"）', () => {
  const m = DEF();
  m.types.T0.members[0] = { off: '0x5d818', width: 4 };       // 去掉 kind:"ptr"
  const rep = run(m);
  assert.ok(rep.findings.some((f) => f.where.startsWith("G5'") && f.why.includes('kind:"ptr"')), `应报 G5'，实际 ${JSON.stringify(rep.findings.map((f) => f.where))}`);
});

test('★④ G7：声明了 `kind:"ptr"` 却零解引用 ⇒ **单列"空话声明"**，但不判红（类型共享）', () => {
  const m = DEF();
  m.types.T0.members[1] = { off: '0x5d880', width: 4, kind: 'ptr', type: 'TB' };  // 从不拿它当基址
  const rep = run(m);
  assert.deepEqual(rep.findings, [], 'G7 不判红（别的族文件可能才用它）');
  assert.equal(rep.unfalsifiable.length, 1, '必须单列出来 —— 空话声明不许静默绿');
  assert.equal(rep.unfalsifiable[0].off, 0x5d880);
  assert.equal(rep.cons.unfalsifiable, 1, '计数要进 cons（报告与断言同源）');
});

test('★⑤ 索引没解出来的访问**不许当偏移观察**（否则会编出假观察 / 假解释）', () => {
  // ★ 实测：handler 子集里 `index` 出问题的 310 处中，**175 处的基址是解出来的**。
  //   `mov [esi+edx*4], eax`（edx 未知）报出来的是 `abs=0x0` —— 那**少了索引那一项**：
  //   拿它当"偏移 0"的观察 ⇒ 轻则多一条残差，重则"正好命中某个成员" ⇒ **假解释**。
  const L3 = [
    '.text:00403000 sub_403000 proc near',
    '.text:00403000 mov     esi, ecx',
    '.text:00403003 mov     [esi+edx*4], eax',   // edx 从未定义 ⇒ 基址在、索引没解出来
    '.text:00403006 retn',
    '.text:00403006 sub_403000 endp',
  ];
  // 故意在 0x0 声明一个成员：如果这一处仍被算成"位移 0 的访问"，它就会被**假解释**成这条成员
  const m = base([{ off: '0x0', width: 4 }]);
  m.carriers[0].fn = 'sub_403000';
  const rep = checkClaims(m, { lines: L3, baseDir: '.', observed: OBS, roots: ROOTS });
  const p = rep.perCarrier[0];
  assert.equal(p.indexUnusable, 1, '索引未解析的访问要另计一档');
  assert.equal(rep.cons.partialIndexUnresolved, 1);
  assert.ok(p.explained.every((e) => e.abs !== 0), `不许被假解释成成员 0x0：${JSON.stringify(p.explained)}`);
  assert.ok(p.residue.every((r) => r.abs !== 0), '也不许变成一条"偏移 0"的残差');
  assert.equal(p.total, 0, 'obj0 偏移观察里不许有它');
  const sum = rep.cons.obj0Acc + rep.cons.partial + rep.cons.pointee + rep.cons.local + rep.cons.symbol + rep.cons.unresolvedBase;
  assert.equal(rep.cons.sites, sum, '剔出去之后守恒③ 仍要闭合');
});

test('★⑤ 反向：`lea` 取址（索引已解出）与**真在偏移 0 上的读写**仍算观察', () => {
  const L4 = [
    '.text:00404000 sub_404000 proc near',
    '.text:00404000 mov     esi, ecx',
    '.text:00404003 lea     eax, [esi]',        // 取 obj0 的地址：位移是可用的（偏移 0）
    '.text:00404006 mov     edx, [esi]',        // 真读偏移 0
    '.text:00404009 retn',
    '.text:00404009 sub_404000 endp',
  ];
  const m = base([{ off: '0x0', width: 4 }]);
  m.carriers[0].fn = 'sub_404000';
  const rep = checkClaims(m, { lines: L4, baseDir: '.', observed: OBS, roots: ROOTS });
  const p = rep.perCarrier[0];
  assert.equal(p.total, 2, '两处都应算 obj0 偏移观察（一处 lea 取址、一处真读）');
  assert.equal(p.explained.length, 2, '两处都应被成员 0x0 解释');
  assert.equal(p.indexUnusable, 0);
});

/* ───────────────────────── 视图（模型的推测）与"条数未观察"的数组 ───────────────────────── */

/** `索引寄存器 = *(obj0+0x5d880) − 1`，步长 4：`[esi+edx*4+5D894h]` ⇒ 助手报 `abs=0x5d890`（= 基址 − 4） */
const L5 = [
  '.text:00405000 sub_405000 proc near',
  '.text:00405000 mov     esi, ecx',
  '.text:00405003 mov     eax, [esi+5D880h]',
  '.text:00405009 dec     eax',
  '.text:0040500A mov     edx, eax',
  '.text:0040500C mov     edi, [esi+edx*4+5D894h]',   // = 数组[索引−1] 元素内 0x0
  '.text:00405013 ; 数组末尾 5D8A4h（G4 的依据；不是指令）',
  '.text:00405013 retn',
  '.text:00405013 sub_405000 endp',
];
const ROOTS5 = new Map([['sub_405000', 'ecx']]);
/** 数组：`0x5d894` 起 4 条、元素类型 T1（size 4），可带 `views` */
const arrModel = (views) => {
  const m = base([{ off: '0x5d880', width: 4 }, { off: '0x5d894', type: 'T1', count: 4, ...(views ? { views } : {}) }]);
  m.types.T1 = { size: 4, complete: false, members: [{ off: '0x0', width: 4 }] };
  m.carriers[0].fn = 'sub_405000';
  return m;
};

test('★⑥ 声明的视图（推测）：`索引差 −1` 让"数组前一条"的访问被解释，且**标明是推断**', () => {
  const rep = checkClaims(arrModel([{ elementDelta: -1 }]), { lines: L5, baseDir: '.', observed: OBS, roots: ROOTS5 });
  const p = rep.perCarrier[0];
  assert.deepEqual(rep.findings, [], `不该有红：${JSON.stringify(rep.findings)}`);
  assert.equal(p.viaView, 1, '这一处是**靠视图**解释的');
  assert.equal(p.viaViewEAs[0].ea, 0x40500c);
  assert.equal(p.viaViewEAs[0].delta, -1);
  assert.equal(rep.cons.obj0ExplainedObserved, 1, '另一处是观测档');
  assert.equal(rep.cons.obj0Explained, rep.cons.obj0ExplainedObserved + rep.cons.obj0ExplainedViaView, '守恒④：解释 = 观测档 + 视图档');
});

test('★⑥ 不声明视图时，同一处必须**如实进残差**（而不是被静默圆成"元素 0"）', () => {
  const rep = checkClaims(arrModel(null), { lines: L5, baseDir: '.', observed: OBS, roots: ROOTS5 });
  const p = rep.perCarrier[0];
  assert.equal(p.viaView, 0);
  assert.ok(p.residue.some((r) => r.abs === 0x5d890), `应有一条 0x5d890 的残差：${JSON.stringify(p.residue)}`);
});

test('★⑥ 视图**零观察** ⇒ 空话声明（单列，⛔ 不算通过），但**不判红**（类型共享）', () => {
  const rep = checkClaims(arrModel([{ elementDelta: -3 }]), { lines: L5, baseDir: '.', observed: OBS, roots: ROOTS5 });
  assert.deepEqual(rep.findings, [], 'G9 不判红');
  assert.ok(rep.unfalsifiable.some((u) => u.delta === -3 && u.why.includes('零观察')), `应单列"零观察"：${JSON.stringify(rep.unfalsifiable)}`);
});

test('★⑥ 视图**多余**（范围已被 delta 0 覆盖）⇒ 也进空话声明（不提供新信息）', () => {
  const rep = checkClaims(arrModel([{ elementDelta: 1 }]), { lines: L5, baseDir: '.', observed: OBS, roots: ROOTS5 });
  assert.ok(rep.unfalsifiable.some((u) => u.delta === 1 && u.why.includes('多余')), `应单列"多余"：${JSON.stringify(rep.unfalsifiable)}`);
});

test('★⑥ 视图的算术必须自洽（`member.off + stride·delta == 被解释的那段位移`）', () => {
  const rep = checkClaims(arrModel([{ elementDelta: -1 }]), { lines: L5, baseDir: '.', observed: OBS, roots: ROOTS5 });
  const v = rep.declaredViews[0];
  assert.equal(v.from, 0x5d894 + 4 * -1, '视图窗口起点必须等于 base + stride·delta');
  assert.equal(v.to, v.from + 4);
  assert.equal(rep.perCarrier[0].explained.find((e) => e.viaView).abs, 0x5d890, '被解释的位移必须落在视图窗口内');
});

test('★⑦ 非索引站点落在"标量成员"与"视图窗口"重叠处 ⇒ **选标量**（规则，不靠声明顺序）', () => {
  const L = [
    '.text:00406000 sub_406000 proc near',
    '.text:00406000 mov     esi, ecx',
    '.text:00406003 mov     edi, [esi+5D890h]',   // 裸位移，正好也在"数组 −1 条"的窗口里
    '.text:00406009 retn',
    '.text:00406009 sub_406000 endp',
  ];
  const m = arrModel([{ elementDelta: -1 }]);
  m.types.T0.members = [{ off: '0x5d880', width: 4 }, { off: '0x5d890', width: 4 }, { off: '0x5d894', type: 'T1', count: 4, views: [{ elementDelta: -1 }] }];
  m.carriers[0].fn = 'sub_406000';
  const rep = checkClaims(m, { lines: L, baseDir: '.', observed: OBS, roots: new Map([['sub_406000', 'ecx']]) });
  const p = rep.perCarrier[0];
  assert.equal(p.explained.length, 1);
  assert.equal(p.explained[0].member.offN, 0x5d890, '裸位移要落在**标量成员**上，不是被视图吸走');
  assert.equal(p.viaView, 0, '这一处不算推测档');
  assert.equal(rep.ambiguous.length, 0, '不是"选不出来"：规则能定');
});

test('★⑨ 局部类型载体：**寄存器 → 类型**的断言（span = `regions` 的活窗口）能把"层级 3"判掉', () => {
  // 为什么要有它：路径只能表达"从 obj0 的某个成员读出来的指针"；指针还可能来自**参数 / 调用返回值 / 数据段**
  // —— 那些位置路径没有任何东西可表达。用户的原话："这里本质上是两个独立解引用逻辑"。
  const L = [
    '.text:00408000 sub_408000 proc near',
    '.text:00408000 mov     esi, ecx',
    '.text:00408003 mov     edx, [esi+5D800h]',   // edx 装的是"某个对象的指针"
    '.text:00408009 mov     eax, [edx+4]',        // ★ 打在那个对象 +0x4
    '.text:0040800C mov     eax, [edx+8]',        // ★ +0x8
    '.text:0040800F retn',
    '.text:0040800F sub_408000 endp',
  ];
  const reg = deriveRegions(L, 'sub_408000', 'edx');
  assert.equal(reg.regions.length, 1, 'edx 只有一个段落');
  const r0 = reg.regions[0];
  assert.equal(r0.cat, 'load');
  assert.equal(r0.brokenByCall, false, '窗口里没有 call');

  const m = base([{ off: '0x5d800', width: 4 }], { TL: { size: null, complete: false, members: [{ off: '0x4', width: 4 }, { off: '0x8', width: 4 }] } });
  m.types.T0.members = [{ off: '0x5d800', width: 4 }];
  m.carriers = [{ id: 'l01', kind: 'local', fn: 'sub_408000', reg: 'edx', span: [`0x${r0.from.toString(16)}`, `0x${r0.liveTo.toString(16)}`], type: 'TL*' }];
  const rep = checkClaims(m, { lines: L, baseDir: '.', observed: OBS, roots: new Map([['sub_408000', 'ecx']]) });
  assert.deepEqual(rep.findings, [], `不该有红：${JSON.stringify(rep.findings)}`);
  assert.equal(rep.cons.local, 2, '两处都在活窗口里 ⇒ 归局部桶');
  assert.equal(rep.cons.localOk, 2, '两处都应被 TL 的成员表解释');
  assert.equal(rep.localCarriers[0].cat, 'load');
});

test('★⑨ 局部载体：span **必须等于** `regions` 的活窗口（⛔ 不许自己写）', () => {
  const L = [
    '.text:00408100 sub_408100 proc near',
    '.text:00408100 mov     esi, ecx',
    '.text:00408103 mov     edx, [esi+5D800h]',
    '.text:00408109 mov     eax, [edx+4]',
    '.text:0040810C retn',
    '.text:0040810C sub_408100 endp',
  ];
  const m = base([{ off: '0x5d800', width: 4 }], { TL: { size: null, complete: false, members: [{ off: '0x4', width: 4 }] } });
  m.carriers = [{ id: 'l01', kind: 'local', fn: 'sub_408100', reg: 'edx', span: ['0x408103', '0x408109'], type: 'TL*' }];
  const rep = checkClaims(m, { lines: L, baseDir: '.', observed: OBS, roots: new Map([['sub_408100', 'ecx']]) });
  assert.ok(rep.findings.some((f) => f.where.startsWith('G10') && f.why.includes('活窗口')), `span 不是活窗口 ⇒ 必须红：${JSON.stringify(rep.findings)}`);
});

test('★⑨ 局部载体：caller-saved 的活窗口里被 `call` 打断 ⇒ 红（值可能已被抹掉，断言不成立）', () => {
  const L = [
    '.text:00408200 sub_408200 proc near',
    '.text:00408200 mov     esi, ecx',
    '.text:00408203 mov     edx, [esi+5D800h]',
    '.text:00408209 call    sub_999999',          // ← edx 是 caller-saved，这里之后它可能已经不是那个指针了
    '.text:0040820E mov     eax, [edx+4]',
    '.text:00408211 retn',
    '.text:00408211 sub_408200 endp',
  ];
  const reg = deriveRegions(L, 'sub_408200', 'edx');
  const r0 = reg.regions[0];
  assert.equal(r0.brokenByCall, true, '夹具前提：这一段的活窗口里确实有 call');
  const m = base([{ off: '0x5d800', width: 4 }], { TL: { size: null, complete: false, members: [{ off: '0x4', width: 4 }] } });
  m.carriers = [{ id: 'l01', kind: 'local', fn: 'sub_408200', reg: 'edx', span: [`0x${r0.from.toString(16)}`, `0x${r0.liveTo.toString(16)}`], type: 'TL*' }];
  const rep = checkClaims(m, { lines: L, baseDir: '.', observed: OBS, roots: new Map([['sub_408200', 'ecx']]) });
  assert.ok(rep.findings.some((f) => f.where.startsWith('G10') && f.why.includes('打断')), `被 call 打断 ⇒ 必须红：${JSON.stringify(rep.findings)}`);
});

test('★⑨ 局部载体：段落类别是常量/初始化 ⇒ 红（那不是指针）', () => {
  const L = [
    '.text:00408300 sub_408300 proc near',
    '.text:00408300 mov     esi, ecx',
    '.text:00408303 xor     edx, edx',            // ← 初始化：不是指针
    '.text:00408305 mov     eax, [edx+4]',
    '.text:00408308 retn',
    '.text:00408308 sub_408300 endp',
  ];
  const reg = deriveRegions(L, 'sub_408300', 'edx');
  const r0 = reg.regions[0];
  const m = base([{ off: '0x5d800', width: 4 }], { TL: { size: null, complete: false, members: [{ off: '0x4', width: 4 }] } });
  m.carriers = [{ id: 'l01', kind: 'local', fn: 'sub_408300', reg: 'edx', span: [`0x${r0.from.toString(16)}`, `0x${r0.liveTo.toString(16)}`], type: 'TL*' }];
  const rep = checkClaims(m, { lines: L, baseDir: '.', observed: OBS, roots: new Map([['sub_408300', 'ecx']]) });
  assert.ok(rep.findings.some((f) => f.where.startsWith('G10')), `常量段不能断言指针类型：${JSON.stringify(rep.findings)}`);
});
test('★⑧ `count:null`（条数未观察）必须给 `sweep` —— 否则窗口无界 ⇒ 会吞掉窗口外的偏移（假解释）', () => {
  // 站点的索引身份与步长**都对上**，但位移在窗口之外 ⇒ ⛔ 不许被解释
  const L = [
    '.text:00407000 sub_407000 proc near',
    '.text:00407000 mov     esi, ecx',
    '.text:00407003 mov     eax, [esi+5D880h]',
    '.text:00407009 mov     edi, [esi+eax*4+5EE10h]',   // 位移 = 0x5ee10（窗口只到 0x5ee08）
    '.text:00407010 retn',
    '.text:00407010 sub_407000 endp',
  ];
  const m = base([{ off: '0x5d880', width: 4 }, { off: '0x5ee04', type: 'T2', count: null, sweep: { stride: 4, index: '*(Engine+0x5d880)' } }]);
  // ★ 元素类型里**故意**放一个 0xc 成员：这样"窗口外的 0x5ee10 相对窗口起点是 0xc"——
  //   如果窗口范围判据被拆掉，这一处就会被**解释**成 T2+0xc（假解释）⇒ 下面的断言当场红。
  m.types.T2 = { size: 4, complete: false, members: [{ off: '0x0', width: 4 }, { off: '0xc', width: 4 }] };
  m.carriers[0].fn = 'sub_407000';
  const roots = new Map([['sub_407000', 'ecx']]);
  const rep = checkClaims(m, { lines: L, baseDir: '.', observed: OBS, roots });
  const p = rep.perCarrier[0];
  assert.equal(p.viaView + p.explained.filter((e) => e.member.offN === 0x5ee04).length, 0, '窗口外的位移一个都不许被这个无界成员解释');
  assert.ok(p.residue.some((r) => r.abs === 0x5ee10), `应进残差：${JSON.stringify(p.residue)}`);
  // 缺 sweep ⇒ 红（声明不可判定）
  const m2 = base([{ off: '0x5d880', width: 4 }, { off: '0x5ee04', type: 'T2', count: null }]);
  m2.types.T2 = { size: 4, complete: false, members: [{ off: '0x0', width: 4 }] };
  m2.carriers[0].fn = 'sub_407000';
  const rep2 = checkClaims(m2, { lines: L, baseDir: '.', observed: OBS, roots });
  assert.ok(rep2.findings.some((f) => f.where.startsWith('G8') && f.why.includes('sweep')), `缺 sweep 必须红：${JSON.stringify(rep2.findings)}`);
});

test('★⑪ `noreturn` 调用之后**没有** fall-through：抛出路径上的写不许污染汇合点后面的块', () => {
  // ★ 实测（`sub_41C6A0`）：`add esi,8` 只在"阶层太深"的抛出路径上，而 `jl` 那条边的块里 `esi` 仍是 obj0。
  //   早先它连了 fall-through ⇒ 汇合后判 UNK ⇒ 后面 9 处访问全丢（+8 之后落在已声明成员上）。
  const L = [
    '.text:0040A000 sub_40A000 proc near',
    '.text:0040A000 mov     esi, ecx',
    '.text:0040A003 cmp     dword ptr [esi+5D880h], 27h',
    '.text:0040A00A jl      short loc_40A020',
    '.text:0040A00C add     esi, 8',                 // ← 只在抛出路径上
    '.text:0040A00F push    0',
    '.text:0040A011 call    __CxxThrowException@8',  // ← 不返回
    '.text:0040A020 loc_40A020:',
    '.text:0040A020 mov     eax, [esi+5D890h]',      // ← 这里 esi 必须仍是 obj0
    '.text:0040A026 retn',
    '.text:0040A026 sub_40A000 endp',
  ];
  const m = base([{ off: '0x5d890', width: 4 }]);
  m.carriers[0].fn = 'sub_40A000';
  const rep = checkClaims(m, { lines: L, baseDir: '.', observed: OBS, roots: new Map([['sub_40A000', 'ecx']]) });
  const p0 = rep.perCarrier[0];
  assert.deepEqual(rep.findings, [], `不该有红：${JSON.stringify(rep.findings)}`);
  assert.equal(p0.unresolvedBase, 0, `noreturn 之后的那处不该是未解析：${JSON.stringify(p0.unresolvedByReg)}`);
  assert.ok(p0.explained.some((e) => e.abs === 0x5d890), `必须按 obj0+0x5d890 解释：${JSON.stringify(p0.explained.map((e) => e.abs))}`);
});

test('★⑫ 对象根溢出到栈槽、再读回：身份要接上（⛔ 不是"栈槽是对象形状"）', () => {
  // ★ 实测（`sub_429460`）：`mov esi,ecx` → `mov [ebp+var_354],esi` → catch 里 `mov esi,[ebp+var_354]`。
  //   栈槽本身**不算**站点（`siteOf` 跳过），它只让"值"能过桥。
  const L = [
    '.text:0040B000 sub_40B000 proc near',
    '.text:0040B000 push    ebp',
    '.text:0040B001 mov     ebp, esp',
    '.text:0040B003 mov     esi, ecx',
    '.text:0040B005 mov     [ebp+var_4], esi',      // 溢出
    '.text:0040B008 mov     ecx, 0',
    '.text:0040B00D call    sub_999999',
    '.text:0040B012 mov     esi, [ebp+var_4]',      // 读回
    '.text:0040B015 mov     eax, [esi+5D890h]',     // ⇒ obj0 + 0x5d890
    '.text:0040B01B mov     esp, ebp',
    '.text:0040B01D pop     ebp',
    '.text:0040B01E retn',
    '.text:0040B01E sub_40B000 endp',
  ];
  const m = base([{ off: '0x5d890', width: 4 }]);
  m.carriers[0].fn = 'sub_40B000';
  const rep = checkClaims(m, { lines: L, baseDir: '.', observed: OBS, roots: new Map([['sub_40B000', 'ecx']]) });
  const p0 = rep.perCarrier[0];
  assert.deepEqual(rep.findings, [], `不该有红：${JSON.stringify(rep.findings)}`);
  assert.equal(p0.unresolvedBase, 0, `读回的 esi 不该是未解析：${JSON.stringify(p0.unresolvedByReg)}`);
  assert.ok(p0.explained.some((e) => e.abs === 0x5d890), '必须按 obj0+0x5d890 解释');
});

test('★⑬ 闭合度：间接调用 / 间接跳转 / 抛异常 / x87 / 字符串指令 都要被点名，且**判定会翻**', async () => {
  const { coverageOf } = await import('../lib/shape-coverage.mjs');
  // 合成一个"什么都有"的函数：obj0 访问 + 间接调用 + 间接跳转 + 抛出 + x87 + 字符串指令 + 栈槽
  const L = [
    '.text:0040C000 sub_40C000 proc near',
    '.text:0040C000 push    ebp',
    '.text:0040C001 mov     ebp, esp',
    '.text:0040C003 mov     ecx, eax',                 // 根未知 ⇒ 后面 [ecx+..] 是未解析基址
    '.text:0040C005 mov     [ebp+var_4], eax',         // 栈槽：**不算缺口**
    '.text:0040C008 call    eax',                      // 间接调用
    '.text:0040C00A call    dword ptr [esi+4]',        // 间接调用（经内存）
    '.text:0040C00E call    sub_123456',               // 直接调用
    '.text:0040C013 call    __CxxThrowException@8',    // 直接调用 + 抛出
    '.text:0040C018 jmp     ds:off_412345',            // 间接跳转
    '.text:0040C01D fld     dword ptr [esi+8]',        // x87 内存操作数（模型之外）
    '.text:0040C020 rep movsd',                        // 字符串指令（不经寄存器模型）
    '.text:0040C022 retn',
    '.text:0040C022 sub_40C000 endp',
  ];
  const { analyzeLines } = await import('../lib/shape-access.mjs');
  const span = { from: 0, to: L.length - 1 };
  const { sites } = analyzeLines(L, span.from, span.to, 'ecx', {});
  const cov = coverageOf(L, 'sub_40C000', span, 'ecx', sites);
  assert.equal(cov.escapes.indirectCall.length, 2, `两处间接调用：${JSON.stringify(cov.escapes.indirectCall)}`);
  assert.equal(cov.escapes.indirectJmp.length, 1, '一处间接跳转');
  assert.equal(cov.escapes.throw.length, 1, '一处抛出');
  assert.equal(cov.escapes.floatMem.length, 1, '一处 x87 内存操作数');
  assert.equal(cov.escapes.stringOps.length, 1, '一处字符串指令');
  assert.equal(cov.kinds.stack, 1, '栈槽行要单独算（⛔ 不算缺口）');
  assert.equal(cov.closed, false, '有间接调用 ⇒ **未闭合**');
  assert.ok(cov.reasons.some((r) => r.includes('间接调用')), `原因里必须点名间接调用：${JSON.stringify(cov.reasons)}`);

  // 同一段代码，去掉逃逸通道 ⇒ 闭合（口径要能翻，不是恒绿）
  const L2 = L.filter((l) => !/call\s+eax|call\s+dword|jmp\s+ds:/.test(l));
  const s2 = analyzeLines(L2, 0, L2.length - 1, 'ecx', {});
  const cov2 = coverageOf(L2, 'sub_40C000', { from: 0, to: L2.length - 1 }, 'ecx', s2.sites);
  assert.equal(cov2.escapes.indirectCall.length, 0);
  assert.equal(cov2.closed, true, `去掉逃逸后应当闭合：${JSON.stringify(cov2.reasons)}`);
});

test('★⑭ 调用链一致性：调用方实参类型 ↔ 被调方载体声明（same / **mismatch** / 单侧）', async () => {
  const { auditCalls } = await import('../lib/shape-calls.mjs');
  // 三个被调方：都声明成 T0* 的 / 声明 T0* 但调用方传的是 *(this+0xaa514) / 完全不声明
  const L = [
    '.text:0040D000 sub_40D000 proc near',
    '.text:0040D000 mov     [ebp+var_4], ecx',   // 溢出 this（call 会清 ecx）
    '.text:0040D003 call    sub_40D100',          // ① ecx = this（调用方 T0）
    '.text:0040D008 mov     eax, [ebp+var_4]',    // 取回 this（栈槽传播）
    '.text:0040D00B mov     ecx, eax',
    '.text:0040D00D mov     eax, [ecx+0AA514h]',  // eax = *(this+0xaa514) ⇒ T_aa514
    '.text:0040D013 mov     ecx, eax',
    '.text:0040D015 call    sub_40D200',          // ② 调用方 T_aa514、被调方声明 T0* ⇒ **不一致**
    '.text:0040D01A mov     ecx, [ebp+var_4]',    // 取回 this
    '.text:0040D01D call    sub_40D300',          // ③ 被调方没声明 ⇒ **单侧**
    '.text:0040D022 retn',
    '.text:0040D022 sub_40D000 endp',
    '.text:0040D100 sub_40D100 proc near',
    '.text:0040D100 retn',
    '.text:0040D100 sub_40D100 endp',
    '.text:0040D200 sub_40D200 proc near',
    '.text:0040D200 retn',
    '.text:0040D200 sub_40D200 endp',
    '.text:0040D300 sub_40D300 proc near',
    '.text:0040D300 retn',
    '.text:0040D300 sub_40D300 endp',
  ];

  const types = {
    T0: { size: null, complete: false, members: [{ off: '0xaa514', width: 4, kind: 'ptr', type: 'T_aa514' }] },
    T_aa514: { size: null, complete: false, members: [{ off: '0x4', width: 4 }] },
  };
  const models = [
    { file: 'a.json', model: { carriers: [{ id: 'a1', kind: 'arg', fn: 'sub_40D000', reg: 'ecx', type: 'T0*' }] } },
    { file: 'b.json', model: { carriers: [{ id: 'b1', kind: 'arg', fn: 'sub_40D100', reg: 'ecx', type: 'T0*' }] } },
    { file: 'c.json', model: { carriers: [{ id: 'c1', kind: 'arg', fn: 'sub_40D200', reg: 'ecx', type: 'T0*' }] } },
  ];
  const roots = new Map([['sub_40D000', 'ecx']]);
  const r = auditCalls({ lines: L, fns: ['sub_40D000'], models, types, selfTypeOf: () => 'T0', roots });
  const at = (callee) => r.rows.filter((x) => x.callee === callee && x.reg === 'ecx');
  assert.equal(at('sub_40D100')[0]?.verdict, 'same', `传 this、被调方声明 T0* ⇒ 一致：${JSON.stringify(at('sub_40D100'))}`);
  assert.equal(at('sub_40D200')[0]?.verdict, 'mismatch', `传 *(this+0xaa514)（T_aa514）、被调方却声明 T0* ⇒ **必须判不一致**；实得 ${JSON.stringify(at('sub_40D200'))}`);
  assert.equal(at('sub_40D300')[0]?.verdict, 'calleeUntyped', '被调方没声明 ⇒ **单侧**（这是"另一侧缺声明"的探测器）');
  // 再验一次"一致"的正例：把 sub_40D200 改声明成 T_aa514* ⇒ 判定要翻
  models[2].model.carriers[0].type = 'T_aa514*';
  const r2 = auditCalls({ lines: L, fns: ['sub_40D000'], models, types, selfTypeOf: () => 'T0', roots });
  assert.equal(r2.rows.filter((x) => x.callee === 'sub_40D200' && x.reg === 'ecx')[0]?.verdict, 'same', '改成 T_aa514* 后应当一致');
});
