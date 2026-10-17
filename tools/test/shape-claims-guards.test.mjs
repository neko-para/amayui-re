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
    '.text:0040C018 jmp     ds:off_412345',            // 间接跳转（跳表）
    '.text:0040C01A jmp     short loc_40C022',        // 函数内标签 ⇒ **直接**跳转（别算间接）
    '.text:0040C01C call    ??_U@YAPAXI@Z',          // MSVC 装饰名（operator new[]）⇒ **直接**调用
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
  // ★ 回归（实测假阳）：函数内标签跳转与装饰名调用都**不是**间接目标
  assert.equal(cov.escapes.indirectJmp.length, 1, 'jmp ds:… 才是间接；jmp short loc_* 是直接');
  assert.ok(!cov.escapes.indirectCall.some((x) => /\?\?/.test(x.text)), `装饰名不是间接调用：${JSON.stringify(cov.escapes.indirectCall)}`);
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

test('★⑮ 类型传播（全图 BFS）：this/interior 同类 · member(path) 换类 · 冲突要报', async () => {
  const { propagate, nextCandidates } = await import('../lib/shape-propagate.mjs');
  const types = {
    T0: { size: null, complete: false, members: [{ off: '0xaa514', width: 4, kind: 'ptr', type: 'TX' }, { off: '0x408', type: 'TX', count: 1 }] },
    TX: { size: null, complete: false, members: [{ off: '0x4', width: 4 }] },
  };
  // A（已声明 T0）调用 B（传 this）、C（传 interior）、D（传 *(this+0xaa514)）、E（传未知）
  const links = {
    sub_A: [
      { kind: 'call', target: 'sub_B', ea: 0xa01, args: { ecx: { k: 'this' } } },
      { kind: 'call', target: 'sub_C', ea: 0xa02, args: { ecx: { k: 'interior', obj: 'this', off: 8 } } },
      { kind: 'call', target: 'sub_D', ea: 0xa03, args: { ecx: { k: 'member', path: [0xaa514], off: 4 } } },
      { kind: 'call', target: 'sub_E', ea: 0xa04, args: { ecx: { k: 'unknown' } } },
      { kind: 'call', target: 'sub_F', ea: 0xa05, args: { ecx: { k: 'this' } } },   // 与 G 冲突用
      { kind: 'call', target: 'sub_C8', ea: 0xa06, args: { ecx: { k: 'interior', obj: 'this', off: 0x408 } } },
      { kind: 'call', target: 'sub_C0', ea: 0xa07, args: { ecx: { k: 'interior', obj: 'this', off: 0 } } },
    ],
    sub_G: [{ kind: 'call', target: 'sub_F', ea: 0xb01, args: { ecx: { k: 'member', path: [0xaa514] } } }],
  };
  const carriers = [
    { id: 'a', kind: 'arg', fn: 'sub_A', reg: 'ecx', type: 'T0*' },
    { id: 'g', kind: 'arg', fn: 'sub_G', reg: 'ecx', type: 'T0*' },
  ];
  const analyzeFn = (sym) => (links[sym] ? { links: links[sym] } : null);
  const r = propagate({ carriers, types, analyzeFn });
  assert.equal(r.inferred.get('sub_B')?.type, 'T0', 'this ⇒ 同类');
  // ★ interior 的**类型层**规则（batch-01/a+c 实测证伪了"同类"）：
  //   k===0 ⇒ 同类；k 是已声明的**内嵌子对象** ⇒ 那个子类；否则**不传播**（⛔ 不许当同类）。
  assert.equal(r.inferred.has('sub_C'), false, 'interior(this+0x8) 的 0x8 未声明成内嵌子对象 ⇒ **不传播**');
  assert.equal(r.inferred.get('sub_C8')?.type, 'TX', 'interior(this+0x408) 是已声明的内嵌子对象 ⇒ 换类成 TX');
  assert.equal(r.inferred.get('sub_D')?.type, 'TX', 'member(path) ⇒ 沿 types 解析成目标类型');
  assert.equal(r.inferred.has('sub_E'), false, 'unknown ⇒ **不传播**（⛔ 不猜）');
  assert.equal(r.inferred.get('sub_C0')?.type, 'T0', 'interior+0 ⇒ 对象本身，同类');
  assert.equal(r.inferred.has('sub_G'), true, 'G 自己有载体 ⇒ 已知');
  // sub_F 被 A 传 this（T0）、被 G 传 TX ⇒ 冲突必须报出来
  assert.equal(r.conflicts.length, 1, `必须报 1 处传播冲突：${JSON.stringify(r.conflicts)}`);
  assert.equal(r.conflicts[0].target, 'sub_F');
  // 下一批候选：已推出类型但没有载体的
  const next = nextCandidates({ inferred: r.inferred, carriers });
  const fns = next.map((x) => x.fn);
  assert.ok(fns.includes('sub_B') && fns.includes('sub_D'), `候选应含 B/D：${fns.join(',')}`);
  assert.ok(!fns.includes('sub_A'), 'A 自己有载体 ⇒ 不是候选');

  // ★ 回归：**local 载体只说明那个寄存器**，不许被当成"函数的 this 是什么"的种子
  const r2 = propagate({
    carriers: [
      { id: 's', kind: 'arg', fn: 'sub_S', reg: 'ecx', type: 'T0*' },
      { id: 'l', kind: 'local', fn: 'sub_L', reg: 'edi', type: 'TX*' },
    ],
    types,
    analyzeFn: (sym) => (sym === 'sub_L' ? { links: [{ kind: 'call', target: 'sub_N', ea: 0xc01, args: { ecx: { k: 'this' } } }] } : null),
  });
  assert.equal(r2.inferred.has('sub_L'), false, 'local 载体**不许**给函数定根类型');
  assert.equal(r2.inferred.has('sub_N'), false, 'local 载体也不许把类型传播到它的被调方（会被污染成 TX）');
});

test('★⑯ 争议台账（G12）：拒绝声明也要留下**可检查**的争议（缺 EA / 过期 / 没落点都红）', () => {
  // 为什么需要：判据只在"有站点落地"时说话。若用法全是 lea 或基址未解析，站点不成立 ⇒ 工具全绿，
  // 而"这里其实是指针/是数组、只是我说不出目标/条数"这件事**无处安放**。争议台账就是它的家。
  const mkModel = (disputes) => {
    const m = base([{ off: '0x5d800', width: 4 }]);
    m.types.TB = { size: null, complete: false, members: [{ off: '0x0', width: 4 }] };
    m.carriers[0].fn = 'sub_409000';
    m.disputes = disputes;
    return m;
  };
  const L = ['.text:00409000 sub_409000 proc near', '.text:00409000 mov eax, [ecx+5D800h]', '.text:00409006 retn', '.text:00409006 sub_409000 endp'];
  const R = new Map([['sub_409000', 'ecx']]);
  const run = (d) => checkClaims(mkModel(d), { lines: L, baseDir: '.', observed: OBS, roots: R });

  // ① 合法：关于已声明成员的 size 争议 + 有 witness
  const okRep = run([{ container: 'T0', off: '0x5d800', kind: 'size-unknown', why: '条数未证', evidence: ['0x409000'] }]);
  assert.deepEqual(okRep.findings, [], `合法争议不该红：${JSON.stringify(okRep.findings)}`);
  assert.equal(okRep.disputes.length, 1, '合法争议要进报告');
  assert.equal(okRep.disputes[0].kind, 'size-unknown');

  // ② 缺 witness EA ⇒ 红（⛔ 不许只写一句"我觉得是指针"）
  const noEv = run([{ container: 'T0', off: '0x5d800', kind: 'ptr-unknown', why: '' }]);
  assert.ok(noEv.findings.some((f) => f.where.startsWith('G12') && f.why.includes('witness')), `缺 EA 必须红：${JSON.stringify(noEv.findings)}`);

  // ③ 落点不是已声明成员 ⇒ 红（争议必须有落点）
  const noMember = run([{ container: 'T0', off: '0x9999', kind: 'ptr-unknown', why: '', evidence: ['0x409000'] }]);
  assert.ok(noMember.findings.some((f) => f.where.startsWith('G12') && f.why.includes('不是已声明成员')), '没落点必须红');

  // ④ 过期：声明已经回答了这条争议 ⇒ 红（删掉旧账）
  const m2 = base([{ off: '0x5d800', width: 4, kind: 'ptr', type: 'TB' }]);
  m2.types.TB = { size: null, complete: false, members: [{ off: '0x0', width: 4 }] };
  m2.carriers[0].fn = 'sub_409000';
  m2.disputes = [{ container: 'T0', off: '0x5d800', kind: 'ptr-unknown', why: '目标不明', evidence: ['0x409000'] }];
  const stale = checkClaims(m2, { lines: L, baseDir: '.', observed: OBS, roots: R });
  assert.ok(stale.findings.some((f) => f.where.startsWith('G12') && f.why.includes('已解决')), `过期争议必须红：${JSON.stringify(stale.findings)}`);

  // ⑤ kind 不在闭集合 ⇒ 红
  const badKind = run([{ container: 'T0', off: '0x5d800', kind: '随便', why: '', evidence: ['0x409000'] }]);
  assert.ok(badKind.findings.some((f) => f.where.startsWith('G12')), 'kind 越界必须红');
});

test('★⑰ 全局载体（G13）：**根对象唯一的来源**是"某个全局槽里装着它"，从根正向传播靠它', async () => {
  // 为什么需要这一条：Engine（IDA 类名 `Command`）**不是任何函数的参数** ——
  // `_WinMain@16`(0x4ba890) 里 `push 0AABC8h` → `operator new`（0x4baa11）→ `call sub_415640`（构造器，0x4baa2e）
  // → `mov ds:dword_55E1BC, eax`（0x4baa3a）才把它存进全局。
  // 于是所有 `mov ecx, ds:dword_55E1BC; call X` 都是"X 的 this = 根对象"的机械证据；
  // 而在加这一类之前，这类调用点上 ecx 的来源整类是 `unknown`（实测 4256 个 unknown 里的一大撮）。
  const { analyzeLines } = await import('../lib/shape-access.mjs');
  const { propagate } = await import('../lib/shape-propagate.mjs');

  // ── ① 访问助手：`mov ecx, ds:<全局>` 之后 call ⇒ 该调用点的 ecx 来源必须是 `global`（不是 unknown）
  const LW = [
    '.text:0040D000 _WinMain@16 proc near',
    '.text:0040D000 push    ebp',
    '.text:0040D001 mov     ebp, esp',
    '.text:0040D003 push    0AABC8h',
    '.text:0040D008 call    ??2@YAPAXI@Z',
    '.text:0040D00D mov     ecx, eax',
    '.text:0040D00F call    sub_40D100',
    '.text:0040D014 mov     ds:dword_55E1BC, eax',
    '.text:0040D01A mov     ecx, ds:dword_55E1BC',
    '.text:0040D020 call    sub_40D200',
    '.text:0040D026 mov     eax, ds:dword_55E1BC',
    '.text:0040D02C mov     edx, [eax+5D818h]',
    '.text:0040D032 retn',
    '.text:0040D032 _WinMain@16 endp',
  ];
  const { links } = analyzeLines(LW, 0, LW.length - 1, null, {});
  const toRoot = links.find((l) => l.target === 'sub_40D200');
  assert.equal(toRoot?.args?.ecx?.k, 'global', `必须认成 global：${JSON.stringify(toRoot?.args?.ecx)}`);
  assert.equal(toRoot.args.ecx.sym, 'dword_55E1BC', '符号名要原样带出来（⛔ 不许折成 "unknown"）');
  // ★ 带方括号的（`ds:0[ecx*4]`，位移写在段前缀里，语料 363 处）**不是**符号 —— 别把段前缀整类吃掉
  const sz = analyzeLines([
    '.text:0040D100 sub_40D100 proc near',
    '.text:0040D100 lea     eax, ds:0[ecx*4]',      // 纯算术（语料形态）
    '.text:0040D107 mov     ecx, eax',
    '.text:0040D109 call    sub_SZ',
    '.text:0040D10E mov     ecx, ds:5D100h[ecx*4]',  // 带索引的段前缀：解不出值
    '.text:0040D116 call    sub_SZ2',
    '.text:0040D11B retn',
    '.text:0040D11B sub_40D100 endp',
  ], 0, 7, null, {});
  const mk1 = sz.links.find((l) => l.target === 'sub_SZ');
  assert.notEqual(mk1?.args?.ecx?.k, 'global', '`ds:位移[索引]` 是算术，不是"全局槽里装着的东西"');
  const mk2 = sz.links.find((l) => l.target === 'sub_SZ2');
  assert.notEqual(mk2?.args?.ecx?.k, 'global', '带索引的段前缀解不出值 ⇒ 不许当 glob（⛔ 也不许猜成常量）');

  // ── ② 传播规则：只有**声明过**的全局能定类；没声明 ⇒ 不传播（⛔ 不猜）
  const types = { T0: { size: null, complete: false, members: [{ off: '0x5d818', width: 4 }] } };
  const gLinks = {
    _WinMain: [{ kind: 'call', target: 'sub_E1', ea: 0xe01, args: { ecx: { k: 'global', sym: 'dword_55E1BC' } } }],
    sub_NoDecl: [{ kind: 'call', target: 'sub_E2', ea: '0xe02', args: { ecx: { k: 'global', sym: 'dword_DEADBE' } } }],
  };
  const analyzeFn = (sym) => (gLinks[sym] ? { links: gLinks[sym] } : null);
  const r = propagate({
    carriers: [{ id: 'g01', kind: 'global', sym: 'dword_55E1BC', type: 'T0*', witness: ['0x4baa3a'] }],
    types,
    analyzeFn,
    globals: new Map([['dword_55E1BC', 'T0']]),
    extraCallers: ['_WinMain', 'sub_NoDecl'],
  });
  assert.equal(r.inferred.get('sub_E1')?.type, 'T0', '声明过的全局 ⇒ 被调方根类型 = 该类型（**从根正向**的第一步）');
  assert.equal(r.inferred.has('sub_E2'), false, '没声明的全局 ⇒ **不传播**（⛔ 不猜）');
  assert.equal(r.inferred.has('_WinMain'), false, '全局扫描种子自己**没有**类（它只是入口），不许被记成 T0');
  // ★ 反向：不给 `extraCallers` ⇒ 没有根约定、又不在调用图上的入口**根本扫不到**（这正是"倒着爬"的成因）
  const rNo = propagate({ carriers: [{ id: 'g01', kind: 'global', sym: 'dword_55E1BC', type: 'T0*' }], types, analyzeFn, globals: new Map([['dword_55E1BC', 'T0']]) });
  assert.equal(rNo.inferred.has('sub_E1'), false, '没有根锚定扫描 ⇒ 入口的调用点扫不到（数字变小，不是报错）');
  // ★ 被调方必须真的收 `this`：`__cdecl` 函数里的 ecx 是残留值 ⇒ 记上去就是**假断言 + 假冲突**
  //   实测形态：`sub_40A160`(`__cdecl`) 同时被推出 T_77960 与 T0。
  const rCdecl = propagate({
    carriers: [{ id: 'g01', kind: 'global', sym: 'dword_55E1BC', type: 'T0*' }],
    types, analyzeFn, globals: new Map([['dword_55E1BC', 'T0']]), extraCallers: ['_WinMain'],
    acceptTarget: (sym) => sym !== 'sub_E1',
  });
  assert.equal(rCdecl.inferred.has('sub_E1'), false, 'acceptTarget 说它不收 this ⇒ 不许给它记类型');

  // ── ③ G13 的三条硬要求：符号真实存在 · witness 必须是**写**那个全局的 store · 类型已声明
  const LS = [
    '.text:0040E000 sub_40E000 proc near',
    '.text:0040E000 mov     edx, ds:dword_ABCDEF',
    '.text:0040E006 mov     ds:dword_ABCDEF, eax',
    '.text:0040E00C mov     eax, [edx+5D818h]',
    '.text:0040E012 retn',
    '.text:0040E012 sub_40E000 endp',
  ];
  const RS = new Map([['sub_40E000', 'ecx']]);
  const mkG = (g) => {
    const m = base([{ off: '0x5d818', width: 4 }]);
    m.carriers[0].fn = 'sub_40E000';
    m.carriers = [m.carriers[0], g];
    return m;
  };
  const runG = (g) => checkClaims(mkG(g), { lines: LS, baseDir: '.', observed: OBS, roots: RS });
  const okG = runG({ id: 'g02', kind: 'global', sym: 'dword_ABCDEF', type: 'T0*', witness: ['0x40e006'] });
  assert.deepEqual(okG.findings, [], `合法全局载体不该红：${JSON.stringify(okG.findings)}`);
  assert.equal(okG.globalCarriers.length, 1, '合法的全局载体要进报告');

  const readOnly = runG({ id: 'g02', kind: 'global', sym: 'dword_ABCDEF', type: 'T0*', witness: ['0x40e000'] });
  assert.ok(readOnly.findings.some((f) => f.where.startsWith('G13') && f.why.includes('没有一条是')),
    `witness 是**读**（0x40e000）⇒ 红：${JSON.stringify(readOnly.findings)}`);
  const noWit = runG({ id: 'g02', kind: 'global', sym: 'dword_ABCDEF', type: 'T0*' });
  assert.ok(noWit.findings.some((f) => f.where.startsWith('G13') && f.why.includes('witness')), '缺 witness ⇒ 红');
  const ghostEa = runG({ id: 'g02', kind: 'global', sym: 'dword_ABCDEF', type: 'T0*', witness: ['0x409999'] });
  assert.ok(ghostEa.findings.some((f) => f.where.startsWith('G13')), 'witness EA 在语料里不存在 ⇒ 红（别拿编的地址凑数）');
  const badType = runG({ id: 'g02', kind: 'global', sym: 'dword_ABCDEF', type: 'TNOPE*', witness: ['0x40e006'] });
  assert.ok(badType.findings.some((f) => f.where.startsWith('G13') && f.why.includes('未声明')), '类型未声明 ⇒ 红');
  // 同一个全局两种类型 ⇒ 红（否则后面"从根传播"会随声明顺序漂）
  const dup = runG({ id: 'g02', kind: 'global', sym: 'dword_ABCDEF', type: 'T0*', witness: ['0x40e006'] });
  const dupModel = mkG({ id: 'g02', kind: 'global', sym: 'dword_ABCDEF', type: 'T0*', witness: ['0x40e006'] });
  dupModel.types.TX = { size: 8, complete: false, members: [{ off: '0x0', width: 4 }] };
  dupModel.carriers.push({ id: 'g03', kind: 'global', sym: 'dword_ABCDEF', type: 'TX*', witness: ['0x40e006'] });
  const dupRep = checkClaims(dupModel, { lines: LS, baseDir: '.', observed: OBS, roots: RS });
  assert.ok(dupRep.findings.some((f) => f.where.startsWith('G13') && f.why.includes('又声明成')), '同一全局两种类型 ⇒ 红');
  assert.equal(dup.findings.length, 0, '单条声明仍应绿（上面那组是控制变量）');
});


test('★⑱ 适配器类型（adapter:true）：**同偏移不同类 = 基类相容**，不是冲突（`this->+0x4` 转发器那一族）', async () => {
  // 实测出处：`sub_4034C0`/`D0`/`E0` 都做 `mov ecx,[ecx+4]` 之后转发（`0x4034c3`/`0x4034d3`/`0x4034e3`），
  // 其中 `sub_4034E0` 是 `mov eax,[ecx]; mov eax,[eax+8]; jmp eax` = **虚调用**。
  // 它们被 `T0` 与内嵌子对象 `T_1ee8`/`T_4ed10`/`T_a609c` 四种类当 this 调用。
  // ★ 真相是**通用基类 `BaseObject`**：`sub_4034a0`（RTTI 名 `??_7BaseObject@@6B@`）在 `0x4034a2` 写 vtable、
  //   在 `0x4034a8` 写 `[this+4] = offset dword_55BF70`（全局输出对象的地址），而它有 **26 个类的构造器**调用
  //   （含 `sub_415640`=Command）⇒ 子类在**偏移 0** 处就是 `BaseObject`，`this` 不需调整 ⇒ 天然相容。
  // 本用例用合成名字 `T_ptr4` 表达同一个形状（`adapter:true` + 只有 `+0x4 : ptr→未知`）；
  // ⛔ 若不给这个显式标记，调用点对账会记成 4 处"不一致"。
  const { auditCalls } = await import('../lib/shape-calls.mjs');
  const { propagate } = await import('../lib/shape-propagate.mjs');

  const types = {
    T0: { size: null, complete: false, members: [{ off: '0x10', width: 4 }] },
    TA: { size: null, complete: false, members: [{ off: '0x10', width: 4 }] },
    T_ptr4: { size: null, complete: false, adapter: true, members: [{ off: '0x4', width: 4, kind: 'ptr', type: null }] },
  };
  // ① 调用点对账：T0 调一个声明成 T_ptr4 的被调方 ⇒ 判"适配器相容"，⛔ 不是 mismatch
  const LC = [
    '.text:0040F000 sub_40F000 proc near',
    '.text:0040F000 call    sub_40F100',
    '.text:0040F005 retn',
    '.text:0040F005 sub_40F000 endp',
    '.text:0040F100 sub_40F100 proc near',
    '.text:0040F100 mov     ecx, [ecx+4]',
    '.text:0040F106 retn',
    '.text:0040F106 sub_40F100 endp',
  ];
  const models = [{ file: 'm.json', model: { carriers: [
    { id: 'a', kind: 'arg', fn: 'sub_40F000', reg: 'ecx', type: 'T0*' },
    { id: 'b', kind: 'arg', fn: 'sub_40F100', reg: 'ecx', type: 'T_ptr4*' },
  ] } }];
  const r = auditCalls({
    lines: LC, fns: ['sub_40F000'], models, types,
    selfTypeOf: (fn) => (fn === 'sub_40F000' ? 'T0' : null),
    roots: new Map([['sub_40F000', 'ecx'], ['sub_40F100', 'ecx']]),
  });
  const ecxRow = r.rows.find((x) => x.reg === 'ecx');
  assert.equal(ecxRow?.verdict, 'adapter', `必须判"适配器相容"：${JSON.stringify(r.rows)}`);

  // ② 反向：被调方**不是**适配器 ⇒ 仍然是不一致（⛔ 适配器这一档不许糊掉真不一致）
  const nonAdapter = [{ file: 'm.json', model: { carriers: [
    { id: 'a', kind: 'arg', fn: 'sub_40F000', reg: 'ecx', type: 'T0*' },
    { id: 'b', kind: 'arg', fn: 'sub_40F100', reg: 'ecx', type: 'TA*' },
  ] } }];
  const r2 = auditCalls({
    lines: LC, fns: ['sub_40F000'], models: nonAdapter, types,
    selfTypeOf: () => 'T0',
    roots: new Map([['sub_40F000', 'ecx'], ['sub_40F100', 'ecx']]),
  });
  assert.equal(r2.rows.find((x) => x.reg === 'ecx')?.verdict, 'mismatch', '非适配器仍要报不一致');

  // ③ 传播：被调方**已有声明**时，推断出的另一类只记 note，⛔ 不记冲突
  const gLinks = {
    sub_C1: [{ kind: 'call', target: 'sub_A1', ea: 0xf01, args: { ecx: { k: 'this' } } }],
    sub_C2: [{ kind: 'call', target: 'sub_A1', ea: 0xf02, args: { ecx: { k: 'this' } } }],
  };
  const pr = propagate({
    carriers: [
      { id: 'a', kind: 'arg', fn: 'sub_C1', reg: 'ecx', type: 'T0*' },
      { id: 'b', kind: 'arg', fn: 'sub_C2', reg: 'ecx', type: 'TA*' },
      { id: 'c', kind: 'arg', fn: 'sub_A1', reg: 'ecx', type: 'T_ptr4*' },
    ],
    types,
    analyzeFn: (sym) => (gLinks[sym] ? { links: gLinks[sym] } : null),
  });
  assert.equal(pr.conflicts.length, 0, `声明过的被调方不许记冲突：${JSON.stringify(pr.conflicts)}`);
  assert.ok(pr.declaredMismatch.length >= 1, '要单列"推断与声明不一致"（让步但不静默）');
  assert.equal(pr.declaredMismatch[0].declared, 'T_ptr4');

  // ④ G 侧：适配器类型的三条硬要求（给 size / 声明 complete / 空适配器 都红）
  const mkA = (t) => {
    const m = base([{ off: '0x5d818', width: 4 }]);
    m.carriers[0].fn = 'sub_401000';
    m.types.T_ok = t;
    return m;
  };
  const withSize = checkClaims(mkA({ size: 8, complete: false, adapter: true, members: [{ off: '0x4', width: 4 }] }),
    { lines: L, baseDir: '.', observed: OBS, roots: ROOTS });
  assert.ok(withSize.findings.some((f) => f.where.startsWith('适配器') && f.why.includes('size')), '适配器给 size ⇒ 红');
  const withComplete = checkClaims(mkA({ size: null, complete: true, adapter: true, members: [{ off: '0x4', width: 4 }] }),
    { lines: L, baseDir: '.', observed: OBS, roots: ROOTS });
  assert.ok(withComplete.findings.some((f) => f.where.startsWith('适配器') && f.why.includes('complete')), '适配器声明 complete ⇒ 红');
  const empty = checkClaims(mkA({ size: null, complete: false, adapter: true, members: [] }),
    { lines: L, baseDir: '.', observed: OBS, roots: ROOTS });
  assert.ok(empty.findings.some((f) => f.where.startsWith('适配器') && f.why.includes('至少')), '空适配器 ⇒ 红');
});
