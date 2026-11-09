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

/**
 * ★ **元守卫：两档之和 = 全量**（`AGENTS.md` §5 的判据；上一轮只是 `.tmp/` 里的手跑脚本，**不是门禁**）。
 *
 * 为什么它必须进合同档：`@suite` 写坏（非法值 / pragma 形态坏掉）时，该文件会**同时掉出两档** ——
 * `pnpm test`（contract）与 `pnpm verify`（toolchain）都不选它，而两条命令都绿
 * ⇒ "这个守卫在跑"只剩声称。这正是把这条判据从脚本升级成门禁的理由。
 *
 * 四条断言：
 *   ① 每个 `*.test.mjs` 的 pragma 都**解析得出** `@env` / `@suite`（解析不出来 ⇒ 两者都是 undefined ⇒ 两档都不进）；
 *   ② `@suite` 只能 ∈ {contract, toolchain}（省略 = contract，`readPragma` 已归一）；
 *   ③ ★ 核心：**两条门禁命令实际选中的并集 = 全部 `@env pure` 文件**（`pnpm test` ∪ `pnpm verify`，不重不漏）。
 *      这里不另写选择逻辑：读 `package.json` 里 `test` / `verify` 两条脚本的 `--env` / `--suite`，
 *      缺省档取 `test-run.mjs` 的 `DEFAULT_ENVS` / `SUITES`，再用**同一份** `decls` 求并集
 *      ⇒ 脚本接线改了、或默认档改了，都会在这里红；
 *   ④ 反向：两档是**全部** `*.test.mjs` 的划分（交集空、并集全），且档名与 `@env` 名**不许写串位置**（两者正交）。
 *
 * ★ 它自己没有 `@suite` ⇒ 落在**合同档**（`pnpm test` 会跑到它）。
 * ★ 口径边界（别把这条断言读宽）：`@env pure` **之外**的文件**也**带 `@suite`（今天实测：assets 45 contract + 17 toolchain、
 *   external 3 + 3）⇒ 只要求"分档对**全量**文件是一个划分"，**不**要求"只有 pure 才有档位"。
 *   把某个 pure 文件改成 `@env assets` 是**合法**的重分类（它离开默认门禁这件事不是静默的 —— 声明就写在首行），
 *   所以那种改动**不该**红；会红的只有"声明解析不出来 / 档位值超出闭集 / 值写串了位置"。
 */
test('★ 元守卫：两档之和 = 全量（@env pure = contract ∪ toolchain，不重不漏；分档与 @env 正交）', async () => {
  const { survey, ENVS, SUITES, DEFAULT_ENVS } = await import('../test-run.mjs');
  const { decls } = survey(REPO_ROOT);
  assert.ok(decls.length > 0, '至少要扫到一个测试文件');

  // ① pragma 必须解析得出 env / suite（缺 pragma / 形态非法 ⇒ 两者都是 undefined）
  const unparsed = decls.filter((d) => typeof d.env !== 'string' || typeof d.suite !== 'string').map((d) => d.rel);
  assert.deepEqual(unparsed, [], `这些文件的 pragma 解析不出 @env/@suite ⇒ 两档都不进（没有任何门禁会执行它）：\n  - ${unparsed.join('\n  - ')}`);

  // ② @suite 只能是闭集里的值（省略 = contract；非法值同样会让该文件两档都不进）
  const badSuite = decls.filter((d) => !SUITES.includes(d.suite)).map((d) => `${d.rel}: @suite=${JSON.stringify(d.suite)}`);
  assert.deepEqual(badSuite, [], `@suite 非法（合法值只有 ${SUITES.join(' / ')}；省略 = contract）：\n  - ${badSuite.join('\n  - ')}`);

  // ③ ★ 核心：`pnpm test` ∪ `pnpm verify` = 全部 @env pure 文件，**不重不漏**
  const pure = decls.filter((d) => d.env === 'pure');
  const pureRels = pure.map((d) => d.rel);
  const pkg = JSON.parse(fs.readFileSync(path.join(REPO_ROOT, 'package.json'), 'utf8'));
  const gatePick = (script) => {
    const cmd = pkg.scripts?.[script] ?? '';
    const envs = (/--env\s+([A-Za-z,]+)/.exec(cmd)?.[1] ?? DEFAULT_ENVS.join(',')).split(',');
    const suites = (/--suite\s+([A-Za-z,]+)/.exec(cmd)?.[1] ?? SUITES.join(',')).split(',');
    return decls.filter((d) => envs.includes(d.env) && suites.includes(d.suite)).map((d) => d.rel);
  };
  const isTest = gatePick('test');
  const isVerify = gatePick('verify');
  const inBothGates = isTest.filter((r) => isVerify.includes(r));
  assert.deepEqual(inBothGates, [], `同一个文件被 \`pnpm test\` 与 \`pnpm verify\` 同时选中（"两档之和"会重复计数）：\n  - ${inBothGates.join('\n  - ')}`);
  const gateUnion = new Set([...isTest, ...isVerify]);
  const missed = pureRels.filter((r) => !gateUnion.has(r));
  assert.deepEqual(missed, [], `这些 @env pure 文件 \`pnpm test\` 与 \`pnpm verify\` **都不选** ⇒ 默认门禁不会执行它，而两条命令都绿：\n  - ${missed.join('\n  - ')}`);
  const stray = [...gateUnion].filter((r) => !pureRels.includes(r));
  assert.deepEqual(stray, [], `两档选进了非 @env pure 的文件（默认档被改了？）：\n  - ${stray.join('\n  - ')}`);

  // ④ 反向：两档是**全量**文件的划分；档名与 @env 名不许写串位置（两者正交，只是叠在同一个 pragma 上）
  const allContract = decls.filter((d) => d.suite === 'contract').map((d) => d.rel);
  const allToolchain = decls.filter((d) => d.suite === 'toolchain').map((d) => d.rel);
  const inBoth = allContract.filter((r) => allToolchain.includes(r));
  assert.deepEqual(inBoth, [], `同一文件同时属于合同档与工具链档：\n  - ${inBoth.join('\n  - ')}`);
  const union = new Set([...allContract, ...allToolchain]);
  const uncovered = decls.map((d) => d.rel).filter((r) => !union.has(r));
  assert.deepEqual(uncovered, [], `分档没有覆盖这些 *.test.mjs（两档都不进 ⇒ 不会被任何门禁执行）：\n  - ${uncovered.join('\n  - ')}`);
  const swapped = [];
  for (const d of decls) {
    if (ENVS.includes(d.suite)) swapped.push(`${d.rel}: @suite 写成了 @env 的值 ${JSON.stringify(d.suite)}`);
    if (!ENVS.includes(d.env)) swapped.push(`${d.rel}: @env 非法 ${JSON.stringify(d.env)}（档名与 @env 名不许互换）`);
  }
  assert.deepEqual(swapped, [], `@env 与 @suite 的值写串了位置 / 超出闭集（两者正交，不许互换）：\n  - ${swapped.join('\n  - ')}`);
});

