/** @env pure @kind gate @why 分层被破坏、或测试分级声明缺失 */
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
      // ★ 按**同目录**解析判：`./x.mjs` 才算"另一个 CLI"。
      //   （只比 basename 会把 `./lib/<与某 CLI 同名>.mjs` 误判成互相 import —— 领域模型与 CLI 同名是合法的。）
      if (!/^\.\/[^/]+\.mjs$/.test(s)) continue;
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

/**
 * ★ **测试分级声明必须齐全**（旧仓 `organization.test.ts` 的 R1 搬过来，判据换成本仓的 pragma）。
 *
 * 为什么放在**分层守卫**里：它俩守的是同一类东西 —— "结构约定不许靠记得"。
 * 为什么要有它：没有声明 ⇒ 文件**静默落进默认档**或干脆不被选到，于是
 * "我这次跑的是全量还是半量"没人答得出（这正是本轮体检暴露的那个问题）。
 */
test('★ 测试分级：每个 *.test.mjs 首行必须有合法 pragma（@env / @kind / @why）', async () => {
  const { survey } = await import('../test-run.mjs');
  const { decls, problems } = survey(REPO_ROOT);
  assert.ok(decls.length > 0, '至少要扫到一个测试文件');
  assert.deepEqual(problems, [], `分级声明不齐（档位不许靠记得）：\n  - ${problems.join('\n  - ')}`);
  // 三个档都要有人（否则"分级"是空的）
  const envs = new Set(decls.map((d) => d.env));
  for (const e of ['pure', 'assets', 'external']) assert.ok(envs.has(e), `没有任何文件声明 @env ${e}（分级形同虚设）`);
});

/**
 * ★ **不许用"静默 pass"冒充跳过**（旧仓 `organization.test.ts` 的 R3）。
 *
 * 实测（本仓）：`t.diagnostic(...)` 后面**裸 `return`** —— node:test 把它记成 **pass**；
 * 连 `t.diagnostic` 都没有的裸 `return`（`opcodes.test.mjs` 原来那处）同样记成 pass。
 * ⇒ "这台机器上没跑"与"跑了且绿"在门禁输出里长得**一模一样**。
 * 正确写法只有 `t.skip(...)`（记成 skipped）或干脆去掉那条用例。
 *
 * 判据落在源码上（node:test 的报告里区分不出这两种写法）。范围只到"测试体内层"（缩进 ≤ 4 空格），
 * 免得把 `.map(() => { return; })` 这类回调里的 return 误判成跳过。
 */
test('★ 测试卫生：不许用 `t.diagnostic` + 裸 `return` 冒充跳过（要被记成 pass）', () => {
  const scan = (dir, label) => {
    const bad = [];
    for (const f of fs.readdirSync(dir)) {
      if (!f.endsWith('.test.mjs') || f === 'layering.test.mjs') continue;
      const src = fs.readFileSync(path.join(dir, f), 'utf8');
      const lines = src.split('\n');
      for (let i = 0; i < lines.length; i += 1) {
        if (!/^\s{0,4}return;\s*$/.test(lines[i])) continue;
        // ★ **同一条用例里**既有 `t.diagnostic(` 又**没有** `t.skip(` ⇒ 那条 return 是"静默 pass"的源：
        //   看它往前最近的 `test(` 到后面 24 行这段窗口。
        const from = Math.max(0, i - 24);
        const win = lines.slice(from, i + 24).join('\n');
        if (/t\.diagnostic\(/.test(win) && !/t\.skip\(/.test(win)) {
          bad.push(`${label}${f}:${i + 1}：窗口里既有 t.diagnostic 又无 t.skip ⇒ 这条用例会被记成 pass（改用 t.skip）`);
        }
      }
    }
    return bad;
  };
  const bad = [...scan(path.join(REPO_ROOT, 'tools', 'test'), 'tools/test/')];
  for (const d of ['packages', 'plugins']) {
    const base = path.join(REPO_ROOT, d);
    if (!fs.existsSync(base)) continue;
    for (const pkg of fs.readdirSync(base)) {
      const dir = path.join(base, pkg, 'test');
      if (fs.existsSync(dir)) bad.push(...scan(dir, `${d}/${pkg}/test/`));
    }
  }
  assert.deepEqual(bad, [], `静默 pass 冒充跳过：\n  - ${bad.join('\n  - ')}`);
});

