/** @env pure @kind gate @suite toolchain @why CTest 守卫的单测超时被去掉 / 超时被记成「守卫红了」/ 超时后不还原或不继续下一条 —— 一条会挂死的变异就会卡住整轮变异自检（实测 20+ 分钟），而「这条守卫会红」再没有任何机械证据 */
/**
 * tools/test/mutate-timeout.test.mjs —— `tools/mutate-check.mjs` 的 **CTest 单测超时**守卫
 * （`REQ-01M71B4JXCGWR3MV1HNB6T8D05`）
 *
 * 判据（全部落在这里；★ 用例名就是下面这些小写字母）：
 *   a. **超时真的会触发**：假守卫 sleep 5 s、超时设 2 s ⇒ `runTimedCommand` 在 ~2 s 返回
 *      `{timedOut:true}`，**不**等它自己结束（墙钟预算 `TIMEOUT_SEC*1000 + 1000`）；且
 *      `ctest` 与被守可执行组成的**进程树**（含孙进程）被收掉（Windows `taskkill /T`；只杀直接子进程
 *      会留下孤儿继续烧 CPU）。
 *   a2. **对照**：`ok` / `red` 两条假命令照常交回退出码（`timedOut` 不是恒真），且**不许白等**
 *      —— 两次调用合计的墙钟必须 < `TIMEOUT_SEC*1000`（旧实现每次在退出码之后再等 2 s zombie）。
 *   b. **超时 ≠ 红**：`runMutationEntry` 把它记成
 *      `unverifiableKind:'timeout'` + `⏭ **无法判定（超时）**`，⛔ 不是 `✅ 守卫红了`；
 *      同一次运行里**下一条**假条目照跑（两条 `ctest` 各被调用两次：基线 + 施加后），
 *      还原 + 全文比对仍执行，journal 清空。
 *   c. **超时值按守卫名分层**、`--ctest-timeout <秒>` 可覆盖（规则同时写在 `--help` 里）。
 *   d. **wrapper 崩了 ≠ 超时**：假守卫在写出退出码**之前**死（`process.kill(process.ppid,'SIGKILL')`）
 *      ⇒ 从 `gone` 出口返回带 `WRAPPER_GONE_TAG` 的 `{error}`（**结构性**判据：`timedOut !== true`
 *      + error 的稳定标记，⛔ 不看墙钟 —— 机器被编译压住时不假红），而不是熬到 `{timedOut:true}`，
 *      **且它下面那棵树（含孙进程）必须被收掉**。
 *
 * ★ **本用例自己不会挂死**：假守卫无论模式都最多睡 5 s 后自行退出（即使超时逻辑坏掉），
 *   而且它只写 `.tmp/mutate-timeout-test/`（gitignored）—— ⛔ 不碰真实工作树 / 不构建 C++。
 * ★ 这条守卫**用假命令**而不是真 CTest：真 CTest 要 CMake 构建目录与几分钟编译，而这里要验的是
 *   "超时机制本身"（判据在 `runTimedCommand` / `runMutationEntry` 的接线，不在某个 .cpp 里）。
 */
import { after, before, test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { spawnSync } from 'node:child_process';

import {
  CTEST_TIMEOUT_SEC,
  WRAPPER_GONE_TAG,
  ctestTimeoutTier,
  defaultCtestTimeoutSec,
  readMutationJournal,
  runCtestGuard,
  runMutationEntry,
  runTimedCommand,
} from '../mutate-check.mjs';
import { REPO_ROOT } from '../lib/paths.mjs';

const SCRATCH = path.join(REPO_ROOT, '.tmp', 'mutate-timeout-test');
const FAKE = path.join(SCRATCH, 'fake-guard.mjs');
const CLI = path.join(REPO_ROOT, 'tools', 'mutate-check.mjs');
const SLEEP_MS = 5000; // 假守卫自己最多睡这么久（⇒ 即使超时坏掉，用例也不会无限挂）
const TIMEOUT_SEC = 2; // 被测超时：比 SLEEP_MS 小得多

/**
 * 假守卫（用户单里要求的那条"故意 sleep 超过超时"的命令）。
 * 模式：`ok` 立刻退 0 / `red` 退 1 / `hang` 先写自己的 pid、再起一个**孙进程**（模拟 ctest 起的可执行）、
 * 然后睡 `SLEEP_MS`（超时路径下永远睡不到头）。`finally` 里收掉孙进程 ⇒ 红证明那次也不留孤儿。
 */
const FAKE_SOURCE = `
import fs from 'node:fs';
import { spawn } from 'node:child_process';

const mode = process.argv[2] ?? 'ok';
const sleepMs = Number(process.argv[3] ?? '${SLEEP_MS}');
if (mode === 'hang' || mode === 'crash') {
  fs.writeFileSync(process.env.AMAYUI_FAKE_PID_FILE, String(process.pid));
  const grand = spawn(process.execPath, ['-e', 'setTimeout(() => {}, 300000)'], { stdio: 'ignore' });
  fs.writeFileSync(process.env.AMAYUI_FAKE_GRANDCHILD_FILE, String(grand.pid));
  if (mode === 'crash') {
    // ★ 模拟 wrapper 在**写出退出码之前**就死（崩 / 被外人杀）：杀掉父进程 = wrapper。
    //   我们与孙进程还留在原来的进程组里 ⇒ 收树只能靠 kill(-pgid)，这正是 d) 要验的东西。
    process.kill(process.ppid, 'SIGKILL');
  }
  try {
    await new Promise((resolve) => setTimeout(resolve, sleepMs));
  } finally {
    try { grand.kill(); } catch { /* 已被收掉 */ }
  }
}
process.exit(mode === 'red' ? 1 : 0);
`;

before(() => {
  fs.mkdirSync(SCRATCH, { recursive: true });
  fs.writeFileSync(FAKE, FAKE_SOURCE, 'utf8');
});
after(() => fs.rmSync(SCRATCH, { recursive: true, force: true }));

/** 进程还活着吗（Windows 上 `signal 0` 走 OpenProcess，ESRCH = 已退出） */
function alive(pid) {
  if (!pid) return false;
  try {
    process.kill(pid, 0);
    return true;
  } catch (err) {
    return err.code === 'EPERM';
  }
}

/**
 * 等 `pid` 消失（最多 `ms`）；返回最终是否已消失。
 * ★ 缺省预算 1500 ms（不是 3000）：这里的余量 = `SLEEP_MS − runTimedCommand 返回时刻`。
 *   修好后挂在树里的进程 ~0.1–0.2 s 内就 ESRCH（余量 8–15×）；而"只杀壳"那种退化下，
 *   假守卫要睡满剩下的 ~2900 ms 才自己退出 ⇒ 1500 的窗口足够抓住它，3000 只剩 76 ms 余量（实测）。
 *   ⛔ 别用"当前实测值"当阈值：这是**失败判定**的预算，不影响通过路径。
 */
function waitDead(pid, ms = 1500) {
  const deadline = Date.now() + ms;
  while (Date.now() < deadline) {
    if (!alive(pid)) return true;
    Atomics.wait(new Int32Array(new SharedArrayBuffer(4)), 0, 0, 25);
  }
  return !alive(pid);
}

/** `runTimedCommand` 的假命令收口：把 stdout/stderr 重定向到文件（受限沙箱里 `pipe` 会 EPERM） */
function readPid(file) {
  return Number(fs.readFileSync(file, 'utf8').trim());
}

test('a) 超时真的触发：假守卫睡 5 s、超时 2 s ⇒ 立刻返回 timedOut，且整棵进程树（含孙进程）被收掉', async () => {
  const caseDir = fs.mkdtempSync(path.join(SCRATCH, 'timed-'));
  const pidFile = path.join(caseDir, 'fake.pid');
  const grandFile = path.join(caseDir, 'grand.pid');
  const started = Date.now();
  const r = await runTimedCommand(process.execPath, [FAKE, 'hang', String(SLEEP_MS)], {
    root: caseDir,
    cwd: caseDir,
    timeoutSec: TIMEOUT_SEC,
    env: { AMAYUI_FAKE_PID_FILE: pidFile, AMAYUI_FAKE_GRANDCHILD_FILE: grandFile },
  });
  const elapsed = Date.now() - started;

  assert.equal(r.timedOut, true, `必须记成超时（实际：${JSON.stringify(r)}）`);
  assert.equal(r.timeoutSec, TIMEOUT_SEC, '超时值要如实回传（排障要看它）');
  assert.equal(r.treeKilled, true, '超时必须执行进程树收尾（Windows taskkill /T /F）；false 说明只杀了壳');
  assert.ok(r.status === undefined, '超时不许带退出码（否则会被上层读成"守卫红了/没红"）');
  // ★ 预算是**关系式**（超时值 + 1 s），⛔ 不是实测常数：修好 ≈2.0 s。
  //   上限别超过 4000（= 4×TIMEOUT_SEC）：它要抓住"超时之后又真的多等一截"的每一种退化；再放宽
  //   （旧的 `elapsed < SLEEP_MS` = 5000 就是这么失去牙口的），"2 s 超时 + 1.9 s 空转"就会静默滑过。
  //   ⚠ 单独回退**正常结束**那条 10 ms 循环是**不可观测**的（复审实测 5/5 仍绿：主循环的 `await`
  //     已经把尸收完）⇒ 别以为 4000 这个上界是为它留的。
  assert.ok(
    elapsed < TIMEOUT_SEC * 1000 + 1000,
    `超时必须**不等它自己结束**（等了 ${elapsed} ms ≥ 预算 ${TIMEOUT_SEC * 1000 + 1000} ms）`,
  );

  // 进程树：假守卫（≈ ctest）+ 它起的孙进程（≈ 被守的可执行）都必须死
  const fakePid = readPid(pidFile);
  const grandPid = readPid(grandFile);
  assert.ok(waitDead(fakePid), `假守卫进程 ${fakePid} 还活着 ⇒ 超时只杀了 wrapper / 没杀到子进程`);
  assert.ok(waitDead(grandPid), `孙进程 ${grandPid} 还活着 ⇒ 没做到"收掉进程树"（它会继续烧 CPU）`);
});

test('a2) 对照：没超时的命令照常返回退出码（证明 timedOut 不是恒真），且**不许白等**', async () => {
  const started = Date.now();
  const ok = await runTimedCommand(process.execPath, [FAKE, 'ok'], { root: SCRATCH, timeoutSec: TIMEOUT_SEC });
  assert.deepEqual(ok, { status: 0 }, `正常结束要交回退出码 0（实际：${JSON.stringify(ok)}）`);
  const red = await runTimedCommand(process.execPath, [FAKE, 'red'], { root: SCRATCH, timeoutSec: TIMEOUT_SEC });
  assert.deepEqual(red, { status: 1 }, `正常结束要交回退出码 1（实际：${JSON.stringify(red)}）`);
  const elapsed = Date.now() - started;
  // ★ 关系式（⛔ 不写实测常数）：两条"立刻退 0/1"的命令合计**不许**花到一次超时的时间。
  //   坏实现（主循环也是阻塞的）每次在退出码之后再等 2 s zombie ⇒ ≈4216 ms（2.1× 预算）；修好 ≈190 ms。
  //   ⚠ 这条守的是**整体**（主循环 + 两段收尾都 async）。**单独**把正常结束那条 10 ms 回退成
  //     `sleepSync(10)` 是**不可观测**的（复审实测：5/5 仍绿）—— 主循环的 `await` 已经把尸收完了。
  //     ⇒ 别据此以为"10 ms 那条有守卫"（那里不需要、也不值得一条断言）。
  assert.ok(
    elapsed < TIMEOUT_SEC * 1000,
    `普通结束不许白等（实际 ${elapsed} ms ≥ ${TIMEOUT_SEC * 1000} ms —— 退出码写完之后还在等它消失？）`,
  );
});

test('b) 假守卫超时 ⇒ 记「不可判定（超时）」而不是「守卫红了」；继续跑下一条；还原 + 全文比对仍执行', async () => {
  const root = fs.mkdtempSync(path.join(SCRATCH, 'e2e-'));
  const fileA = 'a.txt';
  const fileB = 'b.txt';
  const ORIGINAL_A = 'AAA original（假挂死条目）';
  const ORIGINAL_B = 'BBB original（假红条目）';
  fs.writeFileSync(path.join(root, fileA), ORIGINAL_A, 'utf8');
  fs.writeFileSync(path.join(root, fileB), ORIGINAL_B, 'utf8');
  const journalPath = path.join(root, '.tmp', 'mutate-check', 'journal.json');
  const hangPidFile = path.join(root, 'hang.pid');
  const grandPidFile = path.join(root, 'grand.pid');

  // 每条假 CTest：第 1 次调用 = 施加变异前的基线（必须绿），第 2 次 = 施加后的守卫。
  const calls = new Map();
  const verified = []; // 还原后复验基线的调用（证明"超时后仍走还原路径"）
  const rebuilt = [];
  const fakeRunCtest = (name, logFd, buildDir, opts = {}) => {
    const n = (calls.get(name) ?? 0) + 1;
    calls.set(name, n);
    const mode = n === 1 ? 'ok' : name === 'hang-guard' ? 'hang' : 'red';
    return runCtestGuard(name, logFd, buildDir, {
      ...opts,
      root,
      cwd: root,
      skipBuild: true,
      command: process.execPath,
      args: [FAKE, mode, String(SLEEP_MS)],
      env: {
        AMAYUI_FAKE_PID_FILE: hangPidFile,
        AMAYUI_FAKE_GRANDCHILD_FILE: grandPidFile,
      },
    });
  };
  const ctx = {
    root,
    journalPath,
    journalEntries: [],
    ctestTimeoutSec: TIMEOUT_SEC,
    runCtest: fakeRunCtest,
    rebuild: (buildDir) => {
      rebuilt.push(buildDir);
      return { status: 0 };
    },
    verifyCtest: (name) => {
      verified.push(name);
      return { status: 0 };
    },
  };

  const hang = {
    file: fileA,
    from: 'AAA',
    to: 'ZZZ',
    what: '假挂死条目（sleep 超过单测超时）',
    guard: 'ctest:hang-guard',
    ctest: 'hang-guard',
    buildDir: 'fake/out',
  };
  const red = {
    file: fileB,
    from: 'BBB',
    to: 'YYY',
    what: '假红条目（正常退出码 1）',
    guard: 'ctest:red-guard',
    ctest: 'red-guard',
    buildDir: 'fake/out',
  };

  const started = Date.now();
  const first = await runMutationEntry(hang, ctx);
  const second = await runMutationEntry(red, ctx);
  const elapsed = Date.now() - started;

  // ① 超时 = 不可判定（超时），⛔ 不是红
  assert.equal(first.restoreProblem, null, '超时条目也必须能完成还原（restoreProblem 必须为 null）');
  assert.equal(first.result.unverifiable, true, `超时必须记成不可判定（实际：${JSON.stringify(first.result)}）`);
  assert.equal(first.result.unverifiableKind, 'timeout', `不可判定要细分到 timeout（实际：${first.result.unverifiableKind}）`);
  assert.equal(first.result.ok, false, '超时不能算"守卫红了"（ok 必须 false）');
  assert.match(first.result.verdict, /无法判定（超时）/, `verdict 要点名超时（实际：${first.result.verdict}）`);
  assert.ok(!first.result.verdict.includes('守卫红了'), `⛔ 超时不许记成「守卫红了」（实际：${first.result.verdict}）`);
  assert.match(first.result.ran?.skipReason ?? '', /单测超时/, '原因要带"单测超时"与超时值，便于排障');

  // ② 继续下一条：hang 被调用 2 次（基线 + 施加后）之后，red 也照跑并真的红
  assert.equal(calls.get('hang-guard'), 2, `hang 该跑完基线 + 施加后两次（实际 ${calls.get('hang-guard')} 次）`);
  assert.equal(calls.get('red-guard'), 2, `超时后必须**继续下一条**（red 实际只跑了 ${calls.get('red-guard')} 次）`);
  assert.equal(second.result.ok, true, `下一条正常守卫必须照常判红（实际：${JSON.stringify(second.result)}）`);
  assert.match(second.result.verdict, /✅ 守卫红了/, `下一条的 verdict 应是红（实际：${second.result.verdict}）`);

  // ③ 还原仍执行：两条都写了原文、都比对过、都复验了基线
  assert.equal(fs.readFileSync(path.join(root, fileA), 'utf8'), ORIGINAL_A, '超时条目的源文件必须还原成原文');
  assert.equal(fs.readFileSync(path.join(root, fileB), 'utf8'), ORIGINAL_B, '正常条目的源文件必须还原成原文');
  assert.deepEqual(
    verified,
    ['hang-guard', 'red-guard'],
    `超时后仍要执行"还原 ⇒ 重建 ⇒ 复验基线"（实际复验：${JSON.stringify(verified)}）`,
  );
  assert.equal(rebuilt.length, 2, `两条都要重建回基线（实际 ${rebuilt.length} 次）`);

  // ④ journal 清空（还原 + 全文比对通过后才移除）；进程树收掉
  const loaded = readMutationJournal(journalPath, root);
  assert.equal(loaded.ok, true, `journal 应可读（实际：${JSON.stringify(loaded)}）`);
  assert.equal(loaded.exists, false, '两条都还原并通过后 journal 应被删除（⛔ 不许留下"以为还原了"的条目）');
  assert.ok(waitDead(readPid(hangPidFile)), '超时条目的假守卫进程树必须被收掉');
  assert.ok(waitDead(readPid(grandPidFile)), '超时条目的孙进程必须被收掉');
  // ★ 预算是**关系式**（超时值 + 3 s）。修好 ≈2.3 s（一条 hang 2 s + 一条立刻红的正常条目）。
  //   ⚠ **将来再加一条 hang 条目必须重算**：每条 hang ≈ +TIMEOUT_SEC，现在的 3 s 余量只够兜住
  //     "一条 hang"，加第二条就要写成 `（hang 条数 + 1) * TIMEOUT_SEC * 1000 + 余量`，否则再次退化成
  //     "等于没设防"（旧的 `4 * SLEEP_MS` = 20 s 就是这么失去牙口的）。
  assert.ok(
    elapsed < TIMEOUT_SEC * 1000 + 3000,
    `整轮不能卡在挂死条目上（实际 ${elapsed} ms ≥ 预算 ${TIMEOUT_SEC * 1000 + 3000} ms）`,
  );
});

test('c) 超时值按守卫名分层（assets / external 更大）+ CLI `--ctest-timeout` 可覆盖且在 help 里写明', () => {
  assert.equal(ctestTimeoutTier('guest-token-stress'), 'normal');
  assert.equal(ctestTimeoutTier('assets-gpu'), 'assets');
  assert.equal(ctestTimeoutTier('gpu-assets'), 'assets');
  assert.equal(ctestTimeoutTier('external-boot'), 'assets');
  assert.equal(defaultCtestTimeoutSec('guest-token-stress'), CTEST_TIMEOUT_SEC.normal);
  assert.equal(defaultCtestTimeoutSec('external-boot'), CTEST_TIMEOUT_SEC.assets);
  assert.ok(
    CTEST_TIMEOUT_SEC.assets > CTEST_TIMEOUT_SEC.normal,
    'assets / external 档必须比普通档大（⛔ 一个常数拍死全部会把慢档冤枉成超时）',
  );

  // help：分层规则必须写出来（否则用户不知道 `--ctest-timeout` 什么时候该用）
  const base = path.join(SCRATCH, `help-${process.pid}`);
  const of = fs.openSync(`${base}.out`, 'w');
  const ef = fs.openSync(`${base}.err`, 'w');
  let help;
  try {
    help = spawnSync(process.execPath, [CLI, '--help'], {
      cwd: REPO_ROOT,
      stdio: ['ignore', of, ef],
    });
  } finally {
    fs.closeSync(of);
    fs.closeSync(ef);
  }
  const stdout = fs.readFileSync(`${base}.out`, 'utf8');
  assert.equal(help.status, 0, `--help 应退出 0（实际 ${help.status}）`);
  assert.match(stdout, /--ctest-timeout/, 'help 必须提到 --ctest-timeout');
  assert.match(stdout, /assets/, 'help 必须写明 assets / external 档的分层规则');

  // 非法值必须立刻红（⛔ 不许被静默忽略成"用缺省值"）
  // ★ 用文件重定向而不是 pipe：受限沙箱里捕获子进程输出要开命名管道 ⇒ spawn EPERM
  const badOut = `${base}.bad.out`;
  const badErr = `${base}.bad.err`;
  const bof = fs.openSync(badOut, 'w');
  const bef = fs.openSync(badErr, 'w');
  let bad;
  try {
    bad = spawnSync(process.execPath, [CLI, '--ctest-timeout', 'abc', '--list'], {
      cwd: REPO_ROOT,
      stdio: ['ignore', bof, bef],
    });
  } finally {
    fs.closeSync(bof);
    fs.closeSync(bef);
  }
  assert.notEqual(bad.status, 0, '--ctest-timeout abc 必须非 0 退出');
  assert.match(fs.readFileSync(badErr, 'utf8'), /正数秒/, '必须说明合法形态（而不是静默用缺省值）');
});

test('d) wrapper 崩了（写出退出码之前就死）≠ 超时：立刻返回 {error}，且它下面那棵树（含孙进程）被收掉', async () => {
  // 判据来源（实测）：旧实现里 wrapper 一死就永远看不到（zombie + POSIX 的 `kill(pid,0)` 恒成功）
  // ⇒ 熬到 `timeoutSec` 才返回 `{timedOut:true}`（`timeoutSec:1` 实测 6036 ms / timedOut:true）。
  // ⛔ 别用"自己 1.5 s 就退出"的假命令来"证明"树被收 —— 那证明不了任何东西。这里必须真起一个
  // 孙进程（≈ ctest 起被守的可执行），再验**两个 pid 都 ESRCH**。
  const caseDir = fs.mkdtempSync(path.join(SCRATCH, 'gone-'));
  const pidFile = path.join(caseDir, 'fake.pid');
  const grandFile = path.join(caseDir, 'grand.pid');
  try {
    const r = await runTimedCommand(process.execPath, [FAKE, 'crash', String(SLEEP_MS)], {
      root: caseDir,
      cwd: caseDir,
      timeoutSec: 1,
      env: { AMAYUI_FAKE_PID_FILE: pidFile, AMAYUI_FAKE_GRANDCHILD_FILE: grandFile },
    });

    assert.notEqual(r.timedOut, true, `wrapper 崩了不许记成超时（实际：${JSON.stringify(r)}）`);
    assert.ok(r.error instanceof Error, `必须交回"子进程异常结束"的 Error（实际：${JSON.stringify(r)}）`);
    // ★ **结构性判据**（`REQ-01M78A0Z5G7P7CGYRXPFEBJKCA`）：看的是**走的哪个出口** —— `gone` 分支交回的
    //   error 带稳定标记 `WRAPPER_GONE_TAG`。⛔ 不看耗时：机器被编译压住时，"发现它没了之后立刻返回"
    //   也会超过任何固定墙钟阈值 ⇒ 旧断言 `elapsed < 1000` 在负载下假红（重跑即绿）。
    assert.ok(
      r.error.message.startsWith(WRAPPER_GONE_TAG),
      `error 必须以 ${WRAPPER_GONE_TAG} 开头 ⇒ 才有机械证据证明走的是 gone 出口（实际：${r.error.message}）`,
    );

    // ★ 树：wrapper 一死，ctest / 被守的可执行就被 launchd/init 收养 ⇒ 只有 `kill(-pgid)` 能收到它们。
    //   收不到就是孤儿继续烧 CPU（假守卫还要自己睡满 SLEEP_MS，孙进程睡 300 s）。
    const fakePid = readPid(pidFile);
    const grandPid = readPid(grandFile);
    assert.ok(waitDead(fakePid, 1500), `假守卫 ${fakePid} 还活着 ⇒ "wrapper 崩了"那条分支没收树`);
    assert.ok(waitDead(grandPid, 1500), `孙进程 ${grandPid} 还活着 ⇒ 只杀了壳 / 没杀整个进程组`);
  } finally {
    // 坏实现下树可能没被收掉：别把 5 s 的假守卫 / 300 s 的孙进程留给 after()（那会拖住整个测试进程）
    for (const file of [pidFile, grandFile]) {
      try {
        const pid = Number(fs.readFileSync(file, 'utf8').trim());
        if (pid) process.kill(pid, 'SIGKILL');
      } catch {
        /* 已经死了 / pid 文件没写出来 */
      }
    }
  }
});
