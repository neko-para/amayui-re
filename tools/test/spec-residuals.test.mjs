/** @env pure @kind gate @why 残差地图的两轴被改坏（新章没登记通道 / 弃权不看正文 / 矩阵与明细不同源） */
/**
 * tools/test/spec-residuals.test.mjs —— `spec residuals`（残差地图）的契约与判别力
 *
 * 守四件事：
 * ① **两轴都闭合**：每条残差都落在**已登记通道**上（新章不登记 ⇒ 红），类别只会是 A/B/C。
 * ② **A 由规则正文决定**（弃权是**正文**的一句「未规定」，不是残差措辞）——造两条样本：
 *    正文弃权 + 残差措辞中性 ⇒ A；正文不弃权 + 残差中性 ⇒ C。
 * ③ **B 独立于 A**：`unconsumed` 标志只看残差措辞，且与类别正交（弃权规则下也能有"有写无读"）。
 * ④ **矩阵与明细同源**：各格之和 == 行数 == 类别计数之和；规则级统计只数规则、不数残差。
 *
 * 运行：`pnpm test`（纯文本 / 纯函数）
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';

import {
  RESIDUAL_CHANNELS,
  channelRules,
  classifyResidual,
  loadSpec,
  residualInventory,
  residualMatrix,
  ruleAbstains,
} from '../lib/spec.mjs';

const { model } = loadSpec();

test('★ 残差两轴闭合：通道已登记、类别只有 A/B/C', () => {
  const rows = residualInventory(model);
  assert.ok(rows.length > 0, '残差地图不能是空的');
  const unclassified = rows.filter((r) => r.channel === 'unclassified');
  assert.deepEqual(
    unclassified.map((r) => r.chapter || r.rule),
    [],
    '有章节没在 RESIDUAL_CHANNELS 里登记通道（加一行即可；不登记会让该子系统的缺陷评估漏掉）',
  );
  for (const r of rows) assert.ok(['A', 'B', 'C'].includes(r.cls), `类别非法：${r.id} → ${r.cls}`);
  for (const r of rows) assert.ok(r.rule && r.id, '每行都要有规则 id 与残差 id');
});

test('★ A 由规则正文决定（弃权 ≠ 残差措辞）', () => {
  // 正文弃权 + 残差措辞中性 ⇒ A
  const abstainEntry = { id: 'R-AB', chapter: 'c', title: 'x', text: '…这个值本版不规定。', residual: [{ id: 'RES-1', kind: 'unknown', what: '某个细节' }] };
  // 正文不弃权 + 残差中性 ⇒ C
  const pinnedEntry = { id: 'R-PIN', chapter: 'c', title: 'y', text: '…这个值唯一确定。', residual: [{ id: 'RES-2', kind: 'unknown', what: '某个细节' }] };
  const fake = { chapters: [{ id: 'c', file: 'chapters/60-ops-text.md' }], entries: [abstainEntry, pinnedEntry] };
  const rows = residualInventory(fake, { channels: { '60-ops-text': 'text' } });
  assert.equal(rows.find((r) => r.id === 'RES-1').cls, 'A');
  assert.equal(rows.find((r) => r.id === 'RES-2').cls, 'C');
  assert.equal(ruleAbstains(abstainEntry), true);
  assert.equal(ruleAbstains(pinnedEntry), false);
  // 正文弃权 ≠ 残差写「不规定」：措辞命中的那条规则若正文不弃权，仍然是 C
  assert.equal(classifyResidual({ what: '本版不规定' }, { abstains: false }), 'C');
  assert.equal(classifyResidual({ what: '本版不规定' }, { abstains: true }), 'A');
});

test('★ B（有写无读）独立于 A，且判据真能命中', () => {
  assert.equal(classifyResidual({ what: '请求只记一个计数，消费者没有建模。' }, { abstains: false }), 'B');
  const fake = {
    chapters: [{ id: 'c', file: 'chapters/60-ops-text.md' }],
    entries: [
      {
        id: 'R-AB',
        chapter: 'c',
        title: 'x',
        text: '本版不规定某值。',
        residual: [{ id: 'RES-U', kind: 'unknown', what: '请求发出去了，但没有消费者。' }],
      },
    ],
  };
  const [row] = residualInventory(fake, { channels: { '60-ops-text': 'text' } });
  assert.equal(row.cls, 'A', '类别按弃权优先');
  assert.equal(row.unconsumed, true, 'unconsumed 与类别正交：弃权规则下照样标出"有写无读"');
  const rows = residualInventory(model);
  assert.ok(rows.some((r) => r.cls === 'A'), 'A 类不能为空（否则地图失去首要用途）');
  assert.ok(rows.some((r) => r.unconsumed), '有写无读的残差不能为空（否则"功能做了一半"看不见）');
});

test('★ 矩阵与明细同源；规则级统计只数规则', () => {
  const rows = residualInventory(model);
  const { matrix, counts, channels } = residualMatrix(rows);
  const matrixTotal = [...matrix.values()].reduce((a, m) => a + m.total, 0);
  assert.equal(matrixTotal, rows.length);
  assert.equal(counts.A + counts.B + counts.C, rows.length);
  assert.equal(channels.length, matrix.size);
  for (const [ch, m] of matrix) assert.equal(m.A + m.B + m.C, m.total, `${ch} 的三个类别加起来不等于合计`);
  const rules = channelRules(model);
  assert.equal(
    [...rules.values()].reduce((a, x) => a + x.rules, 0),
    (model.entries ?? []).length,
    '规则级统计的 rules 之和必须等于规则总数（不是残差数）',
  );
  const abstain = [...rules.values()].reduce((a, x) => a + x.abstainRules, 0);
  assert.ok(abstain > 0 && abstain < (model.entries ?? []).length, '弃权规则数应在 (0, 全部) 之间');
  assert.ok(Object.keys(RESIDUAL_CHANNELS).length >= 20, '通道表不该被删空');
});
