/** @env pure @kind gate @suite toolchain @why 清单里的 from 片段与源文件脱节（变量改名 / 代码搬进 lambda / 文案改写 / 清理点搬家 / 判据加严）时 --list 的静态存在性检查不再报红 —— 清单会静默过期，全量变异自检变成空转，而 --list 看起来一切正常 */
/**
 * tools/test/mutate-static.test.mjs —— `tools/mutate-check.mjs --list` 的**静态存在性检查**守卫
 *
 * 被测判据是 `staticManifestCheck()`（`tools/mutate-check.mjs`）：对清单每一条**只读**地判
 *   ① `file` 存在（相对仓库根）且不是目录 / glob（目录 / 多文件声明 ⇒ 静态检查不适用，**跳过并报出来**）；
 *   ② `from` 是非空字符串、且是**当前**文件内容的子串。
 *
 * 为什么它必须常驻（实测，2026-11-10）：`from` 失配原先只有"全量跑到那一条"时才报
 * `⚠ 变异点没找到` —— 那是**清单过期**（源文件变了），⛔ 不是"守卫没红"，而且代价是整轮跑完、
 * 只在那一条上闪一下。上一轮 `REQ-01M70KGDXSKDVK7MCJYFABXERH` 的 5 条 `from` 就是这样静默积累的。
 *
 * 这条守卫守两件事（都**不是**"现在清单里有几条"）：
 *   a. **不变量**：真清单当前 **0 条静态失配**（失配数 = 0 才是"清单可用"的前提）；
 *   b. **它会红**：往 `MUTATIONS` 的**副本**里注入一条 `from` 永远匹配不到的假条目
 *      （⛔ 不改真实清单 —— 改真清单证明"会红"等于把工作树弄脏，且证明不了判定函数本身），
 *      断言判定函数**报出它**、点名那个文件、写明原因；
 *   c. **接线**：真 spawn `node tools/mutate-check.mjs --list`，退出码 0 且打出"静态检查通过 … 条里验证了 … 条"
 *      （否则 a 绿、而 `--list` 根本没在查）。
 *
 * ★ 2026-11-17 追加 `buildTarget` 的静态存在性（判据见 `tools/mutate-check.mjs` 的 `checkBuildTarget` /
 *   `ctestCommandTarget`）：填错一个 target 名 ⇒ `cmake --build --target` 失败 ⇒ 那条变异永远记
 *   「不可判定（构建失败）」⇒ 它再也不会证明"守卫会红"，而 `--list` 看起来正常（与 `buildDir` 指向已删
 *   构建树是同一种失效）。d) 解析三种 `COMMAND` 形态、e) 两类失配都点名、f) 真清单每条都真被比对过。
 *
 * ★ 全程只读仓库内的文本；子进程的 journal 指到 `.tmp/` 下（gitignored），不碰真实工作树。
 */
import { after, test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { spawnSync } from 'node:child_process';

import { MUTATIONS, ctestCommandTarget, staticManifestCheck } from '../mutate-check.mjs';
import { REPO_ROOT } from '../lib/paths.mjs';

const SCRATCH = path.join(REPO_ROOT, '.tmp', 'mutate-static-test');
const CLI = path.join(REPO_ROOT, 'tools', 'mutate-check.mjs');

after(() => fs.rmSync(SCRATCH, { recursive: true, force: true }));

/** 失配清单 → 一行一条，断言失败时方便定位（`assert` 的 message 只给第一现场） */
const describeProblems = (problems) =>
  problems.map((p) => `  ⛔ ${p.file}: ${p.why}\n      ${p.what}`).join('\n');

test('a) 不变量：真清单 0 条静态失配（每条 file 都在、from 是它的子串；不适用的逐条报出）', () => {
  assert.ok(MUTATIONS.length > 0, '清单不能是空的（空清单会让本用例恒真）');
  const r = staticManifestCheck(MUTATIONS, REPO_ROOT);

  // ★ 每一条要么**真验过**、要么被显式列为跳过 —— ⛔ 不许有"两边都不算"的条目（那就是静默放过）
  assert.equal(
    r.checked + r.skipped.length,
    MUTATIONS.length,
    `每条都必须被验证或显式跳过（验过 ${r.checked} + 跳过 ${r.skipped.length} ≠ 清单 ${MUTATIONS.length}）`,
  );
  assert.deepEqual(
    r.problems,
    [],
    `清单里有 ${r.problems.length} 条**静态失配**（源文件变过 ⇒ 全量跑到它只会记「变异点没找到」，什么也没验到；` +
      `按报出的位置改 \`from\`，或把条目删掉）：\n${describeProblems(r.problems)}`,
  );
  // ★ 这条不能用"当前数字"当断言（清单会随加守卫而变）—— 只要求"大多数条目真被验过"这件事本身有意义：
  //   一条都没验到（全是跳过）⇒ 上面那条不变量其实在验空集合。
  assert.ok(r.checked > 0, `没有任何条目真被验证过（全部跳过：${JSON.stringify(r.skipped)}）`);
});

test('b) 构造假失配 ⇒ 判定函数报出它（证明 a 的「0 条」不是恒真断言）', () => {
  const total = MUTATIONS.length;
  const sample = MUTATIONS[0];

  // ① `from` 永远匹配不到：这条必须是 **problem**（不是 skip、不是静默通过）
  const ghost = `${sample.file}`;
  const faked = [...MUTATIONS, { file: ghost, from: '这个片段绝对不会出现在任何源文件里（静态检查自检用）', what: '假失配探针' }];
  const r = staticManifestCheck(faked, REPO_ROOT);
  assert.equal(r.problems.length, 1, `假失配必须被报出来（实际 problems=${JSON.stringify(r.problems)}）`);
  assert.equal(r.problems[0].file, ghost, '报的必须正是那条假条目（而不是清单里别的条目碰巧坏了）');
  assert.match(r.problems[0].why, /from 片段已不存在/, `原因要指名 from 失配（实际：${r.problems[0].why}）`);
  assert.equal(r.problems[0].what, '假失配探针', '要从 `what` 取一句话（便于定位）');

  // ② `file` 不存在：同样是 problem（文件被删 / 改名 ⇒ 清单过期）
  const gone = staticManifestCheck([{ file: 'tools/绝对不存在的文件.mjs', from: 'x', what: '文件不存在探针' }], REPO_ROOT);
  assert.equal(gone.problems.length, 1);
  assert.match(gone.problems[0].why, /file 不存在/);

  // ③ 目录 = 多文件声明 ⇒ **跳过并报出来**（⛔ 不许静默放过）
  const dir = staticManifestCheck([{ file: 'tools/lib', from: 'x', what: '目录探针' }], REPO_ROOT);
  assert.deepEqual(dir.problems, [], '目录声明不是"失配"（本条目不适用，⛔ 不是清单坏了）');
  assert.equal(dir.skipped.length, 1, '跳过的条目必须被报出来');
  assert.match(dir.skipped[0].why, /目录/, `跳过原因要可读（实际：${dir.skipped[0].why}）`);

  // ④ 副本注入**不许**改到真清单（上一轮实测过"改真清单证明会红"会把工作树弄脏）
  assert.equal(MUTATIONS.length, total, '真清单的条数不许被副本操作改变');
  assert.equal(MUTATIONS[0].from, sample.from, '真清单第 1 条的 `from` 不许被副本操作改变');
});

test('c) 接线：`--list` 真的跑静态检查并打汇总（退出码 0；不动任何变异文件）', () => {
  fs.mkdirSync(SCRATCH, { recursive: true });
  let n = 0;
  /** 跑一次真 CLI（两个流分开重定向到文件 —— 受限沙箱里 `stdio: 'pipe'` 会 EPERM） */
  const run = (args) => {
    const base = path.join(SCRATCH, `list-${process.pid}-${n++}`);
    const of = fs.openSync(`${base}.out`, 'w');
    const ef = fs.openSync(`${base}.err`, 'w');
    let res;
    try {
      res = spawnSync(process.execPath, [CLI, ...args], {
        cwd: REPO_ROOT,
        env: { ...process.env, AMAYUI_MUTATE_JOURNAL: path.join(SCRATCH, 'journal.json') },
        stdio: ['ignore', of, ef],
      });
    } finally {
      fs.closeSync(of);
      fs.closeSync(ef);
    }
    const stdout = fs.readFileSync(`${base}.out`, 'utf8');
    const stderr = fs.readFileSync(`${base}.err`, 'utf8');
    fs.rmSync(`${base}.out`, { force: true });
    fs.rmSync(`${base}.err`, { force: true });
    return { status: res.status, stdout, stderr, all: `${stdout}\n${stderr}` };
  };

  const list = run(['--list']);
  assert.equal(list.status, 0, `清单健康时 --list 必须退出 0（${list.all}）`);
  assert.match(
    list.stdout,
    /静态检查通过：\d+ 条里验证了 \d+ 条/,
    `--list 必须打静态检查的汇总（否则这个检查没接线：stdout=${JSON.stringify(list.stdout)}）`,
  );
  // ★ `buildTarget` 的静态检查也必须**在 --list 的汇总里露面**（否则它跑了没跑看不出来）
  assert.match(
    list.stdout,
    /buildTarget：\d+ 条与 CMakeLists 的 add_test 映射一致/,
    `--list 必须打 buildTarget 的静态检查计数（否则这条检查没接线：stdout=${JSON.stringify(list.stdout)}）`,
  );
  assert.ok(!list.stdout.includes('⛔'), `清单健康时不该有 ⛔（实际：${list.stdout}）`);
  assert.equal(list.stderr.trim(), '', `清单健康时 --list 不该往 stderr 写东西（实际：${JSON.stringify(list.stderr)}）`);

  // ★ `--list --match` 是**同一条**静态检查路线（⛔ 不许只在全量 `--list` 上生效）：
  //   选中 1 条带 buildTarget 的条目 ⇒ 汇总里必须出现"buildTarget：1 条"
  const one = run(['--list', '--match', 'aot-window-profile']);
  assert.equal(one.status, 0, `--list --match 一条健康条目必须退出 0（${one.all}）`);
  assert.match(
    one.stdout,
    /buildTarget：1 条与 CMakeLists 的 add_test 映射一致/,
    `--list --match 也必须跑 buildTarget 的静态检查（stdout=${JSON.stringify(one.stdout)}）`,
  );

  // ★ `--match` 一条都没选中 ⇒ 必须**报**，不许打"0 条里验证了 0 条 / 静态检查通过"（一次空验证看起来像通过）
  const noMatch = run(['--list', '--match', `绝不匹配的串-${process.pid}`]);
  assert.notEqual(noMatch.status, 0, `--match 空匹配必须非 0 退出（实际：${noMatch.all}）`);
  assert.match(noMatch.all, /没有匹配/, '必须说明"没有匹配"，而不是静默通过');
  assert.ok(!noMatch.stdout.includes('静态检查通过'), `⛔ 空匹配不许打出"静态检查通过"（假绿）：${noMatch.stdout}`);
});

/* ── 2026-11-17：`buildTarget` 的静态存在性（填错 target = 那条变异永远「不可判定（构建失败）」）── */

/** 假 CMakeLists：覆盖 `COMMAND` 的三种形态 + 跨行 `add_test` + 注释掉的一条（⛔ 不真编 C++） */
const FAKE_CMAKE = [
  '# 假 CMakeLists（只喂给解析器；⛔ 不真编 C++）',
  'add_test(NAME bare COMMAND lift-bare --flag)',
  'add_test(NAME wrapped COMMAND $<TARGET_FILE:lift-wrapped> $<TARGET_FILE:an-arg>)',
  'add_test(NAME multi-line',
  '         COMMAND lift-multi --x "${SOME_DIR}")',
  'add_test(NAME from-var COMMAND ${CMAKE_COMMAND} -E echo hi)',
  '# add_test(NAME commented-out COMMAND lift-nope)',
].join('\n');

test('d) `add_test(NAME … COMMAND …)` ⇒ 可执行 target：裸名 / $<TARGET_FILE:> / 变量判不了 / 注释与跨行', () => {
  // ① 裸名（最常见）
  assert.deepEqual(ctestCommandTarget(FAKE_CMAKE, 'bare'), { found: true, ok: true, target: 'lift-bare' });
  // ② `$<TARGET_FILE:<名>>`：取尖括号里那个名字（后面再跟别的 `$<TARGET_FILE:…>` 实参也不影响）
  assert.deepEqual(ctestCommandTarget(FAKE_CMAKE, 'wrapped'), { found: true, ok: true, target: 'lift-wrapped' });
  // ③ 跨行 add_test（NAME 与 COMMAND 不在同一行）
  assert.deepEqual(ctestCommandTarget(FAKE_CMAKE, 'multi-line'), { found: true, ok: true, target: 'lift-multi' });
  // ④ `COMMAND ${变量}` ⇒ **判不了**（⛔ 不猜）
  const fromVar = ctestCommandTarget(FAKE_CMAKE, 'from-var');
  assert.equal(fromVar.found, true);
  assert.equal(fromVar.ok, false);
  assert.match(fromVar.why, /变量/, `要说明 COMMAND 是变量（实际：${fromVar.why}）`);
  // ⑤ 注释掉的 add_test **不是注册**（与"注册被删"同判）
  assert.deepEqual(ctestCommandTarget(FAKE_CMAKE, 'commented-out'), { found: false });
  // ⑥ 文本里没有这条 add_test
  assert.deepEqual(ctestCommandTarget(FAKE_CMAKE, 'ghost'), { found: false });
});

test('e) 假清单 + 假 CMakeLists：`buildTarget` 的三类失配都点名（找不到 / 判不了 / 应为 X 实为 Y）', () => {
  const root = path.join(SCRATCH, 'fake-root');
  const cmakeDir = path.join(root, 'apps', 'emulator-lift');
  fs.mkdirSync(cmakeDir, { recursive: true });
  fs.writeFileSync(path.join(cmakeDir, 'CMakeLists.txt'), FAKE_CMAKE, 'utf8');
  fs.mkdirSync(path.join(root, 'src'), { recursive: true });
  fs.writeFileSync(path.join(root, 'src', 'a.cpp'), 'int a;\n', 'utf8');

  const base = { file: 'src/a.cpp', from: 'int a;', to: 'int b;', buildDir: 'apps/emulator-lift/out' };
  const withCtest = (ctest, buildTarget, what) => ({ ...base, guard: `ctest:${ctest}`, ctest, buildTarget, what });
  const faked = [
    withCtest('bare', 'lift-bare', '一致（绿）'),
    withCtest('wrapped', 'lift-wrapped', '一致（绿，$<TARGET_FILE:>）'),
    withCtest('ghost', 'lift-bare', 'CTest 名在 CMakeLists 里找不到'),
    withCtest('from-var', 'lift-bare', 'COMMAND 判不了'),
    withCtest('bare', 'lift-wrapped', '值错（应为 lift-bare）'),
    { file: 'src/a.cpp', from: 'int a;', what: '不带 buildTarget（全量构建）' },
  ];
  const r = staticManifestCheck(faked, root);
  // 一致的两条 + 不带字段的一条都不报；三类失配各报一条
  assert.equal(
    r.problems.length,
    3,
    `应当恰好 3 条 buildTarget 失配（实际：\n${r.problems.map((p) => `  ⛔ ${p.what}: ${p.why}`).join('\n')}\n）`,
  );
  assert.equal(r.checked, faked.length, '`from` 检查不受影响（每条都在）');
  assert.equal(r.targetChecked, 3, '一致的两条 + 值错的那条都**真比对过**（找不到 / 判不了的不算比对）');
  assert.deepEqual(r.skipped, [], '这里没有多文件声明');

  const byWhat = new Map(r.problems.map((p) => [p.what, p]));
  // a. CTest 名在 CMakeLists 里找不到 ⇒ 点名 CTest 名 + 条目 + buildDir
  const missing = byWhat.get('CTest 名在 CMakeLists 里找不到');
  assert.ok(missing, 'CTest 名找不到必须报');
  assert.equal(missing.file, 'src/a.cpp', '必须点名**条目**本身（而不是 CMakeLists）');
  assert.match(missing.why, /找不到 add_test\(NAME ghost …\)/, `要点名 CTest 名（实际：${missing.why}）`);
  assert.match(missing.why, /buildDir=apps\/emulator-lift\/out/, `要点名 buildDir（实际：${missing.why}）`);
  // b. `COMMAND ${变量}` 判不了 ⇒ 同样必须报（⛔ 不猜一个 target 出来）
  const unjudgeable = byWhat.get('COMMAND 判不了');
  assert.ok(unjudgeable, 'COMMAND 判不了必须报');
  assert.match(unjudgeable.why, /COMMAND 判不了/, `要说明判不了（实际：${unjudgeable.why}）`);
  assert.match(unjudgeable.why, /buildDir=apps\/emulator-lift\/out/);
  // c. 值错 ⇒ 点名"应为 X，实为 Y"
  const mismatch = byWhat.get('值错（应为 lift-bare）');
  assert.ok(mismatch, '值错必须报');
  assert.match(mismatch.why, /应为 lift-bare，实为 lift-wrapped/, `要点名应为/实为（实际：${mismatch.why}）`);
  assert.match(mismatch.why, /buildDir=apps\/emulator-lift\/out/);
});

test('f) 真清单：每条带 buildTarget 的条目都**真比对过**（覆盖面不是静默跳过）', () => {
  const withTarget = MUTATIONS.filter((m) => typeof m.buildTarget === 'string');
  assert.ok(withTarget.length > 1, `清单里应当有多条带 buildTarget 的条目（实际 ${withTarget.length}）`);
  const r = staticManifestCheck(MUTATIONS, REPO_ROOT);
  assert.deepEqual(
    r.problems,
    [],
    `真清单不该有静态失配：\n${r.problems.map((p) => `  ⛔ ${p.file}: ${p.why}`).join('\n')}`,
  );
  assert.equal(
    r.targetChecked,
    withTarget.length,
    `带 buildTarget 的 ${withTarget.length} 条里只有 ${r.targetChecked} 条真比对过（其余被判不了 ⇒ 这条检查其实没生效）`,
  );
});
