/** @env pure @kind gate @suite toolchain @why 平台面普查的三张表是靠这几条文法抽出来的；任何一条退化都会让"平台面有多大"悄悄错 */
/**
 * tools/test/platform.test.mjs — `pnpm tools platform census` 的**纯函数**守卫
 *
 * 守的是**抽取文法**，不是语料的当前计数：
 *   ① 导入块：`; Imports from` 分组、`dd ?` 与 `extrn …:dword` 两种形态、`align` 结束本组
 *   ② 调用点：`j_` / `ds:<Name>` / 裸名三种形态；`ds:dword_*` **不计入**
 *   ③ vtable：`(*(…)(… + off))(` 的 offset 取**最内层**；没有该形态的计入 `withoutOffset`
 *   ④ 动态解析：`push offset … ; "…"` 的字符串；窗口里没有 ⇒ `arg=null`（不填占位名）
 *   ⑤ 聚合：`j_<Name>_<n>` 仅在精确匹配失败时归一；`callForm` 的 `both` / `direct`
 *   ⑥ 确定性：同一 fixture 两次序列化逐字节相同
 *
 * ★ 不读 `corpus/`（`@env pure`）；不断言"AGE 有几个 DLL"——那是语料当前值，不是约定。
 *
 * 运行：`node --test tools/test/platform.test.mjs`
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';

import {
  aggregateImports,
  buildCensus,
  censusDocument,
  importSymbol,
  parseCallSites,
  parseDynamicSites,
  parseImportGroups,
  parseVtableSites,
} from '../platform.mjs';

// ── ① 导入块 ─────────────────────────────────────────────────────────────

const IMPORT_LINES = [
  '.data:0051D000 ; Imports from d3d9.dll',
  '.data:0051D000 ; IDirect3D9 *(__stdcall *Direct3DCreate9)(UINT SDKVersion)',
  '.data:0051D000 Direct3DCreate9 dd ?',
  '.data:0051D004 Direct3DCreate9Ex dd ?',
  '.data:0051D008 align 8',
  '.data:0051D010 ; Imports from KERNEL32.dll',
  '.data:0051D010 GetModuleHandleA dd ?',
  '.data:0051D014 LoadLibraryA dd ?',
  '.data:0051D018 CreateEventA dd ?',
  '.data:0051D01C align 4',
  '.idata:10154498 ; Imports from ADVAPI32.dll',
  '.idata:10154498 extrn __imp_RegQueryValueExA:dword',
  '.idata:1015449C extrn RegSetValueExA:dword',
  '.idata:101544A0 db 0',
];

test('① 导入块：分组 / 两种 decl / align 结束本组 / __imp_ 归一', () => {
  const { groups } = parseImportGroups(IMPORT_LINES);
  assert.equal(groups.length, 3, '应当识别 3 个 DLL 组');
  assert.deepEqual(groups.map((g) => g.dll), ['d3d9.dll', 'KERNEL32.dll', 'ADVAPI32.dll']);
  assert.deepEqual(
    groups.map((g) => g.entries.length),
    [2, 3, 2],
    'align / db 之后的无名数据不许记到上一个 DLL',
  );
  assert.deepEqual(
    groups.map((g) => g.entries.map((e) => e.decl)),
    [['dd', 'dd'], ['dd', 'dd', 'dd'], ['extrn', 'extrn']],
  );
  assert.equal(groups[0].entries[0].name, 'Direct3DCreate9');
  assert.equal(groups[0].entries[0].ea, 0x51d000);
  assert.equal(importSymbol('__imp_RegQueryValueExA'), 'RegQueryValueExA');
  assert.equal(importSymbol('RegSetValueExA'), 'RegSetValueExA', '没有前缀就原样返回');
});

// ── ② 调用点 ─────────────────────────────────────────────────────────────

const CALL_LINES = [
  '.text:00401000 call    j_Direct3DCreate9',
  '.text:00401010 call    ds:GetModuleHandleA',
  '.text:00401020 call    ds:dword_55E1B4',
  '.text:00401030 call    LoadLibraryA',
  '.text:00401040 call    eax',
  '.text:00401050 jmp     j_Sleep',
  '.text:00401060 call    j_CreateEventA_2',
  '.text:00401070 call    j_NoSuchImport',
];

test('② 调用点：j_ / ds:名字 / 裸名三种形态；ds:dword_* 不计入', () => {
  const { sites, jmpSites, ignored } = parseCallSites(CALL_LINES);
  assert.deepEqual(
    sites.map((s) => [s.form, s.name]),
    [
      ['thunk', 'Direct3DCreate9'],
      ['iat', 'GetModuleHandleA'],
      ['direct', 'LoadLibraryA'],
      ['thunk', 'CreateEventA_2'],
      ['thunk', 'NoSuchImport'],
    ],
  );
  assert.equal(ignored.dsAutoData, 1, '`ds:dword_55E1B4` 是全局函数指针，不是命名导入');
  assert.equal(ignored.register, 0);
  assert.ok(ignored.indirect >= 1, '`call eax` 必须计入"间接"而不是被当成符号');
  assert.equal(jmpSites.length, 1, '`jmp` 尾跳单列，不进调用点表');
});

// ── ③ vtable ─────────────────────────────────────────────────────────────

const C_LINES = [
  '  v1 = (*(void (__stdcall **)(int))(*(_DWORD *)v13 + 8))(v13);',
  '  v2 = (*(int (__thiscall **)(_DWORD *))(*this[1] + 0x44))(this[1]);',
  '  v3 = (*(int (__thiscall **)(_DWORD *, char *))(this[174405] + 4))(this[174405], x);',
  '  v4 = (*(void (__cdecl **)(LPVOID))(*(_DWORD *)ppv + 8))(ppv);',
  '  v5 = (*(int (__thiscall **)(_DWORD))(*v6))(v6);',
];

test('③ vtable：offset 取最内层；没有 `+ off))(` 的计入 withoutOffset', () => {
  const parsed = parseVtableSites(C_LINES);
  assert.deepEqual(parsed.sites.map((s) => s.offset), [8, 0x44, 4, 8]);
  assert.equal(parsed.candidateLines, 5);
  assert.equal(parsed.withoutOffset, 1, '`(*v6)(v6)` 不是 vtable 站点');
});

// ── ④ 动态解析 ───────────────────────────────────────────────────────────

const DYN_LINES = [
  '.text:00401000 push    offset aDdrawDll ; "DDRAW.DLL"',
  '.text:00401005 call    ds:LoadLibraryA',
  '.text:00401010 push    offset ProcName ; "DirectDrawCreate"',
  '.text:00401015 call    ds:GetProcAddress',
  '.text:00401020 push    offset ProcName2 ; "DirectSoundCreate"',
  '.text:00401025 push    esi',
  '.text:00401030 call    ds:GetProcAddress',
  // 故意留出 > 12 行的空档：窗口里没有任何 push 字符串 ⇒ 未解析
  '.text:00402000 push    esi',
  '.text:00402001 ; filler',
  '.text:00402002 ; filler',
  '.text:00402003 ; filler',
  '.text:00402004 ; filler',
  '.text:00402005 ; filler',
  '.text:00402006 ; filler',
  '.text:00402007 ; filler',
  '.text:00402008 ; filler',
  '.text:00402009 ; filler',
  '.text:0040200A ; filler',
  '.text:0040200B ; filler',
  '.text:0040200C ; filler',
  '.text:0040200D ; filler',
  '.text:0040200E call    ds:GetProcAddress',
];

test('④ 动态解析：最近的 push 字符串 + 窗口外未解析（不填占位名）', () => {
  const parsed = parseDynamicSites(DYN_LINES, { window: 12 });
  assert.equal(parsed.sites.length, 4);
  assert.deepEqual(parsed.sites[0], {
    api: 'LoadLibraryA',
    ea: 0x401005,
    line: 2,
    arg: 'DDRAW.DLL',
    prevArg: null,
    caller: null,
  });
  assert.equal(parsed.sites[1].arg, 'DirectDrawCreate');
  assert.equal(parsed.sites[1].prevArg, 'DDRAW.DLL');
  assert.equal(parsed.sites[2].arg, 'DirectSoundCreate');
  assert.equal(parsed.sites[3].arg, null, '窗口里没有字符串 ⇒ null，不许猜');
  assert.equal(parsed.unresolved, 1);
});

// ── ⑤ 聚合 ───────────────────────────────────────────────────────────────

test('⑤ 聚合：精确匹配优先；j_<Name>_<n> 只在失败时归一；callForm 正确', () => {
  const parsed = parseImportGroups(IMPORT_LINES);
  const calls = parseCallSites(CALL_LINES);
  const agg = aggregateImports(parsed, calls.sites, calls.jmpSites);
  const flat = agg.byDll.flatMap((g) => g.functions);
  const byName = new Map(flat.map((f) => [f.name, f]));

  assert.equal(byName.get('Direct3DCreate9').calls.thunk, 1);
  assert.equal(byName.get('Direct3DCreate9').callForm, 'thunk');
  assert.equal(byName.get('GetModuleHandleA').calls.iat, 1);
  assert.equal(byName.get('GetModuleHandleA').callForm, 'iat');
  assert.equal(byName.get('LoadLibraryA').calls.direct, 1);
  assert.equal(byName.get('LoadLibraryA').callForm, 'direct');
  assert.equal(byName.get('CreateEventA').calls.thunk, 1, '`j_CreateEventA_2` 归到 `CreateEventA`');
  assert.equal(byName.get('CreateEventA').callForm, 'thunk');
  assert.equal(byName.get('RegSetValueExA').calls.total, 0);
  assert.equal(byName.get('RegSetValueExA').callForm, null);

  // 归不到导入的：`j_NoSuchImport` 不在导入表里 ⇒ 计入 unattributed，不许瞎归
  assert.equal(agg.unattributed.thunk, 1);
  assert.deepEqual(agg.unattributed.thunkNames, ['NoSuchImport']);
});

// ── ⑥ 汇总 / 确定性 ─────────────────────────────────────────────────────

function fixtureCensus(key = 'fixture') {
  return buildCensus({
    key,
    title: 'FIXTURE',
    lstLines: [...IMPORT_LINES, ...DYN_LINES],
    cLines: C_LINES,
  });
}

test('⑥ buildCensus：三张表齐全；同一输入两次序列化逐字节相同', () => {
  const a = fixtureCensus();
  const b = fixtureCensus();
  assert.equal(a.imports.dllCount, 3);
  assert.equal(a.imports.functionCount, 7);
  assert.equal(a.vtable.siteCount, 4);
  assert.equal(a.dynamic.source, 'lst');
  assert.equal(a.dynamic.sites.length, 4);
  assert.ok(a.honesty.length > 0, '必须带诚实项');

  assert.equal(JSON.stringify(a), JSON.stringify(b), '同一输入必须确定');
  assert.equal(
    JSON.stringify(censusDocument([a])),
    JSON.stringify(censusDocument([b])),
  );
});

test('⑥ censusDocument：模块按 key 升序（与传入顺序无关）', () => {
  const doc = censusDocument([fixtureCensus('z'), fixtureCensus('a')]);
  assert.deepEqual(doc.modules.map((m) => m.module), ['a', 'z']);
  assert.equal(doc.schema, 1);
  assert.equal(doc.tool, 'platform');
});
