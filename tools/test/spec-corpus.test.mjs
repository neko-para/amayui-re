/** @env assets @kind gate @why effect 枚举 / 覆盖与语料脱节（报告会绿而错：effect 未登记或块集合漂了） */
/**
 * tools/test/spec-corpus.test.mjs —— spec 层与**语料**的对账（assets 档）
 *
 * 纯文本档（`spec.test.mjs`）只能验"声明内部自洽"；这一档回答只能看语料的问题：
 * ① `pilot.sites`（块入口）是否等于**语料里**这批操作数原语的 case / default 站点；
 * ② 从语料机械枚举的 effect 是否被 `covers` / `internal` / `residuals` 全部归属（未登记 = 0）；
 * ③ 每条规则的站点 EA 是否落在**指令行首**；引用的函数 / 分派指令 / default 标签是否存在。
 *
 * ★ `PILOT_PRIMITIVES` 是**这个试点自己的**枚举口径（测试夹具），不在通用 schema 里。
 *
 * 运行：`pnpm test:assets`（需要 `corpus/disasm/files/` 已解压）
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';

import { REPO_ROOT } from '../lib/paths.mjs';
import { pickListing } from '../lib/disasm.mjs';
import { functionSpans } from '../lib/shape-access.mjs';
import { checkCorpusBindings, loadSpec } from '../lib/spec.mjs';
import { computeEffectCoverage, derivePilotEffects } from '../lib/spec-effects.mjs';

const listing = pickListing(path.join(REPO_ROOT, 'corpus', 'disasm', 'files'));
const skip = listing ? false : '语料未解压（先 `pnpm tools disasm build`）';
const spec = loadSpec().model;

/** 本试点自己的枚举口径：五个操作数原语的分派指令 + 函数 */
const PILOT_PRIMITIVES = [
  { id: 'address', fn: 'sub_42AEA0', dispatchEa: '0x42af16' },
  { id: 'read-int', fn: 'sub_41BF50', dispatchEa: '0x41bf99' },
  { id: 'read-float', fn: 'sub_41C300', dispatchEa: '0x41c349' },
  { id: 'write-int', fn: 'sub_42B4B0', dispatchEa: '0x42b51f' },
  { id: 'write-float', fn: 'sub_42BA00', dispatchEa: '0x42ba6f' },
];

const normEa = (v) => `0x${Number.parseInt(String(v).replace(/^0x/i, ''), 16).toString(16)}`;

/** 从语料重新枚举:每个原语的 case 站点 + default 站点 */
function derivePilotSites(lines) {
  const sites = new Map();
  const eaOf = (line) => {
    const m = /^\.text:([0-9A-F]{8})/.exec(line);
    return m ? `0x${Number.parseInt(m[1], 16).toString(16)}` : null;
  };
  for (const p of PILOT_PRIMITIVES) {
    const jt = p.dispatchEa.replace(/^0x/i, '').toUpperCase().padStart(8, '0');
    for (const line of lines) {
      const m = /; jumptable ([0-9A-F]{8}) case (\d+)/.exec(line);
      if (m && m[1] === jt) {
        const ea = eaOf(line);
        if (ea) sites.set(ea, `${p.id} case ${m[2]}`);
      }
    }
    for (const line of lines) {
      if (!new RegExp(`def_${jt.slice(-6)}:`).test(line)) continue;
      const m = /; jumptable ([0-9A-F]{8}) default case/.exec(line);
      if (m && m[1] === jt) {
        const ea = eaOf(line);
        if (ea) sites.set(ea, `${p.id} default`);
      }
    }
  }
  return sites;
}

test('★ 试点块 = 语料里这五个原语的 case / default 站点（x86 EA 集合）', { skip }, () => {
  const lines = fs.readFileSync(listing, 'utf8').split('\n');
  const real = derivePilotSites(lines);
  assert.ok(real.size > 0, '语料里应当能枚举出站点');
  const declared = new Map();
  for (const s of spec.pilot.sites) declared.set(normEa(s.ea), s.note ?? s.ea);
  assert.deepEqual([...declared.keys()].sort(), [...real.keys()].sort(), '试点块 EA 集合与语料跳转表不一致');
  for (const [ea, note] of real) assert.ok(String(declared.get(ea)).includes(note.split(' ')[0]), `${ea} 的 note 应当说明它属于哪个原语`);
});

test('★ effect 由语料机械枚举，且全部被覆盖 / 内部 / 残差归属（未登记 = 0）', { skip }, () => {
  const fx = derivePilotEffects(spec);
  assert.ok(fx.ok, fx.why);
  assert.ok(fx.effects.length > 0);
  const ids = new Set();
  for (const e of fx.effects) {
    assert.match(e.id, /^0x[0-9a-f]+\/[a-z-]+\/\d+$/, `effect id 形态非法：${e.id}`);
    assert.match(e.block, /^0x[0-9a-f]+$/, `effect 缺 block：${e.id}`);
    assert.ok(!ids.has(e.id), `effect id 重复：${e.id}`);
    ids.add(e.id);
  }
  const cov = computeEffectCoverage(spec, fx.effects);
  assert.deepEqual(cov.unregistered, [], `未登记 effect：${cov.unregistered.map((e) => e.id).join(', ')}`);
  assert.equal(cov.covered + cov.internal + cov.residual, fx.effects.length);
  assert.ok(cov.covered > 0 && cov.internal > 0 && cov.residual > 0);
  assert.deepEqual(cov.redundantResidual, [], '残差选择器不应命中已覆盖的 effect');
});

test('★ 每条规则的站点 EA 都落在语料的指令行首', { skip }, () => {
  const r = checkCorpusBindings(spec);
  assert.deepEqual(r.problems, []);
});

test('★ 分派指令 / default 标签 / 函数都真的存在', { skip }, () => {
  const lines = fs.readFileSync(listing, 'utf8').split('\n');
  const starts = new Set();
  for (const line of lines) {
    const m = /^\.text:([0-9A-F]{8}) /.exec(line);
    if (m) starts.add(Number.parseInt(m[1], 16));
  }
  const syms = new Set(functionSpans(lines).map((s) => s.sym));
  const problems = [];
  for (const p of PILOT_PRIMITIVES) {
    const n = Number.parseInt(p.dispatchEa.replace(/^0x/i, ''), 16);
    if (!starts.has(n)) problems.push(`${p.id}.dispatchEa=${p.dispatchEa} 不是指令行首`);
    if (!syms.has(p.fn)) problems.push(`${p.id}.fn=${p.fn} 在语料里不是函数`);
    const jt = p.dispatchEa.replace(/^0x/i, '').toUpperCase().padStart(8, '0');
    if (!lines.some((l) => new RegExp(`def_${jt.slice(-6)}:`).test(l))) problems.push(`${p.id} 找不到 default 标签 def_${jt.slice(-6)}`);
  }
  for (const e of spec.entries) {
    for (const f of e.binding?.functions ?? []) if (!syms.has(f)) problems.push(`${e.id}.${f} 在语料里不是函数`);
  }
  assert.deepEqual(problems, []);
});
