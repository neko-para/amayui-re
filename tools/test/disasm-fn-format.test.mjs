/** @env pure @kind gate @why 语料文法（`.lst` 行 / `.c` 定义判定）或两侧配对被改坏：会静默给出"看起来对"的行区间与参数偏移 */
/**
 * tools/test/disasm-fn-format.test.mjs —— **文法与配对**的机械判据（**合成样本**，不需要语料）
 *
 * ## 为什么要有这一档（`@env pure`）
 * `disasm-fn` 的两侧切分全靠"文法判据"，而其中**每一条都踩过坑**：
 * 1. **定义 vs 原型**：旧判据是"行尾 `;`"，于是**带尾注释的原型**（`…); // idb`）被当成定义，
 *    该符号的行区间落在文件顶部的**原型区**（实测 40 个符号）⇒ 调用方会把一行原型当成"C 体"，**不报错**。
 *    现判据：列 0 + 以 `)` 结尾 + **下一个非空行恰为 `{`**；原型**不以 `)` 结尾** ⇒ 天然免疫。
 * 2. **跨行签名**：`int __thiscall sub_X(int a1,` / `int a2)` / `{` 这种签名真实存在，
 *    "只认单行"的判据会**整条漏掉**它（表现与"Hex-Rays 没反编译"一模一样）。
 * 3. **`@<reg>` 标注**：`__usercall` / `__userpurge` 把寄存器写在**参数**上（`_BYTE *a1@<edx>`），
 *    名字里的 `@<eax>` 不剥掉、参数上的 `@<reg>` 不认出来 ⇒ 定义对不上 `.lst`、偏移全错。
 * 4. **参数配对**：`.lst` 与 `.c` 的参数**必须按名字优先、顺序兜底**（实测反例 `sub_401100`）。
 * 5. **`call ds:dword_X`**：旧的间接调用正则**不含 `ds:`** ⇒ 这一整类调用点被**静默漏出**调用图（实测 69 处）。
 * 6. **数据续行**：一个标签 + 后续无名字的行 = **一个** item（长度/表项都靠它）；
 *    引号里的逗号不先藏掉，长度会虚高。
 *
 * 这些判据**只依赖文法**，所以这一档用**合成样本**跑（快、稳、不要求解压语料）。
 * 真语料上的契约（覆盖率 / join 往返 / 具体已知符号）在 `disasm-fn.assets.test.mjs`。
 *
 * 运行：`pnpm test`
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';

import { buildCatalog, findFunction } from '../lib/catalog.mjs';
import { AUTO_NAME_EA_RE, eaOfAutoName, parseListingLine, undecorate } from '../lib/lst-format.mjs';
import { scanDecompiled, scanListing } from '../lib/catalog.mjs';

// ─────────────────────────────────────────────────────────── 格式词汇表

test('★ `段:EA` 前缀：**段名以 `.` 开头**也必须认（`.text` / `.data`）', () => {
  const a = parseListingLine('.text:00401000 sub_401000 proc near');
  assert.deepEqual({ seg: a.seg, ea: a.ea }, { seg: '.text', ea: 0x401000 });
  const b = parseListingLine('seg002:00559A68 dword_559A68 dd 0');
  assert.equal(b.seg, 'seg002');
  assert.equal(b.ea, 0x559a68);
  assert.equal(parseListingLine('  不是一行清单'), null, '不是 `段:EA` 形态 ⇒ null（不许猜）');
});

test('★ 自动名 → EA：带 EA 的才给值，`nullsub_3` 这种**没有 EA** 的必须给 null', () => {
  for (const [name, ea] of [
    ['sub_401000', 0x401000], ['loc_401013', 0x401013], ['dword_559A68', 0x559a68],
    ['off_51D574', 0x51d574], ['unk_51D604', 0x51d604], ['jpt_418BC7', 0x418bc7],
    ['def_418BC7', 0x418bc7], ['locret_401604', 0x401604], ['xmmword_52FD60', 0x52fd60],
    ['tbyte_5303D0', 0x5303d0], ['asc_51D6DC', 0x51d6dc], ['stru_51D674', 0x51d674],
  ]) {
    assert.equal(eaOfAutoName(name), ea, `${name} 的 EA`);
  }
  assert.equal(eaOfAutoName('nullsub_3'), null, '`nullsub_3` 的后缀是**序号**不是 EA ⇒ 必须 null');
  assert.equal(eaOfAutoName('SomeUserFunction'), null);
  assert.equal(AUTO_NAME_EA_RE.test('nullsub_3'), false);
});

test('MSVC 修饰名去壳：`_WinMain@16` ⇒ `WinMain`；`??2@YAPAXI@Z` 原样（**不猜**）', () => {
  assert.equal(undecorate('_WinMain@16'), 'WinMain');
  assert.equal(undecorate('??2@YAPAXI@Z'), '??2@YAPAXI@Z');
  assert.equal(undecorate('sub_401000'), 'sub_401000');
});

// ─────────────────────────────────────────────────────────── `.c` 切分

const C = [
  '/* 头部注释',
  '   End of comment. */',
  '#include <defs.h>',
  '',
  '// Function declarations',
  '',
  'int __stdcall sub_401000(void *Src, int a2, void *a3); // idb',
  'int __thiscall sub_401100(int this, LPCSTR lpFileName, const char *a3);',
  'int __cdecl sub_402000(void); // weak',
  '',
  'int __stdcall sub_401000(void *Src, int a2, void *a3)',
  '{',
  '  return 0;',
  '}',
  '',
  'int __thiscall sub_401100(int this,',
  '                    LPCSTR lpFileName)',
  '{',
  '  return 0;',
  '}',
  '',
  'int __userpurge sub_403000@<eax>(_BYTE *a1@<edx>, unsigned int a2@<ecx>, unsigned int a3)',
  '{',
  '  return 0;',
  '}',
  '',
  '_UNKNOWN unk_51D604; // weak',
  'char aError[6] = "ERROR"; // weak',
  'GUID stru_51D674 = { 1u, 2u };',
];

test('★ 定义 vs 原型：**带尾注释的原型**不许被当成定义（旧判据在这里静默出错）', () => {
  const c = scanDecompiled(C);
  assert.deepEqual(c.funcs.map((f) => f.name), ['sub_401000', 'sub_401100', 'sub_403000'], '只认**跟着 `{`** 的那三条');
  // 原型区边界 = 最后一条 `…);` 形态的 sub_ 原型行（这里 3 条原型里最后一条在第 9 行）
  assert.equal(c.lastProtoLine, 9, '原型区边界必须落在原型区（独立判据：`);` 收尾）');
  for (const f of c.funcs) {
    assert.ok(f.from > c.lastProtoLine, `★ ${f.name} 的定义行 ${f.from} 不许落在原型区（≤ ${c.lastProtoLine}）`);
    assert.ok(f.to > f.from, `${f.name} 必须真的跨行（从 { 到 }）`);
  }
  const p = c.funcs.find((f) => f.name === 'sub_401000');
  assert.deepEqual(p.params.map((x) => x.name), ['Src', 'a2', 'a3'], '参数名按位置取出');
  assert.equal(p.conv, '__stdcall');
});

test('★ 跨行签名必须被找到（"只认单行"会把整条定义漏掉，而漏掉与"没有 C"长得一样）', () => {
  const c = scanDecompiled(C);
  const multi = c.funcs.find((f) => f.name === 'sub_401100');
  assert.ok(multi, 'sub_401100 的签名跨两行 ⇒ 必须仍然找到');
  assert.equal(multi.conv, '__thiscall');
  assert.deepEqual(multi.params.map((x) => x.name), ['this', 'lpFileName']);
});

test('★ 名字里的 `@<eax>` 与参数上的 `@<reg>`：都要处理（否则定义对不上、偏移全错）', () => {
  const c = scanDecompiled(C);
  const up = c.funcs.find((f) => f.name === 'sub_403000');
  assert.ok(up, '`sub_403000@<eax>` 的名字必须是 `sub_403000`（`@<eax>` 是**约定标注**，不是名字的一部分）');
  assert.equal(up.conv, '__userpurge');
  assert.deepEqual(up.params.map((x) => [x.name, x.reg]), [['a1', 'edx'], ['a2', 'ecx'], ['a3', null]]);
});

test('★ 数据定义：列 0 + 无括号 + `;` 收尾；**原型行不许**被抠成"数据定义"', () => {
  const c = scanDecompiled(C);
  assert.deepEqual(c.dataDefs.map((d) => d.name), ['unk_51D604', 'aError', 'stru_51D674']);
  assert.deepEqual(c.dataDefs.map((d) => d.ea), [0x51d604, null, 0x51d674], '可读名（`aError`）没有 EA ⇒ null，不许猜');
});

// ─────────────────────────────────────────────────────────── `.lst` 切分

const LST = [
  '.text:00401000 sub_401000 proc near',
  '.text:00401000',
  '.text:00401000 arg_0= dword ptr  8',
  '.text:00401000 arg_4= dword ptr  0Ch',
  '.text:00401000 var_4= dword ptr -4',
  '.text:00401000',
  '.text:00401000 push    ebp',
  '.text:00401001 mov     eax, ds:dword_559A68',
  '.text:00401007 mov     ds:dword_559A70, eax',
  '.text:0040100D add     ds:dword_559A74, 1',
  '.text:00401013 call    sub_402000',
  '.text:00401018 call    ds:dword_55E1B4',
  '.text:0040101E mov     ecx, [ebp+arg_8]',
  '.text:00401021 jmp     sub_403000',
  '.text:00401026 sub_401000 endp',
  '.data:0051D568 First dd 0',
  '.data:0051D56C dd offset sub_4EE07A',
  '.data:0051D570 dd offset ___onexitinit',
  '.data:0051D574 jpt_51D574 dd 0',
];

test('★ 函数切分：`proc near` … `endp`；`call` / 尾跳 `jmp` 分别成边', () => {
  const s = scanListing(LST);
  assert.equal(s.functions.length, 1);
  const f = s.functions[0];
  assert.deepEqual({ sym: f.sym, ea: f.ea, from: f.fromLine, to: f.toLine, lastEa: f.lastEa }, {
    sym: 'sub_401000', ea: 0x401000, from: 1, to: 15, lastEa: 0x401026,
  });
  assert.deepEqual(f.callees, ['sub_402000'], '`call sub_X` 进 callees');
  assert.deepEqual(f.tailCalls, ['sub_403000'], '★ `jmp sub_X`（尾跳）也是一条边');
  assert.equal(f.calleeHits.get('sub_402000'), 1);
  assert.deepEqual(f.externalTargets, [], '本样本没有具名外部目标');
});

test('★ `call ds:dword_X` 必须计入**间接调用点**（旧正则不含 `ds:` ⇒ 这一整类被静默漏掉，实测 69 处）', () => {
  const s = scanListing(LST);
  const f = s.functions[0];
  assert.equal(f.indirectCallSites, 1, '`call ds:dword_55E1B4` 解不出目标 ⇒ 计入间接调用点');
  assert.equal(s.indirectCallSites, 1);
  // 同时它是**读了一个全局**（那是函数指针槽）—— 两件事都要记，不互相顶掉
  const g = f.globals.find((x) => x.name === 'dword_55E1B4');
  assert.ok(g && g.rw === 'r', '`call ds:dword_X` 同时是对该全局的**读**');
});

test('★ 全局读/写按**操作数位置**判定（`mov eax, X` 读 / `mov X, eax` 写 / `add X, 1` 读+写）', () => {
  const f = scanListing(LST).functions[0];
  const rw = Object.fromEntries(f.globals.map((g) => [g.name, g.rw]));
  assert.equal(rw.dword_559A68, 'r');
  assert.equal(rw.dword_559A70, 'w');
  assert.equal(rw.dword_559A74, 'rw');
  assert.deepEqual(f.globals.map((g) => g.ea), [...f.globals.map((g) => g.ea)].sort((a, b) => a - b), '全局按名字排序、EA 可回连');
});

test('★ 栈槽：声明按**偏移**分类（≥8 参数 / <0 局部）；**被引用但没声明**的也要报出来（偏移由名字算）', () => {
  const f = scanListing(LST).functions[0];
  assert.deepEqual(f.args.map((a) => [a.name, a.offset]), [['arg_0', 8], ['arg_4', 12]], '按偏移排序');
  assert.deepEqual(f.locals.map((a) => [a.name, a.offset]), [['var_4', -4]]);
  assert.deepEqual(
    f.refOnly.map((a) => [a.name, a.offset, a.kind]),
    [['arg_8', 16, 'arg']],
    '★ `arg_8` 只被引用、没声明 ⇒ 偏移从名字算（8+8），并标 declared:false',
  );
});

test('★ 数据 item：一个标签 + 它的续行 = **一个** item；长度要**先藏引号里的逗号**', () => {
  const s = scanListing([
    '.data:0051D6BC aError db \'ERROR\',0',
    '.data:0051D6C4 aCsv db \'1,4,8,24,32\',0',
    '.data:0051D6DC asc_51D6DC db \'\\\\\',0',
  ]);
  assert.deepEqual(s.data.map((d) => [d.name, d.sizeBytes]), [['aError', 6], ['aCsv', 12], ['asc_51D6DC', 2]],
    '★ 字符串按**字符数**算字节：`\'ERROR\',0`=6、`\'1,4,8,24,32\',0`=12（引号里的逗号不算分隔符）、`\'\\\\\',0`=2（转义按一个字节算）');
});

test('★ 数据 item 的函数指针表项：`dd offset sub_X` 既记进 item 也记成**表边**', () => {
  const s = scanListing(LST);
  const first = s.data.find((d) => d.name === 'First');
  assert.deepEqual({ ea: first.ea, size: first.sizeBytes, from: first.fromLine, to: first.toLine }, { ea: 0x51d568, size: 12, from: 16, to: 18 });
  assert.deepEqual(first.tableTargets, ['sub_4EE07A', '___onexitinit']);
  assert.equal(s.tableEdges.length, 2);
  assert.equal(s.dataEas.has(0x51d568), true, '非 `.text` 段的 EA 进 dataEas（锚落在这里不算"归属失败"）');
});

// ─────────────────────────────────────────────────────────── 端到端（合成一对语料）

/** 造一对最小的 `.lst` + `.c`（写进临时目录；用完即弃） */
function miniPair(lstText, cText) {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'amayui-catalog-'));
  const lst = path.join(dir, 'synthetic.lst');
  const c = path.join(dir, 'synthetic.c');
  fs.writeFileSync(lst, lstText, 'utf8');
  if (cText) fs.writeFileSync(c, cText, 'utf8');
  return { lst, c: cText ? c : null };
}

test('★ 参数配对：**名字优先、顺序兜底** —— 位置法在这里会给出错的偏移（实测 sub_401100）', () => {
  const { lst, c } = miniPair(
    [
      '.text:00401000 sub_401100 proc near',
      '.text:00401000 lpFileName= dword ptr  8',
      '.text:00401000 arg_4= dword ptr  0Ch',
      '.text:00401000 push    ebp',
      '.text:00401001 retn',
      '.text:00401002 sub_401100 endp',
    ].join('\n') + '\n',
    [
      'int __thiscall sub_401100(int this, LPCSTR lpFileName, const char *a3)',
      '{',
      '  return 0;',
      '}',
    ].join('\n') + '\n',
  );
  const cat = buildCatalog({ lstFile: lst, cFile: c });
  assert.deepEqual(cat.problems, [], `目录自检必须全过：${cat.problems.join('；')}`);
  const f = findFunction(cat, { sym: 'sub_401100' }).item;
  assert.deepEqual(
    f.args.map((a) => [a.name, a.offset ?? a.reg, a.cName, a.pairBy]),
    [['<ecx>', 'ecx', 'this', 'reg'], ['lpFileName', 8, 'lpFileName', 'name'], ['arg_4', 12, 'a3', 'order']],
    '★ `lpFileName` 两边逐字同名 ⇒ 按名字配（位置法会把它配到 0Ch、把 a3 配到 10h —— 全错）',
  );
});

test('★ 没有 C 的函数：`c` 必须是 null + 也能给出 `.lst` 区间（不许静默当成"没有逻辑"）', () => {
  const { lst } = miniPair(
    ['.text:00401000 sub_401000 proc near', '.text:00401000 retn', '.text:00401001 sub_401000 endp'].join('\n') + '\n',
    null,
  );
  const cat = buildCatalog({ lstFile: lst, cFile: null });
  const f = findFunction(cat, { sym: 'sub_401000' }).item;
  assert.equal(f.hasC, false);
  assert.equal(f.c, null);
  assert.ok(f.toLine > f.fromLine, '.lst 区间仍然给');
  assert.equal(f.conv, null, '没有 C ⇒ 调用约定未知，不许编一个');
});

test('★ 定位不到就**抛**（不许给一个空体或前一个函数）', () => {
  const { lst, c } = miniPair(
    ['.text:00401000 sub_401000 proc near', '.text:00401000 retn', '.text:00401001 sub_401000 endp'].join('\n') + '\n',
    'int __cdecl sub_401000(void)\n{\n  return 0;\n}\n',
  );
  const cat = buildCatalog({ lstFile: lst, cFile: c });
  assert.throws(() => findFunction(cat, { sym: 'not_a_symbol' }), /不在目录里/);
  assert.throws(() => findFunction(cat, { ea: 0x1 }), /不落在任何函数里/);
  assert.equal(findFunction(cat, { ea: 0x401000 }).isStart, true);
  // ★ `endp` 那一行带的是**下一条指令的地址**（IDA 的写法），它仍在区间内 ⇒ 真正的外面要再往后一格
  assert.throws(() => findFunction(cat, { ea: 0x401002 }), /不落在任何函数里/, '函数末尾之后（下一个函数之前）是空隙');
});
