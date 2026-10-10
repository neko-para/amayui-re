/** @env pure @kind contract @why 覆盖度查询把"锚属于哪个函数/收没收口"算错了：会把没分析过的说成收口、或把前沿指错人 —— 而这是"下一步该做什么"的唯一机械依据 */
/**
 * tools/test/ledger-coverage.test.mjs —— **函数覆盖度**（`pnpm tools ledger coverage`）的守卫
 *
 * ## 为什么它必须条条断言（而不是"跑一遍看看数"）
 * 这个查询要回答的问题是「**下一步该取证谁**」——它错了不会报错，只会把人指到错的地方。
 * 所以每一条判据都拿一份**合成语料 + 合成台账**钉住：
 *
 * * 锚 EA **在函数起点** vs **在体内** 必须分开（`complete` 要求前者）；
 * * `complete` 是**最大不动点**：callee 没登记 ⇒ 调用方不算收口；`call/jmp` 两种边都要算；
 * * **互递归**（A↔B 都登记且没有别的未收口 callee）必须判**收口**（否则"所有递归都永远不算完"）；
 * * 前沿只列**未登记却被已登记调用**的；已登记的不许混进来；
 * * **诚实项**：`call eax` 这类解不出目标的调用点必须计数（否则"闭包完整"会被当成事实）。
 *
 * 运行：`pnpm test`（`@env pure`：合成 `.lst` + 内存里的记录，不碰语料、不碰真台账）
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';

import { coverage, TIERS } from '../lib/coverage.mjs';
import { functionOfEa, functionInventory, pickListing } from '../lib/disasm.mjs';

/** 合成语料：4 个函数 + 一条间接调用 + 一对互递归 + 一个体内锚的目标 */
const LST_TEXT = [
  // sub_401000 → sub_401100（已登记）+ 一条解不出目标的调用
  '.text:00401000 sub_401000 proc near',
  '.text:00401000 call    sub_401100',
  '.text:00401005 call    eax',
  '.text:0040100A retn',
  '.text:0040100B sub_401000 endp',
  // sub_401100：叶子、已登记 ⇒ 收口
  '.text:00401100 sub_401100 proc near',
  '.text:00401100 retn',
  '.text:00401101 sub_401100 endp',
  // sub_401200 → sub_401300（**未登记**）⇒ 调用方不算收口
  '.text:00401200 sub_401200 proc near',
  '.text:00401200 call    sub_401300',
  '.text:00401205 retn',
  '.text:00401206 sub_401200 endp',
  // sub_401300：未登记 ⇒ 前沿（被 sub_401200 调用）
  '.text:00401300 sub_401300 proc near',
  '.text:00401300 retn',
  '.text:00401301 sub_401300 endp',
  // sub_401400 ← 只有**体内**锚（0x401405）：点状，不算"被人整体看过"
  '.text:00401400 sub_401400 proc near',
  '.text:00401400 retn',
  '.text:00401405 nop',
  '.text:00401406 retn',
  '.text:00401407 sub_401400 endp',
  // sub_401500 ↔ sub_401600 互递归（都登记、都没有别的 callee）⇒ 两个都该收口
  '.text:00401500 sub_401500 proc near',
  '.text:00401500 call    sub_401600',
  '.text:00401505 retn',
  '.text:00401506 sub_401500 endp',
  '.text:00401600 sub_401600 proc near',
  '.text:00401600 jmp     sub_401500',
  '.text:00401605 sub_401600 endp',
  // ★ 库桩（/GS cookie 形态：无内存操作数、不调 sub_、尾跳 CRT）⇒ 必须被判成"疑似库代码"
  '.text:00401800 sub_401800 proc near',
  '.text:00401800 cmp     ecx, ds:dword_559A68',
  '.text:00401806 jnz     short loc_40180A',
  '.text:00401808 rep retn',
  '.text:0040180A loc_40180A:',
  '.text:0040180A jmp     ___report_gsfailure',
  '.text:0040180A sub_401800 endp',
  // ★ 一个**引擎**函数：调库桩 + CRT，但**碰内存**（有 `[`）⇒ 不许被摘掉
  '.text:00401900 sub_401900 proc near',
  '.text:00401900 mov     eax, [ecx+5D8B4h]',
  '.text:00401906 call    sub_401800',
  '.text:0040190B call    _memcpy',
  '.text:00401910 retn',
  '.text:00401911 sub_401900 endp',
  // 具名函数：默认不计入宇宙
  '.text:00401700 NamedThing proc near',
  '.text:00401700 retn',
  '.text:00401701 NamedThing endp',
  // ★ 库候选的**假阳性**（2026-10 收窄）：内联的 STL 抛出出口 + **Engine 尺度位移**（0x5D880h）
  //   ⇒ 这是个在 `Engine` 大对象上干活的引擎函数，⛔ 不许被摘出分桶与两张榜
  '.text:00401A00 sub_401A00 proc near',
  '.text:00401A00 mov     eax, [ecx+5D880h]',
  '.text:00401A06 call    ?_Xout_of_range@std@@YAXPBD@Z',
  '.text:00401A0B retn',
  '.text:00401A0C sub_401A00 endp',
  // ★ 对照 ①：同一形态，但位移只有 SSO 量级（0x10h）⇒ **仍然**算库候选（收窄不许滥杀真库代码）
  '.text:00401B00 sub_401B00 proc near',
  '.text:00401B00 mov     eax, [ecx+10h]',
  '.text:00401B06 call    ?_Xout_of_range@std@@YAXPBD@Z',
  '.text:00401B0B retn',
  '.text:00401B0C sub_401B00 endp',
  // ★ 对照 ②：**栈基址**上的大位移不算"引擎尺度"（栈帧可以很大）⇒ 仍算库候选
  '.text:00401C00 sub_401C00 proc near',
  '.text:00401C00 sub     esp, 4000h',
  '.text:00401C06 mov     eax, [ebp-4000h]',
  '.text:00401C0C call    ?_Xout_of_range@std@@YAXPBD@Z',
  '.text:00401C12 leave',
  '.text:00401C13 retn',
  '.text:00401C14 sub_401C00 endp',
].join('\n') + '\n';

/** 合成台账条目：只给覆盖度要用到的三个字段（`effective` / `anchor` / `subject`） */
const rec = (id, ea, subject = `probe/${id}`) => ({
  id,
  subject,
  effective: 'accepted',
  anchor: [{ type: 'bin', repo: 'reference', path: 'x.EXE', ea }],
});

function fixture() {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'amayui-coverage-'));
  const lst = path.join(dir, 'synthetic.lst');
  fs.writeFileSync(lst, LST_TEXT, 'utf8');
  const entries = [
    rec('R1', 0x401000), // 起点：callee 401100 已登记 ⇒ 收口
    rec('R2', 0x401100), // 叶子 + 起点 ⇒ 收口
    rec('R3', 0x401200), // 起点，但 callee 401300 未登记 ⇒ partial-rooted
    rec('R4', 0x401405), // **体内**锚 ⇒ partial-spotty
    rec('R5', 0x401500), // 互递归一半 ⇒ 收口
    rec('R6', 0x401600), // 互递归另一半 ⇒ 收口（jmp 尾跳也算边）
  ];
  return { lst, entries };
}

test('★ 函数清单与 EA 归属：起止行、`jmp` 尾跳、具名函数', () => {
  const { lst } = fixture();
  const inv = functionInventory(lst);
  assert.equal(inv.functions.length, 13, '13 个 proc near（含 1 个具名 + 1 个库桩 + 1 个调库桩的引擎函数 + 3 个收窄判据的样本）');
  const f = inv.bySym.get('sub_401000');
  assert.equal(f.ea, 0x401000);
  assert.equal(f.fromLine, 1);
  assert.equal(f.toLine, 5);
  assert.deepEqual(f.callees, ['sub_401100'], 'call 目标要进 callees');
  assert.equal(f.indirectCallSites, 1, '`call eax` 必须计入间接调用点');
  assert.deepEqual(inv.bySym.get('sub_401600').callees, ['sub_401500'], '`jmp sub_…`（尾跳）也算一条边');

  // 归属：起点 / 体内 / 之后 / 之前
  assert.equal(functionOfEa(inv, 0x401000), 'sub_401000');
  assert.equal(functionOfEa(inv, 0x40100a), 'sub_401000', '函数体内（含最后一条指令）都归属它');
  assert.equal(
    functionOfEa(inv, 0x4010ff),
    null,
    '★ 函数末尾之后、下一个函数之前是**空隙** ⇒ 不归属任何人（只看"下一个函数起点"会把它错归给前一个）',
  );
  assert.equal(functionOfEa(inv, 0x401100), 'sub_401100');
  assert.equal(functionOfEa(inv, 0x400fff), null, '第一个函数之前 ⇒ null');
  assert.equal(functionOfEa(inv, 0x401405), 'sub_401400');
  assert.equal(pickListing(path.dirname(lst)), lst, 'pickListing 必须找到那份 .lst');
});

test('★ 库代码分类：/GS 桩那样的"只往库里转一手"必须被摘出分桶与前沿，且摘出的看得见', () => {
  const { lst, entries } = fixture();
  const r = coverage(entries, { lstFile: lst });
  // 桩本体被摘出（理由可读）
  const lib = new Map(r.honest.libraryCandidates.map((x) => [x.sym, x.reasons]));
  assert.ok(lib.has('sub_401800'), '无内存操作数 + 不调 sub_ + 尾跳非 sub_ 目标 ⇒ 疑似库代码');
  assert.ok(lib.get('sub_401800').includes('thin-forwarder-to-library'));
  // ★ 它**不许**出现在分桶/前沿里（"下一步该取证谁"不能指向库代码）
  assert.ok(!r.allRows.some((x) => x.sym === 'sub_401800'), '库代码不进逐函数表（= 不进任何桶）');
  assert.ok(!r.frontier.direct.some((x) => x.sym === 'sub_401800'));
  assert.ok(!r.frontier.mostUsedUnregistered.some((x) => x.sym === 'sub_401800'));
  // ★ 而**碰内存**的引擎函数不许被摘（判据要窄：宁可漏，不可滥）
  assert.ok(!lib.has('sub_401900'), '有 `[` 内存操作数的函数不算"只往库里转一手"');
  assert.ok(r.allRows.some((x) => x.sym === 'sub_401900'), '引擎函数要在桶里');
  // ★ 库代码算"无需分析"：一个只调库桩（+CRT）的引擎函数，callee 闭包不该因此算不全
  assert.equal(r.universe.library, 3, '合成语料里恰好 3 个库候选（1 个库桩 + 2 个真 STL 形态）');
  assert.equal(r.universe.counted, 9, '宇宙 = 12 个 sub_ 减去 3 个库候选');
});

test('★ 库候选的收窄（`calls-stl-internal` 的假阳性）：Engine 尺度位移 ⇒ 判回引擎代码，且"取消"看得见', () => {
  const { lst, entries } = fixture();
  const r = coverage(entries, { lstFile: lst, top: 0 });
  const lib = new Map(r.honest.libraryCandidates.map((x) => [x.sym, x.reasons]));
  const cancelled = r.honest.libraryCandidatesCancelled;

  // ★ 假阳性：内联的 `vector<T>::operator[]` 抛出口 + `[ecx+5D880h]` ⇒ **不许**判成库候选
  assert.ok(!lib.has('sub_401A00'), '体内有 ≥0x1000 的非栈位移 ⇒ `calls-stl-internal` 不作数（这正是 `sub_42B4B0` 的假阳性）');
  // ★ 而且它必须**看得见**：既在"被取消"名单里（带理由 + 那个位移），又在引擎宇宙的分桶里
  assert.deepEqual(cancelled.map((x) => x.sym).sort(), ['sub_401A00'], '被取消的候选单列，不许静默改口径');
  assert.equal(cancelled[0].maxNonStackDisp, 0x5d880, '取消的理由要带上现算出来的那个位移（可复核）');
  assert.ok(cancelled[0].reasons.includes('calls-stl-internal'));
  assert.ok(r.allRows.some((x) => x.sym === 'sub_401A00'), '取消后它回到引擎宇宙（于是分桶与两张榜里都能看到）');
  assert.ok(!r.frontier.mostUsedUnregistered.some((x) => x.sym === 'sub_401A00'), '★ 它没人调用 ⇒ 不假造前沿（收窄只让该看见的看见）');

  // ★ 对照 ①：位移只有 SSO 量级 ⇒ 仍算库候选（收窄不许滥杀）
  assert.ok(lib.has('sub_401B00'), '位移 0x10h（SSO 量级）⇒ 还是库候选');
  assert.ok(lib.get('sub_401B00').includes('calls-stl-internal'));
  assert.ok(!cancelled.some((x) => x.sym === 'sub_401B00'));
  // ★ 对照 ②：**栈基址**的大位移不算引擎尺度（栈帧可以很大）
  assert.ok(lib.has('sub_401C00'), '`[ebp-4000h]` 是栈 ⇒ 不算引擎尺度位移，仍算库候选');
});

test('★ 分桶：起点锚 / 体内锚 / callee 未登记 / 互递归（最大不动点）', () => {
  const { lst, entries } = fixture();
  const r = coverage(entries, { lstFile: lst, top: 0 });
  assert.deepEqual(Object.keys(r.buckets).sort(), [...TIERS].sort(), '桶名是闭集合');
  assert.equal(r.buckets.complete, 4, '收口 = R1 + R2 + 互递归的两个（R5/R6）');
  assert.deepEqual(r.completeList, ['sub_401000', 'sub_401100', 'sub_401500', 'sub_401600']);
  assert.equal(r.buckets['partial-rooted'], 1, 'R3：锚在起点但 callee 未登记');
  assert.deepEqual(r.partialRootedList, ['sub_401200']);
  assert.equal(r.buckets['partial-spotty'], 1, 'R4：只有体内锚 ⇒ 没人把它当整体看过');
  assert.deepEqual(r.partialSpottyList, ['sub_401400']);
  assert.equal(r.buckets.unobserved, 3, '未登记 = sub_401300 + sub_401900 + sub_401A00（具名的 NamedThing 与库候选 sub_401800/401B00/401C00 都不计入）');
  // ★ 具名函数不进宇宙（问题是按 `sub_` 问的），但要在报告里点名
  assert.ok(!r.allRows.some((x) => x.sym === 'NamedThing'), '具名函数不计入 sub_ 宇宙');
  assert.deepEqual(r.universe.named, ['NamedThing']);
  assert.equal(r.observed, 6);
});

test('★ 前沿：只列"未登记却被已登记调用"的（已登记的不许混进来）', () => {
  const { lst, entries } = fixture();
  const r = coverage(entries, { lstFile: lst, top: 0 });
  assert.deepEqual(r.frontier.direct.map((x) => x.sym), ['sub_401300'], '前沿只有它');
  assert.deepEqual(r.frontier.direct[0].callers, ['sub_401200']);
  assert.equal(r.frontier.direct[0].calledBy, 1, 'calledBy = 已登记调用方数（它挡了多少活）');
  assert.equal(r.frontier.direct[0].calledByAll, 1, 'calledByAll = 全语料调用方数（它用得多广）');
  assert.equal(r.frontier.directCount, 1);
  // ★ 第二张榜：全语料用得最多、而自己没登记（叶子助手通常在这张榜上，见 `sub_408050` 的 110 个调用方）
  assert.deepEqual(r.frontier.mostUsedUnregistered.map((x) => x.sym), ['sub_401300']);
  assert.equal(r.frontier.mostUsedUnregistered[0].calledByAll, 1);
  assert.equal(r.frontier.mostUsedUnregistered[0].callSitesAll, 1, 'callSitesAll = 调用**处**数（同一函数调两次算两处）');
  // 传递前沿：401300 之后没有更多（它是叶子）
  assert.equal(r.frontier.transitiveCount, 1);
  assert.deepEqual(r.frontier.byDepth, { 1: 1 });
});

test('★ 诚实项必须报出来：间接调用点 / 归属不到 / 无 callee', () => {
  const { lst, entries } = fixture();
  const r = coverage(entries, { lstFile: lst, top: 0 });
  assert.equal(r.honest.indirectCallSites, 1, '`call eax` 必须计数（否则"闭包完整"会被当成事实）');
  assert.equal(r.anchors.unattributed, 0);
  assert.equal(r.anchors.atStart, 5, '起点锚 5 个（R1/R2/R3/R5/R6）');
  assert.equal(r.anchors.insideBody, 1, '体内锚 1 个（R4）');
  assert.equal(r.universe.noCallee, r.allRows.filter((x) => x.callees === 0).length);

  // 锚 EA 不落在任何函数里 ⇒ 单列，不许静默丢
  const r2 = coverage([...entries, rec('R7', 0x999999)], { lstFile: lst, top: 0 });
  assert.equal(r2.anchors.unattributed, 1);
  assert.deepEqual(r2.honest.unattributedAnchors.map((x) => x.id), ['R7']);
});

test('★ 历史行不参与：被 `replaces` 取代 / `retracted` 的记录不许撑起覆盖度', () => {
  const { lst, entries } = fixture();
  const revoked = entries.map((e) => (e.id === 'R3' ? { ...e, effective: 'retracted' } : e));
  const r = coverage(revoked, { lstFile: lst, top: 0 });
  assert.equal(r.buckets['partial-rooted'], 0, '撤回掉的记录不再让 sub_401200 算"有登记"');
  assert.equal(r.buckets.unobserved, 4, '于是 sub_401200 也回到未登记');
  // ★ 因果链：撤回那条记录，**它指向的前沿也一起塌掉** —— 因为 sub_401200 自己都不再算"已登记"，
  //   于是"被已登记函数调用"这个条件对 sub_401300 也不再成立。
  assert.equal(r.frontier.directCount, 0, '前沿随记录一起塌');
  assert.equal(r.frontier.transitiveCount, 0);
});
