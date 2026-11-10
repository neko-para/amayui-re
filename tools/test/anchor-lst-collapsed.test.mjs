#!/usr/bin/env node
/** @env pure @kind gate @suite toolchain @why `collapsed` 只认折叠的一种说法 ⇒ `COLLAPSED CHUNK OF FUNCTION` 这类站点不被标 collapsed，被判成"函数体只有一行注释"，依赖它的判定在这一类站点上绿而错 */
/**
 * tools/test/anchor-lst-collapsed.test.mjs —— `collapsed` 判据的**形状**（纯夹具，不吃资产）
 *
 * ## 为什么需要它（需求单 `REQ-01M71BT8GMBMMYQNA8FGWWJB`）
 * `anchor-lst.mjs` 的 `COLLAPSED_RE` 原先只认 `COLLAPSED FUNCTION` / `START OF FUNCTION CHUNK`，
 * 漏掉 IDA 的第二种说法 `COLLAPSED CHUNK OF FUNCTION`（语料里 **122 行**，实测）⇒ 那些 EA 的
 * `collapsed` 是 `false`：他们被当成"有 listing 行但无真指令"的普通站点，`bodies` 里只有一条注释放不进
 * 真正的指令，而**依赖 `collapsed` 判"这是折叠块、不是函数体"的判定在这一类站点上绿而错**。
 *
 * ## 两个判据都要钉（它们**不是**同一个问题）
 * * `COLLAPSED_RE`（本模块）= **布尔**站点判据："这行注释是不是折叠块"；
 * * `disasm.mjs` 的 `parseCollapsedLine` = **读出长度**的判据（`[NNNNNNNN BYTES: …]`），
 *   它**已经**同时认 `FUNCTION` 与 `CHUNK OF FUNCTION`（见那里的注释）—— 本文件钉住这一点，
 *   免得"一个认了、一个没认"这种两套判据漂开的老问题再出现。
 *
 * ## 怎么才会红
 * 1. 有人把 `COLLAPSED_RE` 改回"只认某一种固定短语" ⇒ 第 1、2 条当场红（三种形态逐个点名）；
 * 2. 有人把 `collapsed` 的赋值从 `parseListingText` 里删掉 ⇒ 第 2 条红；
 * 3. `parseCollapsedLine` 只认一种形态 / 把长度读成别的东西 ⇒ 第 3 条红。
 *
 * ★ **夹具是语料里的逐字原行**（不是编出来的形状）：取自
 *   `corpus/disasm/files/AGE.EXE__dumped.sectfix.EXE.lst`，改语料前这些行都在其中。
 * ★ 全语料的**机械遍历**在同名的 `.assets` 文件里（那条要真语料，缺席则 skip）。
 *
 * 运行：`pnpm verify`（`@env pure`，零外部依赖）
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';

import { parseCollapsedLine } from '../lib/disasm.mjs';
import { COLLAPSED_RE, LST_LOC_RE, parseListingText } from '../lib/anchor-lst.mjs';

/**
 * 语料里的**逐字原行**（前五条逐字来自 `corpus/disasm/files/AGE.EXE__dumped.sectfix.EXE.lst`；
 * `insn` 一条是合成的 —— 只为反向那一半"指令行不是折叠块"，它不需要语料形状）。
 */
const VERBATIM = {
  fn: '.text:004E8A6E ; [0000004C BYTES: COLLAPSED FUNCTION _CxxThrowException(x,x). PRESS CTRL-NUMPAD+ TO EXPAND]',
  chunk: '.text:004E9B52 ; [0000002B BYTES: COLLAPSED CHUNK OF FUNCTION _setlocale. PRESS CTRL-NUMPAD+ TO EXPAND]',
  chunk2: '.text:00401CA0 ; [0000000A BYTES: COLLAPSED CHUNK OF FUNCTION sub_4D0D00. PRESS CTRL-NUMPAD+ TO EXPAND]',
  startChunk: '.text:00401C90 ; START OF FUNCTION CHUNK FOR sub_401FA0',
  plainComment: '.text:0048F000 ; int __thiscall sub_48F000(_DWORD *this, int, int)',
  insn: '.text:00001000 push    ebp',
};

/** 取一行 listing 的正文（`段:EA 正文`）—— 判据作用在**正文**上 */
const bodyOf = (line) => {
  const m = LST_LOC_RE.exec(line);
  assert.ok(m, `夹具不是 listing 行：${line}`);
  return m[3].trim();
};

test('★ `COLLAPSED_RE` 必须同时认三种折叠说法（`COLLAPSED FUNCTION` / `COLLAPSED CHUNK OF FUNCTION` / `START OF FUNCTION CHUNK`）', () => {
  for (const [k, line] of Object.entries(VERBATIM)) {
    const body = bodyOf(line);
    if (k === 'plainComment' || k === 'insn') continue;
    assert.equal(COLLAPSED_RE.test(body), true, `★ 折叠形态没被 \`COLLAPSED_RE\` 命中（key=${k}）：「${body}」—— 漏掉它 = 这种站点不标 collapsed，被判成"函数体只有一行注释"`);
  }
  // 反向（判据双向）：普通注释 / 普通指令行**不许**被当成折叠块
  for (const k of ['plainComment', 'insn']) {
    assert.equal(COLLAPSED_RE.test(bodyOf(VERBATIM[k])), false, `★ 非折叠行被当成折叠块（key=${k}）：「${VERBATIM[k]}」`);
  }
  // ★ 逐字钉住"只看词根"这条口径：任何含 COLLAPSED 的注释行都命中，不管它后面跟的是 FUNCTION 还是别的
  for (const tail of ['[0000004C BYTES: COLLAPSED FUNCTION f.', '[0000002B BYTES: COLLAPSED CHUNK OF FUNCTION f.', '[00000010 BYTES: COLLAPSED SOMETHING ELSE f.']) {
    assert.equal(COLLAPSED_RE.test(`; ${tail}`), true, `★ "注释行里出现 COLLAPSED 就算折叠块"这条口径被收窄了：「${tail}」`);
  }
});

test('★ `parseListingText` 的 `collapsed` 字段：三种折叠形态都置 true，普通注释 / 指令行置 false', () => {
  const idx = parseListingText(
    [
      VERBATIM.fn,
      VERBATIM.chunk,
      VERBATIM.chunk2,
      VERBATIM.startChunk,
      VERBATIM.plainComment,
      '.text:0048F002 mov     ebp, esp',
    ].join('\n'),
  );
  const collapsed = (ea) => idx.byEa.get(ea)?.collapsed;
  assert.equal(collapsed(0x4e8a6e), true, '`COLLAPSED FUNCTION` ⇒ `collapsed: true`');
  assert.equal(collapsed(0x4e9b52), true, '★ `COLLAPSED CHUNK OF FUNCTION` ⇒ 必须 `collapsed: true`（本单的缺陷点：它曾被当成普通站点）');
  assert.equal(collapsed(0x401ca0), true, '★ 同上，另一条 `COLLAPSED CHUNK OF FUNCTION`（函数体的中段 CHUNK）');
  assert.equal(collapsed(0x401c90), true, '`START OF FUNCTION CHUNK` ⇒ `collapsed: true`（口径的第三形态）');
  assert.equal(collapsed(0x48f000), false, '★ 普通注释行**不是**折叠块（否则"函数体只有一行注释"这种误判会扩散到所有带注释的 EA）');
  assert.equal(collapsed(0x48f002), false, '真指令行不是折叠块');
  // ★ 前提：这三个 EA 确实只靠注释行存在（`real === false`）—— 那正是"折叠站点"的形状
  for (const ea of [0x4e8a6e, 0x4e9b52, 0x401ca0, 0x401c90]) {
    assert.equal(idx.byEa.get(ea)?.real, false, `0x${ea.toString(16)} 是折叠站点（有 listing 行、无真指令）——夹具形状不对，这条用例就没在测折叠站点`);
  }
});

test('★ `disasm.mjs` 的 `parseCollapsedLine` 覆盖同一群体：三种形态都非 null，长度读的是**这一行**声明的字节数', () => {
  const cases = [
    [VERBATIM.fn, 0x4c, '`COLLAPSED FUNCTION` ⇒ 0x4C'],
    [VERBATIM.chunk, 0x2b, '★ `COLLAPSED CHUNK OF FUNCTION` ⇒ 0x2B（它与 `COLLAPSED_RE` 是两套判据，不许一套认了另一套没认）'],
    [VERBATIM.chunk2, 0x0a, '★ 同上，另一条 CHUNK 行 ⇒ 0x0A'],
    [VERBATIM.startChunk, null, '`START OF FUNCTION CHUNK` 没有 `[NNNN BYTES:` ⇒ `declaredBytes` 必须是 null（不是 0：0 会让下游以为"空区间"）'],
  ];
  for (const [line, want, why] of cases) {
    const r = parseCollapsedLine(line);
    assert.ok(r, `★ 折叠行没被 \`parseCollapsedLine\` 认出来：${line}`);
    assert.equal(r.declaredBytes, want, why);
  }
  for (const k of ['plainComment', 'insn']) {
    assert.equal(parseCollapsedLine(VERBATIM[k]), null, `非折叠行不许被 \`parseCollapsedLine\` 认成折叠行（key=${k}）`);
  }
});
