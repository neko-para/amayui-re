#!/usr/bin/env node
/** @env pure @kind gate @suite toolchain @why add_test 声明的 flag 在目标源文件里没有分派 ⇒ 用例静默跑默认套件、ctest 假绿 */
/**
 * tools/test/cmake-test-entry.test.mjs —— **`add_test` 的实参必须被目标认识**
 *
 * ## 要防的故障形态（本会话撞到过一次）
 * `add_test(NAME x COMMAND <bin> --mode-x)` 里的 `--mode-x` 在目标源文件里**没有 argv 分派分支**
 * ⇒ 二进制照常跑它的**默认套件**、退出码 0 ⇒ **`ctest` 绿，但那条用例什么新东西都没断言**。
 * 这是最难发现的失败模式：它比红危险，因为没有信号。
 *
 * `conformance-rejects-hollow-case` 守的是**夹具空心**（前后快照逐位相同），抓不到"flag 没被认识"；
 * 所以这里另立一条，判据落在**注册面 ↔ 源文件**上：
 *
 *   1. `add_test(NAME … COMMAND <target> <args…>)` 里每个以 `--` 开头的**字面量** flag，
 *      必须作为**字符串字面量**出现在 `<target>` 的源文件里（`$<…>` 生成器表达式与
 *      `${…}` CMake 变量跳过：它们不是 flag 字面量）。
 *   2. 没有 flag 的条目：源文件里必须有 `main(`，且**注册名与用例名对得上** ——
 *      `finish("x")` 或 `g_current_case = "x"` 的 `x` 等于 CTest 名。
 *   3. 报错点名：哪条 `add_test`、哪个 flag、在哪个源文件里找不到。
 *
 * ## 解析口径（为什么不是 `if(...)` 正则拍脑袋）
 * - 先按 CMake 语法去注释（`#` 到行尾，**引号内不算**），再按括号取整条命令；
 * - 实参按引号/空白切词；多行 `add_test`、`WORKING_DIRECTORY` 等属性行天然不在同一条命令里；
 * - `add_executable(<target> <src…>)` 建立 target → 源文件映射；一个 target 多个源文件时逐个查。
 *
 * ## 今天实测（2026-11-14，`node --test` 本文件）
 * - flag 判据：90 条 `add_test` / 0 条不满足（`--assets` / `--gpu` / `--stdout-quiet` /
 *   `--no-gpu-refuses-start` / `--expect-early-stop` 都在各自源文件里有字符串字面量）。
 * - 用例名判据：8 条**显式例外**（见 `NAME_DIVERGENCES`，逐条带"证据仍在"的断言，
 *   ⛔ 不是按模式放行）：改名的是历史命名（`feeder-seg-e2e` 的 finish 标签是 `feeder-seg`；
 *   `stress-*` 的用例名是 `s1/s2/s3/s9-*`），另外 3 条 conformance 是**表驱动 / ENVIRONMENT 选臂**，
 *   本来就没有"单个用例名"这一个判别子。例外表里任何一条失去证据（字面量 / env / 表注册消失）
 *   都会让本守卫变红 —— 它不会"永远绿"。
 *
 * ★ 本文件只读 `apps/emulator-lift/CMakeLists.txt` 与它声明的源文件，不构建、不跑二进制。
 * 运行：`pnpm verify`
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';

import { REPO_ROOT } from '../lib/paths.mjs';

const APP_DIR = path.join(REPO_ROOT, 'apps', 'emulator-lift');
const CMAKE_REL = 'apps/emulator-lift/CMakeLists.txt';
const CMAKE_ABS = path.join(REPO_ROOT, CMAKE_REL);

/**
 * CMake 源里的注释：`#` 到行尾，但**引号里的 `#` 不是注释**。
 * 去注释是为了不把"注释里提到的旧 flag"当成注册面（那样守卫会假绿）。
 */
function stripCmakeComments(text) {
  let out = '';
  let inQuote = false;
  for (let i = 0; i < text.length; i++) {
    const c = text[i];
    if (c === '"') inQuote = !inQuote;
    if (!inQuote && c === '#') {
      while (i < text.length && text[i] !== '\n') i++;
      out += '\n';
      continue;
    }
    out += c;
  }
  return out;
}

/** CMake 实参切词：双引号成组、去掉引号；`${…}` / `$<…>` 原样保留（调用方再判）。 */
function tokenizeCmakeArgs(body) {
  const tokens = [];
  let cur = '';
  let inQuote = false;
  for (let i = 0; i < body.length; i++) {
    const c = body[i];
    if (c === '"') {
      inQuote = !inQuote;
      continue;
    }
    if (!inQuote && /\s/.test(c)) {
      if (cur) {
        tokens.push(cur);
        cur = '';
      }
      continue;
    }
    cur += c;
  }
  if (cur) tokens.push(cur);
  return tokens;
}

/**
 * `add_executable(<target> <src…>)` → **声明列表** `[{target, sources}]`（一条命令一条）。
 * ★ 为什么不能直接 `Map(target → sources)`：同一个 target 可以在 `if(...)` / `else()` 两个互斥分支里
 *   各声明一次（`lift-x87-diff` / `lift-ops-diff` / `lift-s7-structured` 就是这样：AOT 开时多链一个
 *   `aot_dispatch.cpp`）。Map 会按 target 去重 ⇒ 声明数少了 3 条，于是"解析器不静默漏条"的完整性
 *   断言会误报。声明列表保留每一条 ⇒ 完整性判据照旧（解析到的声明数 == 文本里的 add_executable 数）。
 */
function parseExecutableDeclarations(cmakeText) {
  const declarations = [];
  for (const m of cmakeText.matchAll(/\badd_executable\s*\(\s*([A-Za-z0-9_.-]+)\s+([^)]*)\)/g)) {
    declarations.push({
      target: m[1],
      sources: tokenizeCmakeArgs(m[2]).filter((t) => !t.startsWith('$<') && !t.startsWith('${')),
    });
  }
  return declarations;
}

/** 声明列表 → Map(target → 源文件并集)。互斥分支的两条声明合并成并集（判 flag/用例名时是超集，不会假绿）。 */
function executableSources(declarations) {
  const executables = new Map();
  for (const { target, sources } of declarations) {
    const prev = executables.get(target) ?? [];
    executables.set(target, [...prev, ...sources.filter((s) => !prev.includes(s))]);
  }
  return executables;
}

/**
 * `add_test(NAME <名字> COMMAND <target> <args…>)` → 注册条目。
 * `WORKING_DIRECTORY` / `ENVIRONMENT` 等属性在**另一条** `set_tests_properties` 里，
 * 不会混进实参；这里单独解析 ENVIRONMENT（按测试名），供"选臂靠 env"的例外核证据。
 */
function parseTests(cmakeText, executables) {
  const tests = [];
  for (const m of cmakeText.matchAll(/\badd_test\s*\(\s*NAME\s+([^\s)]+)\s+COMMAND\s+([^)]*)\)/g)) {
    const argv = tokenizeCmakeArgs(m[2]);
    const target = argv[0] ?? '';
    tests.push({
      name: m[1],
      target,
      args: argv.slice(1),
      sources: executables.get(target) ?? [],
      env: [],
    });
  }
  for (const m of cmakeText.matchAll(/\bset_tests_properties\s*\(\s*([^\s)]+)\s+PROPERTIES\s+([^)]*)\)/g)) {
    const tokens = tokenizeCmakeArgs(m[2]);
    const envIndex = tokens.indexOf('ENVIRONMENT');
    if (envIndex === -1) continue;
    const spec = tokens[envIndex + 1] ?? '';
    const names = spec
      .split(';')
      .map((s) => s.split('=')[0])
      .filter(Boolean);
    const entry = tests.find((t) => t.name === m[1]);
    if (entry) entry.env.push(...names);
  }
  return tests;
}

/** 字面量 flag：`--` 开头，且不是生成器表达式 / CMake 变量（那些不是"字面量"）。 */
function literalFlags(args) {
  return args.filter((a) => a.startsWith('--') && !a.includes('$<') && !a.includes('${'));
}

const sourceCache = new Map();

/** 源文件去 C++ 注释（行注释 / 块注释，尊重字符/字符串字面量与转义）后的文本。 */
function cleanSource(rel) {
  if (sourceCache.has(rel)) return sourceCache.get(rel);
  const abs = path.join(APP_DIR, rel);
  const raw = fs.readFileSync(abs, 'utf8');
  let out = '';
  let i = 0;
  while (i < raw.length) {
    const c = raw[i];
    if (c === '"' || c === "'") {
      const quote = c;
      out += c;
      i++;
      while (i < raw.length) {
        const d = raw[i];
        out += d;
        i++;
        if (d === '\\') {
          if (i < raw.length) {
            out += raw[i];
            i++;
          }
          continue;
        }
        if (d === quote) break;
      }
      continue;
    }
    if (c === '/' && raw[i + 1] === '/') {
      while (i < raw.length && raw[i] !== '\n') i++;
      out += '\n';
      continue;
    }
    if (c === '/' && raw[i + 1] === '*') {
      i += 2;
      while (i < raw.length && !(raw[i] === '*' && raw[i + 1] === '/')) i++;
      i += 2;
      continue;
    }
    out += c;
    i++;
  }
  sourceCache.set(rel, out);
  return out;
}

function sourcesText(sources) {
  return sources.map(cleanSource).join('\n');
}

/** 源文件里注册过的用例名（`finish("x")` 与 `g_current_case = "x"` 两种写法）。 */
function registeredCases(sources) {
  const text = sourcesText(sources);
  const names = new Set();
  for (const m of text.matchAll(/\bfinish\s*\(\s*"([^"]*)"\s*\)/g)) names.add(m[1]);
  for (const m of text.matchAll(/\bg_current_case\s*=\s*"([^"]*)"/g)) names.add(m[1]);
  return names;
}

function hasMain(sources) {
  return sources.some((s) => /\bmain\s*\(/.test(cleanSource(s)));
}

/** 整个 app 的源文本（target 源文件 + `src/**`）：用于"ENVIRONMENT 选臂"例外的 env 读取证据。 */
let appSourceCache;
function appSourceText() {
  if (appSourceCache !== undefined) return appSourceCache;
  const parts = [];
  const walk = (dir) => {
    for (const ent of fs.readdirSync(dir, { withFileTypes: true })) {
      const abs = path.join(dir, ent.name);
      if (ent.isDirectory()) {
        walk(abs);
      } else if (/\.(c|cc|cpp|h|hpp)$/.test(ent.name)) {
        parts.push(cleanSource(path.relative(APP_DIR, abs).split(path.sep).join('/')));
      }
    }
  };
  walk(path.join(APP_DIR, 'src'));
  appSourceCache = parts.join('\n');
  return appSourceCache;
}

/**
 * 用例名对不上的**显式例外**（8 条，逐条带证据断言）。
 * ⛔ 这不是"按模式永远放行"：每条都必须在源文件/CMake 里找到对应证据，否则本守卫红。
 * 命名不一致是历史事实（需求单的 `verify` 锚的是这些字面量，改名会断链），
 * 而 `conformance*` 三条本来就没有"单个用例名"这一个判别子。
 */
const NAME_DIVERGENCES = new Map([
  [
    'feeder-seg-e2e',
    {
      expectCase: 'feeder-seg',
      why: '端到端 harness 的 finish 标签是 feeder-seg（测试名多 -e2e 只是 CTest 命名；需求单 verify 锚这个字面量）',
    },
  ],
  ['stress-boot', { expectCase: 's1-boot-first-frame', why: '压力档用例名用 s1-* 编号，CTest 名用 stress-*' }],
  [
    'stress-title-steady',
    { expectCase: 's2-title-steady', why: '压力档用例名用 s2-* 编号，CTest 名用 stress-*' },
  ],
  ['stress-adv-audio', { expectCase: 's3-adv-audio', why: '压力档用例名用 s3-* 编号，CTest 名用 stress-*' }],
  [
    'stress-gpu-coverage',
    { expectCase: 's9-gpu-coverage', why: '压力档用例名用 s9-* 编号，CTest 名用 stress-*' },
  ],
  [
    'conformance',
    {
      table: true,
      why: '表驱动：用例表由 conformance_cases.cpp 的 conformanceCases() 注册，没有单个用例名',
    },
  ],
  [
    'conformance-safe',
    {
      env: 'LIFT_SAFE_MEMORY',
      why: '同一张用例表在 runtime 的 safe-memory 模式下重跑；判别子是 CTest 的 ENVIRONMENT，不是 argv/用例名',
    },
  ],
  [
    'conformance-rejects-hollow-case',
    {
      env: 'LIFT_CONFORMANCE_SELFCHECK',
      why: '同一张用例表 + 恒等判据负控；判别子是 CTest 的 ENVIRONMENT（源文件里 getenv 读取）',
    },
  ],
]);

const cmakeText = stripCmakeComments(fs.readFileSync(CMAKE_ABS, 'utf8'));
const executableDeclarations = parseExecutableDeclarations(cmakeText);
const executables = executableSources(executableDeclarations);
const tests = parseTests(cmakeText, executables);

test('CMakeLists 的每条 add_test / add_executable 都被解析到（解析器不静默漏条）', () => {
  const rawAddTest = [...cmakeText.matchAll(/\badd_test\s*\(/g)].length;
  const rawAddExecutable = [...cmakeText.matchAll(/\badd_executable\s*\(/g)].length;
  assert.equal(tests.length, rawAddTest, `解析到 ${tests.length} 条 add_test，但文本里有 ${rawAddTest} 条`);
  assert.equal(
    executableDeclarations.length,
    rawAddExecutable,
    `解析到 ${executableDeclarations.length} 条 add_executable 声明，但文本里有 ${rawAddExecutable} 条`,
  );
  // 同一 target 在互斥分支里的重复声明是合法的；但每个声明都必须被解析出 target + 至少一个源文件。
  for (const d of executableDeclarations) {
    assert.ok(d.sources.length > 0, `add_executable \`${d.target}\` 没有解析出源文件（解析器漏了这条声明）`);
  }
  // 每个 add_test 的 target 都必须能映射到源文件（否则守卫判不了）。
  for (const t of tests) {
    assert.ok(
      t.sources.length > 0,
      `add_test \`${t.name}\` 的 COMMAND target \`${t.target}\` 没有 add_executable 源文件映射（解析不了 ⇒ 守卫无法判定）`,
    );
  }
});

test('每个字面量 --flag 都作为字符串字面量出现在目标源文件里', () => {
  const problems = [];
  for (const t of tests) {
    const text = sourcesText(t.sources);
    for (const flag of literalFlags(t.args)) {
      if (!text.includes(`"${flag}"`)) {
        problems.push(
          `add_test \`${t.name}\`（target \`${t.target}\`）：flag \`${flag}\` 在 ${t.sources.join(' / ')} 里找不到字符串字面量 ` +
            `⇒ 二进制不认识它、会跑默认套件（ctest 仍可能绿）`,
        );
      }
    }
  }
  assert.deepEqual(problems, [], problems.join('\n'));
});

test('没有 flag 的 add_test 与源文件里的用例名对得上（或落在显式例外表里且证据仍在）', () => {
  const problems = [];
  const usedDivergences = new Set();
  for (const t of tests) {
    if (literalFlags(t.args).length > 0) continue;
    if (!hasMain(t.sources)) {
      problems.push(`add_test \`${t.name}\`（target \`${t.target}\`）：源文件 ${t.sources.join(' / ')} 里没有 main(`);
      continue;
    }
    const cases = registeredCases(t.sources);
    if (cases.has(t.name)) continue;

    const divergence = NAME_DIVERGENCES.get(t.name);
    if (!divergence) {
      problems.push(
        `add_test \`${t.name}\`（target \`${t.target}\`）：没有 flag，但源文件 ${t.sources.join(' / ')} 里既没有 ` +
          `finish("${t.name}") 也没有 g_current_case = "${t.name}"（注册名与用例名对不上 ⇒ 这条用例可能什么都不验）`,
      );
      continue;
    }
    usedDivergences.add(t.name);
    if (divergence.expectCase) {
      if (!cases.has(divergence.expectCase)) {
        problems.push(
          `add_test \`${t.name}\` 的例外证据失效：源文件里找不到用例名 \`${divergence.expectCase}\`（${divergence.why}）`,
        );
      }
    } else if (divergence.table) {
      if (!sourcesText(t.sources).includes('conformanceCases')) {
        problems.push(`add_test \`${t.name}\` 的例外证据失效：源文件里没有 conformanceCases（${divergence.why}）`);
      }
    } else if (divergence.env) {
      if (!t.env.includes(divergence.env)) {
        problems.push(
          `add_test \`${t.name}\` 的例外证据失效：CMake 的 ENVIRONMENT 里没有 ${divergence.env}（${divergence.why}）`,
        );
      } else if (
        !(sourcesText(t.sources) + '\n' + appSourceText()).includes(`getenv("${divergence.env}")`)
      ) {
        problems.push(
          `add_test \`${t.name}\` 的例外证据失效：目标源文件与 src/** 里都没有 getenv("${divergence.env}")（${divergence.why}）`,
        );
      }
    }
  }
  for (const name of NAME_DIVERGENCES.keys()) {
    if (!usedDivergences.has(name)) {
      problems.push(`例外表过期：\`${name}\` 今天已经不需要例外（或 CMake 里没有这条 add_test）⇒ 删掉它`);
    }
  }
  assert.deepEqual(problems, [], problems.join('\n'));
});
