/**
 * tools/test/layering.test.mjs — **分层不会被破坏**的守卫
 *
 * 用户口径："为了方便在交叉 import/export；纯粹的工具（例如驱动 git）应该作为独立工具 mjs 提供，
 * 而非从业务 mjs 中导出。" ⇒ 结构钉成三层，且**依赖方向单向**：
 *
 *   lib/（纯工具 + 领域模型）  ←  CLI（tools/*.mjs，只做"参数 → 模型 → 输出"）  ←  cli.mjs（派发器）
 *
 * 断言：
 *   ① `lib/**` **不得** import `tools/*.mjs`（模型不许依赖 CLI）；
 *   ② CLI **不得** import 另一个 CLI（要模型就 import `lib/`）——只有派发器 `cli.mjs` 才认识各 CLI；
 *   ③ **纯工具**（paths/fsx/exec/zip/time/cp932）**不得** import 领域模型（manifest/samples）——
 *      反向也不行（模型可以依赖纯工具，纯工具不认识领域概念）。
 *
 * 运行：`pnpm test`
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';

import { REPO_ROOT } from '../lib/paths.mjs';

const TOOLS = path.join(REPO_ROOT, 'tools');
const LIB = path.join(TOOLS, 'lib');

/** 纯工具（不认识任何领域数据） */
const PURE = new Set(['paths', 'fsx', 'exec', 'zip', 'time', 'cp932']);
/** 领域模型（schema / 不变量 / 读 / 写） */
const MODEL = new Set(['manifest', 'samples']);

const importSpecifiers = (file) => {
  const src = fs.readFileSync(file, 'utf8');
  return [...src.matchAll(/from\s+'(\.[^']+)'/g)].map((m) => m[1]);
};

const toolFiles = () =>
  fs
    .readdirSync(TOOLS, { withFileTypes: true })
    .filter((e) => e.isFile() && e.name.endsWith('.mjs'))
    .map((e) => e.name);
const libFiles = () => fs.readdirSync(LIB).filter((f) => f.endsWith('.mjs'));

test('① lib/** 不得 import tools/*.mjs（模型不许依赖 CLI）', () => {
  const bad = [];
  for (const f of libFiles()) {
    for (const s of importSpecifiers(path.join(LIB, f))) {
      if (/^\.\.\/[a-z-]+\.mjs$/.test(s)) bad.push(`lib/${f} → ${s}`);
    }
  }
  assert.deepEqual(bad, [], `模型依赖了 CLI：\n  - ${bad.join('\n  - ')}`);
});

test('② CLI 之间不得互相 import（只有派发器 cli.mjs 认识各 CLI）', () => {
  const clis = toolFiles().filter((f) => f !== 'cli.mjs');
  const bad = [];
  for (const f of clis) {
    for (const s of importSpecifiers(path.join(TOOLS, f))) {
      const base = path.basename(s);
      if (clis.includes(base)) bad.push(`${f} → ${s}`);
    }
  }
  assert.deepEqual(bad, [], `CLI 互相 import：\n  - ${bad.join('\n  - ')}`);
});

test('③ 纯工具不得 import 领域模型；模型可以 import 纯工具', () => {
  const bad = [];
  for (const f of libFiles()) {
    const name = path.basename(f, '.mjs');
    if (!PURE.has(name)) continue;
    for (const s of importSpecifiers(path.join(LIB, f))) {
      const dep = path.basename(s, '.mjs');
      if (MODEL.has(dep)) bad.push(`lib/${f}（纯工具）→ ${s}（领域模型）`);
    }
  }
  assert.deepEqual(bad, [], `纯工具依赖了领域模型：\n  - ${bad.join('\n  - ')}`);
});

test('分层齐全：纯工具与领域模型都在 lib/，且每个 CLI 只用 lib/', () => {
  const libs = libFiles().map((f) => path.basename(f, '.mjs'));
  for (const need of [...PURE, ...MODEL]) assert.ok(libs.includes(need), `lib/ 里缺 ${need}.mjs`);
  for (const f of toolFiles()) {
    if (f === 'cli.mjs') continue; // 派发器动态 import 各 CLI，不用相对 from
    for (const s of importSpecifiers(path.join(TOOLS, f))) {
      assert.match(s, /^\.\/lib\//, `${f} 只应 import ./lib/*，实际含 ${s}`);
    }
  }
});
