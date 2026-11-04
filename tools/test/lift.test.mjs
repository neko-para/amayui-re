/** @env pure @kind gate @why 覆盖清单与 C++ 注册表漂了（stub 没有理由 / 两处不一致 / 派生文件没接上） */
/**
 * tools/test/lift.test.mjs — 覆盖清单的**纯档**守卫（不需要 LFS 资产）
 *
 * 两条：
 * ① `data/lift/overrides.json` ↔ `apps/emulator-lift/src/shims/*.cpp` 的 stub 登记双向一致
 *    （含派生缺口文件；reason 逐字相等）。
 * ② `registerImportGaps` 真的接进了注册链（声明 + 调用），否则派生缺口文件是"写了没人用"。
 *
 * ★ 需要 PE 导入表的那部分（裸缺口 = 0、sha256 锚）在 `pnpm tools lift check` 里；
 *   它要 `corpus/` 的拆壳件 ⇒ 不进 `@env pure`，由 `pnpm tools lift check` / requirements 门禁跑。
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';

import { REPO_ROOT } from '../lib/paths.mjs';
import {
  D6_FAMILIES,
  EXTRA_FAMILIES,
  FAMILY_STUB_APPROVED,
  GAP_FILE,
  OVERRIDES_FILE,
  readOverrides,
  readRegistrations,
  validateOverrides,
} from '../lift.mjs';

const read = (rel) => fs.readFileSync(path.join(REPO_ROOT, rel), 'utf8');

test('overrides.json 的 stub 登记与 C++ 注册表双向一致（reason 逐字）', () => {
  const { data, error } = readOverrides();
  assert.equal(error, null, error ?? '');
  const schemaProblems = validateOverrides(data);
  assert.deepEqual(schemaProblems, [], `schema 不变量被破坏：\n  - ${schemaProblems.join('\n  - ')}`);

  const regs = readRegistrations({ skipGapFile: false });
  assert.deepEqual(regs.problems, [], `C++ 注册表自身有问题：\n  - ${regs.problems.join('\n  - ')}`);

  const entries = new Map(data.entries.filter((e) => e.kind === 'stub' && e.target).map((e) => [e.target, e]));
  const missing = [];
  const mismatched = [];
  for (const [key, s] of regs.stubs) {
    const e = entries.get(key);
    if (!e) missing.push(key);
    else if (e.reason !== s.reason) mismatched.push(`${key}\n    C++  : ${s.reason}\n    JSON : ${e.reason}`);
  }
  const extra = [...entries.keys()].filter((k) => !regs.stubs.has(k) && !regs.implemented.has(k));

  assert.deepEqual(missing, [], `C++ 有登记但 overrides.json 没有：${missing.join(', ')}（跑 pnpm tools lift sync --write）`);
  assert.deepEqual(mismatched, [], `reason 不一致：\n  - ${mismatched.join('\n  - ')}`);
  assert.deepEqual(extra, [], `overrides.json 有、C++ 里找不到的 stub：${extra.join(', ')}`);
});

test('派生缺口文件接进了注册链（声明 + 调用），且文件头标明"不要手改"', () => {
  const header = read('apps/emulator-lift/src/shims/register.h');
  const registry = read('apps/emulator-lift/src/shims/register.cpp');
  assert.ok(header.includes('void registerImportGaps(ShimRegistry& shims);'), 'register.h 缺 registerImportGaps 声明');
  assert.ok(registry.includes('registerImportGaps(shims);'), 'register.cpp 没调用 registerImportGaps —— 缺口文件是死数据');
  const gap = read(GAP_FILE);
  assert.ok(gap.includes('不要手改'), `${GAP_FILE} 的头部必须明说"派生文件 / 不要手改"`);
  assert.ok(/void registerImportGaps\(ShimRegistry& shims\) \{/.test(gap), `${GAP_FILE} 缺 registerImportGaps 定义`);
  assert.ok(dataHasDocPointer(), `${OVERRIDES_FILE} 的 _doc 必须指向 pnpm tools lift describe`);
});

function dataHasDocPointer() {
  const data = JSON.parse(read(OVERRIDES_FILE));
  return typeof data._doc === 'string' && data._doc.includes('lift describe');
}

test('覆盖清单说明书存在且含「怎么查 / 怎么改」（json-docs 守卫的同一判据，这里提前红）', () => {
  const docRel = OVERRIDES_FILE.replace(/\.json$/, '.md');
  assert.ok(fs.existsSync(path.join(REPO_ROOT, docRel)), `缺同名说明书 ${docRel}`);
  const doc = read(docRel);
  for (const need of ['## 怎么查', '## 怎么改', 'describe']) {
    assert.ok(doc.includes(need), `${docRel} 缺「${need}」`);
  }
});

// ---------------------------------------------------------------------------------------
// D6 七族（A5）的判据数据本身：族名集合必须是**划分**，否则 stub 的分类不可预期。
// ★ 这是 pure 守卫（不读 PE）：判据数据的形状错了 ⇒ `pnpm tools lift check` 的分组必然错。
// ---------------------------------------------------------------------------------------
const KEY_RE = /^[a-z0-9_]+![A-Za-z0-9_@#]+$/;

test('D6 七族 / 七族外族的名字集合是一个划分（同名不许落在两族），且白名单只含 module!name', () => {
  assert.equal(D6_FAMILIES.length, 7, `D6_FAMILIES 必须有 7 族（现在是 ${D6_FAMILIES.length}）`);
  assert.equal(EXTRA_FAMILIES.length, 3, `EXTRA_FAMILIES 必须有 3 族（现在是 ${EXTRA_FAMILIES.length}）`);

  const families = [...D6_FAMILIES, ...EXTRA_FAMILIES];
  const owner = new Map();
  const dups = [];
  const badKeys = [];
  const emptyFamilies = [];
  for (const f of families) {
    if (!Array.isArray(f.names) || f.names.length === 0) emptyFamilies.push(`${f.id}（${f.title}）`);
    for (const key of f.names ?? []) {
      if (typeof key !== 'string' || !KEY_RE.test(key)) {
        badKeys.push(`${f.id}: ${JSON.stringify(key)}`);
        continue;
      }
      if (owner.has(key)) dups.push(`${key}（${owner.get(key)} / ${f.id}）`);
      else owner.set(key, f.id);
    }
  }
  assert.deepEqual(emptyFamilies, [], `这些族的 names 是空的：${emptyFamilies.join(', ')}`);
  assert.deepEqual(badKeys, [], `这些键不是 module!name（模块名小写）：${badKeys.join(', ')}`);
  assert.deepEqual(dups, [], `这些键同时落在两个族里（分类不可预期）：${dups.join(', ')}`);

  assert.ok(Array.isArray(FAMILY_STUB_APPROVED), 'FAMILY_STUB_APPROVED 必须是数组');
  const badApproved = FAMILY_STUB_APPROVED.filter((k) => typeof k !== 'string' || !KEY_RE.test(k));
  assert.deepEqual(badApproved, [], `FAMILY_STUB_APPROVED 里这些不是 module!name 字符串：${badApproved.map((k) => JSON.stringify(k)).join(', ')}`);
});
