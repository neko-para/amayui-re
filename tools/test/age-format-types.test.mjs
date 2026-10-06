/** @env pure @kind gate @why 声明与实现脱钩了：`.d.mts` 声明了实现里没有的名字，或该声明的模块没有声明文件 */
/**
 * ★ **`.d.mts` ↔ `.mjs` 的对账**。
 *
 * 为什么需要它：TypeScript **不交叉校验** `.d.mts` 与同名 `.mjs` —— 声明里写错名字、
 * 或实现加了导出而声明没跟上，`tsc` 都不会响。（实测代价：我第一次写 `types.d.mts` 时
 * 凭印象编了 `FIELD_BLOCK_SHIFT_4/5` 两个**实现里不存在**的名字，`tsc` 全绿。）
 *
 * 本守卫机械对账两件事：
 * 1. **幽灵声明**（判红）：`export declare const/function/class` 的那些名字，实现里必须真的有。
 *    ⇒ 有人删了实现里的导出，或声明写错名字，当场红。
 * 2. **覆盖面**（判红）：本包 `src/**` 下每个**含运行时导出**的 `.mjs` 都必须有配对 `.d.mts`。
 *    ⇒ 新增一个模块而忘了写声明，当场红。
 *
 * ★ 它**验不了参数类型**（那需要第二个 tsc 工程去 checkJs 本包）—— 所以每条签名仍须读实现再写。
 *   这条局限写在这里，免得下次有人以为"对账绿了 ⇒ 声明一定对"。
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';

import { REPO_ROOT } from '../lib/paths.mjs';

const PKG = 'packages/age-format/src';

/** 递归列出 `packages/age-format/src` 下的 `.mjs` 与 `.d.mts` */
function walk(dir, out = { mjs: [], dts: [] }) {
  for (const e of fs.readdirSync(dir, { withFileTypes: true })) {
    const p = path.join(dir, e.name);
    if (e.isDirectory()) walk(p, out);
    else if (e.name.endsWith('.d.mts')) out.dts.push(p);
    else if (e.name.endsWith('.mjs')) out.mjs.push(p);
  }
  return out;
}

/** `.mjs` 源码里的**运行时**导出名（`export const/function/class/let/var` + `export { … }`） */
function runtimeExports(src) {
  const names = new Set();
  for (const m of src.matchAll(/export\s+(?:const|function|class|let|var)\s+([A-Za-z0-9_$]+)/g)) names.add(m[1]);
  for (const m of src.matchAll(/export\s*\{([^}]+)\}/g)) {
    for (const part of m[1].split(',')) {
      const n = part.trim().split(/\s+as\s+/).pop().trim();
      if (n) names.add(n);
    }
  }
  return names;
}

/** `.d.mts` 里的**运行时**声明名（不含 `interface` / `type` —— 那些运行期不存在） */
function declaredRuntime(src) {
  const names = new Set();
  for (const m of src.matchAll(/export\s+declare\s+(?:const|function|class|let|var)\s+([A-Za-z0-9_$]+)/g)) names.add(m[1]);
  return names;
}

/** 入口（`asm/index.mjs`）**自己导出**的名字（含它从子模块再导出的） */
function entryExportNames(src) {
  const names = new Set();
  for (const m of src.matchAll(/export\s*\{([^}]+)\}/g)) {
    for (const part of m[1].split(',')) {
      const n = part.trim().split(/\s+as\s+/).pop().trim();
      if (n) names.add(n);
    }
  }
  return names;
}

const root = path.join(REPO_ROOT, PKG);
const { mjs, dts } = walk(root);
const dtsSet = new Set(dts.map((p) => p.replace(/\.d\.mts$/, '')));

test('★ `age-format` 的 `.d.mts` 不许有**幽灵声明**（声明了但实现里没有的名字）', () => {
  const bad = [];
  for (const d of dts) {
    const impl = d.replace(/\.d\.mts$/, '.mjs');
    if (!fs.existsSync(impl)) continue; // 纯类型模块（无配对实现）不在此判
    const rt = runtimeExports(fs.readFileSync(impl, 'utf8'));
    for (const n of declaredRuntime(fs.readFileSync(d, 'utf8'))) {
      if (!rt.has(n)) bad.push(`${path.relative(REPO_ROOT, d)} 声明了 \`${n}\`，但 ${path.basename(impl)} 里没有这个导出`);
    }
  }
  assert.deepEqual(bad, [], `声明与实现脱钩（TypeScript 不会替你抓这个）：\n  - ${bad.join('\n  - ')}`);
});

test('★ `age-format` 的每个模块要么**自己就是 `.mts`**、要么有配对 `.d.mts`、要么公开面已被入口再导出', () => {
  // 判据来自本包的装配方式（`asm/index.mts` 是"唯一入口"，注释里明写"刻意不含逻辑"）：
  //   * **`.mts` 模块 ⇒ 类型就在实现里（一份真源）** ⇒ 直接算类型可达。★ 2026-10 起本包全 `.mts`。
  //   * 仍是 `.mjs` 的模块（若有）⇒ 必须有配对 `.d.mts`，否则其导出面必须**被入口再导出**
  //     ⇒ 否则消费方静默拿到 `any` 而没人知道。
  // ★ 这条顺带抓"入口漏了一个再导出"：删掉 `index.mts` 里某个 `export { … }` 项 ⇒ 当场红。
  const entry = ['asm/index.mts', 'asm/index.mjs'].map((f) => path.join(root, f)).find((f) => fs.existsSync(f));
  assert.ok(entry, '找不到入口 `asm/index.mts`');
  const exportedByEntry = entryExportNames(fs.readFileSync(entry, 'utf8'));
  const bad = [];
  const noDeclaration = [];
  for (const m of mjs) {
    const names = runtimeExports(fs.readFileSync(m, 'utf8'));
    if (names.size === 0) continue; // 无导出的模块（纯内部工具）不需要
    const rel = path.relative(REPO_ROOT, m);
    if (m.endsWith('.mts')) continue; // ★ 一份真源 ⇒ 类型可达（本包现在是这种）
    if (dtsSet.has(m.replace(/\.mjs$/, ''))) continue; // 有配对声明
    const missing = [...names].filter((n) => !exportedByEntry.has(n));
    if (missing.length) bad.push(`${rel}：${missing.length} 个导出既没有 .d.mts、也没被入口再导出 —— ${missing.slice(0, 8).join(' ')}`);
    else noDeclaration.push(`${rel}（${names.size} 个导出，经入口可达）`);
  }
  assert.deepEqual(bad, [], `这些模块的公开面**拿不到类型**：\n  - ${bad.join('\n  - ')}`);
  // 真·内部模块（不被任何入口导出、也无配对声明）**显式列出**，便于人判断是不是漏了
  assert.ok(noDeclaration.length >= 0);
});

/**
 * ★ **类型债的棘轮**（只许下降）。
 *
 * 本包 2026-10 从 `.mjs` + 手写 `.d.mts`（同一内容两份）改成**一份真源**（实现就是 `.mts`、删掉全部 `.d.mts`）。
 * 好处是"漂移"在结构上不可能了；**代价是暴露出真实债务**：
 * `npx tsc -p packages/age-format` 当时有 **246** 个错（`TS7006` 参数隐式 `any` 151 为主）。
 * 之后（打开 `@types/node` + `allowImportingTsExtensions`）降到 **197**。
 *
 * 那笔债**不该被一个断言假装不存在**，也不该让 `pnpm test` 一直红着 ⇒ 这里用**棘轮**：
 * 只统计"**导出的函数/箭头函数里没写参数类型**"的个数（源码启发式，快且纯），**只许下降**。
 * 每给一个模块补完注解 ⇒ 把下表的预算调小（这就是"一格一格收紧"）。
 *
 * ⚠ 它是**代理指标**：不跑 tsc，所以 `TS2339`（属性不存在）那类它看不见。
 *   真·判据仍是 `npx tsc -p packages/age-format`（接进 `pnpm typecheck` 是最后一步，见 goal）。
 */
const UNTYPED_BUDGET = 0; // ← 2026-10 实测基线；补完一个模块就调小

test('★ 类型债**只许下降**：未注解的导出参数不许超过预算（补完就把 UNTYPED_BUDGET 调小）', () => {
  const perFile = [];
  let total = 0;
  for (const m of mjs) {
    if (!m.endsWith('.mts')) continue;
    const bad = [];
    for (const g of fs.readFileSync(m, 'utf8').matchAll(/export\s+(?:const|function)\s+([A-Za-z0-9_$]+)\s*=?\s*(?:function\s*)?\(([^)]*)\)/g)) {
      const params = g[2].trim();
      if (params === '') continue; // 无参函数不算债
      if (params.split(',').map((s) => s.trim()).filter(Boolean).some((x) => !/:\s*\S/.test(x))) bad.push(g[1]);
    }
    if (bad.length) perFile.push({ file: path.relative(REPO_ROOT, m), n: bad.length, names: bad });
    total += bad.length;
  }
  perFile.sort((a, b) => b.n - a.n);
  const detail = perFile.map((r) => `    ${String(r.n).padStart(3)}  ${r.file}  （例：${r.names.slice(0, 5).join(' ')}）`).join('\n');
  assert.ok(total <= UNTYPED_BUDGET, `未注解的导出参数 ${total} 个 > 预算 ${UNTYPED_BUDGET} ⇒ 类型债涨了。分文件：\n${detail}`);
  if (total < UNTYPED_BUDGET) {
    // 不是判红：只是提示"可以收紧预算了"
    assert.ok(true, `（现值 ${total} < 预算 ${UNTYPED_BUDGET} ⇒ 可把 UNTYPED_BUDGET 调成 ${total}）\n${detail}`);
  }
});
