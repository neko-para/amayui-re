/** @env pure @kind gate @suite toolchain @why bench 的日志解析坏了会让吞吐基线静默取到错数值（把 boot 当窗口 / Δms 与 Δinsns 算反 / 只有一个打点也照样出数） */
/**
 * tools/test/lift-bench.test.mjs — `pnpm tools lift bench` 的**解析器**守卫（纯档）
 *
 * 只测"日志 → 数值"这一层（`parseBenchLog` 是纯函数）：打点识别 / 窗口选择 / ms 与 Minsn 的换算 /
 * 块命中率与平均块长 / 三种必须**响亮失败**的边界。
 *
 * ⛔ 不测"真跑一遍 exe"：那要安装树 + 几十秒墙钟 + 一台安静的机器（`@env assets` 都不够，是墙钟工况）。
 * 真读数走 `pnpm tools lift bench <工况> --n 5`，落 `.cache/lift-bench/*.json`。
 *
 * ★ 为什么这几条断言值得守：这套换算的几处错法**都不会让程序崩**，只会把基线写错一位数：
 *   ① 把第一个打点当成"起点 = 0"（boot 混进窗口）⇒ ms/M 偏高；
 *   ② 同一 tick 的重复点 / `at frame` 打点 ⇒ Δ=0，除出 Infinity / NaN；
 *   ③ 把 `insns :` 的累计值当窗口的 Δ（或反过来）⇒ 差 2~3 倍。
 */

import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';

import { REPO_ROOT } from '../lib/paths.mjs';
import { parseBenchLog, resolveBenchScript } from '../lift.mjs';

/** 一次正常运行的日志骨架（数字取自 2026-11-10 的实跑，便于人眼核对量级）。 */
const LOG_LINES = [
  'input-script: apps/emulator-lift/scripts/bench/idle.txt（3 条动作；触发 = at frame / at guest_ms / at insn / after / wait）',
  'game-dir  : E:\\Projects\\amayui-re\\links\\install',
  '[input-script] frame=263 guest_ms=439613379 insns=250000004 wait 0 insn',
  '[input-script] frame=974 guest_ms=439634374 insns=450000035 wait 0 insn',
  'stop      : stopped @ eip=004CAF72',
  'insns     : 450000035',
  'blocks    : 10934 (insns cached 63786, hits 56460000, misses 8000)',
  'shims     : imported 310 / called-unimplemented 0',
];
const LOG_OK = LOG_LINES.join('\n');

test('解析器：两个打点 ⇒ 窗口 = 两者之差，ms/M 与 MIPS 互为倒数换算', () => {
  const r = parseBenchLog(LOG_OK);
  assert.equal(r.marks.length, 2);
  assert.equal(r.duplicateMarks, 0);
  // 窗口 = 第一个打点 → 最后一个打点
  assert.deepEqual(
    [r.window.from.insns, r.window.to.insns, r.window.from.frame, r.window.to.frame],
    [250000004, 450000035, 263, 974],
  );
  assert.equal(r.window.hostMs, 439634374 - 439613379);
  assert.equal(r.window.insns, 450000035 - 250000004);
  // 20995 ms / 200,000,031 条 = 104.97 ms/百万条 = 9.526 M insn/s（人可核对）
  assert.ok(Math.abs(r.msPerMinsn - 104.975) < 0.01, `msPerMinsn=${r.msPerMinsn}`);
  assert.ok(Math.abs(r.mips - 9.526) < 0.001, `mips=${r.mips}`);
  // ★ 真不变量：ms/百万条 × 百万条/秒 = 1000（换算写反了这条立刻红）
  assert.ok(Math.abs(r.msPerMinsn * r.mips - 1000) < 1e-6, `${r.msPerMinsn} × ${r.mips}`);
  assert.deepEqual(r.problems, []);
});

test('解析器：块命中率 / 平均块长取自运行摘要（不靠 profiler）', () => {
  const r = parseBenchLog(LOG_OK);
  const hits = 56460000;
  const misses = 8000;
  assert.equal(r.summary.hits, hits);
  assert.equal(r.summary.misses, misses);
  assert.equal(r.summary.insns, 450000035);
  assert.ok(Math.abs(r.blockHitRate - hits / (hits + misses)) < 1e-12);
  // 平均块长 = 总执行条数 / 块执行次数（约 8 条/块）
  assert.ok(Math.abs(r.avgBlockLen - 450000035 / (hits + misses)) < 1e-9);
  assert.ok(r.avgBlockLen > 7.9 && r.avgBlockLen < 8.0, `avgBlockLen=${r.avgBlockLen}`);
});

test('解析器：多个打点 ⇒ 窗口仍是「第一个 → 最后一个」（中间点只作诊断，不参与换算）', () => {
  const text = [
    ...LOG_LINES.slice(0, 3),
    '[input-script] frame=600 guest_ms=439620000 insns=350000000 wait 0 insn',
    ...LOG_LINES.slice(3),
  ].join('\n');
  const r = parseBenchLog(text);
  assert.equal(r.marks.length, 3);
  // 中间点（350M / +6621 ms）不进窗口：窗口仍是 250M → 450M
  assert.equal(r.window.from.insns, 250000004);
  assert.equal(r.window.to.insns, 450000035);
  assert.equal(r.window.hostMs, 20995);
  assert.equal(r.window.insns, 200000031);
});

test('解析器：同一 tick 的重复打点（脚本尾部 quit 与最后一个 wait 同时到期）⇒ 去掉，不算第三个点', () => {
  const text = [
    ...LOG_LINES.slice(0, 4),
    '[input-script] frame=974 guest_ms=439634374 insns=450000035 quit',
    ...LOG_LINES.slice(4),
  ].join('\n');
  const r = parseBenchLog(text);
  assert.equal(r.marks.length, 2, '同 (guest_ms, insns) 的连续点必须合并');
  assert.equal(r.duplicateMarks, 1);
  assert.equal(r.window.hostMs, 20995);
  assert.equal(r.window.insns, 200000031);
});

test('解析器：非打点的 `[input-script]` 行不算打点（click-release / marker edge / release 账）', () => {
  const text = [
    ...LOG_LINES.slice(0, 4),
    '[input-script] frame=974 guest_ms=439634374 click-release at=(812,622) left auto=1 waited_samples=3 hold_ms=52',
    '[input-script] frame=974 guest_ms=439634374 marker edge: cur=0 id=0x00000000 name=MAIN',
    '[input-script] release  : total=2 auto=2 explicit=0 deferred-ups=0 bounded-no-sample=0 waited-samples-max=3 hold-ms-max=52',
    ...LOG_LINES.slice(4),
  ].join('\n');
  const r = parseBenchLog(text);
  assert.equal(r.marks.length, 2, '只有 `frame= guest_ms= insns=` 那一行是打点');
  assert.equal(r.window.insns, 200000031);
});

test('解析器：打点 < 2 ⇒ 报错（⛔ 不许拿单点当窗口）', () => {
  const text = LOG_LINES.filter((l) => !l.includes('insns=450000035')).join('\n');
  assert.throws(() => parseBenchLog(text), /只有 1 个打点/);
});

test('解析器：一个打点都没有 ⇒ 报错并说清缺什么', () => {
  assert.throws(
    () => parseBenchLog('input-script: x.txt（0 条动作）\nstop      : stopped @ eip=004CAF72\n'),
    /没有 `\[input-script\]` 打点/,
  );
});

test('解析器：两个打点落在同一刻（零宽窗口）⇒ 报错，不产生 Infinity/NaN', () => {
  const text = [
    '[input-script] frame=263 guest_ms=439613379 insns=250000004 wait 0 insn',
    '[input-script] frame=263 guest_ms=439613379 insns=250000004 wait 0 insn',
    'insns     : 250000004',
    'blocks    : 10 (insns cached 80, hits 100, misses 1)',
  ].join('\n');
  assert.throws(() => parseBenchLog(text), /窗口至少要 2 个点|Δinsns|Δguest_ms/);
});

test('解析器：摘要缺席（运行没正常收口）⇒ 数能算，但进 problems', () => {
  const text = LOG_LINES.filter((l) => !/^(insns|blocks)\s*:/.test(l)).join('\n');
  const r = parseBenchLog(text);
  assert.equal(r.problems.length, 2, r.problems.join('；'));
  assert.equal(r.summary.insns, null);
  assert.equal(r.summary.hits, null);
  assert.equal(r.blockHitRate, null);
  assert.equal(r.avgBlockLen, null);
  assert.ok(Math.abs(r.msPerMinsn - 104.975) < 0.01);
});

test('工况名 → 脚本：`idle` 解析到入库脚本，且它自带 ≥2 个打点（工具能用的前提）', () => {
  const script = resolveBenchScript('idle');
  assert.match(script, /^apps\/emulator-lift\/scripts\/bench\/idle\.txt$/);
  const body = fs.readFileSync(path.join(REPO_ROOT, script), 'utf8');
  const marks = body.split(/\r?\n/).filter((l) => /^wait\s+\d+\s+insn\b/.test(l.trim()));
  assert.ok(marks.length >= 2, `idle.txt 的打点 ${marks.length} 个（要 ≥2 才量得出窗口）`);
  assert.throws(() => resolveBenchScript('no-such-scenario'), /找不到工况脚本/);
});
