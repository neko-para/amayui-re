/** @env pure @kind gate @why 证据锚指向的站点用例必须真的列出该 rule；否则“用例存在”≠“用例覆盖该 rule” */
/**
 * tools/test/spec-evidence-covers-rule.test.mjs —— spec 证据锚的**覆盖**守卫
 *
 * `bindings.json` 的 `evidence` 可以写 `guard:<path>#<用例名>`。`pnpm tools spec validate` 只解析
 * “文件存在 + 用例名存在于文件里”，**不检查这个用例有没有覆盖该 rule**。
 * 实测（2026-10-30 复审）：19 条新规则引用 `spec-ops-100-200-sites`，而该用例的 `RULES`
 * 是硬编码名单、24 条新规则一条都不在里面 ⇒ 站点写错/删空，这条 evidence 也不会红。
 *
 * 本守卫只针对**带 `const RULES = [...]` 名单的站点用例**：名单里必须出现引用它的 rule id。
 * ⛔ 不是通用的“证据语义正确”判据 —— 它只堵住“锚指向一个不包含该 rule 的名单”这一类空转。
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';

import { REPO_ROOT } from '../lib/paths.mjs';

const BINDINGS = path.join(REPO_ROOT, 'data', 'spec', 'bindings.json');

function collect(bindings) {
  const rows = bindings.bindings ?? bindings.rules ?? [];
  const problems = [];
  const cache = new Map();
  for (const row of rows) {
    const rule = row.rule;
    for (const evidence of row.evidence ?? []) {
      if (!evidence.startsWith('guard:')) continue;
      const spec = evidence.slice('guard:'.length);
      const hash = spec.indexOf('#');
      const file = hash < 0 ? spec : spec.slice(0, hash);
      if (!file.endsWith('.mjs')) continue;
      const abs = path.join(REPO_ROOT, file);
      if (!fs.existsSync(abs)) continue; // 存在性由 spec validate 管
      if (!cache.has(abs)) cache.set(abs, fs.readFileSync(abs, 'utf8'));
      const text = cache.get(abs);
      if (!text.includes('const RULES')) continue; // 只堵“硬编码名单”这一类
      if (!text.includes(`'${rule}'`) && !text.includes(`"${rule}"`)) {
        problems.push(`${rule} 的证据锚 ${file} 的 RULES 名单里没有它（用例存在 ≠ 覆盖该 rule）`);
      }
    }
  }
  return problems;
}

test('★ 站点用例的 RULES 名单必须覆盖引用它的 rule', () => {
  const bindings = JSON.parse(fs.readFileSync(BINDINGS, 'utf8'));
  const problems = collect(bindings);
  assert.deepEqual(problems, [], problems.join('\n'));
});
