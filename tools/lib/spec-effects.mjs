/**
 * tools/lib/spec-effects.mjs —— **effect 级覆盖**：把机械枚举出的 x86 effect 与 audit 的选择器对账
 *
 * 口径：
 * * 候选是 `audit.pilot.sites`（块入口 EA）；effect 由 `x86-effects` 从语料机械枚举，带 `block`。
 * * 规则的 `covers` 是**选择器**：`{blocks?, kinds?, ops?}`（至少一项）。
 * * 一个 effect 的归属按顺序判：**规则 covered** → **pilot.internal（有理由的内部省略）** →
 *   **pilot.residuals（明确残差，带 next）** → **未登记**（红）。
 * * `reg` / `stack` / `control` 不是"没有 effect"：它们在 `pilot.internal` 里逐类给出理由。
 */
import fs from 'node:fs';
import path from 'node:path';
import { REPO_ROOT } from './paths.mjs';
import { pickListing } from './disasm.mjs';
import { effectsForEaSites } from './x86-effects.mjs';

const sameEa = (a, b) => Number.parseInt(String(a).replace(/^0x/i, ''), 16) === Number.parseInt(String(b).replace(/^0x/i, ''), 16);

export function selectorMatches(sel, e) {
  if (!sel || typeof sel !== 'object') return false;
  if (sel.blocks !== undefined) {
    if (!Array.isArray(sel.blocks) || !sel.blocks.some((b) => sameEa(b, e.block))) return false;
  }
  if (sel.kinds !== undefined) {
    if (!Array.isArray(sel.kinds) || !sel.kinds.includes(e.kind)) return false;
  }
  if (sel.ops !== undefined) {
    if (!Array.isArray(sel.ops) || !sel.ops.includes(e.op)) return false;
  }
  if (sel.targets !== undefined) {
    if (!Array.isArray(sel.targets) || !sel.targets.includes(e.target)) return false;
  }
  return true;
}

/** 从语料枚举 pilot.sites 的 effect */
export function derivePilotEffects(spec, { repoRoot = REPO_ROOT } = {}) {
  const lst = pickListing(path.join(repoRoot, 'corpus', 'disasm', 'files'));
  if (!lst) return { ok: false, why: '语料未解压（corpus/disasm/files/*.lst 不在场）', effects: [], blocks: [] };
  const lines = fs.readFileSync(lst, 'utf8').split('\n');
  const sites = (spec?.pilot?.sites ?? spec?.sites ?? []).map((s) => s.ea);
  const r = effectsForEaSites(lines, sites);
  return r.ok ? { ...r, listing: path.relative(repoRoot, lst).split(path.sep).join('/') } : { ...r, effects: [], blocks: [] };
}

/**
 * 一个 effect 的归属。返回 `{covered, internal, residual, unregistered}` 以及逐条归属。
 * @param {object} spec 合并后的 model（含 entries / pilot）
 * @param {Array} effects
 */
export function computeEffectCoverage(spec, effects) {
  const rules = (spec.entries ?? []).filter((e) => e.disposition === 'handled' || e.disposition === 'replaced');
  const internal = new Map((spec.pilot?.internal ?? []).map((x) => [x.kind, x]));
  const residuals = spec.pilot?.residuals ?? [];
  const rows = [];
  const byKind = {};
  const unregistered = [];
  const redundantResidual = [];
  for (const e of effects) {
    byKind[e.kind] = byKind[e.kind] ?? { total: 0, covered: 0, internal: 0, residual: 0, unregistered: 0 };
    byKind[e.kind].total += 1;
    let hit = null;
    for (const r of rules) {
      const sel = (r.covers ?? []).find((s) => selectorMatches(s, e));
      if (sel) { hit = { how: 'covered', rule: r.id, sel }; break; }
    }
    if (!hit && internal.has(e.kind)) hit = { how: 'internal', why: internal.get(e.kind).why };
    if (!hit) {
      const res = residuals.find((x) => selectorMatches(x, e));
      if (res) hit = { how: 'residual', id: res.id, why: res.why, next: res.next };
    } else {
      const res = residuals.find((x) => selectorMatches(x, e));
      if (res && hit.how === 'covered') redundantResidual.push({ effect: e, rule: hit.rule, id: res.id });
    }
    if (!hit) { hit = { how: 'unregistered' }; unregistered.push(e); }
    byKind[e.kind][hit.how] += 1;
    rows.push({ ...e, ...hit });
  }
  const count = (how) => rows.filter((r) => r.how === how).length;
  return { rows, byKind, covered: count('covered'), internal: count('internal'), residual: count('residual'), unregistered, redundantResidual };
}
