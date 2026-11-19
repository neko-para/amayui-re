/** @env pure @kind gate @why AOT 臂的 `guestToken().arm(...)` / `disarm()` 成对调用被删 · 或摘要的 `token     :` 行被删 / 被移出 `phase0Enabled()` 闸门 / 与解释器格式分叉 —— 四条都是**静默**的（运行期输出照旧、缺省档也看不出来），而且正是 `REQ-01M7JFEA7J8AWQHPXTW6GB41H0` 的形态 */
/**
 * **形态守卫（证明力弱：不证明运行期）** —— 只按源码文本判，不构建、不跑 exe。
 *
 * ## 它守什么
 * ① `src/aot/aot_run.cpp` 里**恰好各一次** `guestToken().arm(` 与 `guestToken().disarm(`（且 arm 在前）。
 *    - 删 `arm(` ⇒ 违规计数整轮关着（`check()` 未 arm 时恒真）⇒ AOT 上的容器并发守卫变假绿；
 *    - 删 `disarm(` ⇒ 运行收口之后的宿主析构 / 收尾仍记进本轮账（与解释器 `machine.cpp:1879` 的落点分叉）。
 * ② `src/aot/aot_host.cpp` 摘要里的 `token     : violations=… max-concurrent=…` 那行**恰好一条**
 *    `fprintf`，且它落在 `if (guestToken().phase0Enabled()) { … }` 的**块内**；同一个块里也有
 *    `dumpTokenPhase0(`（闸门只有一个）。越出闸门 ⇒ 缺省档 stdout 不再逐字不变
 *    （`AGENTS.md` §12.8 第 3 条）。
 * ③ 那行的**格式串与解释器完全一致**（`machine.cpp` 与 `aot_host.cpp` 各出现一次同一串）
 *    ⇒ 两臂的 `token     :` 行可逐字段对照。
 *
 * ## 它守不了什么（⛔ 不许把它说成运行期守卫）
 * ① **不证明运行期真的 arm / disarm 了**，也不证明摘要真的打出来了（那是
 *    `apps/emulator-lift/CMakeLists.txt#aot-token-summary` 的职责 —— 它真跑两次 AOT）；
 * ② 不判 `violations` 的数值（那是读数；且 2026-11-19 实测 AOT 真跑是 2，另一个缺陷在跟）；
 * ③ 按**字面量**判，改名 / 改行内写法能绕过（有意的取舍：这四条都是"整句被删 / 被挪"型回归）。
 *
 * ★ 为什么还要它：运行期用例只能抓 `arm(` 被删（`phase0Enabled()` 变 false ⇒ 少一行），
 *   抓不到 `disarm(` 被删（收口后的记账变化在十几毫秒的进程退出里不可观测）。
 */
import assert from 'node:assert/strict';
import fs from 'node:fs';
import test from 'node:test';

const RUN = new URL('../../apps/emulator-lift/src/aot/aot_run.cpp', import.meta.url);
const HOST = new URL('../../apps/emulator-lift/src/aot/aot_host.cpp', import.meta.url);
const INTERP = new URL('../../apps/emulator-lift/src/runtime/machine.cpp', import.meta.url);

const ARM = 'guestToken().arm(';
const DISARM = 'guestToken().disarm(';
const SUMMARY_FORMAT = 'token     : violations=%llu max-concurrent=%u%s%s\\n';
const SUMMARY_FPRINTF = `std::fprintf(out, "${SUMMARY_FORMAT}"`;

function read(url) {
  return fs.readFileSync(url, 'utf8');
}

function count(hay, needle) {
  let n = 0;
  for (let at = hay.indexOf(needle); at !== -1; at = hay.indexOf(needle, at + needle.length)) ++n;
  return n;
}

/** `open` 指向 `{`；返回与它配对的 `}` 的下标（找不到 ⇒ -1）。只数花括号（本文件的块里没有字符串伪装）。 */
function matchingBrace(text, open) {
  assert.equal(text[open], '{');
  let depth = 0;
  for (let i = open; i < text.length; ++i) {
    if (text[i] === '{') ++depth;
    else if (text[i] === '}') {
      --depth;
      if (depth === 0) return i;
    }
  }
  return -1;
}

/** 从 `from` 起找第一个 `if (guestToken().phase0Enabled()) {`，返回 `{` 的下标（找不到 ⇒ -1）。 */
function phase0GateOpen(text, from) {
  const at = text.indexOf('if (guestToken().phase0Enabled())', from);
  if (at === -1) return -1;
  const open = text.indexOf('{', at);
  return open === -1 ? -1 : open;
}

test('aot-token-shape/arm-disarm-pair', () => {
  const run = read(RUN);
  assert.equal(count(run, ARM), 1, '`AotHost::run` 里必须**恰好一次** `guestToken().arm(`（删掉 ⇒ 违规计数整轮关着）');
  assert.equal(count(run, DISARM), 1, '`AotHost::run` 里必须**恰好一次** `guestToken().disarm(`（删掉 ⇒ 收口后仍记进本轮账）');
  const armAt = run.indexOf(ARM);
  const disarmAt = run.indexOf(DISARM);
  assert.ok(armAt < disarmAt, '`arm(` 必须在 `disarm(` 之前（成对顺序）');
  // arm 必须落在 `AotHost::run` 函数体内（⛔ 不许挪到 prepare / main 里——那会让"每轮一次"变成"每进程一次"）。
  const runFn = run.indexOf('AotRunResult AotHost::run(const AotRunOptions& opts)');
  assert.notEqual(runFn, -1, '找不到 `AotHost::run` 定义');
  assert.ok(runFn < armAt, '`arm(` 必须定义在 `AotHost::run` 内');
});

test('aot-token-shape/summary-line-same-shape-as-interpreter', () => {
  const host = read(HOST);
  const interp = read(INTERP);
  assert.equal(count(host, SUMMARY_FPRINTF), 1, 'AOT 摘要必须有**恰好一条** `token     : violations=…` 的 fprintf');
  assert.equal(count(interp, SUMMARY_FPRINTF), 1, '解释器摘要必须有**恰好一条**同一格式串（两臂同形）');
});

test('aot-token-shape/summary-line-phase0-gated', () => {
  const host = read(HOST);
  const printAt = host.indexOf(SUMMARY_FPRINTF);
  const gateAt = phase0GateOpen(host, 0);
  assert.notEqual(gateAt, -1, '找不到 `if (guestToken().phase0Enabled()) {` 闸门');
  // ★ 闸门必须就是包住打印点的那一个：从 printAt 往前找最后一道 phase0 闸门，它必须还没闭合。
  let gate = phase0GateOpen(host, 0);
  let lastOpen = -1;
  while (gate !== -1 && gate < printAt) {
    lastOpen = gate;
    gate = phase0GateOpen(host, gate + 1);
  }
  assert.notEqual(lastOpen, -1, '打印点之前没有 `phase0Enabled()` 闸门 ⇒ 缺省档也会打这一行（默认 stdout 不再逐字不变）');
  assert.ok(matchingBrace(host, lastOpen) > printAt, '`token     :` 打印点必须落在 `phase0Enabled()` 块内');
  const gateClose = matchingBrace(host, lastOpen);
  const block = host.slice(lastOpen, gateClose);
  assert.ok(block.includes('dumpTokenPhase0('), '同一个 `phase0Enabled()` 块里必须也有 `dumpTokenPhase0(`（闸门只有一个）');
});
