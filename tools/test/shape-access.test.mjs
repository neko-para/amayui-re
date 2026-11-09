/** @env pure @kind gate @suite toolchain @why 折叠层把 `lea reg,[一跳基址-K]` 的负位移当成"二级偏移"⇒ 判定器会把它记成"某类型缺 0x-4 这个成员"（假缺口），必须在 access 层就置 null */
/**
 * tools/test/shape-access.test.mjs —— **access 层**（`analyzeLines`）的最小守卫。
 *
 * 为什么需要这个文件：此前 access 层**没有独立测试**，所有守卫都在 claims 层（下游）。
 * 于是"折叠层把负位移写进 pointeeOff"这种**上游**错误只能在 claims 里被兜住（记一条负偏移的残差），
 * 而改 access 的代码**没有任何用例会红** —— 等于没法改。
 *
 * 钉住的一件事：`lea reg,[<一跳基址>-K]` 折出来的 `pointeeOff` 必须是 `null`（判不了），
 * 不能是负数。合成复现见 `.tmp/shape/probe-negative-pointee.mjs`；现场是 `T_5d8bc+0x-4`（`0x40EA97`/`0x40EAA2`）。
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';

import { analyzeLines } from '../lib/shape-access.mjs';
import { offsetUnusable } from '../lib/shape-claims.mjs';

const L = [
  '.text:00402000 sub_402000 proc near',
  '.text:00402000 mov     esi, ecx',
  '.text:00402002 mov     eax, [esi+100h]',   // 一跳：eax = *(this+0x100)
  '.text:00402008 lea     ecx, [eax-4]',      // 取址算术带 −4（现场 0x40EA97）
  '.text:0040200B mov     ecx, [ecx]',        // 以 eax-4 为基址读第二跳
  '.text:0040200D retn',
  '.text:0040200D sub_402000 endp',
];

test('★access-01 `lea reg,[一跳基址-K]` 的负位移不得变成负的 `pointeeOff`（判不了 ⇒ null）', () => {
  const r = analyzeLines(L, 0, L.length - 1, 'ecx', {});
  const withPath = r.sites.filter((s) => (s.pointeePath?.length ?? 0) >= 1);
  assert.ok(withPath.length >= 1, `这两条站点必须带 pointee 路径：${JSON.stringify(r.sites.map((s) => s.ea))}`);
  for (const s of withPath) {
    assert.ok(!(typeof s.pointeeOff === 'number' && s.pointeeOff < 0),
      `负的 pointeeOff 会变成"缺成员"假缺口（EA ${s.ea?.toString(16)} ⇒ ${s.pointeeOff}）`);
    assert.equal(s.pointeeOff, null, `应当置 null（判不了）：EA ${s.ea?.toString(16)}`);
  }
});

test('★access-02 正向位移不受影响（`lea reg,[一跳基址+K]` 仍要给出正常偏移）', () => {
  const L2 = [
    '.text:00402100 sub_402100 proc near',
    '.text:00402100 mov     esi, ecx',
    '.text:00402102 mov     eax, [esi+100h]',
    '.text:00402108 lea     ecx, [eax+8]',     // 正位移：内嵌子对象基址的常见形态
    '.text:0040210B mov     ecx, [ecx]',
    '.text:0040210D retn',
    '.text:0040210D sub_402100 endp',
  ];
  const r = analyzeLines(L2, 0, L2.length - 1, 'ecx', {});
  const hit = r.sites.find((s) => s.ea === 0x40210b);
  assert.ok(hit, '第二条站点要在');
  assert.equal(hit.pointeeOff, 8, `正位移要原样保留：${hit.pointeeOff}`);
});

test('★access-03 强度削减：`mov edx,eax; shl edx,4; sub edx,eax` ⇒ 有效步长 = 15×8 = 0x78（不是 8）', () => {
  const L3 = [
    '.text:00402300 sub_402300 proc near',
    '.text:00402300 mov     eax, [ecx+5D880h]',
    '.text:00402306 mov     edx, eax',
    '.text:00402308 shl     edx, 4',
    '.text:0040230B sub     edx, eax',
    '.text:0040230D mov     [ecx+edx*8+5D8BCh], esi',
    '.text:00402315 retn',
    '.text:00402315 sub_402300 endp',
  ];
  const r = analyzeLines(L3, 0, L3.length - 1, 'ecx', {});
  const hit = r.sites.find((s) => s.ea === 0x40230d);
  assert.ok(hit, '站点要在');
  assert.equal(hit.memScale, 120, `真实步长 0x78：${hit.memScale}（裸 scale 是 8 ⇒ 会与 size(T_5d8bc)=0x1C 假冲突）`);
});

test('★access-04 普通索引不受影响：`[ecx+esi*4+100h]` 的 memScale 仍是 4', () => {
  const L4 = [
    '.text:00402400 sub_402400 proc near',
    '.text:00402400 mov     esi, eax',
    '.text:00402402 mov     edx, [ecx+esi*4+100h]',
    '.text:00402409 retn',
    '.text:00402409 sub_402400 endp',
  ];
  const r = analyzeLines(L4, 0, L4.length - 1, 'ecx', {});
  const hit = r.sites.find((s) => s.ea === 0x402402);
  assert.ok(hit, '站点要在');
  assert.equal(hit.memScale, 4, `普通索引的步长不该被改：${hit.memScale}`);
});
test('★access-05 串指令（rep movs*/stos*）之后 `esi/edi` 必须失效：`[esi+400h]` 不得再被解析成对象相对位移', () => {
  const L5 = [
    '.text:00402700 sub_402700 proc near',
    '.text:00402700 mov     esi, ecx',
    '.text:00402702 rep movsd',
    '.text:00402704 mov     edx, [esi+400h]',
    '.text:0040270A mov     eax, [esi+500h]',
    '.text:00402710 retn',
    '.text:00402710 sub_402700 endp',
  ];
  const r = analyzeLines(L5, 0, L5.length - 1, 'ecx', {});
  for (const s of r.sites) {
    assert.ok(!(typeof s.absDisp === 'number' && s.absDisp > 0x100),
      `rep movsd 之后 esi 已前进，[esi+${s.absDisp?.toString(16)}] 不得算成对象相对位移（EA ${s.ea?.toString(16)}）`);
  }
  assert.ok(r.sites.length >= 1, '至少一个站点要在');
});

test('★access-06 `stos*` 只影响 `edi`，不影响 `esi`', () => {
  const L6 = [
    '.text:00402800 sub_402800 proc near',
    '.text:00402800 mov     esi, ecx',
    '.text:00402802 stosd',
    '.text:00402804 mov     eax, [esi+600h]',
    '.text:0040280A retn',
    '.text:0040280A sub_402800 endp',
  ];
  const r = analyzeLines(L6, 0, L6.length - 1, 'ecx', {});
  const hit = r.sites.find((s) => s.ea === 0x402804);
  assert.ok(hit, '站点要在');
  assert.equal(hit.absDisp, 0x600, `stos 不推进 esi ⇒ [esi+600h] 仍应是对象相对位移：${hit.absDisp}`);
});

test('★access-07 索引解不出的 `lea` ⇒ 目标寄存器是"`this`+未知项"：下游 `[reg+K]` 进「位移不可用」，⛔ 不许当 `obj0+K`', () => {
  const L7 = [
    '.text:00402A00 sub_402A00 proc near',
    '.text:00402A00 mov     edi, ecx',
    '.text:00402A02 call    sub_41BF50',   // 返回值 ⇒ edx 的来源是"调用返回值"（解不出）
    '.text:00402A08 mov     edx, eax',
    '.text:00402A0B lea     esi, [edi+edx*4]',   // 现场形态（0x422527：跳表算出的基址）
    '.text:00402A0E mov     eax, [esi+0Ch]',     // 下游：曾被记成 obj0+0xc
    '.text:00402A11 add     eax, [esi+4]',       // 同族：曾被记成 obj0+0x4
    '.text:00402A14 retn',
    '.text:00402A14 sub_402A00 endp',
  ];
  const r = analyzeLines(L7, 0, L7.length - 1, 'ecx', {});
  for (const ea of [0x402a0e, 0x402a11]) {
    const hit = r.sites.find((s) => s.ea === ea);
    assert.ok(hit, `站点 0x${ea.toString(16)} 要在`);
    assert.equal(offsetUnusable(hit), 'index-unresolved',
      `0x${ea.toString(16)} 的位移少了索引那一项 ⇒ 只能进「位移不可用」（实测 abs=${hit.absDisp?.toString(16)} problems=${JSON.stringify(hit.problems)}）`);
  }
  // 对照：`mov esi, ecx` 之后同样形态**必须**仍是普通 obj0 偏移（⛔ 别把"位移不可用"做成整类误杀）
  const L8 = [
    '.text:00402B00 sub_402B00 proc near',
    '.text:00402B00 mov     esi, ecx',
    '.text:00402B02 mov     eax, [esi+0Ch]',
    '.text:00402B05 retn',
    '.text:00402B05 sub_402B00 endp',
  ];
  const r8 = analyzeLines(L8, 0, L8.length - 1, 'ecx', {});
  const hit8 = r8.sites.find((s) => s.ea === 0x402b02);
  assert.ok(hit8, '对照站点要在');
  assert.equal(offsetUnusable(hit8), null, '`esi` 就是 `this` 时不带未知项 ⇒ 仍是可用偏移');
  assert.equal(hit8.absDisp, 0xc);
});
/**
 * ★ T2：未解析基址必须带**来路**（`baseWhy`）—— 四类各有明确的产生点，且落在闭集合里。
 * ★ 为什么：守恒③ 只报一个总数（+ 按寄存器分组）⇒ `call` 返回值（要新表达方式）、`stack` 出参（要另一套证据）、
 *   `table` 表项（要索引证据）、`arg` 参数位（要载体）在报告里长得一样，**派不了活**。
 * ⛔ `baseWhy` 只描述"值怎么变成未知的"，**不是类型判断**（不许据此猜类）。
 */
test('★access-08 未解析基址的来路（call / stack / table / arg）必须分得开，且落在闭集合里', async () => {
  const { BASE_WHY } = await import('../lib/shape-access.mjs');
  const L8 = [
    '.text:00403000 sub_403000 proc near',
    '.text:00403000 mov     esi, ecx',
    '.text:00403003 call    sub_999999',            // eax/edx/ecx 被杀 ⇒ 来路 call
    '.text:00403008 mov     ebx, [eax+10h]',        // 基址 = call 返回值
    '.text:0040300B mov     eax, [ebp+var_8]',      // 栈槽没存过 ⇒ 来路 stack
    '.text:0040300E mov     dword ptr [eax+4], 1',  // 基址 = 栈出参
    '.text:00403012 mov     eax, [esi+edx*4+200h]', // 索引 edx 未解出 ⇒ 值是"表项"
    '.text:00403019 mov     ecx, [eax+8]',          // 基址 = 表查询结果
    '.text:0040301C mov     eax, [edi+0Ch]',        // edi 从没写过 ⇒ 参数位
    '.text:0040301F retn',
    '.text:0040301F sub_403000 endp',
  ];
  const r = analyzeLines(L8, 0, L8.length - 1, 'ecx', {});
  const why = (ea) => r.sites.find((s) => s.ea === ea)?.baseWhy;
  assert.equal(why(0x403008), 'call', `call 之后用 eax 当基址 ⇒ why=call；实得 ${why(0x403008)}`);
  assert.equal(why(0x40300e), 'stack', `从没存过的栈槽读回来当基址 ⇒ why=stack；实得 ${why(0x40300e)}`);
  assert.equal(why(0x403019), 'table', `表项（索引未解出）当基址 ⇒ why=table；实得 ${why(0x403019)}`);
  assert.equal(why(0x40301c), 'arg', `从没写过的寄存器当基址 ⇒ why=arg；实得 ${why(0x40301c)}`);
  for (const s of r.sites) {
    if (!s.problems.includes('base')) { assert.equal(s.baseWhy, null, `已解析的站点不该带 baseWhy（EA ${s.ea?.toString(16)}）`); continue; }
    assert.ok(BASE_WHY.includes(s.baseWhy), `baseWhy 落在闭集合外：${s.baseWhy}（EA ${s.ea?.toString(16)}）`);
  }
});

/**
 * ★ T3：块搬移 / 清零的 **extent**（`{基址, 字节数, EA}`）—— `rep movs*` / `memcpy` / `memset`。
 * ★ 为什么：有些对象**一条字段访问都没有**，唯一的 width 证据就是"整块搬 N 字节"；没有它，
 *   1200 字节副块 / 740 字节记录 / 572 字节值表的宽度只能靠人读 `.lst`。
 * ⛔ 只给宽度、不解成员、不放宽 G4；计数解不出时**照记站点**并写 `count-unknown`（⛔ 不许静默丢）。
 */
test('★access-09 块搬移 / 清零的 extent：字节数 / 源 / 目的都要给（含"计数判不了"照记）', () => {
  const L9 = [
    '.text:00404000 sub_404000 proc near',
    '.text:00404000 mov     ebx, ecx',
    '.text:00404002 lea     esi, [ebx+3F5Ch]',   // 源
    '.text:00404008 lea     edi, [ebx+440Ch]',   // 目的
    '.text:0040400E mov     ecx, 12Ch',          // 计数（IDA 不补前导 0）
    '.text:00404013 rep movsd',                  // ⇒ 0x12C × 4 = 1200 B
    '.text:00404015 lea     eax, [ebx+4F458h]',
    '.text:0040401B lea     edx, [ebx+54278h]',
    '.text:00404021 push    4E20h',              // 20000
    '.text:00404026 push    eax',
    '.text:00404027 push    edx',
    '.text:00404028 call    _memcpy',
    '.text:0040402D add     esp, 0Ch',
    '.text:00404030 lea     eax, [ebx+54278h]',
    '.text:00404036 push    100h',
    '.text:0040403B push    0',
    '.text:00404040 push    eax',
    '.text:00404041 call    _memset',
    '.text:00404046 add     esp, 0Ch',
    '.text:00404049 mov     ecx, [ebx+0AA514h]', // 计数是个 load ⇒ 判不了
    '.text:0040404F rep stosd',
    '.text:00404051 retn',
    '.text:00404051 sub_404000 endp',
  ];
  const r = analyzeLines(L9, 0, L9.length - 1, 'ecx', {});
  const at = (ea) => r.extents.find((e) => e.ea === ea);
  const mv = at(0x404013);
  assert.ok(mv, `rep movsd 必须被记成 extent：${JSON.stringify(r.extents.map((e) => e.ea.toString(16)))}`);
  assert.equal(mv.bytes, 0x4b0, `0x12C × 4 = 1200 B（实得 ${mv.bytes}）`);
  assert.equal(mv.dst?.abs, 0x440c);
  assert.equal(mv.src?.abs, 0x3f5c);
  const mc = at(0x404028);
  assert.ok(mc, 'memcpy 必须被记成 extent');
  assert.equal(mc.bytes, 20000);
  assert.equal(mc.dst?.abs, 0x54278);
  assert.equal(mc.src?.abs, 0x4f458);
  const ms = at(0x404041);
  assert.ok(ms, 'memset 必须被记成 extent');
  assert.equal(ms.bytes, 256);
  const st = at(0x40404f);
  assert.ok(st, '计数判不了的块清零也要**照记**（⛔ 不许因为它解不出就丢）');
  assert.equal(st.bytes, null);
  assert.equal(st.why, 'count-unknown');
});
