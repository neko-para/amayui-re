/** @env assets @kind contract @why 整数池的 DEC/ENC 口径错了（读出来的值不对 / 下标被误过编解码） */
/**
 * tools/test/engine-value-codec.test.mjs —— **整数值编解码（DEC/ENC）的双向守卫**
 *
 * 两条独立的判据，缺一不可：
 *   ① **公式互逆**（纯算术，合成数据即可）：`decInt(encInt(v, key), key) === v`，
 *      且"int 过编解码、下标不过"这条口径本身要被断言到。
 *   ② ★ **公式与语料的指令序列一致**（要语料）：DEC 是 `rol 11; xor key; ror 25`、
 *      ENC 是 `ror 7; xor key; rol 21`、取址是 `base + idx*4`（无位运算）——
 *      如果哪天有人改了 `value-codec.mjs` 里的移位量，这里会**当场红**；
 *      反过来，语料一变（换镜像/换导出），这里也会红。
 *
 * 运行：`pnpm test:assets`
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';

import { REPO_ROOT } from '../lib/paths.mjs';
import { CODEC, decInt, encInt, encZero, intSlotOffset, rol32, ror32 } from '@amayui/age-format/src/asm/value-codec.mts';

const FILES_DIR = path.join(REPO_ROOT, 'corpus', 'disasm', 'files');
const listing = (() => {
  if (!fs.existsSync(FILES_DIR)) return null;
  const lst = fs.readdirSync(FILES_DIR).filter((f) => f.endsWith('.lst')).sort();
  return lst.length ? path.join(FILES_DIR, lst[0]) : null;
})();
const skip = listing ? false : '语料未解压（先 `pnpm tools disasm build`）';

/**
 * 取某 EA 之后的 `n` 行，并**归一化空白**。
 * ★ 为什么要归一化：IDA 的行是 `rol     eax, 0Bh`（助记符后多个空格、逗号后一个空格），
 *   直接 `startsWith('rol eax,0Bh')` **永远匹配不到**（实测踩过）。归一化后才是"逐字比对"。
 */
function windowAfter(ea, n) {
  const all = fs.readFileSync(listing, 'utf8').split('\n');
  const hex = `${ea.toString(16).toUpperCase().padStart(8, '0')}`;
  const start = all.findIndex((l) => l.includes(`:${hex} `));
  assert.ok(start >= 0, `语料里找不到 EA 0x${ea.toString(16)}`);
  return all
    .slice(start, start + n)
    .map((l) => l.replace(/^\S+:[0-9A-F]{8}\s*/, '').trim().replace(/\s+/g, ' ').replace(/, /g, ','));
}

/** 在一段行里按顺序找若干"指令片段"（允许中间夹别的行） */
function findInOrder(lines, needles) {
  let i = 0;
  const hit = [];
  for (const need of needles) {
    const want = need.replace(/\s+/g, ' ').replace(/, /g, ',');
    let found = -1;
    for (let k = i; k < lines.length; k += 1) {
      if (lines[k].startsWith(want)) {
        found = k;
        break;
      }
    }
    if (found < 0) return { ok: false, missing: need, hit };
    hit.push(lines[found]);
    i = found + 1;
  }
  return { ok: true, hit };
}

// ─────────────────────────────────────────────── ① 公式：互逆 + 边界

test('★ DEC/ENC 互为逆：对多个 key 与一组边界值都成立', () => {
  const keys = [0, 1, 0xdeadbeef, 0xffffffff, 0x12345678];
  const values = [0, 1, 2, 255, 256, 0x7fffffff, 0x80000000, 0xffffffff, 12345678, 0x1234abcd];
  for (const key of keys) {
    for (const v of values) {
      const enc = encInt(v, key);
      assert.equal(decInt(enc, key), v >>> 0, `dec(enc(${v}, ${key}), ${key}) 不等于原值`);
      // 编码本身不该是恒等（除非恰好撞上）；这条防"两边都写成 no-op 也能互逆"
      if (key !== 0 || (v !== 0 && v !== 0xffffffff)) {
        assert.notEqual(enc, v >>> 0, `key=${key} v=${v}：编解码看起来是恒等 ⇒ 公式可能没生效`);
      }
    }
  }
});

test('★ `enc_zero` 是"非 0 的位模式"：局部 int 池的初值不是 0（拿它当 0 判会错）', () => {
  for (const key of [1, 0xdeadbeef, 0x12345678]) {
    const z = encZero(key);
    assert.equal(decInt(z, key), 0, 'enc_zero 解码回来必须是 0');
    assert.notEqual(z, 0, `key=${key} 时 enc_zero 不应为 0 —— "未初始化 = 0"是错的`);
  }
  assert.equal(encZero(0), 0, 'key=0 时 enc_zero 恰好是 0（这正是最容易被当成普遍规律的那种巧合）');
});

test('★ 下标**不过**编解码：`base + idx*4` 是纯算术（把 idx 也过 DEC 会取到别的槽）', () => {
  assert.equal(intSlotOffset(0), 0);
  assert.equal(intSlotOffset(1), 4);
  assert.equal(intSlotOffset(0x100), 0x400);
  // 反证：如果误把下标过一遍 DEC，绝大多数下标会落到完全不同的槽上
  const key = 0xdeadbeef;
  const wrong = decInt(intSlotOffset(7), key);
  assert.notEqual(wrong, intSlotOffset(7), '把下标过 DEC 会得到不同的偏移 ⇒ 这条判据红得有意义');
});

test('★ rol32/ror32 是 32 位环移（`n` 取模、且不引入符号位）', () => {
  assert.equal(rol32(0x80000000, 1), 1, '左移把最高位绕回最低位');
  assert.equal(ror32(1, 1), 0x80000000, '右移把最低位绕回最高位');
  assert.equal(rol32(0x12345678, 0), 0x12345678);
  assert.equal(rol32(0x12345678, 32), 0x12345678);
  assert.equal(rol32(rol32(0x12345678, 5), 27), 0x12345678, '互补移位应回到原值');
  assert.ok(rol32(0xffffffff, 7) >= 0, '结果始终是无符号 32 位');
});

// ─────────────────────────────────────────────── ② 公式 vs 语料

test('★ 语料里 DEC 的指令序列与 `CODEC.dec` 一致（rol → xor key → ror）', { skip }, () => {
  const win = windowAfter(0x40d500, 26);
  // key 先从 `Engine+0x5EC8C` 取出（这一步也要在场，否则"xor 一个来路不明的值"不算证据）
  assert.ok(win.some((l) => l.startsWith(CODEC.dec.keyLoad)), `DEC 里应从 ${CODEC.keyField.name} 取 key（缺 "${CODEC.dec.keyLoad}"）`);
  const r = findInOrder(win, CODEC.dec.insns);
  assert.ok(r.ok, `语料里找不到 DEC 的指令序列（缺 "${r.missing}"）；要么公式移位量被改了，要么语料换了镜像`);
  // 移位量必须与常量里的数字对得上
  assert.ok(CODEC.dec.insns[0].includes(`0${CODEC.dec.shifts[0].toString(16).toUpperCase()}h`), 'DEC 首个移位量应与 shifts[0] 一致');
  assert.ok(CODEC.dec.insns[2].includes(`${CODEC.dec.shifts[1].toString(16).toUpperCase()}h`), 'DEC 末个移位量应与 shifts[1] 一致');
});

test('★ 语料里 ENC 的指令序列与 `CODEC.enc` 一致（ror → xor key → rol）', { skip }, () => {
  const win = windowAfter(0x4103b2, 20);
  assert.ok(win.some((l) => l.startsWith(CODEC.enc.keyLoad)), `ENC 里应从 ${CODEC.keyField.name} 取 key`);
  const r = findInOrder(win, CODEC.enc.insns);
  assert.ok(r.ok, `语料里找不到 ENC 的指令序列（缺 "${r.missing}"）`);
  assert.ok(CODEC.enc.insns[0].includes(`,${CODEC.enc.shifts[0].toString(16).toUpperCase()}`), 'ENC 首个移位量应与 shifts[0] 一致（十进制 7 的十六进制写法）');
  assert.ok(CODEC.enc.insns[2].includes(`${CODEC.enc.shifts[1].toString(16).toUpperCase()}h`), 'ENC 末个移位量应与 shifts[1] 一致');
});

test('★ 取址侧不过编解码：`base + idx*4` 的逐字证据在场', { skip }, () => {
  const win = windowAfter(0x42af2d, 6);
  const r = findInOrder(win, CODEC.indexPath.insns);
  assert.ok(r.ok, `语料里找不到 type 3 的取址形态（缺 "${r.missing}"）—— 若它变成带位运算的形态，说明下标也过编解码了`);
});

test('★ key 是**运行期写入**的（不是立即数）⇒ 编解码器不许写死 key', { skip }, () => {
  const all = fs.readFileSync(listing, 'utf8');
  // `.text:00417359 mov [esi+5EC8Ch], edx` —— 源是寄存器 edx，不是立即数
  const m = /:00417359\s+mov\s+\[esi\+5EC8Ch\],\s*(\S+)/.exec(all);
  assert.ok(m, '语料里应能找到 key 的写入点 `.text:00417359`');
  assert.match(m[1], /^e[a-z]{2}$|^[a-z]{2,3}$/, `key 的源应是寄存器（实测 edx），实际 ${m[1]} —— 若是立即数则 key 是常量，本判据要改`);
  assert.ok(!/^[0-9A-F]+h?$/i.test(m[1]), 'key 不许是立即数 ⇒ `value-codec.mjs` 不提供默认 key 是对的');
});

/**
 * ★★ **G1**：key 的"格"必须被**机械钉住**。
 *
 * 为什么需要它：`CODEC.dec.keyLoad` / `CODEC.enc.keyLoad` 是**写死的字面串**，与 `CODEC.keyField.dword` **脱钩**
 * ⇒ 把 `dword: 97059` 改成别的数**无声通过**（实测：`pnpm test:mutation` 里加这条之前它是绿的）。
 * 这条把三件事钉在一起：
 *   ① 全语料里 `5EC8Ch` 的**写入点恰好 1 处**，EA == `0x417359`，且源是寄存器（不是立即数）；
 *   ② 它其余的引用**全都是读**（`cmp dword ptr [esi+5EC8Ch],0`）—— 没有第二个写点；
 *   ③ `CODEC.keyField.dword × 4 == 0x5EC8C`，且与 `encZeroField`（`0x5EC90`）**相邻但不同格**。
 */
test('★ G1：key 的写入点全语料恰好 1 处且 EA == 0x417359；`CODEC.keyField` 的槽号必须与那一格一致', { skip }, () => {
  const all = fs.readFileSync(listing, 'utf8').split('\n');
  const refs = [];
  for (const raw of all) {
    if (!raw.includes('5EC8Ch')) continue;
    const m = /:([0-9A-F]{8})\s+(.*?)\s*$/.exec(raw.trim());
    if (!m) continue;
    refs.push({ ea: Number.parseInt(m[1], 16), text: m[2].replace(/\s+/g, ' ').replace(/, /g, ',') });
  }
  assert.ok(refs.length > 0, '语料里凡是提到 `5EC8Ch` 的行都应解析出 EA');

  const writes = refs.filter((r) => /^mov \[esi\+5EC8Ch\],/.test(r.text));
  assert.equal(writes.length, 1,
    `① key 的写入点必须**全语料恰好 1 处**，实测 ${writes.length}：${writes.map((w) => `0x${w.ea.toString(16)}`).join(', ')}`);
  assert.equal(writes[0].ea, 0x417359, `① key 的写入点必须是 .text:00417359，实测 0x${writes[0].ea.toString(16)}`);
  assert.match(writes[0].text, /^mov \[esi\+5EC8Ch\],\s*e?[a-z]{2}$/i,
    `① key 的源必须是寄存器（实测 edx；立即数意味着 key 是常量），实际 "${writes[0].text}"`);

  const others = refs.filter((r) => r.ea !== writes[0].ea);
  // 「写」的形态 = **那个格子出现在目的位置**（`… [esi+5EC8Ch],…` 或读-改-写）；其余都是读。
  const otherWrites = others.filter((r) => /^(?:or|add|and|xor|sub|inc|dec|not|neg|shl|shr|rol|ror|mov)\s+(?:dword ptr )?\[esi\+5EC8Ch\]/.test(r.text));
  assert.deepEqual(otherWrites.map((r) => `0x${r.ea.toString(16)} ${r.text}`), [],
    '② key **没有第二个写入点**（其余引用必须都是读：`mov r,[esi+5EC8Ch]` 或 `cmp dword ptr [esi+5EC8Ch],…`）');
  assert.ok(others.some((r) => /^mov \w+,\[esi\+5EC8Ch\]$/.test(r.text)), '② 语料里应有大量"读 key"的引用（否则 key 根本没被用过）');

  // ③ ★ 这一条让"把 dword 索引改坏"当场红（`keyLoad` 是字面串、抓不住它）
  assert.equal(CODEC.keyField.dword * 4, 0x5ec8c,
    `③ CODEC.keyField.dword=${CODEC.keyField.dword} ⇒ 0x${(CODEC.keyField.dword * 4).toString(16)}，而语料里 key 在 0x5EC8C`);
  assert.equal(CODEC.keyField.name, 'Engine+0x5EC8C', '③ key 的槽名');
  assert.equal(CODEC.encZeroField.dword * 4, 0x5ec90,
    `③ CODEC.encZeroField.dword=${CODEC.encZeroField.dword} ⇒ 0x${(CODEC.encZeroField.dword * 4).toString(16)}，而语料里 enc_zero 在 0x5EC90`);
  assert.notEqual(CODEC.keyField.dword, CODEC.encZeroField.dword,
    '③★ key 与 enc_zero 是**相邻但不同**的两格（混成一格会把 key 读成 enc_zero）');
  // 两处 keyLoad 必须指同一格
  assert.ok(CODEC.dec.keyLoad.includes('5EC8Ch'), `DEC 的 keyLoad 必须读 5EC8C，实际 "${CODEC.dec.keyLoad}"`);
  assert.ok(CODEC.enc.keyLoad.includes('5EC8Ch'), `ENC 的 keyLoad 必须读 5EC8C，实际 "${CODEC.enc.keyLoad}"`);
});

/**
 * ★★ **G3**：`enc_zero` 不是"另一个常量"，而是 `rol32(key, 21)` **推导出来的**。
 * 判据 = 推导函数的逐字序列（`sub_405620`：`mov eax,0` / `xor eax,ebx`(key) / `rol eax,15h`）
 *   + 它的两个写入点（引擎构造 `0x415990`、装载器把 int 池初值填成它 `0x40F4A6`→`0x40F4AC`）。
 * ★ 把 `rol 21` 或 `encZeroField` 那一格改坏都会当场红。
 */
test('★ G3：`enc_zero = rol32(key,21)` 的推导在场（`sub_405620`）+ 两个写入点（`0x415990` / 装载器 `0x40F4A6`）', { skip }, () => {
  const win = windowAfter(0x405625, 6);
  const r = findInOrder(win, ['mov eax,0', 'mov ebx,[ebp+arg_0]', 'xor eax,ebx', 'rol eax,15h']);
  assert.ok(r.ok, `\`sub_405620\` 里 \`enc_zero = rol32(key,21)\` 的推导序列缺失（缺 "${r.missing}"）—— 实测：\n${win.join('\n')}`);
  assert.equal(CODEC.enc.shifts[1], 0x15, '语料里是 `rol eax,15h` ⇒ 移位量 21（`CODEC.enc.shifts[1]`）');
  for (const key of [1, 0xdeadbeef, 0x12345678]) {
    assert.equal(encZero(key), rol32(key, 21),
      `enc_zero 必须就是 rol32(key,21)（推导 = 0 ^ key 后再 rol 21）；key=${key} 时两者不等`);
  }
  // 写入点 ①：引擎构造里把它写进 Engine+0x5EC90
  const w1 = windowAfter(0x415990, 2);
  assert.ok(w1.some((l) => l.startsWith('mov [esi+5EC90h],eax')),
    `\`0x415990\` 必须把 eax（= rol(key,21)）写进 Engine+0x5EC90，实测：\n${w1.join('\n')}`);
  // 写入点 ②：装载器把 int 池的每一格初始化成 enc_zero（`0x40F4A6` 取它 → `0x40F4AC` 写进格）
  const w2 = windowAfter(0x40f4a6, 4);
  const r2 = findInOrder(w2, ['mov edx,[esi+5EC90h]', 'mov [eax+ecx*4],edx']);
  assert.ok(r2.ok, `装载器应把 int 池初值填成 enc_zero（缺 "${r2.missing}"）—— 实测：\n${w2.join('\n')}`);
});
