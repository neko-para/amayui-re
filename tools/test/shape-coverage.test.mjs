/** @env pure @kind gate @why 归档（"下一层能用的函数清单"）的算术一旦算错，就会把未闭合的函数算进"能用"，或把同一处缺口记两遍 —— 下一层据此判断"有没有未点名的副作用"会得出反向结论 */
/**
 * tools/test/shape-coverage-archive.test.mjs —— `coverageArchive()` 的口径守卫（纯函数）。
 *
 * 钉住三件事：
 * ① **两个口径不许混**：`unclosed` 按**函数**计，`counts.byReason` 按**原因**计（一个函数可有两个原因）；
 * ② 闭合的函数**不进任何分组**（⛔ 不制造"零原因"分组；那会让"没原因"与"没检查"长得一样）；
 * ③ 未知的原因 kind **当场报错**（闭集合 `REASON_KINDS`）—— 否则新原因会被静默丢出归档。
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';

import { coverageArchive, coverageOf, REASON_KINDS } from '../lib/shape-coverage.mjs';

const cov = (sym, kinds) => ({
  sym,
  closed: kinds.length === 0,
  reasons: kinds.map((k) => `${k}-text`),
  reasonKinds: kinds.map((k) => ({ kind: k, count: 1, text: `${k}-text` })),
  sites: 3,
});

test('★arch-01 按函数计 vs 按原因计：一个函数两个原因 ⇒ unclosed 1，但两个分组里都有它', () => {
  const a = coverageArchive([
    cov('sub_000001', ['indirectCall', 'unresolvedBase']),
    cov('sub_000002', ['indirectCall']),
    cov('sub_000003', []),
  ]);
  assert.equal(a.total, 3);
  assert.equal(a.closed, 1);
  assert.equal(a.unclosed, 2);
  assert.deepEqual(a.byReason.indirectCall, ['sub_000001', 'sub_000002']);
  assert.deepEqual(a.byReason.unresolvedBase, ['sub_000001']);
  assert.deepEqual(a.byReason.indirectJmp, []);
  // ★ 两个口径都在，且**不同**：把 byReason 的和当成"未闭合函数数"会虚高
  const sumByReason = Object.values(a.counts.byReason).reduce((x, y) => x + y, 0);
  assert.equal(sumByReason, 3);
  assert.notEqual(sumByReason, a.unclosed);
});

test('★arch-02 闭合的函数不进任何分组，也不出现在 byReason 的任何清单里', () => {
  const a = coverageArchive([cov('sub_000001', []), cov('sub_000002', []), cov('sub_000003', ['unparsedLine'])]);
  for (const k of REASON_KINDS) {
    assert.ok(!a.byReason[k].includes('sub_000001'), `${k} 分组里不该有闭合函数`);
    assert.ok(!a.byReason[k].includes('sub_000002'), `${k} 分组里不该有闭合函数`);
  }
  assert.deepEqual(a.byReason.unparsedLine, ['sub_000003']);
  assert.equal(a.functions.filter((f) => f.closed).length, 2);
});

test('★arch-03 未知原因 kind ⇒ **当场报错**（闭集合之外的原因不许被静默丢出归档）', () => {
  assert.deepEqual(REASON_KINDS, ['unparsedLine', 'indirectCall', 'indirectJmp', 'unresolvedBase']);
  const bad = { sym: 'sub_000009', closed: false, reasons: ['x'], reasonKinds: [{ kind: 'somethingNew', count: 1, text: 'x' }], sites: 0 };
  assert.throws(() => coverageArchive([bad]), /未知的未闭合原因 kind：somethingNew/);
});

test('★arch-04 函数行带 `reasonKinds`（字符串化的原因不许是归档的唯一真源）', () => {
  const a = coverageArchive([cov('sub_000001', ['unresolvedBase'])]);
  const f = a.functions[0];
  assert.deepEqual(f.reasonKinds, ['unresolvedBase']);
  assert.equal(f.sites, 3);
});

test('★arch-05 `lea r,[r+0]`（对齐填充）不许算成"未归类行"——它是假红，会让函数永远闭不上', () => {
  const lines = [
    '.text:00402000 sub_402000 proc near',
    '.text:00402000 lea     ecx, [ecx+0]',   // MSVC 6 字节填充：判定层已按同一口径不当访问
    '.text:00402006 mov     eax, [ecx+100h]',
    '.text:0040200D retn',
  ];
  const sites = [{ ea: 0x402006, access: 'r', baseKind: 'this', obj: 'Engine', absDisp: 0x100, problems: [], pointeePath: [] }];
  const padding = coverageOf(lines, 'sub_402000', { from: 0, to: lines.length - 1 }, 'ecx', sites);
  assert.equal(padding.escapes.unparsed.length, 0, `填充不该是"未归类行"：${JSON.stringify(padding.escapes.unparsed)}`);
  assert.ok(padding.kinds.pureReg >= 1, '填充要归到"纯寄存器"（不碰内存）');
  assert.equal(padding.closed, true, '除填充外没有缺口 ⇒ 必须判 L0 闭合');

  // 对照（⛔ 别把"未归类行"做成整类误杀）：真的"有内存操作数却没站点"仍然是缺口
  const real = [lines[0], '.text:00402000 mov     eax, [ecx+104h]', lines[3]];
  const gap = coverageOf(real, 'sub_402000', { from: 0, to: real.length - 1 }, 'ecx', sites);
  assert.equal(gap.escapes.unparsed.length, 1, '真缺口必须仍在');
  assert.equal(gap.closed, false);
});
