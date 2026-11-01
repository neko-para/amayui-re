/** @env assets @kind contract @why 按需补齐在真模型上给不出候选（漏传播）· 草稿混进已声明窗口（会与 G1/G2 打架、且看起来像补过了）· witness 不是调用点（G16 会红）⇒ "用到时补一格"重新变成回语料手读 */
/**
 * tools/test/shape-need.assets.test.mjs —— `shape need` 在**真语料 + 真模型**上的守卫
 *
 * 判据（红了意味着什么）：
 *  ① 至少有一个**零成员类型**能给出传播候选（否则"按需补齐"对最该补的那一类无效）；
 *  ② 草稿的成员**全部**来自"未声明偏移"（⛔ 不许把已声明窗口里的偏移再写一遍——那会与 G1/G2 打架）；
 *  ③ 每个候选载体的 `witness` 落在一条 `call <该函数>` 的**调用点**行上（G16 的硬要求）。
 *
 * 要语料（`corpus/disasm/files/`）；缺席时 `t.skip`。
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';

import { loadListing } from '../lib/shape-access.mjs';
import { DEFAULT_OUT_DIR, REPO_ROOT } from '../lib/paths.mjs';

const hasCorpus = fs.existsSync(DEFAULT_OUT_DIR) && fs.readdirSync(DEFAULT_OUT_DIR).some((f) => f.endsWith('.lst'));
const skip = hasCorpus ? false : `语料未解压（先 \`pnpm tools disasm build\`）—— 期望目录 ${path.relative(REPO_ROOT, DEFAULT_OUT_DIR)}`;

test('★ need 真语料：零成员类型给出候选 + 未声明偏移 + 草稿；成员不与已声明窗口重叠、witness 落在 call 行', { skip }, async () => {
  const dir = path.join(REPO_ROOT, 'data', 'shape');
  const types = JSON.parse(fs.readFileSync(path.join(dir, 'types.json'), 'utf8')).types;
  const zero = Object.entries(types)
    .filter(([, t]) => (t.members ?? []).length === 0 && Number.isFinite(t.size))
    .map(([k]) => k);
  assert.ok(zero.length > 0, '本仓至少应有一个零成员类型（`T_FadeTimer` 那一类）；若全补完了，这条守卫要改成"零成员为空"的形态');

  const { buildNeed, analyzePropagation } = await import('../shape.mjs');
  const lines = loadListing();
  const prop = analyzePropagation({ lines });          // ★ 只跑一次（三次调用会各跑一遍 BFS）
  const proposals = zero.map((t) => buildNeed({ type: t, lines, prop }));
  const withCand = proposals.filter((p) => p.candidates.length);
  assert.ok(withCand.length > 0, `零成员类型里至少要有一个能给出传播候选；实得 ${zero.map((t) => `${t}:${proposals.find((p) => p.type === t).candidates.length}`).join(' ')}`);

  const p = withCand[0];
  assert.ok(p.stub.members?.length > 0, `${p.type} 有 ${p.candidates.length} 个候选却没有成员草稿`);
  const undeclared = new Set(p.undeclared.map((e) => e.off));
  for (const m of p.stub.members) {
    const off = Number.parseInt(m.off, 16);
    assert.ok(undeclared.has(off), `草稿成员 ${m.off} 不在未声明清单里 ⇒ 会与已声明窗口重叠（G1/G2 会打架）`);
  }
  // witness 必须是**调用点**：那一行逐字是 `call <fn>`（G16 要的证据）
  for (const c of (p.stub.carriers ?? []).slice(0, 3)) {
    assert.equal(c.witness.length, 1, `候选 ${c.fn} 的 witness 应当恰好一条调用点 EA`);
    const ea = Number.parseInt(c.witness[0], 16);
    const tag = `:${ea.toString(16).padStart(8, '0').toUpperCase()} `;
    const line = lines.find((l) => l.includes(tag));
    assert.ok(line, `witness EA ${c.witness[0]} 不在语料里`);
    assert.match(line, new RegExp(`\\bcall\\s+${c.fn}\\b`), `witness 必须是调用点（call ${c.fn}），实得：${line.trim()}`);
  }
});
