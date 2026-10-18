/** @env pure @kind gate @why 折叠层把 `lea reg,[一跳基址-K]` 的负位移当成"二级偏移"⇒ 判定器会把它记成"某类型缺 0x-4 这个成员"（假缺口），必须在 access 层就置 null */
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