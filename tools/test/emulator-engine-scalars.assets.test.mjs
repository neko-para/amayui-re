/** @env assets @kind contract @why 引擎构造函数对 `Engine+0x5EC9C..0x5ECE8` 那片标量的清零口径塌了（"没写过 = 0"变成假象）*/
/**
 * tools/test/emulator-engine-scalars.assets.test.mjs —— **标量堆"默认 0"的语料锚**
 *
 * ## 为什么它必须单独存在（`emulator-engine-scalars.test.mjs` 那条纯用例的欠账）
 * 纯用例断言的是"**空的** `EngineScalars` 读 0" —— 那是**恒真**的（构造出来本来就是空的），
 * 它回不了语料：把实现改成 `?? 1` 它才红，而**引擎那边换了别的行为它不会红**。
 *
 * 这里把判据挂回**逐字**：引擎构造函数里连续 **20 条** `mov [esi+5EC9Ch..5ECE8h], edi`（`edi = 0`）
 * —— 逐字 `.lst:33974-33993`（EA `0x415C0F..0x415C81`），台账 subject
 * `Engine+0x5EC9C..0x5ECE8/ctor-zero-fill`。三件事一起钉住：
 *   ① 那 20 条**恰好 20 条、恰好是那 20 个槽、恰好连续**（多一条/少一条/错一格都红）；
 *   ② 写进去的是 **0**（`edi` 在函数里被 `xor edi,edi` 清过 —— 所以"写的是 0"不是猜的）；
 *   ③ 那片**紧接在 20 条之后的第 21 个 dword 不是同一批**（`+0x5C73C` 另起一段：
 *      `.lst:33995`）⇒ 顺手钉住"这批的**边界**是 0x5ECE8 而不是一路清下去"。
 *
 * ★ 语料（`.lst`）不在场 ⇒ `skip`（它是解压产物；`pnpm tools disasm build` 可重建）。
 * 运行：`pnpm test:assets`
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';

import { REPO_ROOT } from '../lib/paths.mjs';
// ★ 被锚的是**这一段**：模型里那 20 格的可再校验形式（改模型 ⇒ 这里当场红）
import { CTOR_ZERO_FILLED } from '../../apps/emulator/src/model/engine-scalars.ts';
// ★ 条件位副作用那张表（`0x88` 写常量 1 / 清 bit27、`0x71` 置 bit27）的**逐字锚**
import { ENGINE_SCALAR_BITS } from '@amayui/age-format/src/engine/layout.mts';

const FILES_DIR = path.join(REPO_ROOT, 'corpus', 'disasm', 'files');
const listing = (() => {
  if (!fs.existsSync(FILES_DIR)) return null;
  // ★ `.lst` 有两个（AGE.EXE 与 AGERC.DLL）；引擎构造函数在 AGE.EXE 那份里 ⇒ 按名字挑，不靠排序
  const lst = fs.readdirSync(FILES_DIR).filter((f) => f.endsWith('.lst') && f.startsWith('AGE.EXE')).sort();
  return lst.length ? path.join(FILES_DIR, lst[0]) : null;
})();
const skip = listing ? false : '反汇编语料不在场（先 `pnpm tools disasm build`）';

/** 那 20 个槽（绝对偏移，逐字取自 `.lst:33974-33993`） */
const SLOTS = [...CTOR_ZERO_FILLED];

test('★★ `Engine+0x5EC9C..0x5ECE8`：构造函数里连续 20 条写 0（逐字锚 ⇒ "默认 0"才是可再校验的）', { skip }, () => {
  const lines = fs.readFileSync(listing, 'utf8').split('\n');
  // 归一：去掉 `文件:地址  ` 前缀与多余空格（与 `emulator-iterate` 那条同形）
  const norm = (l) => l.replace(/^\S+:[0-9A-F]{8}\s*/, '').trim().replace(/\s+/g, ' ');

  const i = lines.findIndex((l) => norm(l) === 'mov [esi+5EC9Ch], edi');
  assert.ok(i > 0, '语料里应能找到 `mov [esi+5EC9Ch], edi`（引擎构造函数清那片标量的第一条）');
  // ⓪ 锚的**形状**先钉住（模型里那段常量本身被改坏时，这条比下面的逐字比对更早说话）
  assert.equal(SLOTS.length, 20, '构造函数的清零是**连续 20 格**（每格 4 字节）');
  assert.deepEqual(SLOTS, Array.from({ length: 20 }, (_, k) => 0x5ec9c + 4 * k),
    '那 20 格的偏移必须是 `0x5EC9C + 4k`（步长 4、首尾 0x5EC9C→0x5ECE8）');

  // ① 恰好那 20 条、恰好那 20 个槽、恰好连续
  const got = lines.slice(i, i + 20).map(norm);
  const want = SLOTS.map((a) => `mov [esi+${a.toString(16).toUpperCase()}h], edi`);
  assert.deepEqual(got, want,
    '构造函数的清零序列与知识层登记的 20 个槽不再逐字相同（槽变了 / 顺序变了 / 中间插了别的指令）');
  // ② 第 21 条**不许**还是同一批（边界就是 0x5ECE8）
  assert.notEqual(norm(lines[i + 20]), `mov [esi+5ECECh], edi`, '★ 这批的边界是 0x5ECE8；越界再多清一格就不是同一条观察了');
  // ③ 写进去的**是 0**：`edi` 必须**在本函数里**被 `xor edi, edi` 清过，而且此后**再没被写过**
  //    （★ 不是"猜它进函数时是 0"—— 那是调用约定，不是可再校验的观察）
  const body = lines.slice(0, i).map(norm);
  let lastXor = -1;
  for (let k = 0; k < body.length; k += 1) if (body[k] === 'xor edi, edi') lastXor = k;
  assert.ok(lastXor > 0, '`edi` 必须在**同一个函数体里**被 `xor edi, edi` 清过（这一条找不到 ⇒ "写的是 0"就只是猜测）');
  //    ★ `mov edi, edi` 是 MSVC 的 2 字节空转（不改变值）⇒ 不算"写过"
  const writes = body.slice(lastXor + 1)
    .filter((l) => l !== 'mov edi, edi')
    .filter((l) => /^(mov|xor|add|sub|and|or|inc|dec|lea|pop|shl|shr|not|neg|imul)\s+edi,/.test(l));
  assert.deepEqual(writes, [],
    `★ \`xor edi, edi\`（第 ${lastXor + 1} 行）之后又写过 \`edi\` ⇒ 构造函数的清零写的不一定是 0：\n  - ${writes.join('\n  - ')}`);
  // ④ EA 也要在（锚是 EA，不是行号）：`.lst:33974` 的 EA = 0x415C0F
  assert.match(lines[i], /:00415C0F\s/, `第一条清零的 EA 应当是 0x415C0F（换一次反汇编只重建 行号→EA 映射，不改锚）`);
});

/**
 * ★★ **条件位副作用**（`ENGINE_SCALAR_BITS`）的三条逐字锚 —— ★ 这一条是"**这个位现在是 0 还是 1**"
 * 这件事的**取证面**：纯用例只断言模型行为，而"表里的 dword/mask 抄错了"只有回语料才能发现。
 *
 * 逐字（`.lst`，EA 为锚）：
 * ```
 *   0041FAE4 mov dword ptr [esi+77800h], 1          ;; 0x88 真支：写常量 1（77800h/4 = 122368）
 *   0041FAF0 and dword ptr [esi+0AAB44h], 0F7FFFFFFh;; 0x88 假支：清 bit27（0AAB44h/4 = 174801）
 *   0041EE8D or  dword ptr [ebx+0AAB44h], 8000000h  ;; 0x71      ：置 bit27（与上面那一清成对）
 * ```
 * ★ 判据是**现算**的：期望文本由表里的 `dword`（`×4` = 字节偏移）与 `value`/`mask` 拼出来
 *   ⇒ 抄错槽号、抄错掩码、或把置/清写成同一形态，这里都会红。
 * ★ 台账 subject：`opcode/0x88` + `opcode/0x71`（两条正文互指同一格同一位）。
 */
test('★★ 条件位副作用：`0x88` 真支写常量 1 / 假支清 bit27、`0x71` 置 bit27（逐字锚 = EA）', { skip }, () => {
  const lines = fs.readFileSync(listing, 'utf8').split('\n');
  const norm = (l) => l.replace(/^\S+:[0-9A-F]{8}\s*/, '').trim().replace(/\s+/g, ' ');
  /** IDA 的十六进制写法：首字符是 A–F 时前面补一个 `0`（`0AAB44h` / `0F7FFFFFFh`） */
  const idaHex = (n) => { const s = (n >>> 0).toString(16).toUpperCase(); return (/^[A-F]/.test(s) ? '0' : '') + s + 'h'; };
  const at = (ea) => {
    const tag = `:${ea.toString(16).toUpperCase().padStart(8, '0')} `;
    // ★ 同一个 EA 上可能先是标签行（`loc_41FAF0:`）⇒ 只取**指令**那一行（标签 = 空串或以 `:` 结尾）
    const cands = lines.filter((x) => x.includes(tag)).map(norm).filter((t) => t !== '' && !t.endsWith(':'));
    assert.ok(cands.length, `语料里找不到 EA 0x${ea.toString(16)} 的指令`);
    return cands[0];
  };
  const of = (opcode, when) => {
    const b = ENGINE_SCALAR_BITS.find((x) => x.opcode === opcode && x.when === when);
    assert.ok(b, `知识层缺 0x${opcode.toString(16)} / ${when} 那条登记（没有它这条守卫失去对象）`);
    return b;
  };

  // ① `0x88` 真支：写**常量** 1 到 `Engine.d122368`
  const w = of(0x88, 'op1!=0');
  assert.equal(at(0x41fae4), `mov dword ptr [esi+${idaHex(w.dword * 4)}], ${w.value}`,
    '真支那条 `mov [esi+77800h], 1` 与知识层的 dword/value 不再一致');
  // ② `0x88` 假支：清 `Engine.d174801` 的 bit27（掩码取反后是 `0F7FFFFFFh`）
  const c = of(0x88, 'op1==0');
  assert.equal(at(0x41faf0), `and dword ptr [esi+${idaHex(c.dword * 4)}], ${idaHex(~c.mask)}`,
    '假支那条 `and [esi+0AAB44h], 0F7FFFFFFh` 与知识层的 dword/mask 不再一致');
  // ③ `0x71`：置**同一位**
  const s = of(0x71, 'always');
  assert.equal(at(0x41ee8d), `or dword ptr [ebx+${idaHex(s.dword * 4)}], ${idaHex(s.mask)}`,
    '`0x71` 那条 `or [ebx+0AAB44h], 8000000h` 与知识层的 dword/mask 不再一致');
  // ④ 成对：同一格、同一位（"两条合起来才构成这个位的来去"）
  assert.equal(c.name, s.name, '★ 置与清必须是**同一个槽**');
  assert.equal(c.mask, s.mask, '★ 置与清必须是**同一位**');
  // ⑤ 形态不许混：一条 `set`、一条 `clear`（都写成 `set` ⇒ "清不掉"这件事在表里就看不出来了）
  assert.equal(c.op, 'clear');
  assert.equal(s.op, 'set');
});
