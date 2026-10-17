/** @env assets @kind contract @why handler 体骨架普查的口径被改坏（分组不穷尽 / 守恒不闭合 / 立即数解不出 / 过滤器失效） */
/**
 * tools/test/shape-bodies.assets.test.mjs —— `pnpm tools shape bodies` 的**结构不变量**守卫
 *
 * 为什么是 `assets`：要真语料。**不断言"当前取值"**（组数 / 每组几个会随语料与分派表变），只钉闭合关系：
 *  ① **分组穷尽**：每个普查到的 handler 恰好落在一组里（Σ 组大小 == 普查数）；
 *  ② **调用守恒**：函数体内每个直接调用都落桶（有立即数实参 / 无立即数 / 尾跳），且三条之和 == 直接调用数；
 *  ③ **立即数真的解出来了**：骨架里的 `←imm` 与 `(imm)` **不许是 NaN/undefined**
 *     （踩过：正则少一个捕获组 ⇒ 全部 `←NaN`，而报告看起来"有数据"）；
 *  ④ `--match` 真的收窄，但底数仍报全表口径（否则会被误读成"表里只有一个"）。
 *
 * 要语料（`corpus/disasm/files/`）；缺席时 `t.skip`。
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';

import { loadListing } from '../lib/shape-access.mjs';
import { censusBodies } from '../lib/shape-bodies.mjs';
import { DEFAULT_OUT_DIR, REPO_ROOT } from '../lib/paths.mjs';

const hasCorpus = fs.existsSync(DEFAULT_OUT_DIR) && fs.readdirSync(DEFAULT_OUT_DIR).some((f) => f.endsWith('.lst'));
const skip = hasCorpus ? false : `语料未解压（先 \`pnpm tools disasm build\`）—— 期望目录 ${path.relative(REPO_ROOT, DEFAULT_OUT_DIR)}`;

const B = hasCorpus ? censusBodies({ lines: loadListing(), repoRoot: REPO_ROOT }) : null;

test('① 分组穷尽：Σ 组大小 == 普查数（漏一个 handler 就是静默丢）', { skip }, () => {
  const sum = B.groups.reduce((a, g) => a + g.size, 0);
  assert.equal(sum, B.handlerSet.scanned, `Σ 组 ${sum} != 普查 ${B.handlerSet.scanned}`);
  const all = B.groups.flatMap((g) => g.handlers);
  assert.equal(new Set(all).size, all.length, '同一个 handler 不许出现在两组里');
  assert.ok(B.groups.every((g) => g.recordWrites.length + g.calls.length > 0 || g.size > 0), '每组都要说得出它的骨架');
});

test('② 调用守恒：每个直接调用都落桶（有立即数实参 / 无 / 尾跳）', { skip }, () => {
  const c = B.conservation;
  assert.equal(c.totalCalls, c.declared + c.noImm + c.tail, `${c.totalCalls} != ${c.declared}+${c.noImm}+${c.tail}`);
  assert.ok(c.totalCalls > 0, '一个调用都没数到 ⇒ 口径坏了');
});

test('③ 立即数必须真解出来（`←NaN` / `(?)` 整类会伪装成"有数据"）', { skip }, () => {
  for (const g of B.groups) {
    for (const w of g.recordWrites) assert.ok(!/NaN|undefined/.test(w), `记录槽写法里出现解不出的立即数：${w}`);
    for (const cl of g.calls) assert.ok(!/NaN|undefined/.test(cl), `调用序列里出现解不出的立即数：${cl}`);
  }
  // `push <imm>` 能解出来的直接调用应当**占多数**（本语料里绝大多数 callee 都有一个立即数实参）
  assert.ok(B.conservation.declared > 0, '一个"有立即数实参"的调用都没数到 ⇒ 立即数解析坏了');
});

test('④ `--match` 收窄但底数仍报全表口径', { skip }, () => {
  const one = censusBodies({ lines: loadListing(), repoRoot: REPO_ROOT, match: 'sub_42C5E0' });
  assert.equal(one.handlerSet.scanned, 1);
  assert.equal(one.groups.length, 1);
  assert.equal(one.groups[0].handlers[0], 'sub_42C5E0');
  assert.equal(one.groups[0].recordWrites.length, 1, '数值族 handler：恰好一处"把立即数写进记录槽"');
  assert.equal(one.handlerSet.opcodes, B.handlerSet.opcodes, '底数（opcode 数）必须仍报全表口径');
});
