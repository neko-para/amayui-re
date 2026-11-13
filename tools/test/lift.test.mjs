/** @env pure @kind gate @suite toolchain @why 覆盖清单与 C++ 注册表漂了（kind 错 / 两处不一致 / 派生文件没接上 / 手改 overrides.json） */
/**
 * tools/test/lift.test.mjs — 覆盖清单的**纯档**守卫（不需要 LFS 资产）
 *
 * ① `data/lift/overrides.json` **逐字节**等于 `renderOverrides(expectedEntries(...))`
 *    （手改一个字符 ⇒ 红；唯一写入口 = `pnpm tools lift sync --write`）。
 *    ★ 这里用「C++ 注册表 + 派生缺口文件」重算，**不读 PE** ⇒ 纯档可跑；
 *      需要 PE 导入表的那一半（裸缺口 = 0、AGERC 的 bareUnexecuted、not-referenced）在
 *      `pnpm tools lift check` 与下面 ③④（corpus 二进制在时跑，缺了就 `t.skip`）。
 * ② `kind` 三值：`addStub` ⇒ `noop`、`addUnimplemented` ⇒ `trap`、`add` ⇒ `replace`；旧值 `stub` 不存在。
 * ③ `executed=false` 的模块（AGERC.DLL）的**裸导入**⛔ 不进 overrides / 派生缺口文件，进 `bareUnexecuted`。
 * ④ `not-referenced` = 不在**任一**模块导入表 ∧ 不是被拦截的 AGERC 导出（`agerc!_*`）。
 * ⑤ `reason` 读取：相邻字符串字面量按序拼接（C++ 常规语义）。
 * ⑥ `registerImportGaps` 真的接进了注册链（声明 + 调用），否则派生缺口文件是"写了没人用"。
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';

import { REPO_ROOT } from '../lib/paths.mjs';
import {
  AGERC_FILE,
  D6_FAMILIES,
  EXTRA_FAMILIES,
  FAMILY_STUB_APPROVED,
  GAP_FILE,
  GUEST_MODULES,
  KINDS,
  OVERRIDES_FILE,
  classifyImports,
  expectedEntries,
  expectedGapKeys,
  kindForOrigin,
  notReferencedKeys,
  readGuestImports,
  readOverrides,
  readRegistrations,
  reasonOf,
  renderGapFile,
  renderOverrides,
  reportCoverage,
} from '../lift.mjs';

const read = (rel) => fs.readFileSync(path.join(REPO_ROOT, rel), 'utf8');

/** 读一份覆盖清单 + 注册表（断言自身没坏），`existing` 供 expectedEntries 保留手写项。 */
function loadPure() {
  const { data, error } = readOverrides();
  assert.equal(error, null, error ?? '');
  const regs = readRegistrations({ skipGapFile: false }); // 含派生缺口文件 ⇒ 不需要 PE
  assert.deepEqual(regs.problems, [], `C++ 注册表自身有问题：\n  - ${regs.problems.join('\n  - ')}`);
  return { data, regs };
}

/**
 * 读整份 guest 模块集。corpus 二进制（LFS）不在时 `t.skip` ——
 * ⛔ 不用 `t.diagnostic(...) + return`：那会被 node:test 记成 **pass**（见 AGENTS.md 测试卫生）。
 */
function loadGuest(t) {
  for (const m of GUEST_MODULES) {
    const p = path.join(REPO_ROOT, m.path);
    if (!fs.existsSync(p)) {
      t.skip(`缺 corpus 二进制 ${m.path}（LFS 未取）⇒ 跳过需要 PE 导入表的断言`);
      return null;
    }
  }
  try {
    return readGuestImports();
  } catch (err) {
    t.skip(`corpus 二进制不是可读 PE（${err.message}）⇒ 跳过需要 PE 导入表的断言`);
    return null;
  }
}

const executedImportsOf = (guest) => {
  const out = new Map();
  for (const m of guest.modules) {
    if (!m.executed) continue;
    for (const [k, imp] of m.pe.imports) out.set(k, imp);
  }
  return out;
};

test('① overrides.json 逐字节等于 lift sync 的派生结果（手改一个字符就红）', () => {
  const { data, regs } = loadPure();
  const schemaProblems = [];
  for (const [i, e] of (data.entries ?? []).entries()) {
    if (!KINDS.includes(e.kind)) schemaProblems.push(`entries[${i}].kind=${JSON.stringify(e.kind)}`);
  }
  assert.deepEqual(schemaProblems, [], `kind 不在 ${KINDS.join('|')} 里：${schemaProblems.join(', ')}`);

  // ★ 不读 PE：派生缺口那几条从 `register_import_gaps.cpp` 读回（`skipGapFile:false`），
  //   与 `sync` 从 PE 派生的结果逐字节相同（`check` 另有一条断言钉住"缺口文件 ↔ PE"）。
  const expected = renderOverrides(expectedEntries({ imports: new Map(), regs, existing: data }));
  assert.equal(
    read(OVERRIDES_FILE),
    expected,
    `${OVERRIDES_FILE} 与 \`pnpm tools lift sync\` 的派生结果不是逐字节相同 —— ` +
      '唯一写入口是 `sync --write`，⛔ 不要手改（哪怕只改一个缩进）',
  );
});

test('② kind 三值：addStub ⇒ noop、addUnimplemented ⇒ trap、add ⇒ replace（旧 stub 值已消失）', () => {
  assert.deepEqual(KINDS, ['replace', 'noop', 'trap'], 'kind 必须是三值（用户 2026-11-13 裁决）');
  assert.equal(kindForOrigin('add'), 'replace');
  assert.equal(kindForOrigin('addStub'), 'noop');
  assert.equal(kindForOrigin('addUnimplemented'), 'trap');
  assert.equal(kindForOrigin('gap'), 'trap');

  const { data, regs } = loadPure();
  const byTarget = new Map((data.entries ?? []).filter((e) => e.target).map((e) => [e.target, e]));
  const bad = [];
  for (const [key, s] of regs.stubs) {
    const e = byTarget.get(key);
    if (!e) bad.push(`${key}：overrides.json 缺条目`);
    else if (e.kind !== kindForOrigin(s.origin)) bad.push(`${key}：C++=${s.origin} 应 ${kindForOrigin(s.origin)}，JSON=${e.kind}`);
  }
  assert.deepEqual(bad, [], `kind 与 C++ 注册来源不一致：\n  - ${bad.join('\n  - ')}`);

  const legacy = (data.entries ?? []).filter((e) => e.kind === 'stub').map((e) => e.target);
  assert.deepEqual(legacy, [], `旧值 kind:"stub" 不许保留（sync --write 会重写）：${legacy.join(', ')}`);

  // 两值都真的出现过（否则上面的循环可能是空转：比如注册表全变成 addStub）
  const kinds = new Set((data.entries ?? []).map((e) => e.kind));
  assert.ok(kinds.has('noop'), '清单里没有任何 noop 条目 —— kind 映射可能整条失效');
  assert.ok(kinds.has('trap'), '清单里没有任何 trap 条目 —— kind 映射可能整条失效');
});

test('③ AGERC（executed=false）的裸导入⛔ 不进 overrides / 派生缺口文件，进 bareUnexecuted', (t) => {
  // 模块集是**声明**的：AGERC 必须存在且 executed=false（⛔ 不是 install/AGERC.DLL）
  assert.deepEqual(
    GUEST_MODULES.map((m) => ({ path: m.path, executed: m.executed })),
    [
      { path: 'corpus/assets/age/AGE.EXE__dumped.sectfix.EXE', executed: true },
      { path: AGERC_FILE, executed: false },
    ],
  );
  assert.equal(AGERC_FILE, 'corpus/assets/agerc/AGERC.DLL');

  const guest = loadGuest(t);
  if (!guest) return;
  const { data, regs } = loadPure();
  const executed = executedImportsOf(guest);
  const gaps = expectedGapKeys(executed, regs);
  // 盘上的派生缺口文件与期望一致时，缺口才算"已登记"（与 check/report 同一口径）
  const gapFileKeys = new Set();
  if (read(GAP_FILE) === renderGapFile(gaps)) {
    for (const g of gaps) gapFileKeys.add(g.key);
  }

  const unexecuted = guest.modules.filter((m) => !m.executed);
  assert.equal(unexecuted.length, 1, '恰好一个未执行模块（AGERC.DLL）');
  const agerc = unexecuted[0];
  const cls = classifyImports(agerc.pe.imports, regs, gapFileKeys);

  const fileTargets = new Set((data.entries ?? []).map((e) => e.target));
  const leaked = cls.bare.filter((k) => fileTargets.has(k));
  assert.deepEqual(leaked, [], `AGERC 的裸导入被登记进 overrides.json（应在 bareUnexecuted 单列）：${leaked.join(', ')}`);

  const gapOnly = [...regs.stubs.keys()].filter((k) => !readRegistrations({ skipGapFile: true }).stubs.has(k));
  const leakedGap = cls.bare.filter((k) => gapOnly.includes(k));
  assert.deepEqual(leakedGap, [], `AGERC 的裸导入被写进派生缺口文件：${leakedGap.join(', ')}`);

  const r = reportCoverage();
  assert.equal(r.counts.bareUnexecuted, cls.bare.length, 'report 的 bareUnexecuted 必须等于未执行模块的裸导入数');
  assert.equal(r.counts.bare, 0, 'executed=true 的裸缺口必须是 0');
  const reportBare = new Set(r.unexecutedRows.map((x) => x.target));
  for (const k of cls.bare) assert.ok(reportBare.has(k), `report 的 bareUnexecuted 节缺 ${k}`);
});

test('④ not-referenced = 不在任一模块导入表 ∧ 不是被拦截的 AGERC 导出（agerc!_*）', (t) => {
  const guest = loadGuest(t);
  if (!guest) return;
  const { regs } = loadPure();
  const nr = notReferencedKeys(regs, guest.imports);

  for (const k of nr) {
    assert.ok(!guest.imports.has(k), `${k} 被某个 guest 模块导入了，不该是 not-referenced`);
    assert.ok(!k.startsWith('agerc!'), `被拦截的 AGERC 导出 ${k} 经 GetProcAddress 可达，不该是 not-referenced`);
  }
  // 三个 `agerc!_*` 都是被拦截导出（GetInstance / OperateMenu / SetNameLenMax）⇒ 一个都不在清单里
  const agercStubs = [...regs.stubs.keys()].filter((k) => k.startsWith('agerc!'));
  assert.ok(agercStubs.length > 0, 'AGERC 的拦截导出登记不见了');
  for (const k of agercStubs) assert.ok(!nr.includes(k), `${k} 是拦截导出，不该进 not-referenced`);

  // 每个 C++ gap 登记必须落进三类之一：被某模块导入 / 被拦截导出 / not-referenced（不重不漏）
  const buckets = new Set([...nr, ...agercStubs, ...[...regs.stubs.keys()].filter((k) => guest.imports.has(k))]);
  const lost = [...regs.stubs.keys()].filter((k) => !buckets.has(k));
  assert.deepEqual(lost, [], `这些 gap 登记既没被引用、也不在 not-referenced：${lost.join(', ')}`);

  // 只被 AGERC 导入的那条（RegCloseKey）**不是** not-referenced（审计的 never-imported 错觉）
  if (guest.imports.has('advapi32!RegCloseKey') && !executedImportsOf(guest).has('advapi32!RegCloseKey')) {
    assert.ok(!nr.includes('advapi32!RegCloseKey'), 'RegCloseKey 被 AGERC 导入 ⇒ 不是 not-referenced');
  }
});

test('⑤ reason 读取：相邻字符串字面量按序拼接（C++ 常规语义；⛔ 不把引号/缩进当内容）', () => {
  assert.equal(reasonOf('"a" "b"'), 'ab');
  assert.equal(reasonOf('"a"\n              "b"'), 'ab', '换行 + 缩进必须被跳过（kernel32_agerc.cpp 那处）');
  assert.equal(reasonOf('"a" "b" "c"'), 'abc');
  assert.equal(reasonOf('"x" + suffix'), null, '不是纯字面量序列 ⇒ 解不出来（⛔ 不猜）');
  assert.equal(reasonOf('UNKNOWN_CONST'), null);

  const regs = readRegistrations({ skipGapFile: false });
  const s = regs.stubs.get('agerc!_SetNameLenMax@20');
  assert.ok(s, '缺 agerc!_SetNameLenMax@20 的登记');
  assert.ok(!s.reason.includes('"\n'), `reason 还落成了相邻字面量的原文：${JSON.stringify(s.reason)}`);
  assert.ok(!s.reason.includes('\n'), `reason 里有换行（缩进被当内容）：${JSON.stringify(s.reason)}`);
  assert.ok(s.reason.includes('进 g_nameLenMax'), `reason 拼接结果不对：${JSON.stringify(s.reason)}`);
});

test('⑥ 派生缺口文件接进了注册链（声明 + 调用），且文件头标明"不要手改"', () => {
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
// D6 七族（A5）的判据数据本身：族名集合必须是**划分**，否则 gap 的分类不可预期。
// ★ 这是 pure 守卫（不读 PE）：判据数据的形状错了 ⇒ `pnpm tools lift check` 的分组必然错。
// ---------------------------------------------------------------------------------------
const KEY_RE = /^[a-z0-9_]+![A-Za-z0-9_@#]+$/;

test('D6 七族 / 七族外族的名字集合是一个划分（同名不许落在两族），且白名单只含 module!name', () => {
  assert.equal(D6_FAMILIES.length, 7, `D6_FAMILIES 必须有 7 族（现在是 ${D6_FAMILIES.length}）`);
  assert.equal(EXTRA_FAMILIES.length, 2, `EXTRA_FAMILIES 必须有 2 族（现在是 ${EXTRA_FAMILIES.length}）`);

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
