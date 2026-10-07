/** @env assets @kind contract @why "EA → 所属函数 → C 体"这条链断了：EA 在函数体内时定位到**后一个**函数，或"没有 C 体"被静默当成空 */
/**
 * tools/test/disasm-pseudo.assets.test.mjs —— **两层工作流**（C 读 / lst 证）的机械判据
 *
 * ## 它守的是什么
 * 逆向的两层分工是"**用 Hex-Rays 的 C 读逻辑**、**用 `.lst` 定位与核验**"。这条链有两个**会静默出错**的接缝：
 *
 * 1. **`.c` 里一处地址都没有**（实测 `0x00xxxxxx` 计数 = 0）⇒ 只能靠**函数符号**回连；
 *    而大量被引用的 EA **不是函数起点**（实测：`0x40D500` = `loc_40D500`，属于 `sub_40CD10`，函数起点在它**前面 683 行**）。
 *    ⇒ 如果"包含该 EA 的函数"算错，拿到的是**后一个函数**的 C 体 —— 读起来完全正常，结论全错。
 * 2. **不是每个函数都有 C**（实测 3807 个 `proc near` 里有 77 个没有）⇒ 缺 C 必须**明说**，
 *    否则"没读到"会被读成"这段代码没有逻辑"。
 *
 * ★ 反例（本守卫要钉住的正是这条）：`spanOfFunction()` 从 EA 那一行**向后**找 `proc near`
 *   ⇒ 对"EA 在体内"的情形它会找到**后一个**函数。两件事必须分开：`spanOfFunction` = 函数起点的区间；
 *   `enclosingFunction` = **包含**该 EA 的函数。
 *
 * 运行：`pnpm test:assets`（要语料；语料不在场 ⇒ 如实 skip）
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';

import { REPO_ROOT } from '../lib/paths.mjs';
import { buildIndex, buildSymbolIndex, enclosingFunction, pickDecompiled, pseudoOfFunction, spanOfFunction } from '../lib/disasm.mjs';

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
  assert.ok(enc.fromLine < 21580 + 1 && enc.toLine > 22797 - 1, `区间应当覆盖那个大函数（实测 lst 行 21580-22797），实际 ${enc.fromLine}-${enc.toLine}`);

  // ★ 反例：这两件事**不是**同一件 —— `spanOfFunction` 从 EA 往后找，会撞上后一个函数
  const sp = spanOfFunction(LST, idx, INSIDE);
  assert.notEqual(sp.fromLine, enc.fromLine, '★ `spanOfFunction` 对"体内的 EA"会给**后一个**函数 —— 这正是本守卫要钉住的分工');

  // 而对**函数起点**的 EA，两者必须一致（不许修一个坏一个）
  const atStart = enclosingFunction(LST, idx, ENCLOSING);
  const spStart = spanOfFunction(LST, idx, ENCLOSING);
  assert.equal(atStart.symbol, 'sub_40CD10');
  assert.equal(atStart.fromLine, spStart.fromLine, '函数起点上两者必须一致');
  assert.equal(atStart.toLine, spStart.toLine, '函数起点上两者必须一致');
});

test('★ C 体区间自洽；被截断必须明说（不许把"没给"当成"没有"）', { skip }, () => {
  const r = pseudoOfFunction(LST, idx, { sym: 'sub_42CA50' });
  assert.ok(r.c, 'sub_42CA50 应当有 C 体');
  assert.equal(r.c.toLine - r.c.fromLine + 1, r.c.bodyLines, '行区间长度必须与 bodyLines 自洽');
  assert.equal(r.c.body.length, r.c.bodyLines, '未截断时必须给全');
  assert.equal(r.c.truncated, false);
  assert.ok(r.c.fromLine < r.c.toLine, '函数体至少要跨两行（`{` 与 `}`）');

  const cut = pseudoOfFunction(LST, idx, { sym: 'sub_42CA50', lines: 5 });
  assert.equal(cut.c.body.length, 5, '--lines 5 ⇒ 给 5 行');
  assert.equal(cut.c.truncated, true, '★ 截断了就必须说 truncated —— 否则读者会把"没给"当成"没有"');
  assert.equal(cut.c.bodyLines, r.c.bodyLines, '截断不改变"这个函数有多少行"这个事实');
  assert.equal(cut.c.body[0], r.c.body[0], '截断只砍尾巴，头一行仍是函数签名');
});

test('★ 缺 C 必须**明说**（实测有函数没有 C 体），且同时给出 `.lst` 区间', { skip }, () => {
  // ★ 这个符号是从"`.lst` 有 `proc near`、`.c` 没有定义"的那批里取的（实测 77 个之一）
  const r = pseudoOfFunction(LST, idx, { sym: 'sub_4D13F0' });
  assert.equal(r.c, null, '它没有 C 体 ⇒ c 必须是 null');
  assert.match(r.note ?? '', /没有定义/, '★ 必须明说"没有定义"，而不是给一个空 body');
  assert.ok(r.lst.toLine > r.lst.fromLine, '同时必须给 .lst 行区间（否则读者手里什么都没有）');
});

test('★ C 层必须覆盖**绝大多数**函数 —— 否则"用 C 读"这个前提不成立', { skip }, () => {
  const cFile = pickDecompiled(LST);
  assert.ok(cFile, '配套的 .c 必须在语料里（zip 里就是 4 个文件）');
  const sym = buildSymbolIndex(cFile);
  const procCount = (fs.readFileSync(LST, 'utf8').match(/ proc near/g) ?? []).length;
  assert.ok(procCount > 1000, `lst 里的 proc near 数量应当是几千量级，实际 ${procCount}`);
  const cov = sym.defs.size / procCount;
  assert.ok(cov > 0.95, `★ C 覆盖率 ${(cov * 100).toFixed(1)}% —— 低于 95% 就该重新评估"两层工作流"是否还成立`);
  assert.ok(sym.protos.size > 0, '`.c` 头部有一大段原型（本工具靠"行尾 `;`"把它们与定义分开）');
});

test('★ 没有配套 `.c` 时 `pickDecompiled` 返回 null（不静默造一个路径）', { skip }, () => {
  assert.equal(pickDecompiled(path.join(FILES_DIR, '根本没有这个文件.lst')), null);
  const hit = pickDecompiled(LST);
  assert.ok(hit && hit.endsWith('.c'), `应当由 .lst 推出同名 .c，实际 ${hit}`);
  assert.ok(fs.existsSync(hit), '推出的 .c 必须真的存在');
});

test('★ 定位不到函数时要**抛**（不许编一个符号出来）', { skip }, () => {
  // 0x1 这种地址不在任何已索引段里 ⇒ 必须抛，而不是返回一个空体
  assert.throws(() => pseudoOfFunction(LST, idx, { ea: 0x1 }), /定位不到函数符号|不落在任何已索引段/);
  assert.throws(() => pseudoOfFunction(LST, idx, { sym: 'not_a_symbol' }), /定位不到函数符号/);
});
