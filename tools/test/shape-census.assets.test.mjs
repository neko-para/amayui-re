/** @env assets @kind contract @why 全表普查的口径被改坏（分桶不闭合 / 偏移聚类丢站点 / 过滤器失效 / 不可检查的载体不再点名） */
/**
 * tools/test/shape-census.assets.test.mjs —— `pnpm tools shape census` 的**结构不变量**守卫
 *
 * ⛔ 不断言"当前取值"（那是哨兵的事，见 `AGENTS.md` §10）：这里只钉**闭合关系**与**点名义务** ——
 * 它们红了都指向真实不一致：
 *  ① `this` 基址的站点**要么**是可用偏移、**要么**是"位移不可用"，不许两头不着地；
 *  ② 偏移聚类必须**穷尽** `obj0` 站点（聚类丢站点 = "哪些偏移被碰"整类静默消失）；
 *  ③ handler 底数自洽（找到 + 找不到 = 普查数；唯一符号数 ≤ opcode 数——存在多 opcode 共用）；
 *  ④ 没有对象根的 handler 必须**被点名**，且它们的 `obj0` 计数必须是 0（查不到根 ⇒ 不许假绿）；
 *  ⑤ `--match` 过滤器真的收窄（否则"看某一族"的用法是假的）。
 *
 * 要语料（`corpus/disasm/files/`）；缺席时 `t.skip`。
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';

import { loadListing } from '../lib/shape-access.mjs';
import { censusHandlers } from '../lib/shape-census.mjs';
import { DEFAULT_OUT_DIR, REPO_ROOT } from '../lib/paths.mjs';

const hasCorpus = fs.existsSync(DEFAULT_OUT_DIR) && fs.readdirSync(DEFAULT_OUT_DIR).some((f) => f.endsWith('.lst'));
const skip = hasCorpus ? false : `语料未解压（先 \`pnpm tools disasm build\`）—— 期望目录 ${path.relative(REPO_ROOT, DEFAULT_OUT_DIR)}`;

/** 普查只跑一次（~1 s；缺语料时为 null） */
const C = hasCorpus ? censusHandlers({ lines: loadListing(), repoRoot: REPO_ROOT }) : null;

test('① `this` 基址的站点：可用偏移 + 位移不可用 = 全部（两头不着地就是漏账）', { skip }, () => {
  const s = C.sites;
  assert.equal(s.byBaseKind.this, s.obj0 + s.partialLea,
    `this 桶 ${s.byBaseKind.this} != obj0 ${s.obj0} + 位移不可用 ${s.partialLea}`);
  // 位移不可用的两个原因都必须报出来（⛔ 不许只给总和）
  assert.equal(s.partialTakeAddr + s.partialIndexUnresolved, s.partialLea, '两个原因之和必须等于总数');
});

test('② 偏移聚类穷尽 `obj0` 站点（聚类丢站点 ⇒ "哪些偏移被碰"静默消失）', { skip }, () => {
  const sum = C.obj0Offsets.reduce((a, o) => a + o.sites, 0);
  assert.equal(sum, C.sites.obj0, `Σ 偏移簇站点 ${sum} != obj0 站点 ${C.sites.obj0}`);
  const idxSum = C.indexedClusters.reduce((a, c) => a + c.sites, 0);
  assert.ok(idxSum <= C.sites.obj0, '带索引的簇只能是 obj0 站点的子集');
  assert.ok(C.obj0Offsets.every((o) => o.handlers >= 1 && o.sampleEAs.length >= 1), '每个偏移簇都要能追到 handler 与样例 EA');
});

test('③ handler 底数自洽（找到 + 找不到 = 普查数；唯一符号 ≤ opcode，因为有共用的）', { skip }, () => {
  const h = C.handlerSet;
  assert.equal(h.found + h.missing.length, h.scanned, '找到 + 找不到 != 普查数');
  assert.ok(h.uniqueSymbols <= h.opcodes, '唯一符号数不可能多于 opcode 数（多 opcode 共用同一 handler）');
  assert.ok(h.sharedBySeveralOpcodes.every((x) => x.opcodes.length > 1), '共用清单里每条的 opcode 数必须 > 1');
});

test('④ 没有对象根的 handler 必须被点名，且它们不许贡献 `obj0` 站点（假绿的反面）', { skip }, () => {
  const noRoot = C.perHandler.filter((h) => !h.root);
  assert.deepEqual(C.roots.withoutRoot.slice().sort(), noRoot.map((h) => h.sym).sort(),
    '`roots.withoutRoot` 必须与逐 handler 的事实一致（⛔ 不许只报个数字）');
  for (const h of noRoot) assert.equal(h.obj0, 0, `${h.sym} 没有对象根 ⇒ 不可能有 obj0 相对访问`);
});

test('⑤ `--match` 真的收窄（否则"只看某个 handler"的用法是假的）', { skip }, () => {
  const one = censusHandlers({ lines: loadListing(), repoRoot: REPO_ROOT, match: 'sub_42C5E0' });
  assert.equal(one.handlerSet.scanned, 1);
  assert.equal(one.perHandler.length, 1);
  assert.equal(one.perHandler[0].sym, 'sub_42C5E0');
  // 收窄后底数（opcode / 唯一符号）仍报全表口径 —— 否则会被误读成"表里只有这一个"
  assert.equal(one.handlerSet.opcodes, C.handlerSet.opcodes);
});
