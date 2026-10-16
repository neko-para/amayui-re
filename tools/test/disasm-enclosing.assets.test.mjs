/** @env assets @kind contract @why "EA → 所属函数"这条定位链断了：EA 在函数体内时算成**后一个**函数，或带 `@` 的修饰名不被认作函数头 ⇒ 静默给前一个函数 */
/**
 * tools/test/disasm-enclosing.assets.test.mjs —— `.lst` 侧**定位**的两条老判据（从 `disasm-pseudo` 里留下）
 *
 * 这两条与"item 目录"（`disasm-fn`）无关，是 `disasm-at` 的**定位**能力，所以跟着 `lib/disasm.mjs` 留在这一档：
 * 1. `spanOfFunction` = **函数起点**的区间（向后找 `proc near`）；`enclosingFunction` = **包含**该 EA 的函数。
 *    实测 `0x40D500` 不是函数起点（它是 `loc_40D500`，属于 `sub_40CD10`，起点在它**前面 683 行**）
 *    ⇒ 用前者去问"这个 EA 属于谁"会得到**后一个**函数 —— 读起来完全正常，结论全错。
 * 2. 符号名必须允许 `@`（MSVC 修饰名 `_WinMain@16`）。旧字符类不含 `@` ⇒ 那一行不被认作函数头，
 *    `enclosingFunction` 落到**前一个**函数 `sub_4BA6B0`。
 *
 * 运行：`pnpm test:assets`（要语料；语料不在场 ⇒ 如实 skip）
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';

import { REPO_ROOT } from '../lib/paths.mjs';
import { buildIndex, enclosingFunction, pickDecompiled, spanOfFunction } from '../lib/disasm.mjs';

const FILES_DIR = path.join(REPO_ROOT, 'corpus', 'disasm', 'files');
const LST = fs.existsSync(FILES_DIR)
  ? (fs.readdirSync(FILES_DIR).filter((f) => f.endsWith('.lst')).sort().map((f) => path.join(FILES_DIR, f))[0] ?? null)
  : null;
const skip = LST ? false : '语料未解压（先 `pnpm tools disasm build`）';
const idx = LST ? buildIndex(LST) : null;

/** ★ 实测：`0x40D500`（DEC 公式的锚）不是函数起点，它属于 `sub_40CD10` */
const INSIDE = 0x40d500;
const ENCLOSING = 0x40cd10;

test('★ `EA 在函数体内` 必须定位到**包含**它的函数（不是后一个）', { skip }, () => {
  const enc = enclosingFunction(LST, idx, INSIDE);
  assert.equal(enc.symbolEa, ENCLOSING, `包含 0x40d500 的应当是 sub_40CD10，实际 ${enc.symbol}`);
  assert.equal(enc.symbol, 'sub_40CD10');
  assert.ok(enc.fromLine < enc.toLine, '区间必须非空');

  // ★ 反例：这两件事**不是**同一件 —— `spanOfFunction` 从 EA 往后找，会撞上后一个函数
  const sp = spanOfFunction(LST, idx, INSIDE);
  assert.notEqual(sp.fromLine, enc.fromLine, '★ 对"体内的 EA"，`spanOfFunction` 给的是**后一个**函数');

  // 而对**函数起点**的 EA，两者必须一致（不许修一个坏一个）
  const atStart = enclosingFunction(LST, idx, ENCLOSING);
  const spStart = spanOfFunction(LST, idx, ENCLOSING);
  assert.equal(atStart.symbol, 'sub_40CD10');
  assert.equal(atStart.fromLine, spStart.fromLine, '函数起点上两者必须一致');
  assert.equal(atStart.toLine, spStart.toLine, '函数起点上两者必须一致');
});

test('★ 符号名带 `@` 的函数也必须被认作函数头（MSVC 修饰名）—— 否则**静默**给前一个函数', { skip }, () => {
  // 反例：符号组原先是 `[A-Za-z_][\w.]{0,40}`（不含 `@`）⇒ `.text:004BA890 _WinMain@16 proc near`
  // 不被认作函数头，`enclosingFunction(0x4BAA3A)` 落到**前一个**函数 `sub_4BA6B0`。
  const fn = enclosingFunction(LST, idx, 0x4baa3a);
  assert.equal(fn.symbol, '_WinMain@16', `EA 0x4baa3a 必须归属 _WinMain@16，实际 ${fn.symbol}`);
  assert.equal(fn.fromLine, 295818, `函数起点行必须是 295818（_WinMain@16 proc near），实际 ${fn.fromLine}`);
  assert.equal(fn.toLine, 296907, `函数结束行必须是 296907（_WinMain@16 endp），实际 ${fn.toLine}`);
});

test('★ 没有配套 `.c` 时 `pickDecompiled` 返回 null（不静默造一个路径）', { skip }, () => {
  assert.equal(pickDecompiled(path.join(FILES_DIR, '根本没有这个文件.lst')), null);
  const hit = pickDecompiled(LST);
  assert.ok(hit && hit.endsWith('.c'), `应当由 .lst 推出同名 .c，实际 ${hit}`);
  assert.ok(fs.existsSync(hit), '推出的 .c 必须真的存在');
});
