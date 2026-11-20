#!/usr/bin/env node
/**
 * tools/mutate-check.mjs —— **守卫自检**：改坏一个常量，确认对应守卫**当场红**
 *
 * ## 为什么需要它
 * 本仓的纪律是"守卫要**红得有意义**"。但"写了守卫"和"守卫真的会红"是两件事 ——
 * 一个恒真的断言、一个把 `expected` 抄成 `actual` 的断言，都能"一直绿"。
 * 本工具用**变异测试**把这件事变成可复跑的判据：对每个**关键守卫**施加一处已知的破坏，
 * 要求**指定的守卫文件退出码非 0**（每个守卫一条代表，见 `AGENTS.md` §10 —— ⛔ 不是每个常量一条）。
 *
 * ## 安全口径（★ 动的是工作树里的文件，所以必须能还原）
 * 1. 施加前后都读一遍原文；**原文进内存**，`finally` 里写回（异常路径也还原）。
 * 2. 还原后**再读一遍比对**（不只看长度 —— 比对全文）。
 * 3. 任何一步不符就**立刻中止**并报告，不继续跑下一个变异（避免"半坏的工作树"）。
 * 4. 不联网、不写别的文件、不改旧仓。
 *
 * ## 崩溃安全：预写日志（journal）+ 启动自愈
 * `finally` 只兜得住"进程还能跑"的路径。SIGKILL / 休眠 / 断电会绕过它，把**变异态文件**
 * 留在工作树里 —— 下一次启动毫无察觉，于是"跑绿了"可能是"在变异态上跑绿了"。所以：
 * 1. **动任何文件之前**，把 `{file, before, mutated, guard?}` 原子写进
 *    `.tmp/mutate-check/journal.json`（临时文件 + `rename`；gitignored）。
 * 2. **每次启动**（含 `--list`）先做自愈：盘上是 `before` ⇒ 无需恢复；是 `mutated` ⇒ 写回
 *    `before`；**两者都不是** ⇒ 拒绝运行（退出码 3，保留 journal），因为那可能是别人改的。
 * 3. 带 CTest 守卫信息的条目在恢复后**重建构建目录**（否则 exe 仍是变异版）。
 * 4. `SIGINT` / `SIGTERM` 装同步恢复（把当前已施加的变异写回原文再退出）；`SIGKILL` 靠 journal。
 * 5. 逐条还原并比对通过后条目才从 journal 移除；全部清空时删除 journal。
 * 恢复/判定逻辑抽成导出函数（`recoverMutationJournal` 等），守卫在 `tools/test/mutate-journal.test.mjs`。
 *
 * ## 单测超时（`REQ-01M71B4JXCGWR3MV1HNB6T8D05`）
 * CTest 守卫会再起被守的可执行；某条变异把它挂死时（实测：无锁并发改 `std::map` ⇒ 空链表成环、烧满
 * 20+ 分钟），没有超时就会**卡住整轮自检**。所以每条 CTest 守卫都有**单测超时**：
 * 1. 超时 ⇒ 收掉 `ctest` 与它起的可执行组成的**进程树**（Windows `taskkill /T /F`，POSIX 进程组），
 *    记「**不可判定（超时）**」并**继续下一条**；
 * 2. 超时**不是**「✅ 守卫红了」——后者是"变异被抓住"的机械证据，超时什么也没证明（⛔ 不许混）；
 * 3. 超时后照样执行**还原 + 全文比对**（`finally` 里，与其它分支同一条路径）；
 * 4. 超时值按守卫名分层（缺省：普通 120 s；名字含 `assets` / `external` 段 600 s），
 *    `--ctest-timeout <秒>` 全局覆盖；规则同时写在 `--help` 与 `defaultCtestTimeoutSec`。
 * ⇒ 四态：**红** / **绿** / **不可判定(超时)** / **不可判定(其它：构建失败 / 构建目录未配置 / skip)**；
 *   汇总行**分开计数**，有超时 ⇒ 退出码 **5**（⛔ 不许让 exit 0 把它盖过去）。
 * 判据抽成导出函数（`runTimedCommand` / `runMutationEntry`），守卫在
 * `tools/test/mutate-timeout.test.mjs`。
 *
 * 用法：`pnpm test:mutation`（也可 `node tools/mutate-check.mjs --list` 只看变异清单 ——
 * 它同时做**静态存在性检查**：① 每条 `from` 是不是还在源文件里；② 每条 `buildTarget` 是否**等于**
 * `add_test(NAME <x> COMMAND <y> …)` 推出的可执行 target（权威来源是 CMakeLists，⛔ 不手工维护第二张表）；
 * ③ 该 CTest 若 spawn 了**别的产品 exe**（`$<TARGET_FILE:z>`，z ≠ buildTarget），同一 CMakeLists 必须有
 * `add_dependencies(buildTarget z)`（否则产品 exe 停在变异前 ⇒ 那条变异**假绿**）。
 * 判据抽成导出函数 `staticManifestCheck` / `ctestCommandTarget` / `ctestSpawnedTargets` /
 * `addDependencyTargets`，守卫在 `tools/test/mutate-static.test.mjs`；清单过期的根因见那里的注释）。
 */
import fs from 'node:fs';
import path from 'node:path';
import process from 'node:process';
import { spawn, spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');

// ★ CTest 类守卫：行为断言在 `apps/emulator-cxx` 的 CTest 里，`node --test` 跑不了它。
//   这类条目必须**先重建再跑**（判据在 exe 里）；还原源文件之后也必须重建成基线，
//   否则下一次 `ctest` 会假红（源文件已还原、二进制里还留着变异）。
const CTEST_BUILD_DIR = 'apps/emulator-cxx/out';
const ctestStdio = (logFd) => (logFd === null ? 'ignore' : ['ignore', logFd, logFd]);

/* ───────────── 单测超时：缺省分层 + 带进程树收尾的同步命令执行 ───────────── */

/**
 * CTest 单测超时的**缺省分层**（秒）。
 * ★ 为什么不是"一个常数拍死全部"：CTest 单条通常 < 30 s，但 `assets` / `external` 档要读 LFS 资产 /
 *   真机安装，几分钟是正常的 —— 用普通档的常数会把它们冤枉成"超时"（= 不可判定），
 *   而用它们的常数会让普通守卫挂死时白等十几分钟。规则（与 `--help` 一致）：
 *   守卫名里出现**独立的** `assets` / `external` 段（`assets-*` / `*-assets` / `external/...`）
 *   ⇒ 用 `assets` 档；其余 ⇒ `normal`。`--ctest-timeout <秒>` 全局覆盖。
 */
export const CTEST_TIMEOUT_SEC = { normal: 120, assets: 600 };

/** 守卫名 → 超时档（`normal` / `assets`）；判据只看名字里的独立段，⛔ 不做子串匹配。 */
export function ctestTimeoutTier(name) {
  return /(^|[-_/])(assets|external)([-_/]|$)/i.test(String(name)) ? 'assets' : 'normal';
}

/** 守卫名 → 缺省单测超时（秒）。 */
export function defaultCtestTimeoutSec(name) {
  return CTEST_TIMEOUT_SEC[ctestTimeoutTier(name)];
}

/** 进程还在不在（Windows 上 `signal 0` 走 OpenProcess；ESRCH = 已退出）。 */
function isProcessAlive(pid) {
  if (!pid) return false;
  try {
    process.kill(pid, 0);
    return true;
  } catch (err) {
    return err.code === 'EPERM'; // 存在但没权限：仍算"在"
  }
}

/**
 * 收掉 `pid` 及其**整棵子树**。
 * ★ 为什么不能只杀 `ctest`：`ctest` 只是壳，被守的可执行是它的子进程；Windows 的
 *   `TerminateProcess` / POSIX 的 `kill(pid)` 都只碰直接子进程 ⇒ 可执行会变成孤儿继续烧 CPU
 *   （实测症状：`ctest` 死了、`lift-guest-token.exe` 还在跑）。
 *   ⇒ Windows 用 `taskkill /PID <pid> /T /F`（`/T` = 整棵树）；POSIX 上子进程以
 *   `detached: true` 起成**独立进程组**，杀 `-pid`（组）。
 */
function killProcessTree(pid) {
  if (!pid) return false;
  if (process.platform === 'win32') {
    const r = spawnSync('taskkill', ['/PID', String(pid), '/T', '/F'], { stdio: 'ignore', windowsHide: true });
    return !r.error && (r.status === 0 || r.status === 128);
  }
  try {
    process.kill(-pid, 'SIGKILL');
    return true;
  } catch {
    try {
      process.kill(pid, 'SIGKILL');
      return true;
    } catch {
      return false;
    }
  }
}

/**
 * 退出码落盘用的 wrapper：`node -e <它> <command> <args...>`（`-e` 下 `argv[1]` 就是第一个参数）。
 *
 * ★ 为什么是 `spawn` 而不是 `spawnSync`（复审裁决）：wrapper 必须把**真命令的 pid** 记下来 ——
 *   Windows 上 `taskkill /T /F` 是靠**父进程枚举子进程**收树的，wrapper 一死就再也枚举不到
 *   `ctest` / 被守的可执行（= 孤儿）。`spawnSync` 的返回对象**有** `pid`，但它**只在子进程退出后才返回**
 *   ⇒ pid 到手时子进程已经没了、`gone` 分支用不上；所以必须 `spawn` + `exit` 事件（pid 在跑的时候就拿得到）。
 *   语义不变：仍然是"先写退出码文件，再 `process.exit`"。
 */
const TIMED_WRAPPER = `
const { spawn } = require('node:child_process');
const fs = require('node:fs');
const out = process.env.AMAYUI_MUTATE_EXIT_FILE;
const childFile = process.env.AMAYUI_MUTATE_CHILD_FILE;
const cmd = process.argv[1];
const args = process.argv.slice(2);
const writeOut = (code) => {
  if (!out) return;
  try { fs.writeFileSync(out, String(code)); } catch { /* 写不进去就只剩 126 兜底 */ }
};
let child;
try {
  child = spawn(cmd, args, { stdio: 'inherit' });
} catch (err) {
  writeOut(126);
  process.exit(126);
}
// ★ 记下真命令的 pid：wrapper 死了之后，父进程只能靠这个文件找到这棵树。
//   ⛔ 写失败不影响主路径（它只用于"wrapper 崩了"那条兜底分支）。
if (childFile && child.pid) {
  try { fs.writeFileSync(childFile, String(child.pid)); } catch { /* 兜底用，失败不致命 */ }
}
let exited = false;
child.on('error', () => {
  if (exited) return;
  exited = true;
  writeOut(126);
  process.exit(126);
});
child.on('exit', (code) => {
  if (exited) return;
  exited = true;
  const c = typeof code === 'number' ? code : 125;
  writeOut(c);
  process.exit(c);
});
`;

/**
 * 读 wrapper 记下的**真命令** pid（`AMAYUI_MUTATE_CHILD_FILE`）。
 * 只有 `gone`（wrapper 在写出退出码之前就没了）分支在 **Windows** 上用它：那边 `taskkill /T`
 * 靠父进程枚举子进程，wrapper 一死就枚举不出被收养的 `ctest` / 被守的可执行。
 * @returns {number|null} 读不到 / 内容非法 ⇒ `null`
 */
function readRecordedChildPid(file) {
  try {
    const pid = Number.parseInt(fs.readFileSync(file, 'utf8').trim(), 10);
    return Number.isInteger(pid) && pid > 0 ? pid : null;
  } catch {
    return null;
  }
}

let timedCommandSeq = 0;

/**
 * 当前**在飞**的 wrapper pid（spawn 成功后置、`finally` 清）。
 * ★ 只给 `restoreOnSignal()` 用：等待循环改 async 之后，SIGINT / SIGTERM **真的会**在 `await` 期间派发
 *   （旧的 `Atomics.wait` 把它整整挡住），而它会 `process.exit` —— 不先把在飞的 wrapper 树收掉，
 *   `ctest` 与被守的可执行就变成孤儿继续烧 CPU。
 */
let activeTimedPid = null;

/**
 * `gone` 出口（wrapper 在写出退出码**之前**就结束）的 error 上带的**稳定标记**。
 * ★ 为什么要有它（`REQ-01M78A0Z5G7P7CGYRXPFEBJKCA`）：区分「wrapper 崩了」与「真超时」必须是
 *   **结构性**判据 —— 看 `runTimedCommand` 走的是哪个出口（带本标记的 `{error}` vs `{timedOut:true}`），
 *   ⛔ 不是看墙钟：机器被编译压住时"发现它没了之后立刻返回"也会超过任何固定阈值 ⇒ 假红。
 *   守卫 `tools/test/mutate-timeout.test.mjs` 的 d) 断言「`timedOut !== true` + error 带本标记」。
 */
export const WRAPPER_GONE_TAG = '[wrapper-gone]';

/**
 * 跑一条命令（**async**），带**单测超时**与**进程树收尾**。
 *
 * 为什么要一层 `node -e` wrapper（而不是 `spawnSync({timeout})`）：`spawnSync` 的 `timeout` 到点只
 * `TerminateProcess` 直接子进程（= `ctest`），被守的可执行变孤儿继续烧 CPU；而我们需要在**根进程还
 * 活着时**执行 `taskkill /T`（父子链还在）。wrapper 的职责有三条：
 * ① 用 `spawn` 等 `cmd` 结束；② 把退出码写进文件（父进程轮询，拿不到子进程的 `exitCode`）；
 * ③ 把 `cmd` 的 pid 记进 `AMAYUI_MUTATE_CHILD_FILE`（`gone` 分支在 Windows 上只能靠它找树）。
 *
 * ★ 为什么是 **async**（`REQ-01M71YHT35RMW1XA6CCSNT4RNM`）：等待循环原先用 `sleepSync`（`Atomics.wait`）
 *   阻塞事件循环 ⇒ libuv 的 `waitpid` 永远跑不到 ⇒ 根进程停在 **zombie**，而 POSIX 的
 *   `process.kill(pid, 0)` 对 zombie **仍然成功** ⇒ "等它消失"等的是一个永远收不到的尸体
 *   （实测：超时白等满 5 s、正常结束白等 2 s、wrapper 崩了还被误报成"超时"）。
 *   改成 `await sleep(…)` 后事件循环每一轮都能收尸，"根真的没了"才成为真判据。
 *
 * @param {string} command
 * @param {string[]} args
 * @param {{cwd?:string, logFd?:number|null, timeoutSec?:number, root?:string, env?:object}} [opts]
 * @returns {Promise<{status:number}|{timedOut:true, timeoutSec:number, treeKilled:boolean, elapsedMs:number}|{error:Error}>}
 *   ★ `gone`（wrapper 没写退出码就没了）的 `{error}` 的 message **以 `WRAPPER_GONE_TAG` 开头** ——
 *     这是"崩了 ≠ 超时"的**结构性**判别式（⛔ 别用耗时判）。
 */
export async function runTimedCommand(command, args, opts = {}) {
  const root = opts.root ?? ROOT;
  const timeoutSec = opts.timeoutSec ?? CTEST_TIMEOUT_SEC.normal;
  const dir = path.join(root, '.tmp', 'mutate-check');
  fs.mkdirSync(dir, { recursive: true });
  const statusFile = path.join(
    dir,
    `.exit-${process.pid}-${++timedCommandSeq}-${Math.random().toString(36).slice(2, 10)}`,
  );
  // ★ wrapper 会把**真命令**的 pid 写进它（见 `TIMED_WRAPPER`）：`gone` 分支在 Windows 上要靠它收树。
  const childFile = `${statusFile}.child`;
  const stdio = opts.logFd == null ? 'ignore' : ['ignore', opts.logFd, opts.logFd];
  let child;
  try {
    child = spawn(process.execPath, ['-e', TIMED_WRAPPER, command, ...args], {
      cwd: opts.cwd ?? ROOT,
      env: {
        ...process.env,
        ...(opts.env ?? {}),
        AMAYUI_MUTATE_EXIT_FILE: statusFile,
        AMAYUI_MUTATE_CHILD_FILE: childFile,
      },
      stdio,
      windowsHide: true,
      // POSIX：自成进程组 ⇒ 超时可以整组收掉（Windows 走 taskkill /T）
      detached: process.platform !== 'win32',
    });
  } catch (err) {
    return { error: err };
  }
  // spawn 的失败是**异步**的（'error' 事件）；不挂监听器会变成未捕获异常。挂上并靠 pid 判早期失败。
  child.on('error', () => {});
  if (!child.pid) return { error: new Error(`spawn 失败：${command}`) };
  // ★ 在飞期间登记：`restoreOnSignal()` 会在 `process.exit` 之前把这一棵树收掉（见 `activeTimedPid`）。
  activeTimedPid = child.pid;
  try {
    const timeoutMs = Math.max(1, Math.round(timeoutSec * 1000));
    const started = Date.now();
    let done = false; // wrapper 写出了退出码
    let gone = false; // wrapper 没了却没写退出码（崩了）
    let timedOut = false;
    while (!done && !gone && !timedOut) {
      if (fs.existsSync(statusFile)) {
        done = true;
        break;
      }
      if (Date.now() - started >= timeoutMs) {
        timedOut = true;
        break;
      }
      // ★ 顺序很重要：先看退出码文件、再看进程存活 —— wrapper 先写文件后退出，反过来会误判成"崩了"
      if (!isProcessAlive(child.pid)) {
        gone = true;
        break;
      }
      // ★ 必须 `await`（⛔ 不是 `sleepSync`）：阻塞事件循环 ⇒ libuv 收不了尸 ⇒ zombie 的 `kill(pid,0)`
      //   仍然成功 ⇒ "等它消失"等的是永远收不到的东西（见函数头注释）。
      await sleep(25);
    }

    let treeKilled = false;
    if (timedOut) {
      treeKilled = killProcessTree(child.pid);
      // 等根进程真的消失（最多 5 s）：下一次重建 / restore 要独占构建目录里的文件
      const deadline = Date.now() + 5000;
      while (Date.now() < deadline && isProcessAlive(child.pid)) await sleep(25);
    } else if (done) {
      // 正常结束时也短暂等它退出：避免紧接着的重建撞上还没释放句柄的进程（Windows 文件占用）
      const deadline = Date.now() + 2000;
      while (Date.now() < deadline && isProcessAlive(child.pid)) await sleep(10);
    } else {
      // ★ `gone`（wrapper 在写出退出码之前就没了：崩 / 被外人杀）也必须**收树**。
      //   旧实现靠"这条分支在 POSIX 上走不到、最终走 timedOut ⇒ 上面那个 `killProcessTree`"兜住；
      //   改 async 之后它 ~25–50 ms 就会返回 `{error}` ⇒ 不在这里补一刀，ctest 与被守的可执行
      //   就被 launchd/init 收养成孤儿继续烧 CPU。
      //   实测：根（group leader）即使已被收尸，`kill(-pgid)` 仍有效 ⇒ 组里的孙进程 300 ms 内死。
      treeKilled = killProcessTree(child.pid);
      // ★ **Windows 必须再收一次**（复审裁决）：那边 `taskkill /T /F /PID <pid>` 靠**枚举 `pid` 的子进程**
      //   找整棵树，而 wrapper 已经死了 ⇒ 枚举不到被收养的 `ctest` / 被守的可执行 ⇒ 上面那一刀只会
      //   "找不到 pid"，孤儿照留。⇒ 拿 wrapper 自己记下的**真命令 pid** 再收一次（`ctest` 还活着时，
      //   `taskkill /T` 能枚举到它自己的孩子）。
      //   ⚠ pid 复用（残余风险）：只在 `recordedPid` **此刻还活着**时才动手，窗口 = wrapper 死亡 →
      //   本分支（`await sleep(25)` 的量级，毫秒级）；pid 文件名带 `-<pid>-<seq>-<random>` ⇒ 不会读到
      //   别次运行的记录。即便如此，"pid 刚好被复用成另一个无关进程"是理论上的误杀面。
      //   ★ **POSIX 不走这条**：进程组（`kill(-pgid)`）在组长被收尸后仍然有效，不必猜 pid（杜绝误杀）。
      if (process.platform === 'win32') {
        const recordedPid = readRecordedChildPid(childFile);
        if (recordedPid && recordedPid !== child.pid && isProcessAlive(recordedPid)) {
          treeKilled = killProcessTree(recordedPid) || treeKilled;
        }
      }
    }

    if (timedOut) {
      return { timedOut: true, timeoutSec, treeKilled, elapsedMs: Date.now() - started };
    }
    if (done) {
      const text = fs.readFileSync(statusFile, 'utf8').trim();
      const status = Number.parseInt(text, 10);
      return Number.isInteger(status)
        ? { status }
        : { error: new Error(`退出码文件内容非法：${JSON.stringify(text)}`) };
    }
    return { error: new Error(`${WRAPPER_GONE_TAG} 子进程在写出退出码之前就结束了（${command}）`) };
  } finally {
    activeTimedPid = null;
    fs.rmSync(statusFile, { force: true });
    fs.rmSync(childFile, { force: true });
  }
}

/** 让事件循环转一次（libuv 才收得到尸）；⚠ 只有 `buildCtestTargets` 的重试退避还用它 */
const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

/** 同步睡 `ms`（`Atomics.wait`）。★ 只留给 `buildCtestTargets` 的**重试退避** —— 它不需要收尸，
 *  而且那里是纯同步的 `spawnSync` 序列；⛔ 别把它再用回"等子进程消失"（那等于等一个收不到的 zombie）。 */
function sleepSync(ms) {
  Atomics.wait(new Int32Array(new SharedArrayBuffer(4)), 0, 0, ms);
}

/**
 * `cmake --build` 的 argv（**唯一**一处构造它 ⇒ 单测只需断言这一个纯函数）。
 *
 * ★ `target`（条目字段 `buildTarget`）= **只重建守卫生效的那个 target**。
 *   为什么需要它：本仓的 `emulator-lift-runtime` 是**静态库** ⇒ 链进每一个 exe；只改一个 TU
 *   也会把 15 MB 的 `emulator-lift-aot.exe` 重链一遍（实测单条变异 12 min，其中编译只占几秒）。
 *   带 `--target` 之后单条变异只编 1 个 TU + 链 1 个小 exe。
 *   ⚠ 代价：**其它 exe 会停在变异版** ⇒ 一轮结束时必须全量重建回基线（`finalizeBuildDirs`）。
 * @param {string} buildDir
 * @param {string|null|undefined} target
 * @returns {string[]}
 */
export function cmakeBuildArgs(buildDir, target) {
  const args = ['--build', buildDir];
  if (target) args.push('--target', String(target));
  return args;
}

/**
 * 重建构建目录。
 *
 * ★ 为什么要**重试**（实测，2026-11-08）：Windows 上同一个 exe 在几秒内被链接两次时，
 *   `lld-link` 偶尔会因为**别人还开着刚写出的 exe**（实时防护扫描 / `vcpkg z-applocal`
 *   后处理 / 上一个刚跑完的测试进程还没完全释放句柄）而失败，报
 *   `The process cannot access the file because it is being used by another process.`。
 *   症状是**抖动**：同一份源文件、同一条命令，手工跑绿、在 harness 里跑红，而且红在不同 target 上
 *   （实测 `lift-gpu-mapping.exe` / `lift-*` 轮换）。CTest 类条目一轮里要构建 3 次
 *   （基线 / 变异 / 还原），撞上的概率更高 —— 而"构建抖动"会被读成"守卫没红"或"还原失败"，
 *   那是最糟的误判（前者会被当成假守卫，后者会让整轮中止）。
 *   ⇒ 重试是安全的：构建是幂等的，真编译错误重试也不会变绿（只是慢一点）。
 *   ⛔ 不要用重试掩盖**判据**层面的失败 —— 它只包 `cmake --build`，不包 ctest / node --test。
 *
 * ★ `target`（缺省 null = **全量**）：见 `cmakeBuildArgs`。**收尾那次必须不带它**。
 */
function buildCtestTargets(logFd, buildDir = CTEST_BUILD_DIR, { attempts = 3, delayMs = 800, target = null } = {}) {
  let last = null;
  let prev = null;
  for (let i = 1; i <= attempts; i += 1) {
    last = spawnSync('cmake', cmakeBuildArgs(buildDir, target), { cwd: ROOT, stdio: ctestStdio(logFd) });
    if (!last.error && last.status === 0) {
      if (i > 1) {
        console.error(
          `（重建 ${buildDir}${target ? ` --target ${target}` : ''} 第 ${i} 次才成功；上一次 ${prev?.error ? prev.error.message : `退出码 ${prev?.status}`}，` +
            '按 Windows 文件占用抖动重试）',
        );
      }
      return last;
    }
    prev = last;
    if (i < attempts) sleepSync(delayMs);
  }
  return last;
}

/**
 * 一轮结束时的**全量重建回基线**（每个构建目录一次，⛔ 不带 `--target`）。
 *
 * ★ 为什么必须有（这条比提速更重要）：条目带 `buildTarget` 时，本轮只重建了守卫那一个 target
 *   ⇒ `out/` 里**其它 exe 会停在变异版**。之后谁拿 `out/emulator-lift-aot.exe` 去测墙钟 A/B，
 *   读的就是一个被改坏过的二进制 —— **静默错、没有症状**。
 *   ⇒ 判据不是"尽量重建"，而是"**没回到基线就不许 exit 0**"。
 * ★ 不走这条的只有一种情况：**还原失败**（源文件可能还是变异文）—— 那时全量构建会把变异编进去，
 *   比"exe 陈旧"更糟；那条路由 journal 下次自愈（`recoverMutationJournal`，它自己全量重建）。
 * @param {number|null} logFd
 * @param {string[]} dirs 去重后的构建目录（相对仓库根）
 * @param {{build?: Function}} [opts] 测试注入点（假 build，避免真跑 cmake）
 * @returns {{ok:boolean, dirs:string[], failedDir?:string, why?:string}}
 */
export function finalizeBuildDirs(logFd, dirs, opts = {}) {
  const root = opts.root ?? ROOT;
  const build = opts.build ?? buildCtestTargets;
  const done = [];
  for (const dir of dirs) {
    // 没配置过的目录不重建（那是"本机跑不了"，由条目自己记"无法判定"）
    if (!fs.existsSync(path.join(root, dir, 'CMakeCache.txt'))) continue;
    const r = build(logFd, dir, {});  // ★ 空 opts ⇒ 全量，⛔ 不传 target
    if (r?.error || r?.status !== 0) {
      return {
        ok: false,
        dirs: done,
        failedDir: dir,
        why: r?.error ? r.error.message : `退出码 ${r?.status}`,
      };
    }
    done.push(dir);
  }
  return { ok: true, dirs: done };
}


/**
 * 只跑**一条** CTest（不构建），带**单测超时**。
 * 抽出来是为了"还原后复验基线"与"变异后跑守卫"走同一份超时逻辑（⛔ 不许一处有一处没有）。
 * ★ async 是因为 `runTimedCommand` 的等待循环必须让事件循环转（见那边注释）。
 * @returns {Promise<{status:number}|{timedOut:true, timeoutSec:number, unavailable:string, reason:'timeout'}|{unavailable:string, reason:string}>}
 */
async function runCtestOnce(name, logFd, buildDir, opts = {}) {
  const timeoutSec = opts.timeoutSec ?? defaultCtestTimeoutSec(name);
  // ★ `-V` 是**必须**的：`SKIP_RETURN_CODE 77` 让 CTest 把该用例记成 Skipped 且 `ctest` 自身退出码
  //   **仍是 0**，而 `--output-on-failure` **不打印**跳过行 ⇒ 不读日志就无法把"这台机器没 GPU"
  //   与"守卫没抓住"分开。`-V` 让日志里出现 `***Skipped`（第四态，见文件头 `:38`）。
  const args = opts.args ?? ['--test-dir', buildDir, '-R', `^${name}$`, '--output-on-failure', '-V'];
  const r = await runTimedCommand(opts.command ?? 'ctest', args, {
    cwd: opts.cwd ?? ROOT,
    logFd,
    timeoutSec,
    root: opts.root ?? ROOT,
    env: opts.env,
  });
  if (r.error) return { unavailable: `ctest 不可用：${r.error.message}`, reason: 'spawn' };
  if (r.timedOut) {
    return {
      timedOut: true,
      timeoutSec,
      reason: 'timeout',
      unavailable: `单测超时 ${timeoutSec}s 未结束（ctest 与被守的可执行组成的进程树已收掉；` +
        `确属慢档就用 \`--ctest-timeout <秒>\` 调大）`,
    };
  }
  return { status: r.status };
}

/**
 * CTest 这一次**是不是被跳过**（四态里的"不可判定(其它)"）。
 *
 * ★ 为什么必须看日志而不只看退出码：`SKIP_RETURN_CODE 77` 让 CTest 把该用例记成 `Skipped`，
 *   而 `ctest` 自身退出码**仍然是 0** ⇒ 只看退出码会把"这台机器没有 GPU / 输入不在场"记成
 *   「❌ 守卫没红」（假红，整档自检坏掉）；反过来若某个配置把 77 透出成退出码，只看退出码又会
 *   记成「✅ 守卫红了」（假绿，更坏）。⇒ 两种都不是"不可判定"。
 * ★ 判据与 baseline / 变异后两处**共用这一份**（⛔ 不许一处有一处没有）。
 * @param {string} logText 该次 `ctest -V` 的输出
 * @param {number|null} code `ctest` 退出码
 */
export function ctestSkippedFromLog(logText, code) {
  return /\*\*\*Skipped/.test(logText) || code === 77;
}

/**
 * 构建 + 跑**一条** CTest。
 * ★ `buildDir` 可按条目覆盖（`apps/emulator-lift/out` 与 `apps/emulator-cxx/out` 是两个独立工程）。
 * ★ `opts` 是**测试注入点**（`tools/test/mutate-timeout.test.mjs` 用假命令验超时，不碰真 C++ 构建）：
 *   `root` / `cwd` / `timeoutSec` / `command` / `args` / `skipBuild` / `build`。
 * @returns {{status:number}|{unavailable:string}|{timedOut:true, timeoutSec:number, unavailable:string}
 *   |Promise<{status:number}|{unavailable:string}|{timedOut:true, timeoutSec:number, unavailable:string}>}
 *   `unavailable` = 本机跑不起来（没配置构建目录 / 没 cmake / 构建失败 / **超时**）。
 *   ★ 构建前的前置检查是**同步**返回，真正跑 CTest 那条路返回 `Promise`（`runCtestOnce` 是 async）
 *     ⇒ 调用方一律 `await`（`await` 非 Promise 值原样返回，两种返回形态都吃）。
 */
export function runCtestGuard(name, logFd, buildDir = CTEST_BUILD_DIR, opts = {}) {
  const root = opts.root ?? ROOT;
  if (!opts.skipBuild) {
    // ★ 先看构建目录有没有**配置过**：没有 CMakeCache.txt 时 `cmake --build` 只会给一句笼统的
    //   "not a build directory"，那不是"守卫没红"，而是"这台机器上跑不了"（原因要写进输出）。
    if (!fs.existsSync(path.join(root, buildDir, 'CMakeCache.txt'))) {
      return {
        unavailable: `构建目录未配置：${buildDir} 里没有 CMakeCache.txt（先 \`cmake --preset windows-x64\`）`,
        reason: 'config',
      };
    }
    // ★ `opts.buildTarget`（条目字段 `buildTarget`）⇒ 只重建守卫生效的那个 target。
    //   ⛔ 缺省 null = 全量（**不改缺省行为**）；一轮结束的收尾由 `finalizeBuildDirs` 全量补齐。
    const target = opts.buildTarget ?? null;
    const build = (opts.build ?? buildCtestTargets)(logFd, buildDir, { target });
    if (build.error) return { unavailable: `cmake 不可用：${build.error.message}`, reason: 'build' };
    if (build.status !== 0) {
      return { unavailable: `构建失败（cmake --build ${buildDir}，退出码 ${build.status}）`, reason: 'build' };
    }
  }
  return runCtestOnce(name, logFd, buildDir, opts);
}

/**
 * 这条变异的**守卫还在不在**（`null` = 在 / 判不了；字符串 = 不存在的原因；
 * `{unverifiable}` = **本机没启用**这个"配置期 opt-in"的守卫 —— 既不是"被删"，也不是"跑了没红"）。
 *
 * ★ 为什么必须有这一步（实测，2026-11-09）：守卫文件被删之后，`node --test <不存在的文件>`
 *   **退出码也是 1**，而本工具把"退出码非 0"记成 `✅ 守卫红了` ⇒ 一条指向已删守卫的变异会被读成
 *   "这条守卫会红"，它其实**什么也没证明**。这正是"变了就是 bug、而且变了是静默的"那一类。
 *   CTest 侧对称：`ctest -R ^无此名$` 是 `No tests were found` + 退出码 0 ⇒ 会记成"守卫没红"，
 *   也把"清单指向已删的注册"说成了"守卫坏了"。
 *   ⇒ 两类都必须**响亮**报成"守卫不存在"，⛔ 不许混进"守红 / 没红 / 无法判定"。
 *   ★ 构建目录没配置时返回 `null`（交给 `runCtestGuard` 的基线检查报"无法判定"）——
 *     没有 `CTestTestfile.cmake` 不等于"这个用例被删了"。
 */
function guardMissing(m) {
  if (!m.guard.startsWith('ctest:')) {
    return fs.existsSync(path.join(ROOT, m.guard)) ? null : `文件不存在：${m.guard}`;
  }
  const name = m.ctest ?? m.guard.slice('ctest:'.length);
  const buildDir = m.buildDir ?? CTEST_BUILD_DIR;
  const file = path.join(ROOT, buildDir, 'CTestTestfile.cmake');
  if (!fs.existsSync(file)) return null; // 没 configure 过 ⇒ 判不了，不是"被删了"
  // ★ 名字的引号形态由 CMake 版本决定：4.x 写 `add_test("name" …)`，部分版本写
  //   `add_test([=[name]=] …)`（还有裸名）。只认一种会在换 CMake 后把"已注册"读成"未注册"
  //   ⇒ 整份清单 34/49 条假红。三种形态都认，名字按整串比较（⛔ 不做子串匹配）。
  const declared = fs
    .readFileSync(file, 'utf8')
    .split(/\r?\n/)
    .some((line) => {
      if (!line.startsWith('add_test(')) return false;
      const bracket = line.match(/^add_test\(\[=\[(.*?)\]=\]/);
      if (bracket && bracket[1] === name) return true;
      const quoted = line.match(/^add_test\("([^"]*)"/);
      if (quoted && quoted[1] === name) return true;
      const bare = line.match(/^add_test\(([^\s")]+)/);
      return !!bare && bare[1] === name;
    });
  if (declared) return null;
  // ★ **配置期 opt-in** 的守卫（`m.optInEnv`，例：注册在 `if(... AND DEFINED ENV{LIFT_STRESS})` 里）：
  //   没设那个 env 时它**本来就不注册** —— 那是"本机没启用"，⛔ 不是"注册被删"。
  //   ⛔ 也不能返 `null` 放它去跑：`ctest -R ^无此名$` 打 `No tests were found!!!` 且**退出码 0**
  //   （实测）⇒ 会被记成 `❌ 守卫没红`，甚至被读成"红了"（假绿）。所以单列一类**不可判定**。
  //   ⛔ 也不把条目从清单里删掉/藏起来：`staticManifestCheck` 仍要逐条验 `from`（否则变异点
  //   会像 2026-11-10 那批 `from` 一样静默过期）。
  if (m.optInEnv && !process.env[m.optInEnv]) {
    return {
      unverifiable:
        `${m.optInEnv} 未设置 ⇒ 该 CTest 是**配置期 opt-in**` +
        `（CMake 的 \`if(... AND DEFINED ENV{${m.optInEnv}})\`），默认 configure 下不注册；` +
        '要验它就设好 env 重新 configure + 构建后再跑本工具',
    };
  }
  return `CTest 未注册：${buildDir}/CTestTestfile.cmake 里没有 add_test([=[${name}]=])`;
}

/* ───────────────── 静态存在性检查（`--list` 就报"清单过期"，不构建、不施加变异） ───────────────── */

/**
 * 去掉 CMake 文本里的 `#` 注释（**只去真注释**：引号 / bracket 实参里的 `#` 原样保留）。
 * ★ 为什么必须去注释：`# add_test(NAME 已删的用例 …)` 那种注释行**不是注册**；
 *   不去掉的话，"注册被删、只剩一行注释"会被读成"还在"，静态检查就成了"清单说了算"。
 * ★ 为什么不用 `split('#')`：`add_test(… "${P}/a#b")` 里的 `#` 是实参的一部分。
 * @param {string} text
 * @returns {string} 与原文字节数相同的文本（注释字符换成空格、换行保留 ⇒ 偏移/行号不失真）
 */
function stripCmakeComments(text) {
  const out = text.split('');
  let i = 0;
  while (i < text.length) {
    const ch = text[i];
    if (ch === '"') {
      i += 1;
      while (i < text.length) {
        if (text[i] === '\\') {
          i += 2;
          continue;
        }
        if (text[i] === '"') {
          i += 1;
          break;
        }
        i += 1;
      }
      continue;
    }
    if (ch === '[') {
      const br = /^\[(=*)\[/.exec(text.slice(i));
      if (br) {
        const close = `]${br[1]}]`;
        const end = text.indexOf(close, i + br[0].length);
        i = end < 0 ? text.length : end + close.length;
        continue;
      }
    }
    if (ch === '#') {
      const nl = text.indexOf('\n', i);
      const stop = nl < 0 ? text.length : nl;
      for (let k = i; k < stop; k += 1) out[k] = ' ';
      i = stop;
      continue;
    }
    i += 1;
  }
  return out.join('');
}

/**
 * 把 CMake 文本里每个 `<callName>(…)` 的**括号内文本**切出来（`add_test` / `add_dependencies` 共用）。
 * ★ 为什么不按行正则：这两类语句都可以跨行（本仓 `lift-aot-split-manifest` /
 *   `lift/no-gpu-refuses-start` / `lift-gpu-batch` 的依赖就是）—— 按行匹配会在"关键字与实参不在同一行"时判不了。
 * ★ 跳过引号 / `[=[…]=]` 里的括号；`#` 注释已由 `stripCmakeComments` 抹掉。
 * @param {string} text
 * @param {string} callName 形参名（`add_test` / `add_dependencies`）
 * @returns {string[]} 每项 = 一对**平衡括号**之间的文本（不含外层括号）；括号不配对的片段不报
 */
function cmakeCallBlocks(text, callName) {
  const blocks = [];
  const head = new RegExp(`\\b${callName}\\s*\\(`, 'g');
  let m;
  while ((m = head.exec(text)) !== null) {
    let i = m.index + m[0].length;
    const start = i;
    let depth = 1;
    while (i < text.length && depth > 0) {
      const ch = text[i];
      if (ch === '"') {
        i += 1;
        while (i < text.length) {
          if (text[i] === '\\') {
            i += 2;
            continue;
          }
          if (text[i] === '"') {
            i += 1;
            break;
          }
          i += 1;
        }
        continue;
      }
      if (ch === '[') {
        const br = /^\[(=*)\[/.exec(text.slice(i));
        if (br) {
          const close = `]${br[1]}]`;
          const end = text.indexOf(close, i + br[0].length);
          i = end < 0 ? text.length : end + close.length;
          continue;
        }
      }
      if (ch === '(') depth += 1;
      else if (ch === ')') depth -= 1;
      i += 1;
    }
    if (depth === 0) blocks.push(text.slice(start, i - 1));
    head.lastIndex = i;
  }
  return blocks;
}

/** 每个 `add_test(…)` 的括号内文本（`cmakeCallBlocks` 的专用入口） */
function addTestBlocks(text) {
  return cmakeCallBlocks(text, 'add_test');
}

/**
 * 切 CMake 实参：空白分隔；`"…"`（含 `\"` 转义）与 `[=[…]=]` / `[[…]]` 各算**一个** token。
 * @param {string} text 一段 `add_test(…)` 的括号内文本
 * @returns {string[]}
 */
function cmakeArgTokens(text) {
  const tokens = [];
  let i = 0;
  while (i < text.length) {
    const ch = text[i];
    if (/\s/.test(ch)) {
      i += 1;
      continue;
    }
    if (ch === '"') {
      let j = i + 1;
      let out = '';
      while (j < text.length) {
        if (text[j] === '\\') {
          out += text[j + 1] ?? '';
          j += 2;
          continue;
        }
        if (text[j] === '"') break;
        out += text[j];
        j += 1;
      }
      tokens.push(out);
      i = j + 1;
      continue;
    }
    if (ch === '[') {
      const br = /^\[(=*)\[/.exec(text.slice(i));
      if (br) {
        const close = `]${br[1]}]`;
        const end = text.indexOf(close, i + br[0].length);
        if (end >= 0) {
          tokens.push(text.slice(i + br[0].length, end));
          i = end + close.length;
          continue;
        }
      }
    }
    let j = i;
    while (j < text.length && !/\s/.test(text[j])) j += 1;
    tokens.push(text.slice(i, j));
    i = j;
  }
  return tokens;
}

/**
 * `add_test(NAME <x> COMMAND <y> …)` ⇒ CTest `<x>` 的**可执行 target** `<y>`。
 *
 * ★ 为什么这是唯一的权威来源：条目字段 `buildTarget`（见 `cmakeBuildArgs`）会变成
 *   `cmake --build <dir> --target <名>`。填错一个名字 ⇒ 那条变异**永远记「不可判定（构建失败）」**
 *   ⇒ 它再也不会证明"守卫会红"，而 `--list` 与汇总表**都看不出来**（与"`buildDir` 指向已删构建树、
 *   永远「不可判定」"是同一种失效）⇒ 判据必须**从 CMakeLists 派生**，⛔ 不手工维护第二张表。
 *
 * `COMMAND` 的三种形态（本仓现在只用前两种）：
 *   ① `COMMAND <裸名>` ⇒ target = `<裸名>`（最常见）；
 *   ② `COMMAND $<TARGET_FILE:<名>>` ⇒ target = `<名>`；
 *   ③ `COMMAND ${变量}` / 其它生成器表达式 / 路径 ⇒ **判不了**（返回 `ok:false`，⛔ 不猜）。
 *
 * @param {string} cmakeText
 * @param {string} ctestName
 * @returns {{found:false} | {found:true, ok:true, target:string} | {found:true, ok:false, why:string}}
 *   `found:false` = 文本里没有叫这个名字的 `add_test`；`ok:false` = 有，但 `COMMAND` 判不了。
 */
export function ctestCommandTarget(cmakeText, ctestName) {
  /** `COMMAND` 的第一个 token → 判据（三种形态，见函数头） */
  const classify = (cmd) => {
    if (typeof cmd !== 'string' || cmd === '') return { ok: false, why: 'COMMAND 是空的' };
    const target = /^\$<TARGET_FILE:([^>]+)>$/.exec(cmd);
    if (target) return { ok: true, target: target[1].trim() };
    if (cmd.startsWith('${') || cmd.startsWith('$<')) {
      return { ok: false, why: `COMMAND 是变量 / 生成器表达式（${cmd}），推不出 target 名` };
    }
    if (/[\\/]/.test(cmd) || /\.(exe|cmake)$/i.test(cmd)) {
      return { ok: false, why: `COMMAND 是路径而不是 target 名（${cmd}）` };
    }
    return { ok: true, target: cmd };
  };
  const found = [];
  for (const body of addTestBlocks(stripCmakeComments(cmakeText))) {
    const tokens = cmakeArgTokens(body);
    const ni = tokens.findIndex((t) => t.toUpperCase() === 'NAME');
    if (ni < 0 || ni + 1 >= tokens.length) continue;
    if (tokens[ni + 1] !== ctestName) continue;
    const ci = tokens.findIndex((t, k) => k > ni + 1 && t.toUpperCase() === 'COMMAND');
    if (ci < 0 || ci + 1 >= tokens.length) {
      found.push({ ok: false, why: '没有 COMMAND（或 COMMAND 后面没有实参）' });
      continue;
    }
    found.push(classify(tokens[ci + 1]));
  }
  if (found.length === 0) return { found: false };
  // 同名 add_test 出现多次：只有"推出的 target 一致"才算判得了（否则 CTest 名有歧义）
  const distinct = [...new Set(found.map((f) => (f.ok ? `target:${f.target}` : `why:${f.why}`)))];
  if (distinct.length > 1) {
    return { found: true, ok: false, why: `同名 add_test 出现 ${found.length} 次且 COMMAND 不一致` };
  }
  return { found: true, ...found[0] };
}

/**
 * 一条 `add_test(NAME <x> COMMAND …)` 的 COMMAND 里**spawn 了哪些 target**（`$<TARGET_FILE:<名>>`）。
 *
 * ★ 为什么必须从 COMMAND 里取（⛔ 不按"名字像产品 exe"猜）：CTest 的可执行 target 只是**父进程**；
 *   父进程若把产品 exe 当子进程起（`$<TARGET_FILE:…>` 实参），那句变异的施加点可能落在**产品 exe 的 TU** 里
 *   ⇒ `cmake --build --target <守卫>` 不重建产品 exe ⇒ 守卫跑的是变异前的二进制 ⇒ **假绿**。
 *   判据见 `checkSpawnDependencies`（缺 `add_dependencies` 就报）。
 *
 * @param {string} cmakeText
 * @param {string} ctestName
 * @returns {{found:false} | {found:true, targets:string[]}} 按出现顺序去重的 target 名
 *   （同名 `add_test` 的 `COMMAND` 判不了时前面已经报过 —— 这里只看**第一次**匹配的块）
 */
export function ctestSpawnedTargets(cmakeText, ctestName) {
  const text = stripCmakeComments(cmakeText);
  for (const body of cmakeCallBlocks(text, 'add_test')) {
    const tokens = cmakeArgTokens(body);
    const ni = tokens.findIndex((t) => t.toUpperCase() === 'NAME');
    if (ni < 0 || ni + 1 >= tokens.length) continue;
    if (tokens[ni + 1] !== ctestName) continue;
    const ci = tokens.findIndex((t, k) => k > ni + 1 && t.toUpperCase() === 'COMMAND');
    const cmdTokens = ci < 0 ? [] : tokens.slice(ci + 1);
    const targets = [];
    for (const tok of cmdTokens) {
      const re = /\$<TARGET_FILE:([^<>]+)>/g;
      let mm;
      while ((mm = re.exec(tok)) !== null) {
        const name = mm[1].trim();
        if (name !== '' && !targets.includes(name)) targets.push(name);
      }
    }
    return { found: true, targets };
  }
  return { found: false };
}

/**
 * 某 target 声明过哪些 `add_dependencies(<target> <依赖…>)` 依赖（多行 / 多目标都摊平）。
 *
 * ★ 为什么读 CMake 而不是读构建图：CMake 已经是真源，我们只读 `add_test` / `add_dependencies`
 *   两类语句（见需求单 `REQ-01M7K2J7VXKJMK1BJ3H7YCVJFJ` 的「未做」：不做通用依赖图求解）。
 *
 * @param {string} cmakeText
 * @param {string} target
 * @returns {{deps:string[], unjudgeable:boolean}} `unjudgeable` = 有 `add_dependencies` 的第一实参是
 *   变量 / 生成器表达式 ⇒ "这个 target 到底有没有声明依赖"判不了（⛔ 不猜）
 */
export function addDependencyTargets(cmakeText, target) {
  const text = stripCmakeComments(cmakeText);
  const deps = [];
  let unjudgeable = false;
  for (const body of cmakeCallBlocks(text, 'add_dependencies')) {
    const tokens = cmakeArgTokens(body);
    if (tokens.length === 0) continue;
    const head = tokens[0];
    if (head === '' || head.startsWith('$')) {
      unjudgeable = true;
      continue;
    }
    if (head !== target) continue;
    for (const d of tokens.slice(1)) {
      if (d === '' || d.startsWith('$')) continue;
      if (!deps.includes(d)) deps.push(d);
    }
  }
  return { deps, unjudgeable };
}

/**
 * 该条目的 CTest **spawn 了产品 exe** 时，`add_dependencies(<buildTarget> <产品 exe>)` 在不在。
 *
 * ★ 为什么这是假绿面（本仓两次实测）：`buildTarget` 只重建守卫 exe；若变异落在产品 exe 的 TU 里
 *   （`aot_main.cpp` 之类），产品 exe 停在**变异前** ⇒ 守卫跑旧二进制 ⇒ 变异看起来"没红"，
 *   而 `--list`（只查 target 名对不对）完全看不出来。已修的两例：`cli-aot-flags` / `aot-audio-wiring`。
 * ★ 只查**清单里带 `buildTarget` 的条目**（不带 = 全量构建，天然安全）⇒ ⛔ 不要求"所有 CTest"都加依赖。
 * ★ product == buildTarget（自 spawn）⇒ 判据不适用（同一个 exe，本来就会重建）。
 *
 * @returns {{applicable:boolean, problem?:string}} `applicable` = 真有跨 target 的 spawn（判过）
 */
function checkSpawnDependencies({ cmakeRel, text, ctestName, buildTarget, buildDir }) {
  const sp = ctestSpawnedTargets(text, ctestName);
  if (!sp.found) return { applicable: false }; // 前面已报"找不到 add_test"
  const foreign = sp.targets.filter((t) => t !== buildTarget);
  if (foreign.length === 0) return { applicable: false }; // 自 spawn / 不 spawn ⇒ 不需要依赖
  const ad = addDependencyTargets(text, buildTarget);
  const missing = foreign.filter((t) => !ad.deps.includes(t));
  if (missing.length === 0) return { applicable: true };
  return {
    applicable: true,
    problem:
      `buildTarget 静态检查：CTest \`${ctestName}\` 该用例 spawn 产品 exe ` +
      missing.map((t) => `\`${t}\`（\`$<TARGET_FILE:${t}>\`）`).join(' / ') +
      `，但缺 add_dependencies ⇒ 需要这一行：` +
      missing.map((t) => `\`add_dependencies(${buildTarget} ${t})\``).join(' / ') +
      `（${cmakeRel}）⇒ \`cmake --build --target ${buildTarget}\` 只会重建守卫 exe，产品 exe 停在变异前` +
      ` ⇒ 该变异会**假绿**（条目 buildDir=${buildDir}）` +
      (ad.unjudgeable ? '；另有 add_dependencies 的 target 是变量，判不了' : ''),
  };
}

/**
 * 条目字段 `buildTarget` 还成立吗（只读；判据 = `ctestCommandTarget`，来源 = 该 `buildDir` 所属工程的
 * `CMakeLists.txt` 的 `add_test(NAME … COMMAND …)`）。
 *
 * ★ 为什么它必须和 `from` 的存在性检查在**同一趟**里（⛔ 不新增第二次遍历）：`--list` 一次就把
 *   "清单与源文件 / CMakeLists 对不上"全部报出来，而不是等全量跑到那一条。
 * ★ 三类失配都**不可判定**、都不许静默放过：
 *   a. CTest 名在 CMakeLists 里找不到 / `COMMAND` 判不了 ⇒ 推不出 target（⛔ 不猜）；
 *   b. 推得出，但**不等于**条目的 `buildTarget` ⇒ 点名"应为 X，实为 Y"；
 *   c. target 名对，但该 CTest **spawn 了产品 exe** 而 CMakeLists 缺
 *      `add_dependencies(<buildTarget> <产品 exe>)` ⇒ 变异落在产品 exe 的 TU 里时**假绿**
 *      （判据 = `checkSpawnDependencies`；只查带 `buildTarget` 的条目，⛔ 不要求"所有 CTest"都加依赖）。
 * @param {object} m 清单条目
 * @param {string} root 仓库根
 * @param {Map<string,string|{error:string}>} cmakeTexts 同一趟里的 CMakeLists 文本缓存（按相对路径）
 * @returns {{checked:boolean, spawn?:'checked'|'not-applicable'|'problem', problem?:string}}
 *   `checked` = 真比对过（判据成立）；`spawn` = spawn 依赖判过的结果（不适用 = 自 spawn / 不 spawn）；
 *   `problem` = 失配原因
 */
function checkBuildTarget(m, root, cmakeTexts) {
  if (m?.buildTarget === undefined) return { checked: false }; // 不带 = 全量构建，判据不适用
  const buildDir = String(m.buildDir ?? CTEST_BUILD_DIR);
  const name =
    typeof m.ctest === 'string' && m.ctest !== ''
      ? m.ctest
      : String(m.guard ?? '').startsWith('ctest:')
        ? String(m.guard).slice('ctest:'.length)
        : null;
  if (typeof m.buildTarget !== 'string' || m.buildTarget === '') {
    return { checked: false, problem: `buildTarget 不是非空字符串（${JSON.stringify(m.buildTarget)}）` };
  }
  if (name === null) {
    return {
      checked: false,
      problem: `带 buildTarget 却不是 CTest 条目（guard=${JSON.stringify(m.guard)}）⇒ 判不了它对应哪条 add_test`,
    };
  }
  const cmakeRel = path.join(path.dirname(buildDir), 'CMakeLists.txt');
  let text = cmakeTexts.get(cmakeRel);
  if (text === undefined) {
    try {
      text = fs.readFileSync(path.join(root, cmakeRel), 'utf8');
    } catch (err) {
      text = { error: err.code === 'ENOENT' ? '文件不存在' : err.message };
    }
    cmakeTexts.set(cmakeRel, text);
  }
  if (typeof text !== 'string') {
    return {
      checked: false,
      problem: `buildTarget 静态检查：读不了 ${cmakeRel}（${text.error}）⇒ 判不了它注册的可执行 target（条目 buildDir=${buildDir}）`,
    };
  }
  const d = ctestCommandTarget(text, name);
  if (!d.found) {
    return {
      checked: false,
      problem:
        `buildTarget 静态检查：${cmakeRel} 里找不到 add_test(NAME ${name} …) ⇒ 判不了它的可执行 target` +
        `（条目 buildDir=${buildDir}）`,
    };
  }
  if (!d.ok) {
    return {
      checked: false,
      problem: `buildTarget 静态检查：add_test ${name} 的 COMMAND 判不了（${d.why}）（条目 buildDir=${buildDir}）`,
    };
  }
  if (d.target !== m.buildTarget) {
    return {
      checked: true,
      problem:
        `buildTarget 静态检查：应为 ${d.target}，实为 ${m.buildTarget}` +
        `（条目 buildDir=${buildDir}；来源 ${cmakeRel} 的 add_test(NAME ${name} … COMMAND …)）`,
    };
  }
  // ★ target 名对了还不够：若这条 CTest 的 COMMAND **spawn 了别的产品 exe**（`$<TARGET_FILE:X>`，
  //   X ≠ buildTarget），`--target buildTarget` 不会重建 X ⇒ 变异落在 X 的 TU 里时**假绿**。
  //   判据 = 同一个 CMakeLists 里必须有 `add_dependencies(buildTarget X)`（`checkSpawnDependencies`）。
  const spawn = checkSpawnDependencies({
    cmakeRel,
    text,
    ctestName: name,
    buildTarget: m.buildTarget,
    buildDir,
  });
  if (spawn.problem) return { checked: true, spawn: 'problem', problem: spawn.problem };
  return { checked: true, spawn: spawn.applicable ? 'checked' : 'not-applicable' };
}

/**
 * 一条变异的**施加点**在当前源文件里还成立吗（只读：不构建、不施加变异）。
 *
 * ★ 为什么要这一步（实测，2026-11-10，`REQ-01M70KGDXSKDVK7MCJYFABXERH` 的根因）：
 *   原先只有"全量跑到那一条"才报 `⚠ 变异点没找到` —— 那是**清单过期**（源文件变了、`from`
 *   已不是子串），⛔ 不是"守卫没红"。代价：要等整轮跑完才看得见，而且只在那一条上闪一下。
 *   上一轮里 `from` 就这样**静默积累了 5 条**（局部变量改名 `def`→`local` / 代码搬进 lambda
 *   ⇒ 缩进变 / 日志文案改写 / 清理点搬家 / 判据口径加严）—— 每一条都让"守卫自检"变成空转。
 *   ⇒ 存在性检查必须**静态、O(清单)**：`--list` 一次就读完全部源文件，一条也不用真的施加。
 *
 * 判据（每条 `{file, from}`）：
 *   ① `file` 存在（相对仓库根）且**不是目录**；② `from` 是非空字符串；③ `from` 是文件内容的**子串**。
 *   ★ `file` 声明成**目录 / 多文件 / glob** 时 `from` 该落在哪一份里清单说不出来 ⇒ **跳过并报出来**
 *     （⛔ 不静默放过：那会重新变回"看起来检查过了"）。
 *   ④ ★ 可选字段 `buildTarget`：必须**等于**由 `add_test(NAME <ctest> COMMAND <target> …)` 推出的
 *     可执行 target（`checkBuildTarget` / `ctestCommandTarget`）。判不了的（找不到 add_test / COMMAND 是
 *     变量）也算失配 —— 填错的 target 只会让那条变异**永远「不可判定（构建失败）」**。
 *     ⑤ ★ 该 CTest 的 COMMAND 若 **spawn 了别的产品 exe**（`$<TARGET_FILE:X>`，X ≠ buildTarget），
 *     同一 CMakeLists 必须有 `add_dependencies(buildTarget X)`（`checkSpawnDependencies`）；缺了同样算失配 ——
 *     否则 `--target buildTarget` 只重建守卫 exe，变异落在产品 exe 的 TU 里时**假绿**。
 *     ⛔ 只查带 `buildTarget` 的条目（不带 = 全量构建，天然安全）。
 *
 * @param {Array<{file:string, from:string, what:string}>} mutations 清单（真清单或其副本）
 * @param {string} [root] 仓库根（测试用临时根目录）
 * @returns {{checked:number, problems:Array<{file:string, what:string, why:string}>,
 *            skipped:Array<{file:string, what:string, why:string}>, targetChecked:number,
 *            spawnChecked:number, spawnNotApplicable:number}}
 *   `checked` = **真验过 `from`**（file 可读 + `from` 是子串）的条数；`problems` = 失配
 *   （`from` / `buildTarget` / spawn 依赖三类）；`skipped` = `from` 静态检查不适用的条目（附原因）；
 *   `targetChecked` = **真比对过 `buildTarget`** 的条数；`spawnChecked` = 其中**真有跨 target spawn
 *   且 `add_dependencies` 在场**的条数；`spawnNotApplicable` = 带 buildTarget 但**不 spawn 别的 target**
 *   （自 spawn / 无生成器表达式）⇒ 判据不适用。
 */
export function staticManifestCheck(mutations, root = ROOT) {
  const problems = [];
  const skipped = [];
  const cmakeTexts = new Map(); // CMakeLists 文本缓存（同一趟里只读一次 / 一个文件）
  let checked = 0;
  let targetChecked = 0;
  let spawnChecked = 0;
  let spawnNotApplicable = 0;
  for (const m of mutations) {
    const file = String(m?.file ?? '');
    const what = String(m?.what ?? '');
    // ④ ★ `buildTarget` 的静态存在性检查（与 `from` 的检查**同一趟**；只读 CMakeLists）
    const t = checkBuildTarget(m, root, cmakeTexts);
    if (t.checked) targetChecked += 1;
    if (t.spawn === 'checked') spawnChecked += 1;
    else if (t.spawn === 'not-applicable') spawnNotApplicable += 1;
    if (t.problem) problems.push({ file, what, why: t.problem });
    // ① 多文件 / glob 声明：静态检查不适用（必须报出来，⛔ 不静默）
    if (file === '' || /[*?[\]{}]/.test(file)) {
      skipped.push({ file, what, why: 'file 是多文件 / glob 声明 ⇒ 静态检查不适用' });
      continue;
    }
    const abs = path.join(root, file);
    let text;
    try {
      if (fs.statSync(abs).isDirectory()) {
        skipped.push({ file, what, why: 'file 是目录（多文件声明）⇒ 静态检查不适用' });
        continue;
      }
      text = fs.readFileSync(abs, 'utf8');
    } catch (err) {
      problems.push({ file, what, why: err.code === 'ENOENT' ? 'file 不存在' : `file 读不了（${err.message}）` });
      continue;
    }
    if (typeof m?.from !== 'string' || m.from === '') {
      problems.push({ file, what, why: 'from 不是非空字符串' });
      continue;
    }
    checked += 1;
    // ② `from` 必须是**当前**文件内容的子串（`String.prototype.includes` = 逐字节，不做规范化）
    if (!text.includes(m.from)) problems.push({ file, what, why: 'from 片段已不存在' });
    // ③ ★ 必须**恰好出现一次**：`:2781` 的 `before.replace(m.from, m.to)` 是字符串模式（**无 `/g`**）
    //    ⇒ 命中多处时**静默只替换第一处**，对错完全取决于行序（换成"改到别处"就假绿、改到自己就假红，
    //    两种都**不可观测**）⇒ 必须在施加之前挡下，⛔ 不是 warning。
    //    ⛔ 不开 `allowMultiple` 豁免口子：那会让这条判据退化成"清单说了算"。
    const occurrences = text.split(m.from).length - 1;
    if (occurrences > 1) {
      problems.push({
        file,
        what,
        why: `from 片段出现 ${occurrences} 次（必须恰好 1 次；否则 replace 只改第一处，对错取决于行序）`,
      });
    }
  }
  return { checked, problems, skipped, targetChecked, spawnChecked, spawnNotApplicable };
}

/* ───────────────────── 崩溃安全：预写日志（journal）与启动自愈 ───────────────────── */

/**
 * journal 的位置（相对仓库根；`/.tmp` 已在 `.gitignore` 里）。
 * ★ `AMAYUI_MUTATE_JOURNAL=<绝对路径>` 只给守卫测试注入用：让测试把 journal 指到
 *   `.tmp/` 下的临时目录，而**不碰真实工作树**。生产环境不要设置 —— 否则自愈盯的是另一个文件。
 */
export const JOURNAL_REL = '.tmp/mutate-check/journal.json';

/** journal 的绝对路径：`root` 下的缺省位置，或被 `AMAYUI_MUTATE_JOURNAL` 覆盖 */
export function resolveJournalPath(root = ROOT) {
  const override = process.env.AMAYUI_MUTATE_JOURNAL;
  return override ? path.resolve(override) : path.join(root, JOURNAL_REL);
}

/** 原子写：同目录临时文件 + `rename`（rename 在同一文件系统上是原子的） */
function atomicWriteFile(file, text) {
  fs.mkdirSync(path.dirname(file), { recursive: true });
  const tmp = `${file}.${process.pid}.tmp`;
  fs.writeFileSync(tmp, text, 'utf8');
  fs.renameSync(tmp, file);
}

/**
 * 读 + 校验 journal。
 * @returns {{ok:true, exists:boolean, entries:object[]} | {ok:false, exists:boolean, why:string, entries:object[]}}
 *   ★ `exists:false` = 没有 journal（正常：上次跑完 / 从没跑过）—— 不是错误。
 *   ★ `ok:false` = **拒绝运行**的一类：非法 JSON / 形态不对 / 条目指向仓库外。
 */
export function readMutationJournal(journalPath, root = ROOT) {
  let text;
  try {
    text = fs.readFileSync(journalPath, 'utf8');
  } catch (err) {
    if (err.code === 'ENOENT') return { ok: true, exists: false, entries: [] };
    return { ok: false, exists: true, why: `读不了 journal（${err.message}）`, entries: [] };
  }
  let doc;
  try {
    doc = JSON.parse(text);
  } catch (err) {
    return { ok: false, exists: true, why: `journal 不是合法 JSON（${err.message}）`, entries: [] };
  }
  if (doc === null || typeof doc !== 'object' || Array.isArray(doc)) {
    return { ok: false, exists: true, why: 'journal 顶层不是对象', entries: [] };
  }
  if (doc.version !== 1) {
    return { ok: false, exists: true, why: `journal 版本不认识：${JSON.stringify(doc.version)}（本工具只认 1）`, entries: [] };
  }
  if (!Array.isArray(doc.entries)) {
    return { ok: false, exists: true, why: 'journal 的 `entries` 不是数组', entries: [] };
  }
  const entries = [];
  for (let i = 0; i < doc.entries.length; i += 1) {
    const e = doc.entries[i];
    const at = `entries[${i}]`;
    if (e === null || typeof e !== 'object' || Array.isArray(e)) return { ok: false, exists: true, why: `${at} 不是对象`, entries: [] };
    if (typeof e.file !== 'string' || e.file === '') return { ok: false, exists: true, why: `${at}.file 不是非空字符串`, entries: [] };
    if (typeof e.before !== 'string') return { ok: false, exists: true, why: `${at}.before 不是字符串`, entries: [] };
    if (typeof e.mutated !== 'string') return { ok: false, exists: true, why: `${at}.mutated 不是字符串`, entries: [] };
    if (e.guard !== undefined && typeof e.guard !== 'string') return { ok: false, exists: true, why: `${at}.guard 不是字符串`, entries: [] };
    if (e.ctest !== undefined && typeof e.ctest !== 'string') return { ok: false, exists: true, why: `${at}.ctest 不是字符串`, entries: [] };
    if (e.buildDir !== undefined && typeof e.buildDir !== 'string') return { ok: false, exists: true, why: `${at}.buildDir 不是字符串`, entries: [] };
    const rel = path.relative(root, path.resolve(root, e.file));
    if (rel === '' || rel.startsWith('..') || path.isAbsolute(rel)) {
      return { ok: false, exists: true, why: `${at}.file 不在仓库根内（${e.file}）`, entries: [] };
    }
    entries.push(e);
  }
  return { ok: true, exists: true, entries };
}

/**
 * 原子写 journal（条目为空 ⇒ 删除文件，恢复"跑完/没跑过"的正常状态）。
 * ★ 调用点必须保证：**写盘成功之后**才去动被变异的文件（预写日志的意义就在这里）。
 */
export function writeMutationJournal(journalPath, entries) {
  if (!entries.length) {
    fs.rmSync(journalPath, { force: true });
    return;
  }
  atomicWriteFile(
    journalPath,
    `${JSON.stringify(
      { version: 1, tool: 'tools/mutate-check.mjs', pid: process.pid, updatedAt: new Date().toISOString(), entries },
      null,
      2,
    )}\n`,
  );
}

/**
 * 判定一个条目与磁盘现状的关系（**只读，不写**）。
 * @returns {{state:'before'} | {state:'mutated'} | {state:'conflict', why:string}}
 *   - `before`  ⇒ 无需恢复（也许上次已还原、没来得及删 journal）；
 *   - `mutated` ⇒ 这就是上次没复原的变异态，要写回 `before`；
 *   - `conflict`⇒ **第三种内容**（人改过 / 文件没了）⇒ 不能安全恢复。
 */
export function classifyJournalEntry(root, entry) {
  const abs = path.join(root, entry.file);
  let cur;
  try {
    cur = fs.readFileSync(abs, 'utf8');
  } catch (err) {
    return { state: 'conflict', why: err.code === 'ENOENT' ? '文件不存在' : `读不了（${err.message}）` };
  }
  if (cur === entry.before) return { state: 'before' };
  if (cur === entry.mutated) return { state: 'mutated' };
  return { state: 'conflict', why: '内容既不是 before 也不是 mutated（可能被别人改过）' };
}

/**
 * **启动自愈**：发现上次未复原的变异就恢复；判不了就拒绝运行（一个字节也不写）。
 *
 * 判据（按顺序）：
 * 1. 没有 journal ⇒ 幂等无操作（`{ok:true, existed:false}`）；
 * 2. journal 读不了 / 非法 / 形态不对 ⇒ **拒绝运行**（`reason:'invalid'`，journal 保留）；
 * 3. 先对**全部**条目做判定：任一条是第三种内容 ⇒ **拒绝运行**（`reason:'conflict'`，
 *    **不写任何文件**、journal 保留）—— 部分恢复比不恢复更危险；
 * 4. 全部可判 ⇒ 逐条写回 `before`；有 CTest 守卫信息的条目再**重建构建目录**
 *    （源文件还原 ≠ exe 还原）。重建失败 ⇒ journal 保留、`reason:'rebuild-failed'`；
 * 5. 全部成功 ⇒ 删除 journal，打印 `已从上次中断恢复 N 个文件`。
 *
 * @param {object} opts
 * @param {string} [opts.root]         仓库根（测试用临时目录）
 * @param {string} [opts.journalPath]  journal 路径（缺省 `resolveJournalPath(root)`）
 * @param {(line:string)=>void} [opts.log] 报告出口（缺省 stderr）
 * @param {typeof buildCtestTargets} [opts.build] 构建入口（测试注入假实现，避免真跑 cmake）
 */
export function recoverMutationJournal({
  root = ROOT,
  journalPath = resolveJournalPath(root),
  log = (line) => console.error(line),
  build = buildCtestTargets,
} = {}) {
  const loaded = readMutationJournal(journalPath, root);
  if (!loaded.ok) {
    log(`✗ 拒绝运行：${loaded.why}（journal：${journalPath}，已保留）。`);
    return { ok: false, reason: 'invalid', restored: 0, journalKept: true, why: loaded.why };
  }
  if (!loaded.exists) return { ok: true, existed: false, restored: 0, rebuilt: 0 };

  // ① 全部判定完再动手：任一条判不了 ⇒ 一个字节也不写
  const plan = [];
  const conflicts = [];
  for (const entry of loaded.entries) {
    const c = classifyJournalEntry(root, entry);
    if (c.state === 'conflict') conflicts.push({ file: entry.file, why: c.why });
    else plan.push({ entry, state: c.state });
  }
  if (conflicts.length) {
    log(
      `✗ 工作树内容与 journal 不符，不能安全恢复（journal 保留：${journalPath}）：\n` +
        conflicts.map((c) => `  - ${c.file}：${c.why}`).join('\n'),
    );
    return { ok: false, reason: 'conflict', restored: 0, journalKept: true, conflicts };
  }

  // ② 只写回"盘上确实是变异文"的那些（`before` 的属于"已还原、没来得及删 journal"）
  let restored = 0;
  for (const { entry, state } of plan) {
    if (state !== 'mutated') continue;
    fs.writeFileSync(path.join(root, entry.file), entry.before, 'utf8');
    restored += 1;
  }

  // ③ CTest 守卫：源文件还原了，exe 里可能还是变异版 ⇒ 按条目重建（去重）
  //   ★ **全量**（⛔ 不按 `buildTarget`）：journal 里没有 buildTarget 的信息，而且"自愈"的语义是
  //     "把整棵树撤干净" —— 只重建守卫那个 target 会把别的 exe 留在变异版（静默错）。
  const buildDirs = [...new Set(loaded.entries.filter((e) => e.ctest).map((e) => e.buildDir ?? CTEST_BUILD_DIR))];
  for (const dir of buildDirs) {
    const r = build(null, dir);
    if (r?.error || r?.status !== 0) {
      log(
        `✗ 恢复后重建失败（${dir}，${r?.error ? r.error.message : `退出码 ${r?.status}`}）` +
          '：exe 可能仍是变异版 ⇒ journal 保留，下次启动会再试。',
      );
      return { ok: false, reason: 'rebuild-failed', restored, rebuilt: 0, journalKept: true, buildDir: dir };
    }
  }

  fs.rmSync(journalPath, { force: true });
  log(`已从上次中断恢复 ${restored} 个文件${buildDirs.length ? `，并重建了 ${buildDirs.length} 个构建目录` : ''}。`);
  return { ok: true, existed: true, restored, rebuilt: buildDirs.length };
}

/* ─────────────────── 运行期：journal 维护 + 信号恢复（SIGINT / SIGTERM） ─────────────────── */

/** 当前已施加、尚未（确认）还原的条目 —— 信号恢复读它 */
let appliedEntries = [];
/** 当前这次运行的 journal 路径（信号恢复用它清理） */
let activeJournalPath = null;
/** 只安装一次，避免重复 main() 时堆监听器 */
let signalGuardsInstalled = false;
/** 防止两个信号重入恢复 */
let restoringFromSignal = false;

/** 把 `entry` 记进 journal（★ 必须在动文件**之前**调用） */
function journalAdd(journalPath, entries, entry) {
  entries.push(entry);
  writeMutationJournal(journalPath, entries);
  appliedEntries.push(entry);
}

/** 还原并比对通过之后，把条目从 journal 移除（清空时删除文件） */
function journalDrop(journalPath, entries, entry) {
  const i = entries.indexOf(entry);
  if (i >= 0) entries.splice(i, 1);
  appliedEntries = appliedEntries.filter((e) => e !== entry);
  writeMutationJournal(journalPath, entries);
}

/**
 * 信号恢复：把当前已施加的变异**同步**写回原文（只在盘上仍是变异文时写，⛔ 不覆盖别人的改动），
 * CTest 条目重建构建目录，然后删 journal 并退出。
 * ★ `SIGKILL` / 断电走不到这里 —— 那一档由下次启动的 `recoverMutationJournal` 兜底。
 */
function restoreOnSignal(signal) {
  if (restoringFromSignal) return;
  restoringFromSignal = true;
  const code = signal === 'SIGINT' ? 130 : 143;
  const n = appliedEntries.length;
  // ★ **先收在飞的 wrapper 树，再写回 / 重建**（复审裁决的顺序）：改 async 之后信号真的会在 `await`
  //   期间派发（旧的 `Atomics.wait` 把它挡住 ⇒ 这条路径从没被走到过）。留到重建之后才杀，Windows 上
  //   就是"边重建边跑 ctest"⇒ 构建目录里的 exe 还被占着（那正是 `buildCtestTargets` 重试要兜的抖动）。
  if (activeTimedPid) {
    killProcessTree(activeTimedPid);
    activeTimedPid = null;
  }
  try {
    for (const entry of appliedEntries) {
      const abs = path.join(ROOT, entry.file);
      let cur = null;
      try {
        cur = fs.readFileSync(abs, 'utf8');
      } catch {
        /* 文件没了 ⇒ 不写（交给了 journal 的下次自愈） */
      }
      if (cur === entry.mutated) fs.writeFileSync(abs, entry.before, 'utf8');
    }
    const dirs = [...new Set(appliedEntries.filter((e) => e.ctest).map((e) => e.buildDir ?? CTEST_BUILD_DIR))];
    // ★ **全量**（⛔ 不按 `buildTarget`）：这里不知道"当前这条有没有 buildTarget"，而且信号路径的
    //   语义是"把我的改动彻底撤干净" ⇒ 必须让每个 exe 都回到基线。
    for (const dir of dirs) buildCtestTargets(null, dir);
    if (activeJournalPath) fs.rmSync(activeJournalPath, { force: true });
    appliedEntries = [];
    console.error(`\n✗ 收到 ${signal}：已把 ${n} 个已施加的变异写回原文并退出（journal 已清理）。`);
  } catch (err) {
    console.error(
      `\n✗ 收到 ${signal}：同步恢复失败（${err.message}）—— journal 保留在 ${activeJournalPath}，下次启动会自愈。`,
    );
  }
  process.exit(code);
}

function installSignalGuards(journalPath) {
  activeJournalPath = journalPath;
  if (signalGuardsInstalled) return;
  signalGuardsInstalled = true;
  process.on('SIGINT', () => restoreOnSignal('SIGINT'));
  process.on('SIGTERM', () => restoreOnSignal('SIGTERM'));
}

export const DOMAIN = {
  id: 'mutate',
  title: '守卫自检（变异测试）：改坏一处关键常量 ⇒ 确认指定守卫**当场红**',
  data: [
    '**工作树里被守卫覆盖的源文件**（`packages/age-format/src/asm/*.mts` · `apps/emulator/src/model/*.ts` · `apps/emulator-new/src/**` · `apps/emulator-cxx/src/core/*.cpp` · `apps/emulator-lift/src/**`）—— ★ **临时改写后必还原**',
    '`tools/mutate-check.mjs` 里的**变异清单**（本工具的真源：一处破坏 + 该抓它的守卫）',
    '**CTest 类条目**（`guard: \'ctest:<名字>\'`）会重建 + 跑那一条 CTest（还原后也重建成基线）；★ 看条目的 `buildDir` 决定构建哪个工程（缺省 `apps/emulator-cxx/out`，emulator-lift 条目显式写 `apps/emulator-lift/out`）；★ 单测超时缺省按守卫名分层（普通 120 s；名字含独立段 `assets` / `external` 600 s；`--ctest-timeout <秒>` 覆盖）⇒ 超时 = **不可判定（超时）**、收进程树、继续下一条，⛔ 不是"守卫红了"；★ 条目带 `optInEnv`（如 `LIFT_STRESS`）= 该 CTest 是**配置期 opt-in**（注册在 `if(... AND DEFINED ENV{…})` 里）⇒ 没设该 env 时记「不可判定（本机未启用）」、⛔ 不混进「⛔ 守卫不存在」（注册被删）',
    '**预写日志** `.tmp/mutate-check/journal.json`（gitignored；动文件前原子落盘，逐条还原后移除，全清时删除）—— 中途被 SIGKILL / 断电打断后，下次启动靠它自愈或拒绝运行',
  ],
  access: 'r（读原文进内存）→ **w journal（预写日志，原子：临时文件 + rename）**→ rw（写入变异）→ **w（还原，且比对全文）**；CTest 类条目额外重建该条目的 `buildDir`（缺省 `apps/emulator-cxx/out`；生成物，已 gitignore）；★ 启动时先自愈 journal（判不了就拒绝运行，退出码 3，journal 保留）；★ 不碰旧仓',
  tool: 'tools/mutate-check.mjs',
};

export const OPERATIONS = [
  { name: 'check', argv: [], mutates: true, summary: '★ 逐条施加变异 ⇒ 跑指定守卫（`node --test` 用例，或 `ctest:<名字>` 的 CTest）⇒ 要求退出码非 0，最后**逐条还原并比对全文**（会同步恢复 SIGINT/SIGTERM；SIGKILL 后下次启动靠 journal 自愈）；★ CTest 带**单测超时**：超时 = 不可判定（超时）、收掉进程树、继续下一条（退出码 5），⛔ 不记成"守卫红了"' },
  { name: 'list', argv: ['--list'], mutates: false, summary: '只列变异清单（不动任何变异文件；但**启动时仍会先自愈 journal** —— 上次留下的变异态不该因为"只看清单"而继续存在）；★ 顺带做**静态存在性检查**：`file` 存在 + `from` 是该文件内容的子串 + `buildTarget` 等于 `add_test(NAME … COMMAND …)` 推出的可执行 target + 该 CTest spawn 的产品 exe（`$<TARGET_FILE:X>`，X ≠ buildTarget）有 `add_dependencies(buildTarget X)`（不构建、不施加变异），失配 ⇒ 非 0 退出' },
];

export { MUTATIONS };

/**
 * 变异清单：每条 = 一处"已知的破坏" + 应该抓住它的守卫。
 * ★ 口径（`AGENTS.md` §10 / `docs/03-emulator/README.md` §0.1）：**每个守卫留一条代表**。
 *   条目的职责是证明"这个守卫**会**红"（不然它会烂成恒真断言），⛔ **不是**给每个缺陷 / 每个常量
 *   各配一条 —— 那样清单会随修 bug 的轮次线性膨胀，而每条都要花一次构建 + 一次实跑来"自证"。
 *   新增守卫时补一条代表；修好一个缺陷时**不要**顺手再补一条（那条缺陷的历史进 `defects.md`）。
 * ★ CTest 条目**顺手填 `buildTarget`**（= 该用例 `add_test(NAME <x> COMMAND <y> …)` 的可执行 target，
 *   ⛔ 常常 ≠ CTest 名，例：`aot-window-profile` ⇒ `lift-aot-window-profile`）：值错了
 *   `cmake --build --target` 会失败 ⇒ 那条变异**永远记「不可判定（构建失败）」** ⇒ 它再也不会
 *   证明"守卫会红"。⇒ `--list` 的静态检查逐条比对这张映射（`ctestCommandTarget` / `checkBuildTarget`）。
 */
const MUTATIONS = [
  // ── 本机私有清单覆盖（平台相关路径）──
  {
    file: 'tools/lib/manifest.mjs',
    from: '  if (local) manifest.roots = { ...(manifest.roots ?? {}), ...local.roots };',
    to: '  if (local) void local;',
    guard: 'tools/test/manifest-local.test.mjs',
    what: '本机私有覆盖被静默忽略 ⇒ 换一台机器（win32 ↔ macOS）所有来源根都指向另一台机器的路径',
  },

  // ── 清单写入口的闸门：「不新增失败」（★ 改回"零失败"或让差集恒空 ⇒ 缺件机器上写不进去 / 什么都能写进去）──
  {
    file: 'tools/lib/manifest.mjs',
    from: '  return failureKeys(afterReport).filter((k) => !before.has(k));',
    to: '  return [];',
    guard: 'tools/test/corpus-manifest.test.mjs',
    what: '写入口的差集恒空 ⇒ **新写入自己引出来的失败也不再挡**（清单变成"写什么是什么"；`REQ-01M52F9K25YNPJEXQ74QE4CT9Z` 的判据 1 另一半）',
  },

  // ── 台账锚点的 PE 映射（★ 偏移整体少 0x1000 ⇒ 观察静默落到**前一个函数的体内**，而"读到字节"看起来正常）──
  {
    file: 'tools/lib/ledger.mjs',
    from: '      return { rva, imageBase, section: name, offset: rawPtr + (rva - va) };',
    to: '      return { rva, imageBase, section: name, offset: rawPtr + (rva - va) - 0x1000 };',
    guard: 'tools/test/ledger.test.mjs',
    what: 'EA → 文件偏移整体少一页 0x1000（= 那次误判里"工具打 0x95a20"那个数）⇒ 段内函数起点的字节不再是 `.lst` 里那条指令的编码',
  },

  // ── 台账摘要棘轮（★ 模拟"只给 len 就算齐"⇒ 缺摘要的观察可以静默进台账，而 sha256 是唯一证据）──
  {
    file: 'tools/lib/ledger.mjs',
    from: '      if (a.len === undefined || a.sha256 === undefined) {',
    to: '      if (a.len === undefined) {',
    guard: 'tools/test/ledger-reference-bin-digest.test.mjs',
    what: '缺摘要的判据从"`len` 或 `sha256` 缺一即算缺"缩成"只缺 `len` 才算缺" ⇒ 只给 `len` 的 `reference` `bin` 锚被当成齐的（写路径与门禁同时失守：换了二进制也验不出来）',
  },


  // ── wait-subagent 插件：`any_of` 必须如实交回“谁还在跑” ──
  //   ★ 把 `pending` 抹成 `[]` ⇒ 模型以为“都结束了”，于是不再等剩下的 completion notice。
  {
    file: 'plugins/wait-subagent/index.mjs',
    from: 'pending: targets.map((target) => target.childId),',
    to: 'pending: [],',
    guard: 'plugins/wait-subagent/test/plugin.test.mjs',
    what: 'any_of 的 already_finished 不再列出仍在跑的 child ⇒ “还有人没结束”被当成“都结束了”',
  },

  // ── wait-subagent 插件：同一同步边上“人消息优先于 end” ──
  //   ★ 拿掉这条 ⇒ 人正在等的那条插话被 end 吞掉（等待返回“finished”，而人消息没人看）。
  {
    file: 'plugins/wait-subagent/lib/wait.mjs',
    from:
      '      const pending = pendingHumanMessages(agent);\n' +
      '      if (pending.length > 0) {\n' +
      '        interjected(pending[0]);\n' +
      '        return;\n' +
      '      }',
    to:
      '      const pending = pendingHumanMessages(agent);\n' +
      '      if (false && pending.length > 0) {\n' +
      '        interjected(pending[0]);\n' +
      '        return;\n' +
      '      }',
    guard: 'plugins/wait-subagent/test/wait.test.mjs',
    what: 'end 与人消息同一同步边到达时不再优先报人插话 ⇒ 人正在等的那条消息被吞掉',
  },

  // ── fixtures：**一个文件只能有一个身份**（槽载荷 vs 系统容器）──
  //   ★ 去掉它 ⇒ 同一份 `SAVE76.DAT` 能同时登记成槽载荷与 `containers[]`，
  //     两条不变量会互相打架，而 `--list` / 守卫都看不出来。
  {
    file: 'tools/lib/samples.mjs',
    from: '    if (payloadNames.has(n)) bad.push(`containers 的 ${n} 与槽载荷重名（一个文件只能有一个身份）`);',
    to: "    if (false) bad.push('x');",
    guard: 'tools/test/fixtures-cli-write.test.mjs',
    what: '去掉"一个文件只能有一个身份" ⇒ 同一份文件能同时登记成槽载荷与系统容器',
  },

  // ── spec 证据分级：CTest 的档位看**注册位置**（顶层 = pure，块里 = assets）──
  //   ★ 把它退化成"一律 pure" ⇒ 装机流程里的 CTest（`if(EMULATOR_CXX_INSTALL_DIR)` 里注册的那些）
  //     会被当成"默认门禁就会跑"，于是**默认门禁根本不跑的用例**撑起了 reviewed。
  {
    file: 'tools/lib/ledger.mjs',
    from: "    if (hit) return depth > 0 ? 'assets' : 'pure';",
    to: "    if (hit) return 'pure';",
    guard: 'tools/test/spec.test.mjs',
    what: 'CTest 的档位退化成"一律 pure" ⇒ 装机流程里的用例被当成默认门禁会跑，撑起了 reviewed',
  },

  {
    // ★ 静态足迹命中 ⇒ **判不了**（数据依赖分支），⛔ 不是"路径外"。
    //   写反会让"静态出现过、这次没执行"的残差被误判成"本次不涉及"（最会把缺陷筛掉的那种错）。
    file: 'tools/lib/residual-footprint.mjs',
    from: "        if (statics.length > 0) return { scope: 'unknown', by: 'op-static', triggers: statics };",
    to: "        if (statics.length > 0) return { scope: 'offpath', by: 'op-static', triggers: statics };  // 变异：静态命中也算路径外",
    guard: 'tools/test/spec-residual-footprint.test.mjs',
    what:
      '把"静态足迹命中也算路径外"写反 ⇒ 只在脚本里出现、这次没执行的 opcode 残差从「判不了」掉进「路径外」' +
      '（缺陷会被当成"本次不涉及"）⇒ 足迹守卫当场红',
  },


  // ── 需求台账：写路径"报的落盘"必须与磁盘一致 ──
  //   ★ 写后守卫拒回时**仍按"已落盘"打印计划** ⇒ stdout 报成功、盘上什么都没有：
  //     这是"写路径静默说谎"，读 stdout 的人会把单子丢掉（同一张单实测被丢过两次）。
  {
    file: 'tools/requirements.mjs',
    from: '  if (res?.rollback) {',
    to: '  if (false) {',
    guard: 'tools/test/requirements-add-lands.test.mjs',
    what: '被写后守卫拒回时不再拒绝（继续按"已落盘"打印计划）⇒ stdout 报成功而盘上什么都没有',
  },

  // ── 发布链：AGF 缺省来源必须还是"入库件"（★ 别退回"每次重烧"）──
  //   ★ 退回 `dist/ui-bake` ⇒ 进包的字节变成"这次烧出来的"（字节等价不再成立），且默认路径绑在 headless Chrome 上。
  {
    file: 'tools/lib/release.mjs',
    from: '    agfDir: opts.baked ?? opts.agfDir ?? DEFAULT_AGF_DIR,',
    to: '    agfDir: opts.baked ?? opts.agfDir ?? DEFAULT_BAKED_DIR,',
    guard: 'tools/test/release-agf-source.test.mjs',
    what: '发布链的 AGF 缺省来源退回 `dist/ui-bake`（每次重烧）⇒ 又要 headless Chrome，且发出去的字节变成"这次烧出来的"',
  },

  // ── 覆盖清单的**逐字节**闸门（`REQ-01M7A5TZ91V3MRNEFDHWKN20MY`）──
  //   ★ 唯一写入口 = `lift sync --write`；手改一个字符（这里去掉 `_doc` 前的一个缩进空格）必须
  //     被 `lift check` / `tools/test/lift.test.mjs` 的逐字节断言抓住。选缩进而不是字面内容的理由：
  //     **字段级**断言（reason 逐字 / kind / schema）全都看不见它 —— 只有字节比较能红。
  //   ★ 守卫用纯档（不读 PE）：它用「C++ 注册表 + 派生缺口文件」重算 `renderOverrides(...)`。
  {
    file: 'data/lift/overrides.json',
    from: '{\n  "_doc": "由 tools/lift.mjs 拥有',
    to: '{\n "_doc": "由 tools/lift.mjs 拥有',
    guard: 'tools/test/lift.test.mjs',
    what:
      '手改 overrides.json 一个字符（`_doc` 前少一个缩进空格）⇒ 不再是 `lift sync --write` 的逐字节输出 ' +
      '⇒ tools/test/lift.test.mjs 的逐字节断言红（字段级断言看不见这种漂移）',
  },

  // ── emulator-lift（★ 新命令类型：`buildDir` 指到它自己的 out/；判据在它自己的 CTest 里）──

  {
    file: 'apps/emulator-lift/src/runtime/shims.cpp',
    // ★ 2026-11-10 重定向（REQ-01M70KGDXSKDVK7MCJYFABXERH）：`shims.cpp` 割出去之后局部变量
    //   从 `def` 改成了 `local`（`const ShimDef local = …` 的栈上拷贝）⇒ 原 `from` 在现源码里 0 次。
    //   施加点仍是**同一处**（弹参字节数的唯一算法），守卫仍是 `shim-abi` 的 esp 平衡断言。
    from: '    call.stdcall_bytes = (local.conv == Conv::Stdcall) ? static_cast<std::uint32_t>(local.argc) * 4u : 0u;',
    to: '    call.stdcall_bytes = (local.conv == Conv::Stdcall) ? 0u : 0u;',
    guard: 'ctest:shim-abi',
    ctest: 'shim-abi',
    buildDir: 'apps/emulator-lift/out',
    buildTarget: 'lift-shim-abi',
    what: 'stdcall 不再弹参 ⇒ 栈每次调用涨 argc*4 ⇒ shim-abi 的 esp 平衡断言红',
  },


  {
    // ★ 代表"把续延桩 EA 写成字面量"这一类错（AOT 臂 `EnumSystemLocalesA` 的"续延桩不可调用"）：
    //   铺进 guest 栈的 EA 必须是**注册表发过号的桩 EA**（`ShimDef::stub_ea`，形状 = 桩页基 + 槽号*4）。
    //   写回字面量 `0x7FFEF000`（它在桩页 `[0x7FFE0000, 0x7FFE8000)` 之外）⇒ 解释器仍靠块缓存跑得通，
    //   而 AOT 臂没有块缓存、桩表又查不到它 ⇒ `locale_ext.cpp` 的 `enum-locales-no-cache` 当场红。
    file: 'apps/emulator-lift/src/shims/kernel32_string.cpp',
    from: '    const std::uint32_t cont_ea = cont != nullptr ? cont->stub_ea : 0u;',
    to: '    const std::uint32_t cont_ea = kCallbackContinuationEa;  // 变异：字面量（桩页外）',
    guard: 'ctest:locale-ext',
    ctest: 'locale-ext',
    buildDir: 'apps/emulator-lift/out',
    buildTarget: 'lift-locale-ext',
    what:
      '续延桩 EA 改回字面量 0x7FFEF000（在桩页之外）⇒ 无块缓存时桩表路由不到它 ⇒ locale-ext 的 ' +
      'enum-locales-no-cache（`cpu.cache == nullptr`）当场红；★ 解释器臂不会红（块缓存能装任意 EA）',
  },

  // ── 控制层端点（S0 传输 + S1 闸门/时钟；守卫 = `tests/control_endpoint.cpp`）──
  //   ★ 两条各代表一类"静默失效"：
  //     ① 暂停**不冻结 guest 时钟** ⇒ 暂停 2 s 后 guest 的 `timeGetTime` 跳 2000 ms
  //        （症状是超时/动画/帧率自适应随暂停漂 —— 不崩、不报错，只是读数不再可比）；
  //     ② 握手**不拒 `Origin`** ⇒ 任何网页都能连本机端点（安全边界，静默失守）。
  {
    file: 'apps/emulator-lift/src/runtime/control_ws.cpp',
    from: '                if (ctx.clock != nullptr) ctx.clock->freeze();  // ★ 冻结区间对 guest 不可见',
    to: '                (void)0;  // 变异：暂停不冻结 guest 时钟',
    guard: 'ctest:lift-control-endpoint',
    ctest: 'lift-control-endpoint',
    buildDir: 'apps/emulator-lift/out',
    buildTarget: 'lift-control-endpoint',
    what:
      '暂停不冻结 guest 时钟 ⇒ 暂停 2.1 s 后 `guest_ms` 差 ≈2100 ms（判据 6 的"暂停对 guest 不可见"）' +
      '⇒ lift-control-endpoint 的 [judge6] delta < 5 断言红',
  },

  {
    file: 'apps/emulator-lift/src/runtime/control_ws.cpp',
    from: '    if (req.find(http::field::origin) != req.end()) {',
    to: '    if (false) {  // 变异：不拒 Origin',
    guard: 'ctest:lift-control-endpoint',
    ctest: 'lift-control-endpoint',
    buildDir: 'apps/emulator-lift/out',
    buildTarget: 'lift-control-endpoint',
    what:
      '带 Origin 头的握手不再被拒 ⇒ 浏览器里的任意网页都能连本机控制端点（判据 10 后半）' +
      '⇒ lift-control-endpoint 的 origin status == 403 断言红',
  },

  {
    // ★ S4（判据 13）代表 "resume 把暂停期间的 OS 输入当**刚发生**" 这一类错：
    //   暂停 = 完全停（不动 SDL）⇒ 10 s 的点击/拖动/关窗全排在队列里；resume 首轮排空时必须
    //   按 `event.common.timestamp` 丢掉"早于恢复时刻"的 OS 输入，并把暂停期间的关窗 / ESC
    //   当**宿主 stop**（⛔ 投 WM_CLOSE 进 guest ⇒ 这一轮被"用户暂停时点了下窗口"结束掉）。
    //   变异 = 分类器永远 Keep（阈值比较消失）⇒ `tests/present_window.cpp` 的三条断言红。
    file: 'apps/emulator-lift/src/runtime/present.h',
    from:
      'inline StaleEventAction classifyStaleEvent(HostEventClass cls, std::uint64_t timestamp_ns,\n' +
      '                                           std::uint64_t resume_ns) {\n' +
      '    if (resume_ns == 0 || timestamp_ns == 0 || timestamp_ns >= resume_ns) return StaleEventAction::Keep;',
    to:
      'inline StaleEventAction classifyStaleEvent(HostEventClass cls, std::uint64_t timestamp_ns,\n' +
      '                                           std::uint64_t resume_ns) {\n' +
      '    (void)cls; (void)timestamp_ns; (void)resume_ns;  // 变异：不按时间戳过滤\n' +
      '    return StaleEventAction::Keep;',
    guard: 'ctest:present-window',
    ctest: 'present-window',
    buildDir: 'apps/emulator-lift/out',
    buildTarget: 'lift-present-window',
    what:
      'resume 不再丢弃"暂停期间产生的 OS 输入"⇒ 陈旧 motion/button 被当成刚发生（present-window 的 ' +
      'queue==0 / buttons==0 断言红），陈旧关窗也不再当宿主 stop（stopRequested/controlStopReason 断言红）',
  },

  {
    // ★ 同一类错（判据 13）的**另一半**：上面那条改的是"分类器失效"，这条改的是"**根本没 arm**"。
    //   现场：`Control::serve()` 的 Resume 分支漏了通知宿主 ⇒ `stale_input_before_ns` 恒 0 ⇒
    //   纯映射再对也永远走不到（缺陷 `REQ-01M7JFDZK1SZW72SBAW6BC3DRT`）。⛔ 两条都必须留：
    //   只留分类器那条时，接线掉了 `present-window` 照样全绿（它自己直接调 `noteControlResume`）。
    file: 'apps/emulator-lift/src/runtime/control_ws.cpp',
    from: '                if (ctx.host != nullptr) ctx.host->controlOnResume();',
    to: '                (void)0;  // 变异：resume 不通知宿主（不 arm 陈旧事件闸门）',
    guard: 'ctest:lift-control-endpoint',
    ctest: 'lift-control-endpoint',
    buildDir: 'apps/emulator-lift/out',
    buildTarget: 'lift-control-endpoint',
    what:
      'resume 不再 arm "恢复时刻" ⇒ 生产路径上陈旧事件闸门是死代码（`control-resume-arms-host-hook` 的 ' +
      '`controlOnResume` 计数 3 → 0 断言红）',
  },

  // ── AOT 臂的令牌 arm / disarm 与摘要读数（`REQ-01M7JFEA7J8AWQHPXTW6GB41H0`）──
  //   ★ 两条各钉一类**静默**失效：
  //     ① 删 `arm(...)` ⇒ `phase0Enabled()` 恒 false ⇒ 真跑用例的「恰好一行 `token     :`」落空；
  //     ② 删 `disarm()` ⇒ 收口后仍记账（进程退出前的十几毫秒里**不可观测**）⇒ 只有**形态**能钉住；
  //        ⛔ 别把这条说成运行期守卫（`tools/test/aot-token-shape.test.mjs` 的 @why 已标明）。
  {
    file: 'apps/emulator-lift/src/aot/aot_run.cpp',
    from: '    guestToken().arm(opts.profile);',
    to: '    // [变异] 删掉 AOT 的 arm 调用（phase0 关着）',
    guard: 'ctest:aot-token-summary',
    ctest: 'aot-token-summary',
    buildDir: 'apps/emulator-lift/out',
    buildTarget: 'lift-aot-token-summary',
    what:
      'AOT 不再 `guestToken().arm(...)` ⇒ 违规计数整轮关着、摘要的 `token     :` 行不再出现 ⇒ ' +
      '`aot-token-summary` 的 profile 档（恰好一行 + 与解释器同形）当场红',
  },
  {
    file: 'apps/emulator-lift/src/aot/aot_run.cpp',
    from: '    guestToken().disarm();',
    to: '    // [变异] 删掉 AOT 的 disarm 调用（收口后仍记账）',
    guard: 'tools/test/aot-token-shape.test.mjs',
    what:
      'AOT 收口不再 `disarm()` ⇒ 之后的宿主析构 / 收尾仍记进本轮账（运行期不可观测）⇒ ' +
      '`aot-token-shape` 的 arm/disarm 成对断言当场红（★ 形态守卫，证明力弱）',
  },

  // ── 窗口 profile（`REQ-01M7JNX5MR7RWTBNBAE70PBCQP`；守卫 = `tests/aot_window_profile.cpp`）──
  //   ★ 代表"闭式里漏了一个桶"这一类静默失效：把 `shim` 从归一用的包络里去掉 ⇒
  //     归一后 `lift+boundary` 把 shim 那一份吞掉 ⇒ 表里各行百分比互相矛盾。
  //     ★ 变异落在 **`window_prof.cpp`**（闭式的唯一实现）而不是头：头被 `aot_host.h` 包含
  //     ⇒ 头里变异会重编整条 AOT 臂（实测一条 12 分钟）。搬成 TU 后这里只重编 1 个 TU。
  {
    file: 'apps/emulator-lift/src/aot/window_prof.cpp',
    from: '    double envelope = t.wall - t.shim - t.partial - t.instrument;',
    to: '    double envelope = t.wall - t.partial - t.instrument;  // 变异：漏掉 shim',
    guard: 'ctest:aot-window-profile',
    ctest: 'aot-window-profile',
    buildDir: 'apps/emulator-lift/out',
    // ★ 只重建守卫的 target（**CMake target 名带 `lift-` 前缀**，CTest 名没有）：
    //   runtime 是静态库 ⇒ 不带它的全量构建会把 15 MB 的 `emulator-lift-aot.exe` 重链两遍。
    //   安全性由「一轮结束的全量重建回基线」兜底（见 `finalizeBuildDirs`）。
    buildTarget: 'lift-aot-window-profile',
    what:
      '窗口的精确包络漏掉 `shim` ⇒ 归一后 `lift+boundary` 把 shim 那一份算进自己 ⇒ ' +
      '闭式（wall = lift+boundary+shim+partial+instrument）当场不成立',
  },

  // ── `--scale <auto|数值>` 的窗口尺寸公式（`src/runtime/window_scale.cpp`；守卫 = CTest `window-scale`）──
  //   故障形态：`round(1280×s)` / `round(720×s)` 退化成**截断**。这是最像"能跑"的破坏 ——
  //   1.5 / 1.25 这些乘积本来就是整数的档位**完全不变**（1280×1.5、720×1.5、720×1.25 都是整），
  //   只有 1.33 这类非整数倍率的客户区各少 1 px；画面照显、鼠标只有零点几像素偏移。
  //   ⇒ 判据必须是**公式**（`window-scale/fixed-sizes` 的 1.33 ⇒ 1702×958），
  //     ⛔ 不是"窗口建出来了"（那在截断档上照样成立，会假绿）。
  {
    file: 'apps/emulator-lift/src/runtime/window_scale.cpp',
    from: '    const double v = roundHalfAway(static_cast<double>(base) * scale);',
    to:
      '    const double v = static_cast<double>(static_cast<int>(static_cast<double>(base) * scale));  // 变异：截断',
    guard: 'ctest:window-scale',
    ctest: 'window-scale',
    buildDir: 'apps/emulator-lift/out',
    // ★ 只重建守卫的 target（runtime 是静态库 ⇒ 全量构建会把 15 MB 的 AOT 产品臂也重链，
    //   这对本判据没用）；安全性由「一轮结束的全量重建回基线」兜底（`finalizeBuildDirs`）。
    buildTarget: 'lift-window-scale',
    what:
      '窗口尺寸从四舍五入退化成截断 ⇒ 1.33 倍时客户区 1702×957（应 1702×958）⇒ ' +
      '`window-scale/fixed-sizes` 的 1.33 用例红；★ 1.5 / 1.25 的乘积是整数 ⇒ 只钉"窗口建出来了"会假绿',
  },

  // ── 🐞S2 `emu-ctl screenshot` 全黑（`REQ-01M7JAGEKMYWWQPMWGV01Y1FNQ`；守卫 = `tests/control_snapshot.cpp`）──
  //   ★ 代表"截图读的不是最后呈现的那一帧"这一类静默失效：
  //     present **不冻结** ⇒ `dumpLastFrame` 退回写现场后缓冲（下一帧的 Clear(0) 之后 = 全黑 = 本缺陷形态）。
  {
    file: 'apps/emulator-lift/src/runtime/present.cpp',
    from: '        freezePresentedFrame(arena, surface);',
    to: '        (void)0;  // 变异：不冻结',
    guard: 'ctest:lift-control-snapshot',
    ctest: 'lift-control-snapshot',
    buildDir: 'apps/emulator-lift/out',
    buildTarget: 'lift-control-snapshot',
    what:
      'present() 不再冻结该帧像素 ⇒ `dumpLastFrame` 退回写现场（已被清成 0 的后缓冲）⇒ ' +
      'lift-control-snapshot 的 `screenshot-freezes-frame` 在 `shot == ref`（与同帧 `--dump-frame` 参照逐字节相同）' +
      '与 `hash_still_matches` 断言上红；同帧 `--dump-frame` 仍绿 ⇒ 正是本单"同一帧 dump 正常、screenshot 全黑"的形状',
  },

  {
    // ★ S3（判据 3b）代表 "快照里混进了一个会自己走的数" 这一类错：
    //   `state_seq` 的语义是**客户端可见的暂停状态转移**（进入/离开各一次）；采样⛔ 不许自增。
    //   变异 = 已暂停态收到 `sample` 也走"进入暂停"分支（freeze + ++state_seq）⇒
    //   同一暂停点连采两次的 res 文本不再逐字节相同（3b 的机制面红）。
    file: 'apps/emulator-lift/src/runtime/control_ws.cpp',
    from:
      '            case ControlOp::Sample: {\n' +
      '                // ★ `sample` = **暂停（若未停）** + 一份 POD 快照（主线程填，控制线程只序列化）。\n' +
      '                if (!paused) {\n' +
      '                    if (ctx.clock != nullptr) ctx.clock->freeze();\n' +
      '                    paused = true;\n' +
      '                    paused_at = ctx.paused_at;\n' +
      '                    ++state_seq;\n' +
      '                }',
    to:
      '            case ControlOp::Sample: {\n' +
      '                // 变异：每次采样都当"进入暂停"（state_seq 自增 + 重冻时钟）\n' +
      '                {\n' +
      '                    if (ctx.clock != nullptr) ctx.clock->freeze();\n' +
      '                    paused = true;\n' +
      '                    paused_at = ctx.paused_at;\n' +
      '                    ++state_seq;\n' +
      '                }',
    guard: 'ctest:lift-control-endpoint',
    ctest: 'lift-control-endpoint',
    buildDir: 'apps/emulator-lift/out',
    buildTarget: 'lift-control-endpoint',
    what:
      '`sample` 每次自增 state_seq ⇒ 同一暂停点连采两次的 res 文本不同（判据 3b 的机制面：' +
      '`s1.text == s2.text` 断言红）；这会让"步进/快照可复现"静默失效',
  },

  {
    // ★ S2（判据 5 / 客户端契约）代表 "`screenshot` 忘了自己暂停" 这一类错：
    //   设计 §4 的命令表写的是"**暂停 + 写最后呈现那一帧**"；只写文件、不置暂停态 ⇒
    //   客户端拿到一张**不是暂停点**的帧（`state_seq` 不动、`paused` 仍 false），
    //   而错误码/文件都正常 ⇒ 静默。
    //   变异 = 去掉 Screenshot 分支的"若未暂停则停在该汇点"块 ⇒
    //   lift-control-endpoint 的 [judge5-screenshot-pauses]（paused / state_seq+1 / paused_at）红。
    file: 'apps/emulator-lift/src/runtime/control_ws.cpp',
    from:
      '            case ControlOp::Screenshot: {\n' +
      '                // ★ 与 `sample` **同一条**"若未暂停则停在该汇点"的路径（判据 5 / 设计 §4 的命令表：\n' +
      '                //   `screenshot` = 暂停 + 写最后呈现那一帧）。`paused_at` 由**实际停住的汇点**填\n' +
      '                //   （`ctx.paused_at`：块边界 `"block"` / Present 闸门 `"present"`）。\n' +
      '                if (!paused) {\n' +
      '                    if (ctx.clock != nullptr) ctx.clock->freeze();\n' +
      '                    paused = true;\n' +
      '                    paused_at = ctx.paused_at;\n' +
      '                    ++state_seq;  // 进入暂停态严格 +1（与 pause/sample 同一口径）\n' +
      '                }',
    to:
      '            case ControlOp::Screenshot: {\n' +
      '                // 变异：screenshot 不再自己暂停（只写文件）\n',
    guard: 'ctest:lift-control-endpoint',
    ctest: 'lift-control-endpoint',
    buildDir: 'apps/emulator-lift/out',
    buildTarget: 'lift-control-endpoint',
    what:
      '`screenshot` 不再自己暂停 ⇒ 未暂停态调用后 `paused` 仍 false、`state_seq` 不动 ⇒ ' +
      'lift-control-endpoint 的 [judge5-screenshot-pauses] 断言红（客户端只能自己补发 pause）',
  },

  {
    // ★ M1（`REQ-01M7J2P5S27VX7WMRR3ZC9K2E8` 判据 3）代表 "`click` 在暂停态之外也被收下" 这一类错：
    //   合同是"**只在暂停态入队**；未暂停 ⇒ `error.code:"not_paused"`（⛔ 静默丢）"。
    //   去掉那道判定 ⇒ 运行态投的 click 也回 `ok:true` 并真的排队，客户端会以为
    //   "这一下已经按当前画面点过了"（而施加点其实在下一次暂停之后的某个读点）。
    //   守卫：`apps/emulator-lift/tests/control_endpoint.cpp#caseClickDelivery` 的
    //   `[judge3-click-not-paused]`（`code == "not_paused"` 断言当场红）。
    file: 'apps/emulator-lift/src/runtime/control_ws.cpp',
    from:
      '                // ★ 判据 3：`click` **只**在暂停态入队（未暂停 ⇒ `not_paused`；⛔ 静默丢）。\n' +
      '                if (!paused) {',
    to:
      '                // 变异：去掉 click 的 not_paused 判定（运行态也照收并排队）\n' +
      '                if (false) {',
    guard: 'ctest:lift-control-endpoint',
    ctest: 'lift-control-endpoint',
    buildDir: 'apps/emulator-lift/out',
    buildTarget: 'lift-control-endpoint',
    what:
      '`click` 不再只接受暂停态 ⇒ 未暂停时回 `ok:true`（并把 click 排进下一个读点）⇒ ' +
      'lift-control-endpoint 的 [judge3-click-not-paused] 断言红',
  },

  {
    // ★ 代表 "shim 按**别的 API 版本的签名**读参" 这一类错（`REQ-01M780VKKRFHJEX6VM74DRCYBM`）：
    //   `IDirect3DDevice9::SetRenderTarget(DWORD RenderTargetIndex, IDirect3DSurface9*)` 被当成
    //   D3D8 的 `SetRenderTarget(IDirect3DSurface8* pRenderTarget, IDirect3DSurface8* pNewZStencil)`
    //   ⇒ 第 1 个实参（索引，恒 0）被当成表面、纹理表面被当成 zstencil ⇒ 目标恒为"后缓冲"，
    //   「渲染到纹理」的绑定**静默丢弃**（不报错：流水线早退 return S_OK；引擎的错误分支也没触发）
    //   ⇒ 存档缩略图 `SAVE00.STH` 全黑。
    //   守卫：`apps/emulator-lift/tests/d3d9.cpp` 的 `d3d9/offscreen-surface-rt-data`
    //   （纹理 RT `Clear` ⇒ `GetRenderTargetData` ⇒ 锁离屏表面读到 Clear 色；绑不上就红）。
    file: 'apps/emulator-lift/src/shims/d3d9.cpp',
    from:
      '    const std::uint32_t rt_index = call.u32(1);  // 第 2 实参 = DWORD RenderTargetIndex（⛔ 不是 surface）\n' +
      '    const std::uint32_t target = call.u32(2);    // 第 3 实参 = IDirect3DSurface9* pRenderTarget',
    to: '    const std::uint32_t rt_index = 0u;\n    const std::uint32_t target = call.u32(1);',
    guard: 'ctest:d3d9',
    ctest: 'd3d9',
    buildDir: 'apps/emulator-lift/out',
    buildTarget: 'lift-d3d9',
    what:
      'SetRenderTarget 按 D3D8 的签名读参（第 1 个实参当成 pRenderTarget）⇒ 纹理 RT 的绑定退化成' +
      '"绑后缓冲"、像素仍为 0 ⇒ d3d9/offscreen-surface-rt-data 的逐像素断言当场红',
  },

  {
    // ★ 代表 "两个采样过滤被合成一档" 这一类错（REQ-01M737D3BHNMRY97T9A02DARCC；2026-11-14 放行政策翻面后）：
    //   `MAG != MIN` 的批**必须**照画、且把各自的过滤**分别**交进后端 ⇒ 把 `min_filter` 换成
    //   `mag_filter`（合成一档）⇒ 交进后端的 `GpuSampler.min_filter` 不再等于 guest 设的值，
    //   `gpu-batch/sampler-filter-eligibility` 的 (a)/(b) 两个字段断言当场红。
    //   守卫：`apps/emulator-lift/tests/gpu_batch.cpp#gpu-batch/sampler-filter-eligibility`（pure，`gpu-batch-order` 跑它）。
    file: 'apps/emulator-lift/src/runtime/d3d9_pipeline.cpp',
    from: '        ss.min_filter = samplerOf(cpu, pe, s, kSampMinFilter);',
    to: '        ss.min_filter = ss.mag_filter;  // 变异：两个过滤合成一档（min 跟着 mag）',
    guard: 'ctest:gpu-batch-order',
    ctest: 'gpu-batch-order',
    buildDir: 'apps/emulator-lift/out',
    buildTarget: 'lift-gpu-batch',
    what:
      '把 `min_filter` 覆盖成 `mag_filter`（两个过滤合成一档）⇒ 交进后端的 sampler `min_filter` 不再等于' +
      ' guest 设的值 ⇒ gpu-batch/sampler-filter-eligibility 当场红',
  },

  {
    // ★ 代表 "把**身份**判据换成**外观**判据" 这一类错（`REQ-01M780VKKRFHJEX6VM74DRCYBM` 的连带）：
    //   `setRenderTarget` 把 `SetRenderTarget(0, <GetBackBuffer 的表面>)` 规范化成隐式后缓冲，判据是
    //   **像素块同址**（`t_surf.arena == di.arena`）。换成"尺寸相同"就会把**另一张同尺寸表面**
    //   （纹理 RT —— 引擎 `0x4A778B` 那站绑的就是它）也当成后缓冲 ⇒ 往别人的像素上画。
    //   守卫：`apps/emulator-lift/tests/gpu_batch.cpp` 的
    //   `gpu-batch/render-target-backbuffer-identity` ②（同尺寸 64×64 纹理表面必须**不**被规范化、
    //   且必须仍按"目标不是默认后缓冲"回退 CPU）。
    //   ★ 这个 `to` 保留 `textureInfoOfState` 的调用（⛔ 不是为了让文本像原样）：`emulator-lift-runtime`
    //     是 `/W4 /WX`，删出一个未使用变量会让**构建**失败 ⇒ 记成"不可判定"，而不是"守卫红了"。
    file: 'apps/emulator-lift/src/runtime/d3d9_pipeline.cpp',
    from:
      '            if (surfaceInfoOfState(cpu, t_state, &t_surf, nullptr) &&\n' +
      '                deviceInfoOfState(cpu, dev, &di, nullptr) &&\n' +
      '                !textureInfoOfState(cpu, t_state, &t_tex, nullptr) && di.arena != 0 &&\n' +
      '                t_surf.arena == di.arena) {',
    to:
      '            if (surfaceInfoOfState(cpu, t_state, &t_surf, nullptr) &&\n' +
      '                deviceInfoOfState(cpu, dev, &di, nullptr) &&\n' +
      '                (textureInfoOfState(cpu, t_state, &t_tex, nullptr) || di.arena != 0) &&\n' +
      '                t_surf.width == static_cast<std::uint32_t>(di.width) &&\n' +
      '                t_surf.height == static_cast<std::uint32_t>(di.height)) {',
    guard: 'ctest:gpu-batch-order',
    ctest: 'gpu-batch-order',
    buildDir: 'apps/emulator-lift/out',
    buildTarget: 'lift-gpu-batch',
    what:
      '后缓冲规范化改用**外观**判据（尺寸相同）⇒ 同尺寸的**纹理**表面也被当成后缓冲规范化：' +
      '`color_surface` 被清成 0（往别人的像素上画）且这一批不再按"目标不是默认后缓冲"回退 ⇒ ' +
      'gpu-batch/render-target-backbuffer-identity ② 当场红',
  },

  {
    // ★ 代表 "每线程一份 softfloat 状态被退回**进程级共享**"这一类错
    //   （`REQ-01M76ZHN9V2WNCYT1ACJDJ94CW`）：`THREAD_LOCAL` 空 ⇒ `softfloat_exceptionFlags` /
    //   `softfloat_roundingMode` 是模块级变量，而 `x87TakeExceptionFlags()` 是"读 + 清"的读改写
    //   ⇒ 音频 feeder（第二条 guest 执行流）与主线程互相偷 / 串 x87 状态字的 PE 位，
    //   PCM 段的"逐位一致"不成立。判据：`apps/emulator-lift/tests/fpu_thread.cpp` ——
    //   两条线程各 20000 轮，A 的 inexact 只许出现在 A、B 的 invalid 只许在 B（取到的标志里
    //   既不许缺自己的位、也不许有对方的位；另有单线程 sanity 钉住两条臂各自真的置位）。
    //   ★ 两条线程用 `std::barrier` **逐轮强制交错**（对齐开始 / 两边都已置位 / 两边都读完再进下一轮）
    //     ⇒ 共享态下"两家的位同时在场"由同步边保证，第一个 take 必读到对方的位、第二个必读到 0
    //     ⇒ **必然红**。⛔ 不要退回"只堆轮数"：复审实测旧版本在同一变异二进制上连跑 10 次有 1 次绿。
    //   ★ 施加点用**整段三档定义**（不是只改 macOS 那档）：只把 `__thread` 改成空的话，
    //     Windows（clang-cl 走 `_MSC_VER` 档）跑这条变异会「没红」——那会被读成"守卫坏了"，
    //     而实际是"变异点在该平台根本没生效"。整段替换 ⇒ 三平台都退回 `#define THREAD_LOCAL`（空）。
    file: 'apps/emulator-lift/third_party/berkeley-softfloat/platform.h',
    from:
      '#if defined(_MSC_VER)\n#define THREAD_LOCAL __declspec(thread)\n' +
      '#elif defined(__GNUC__) || defined(__clang__)\n#define THREAD_LOCAL __thread\n' +
      '#else\n#define THREAD_LOCAL _Thread_local\n#endif',
    to: '#define THREAD_LOCAL',
    guard: 'ctest:fpu-thread-flags',
    ctest: 'fpu-thread-flags',
    buildDir: 'apps/emulator-lift/out',
    buildTarget: 'lift-fpu-thread',
    what:
      'THREAD_LOCAL 退回空（softfloat 状态进程级共享）⇒ barrier 强制交错下两条线程的异常位同时在场 ' +
      '⇒ 一个 take 读到对方的位、另一个读到 0 ⇒ fpu-thread-flags 每次必红（实测 10/10）',
  },

  {
    // ★ 代表 "给**消费者**强制前置 include 的那条 INTERFACE 选项掉了"这一类错
    //   （`REQ-01M773EFSYYE97YKEA6TQEQ53J`）。`softfloat.h:53-54` 有 `#ifndef THREAD_LOCAL ⇒ 空`
    //   的兜底 ⇒ 消费方 TU 若先 include `softfloat.h`、没先 include `platform.h`，编译 + 链接都过，
    //   运行时却把 **TLS 模板**当变量访问（库写的是每线程 TLS 块）：实测本仓消费方读回 `0`。
    //   判据：`apps/emulator-lift/tests/softfloat_tls.cpp`（**第一条 include 就是 `<softfloat.h>`**、
    //   ⛔ 不 include platform.h）：① 任何 include 之前就看得见 `THREAD_LOCAL` / `LITTLEENDIAN`；
    //   ② 库 `softfloat_raiseFlags()` 置的位在本 TU 读得到；③ 两条线程各写不同值、barrier 之后
    //   各读各的、且 `&softfloat_exceptionFlags` 不同。
    //   ★ 破坏形态 = `INTERFACE` → `PRIVATE`（= 只作用于 vendored `.c` 自己、消费者拿不到）：
    //     前置 include 消失 ⇒ 该用例 7 条断言当场红（实测：`saw platform.h = 0` / `via-library=0` /
    //     两条线程读到同一个 `0x5C` / 两个地址相同）。构建**照旧成功** —— 这正是它是"静默危险"的证明，
    //     也是这条变异必须落在 `INTERFACE`（而不是删掉整行）的原因：删整行同样只影响消费者，
    //     但 `INTERFACE`→`PRIVATE` 精确表达"这条选项还在、挂错了位置"。
    //   ★ 为什么 `from` 要**连 if/else 整段**一起替换：本机只跑 MSVC 臂，但只改那一行的话
    //     Linux/macOS 上这条变异不生效（会被读成"守卫坏了"，而实际是"变异点没落到那一臂"）。
    file: 'apps/emulator-lift/third_party/berkeley-softfloat/CMakeLists.txt',
    from:
      'if (MSVC)\n' +
      '    # clang-cl 也走这一支（CMake 对 MSVC ABI 的 clang 同样置 MSVC=1）\n' +
      '    target_compile_options(berkeley-softfloat INTERFACE /FIplatform.h)\n' +
      'else()\n' +
      '    target_compile_options(berkeley-softfloat INTERFACE -include platform.h)\n' +
      'endif()',
    to:
      'if (MSVC)\n' +
      '    # 变异：前置 include 只作用于 vendored `.c`，消费者拿不到（INTERFACE → PRIVATE）\n' +
      '    target_compile_options(berkeley-softfloat PRIVATE /FIplatform.h)\n' +
      'else()\n' +
      '    target_compile_options(berkeley-softfloat PRIVATE -include platform.h)\n' +
      'endif()',
    guard: 'ctest:softfloat-tls',
    ctest: 'softfloat-tls',
    buildDir: 'apps/emulator-lift/out',
    buildTarget: 'lift-softfloat-tls',
    what:
      '消费者拿不到前置 include（INTERFACE → PRIVATE）⇒ `tests/softfloat_tls.cpp` 那个"先 include ' +
      'softfloat.h"的 TU 退回空 THREAD_LOCAL 兜底 ⇒ softfloat-tls 红（实测 7 条断言：saw ' +
      'platform.h=0 / via-library=0 / 两线程读同一个 0x5C / 两个地址相同）',
  },

  {
    // ★ 代表 "LZSS Encode 的**平局判据**被放宽"这一类错（REQ-01M7375DARF1QFZEFNWX3CAK6T）。
    //   判据：`apps/emulator-lift/tests/native_override.cpp` 的 `lzss/tie-strict-greater` ——
    //   8 字节定点输入（BABABBAB，对象预填 0xCC）下输出必须是 `03 42 41 EE F0 EE F0`：
    //   两个候选（4078 / 4080）对最后一段等长 3，**严格 `>`** 保留先入树的 4078（offset 0xFEE）。
    //   放宽成 `>=` ⇒ match_pos 被后到者顶掉 ⇒ 末尾两个字节变 `F0 F0`。
    //   ★ 为什么挑这一条而不是 `17 → 18`：它直接钉住 `.lst` 0x4365EA `jle` 这个**语义分叉**，
    //     失败方向唯一（输出字节），不依赖任何数据结构细节。
    file: 'apps/emulator-lift/src/runtime/native_overrides.cpp',
    from: '        if (static_cast<std::int32_t>(i) > lzoLoadI32(obj, kLzoMatchLen)) {',
    to: '        if (static_cast<std::int32_t>(i) >= lzoLoadI32(obj, kLzoMatchLen)) {',
    guard: 'ctest:native-override',
    ctest: 'native-override',
    buildDir: 'apps/emulator-lift/out',
    buildTarget: 'lift-native-override',
    what:
      'InsertNode 的平局判据从严格 > 放宽成 >=（等长也替换 match_pos）⇒ 先入树的候选被顶掉：' +
      'lzss/tie-strict-greater 的定点字节 EE F0 变 F0 F0，当场红（实测见 .tmp/reveq/…/report.md）',
  },

  // ── 🗑 2026-11-13 删掉：`agerc-ui-state` ──
  //   它整条断言的是**我们 stub 的返回值**（真机语义相反，`BUG-14`）⇒ CTest 与变异条目一起删除。
  //   ⛔ 不要给它加回条目：指向已删 CTest 的条目只会被 `guardMissing` 记成"守卫不存在"。

  {
    // ★ 2026-11-13 **恢复**（原条目先随 `gdi-font-metrics-cache` 一起删过）：那条 CTest 已按
    //   `BUG-14` 删除（钉的是缓存键 / hit-miss 计数），但它的**真 GDI 合同**（换字号 / 换 bold ⇒
    //   度量必须变）搬进了 `gdi-font` 的 `metrics-size-change` / `metrics-bold-differs`。
    //   ⇒ 这处破坏**仍有守卫**：`gdi-font` 走的正是**生产** `FontEngine::metrics` 缓存
    //   （`gdiTextMetrics` → `fonts.metrics`），忽略缓存键 ⇒ 20px 的度量被复用给 28px / bold。
    //   ⛔ 旧注释里"`gdi-font` 是 assets 档、不经过缓存键"是**错的**：assets 只决定它注册与否
    //   （宿主字体文件在不在），不改变它走的代码路径。
    file: 'apps/emulator-lift/src/runtime/font.cpp',
    from: '    const auto cached = impl_->metrics_cache.find(key);',
    to:
      '    // 变异：忽略键 —— 恒返回表里第一条\n' +
      '    const auto cached = impl_->metrics_cache.empty() ? impl_->metrics_cache.end() : impl_->metrics_cache.begin();',
    guard: 'ctest:gdi-font',
    ctest: 'gdi-font',
    buildDir: 'apps/emulator-lift/out',
    buildTarget: 'lift-gdi-font',
    what:
      '`metrics()` 的缓存忽略键（恒返回第一条）⇒ 小字号的度量复用给大字号 / bold 拿到常规那份 ⇒ ' +
      'gdi-font 的 `metrics-size-change` / `metrics-bold-differs` 当场红',
  },

  // ── 基线补偿：按像素字号表（同字号统一，⛔ 不做逐字型夹取；`REQ-01M7Q9C16JR9ATFJK11A873B26`）──
  //   ① 表值退化：`{16, 1}` → `{16, 2}`（小字号又吃回旧常量）⇒ 10px 注音的目标值从 1 变 2，
  //      origin_y 会越出行框顶 1px（真字体档的读数见 `docs/03-emulator/fonts.md` §3.1）。
  //      守卫选 **pure** 的 `tests/gdi.cpp#baseline-lift-table`：表是纯函数（只读 `fontConfig()`），
  //      不需要字体就能红 ⇒ 这一条不依赖任何资产。`from` 必须与 `font.h` 的表行逐字一致。
  {
    file: 'apps/emulator-lift/src/runtime/font.h',
    from: '    {16, 1},',
    to: '    {16, 2},',
    guard: 'ctest:gdi',
    ctest: 'gdi',
    buildDir: 'apps/emulator-lift/out',
    buildTarget: 'lift-gdi',
    what:
      '按像素字号表的 baseline 目标值退化（`{16,1}` → `{16,2}`）⇒ 10px / 16px 的目标从 1 变 2 ⇒ ' +
      'gdi 的 `baseline-lift-table`（`L(10) == 1`、`L(16) == 1`）当场红',
  },

  //   ② 合成字形档偷吃补偿：`origin_y = black_box_y` 被写成 `black_box_y + 配置量` ⇒ 格式算术契约
  //      被视觉策略污染（`tests/gdi.cpp` 的 `gmptGlyphOrigin.y == 20 @20px` 是它的守卫）。
  //      `from` 必须与 `font.cpp` 的合成档那一行逐字一致。
  {
    file: 'apps/emulator-lift/src/runtime/font.cpp',
    from: '    m.origin_y = m.black_box_y;',
    to: '    m.origin_y = m.black_box_y + fontConfig().baseline_lift;',
    guard: 'ctest:gdi',
    ctest: 'gdi',
    buildDir: 'apps/emulator-lift/out',
    buildTarget: 'lift-gdi',
    what:
      '`syntheticMask` 的 origin_y 从 black_box_y 变成 black_box_y + 2（偷吃宿主补偿量）⇒ gdi 的 ' +
      'ggo-needed-and-metrics（gmptGlyphOrigin.y 必须是 20 @20px）当场红',
  },

  {
    // ★ 代表 "点/线的 GPU 展开退化成零宽" 这一类错（`REQ-01M78BF1FNNH6K8N8GEY6GQYXX`）：
    //   把 1px 宽 quad 的**半宽**写成 0 ⇒ 每秒线展开出的两个三角形都退化成线（面积 0，被退化判据丢掉）
    //   ⇒ 线上一个像素都不写。守卫：`apps/emulator-lift/tests/gpu_batch.cpp` 的
    //   `gpu-batch/lines-points`（**真后端臂**，`--gpu`；没有 SDL_GPU 设备 ⇒ SKIP 77、记「不可判定」）。
    //   ★ 为什么不能拿 pure 的 `gpu-batch/lines-points-admission` 当守卫：它只数入队/顶点数，
    //     零宽的 quad 顶点数不变 ⇒ 它会**假绿**。像素才是这条的判据。
    file: 'apps/emulator-lift/src/runtime/d3d9_pipeline.cpp',
    from: '    constexpr double kExpandedLineHalfWidth = 0.5;  // 1px 宽 ⇒ 半宽 0.5px',
    to: '    constexpr double kExpandedLineHalfWidth = 0.0;  // 1px 宽 ⇒ 半宽 0.5px',
    guard: 'ctest:gpu-batch-barrier',
    ctest: 'gpu-batch-barrier',
    buildDir: 'apps/emulator-lift/out',
    buildTarget: 'lift-gpu-batch',
    what:
      '点/线展开的 quad 半宽写成 0 ⇒ 线上的三角形全部退化（0 像素）⇒ gpu-batch/lines-points 的' +
      '"线上非背景像素 >= 8" 当场红',
  },

  {
    // ★ 声明路的**索引映射**（`.tmp/lift-tickets/decl-path-guards-report.md`；
    //   `REQ-01M7A3M7E59JX74G897Q7W2RVD` 的"变异"判据）：
    //   `buildDeclVertices` 建的是 `[Base+Min, Base+Min+NumVertices)` 这一段顶点，所以按索引取顶点
    //   必须用 `relative = idx - MinVertexIndex`；写成 `idx` 就会从这段的**开头**错位 num_vertices 个顶点。
    //   守卫：`tests/d3d9_pipeline.cpp` 的 `d3d9p/indexed-range-decl` —— (a) 的三个点（红/绿/蓝在 x=10/20/30）
    //   会读到位移后的顶点 2/3/4（蓝/白/黄在 x=30/40/50）⇒ 三条像素断言当场红。
    //   ★ 用例里每段 NumVertices 取 5（不是 FVF 路的 3）就是为了让这条变异读到**已建出来的**顶点：
    //     取 3 的话 `idx*stride` 会越界读 ⇒ 变异可能崩在 UB 上，红得不可复现。
    file: 'apps/emulator-lift/src/runtime/d3d9_pipeline.cpp',
    from: '        const std::int64_t relative = static_cast<std::int64_t>(idx) - min64;',
    to: '        const std::int64_t relative = static_cast<std::int64_t>(idx);  // 变异：不减 MinVertexIndex',
    guard: 'ctest:d3d9-pipeline-gpu',
    ctest: 'd3d9-pipeline-gpu',
    buildDir: 'apps/emulator-lift/out',
    buildTarget: 'lift-d3d9-pipeline',
    what:
      '[decl-range] 声明路的取顶点映射退回 `idx`（不减 MinVertexIndex）⇒ 索引 2/3/4 读成顶点 2/3/4 而不是 0/1/2 ' +
      '⇒ d3d9p/indexed-range-decl 的 (a)(b) 三条像素断言红（点落在 30/40/50 而不是 10/20/30）',
  },

  {
    // ★ `start_index + index_count` 的 **int64 判定**（`REQ-01M7A3M7E59JX74G897Q7W2RVD` 第 2 条判据）：
    //   退回 u32 加法 ⇒ `0xFFFFFFF0 + 0x20` 回绕成 `0x10`，恰好等于 16 个索引的缓冲上界
    //   ⇒ `16 > 16` 为假 ⇒ 越过检查，随后按 `0xFFFFFFF0 * 2 ≈ 8.6 GB` 的偏移读索引（越界读）。
    //   守卫：`tests/d3d9_pipeline.cpp` 的 `d3d9p/indexed-range-start-wrap` —— 正控之后那条
    //   "越界 ⇒ D3DERR_INVALIDCALL" 拿不到返回码（进程崩在 memcpy）⇒ CTest 非 0。
    file: 'apps/emulator-lift/src/runtime/d3d9_pipeline.cpp',
    from:
      '    const std::int64_t index_sum =\n' +
      '        static_cast<std::int64_t>(start_index) + static_cast<std::int64_t>(index_count);',
    to: '    const std::int64_t index_sum = static_cast<std::int64_t>(start_index + index_count);  // 变异：u32 回绕',
    guard: 'ctest:d3d9-pipeline-gpu',
    ctest: 'd3d9-pipeline-gpu',
    buildDir: 'apps/emulator-lift/out',
    buildTarget: 'lift-d3d9-pipeline',
    what:
      '[start-wrap] `start_index + index_count` 退回 u32 加法 ⇒ 0xFFFFFFF0+0x20 回绕成 0x10 越过上界检查 ' +
      '⇒ 按 8.6 GB 偏移读索引 ⇒ d3d9p/indexed-range-start-wrap 进程崩（是崩溃，不是 INVALIDCALL）',
  },

  {
    // ★ `SetRenderTarget` 拒绝**离屏 plain surface**（`REQ-01M79WE8JA7V92RNS537CJB0V6`）：
    //   今天这条拒绝是 ownerless 判据的**副产物**（离屏表面有 `'TEX1'` 状态块、建 surface 时 owner 传 0）
    //   ⇒ 删掉判据 = 离屏 plain surface 被**静默**绑成 RT0，而 GPU 准入层已经不再有 `color_is_texture` 闸
    //   （`REQ-01M79MSMWGFEKF8J7C7VYWS4YP` 判它是死代码）⇒ 渲染写进 `GetRenderTargetData` 的读回目标。
    //   守卫：`tests/d3d9_pipeline.cpp` 的 `d3d9p/set-render-target-offscreen-refused`。
    //   ★ `(void)` 两句是因为 `/W4` 下 `tex_probe` / `is_texture_surface` 会变成未使用变量 ——
    //     那样**构建**失败会被 mutate-check 记成"不可判定"，而不是"守卫红了"。
    file: 'apps/emulator-lift/src/runtime/d3d9_pipeline.cpp',
    from:
      '        if (owner == 0 && is_texture_surface) {\n' +
      '            // 纹理表面却没有 owner ⇒ 绑定后像素块没有任何引用保着它（本单的缺陷会回来）。\n' +
      '            // ⛔ 不静默放过：这是"建 surface 时漏传 owner"的实现错误。\n' +
      '            cpu.com->release(cpu, target);\n' +
      '            pipeLog("SetRenderTarget", "纹理表面 0x%08X 没有像素块 owner ⇒ D3DERR_INVALIDCALL", target);\n' +
      '            if (why) *why = "D3D9 流水线：SetRenderTarget 的纹理表面没有 owner（建 surface 时漏传？）";\n' +
      '            return kInvalidCall;\n' +
      '        }',
    to:
      '        // 变异：删掉 ownerless 判据（离屏 plain surface 会被静默绑成 RT0）\n' +
      '        (void)tex_probe;\n' +
      '        (void)is_texture_surface;',
    guard: 'ctest:d3d9-pipeline-gpu',
    ctest: 'd3d9-pipeline-gpu',
    buildDir: 'apps/emulator-lift/out',
    buildTarget: 'lift-d3d9-pipeline',
    what:
      '[offscreen-rt] 删掉 ownerless 判据 ⇒ CreateOffscreenPlainSurface 的 surface 被静默绑成 RT0 ' +
      '⇒ d3d9p/set-render-target-offscreen-refused 的 INVALIDCALL / 渲染目标不变 / 像素落点三条红',
  },

  {
    // ★ 声明式 Draw 的 **stream1（UV）偏移**（`REQ-01M79P02VYRJ3V0C0V5VHXVE3H` 的变异判据）：
    //   `buildDeclVertices` 按元素的 `offset` 从各自的流里取分量；UV 在 stream1 的 offset 0。
    //   把它写成 +1 字节 ⇒ 采到的 u/v 是**移位后的字节**（(1,0) 变成 ≈(0,0)），四角 texel 全部换色。
    //   守卫：`tests/d3d9_pipeline.cpp` 的 `d3d9p/vertex-decl-draw`（2×2 四色纹理 + POINT 采样）。
    file: 'apps/emulator-lift/src/runtime/d3d9_pipeline.cpp',
    from: '                        s.base + s.offset + static_cast<std::size_t>(idx) * s.stride + used[k]->offset,',
    to:
      '                        s.base + s.offset + static_cast<std::size_t>(idx) * s.stride + used[k]->offset +\n' +
      '                            (k == 1 ? 1u : 0u),  // 变异：stream1（TEXCOORD）偏移 +1 字节',
    guard: 'ctest:d3d9-pipeline-gpu',
    ctest: 'd3d9-pipeline-gpu',
    buildDir: 'apps/emulator-lift/out',
    buildTarget: 'lift-d3d9-pipeline',
    what:
      '[decl-draw-uv] `buildDeclVertices` 的 stream1（TEXCOORD）偏移写成 +1 字节 ⇒ 采到的 UV 错位 ' +
      '⇒ d3d9p/vertex-decl-draw 的四角 texel 颜色（红/绿/蓝/白）当场换掉',
  },

  {
    // ★ 代表 "**没画**的批又回到无账状态" 这一类错（`REQ-01M7AAXVGY9SG75XCREHPX641Z`，M1 账本）：
    //   删掉 `submitPrimitives` **准入早退**里那一次 `noteGpuSkip` ⇒ 准入被拒的批不再进 `skipped`
    //   （`skipped_draws` 恒 0、摘要里没有 `skipped top`），而 `fallback` 也未必看得见它
    //   （CPU 0 像素的那类批）—— 正是设计 §A.3 的盲区。
    //   守卫：`apps/emulator-lift/tests/gpu_batch.cpp` 的 `gpu-batch/skip-ledger`（pure，Mock；
    //   `gpu-batch-order` 跑它）—— ALPHATESTENABLE=1 被拒 ⇒ skipped=1 + 原因进 top。
    //   ★ 位置（`REQ-01M7A74YG08R65NJBFXF7DEJDK`）：拒批改成"准入早退 + 记账"之后，主路径的
    //     `noteGpuSkip` 就是这一行；函数末尾那次只剩"后端失败"那一类。
    file: 'apps/emulator-lift/src/runtime/d3d9_pipeline.cpp',
    from: '        noteGpuSkip(cpu, pe, gpu_why, gpu_skip_bit);',
    to: '        // MUTATION：删掉 noteGpuSkip（跳过账本不再记账）',
    guard: 'ctest:gpu-batch-order',
    ctest: 'gpu-batch-order',
    buildDir: 'apps/emulator-lift/out',
    buildTarget: 'lift-gpu-batch',
    what:
      '删掉 `submitPrimitives` 准入早退里的 noteGpuSkip ⇒ 被拒批的 skipped_draws 恒 0 / 摘要无 skipped top' +
      ' ⇒ gpu-batch/skip-ledger 当场红',
  },

  {
    // ★ 代表 "`Clear(ZBUFFER)` 的 z 在**最后一跳**被写死" 这一类错
    //   （`REQ-01M6RMVJTTM8PDNXB39TVJ40WN`：z 全链传值）：
    //   接口（`GpuRaster::rasterRequestDepthClear(float)`）/ 转发壳（`SdlWindow`）/ pass 的
    //   `clear_depth` 三段都对，但 **shim → 后端** 这一跳"顺手抄回常量 1.0" ⇒ guest 清 0.25 时
    //   GPU 仍从 1.0 起步，后续 `z_enable` 的批按错的初值判深度（画面只少 / 多一层，不报错、账上正常）。
    //   守卫：`apps/emulator-lift/tests/gpu_batch.cpp` 的 `gpu-batch/depth-clear-value`（pure，Mock；
    //   由 `gpu-batch-order` 跑）—— 它逐位断言"后端收到的 z"与"pass 的清除值"都等于 guest 给的 0.25。
    //   ★ 为什么变异点选这里（而不是 `d3d9_gpu.cpp` 的 `depth.clear_depth = …`）：pure 守卫的观测面是
    //     **接口 / 计划层**（没有 SDL_GPU 设备就看不到真 pass），所以它抓的是"z 有没有离开 shim"；
    //     `d3d9_gpu.cpp` 那一跳由 `--gpu` 的 `gpu-batch/depth-segments` 覆盖（+ 本单的代码审查）。
    file: 'apps/emulator-lift/src/runtime/d3d9.cpp',
    from: '        gpu->rasterRequestDepthClear(z);',
    to: '        gpu->rasterRequestDepthClear(1.0f);  // MUTATION：z 写死回 1.0（修前形态）',
    guard: 'ctest:gpu-batch-order',
    ctest: 'gpu-batch-order',
    buildDir: 'apps/emulator-lift/out',
    buildTarget: 'lift-gpu-batch',
    what:
      '把 `Clear(ZBUFFER, z)` 的 z 在 shim→后端那一跳写死回 1.0 ⇒ 后端收到的 z 与 pass 清除值都是 1.0 ' +
      '（guest 给的是 0.25）⇒ gpu-batch/depth-clear-value 的位模式断言当场红',
  },

  {
    // ★ 代表 "surface 复制不校验格式" 这一类错（REQ-01M77ZJYSPX5V21QEB89W51815，存档缩略图链）。
    //   判据：`apps/emulator-lift/tests/d3d9.cpp` 的 `d3d9/offscreen-surface-rt-data` ③b ——
    //   64×32 X8R8G8B8 的源 → 64×32 R5G6B5 的离屏表面必须 `D3DERR_INVALIDCALL`。去掉格式判据后
    //   会掉进行距检查（E_FAIL 0x80004005）⇒ 断言当场红。
    file: 'apps/emulator-lift/src/runtime/d3d9.cpp',
    from: '    if (src.width != dst.width || src.height != dst.height || src.format != dst.format) {',
    to: '    if (src.width != dst.width || src.height != dst.height) {',
    guard: 'ctest:d3d9',
    ctest: 'd3d9',
    buildDir: 'apps/emulator-lift/out',
    buildTarget: 'lift-d3d9',
    what:
      'GetRenderTargetData 不再校验源/目标格式一致 ⇒ 16bpp 目标的失败面变成 E_FAIL（不是 ' +
      'D3DERR_INVALIDCALL）⇒ d3d9 的 offscreen-surface-rt-data ③b 当场红',
  },

  {
    // ★ 代表 "caps 又声明了本线只能**响亮拒绝**的能力" 这一类错（REQ-01M79PP97ZD65BJG9GMAJ08MJ3）。
    //   判据：`apps/emulator-lift/tests/d3d9.cpp` 的 `d3d9/caps-claims` ① —— LineCaps 声明 TEXTURE
    //   时，带纹理的线必须是 S_OK；实际是 `D3DERR_INVALIDCALL`（bit16 记账）⇒ 声明与行为对不上。
    file: 'apps/emulator-lift/src/runtime/d3d9.h',
    from: 'constexpr std::uint32_t kCapsLineClaim = 0x00000006u;',
    // ★ 0x06 = ZTEST|BLEND（ALPHACMP 未声明，见 REQ-01M7AZ65EAE7Q6STW2S0ZNXGM4）；
    //   变异多报 TEXTURE(0x01)|FOG(0x10) 两位 = 0x17，⛔ 不动 ALPHACMP（否则 ④ 也会红，
    //   这条变异要盯的是 ① 的"声明 ⇔ 行为"）。
    to: 'constexpr std::uint32_t kCapsLineClaim = 0x00000017u;',
    // ★ 像素见证随 CPU 光栅一起搬到了**真 GPU 臂**（`d3d9-gpu`）：这条守卫要真的画像素。
    guard: 'ctest:d3d9-gpu',
    ctest: 'd3d9-gpu',
    buildDir: 'apps/emulator-lift/out',
    buildTarget: 'lift-d3d9',
    what:
      'LineCaps 声明改回 0x17（多报 TEXTURE|FOG）⇒ 带纹理的线仍被 D3DERR_INVALIDCALL 拒绝 ⇒ ' +
      'd3d9/caps-claims ① 当场红',
  },

  {
    // ★ 代表 "ValidateDevice 的已裁决声明被悄悄改掉" 这一类错（REQ-01M79PP9H5XZRWFJD4BGS19S54）：
    //   本线**没有多 pass 装备** ⇒ 恒 `S_OK,1`；写成 2 就与注释里的依据/翻面条件脱节。
    //   判据：`apps/emulator-lift/tests/d3d9_pipeline.cpp` 的 `d3d9p/validate-device-one-pass` (i)。
    //   ★ 这条守卫**修前不会红**（防漂移）—— 变异是它唯一会红的方向，正说明它盯的是"声明不许变"。
    file: 'apps/emulator-lift/src/runtime/d3d9_pipeline.cpp',
    from: '    wrU32(cpu, passes_ea, 1u);  // 本线的状态组合一律单 pass',
    to: '    wrU32(cpu, passes_ea, 2u);  // 本线的状态组合一律单 pass',
    guard: 'ctest:d3d9-pipeline-gpu',
    ctest: 'd3d9-pipeline-gpu',
    buildDir: 'apps/emulator-lift/out',
    buildTarget: 'lift-d3d9-pipeline',
    what:
      'validateDevice 把 `*pNumPasses` 写成 2 ⇒ 与"本线无多 pass 装备"的声明矛盾 ⇒ ' +
      'd3d9p/validate-device-one-pass 当场红',
  },

  {
    // ★ 代表 "解码失败被折叠成成功"这一类错（REQ-01M77CMJGJ9EJVNPFS2TKMMHD1）。
    //   判据：`apps/emulator-lift/tests/native_override.cpp` 的 ④c —— 被截断的流（输入提前耗尽 /
    //   输出没填满，comp≠out 的四种形状）必须 `false`；assets 档的
    //   `native_override_assets.cpp` 另用**真字节解释器臂**钉"合法流逐字节 + 截断 ⇒ 回退"。
    //   改回无条件 `true` ⇒ 截断流被当成成功、调用方不再回退解释器 ⇒ 纯档当场红。
    file: 'apps/emulator-lift/src/runtime/native_lzss.cpp',
    from: '    return out == out_end;',
    to: '    return true;',
    guard: 'ctest:native-override',
    ctest: 'native-override',
    buildDir: 'apps/emulator-lift/out',
    buildTarget: 'lift-native-override',
    what:
      'LZSS 解码的结尾判据改回无条件 true（输入提前耗尽 / 输出没填满也报成功）⇒ ' +
      'native-override 的 ④c 截断流用例当场红',
  },

  {
    // ★ 代表 "guest 退出码映射被改成**透传**"这一类错（`REQ-01M7HP2YRZG222E0NE2H6MW6NK` 的
    //   纯函数面）：`exitCodeForGuestExit` 是解释器臂与 AOT 臂**共用**的那一份映射，现在住在
    //   `src/runtime/guest_exit.h`（AOT ⛔ 不 include `machine.h`，所以它必须自成一个纯头）。
    //   判据：`apps/emulator-lift/tests/guest_exit.cpp` —— 0 ⇒ 0；非 0（含 127/128/255/256/259/
    //   0x7FFFFFFF/0xFFFFFFFF）⇒ 2 且**不等于 n**。改成透传 ⇒ 1/3/... 落回自己 ⇒ 当场红。
    //   ★ 为什么变异点选这里（不是 `aot_main` 的接线）：纯用例够不到 AOT 的 e2e 退出码面
    //     （见该文件头的"弱守卫"说明），它能钉住的正是这条映射本身。
    file: 'apps/emulator-lift/src/runtime/guest_exit.h',
    from: '    return guest_exit_code == 0u ? 0 : 2;',
    to: '    return static_cast<int>(guest_exit_code & 0xFFu);',
    guard: 'ctest:lift-guest-exit',
    ctest: 'lift-guest-exit',
    buildDir: 'apps/emulator-lift/out',
    buildTarget: 'lift-guest-exit',
    what:
      'guest 退出码改成透传（非 0 ⇒ n 原值，不再压成 2）⇒ `lift-guest-exit` 的 nonzero 用例当场红',
  },

  {
    // ★ 代表 "`rep movs*` 快路径的**方向 × 重叠**闸门写反" 这一类错
    //   （REQ-01M75XK0XRTSWCZ4G8A5W2GW8）。
    //   判据：`apps/emulator-lift/tests/rep_bulk.cpp` 的 `diff/*` —— 同一份 guest 字节、`X86::rep_bulk`
    //   开/关两臂，比目标区逐字节 + ECX/ESI/EDI/EAX/EFLAGS + insns + `g_access`。
    //   闸门写反 ⇒ "逆向涂抹"（`DF=0 && dst>src` 重叠 / `DF=1 && dst<src` 重叠）也走 4 B 分块
    //   ⇒ 位移 1..size-1 的那几档被涂抹（实测首个差异落在目标区中段），且 hits/declines 互换。
    //   ★ 为什么挑这一条（而不是 committed / 64 位乘）：那两个闸门在 **guest 可见状态**上观察不到
    //     （放过去就是 fault / 读出界），只能靠 `repBulkPlan` 的直接断言抓；而"方向 × 重叠"是本单
    //     唯一会**静默改字节**的闸门，红的方向就是"两臂内存不同"。
    //   ★ 编号/落点说明：判据在 `tests/rep_bulk.cpp`，注册在 lift 的 CMakeLists 顶层（pure ⇒ 默认跑）。
    file: 'apps/emulator-lift/src/runtime/exec_string.cpp',
    from: '            const bool reverse = df ? (dlo < slo) : (dlo > slo);',
    to: '            const bool reverse = df ? (dlo > slo) : (dlo < slo);',
    guard: 'ctest:rep-bulk',
    ctest: 'rep-bulk',
    buildDir: 'apps/emulator-lift/out',
    buildTarget: 'lift-rep-bulk',
    what:
      '`rep movs*` 快路径的方向闸门写反（正/逆涂抹互换）⇒ 逆向重叠的 4 B 分块把源字节涂抹：' +
      'rep-bulk 的 diff 矩阵逐字节差分（34 例）与 hits/declines 断言当场红',
  },

  {
    // ★ 代表 "x87 override 的运算顺序 / `fchs` 被弄丢" 这一类错（REQ-01M6NZSAAD0WCNBE3RKMFF84R5）。
    //   判据：`apps/emulator-lift/tests/native_override_assets.cpp` 的 ④ = `sub_4CCC60` 的**函数级真字节
    //   两臂**（同一份 guest 机器码跑解释器当 oracle，再跑 native 实现；比 dst / 寄存器 / x87 状态字）。
    //   `x87ChangeSign(...)` 是 `.lst` 0x4CCCC8 `fchs` 的唯一落点（B = -(r*sy*sin)）⇒ 去掉它，
    //   B 的符号翻正 ⇒ 每个点的 dst[i] 都变 ⇒ 当场红。
    //   ★ 2026-11-16：实现里两次运算已合并成一行（`x87ChangeSign(x87Mul(t3, sn))`），
    //     旧的 `from: '    x87ChangeSign(b);'` 已不存在 —— 更新到当前文本，破坏语义不变（丢 fchs）。
    //   ★ 为什么挑这一条而不是 sin/cos 对调：`.lst` 0x4CCC80/0x4CCC84 的两次求值互相独立
    //   （都在同一个 θ 上，各自只写自己的栈槽）⇒ 那种"变异"不会改变任何 guest 可见状态，红得没意义。
    file: 'apps/emulator-lift/src/runtime/native_affine.cpp',
    from: '    const X87Val b = x87ChangeSign(x87Mul(t3, sn));',
    to: '    const X87Val b = x87Mul(t3, sn);',
    guard: 'ctest:native-override-assets',
    ctest: 'native-override-assets',
    buildDir: 'apps/emulator-lift/out',
    buildTarget: 'lift-native-override-assets',
    what:
      'B 系数（`fchs` @0x4CCCC8）不再取号 ⇒ 每个顶点的 dst[i] 符号项错 ⇒ ' +
      'native-override-assets 的 sub_4CCC60 两臂逐字节当场红',
  },

  // ★ 下面五条代表 `sub_42F560` 的**插入排序扫描段**（`0x42F5E0..0x42F6EB` ∪ 落位块
  //   `{0x42F783..0x42F793}`）的五类**静默错**。判据都在
  //   `apps/emulator-lift/tests/native_override_assets.cpp` 的 `sortseg/*`（真字节两臂：解释器 = 同一 EA
  //   的 oracle ⇒ 逐字节比 `A[]` + `cpu.insns` 精确相等 + `g_j`/`seed`/`ESP`/`EBP`/`ESI`），
  //   pure 档的四条闸门在 `tests/native_override.cpp` 的 `sortseg/*`。
  //   ★ 全部只让 **native 臂**分叉（解释器臂跑的是同一份真字节）⇒ 不依赖任何"当前数字"。
  //   ★ ⛔ 不要加"`j > p` → `j >= p`"：多写的那一次紧接着被 `A[p+1] = i` 覆盖、G6 又保证无别名
  //     ⇒ **结构上不可观察**（设计稿已立墓碑）。

  {
    file: 'apps/emulator-lift/src/shims/kernel32_bootstrap.cpp',
    from: '            wide.push_back(u);\n            i += static_cast<std::size_t>(n);',
    to: '            (void)u;\n            wide.push_back(static_cast<char16_t>(p[i]));\n            ++i;',
    guard: 'ctest:codepage',
    ctest: 'codepage',
    buildDir: 'apps/emulator-lift/out',
    buildTarget: 'lift-codepage',
    what: 'MultiByteToWideChar 退回逐字节直通（0x82A0 → U+0082 U+00A0）⇒ codepage 的 932 定点断言红',
  },


  {
    file: 'apps/emulator-lift/src/runtime/cp932.cpp',
    from: 'if (b0 < 0x80 || (b0 >= 0xA1 && b0 <= 0xDF) || b0 == 0x80 || b0 == 0xA0 || (b0 >= 0xFD)) {',
    to: 'if (b0 < 0x80) {',
    guard: 'ctest:strfmt',
    ctest: 'strfmt',
    buildDir: 'apps/emulator-lift/out',
    buildTarget: 'lift-strfmt',
    what: '半角片假名 / 8 个单字节例外不再走单字节表 ⇒ strfmt 的 CP932 定点断言红（说明那张表真的被用）',
  },


  {
    file: 'apps/emulator-lift/src/runtime/memory.cpp',
    from: '    return addr >= base && (addr - base) < GuestMemory::kSpace;',
    to: '    return true;  // 变异：永远当 guest fault',
    guard: 'ctest:fault-report',
    ctest: 'fault-report',
    buildDir: 'apps/emulator-lift/out',
    buildTarget: 'lift-fault-report',
    what: '归因恒为 guest ⇒ 宿主 AV 又被报成上一条 guest 访存（`access :` 行回来了）⇒ fault-report 红',
  },

  {
    file: 'apps/emulator-lift/src/shims/kernel32_string.cpp',
    from: '    cpu.mem->writeBytes(buf, text.data(), text.size());',
    to: '    (void)text;  // 变异：A 版只回长度、不写字节',
    guard: 'ctest:locale-ext',
    ctest: 'locale-ext',
    buildDir: 'apps/emulator-lift/out',
    buildTarget: 'lift-locale-ext',
    what: 'GetLocaleInfoA 不写字节 ⇒ A 的空串 != W 的 CP932 投影 ⇒ locale-ext 的 get-locale-info-a 红',
  },

  {
    file: 'apps/emulator-lift/src/shims/kernel32_file.cpp',
    from: '    st->readable = want_read;  // ⛔ 不再有 "|| (!want_write)"：access=0 不授予读',
    to: '    st->readable = want_read || (!want_write);  // 变异：access=0 又按可读处理',
    guard: 'ctest:file-io',
    ctest: 'file-io',
    buildDir: 'apps/emulator-lift/out',
    buildTarget: 'lift-file-io',
    what: 'access=0 又按可读处理 ⇒ 只查属性的句柄能读 ⇒ file-io 的 access-zero-open 红',
  },

  {
    file: 'apps/emulator-lift/src/shims/kernel32_sync.cpp',
    from: '    if ((e->access & mask) != 0) return true;',
    to: '    if (true) return true;  // 变异：访问位检查失效',
    guard: 'ctest:sync-ext',
    ctest: 'sync-ext',
    buildDir: 'apps/emulator-lift/out',
    buildTarget: 'lift-sync-ext',
    what: '同步对象访问位检查失效 ⇒ 零访问位句柄也能改状态 ⇒ sync-ext 的 duplicate-access-and-pseudo 红',
  },


  {
    file: 'apps/emulator-lift/src/shims/kernel32_sync.cpp',
    from: '    shimAbort("kernel32", api,\n              "需要等待：hHandle=0x%08X 未 signaled，dwMilliseconds=%u（0x%08X）⇒ 本模拟器不阻塞；"\n              "请人工检查该调用场景并设计等待方案（SCOPE §1 等待条目）",\n              h, timeout, timeout);',
    to: '    return kWaitTimeout;  // 变异：退回 WAIT_TIMEOUT',
    guard: 'ctest:shims-core',
    ctest: 'shims-core',
    buildDir: 'apps/emulator-lift/out',
    buildTarget: 'lift-shims-core',
    what: '未 signaled 的等待又静默返回 WAIT_TIMEOUT ⇒ shims-core 的 unsignaled-wait-traps 红（子进程没有 abort）',
  },


  {
    file: 'apps/emulator-lift/src/runtime/heap.cpp',
    // ★ `from` 必须**恰好出现一次**（`staticManifestCheck` 的 ③）：这条检查在 `GuestHeap::alloc`
    //   （`heap.cpp:80-82`）与 `GuestHeap::realloc`（`:172-174`）里是**同样两行**，只有第三行的
    //   报错文案不同 ⇒ 把第三行一起写进 `from` / `to` 才能把它钉在 `alloc` 上。
    //   （旧 `from` 只到 `{` ⇒ count==2，靠"alloc 在 realloc 前面"的行序侥幸打对。）
    from:
      '    const std::uint64_t need64 = static_cast<std::uint64_t>(bytes) + opt_.header;\n' +
      '    if (need64 > 0xFFFFFFFFull) {\n' +
      '        std::snprintf(last_error_, sizeof(last_error_), "guest 堆请求过大（%u 字节 + 块头）", bytes);',
    to:
      '    const std::uint32_t need64 = bytes + opt_.header;  // 变异：退回 32 位算\n' +
      '    if (false) {\n' +
      '        std::snprintf(last_error_, sizeof(last_error_), "guest 堆请求过大（%u 字节 + 块头）", bytes);',
    guard: 'ctest:memory-pages',
    ctest: 'memory-pages',
    buildDir: 'apps/emulator-lift/out',
    buildTarget: 'lift-memory-pages',
    what: '堆请求的 bytes+header 退回 32 位 ⇒ 0xFFFFFFF8 请求切出 size=0 的块并返回非空指针 ⇒ memory-pages 的 heap-request-overflow 红',
  },


  // ── D3D9 生命周期：**回收资格**不能来自"最近 init 的那台 Machine"（`activeMachine()`）──

  {
    file: 'apps/emulator-lift/src/runtime/decode.cpp',
    from: '    //   （machine.cpp 里 `left_block == false` 的续跑路径）。\n    return true;',
    to: '    //   （machine.cpp 里 `left_block == false` 的续跑路径）。\n    return false;  // 变异：长块分片当错误',
    guard: 'ctest:dispatch',
    ctest: 'dispatch',
    buildDir: 'apps/emulator-lift/out',
    buildTarget: 'lift-dispatch',
    what: '>256 条无控制流的直路又当成 DecodeError ⇒ dispatch 的 long-block-splits 红（语料里 0x415DF6 的 554 条块）',
  },


  {
    file: 'apps/emulator-lift/tests/decode_crosscheck.cpp',
    from: '            ++len_checked;\n            const std::uint32_t expect = e.next_ea - e.ea;',
    to: '            const std::uint32_t expect = e.next_ea - e.ea;  // 变异：不统计真正比过长度的行',
    guard: 'ctest:decode-crosscheck',
    ctest: 'decode-crosscheck',
    buildDir: 'apps/emulator-lift/out',
    buildTarget: 'lift-decode-crosscheck',
    what: 'A3 的 len_checked 不计数 ⇒ 断言 len_checked==checked 红（防止"只比助记符、不静默跳过长度"）',
  },


  {
    // ★ 2026-11-10 改形态 + 重定向（REQ-01M70KGDXSKDVK7MCJYFABXERH）：这条守的不变量已被
    //   **更细的守卫**取代。原片段 `if (copy && raw_end > img.bytes.size())` 是"只看要拷的字节"的
    //   旧口径，现源码里还有一条更严的（2026-11-05 加严）：节头**声明的整个 raw 区间**
    //   `raw_offset + raw_size` 必须落在文件里 —— `tests/pe_loader.cpp` 的 `raw-padding-beyond-eof`
    //   就是专为它加的用例（越界部分全在 VirtualSize 之后 ⇒ 旧口径静默接受）。
    //   ⇒ 施加点改到那条更严的判据，破坏形态是**退回旧口径**（把 `declared_end` 换成 `copy_end`）：
    //   语义上正是"越界只看要拷的字节"，`raw-padding-beyond-eof` 当场红。
    //   ⛔ 不改成 `if (false)`：那只是"判据整条消失"，而 `copy_end` 形态精确表达**退回旧口径**
    //   （image.cpp 353-362 的注释明令禁止的那件事），守卫红的方向才与这条 `what` 对得上。
    file: 'apps/emulator-lift/src/runtime/image.cpp',
    from: '        if (s.raw_size && declared_end > img.bytes.size()) {',
    to: '        if (s.raw_size && copy_end > img.bytes.size()) {  // 变异：退回"只看要拷的字节"的旧口径',
    guard: 'ctest:pe-loader',
    ctest: 'pe-loader',
    buildDir: 'apps/emulator-lift/out',
    buildTarget: 'lift-pe-loader',
    what: '节 raw 声明区间越过文件末尾又静默接受（退回"只看要拷的字节"）⇒ pe-loader 的 raw-padding-beyond-eof 红（半载入仍返回 true）',
  },


  // ── emulator-lift：本轮新落的 5 组守卫（input-injection / guest-token-stress /
  //    audio-device / gpu-batch-*）──
  //   ★ 它们都是**顶层无条件注册**的 CTest（判据 = 注册位置 ⇒ pure 档），所以 `buildDir`
  //     指到 `apps/emulator-lift/out`、`ctest` 名与 `apps/emulator-lift/CMakeLists.txt` 的
  //     `add_test(NAME …)` 逐字一致。
  {
    file: 'apps/emulator-lift/src/shims/user32_window.cpp',
    from:
      '    const std::uint32_t vk = call.u32(0) & 0xFFu;\n' +
      '    call.result = cpu.windows->keyDown(vk) ? 0x8000u : 0u;',
    to:
      '    const std::uint32_t vk = call.u32(0) & 0xFFu;\n' +
      '    (void)vk;\n' +
      '    call.result = 0u;  // 变异：恒报"未按下"（注入的键态读不到）',
    guard: 'ctest:input-injection',
    ctest: 'input-injection',
    buildDir: 'apps/emulator-lift/out',
    buildTarget: 'lift-input-injection',
    what:
      'GetAsyncKeyState 恒返回 0（丢掉 keyDown 的最高位）⇒ input-injection 的 ' +
      '`testEaxImm(0x8000)` 那几条当场红（注入路径的"按键真的被看见"没有别的判据）',
  },

  // ── 交互图的可选按钮词（图语法 `click <x> <y> [left|right]` / `click center [left|right]`）──

  {
    file: 'apps/emulator-lift/src/runtime/decode.cpp',
    from: '    if (sseMovsdByOperands(zi, zops, key)) {',
    to: '    if (false && sseMovsdByOperands(zi, zops, key)) {  // 变异：不按操作数形态分派',
    guard: 'ctest:conformance',
    ctest: 'conformance',
    buildDir: 'apps/emulator-lift/out',
    buildTarget: 'lift-conformance',
    what:
      '`F2 0F 10`（SSE2 标量 movsd）退回 `Op::Movs` ⇒ 静默按串操作执行（改 [edi] / 推进 esi·edi）；' +
      'conformance 的 `sse-movsd-*` 三条红',
  },


  {
    // ★ 2026-11-13：`conformance-rejects-hollow-case` 按**用途**回**默认档**（顶层无条件注册、
    //   `ENVIRONMENT LIFT_CONFORMANCE_SELFCHECK=1`；它是身份判据的负控、0.5 s、不需要安装树）
    //   ⇒ 默认 configure 下**就在注册表里**，所以**不带 `optInEnv`**（不再有 opt-in 的门）。
    //   它证明的是"**恒等判据真的会红**"（⛔ 不是判据恒假但一直绿）。
    file: 'apps/emulator-lift/tests/conformance.cpp',
    from: '    return r[lift::kEsp] == before.r[lift::kEsp] + 4u;',
    to: '    return r[lift::kEsp] == before.r[lift::kEsp];  // 变异：哨兵 ret 的栈效果没被排除',
    guard: 'ctest:conformance-rejects-hollow-case',
    ctest: 'conformance-rejects-hollow-case',
    buildDir: 'apps/emulator-lift/out',
    buildTarget: 'lift-conformance',
    what:
      '恒等判据退回"含哨兵 ret"的旧口径 ⇒ 空心夹具不再被判红（负控 CTest `conformance-rejects-hollow-case` 红）',
  },


  // ── `WindowManager` 的**主线程亲和性**（REQ-01M76SXW0Y3R8QX1F94AB85M9H 的 2026-11-12 裁决）──
  //   这不是令牌：wm 按 M0 的口径是**主线程独占宿主对象** ⇒ 判据是"谁在碰"，⛔ 不是"持不持牌"。
  //   破坏 = 删掉 `find`（非 const）入口的那一句检查点 ⇒ 非 owner 线程碰 wm 时不再记账。
  //   ★ `from` 必须带上函数签名：`host_window.cpp` 里 `checkMainThread("window.find")` 有**两处**
  //     （const / 非 const 两个 `find`），只拿那一行做锚会落在第一处、说不清删的是哪个入口。
  {
    file: 'apps/emulator-lift/src/runtime/host_window.cpp',
    from:
      'HostWindow* WindowManager::find(std::uint32_t hwnd) {\n' +
      '    checkMainThread("window.find");\n' +
      '    auto it = windows_.find(hwnd);',
    to:
      'HostWindow* WindowManager::find(std::uint32_t hwnd) {\n' +
      '    // 变异：删掉 find 入口的主线程检查点（wm 被非主线程访问时不再记账）\n' +
      '    auto it = windows_.find(hwnd);',
    guard: 'ctest:window-manager-token',
    ctest: 'window-manager-token',
    buildDir: 'apps/emulator-lift/out',
    buildTarget: 'lift-wm-token',
    what:
      '`WindowManager::find` 入口不再调 `checkMainThread("window.find")` ⇒ worker 线程碰 wm 时 ' +
      '`offThread()` 不涨、`firstOffThreadSite()` 仍是 nullptr ⇒ window-manager-token 的 ' +
      '`window-manager-token/off-thread-find` 红（`>=1` 与站点名两条同时失配）',
  },

  // ── 度量缓存的变异条目**已恢复并改指 `ctest:gdi-font`** ──
  //   见本清单上方 `gdi-font` 那一条（2026-11-13 复审：`gdi-font` 走的正是生产 `metrics()` 缓存）。

  // ── 缺字形判据：`FT_Get_Char_Index != 0`，⛔ 不是 `FT_Load_Char` 的返回值 ──────────────
  //   `REQ-01M79N1ZV391BCM7MYSKK0GVV0`：FreeType 对缺字**不报错** —— `FT_Get_Char_Index` 返回 0，
  //   `FT_Load_Char` 仍返回 0 并加载 glyph 0（`.notdef`）。判据退回 load 的返回值 ⇒ 主字体缺的
  //   U+E010（♡）被当成"有字形"，永远到不了游戏自带的 `AGE Extend` 后备 ⇒ 画 `.notdef` 实心块。
  {
    file: 'apps/emulator-lift/src/runtime/font.cpp',
    from: '    const bool primary_has_glyph = glyphIndexFor(*impl_, primary, 0, cp) != 0;',
    to:
      '    // 变异：判据改回 `FT_Load_Char` 的返回值（缺字它也返回成功）\n' +
      '    const bool primary_has_glyph = loadGlyphFor(*impl_, primary, 0, cp, size, bold, &m);',
    guard: 'ctest:gdi-font-fallback',
    ctest: 'gdi-font-fallback',
    buildDir: 'apps/emulator-lift/out',
    buildTarget: 'lift-gdi-font-fallback',
    what:
      '缺字判据改成"load 成功" ⇒ 主 face 的 `.notdef` 被当成有字形（后备永远不被问）⇒ ' +
      'gdi-font-fallback 的 `fallback/mask` 红（期望后备的 mask，实际是 `.notdef` 的 1×1 / advance=3）',
  },

  // ── 缺陷的另一半：shim 必须把 guest 字节**交给引擎**（⛔ 不是只记日志 / 传空 bytes）────────
  //   `REQ-01M79N1ZV391BCM7MYSKK0GVV0`：`shimAddFontMemResourceEx` 若退回"只记日志 + 假句柄"，
  //   则 `addMemoryFont` 永远不被调用 ⇒ 引擎里没有后备字体 ⇒ U+E010 仍画 `.notdef`。
  //   ★ 这条必须由**经 shim** 的用例吃掉：pure 档与 assets「直接 `addMemoryFont`」两段都绕过
  //     shim，它们对这条变异**全绿** —— 那正是原缺陷形态复发不可见的原因。
  {
    file: 'apps/emulator-lift/src/shims/gdi32_font.cpp',
    from:
      '    const std::vector<std::uint8_t> bytes = call.bytes(0, size);\n' +
      '    std::string family;\n' +
      '    const std::uint32_t font_id =\n' +
      '        (cpu.fonts != nullptr) ? cpu.fonts->addMemoryFont(bytes.data(), bytes.size(), &family) : 0u;',
    to:
      '    // 变异：只记日志 + 假 id，**不把 guest 字节交给引擎**（原缺陷形态：丢字节）\n' +
      '    (void)call.bytes(0, size);  // 读了但丢掉\n' +
      '    std::string family = "AGE Extend";\n' +
      '    const std::uint32_t font_id = 1u;  // 假 id：引擎里并没有多任何内存字体',
    guard: 'ctest:gdi-font-fallback-assets',
    ctest: 'gdi-font-fallback-assets',
    buildDir: 'apps/emulator-lift/out',
    buildTarget: 'lift-gdi-font-fallback',
    what:
      'shim 不再把 guest 字节交给 `addMemoryFont`（只记日志 + 假句柄）⇒ 引擎 `memoryFontCount()` ' +
      '仍是 0、U+E010 回 `.notdef` ⇒ gdi-font-fallback-assets 的 `shim/add-font-mem` 红',
  },

  {
    file: 'apps/emulator-lift/src/runtime/audio.cpp',
    from: '        std::memset(out.data(), 0, out.size());',
    to: '        // 变异：不覆盖最终交付缓冲（默认静音失效，音频原样送出）',
    guard: 'ctest:audio-device',
    ctest: 'audio-device',
    buildDir: 'apps/emulator-lift/out',
    buildTarget: 'lift-audio',
    what:
      '去掉最终交付缓冲那一次 `memset` ⇒ 默认静音失效（处理照跑、缓冲没被置 0）⇒ ' +
      'audio-device 的 `default-muted-delivery` 红（`muted_bytes=` 与 dump 的零值判据）',
  },

  // ── emulator-lift：A2 的"裸标识符必须诚实报 Unknown"（★ 假绿口子）──
  //   ★ 退回"裸标识符默认判数据" ⇒ 未映射的助记符（如 `cld`）被吞成数据行，
  //     于是"这条指令没被解出来"在 A2 的对账里**看不出来**（正是审计 T4 的假绿口子）。
  {
    file: 'apps/emulator-lift/src/runtime/mnemonics.cpp',
    from:
      '        //   裸标识符若不在助记符表里 ⇒ 诚实报 Unknown，让 A2 看见（审计 T4 的假绿口子）。\n' +
      '        return LstKind::Unknown;',
    to:
      '        //   裸标识符若不在助记符表里 ⇒ 诚实报 Unknown，让 A2 看见（审计 T4 的假绿口子）。\n' +
      '        return LstKind::DataOrPseudo;  // 变异：裸标识符默认判数据',
    guard: 'ctest:mnemonics',
    ctest: 'mnemonics',
    buildDir: 'apps/emulator-lift/out',
    buildTarget: 'lift-mnemonics',
    what: '裸标识符（如未映射的 `cld`）又默认判数据 ⇒ mnemonics 的 lst-classify 红（A2 的假绿口子回来了）',
  },

  {
    file: 'apps/emulator-lift/src/runtime/d3d9_pipeline.cpp',
    from:
      'void writeViewportFullTarget(X86& cpu, std::uint32_t pe, std::uint32_t w, std::uint32_t h) {\n' +
      '    if (w == 0 || h == 0) return;',
    to:
      'void writeViewportFullTarget(X86& cpu, std::uint32_t pe, std::uint32_t w, std::uint32_t h) {\n' +
      '    if (w == 0 || h == 0 || cpu.mem != nullptr) return;  // 变异：短路 viewport 回写',
    guard: 'ctest:d3d9-pipeline',
    ctest: 'd3d9-pipeline',
    buildDir: 'apps/emulator-lift/out',
    buildTarget: 'lift-d3d9-pipeline',
    what:
      'SetRenderTarget 不再把 viewport 回写成新目标的全幅（修前形状：只写 colorSurface/Arena）⇒ ' +
      'd3d9-pipeline 的 `set-render-target-viewport` 红（绑定纹理表面后 GetViewport 仍是 guest 设的 ' +
      '48×48、切回后缓冲后也不是 64×64；实测 14 条断言红）',
  },

  // ── 对象表**代际**：陈旧持有者（缓存了 (EA, 代际)）不许打到"复用同一 EA"的新对象 ──
  //   （REQ-01M6X4NFHCW1Y6YDJ4DYFA92PS）两条变异分别短路两处消费点，判据各在同名 CTest 里。
  {
    file: 'apps/emulator-lift/src/runtime/com.cpp',
    from: '    if (expect_generation != 0 && it->second.generation != expect_generation) {',
    to: '    if (false && expect_generation != 0 && it->second.generation != expect_generation) {  // 变异：短路代际校验',
    guard: 'ctest:mechanism',
    ctest: 'mechanism',
    buildDir: 'apps/emulator-lift/out',
    buildTarget: 'lift-mechanism',
    what:
      '`releaseIf` 不再校验代际（按 EA 直接减引用）⇒ 陈旧持有者会把**复用同一 EA 的新对象**Release 掉 ⇒ ' +
      'mechanism 的 com-generation-stale-holder 红（新对象的 refcount / 对象表项被减）',
  },

  // ---- C1 性能修（2026-11-20）：每 (iface, methods) 一份 COM 槽计划缓存 ----
  // ★ 为什么这条值得一条变异：命中路径是**新增的第二条建对象路径**。它写 vtable 的那一行如果把
  //   每个槽都写成 slot 0 的 stub，对象仍"建得出来"、guest 也仍能调 —— 但调用会**串到别的方法**
  //   上（静默行为错，不崩、不黑屏），与 `registerSlot` 的"同 stub 不同实现"校验要防的是同一类。
  // ★ 守卫在 `mechanism` 的 `com-slot-plan-hit-and-conflict`：同一张表建两个对象（都走命中路径）
  //   ⇒ vtable 的每个槽必须等于**已知 stub**（前序用例的 get/hole/add 桩）。
  //   ★ 变异后**两个对象**的每个槽都会变成 slot 0 的 stub ⇒ 本 case 自己就红（不再只靠前序
  //     `com-second-object-reuses-stubs`：那条比的是"两个对象互等"，同一个变异会让两者同样错）。
  {
    file: 'apps/emulator-lift/src/runtime/com.cpp',
    from: '            cpu.mem->wr32(vtable + slot * 4u, plan_it->second.stubs[slot]);',
    to: '            cpu.mem->wr32(vtable + slot * 4u, plan_it->second.stubs[0]);  // 变异：命中路径写错槽位',
    guard: 'ctest:mechanism',
    ctest: 'mechanism',
    buildDir: 'apps/emulator-lift/out',
    buildTarget: 'lift-mechanism',
    what:
      'COM 槽计划**命中路径**把每个槽都写成 slot 0 的 stub（对象仍建得出来、guest 仍能调，但调用串线）' +
      '⇒ mechanism 的 `com-slot-plan-hit-and-conflict` 红（两个对象的 vtable 槽都与已知 stub 不符；' +
      '前序 `com-second-object-reuses-stubs` 也红）',
  },

  // ---- C6 性能修（2026-11-20）：`Vfs` 的读路径负缓存按影子代失效 ----
  // ★ 为什么这条值得一条变异：缓存的是**失败结论**（`[notfound]` / `[notdir]`）。影子写了一刀之后
  //   guest 再解析同一路径，旧条目若仍命中，引擎就会"自己刚写完的文件读不到"（静默行为错，
  //   不崩、不黑屏）。代号检查反了 = 新鲜条目被丢、**过期条目照答** —— 正是这条守卫要钉的形态。
  // ★ 守卫在 `file-path-ext` 的 `vfs-negative-cache-invalidated-by-create`：先解析不存在路径
  //   （负结论入缓存），在影子里创建它，再解析**必须找到**。
  {
    file: 'apps/emulator-lift/src/runtime/vfs.cpp',
    from: '    if (it->second.gen != path_cache_gen_) {',
    to: '    if (it->second.gen == path_cache_gen_) {  // 变异：代号检查反了 ⇒ 过期条目照样命中',
    guard: 'ctest:file-path-ext',
    ctest: 'file-path-ext',
    buildDir: 'apps/emulator-lift/out',
    buildTarget: 'lift-file-path-ext',
    what:
      'Vfs 负缓存的**代号检查反了** ⇒ 影子写之后仍拿旧代条目回答（新建之后解析不到）' +
      '⇒ file-path-ext 的 `vfs-negative-cache-invalidated-by-create` 红',
  },


  // ── emulator-lift：时间线的**效果级判据**不许恒真 ─────────────────────────────────────
  //   ★ "先判满足"的短路：任何观测窗在第一个块边界就"看到变化"⇒ `not-changed` / `unclosed`
  //     两例都再也不会失败 ⇒ input-expect 当场红。
  //   ★ 用**运行时恒真**（`sampleCount() >= samples_at_start`，单调计数）而不是 `if (true)`：
  //     `/W4 /WX` 下 C4127（条件恒真）会让"变异体编译失败"，harness 记的是"无法判定"不是红。
  //   ★ `w` 在后面还要用（ok 日志），所以不需要 `(void)w;`；也不新增任何未用局部量。
  {
    file: 'apps/emulator-lift/src/runtime/machine.cpp',
    from: '        if (sampler_.changeCount() > w.changes_at_start) {',
    to:
      '        if (sampler_.sampleCount() >= w.samples_at_start) {  ' +
      '// 变异：任何窗都立即算"看到变化"',
    guard: 'ctest:input-expect',
    ctest: 'input-expect',
    buildDir: 'apps/emulator-lift/out',
    buildTarget: 'lift-input-expect',
    what:
      '观测窗恒判"已变化" ⇒ input-expect 的 expect-change-not-changed / expect-change-unclosed ' +
      '红（"动作没生效"这一类再也不会被抓住）',
  },

  // ── emulator-lift · D3D9 状态块（REQ-01M7372BPVGBD247KAGHB53YK7）────────────────────────
  //   缺陷形态（修前）：`IDirect3DStateBlock9` 的 Capture / Apply 直接返回 S_OK ⇒ 引擎每帧
  //   Live2D 的 `CreateStateBlock(D3DSBT_ALL) → draw → Apply → Release`（`.lst` 0x47843A–0x47847B）
  //   不还原 draw 里改的 FVF / 纹理绑定 / 渲染状态 / 变换，泄漏到后续 2D 绘制。
  //   变异 = 让 `applyStateBlock` 在写回之前返回（= 修前的"空转"形状）。
  //   ★ 用 `cpu.mem != nullptr`（成员指针、运行时恒真）而不是 `if (false)`：本工程 /W4 /WX，
  //     编译期常量条件会变成"构建失败"（记成不可判定）而不是"守卫红了"。
  //   ★ 判据：`tests/d3d9.cpp` 的 `d3d9/stateblock-capture-apply` —— 实测 **24 条红**（渲染状态 ×2 /
  //     纹理 / FVF / 变换 / 采样器 / 纹理级 / 视口 / 裁剪矩形，在"建块"与"Capture"两段各来一遍，
  //     外加 PIXEL 段的回滚断言）。⚠ 这一条**不覆盖**快照持引用那半（`sbHoldRefs` 的 addRef 在
  //     capture 里，Apply 短路碰不到它）—— 引用/生命周期由同一个用例的 ④ 段守，另见 §"引用"。
  {
    file: 'apps/emulator-lift/src/runtime/d3d9_pipeline.cpp',
    from: '    const SbMask mask = sbMaskOf(snap.type);\n' + '    bool failed = false;',
    to:
      '    const SbMask mask = sbMaskOf(snap.type);\n' +
      '    if (cpu.mem != nullptr) return kOk;  // 变异：Apply 不写回（= 修前形状）\n' +
      '    bool failed = false;',
    guard: 'ctest:d3d9',
    ctest: 'd3d9',
    buildDir: 'apps/emulator-lift/out',
    buildTarget: 'lift-d3d9',
    what:
      'Apply 不写回（修前形状）⇒ d3d9 的 stateblock-capture-apply 红：Apply 之后渲染状态 / ' +
      '纹理 / FVF / 变换 / 采样器 / 纹理级 / 视口 / 裁剪矩形仍停在"改后"的值（实测 24 条断言红）',
  },


  // ── emulator-lift · marker 事件源（`TriggerKind::Marker`）──


  // ── emulator-lift · DDraw surface 销毁路径的 arena **回收判据** ──
  //   缺陷形态（REQ-01M6ZY8SZRR3KA92ZC9QBNQ349）：销毁路径"arena 该不该回收"的判据曾取进程级
  //   `activeMachine()` ⇒ 判据一失败（另一台 Machine 抢过 g_active / 上一台已析构），那块 arena 就
  //   **永久没有回收入口**（magic 已先被清，谁也再进不了 destroySurfaceState）。
  //   2026-11-14 的 shim/Machine 解耦把归属换成 `hostOf(cpu)`（+ `HostServices::ownsCpu`）⇒
  //   **"换回 activeMachine()"这个旧变异已无法表达**：`machine.h` 不在本 TU 的 include 图里，
  //   块作用域声明又会落进 `lift::ddraw`（链接失败）—— 2026-11-15 两种写法都实测为构建失败
  //   （= 工具记「无法判定」，等于没验）。
  //   ★ 同一轮还**证伪了"弱化回只判非空"这个方向**：把 `requireMachine` 的 `ownsCpu` 判据去掉后
  //     `ctest:ddraw` 仍然**全绿** —— 没有任何 ddraw CTest 用 `X86` 副本调 ddraw 入口（F1 当时对
  //     feeder 副本的验证是一个**不入库的临时探针**，见 `.tmp/aot/f1/RESULT.md` §④），
  //     连 `surfaceLockCount` 那处 `ownsCpu` 也没有用例覆盖 ⇒ 那个方向今天一条守卫也红不了。
  //   ⇒ 本条目改为钉住这条用例**真正断言的那个不变量**：回收必须真的落到那块 arena 上
  //     （`surfaces().destroy(arena_ea)`，key = `[state+kSfArena]`）。变异 = 把 key 换成状态块 EA
  //     ⇒ 回收变成空操作（后果与缺陷形态逐字相同："arena 永久没有回收入口"）。
  //   ★ 实测（2026-11-15）：`ctest:ddraw` 退出码 8；只红 2 条断言、且**只**在
  //     `ddraw/release-owner-not-active`（:847 / :848 的 surfaceCount / bytesInUse）—— 其它 ddraw
  //     用例没有"回收之后回基线"的断言 ⇒ 这条变异唯一地由本用例抓住。
  //   ★ 不写 `if (false)` 那类短路：`/W4 /WX` 的 C4127 会把构建弄失败（会被记成"无法判定"）。
  {
    file: 'apps/emulator-lift/src/runtime/ddraw_surface.cpp',
    from: '            m->surfaces().destroy(arena_ea);\n',
    to:
      '            m->surfaces().destroy(state);  // 变异：回收 key 换成状态块 EA（≠ arena EA）' +
      ' ⇒ arena 永久漏\n',
    guard: 'ctest:ddraw',
    ctest: 'ddraw',
    buildDir: 'apps/emulator-lift/out',
    buildTarget: 'lift-ddraw',
    what:
      'arena 回收打不到 arena（key 用状态块 EA，而不是 `[state+kSfArena]`）⇒ ddraw 的 ' +
      'release-owner-not-active 红（实测 2 条断言：arena 的 surfaceCount / bytesInUse 停在' +
      '"建完之后" ⇒ 协议上等于永久泄漏）',
  },

  // ── `RaiseException` / `RtlUnwind` 的派发（`REQ-01M708DF24KY0QZNEHY66S0HYW`）──
  //   ★ 这条钉的正是本单要消灭的病：**未处理异常静默收场**（= 修前那种"一抛就没人管、进程照常"）。
  //     `seh` 的 B 组断言"两层 record 都返 0 ⇒ reason == UnhandledException"；静默后理由变成
  //     `decode-error`（合成返回点不可解码）⇒ 当场红。⛔ 不用 `if (false)`：`return out` 精确表达
  //     "那一层派发被悄悄摘掉、没人往上报"。
  {
    file: 'apps/emulator-lift/src/runtime/seh.cpp',
    from:
      '                    levels_.clear();\n' +
      '                    pending_dispatch_ = false;\n' +
      '                    ++unhandled_;\n' +
      '                    return stop(out, why, buf);\n' +
      '                }\n' +
      '                continue;  // 去派发下一条',
    to:
      '                    levels_.clear();\n' +
      '                    pending_dispatch_ = false;\n' +
      '                    return out;  // 变异：未处理异常被悄悄摘掉（= 修前那种 no-op）\n' +
      '                }\n' +
      '                continue;  // 去派发下一条',
    guard: 'ctest:seh',
    ctest: 'seh',
    buildDir: 'apps/emulator-lift/out',
    buildTarget: 'lift-seh',
    what:
      '`fs:[0]` 链走到底时不再报 `StopReason::UnhandledException`，而是把那一层派发悄悄摘掉 ⇒ ' +
      '`seh` 的 B 组（两层 record 都返 0 必须非 ok 停机）红：理由变成 decode-error、' +
      '`unhandledCount()` 也停在 0（"异常没人接"重新变成静默继续）',
  },
  // ── `EXCEPTION_NONCONTINUABLE` + `disposition=1` ⇒ 响亮停（`REQ-01M72GAA7CVKBPP9QRT0PRPBTM`）──
  //   ★ 这条钉的正是缺陷单的现场：**非连续异常被 handler 要求继续执行时，本线照字面装载
  //     CONTEXT** ⇒ 回到 raise 的下一条 ⇒ 异常被静默丢弃（最后落进 `_Xout_of_range` 尾部的
  //     `int3`，报成误导性的"软件中断"）。变异 = 让新判据短路（= 修前形状）。
  //   ★ ⛔ 不写 `if (false)`：本工程 `/W4 /WX`，编译期恒假条件触发 C4127 ⇒ 变异体构建失败
  //     （会被记成"无法判定"而不是红）。`cpu.mem == nullptr` 在这里真的恒假（前面已用过 mem），
  //     但编译器证明不了 ⇒ 既不断言也不告警（同 d3dx9 那条变异的先例）。
  //   ★ 判据：`seh` 的 E 组（`E-noncontinuable-continue-is-loud`）—— 短路后理由不再是
  //     `noncontinuable-exception`，且 `after_ea` 被写成坏标记（异常被"继续"掉了）。
  {
    file: 'apps/emulator-lift/src/runtime/seh.cpp',
    from: '                    if ((rec_flags & kExceptionNonContinuable) != 0) {',
    to:
      '                    if ((rec_flags & kExceptionNonContinuable) != 0 && cpu.mem == nullptr) {  ' +
      '// 变异：NONCONTINUABLE 判据被短路（= 修前"照样装载 CONTEXT"）',
    guard: 'ctest:seh',
    ctest: 'seh',
    buildDir: 'apps/emulator-lift/out',
    buildTarget: 'lift-seh',
    what:
      '`EXCEPTION_NONCONTINUABLE` 上 handler 返回 `disposition=1` 又被当成"装载 CONTEXT 继续执行" ⇒ ' +
      '`seh` 的 E 组红：停因不再是 `noncontinuable-exception`（落到 decode-error / no-code），' +
      '`after_ea` 也被写成坏标记 —— 正是缺陷单里"异常被静默丢弃、最后报成软件中断"的前一步',
  },
  // ── emulator-lift · `D3DXCreateTextureFromFileInMemory` 的 **PNG 解码 + 上传** ──────────────
  //   缺陷形态（`REQ-01M738VYG0DP3TNJKYDSA49J7H`）：解码成功、但**不上传像素** ⇒ 回到修前的
  //   "只建不写"：三张 2048² Live2D 图集页（`texserial=21/22/23`）永远全 0，所有采样它们的
  //   固定功能批次不可见（父单 `REQ-01M735AWXRDZKCDT4124BCFF27` 的根因）。
  //   变异 = 让 `uploadRgbaToTexture` **根本不执行**：注入**运行时**恒假条件 `cpu.mem == nullptr`。
  //   ★ ⛔ 不写 `if (false)`：本工程 `/W4 /WX`，编译期恒假的条件会走 C4127 ⇒ 变异体构建失败 ⇒
  //     被记成"无法判定"而不是红。`cpu.mem == nullptr` 在这里真的恒假（前面已经解引用过），
  //     但编译器证明不了 ⇒ 既不断言也不告警。
  //   ★ ⛔ 也别把它写成 `!(cpu.mem == nullptr) && upload(...)`：那**仍然会调用** upload（只是布尔值
  //     取反），是一条**无效变异**（改了等于没改，会记成"守卫没红"）。
  //   判据：`tests/d3dx9.cpp` 的 `d3dx9/texture-from-memory-png` —— 逐像素（四角/中心/两个非角点）
  //   + `locks==1` / `write_version==version` / 写区 == 整幅 一起红（⛔ 不断言版本号的具体值）。
  {
    file: 'apps/emulator-lift/src/shims/d3dx9.cpp',
    from: '        if (!uploadRgbaToTexture(cpu, state, decoded, &upload_why)) {',
    to:
      '        if (cpu.mem == nullptr && !uploadRgbaToTexture(cpu, state, decoded, &upload_why)) {  ' +
      '// 变异：PNG 解码成功但不上传像素（回到修前的"只建不写"）\n',
    guard: 'ctest:d3dx9',
    ctest: 'd3dx9',
    buildDir: 'apps/emulator-lift/out',
    buildTarget: 'lift-d3dx9',
    what:
      'PNG 解码成功但**不上传像素**（回到"只建不写"）⇒ `d3dx9/texture-from-memory-png` 红：' +
      '四角/中心/两个非角点的逐像素断言全红（期望 R=16x+8 / G=16y+8 / B=240-16x / A=8+32x 的 ' +
      '0xAARRGGBB，拿到全 0）、`locks` 期望 1 得 0、`write_version==version` 假、写区 (0,0,8,8) 全 0 ' +
      '—— 正是"建了但一个像素都没写进去"的缺陷形态',
  },

  {
    // ★ 代表 "mip 选级失效" 这一类错（REQ-01M73G2724479XAVS176WBZ65Q）：LOD 恒取 0
    //   ⇒ 缩小时仍只采 level 0 —— 那正是本单要修的那条保真偏离（"没有 mip 链"）。
    //   守卫：`apps/emulator-lift/tests/d3d9_mip.cpp` 的 `d3d9-mip/lod-levels`（1:1 / 2:1 / 4:1 的级号）。
    //   ★ 不用 `if (false)`：那会把"级号恒 0"写成"整条判据消失"；`lod * 0.0` 精确表达
    //     "LOD 丢了"，同时 `lod` / `levels` 两个参数都仍被使用（/W4 /WX 下无未用告警）。
    file: 'apps/emulator-lift/src/runtime/d3d9_pipeline.cpp',
    from: '    const double biased = std::floor(lod + 0.5);',
    to: '    const double biased = std::floor(lod * 0.0 + 0.5);',
    guard: 'ctest:d3d9-mip',
    ctest: 'd3d9-mip',
    buildDir: 'apps/emulator-lift/out',
    buildTarget: 'lift-d3d9-mip',
    what:
      'LOD 恒取 0（`mipLevelForLod` 的 `lod + 0.5` 变成 `lod*0.0 + 0.5`）⇒ 缩小时仍只采 level 0：' +
      'd3d9-mip 的 2:1 / 4:1 级号断言当场红（实测见 .tmp/reveq/REQ-01M73G2724479XAVS176WBZ65Q/report.md）',
  },


  {
    // ★ 代表 "x87 float→int64 的**截断方向**被改掉" 这一类错（REQ-01M75PW4MXS06MN6FKGXB36ETH，`__ftol2_sse`）。
    //   `__ftol2` 的修正环（`.lst` 0x4E8319..0x4E833B 的 `fstp dword` + `add 7FFFFFFFh` + `sbb/adc`）
    //   把 `fistp` 的"按 RC 就近取整"拉回**向零截断** ⇒ 实现里的 `x87ToI64Trunc(x)`
    //   （= `x87ToI64(x, X87Round::Zero)`）是这一项语义的唯一落点；改成 `X87Round::Near`（就近）
    //   ⇒ `±1.5` / `±2.7` 这类小数部分 ≥0.5 的输入整数值差 1。
    //   守卫：`apps/emulator-lift/tests/native_override_assets.cpp` 的 `ftol/*` 边界表
    //   （解释器臂 = 真字节 oracle，逐值比 `EDX:EAX`）。
    //   ★ 为什么挑这一条而不是"忘弹栈"：截断是**本项的核心语义**，而"忘弹栈"更多是框架 ABI 的
    //     共性错；另外这条变异**只有在边界表带非整数/非 2 的幂时才红**（`±0.4` 那种两边都给 0）
    //     ⇒ 它同时钉住"边界表必须有判别力"这件事。
    //   ★ 2026-11-16：桥函数已改名为 `x87ToI64Trunc`（旧 `x87ToI64(x, /*truncate=*/true)` 不存在）
    //     ⇒ 更新 `from`，破坏语义不变（截断 → 就近）。
    file: 'apps/emulator-lift/src/runtime/native_crt.cpp',
    from: '    const std::int64_t n = x87ToI64Trunc(x);',
    to: '    const std::int64_t n = x87ToI64(x, X87Round::Near);',
    guard: 'ctest:native-override-assets',
    ctest: 'native-override-assets',
    buildDir: 'apps/emulator-lift/out',
    buildTarget: 'lift-native-override-assets',
    what:
      '`__ftol2_sse` 的截断改成"按 RC 就近取整"（`x87ToI64(x, X87Round::Near)`）⇒ `±1.5`/`±2.7` 差 1：' +
      'native-override-assets 的 ftol 边界表逐值断言当场红',
  },


  {
    // ★ 代表 "Huffman 两条分支的 **adv 字节数**被写反" 这一类错
    //   （REQ-01M75WED0VFTEYXX43KGHNPPP0，`sub_4E1570` 的 `0x4E1666` vs `0x4E167C`）。
    //   `.lst` 里成功支 `adv(codelengths[lo])`、失败支 `adv(read)` —— 两个数**不同**；
    //   谁按"标准 Huffman"想当然写成同一个，位游标就错 N bit ⇒ 整包之后全错位
    //   （听得出、引擎零信号：`0x4E40A9 xor eax,eax` 让成功与失败都返回 0）。
    //   守卫：`apps/emulator-lift/tests/native_override_assets.cpp` 的 `huff/fail-len-gt-read`
    //   （解释器臂 = 真字节 oracle，逐值比位读三个 dword）与 `huff-vec/eof-fail`。
    //   ★ 为什么挑这一条而不是"sar 写成 shr"：`sar`/`shr` 只在负 entry 的高位字段上分叉，
    //     而 adv 字节数是**两片叶子的公共骨架**、也是本单的头号风险（评估 §2.4.1 第 2 条）。
    file: 'apps/emulator-lift/src/runtime/native_huffman.cpp',
    from: '        huffAdv(r, bits);  // `0x4E1681 adv(read)`  ← ★ 与成功支的字节数**不同**',
    to: '        huffAdv(r, len);  // 变异：失败支也按码长前进（= adv 字节数写反）',
    guard: 'ctest:native-override-assets',
    ctest: 'native-override-assets',
    buildDir: 'apps/emulator-lift/out',
    buildTarget: 'lift-native-override-assets',
    what:
      '`sub_4E1570` 失败支的 `adv(read)` 改成 `adv(codelengths[lo])`（两条分支的字节数写成同一个）' +
      '⇒ 位游标错位：native-override-assets 的 `huff/fail-len-gt-read` 当场红' +
      '（位读三 dword 的 endbit / ptr 与解释器臂不同）',
  },

  {
    // ★ 代表 "CRC 反射变体的**移位方向**被写反" 这一类错（REQ-01M7BY4D1ZWA4HB97T8M8TVEYQ，
    //   `sub_436D00` 的 `0x436D39 shr eax,8`）。
    //   反射变体的整条语义就是 `crc = T400[(crc ^ b) & 0xFF] ^ (crc >> 8)`；`>>` 写成 `<<`
    //   仍会"有个返回值"、仍会读同一张表 ⇒ 不崩不报，只是**每一条 CRC 都不对**，而引擎侧零信号
    //   （存档写出去、读回来对不上）。
    //   守卫：`apps/emulator-lift/tests/native_override_assets.cpp` 的 `crc/d00-reflected-*`
    //   （解释器臂 = 同一 EA 的真字节 oracle，逐位比 `EAX`；全语料里唯一能当 oracle 的东西）。
    //   ★ 为什么挑这一条而不是"让两个 fn 都查 `+0x400`"：后者要同时改两处（`table_offset` 与
    //     `reflected`），是两处破坏；这一条是**一处**且只让 native 臂分叉（`d50` 不受影响，
    //     红的方向唯一）。
    file: 'apps/emulator-lift/src/runtime/native_crc32.cpp',
    from: 'for (std::uint32_t k = 0; k < n; ++k) crc = table[(crc ^ buf[k]) & 0xFFu] ^ (crc >> 8);',
    to: 'for (std::uint32_t k = 0; k < n; ++k) crc = table[(crc ^ buf[k]) & 0xFFu] ^ (crc << 8);',
    guard: 'ctest:native-override-assets',
    ctest: 'native-override-assets',
    buildDir: 'apps/emulator-lift/out',
    buildTarget: 'lift-native-override-assets',
    what:
      '反射变体（`sub_436D00`）的 `crc >> 8` 写成 `crc << 8`（`.lst` 0x436D39 `shr eax,8` 写反）⇒ ' +
      'native-override-assets 的 `crc/d00-reflected-*` 与 `crc/d00-standard-value` 当场红' +
      '（`crc/d50-msb-*` 不受影响 ⇒ 红的方向唯一）',
  },

  {
    // ★ 代表 "音频第 3 层补偿的**覆盖面**只挂在 stop/setPosition 上" 这一类错
    //   （`REQ-01M75RD3WNA2GZQY32S34V00S7`）：`removeBuffer` 在模型里 erase 掉这条流就返回，
    //   而 `destroyBufferBlocks`（`shims/dsound.cpp`）紧接着 `heap->free` 掉 guest 环与状态块
    //   ⇒ 在飞那一刀 continues 往已 free 的 guest 堆块里写（不只是"写回旧相位"）。
    //   变异 = 把新加的那句有界等待**删掉**（同形于"补丁没落上"）。
    //   判据：`apps/emulator-lift/tests/audio.cpp` 的 `audio-device/remove-waits-inflight` ——
    //   夹具 seam 伪造 `filling_obj_`，另一条线程 20 ms 后清 0；删掉等待 ⇒ `fill_waits` 保持 0、
    //   耗时 < 15 ms ⇒ 当场红。★ 读数只作存在性判据（不锁具体毫秒数）。
    //   ★ 为什么不用 `if (false && …)`：那会在 `/W4 /WX` 下定死一个常量条件（C4127 的风险），
    //     而"整句替换成注释"精确表达"补丁没落上"，且 `obj` / `lock` 仍被别的语句使用（无未用告警）。
    file: 'apps/emulator-lift/src/runtime/audio.cpp',
    from: '        if (!isFeederThread() && filling_obj_.load() == obj) waitFill(obj, lock);',
    to: '        // 变异：removeBuffer 不再等"正在跑的那一刀 fill"（挡在 heap->free 之前的判据消失）',
    guard: 'ctest:audio-device',
    ctest: 'audio-device',
    buildDir: 'apps/emulator-lift/out',
    buildTarget: 'lift-audio',
    what:
      '`removeBuffer` 不再有界等"正在跑的那一刀 fill"（erase/dropPending 之后直接返回）⇒ ' +
      '`audio-device/remove-waits-inflight` 的 `fill_waits == 1` / 耗时 ≥ 15 ms 当场红',
  },


  {
    // ★ 代表"**反馈环闸被删掉**"这一类错（`REQ-01M79MSMWGFEKF8J7C7VYWS4YP` 的新闸）：
    //   同一批把 RT 纹理当采样源时，本后端采样走**上传缓存**（不是 target 自己）⇒ 读到的是本批
    //   之前的内容：不挂死、不报错、像素是旧帧（**静默**）。删掉这条闸 ⇒ 该批照样进 GPU。
    //   守卫：`apps/emulator-lift/tests/gpu_batch.cpp` 的 `d3d9p/render-to-texture-feedback`
    //   （`queued_a==0 ∧ reason 含"被采样"`；控制组解绑采样后必须进 GPU）。
    //   ★ 变异 = 闸的条件再 `&& color_base == nullptr`（RT 分支里 `color_base` 恒非空 ⇒ 闸永不触发）；
    //     ⛔ 不写 `&& false`（`/W4 /WX` 的常量条件告警）。
    file: 'apps/emulator-lift/src/runtime/d3d9_pipeline.cpp',
    // ★ `from` 必须带上前一行：P2 在**同一个 if 块**里给 effect 路的采样源加了**同样一行**
    //   `if (rt_sampled) {`（`ctx.effect_textures[s]`）⇒ 只写那一行会命中 2 次
    //   （`--list` 的静态检查当场判「出现次数 ≠ 1」）。判据 = 固定功能路那一份
    //   （`ctx.textures[s].pixels == ctx.color_base`）。
    from:
      '                                    ctx.textures[s].pixels == ctx.color_base;\n' +
      '            if (rt_sampled) {',
    to:
      '                                    ctx.textures[s].pixels == ctx.color_base;\n' +
      '            if (rt_sampled && ctx.color_base == nullptr) {',
    guard: 'ctest:gpu-batch-order',
    ctest: 'gpu-batch-order',
    buildDir: 'apps/emulator-lift/out',
    buildTarget: 'lift-gpu-batch',
    what:
      '反馈环闸失效（同批把 RT 当采样纹理不再被拒）⇒ `d3d9p/render-to-texture-feedback` 的 ' +
      '`queued_a==0` / reason 断言当场红',
  },

  {
    // ★ 代表"**sprite 路自己的反馈环闸**被删掉"这一类错（`REQ-01M79ZRTHWWD296XRY5KYPR9J4`）：
    //   sprite 合成**不经** `gpuEligible` ⇒ draw 路那条闸（上面那条变异）管不到它。
    //   `REQ-01M79V2TVJH98BKH0C0WQV2FET`（sprite→RT）之后，sprite 可以采样**自己正在写的** RT
    //   （`tbase == base`）：GPU 采样读上传缓存、guest 也可能没读回本批像素 ⇒ 静默旧像素。
    //   守卫：`apps/emulator-lift/tests/gpu_batch.cpp` 的 `d3d9p/sprite-feedback-loop`
    //   （反馈环条目 `queued==0` + `sprite_skipped==1` + 具名位；反向：别的纹理必须照常入队）。
    //   ★ 变异 = 判据换成**永不成立的**比较（`base + 1`）；⛔ 不写 `&& false`（`/W4 /WX` 的常量条件
    //     告警会让它变成"不可判定"而不是"守卫红了"）。
    file: 'apps/emulator-lift/src/runtime/d3dx9_sprite.cpp',
    from: '        if (texture != 0 && tbase != nullptr && tbase == base) {',
    to: '        if (texture != 0 && tbase != nullptr && tbase == base + 1) {',
    guard: 'ctest:gpu-batch-order',
    ctest: 'gpu-batch-order',
    buildDir: 'apps/emulator-lift/out',
    buildTarget: 'lift-gpu-batch',
    what:
      'sprite 采样自己正在写的 RT 不再被跳过（判据失效）⇒ `d3d9p/sprite-feedback-loop` 的 ' +
      '`queued==0` / `sprite_skipped==1` / 具名位断言当场红',
  },

  {
    // ★ 代表"**每次换 target 都清深度**"这一类错（RTT 复审的"必须改 ②"）
    //   （`REQ-01M79MSMWGFEKF8J7C7VYWS4YP` 的新语义）：D3D9 的深度缓冲**属设备**、不随 color target
    //   变；旧行为在 `rasterPrepare` 换 target 时顺手把 `need_depth_clear_` 置位 ⇒ 同尺寸切 RT 也会
    //   把 A 写的深度清成 1.0：之后 z 更远的片元被错误接受（像素静默错，且三套旧 GPU 用例全绿）。
    //   守卫：`apps/emulator-lift/tests/gpu_batch.cpp` 的
    //   `d3d9p/depth-retained-across-target-switch`（A 写 z=0.5 ⇒ 切 B ⇒ z=0.7 必须被拒：B 仍是蓝；
    //   反向：显式 Clear(ZBUFFER) 后同一笔 z=0.7 必须画出来 ⇒ 排除"永远拒绝"的假绿）。
    //   ★ 变异 = 在 `destroyColorTarget()` 之后无条件置位（正是修前那一行的位置）。
    //     守卫在 `--gpu` 臂（`ctest:gpu-batch-barrier`）；无 GPU ⇒ SKIP 77 = 不可判定，⛔ 不是红。
    file: 'apps/emulator-lift/src/runtime/d3d9_gpu.cpp',
    from: '        destroyColorTarget();\n        width_ = width;',
    to:
      '        destroyColorTarget();\n' +
      '        need_depth_clear_ = true;  // 变异：换 target 就清深度（修前行为）\n' +
      '        width_ = width;',
    guard: 'ctest:gpu-batch-barrier',
    ctest: 'gpu-batch-barrier',
    buildDir: 'apps/emulator-lift/out',
    buildTarget: 'lift-gpu-batch',
    ctestTimeoutSec: 300,
    what:
      '换 target 无条件清深度 ⇒ A 写的 z=0.5 在切到 B 后被清成 1.0，B 上 z=0.7 的片元被错误接受 ' +
      '⇒ `d3d9p/depth-retained-across-target-switch` 的第一条断言（B 仍是清屏蓝）当场红',
  },

  {
    // ★ 代表"**SetRenderState 的 default 又把未知状态静默存下**"这一类错
    //   （`REQ-01M79PPA10JCKG4C9HY6QMV6PT`）：default 不拒 ⇒ 214 这种非 D3DRS 值被 setRs 写进
    //   状态块（`Get(214)` 也能读回）⇒ 参数校验退化成"什么都收"。
    //   守卫：`apps/emulator-lift/tests/d3d9_pipeline.cpp` 的 `d3d9p/render-state-policy` (3)
    //   （`Set(214/210/255,1)` INVALIDCALL + guest 堆逐字节不变 + `Get(214)` INVALIDCALL）
    //   与 `d3d9p/render-state-domain` 的"非 D3DRS 值 ⇒ INVALIDCALL 且不记账"段。
    //   ★ `/W4 /WX` 下不能写 `if (false)` ⇒ 用运行期表达式 `state == 0xFFFFFFFFu`
    //     （上界已先拒 210..255 ⇒ 这个合取恒假，但编译器不会报常量条件）。
    file: 'apps/emulator-lift/src/runtime/d3d9_pipeline.cpp',
    from: '            if (!knownRenderState(state)) {\n                char buf[192];',
    to: '            if (!knownRenderState(state) && state == 0xFFFFFFFFu) {\n                char buf[192];',
    guard: 'ctest:d3d9-pipeline',
    ctest: 'd3d9-pipeline',
    buildDir: 'apps/emulator-lift/out',
    buildTarget: 'lift-d3d9-pipeline',
    what:
      'default 不再拒绝白名单外状态 ⇒ `Set(2,1)` 被存下（guest 堆变了 / Set(2) 不再是 INVALIDCALL）' +
      '⇒ `d3d9p/render-state-policy` (3) 与 `d3d9p/render-state-domain` 的"非 D3DRS"段当场红',
  },

  {
    // ★ 代表"**合法 D3DRS 又被当成非法拒掉**"这一类错（`REQ-01M7A3M73R3XYMM7J4KNQ1VNV7`）：
    //   修前 `knownRenderState` 的 A∪B 白名单把 163（`D3DRS_PATCHEDGESTYLE`，官方枚举里有）
    //   落进 default 的"不是 D3DRS"⇒ INVALIDCALL。本单把它（连同 48 / 146–148 / 165 /
    //   172–173 / 178–183 / 198–205）收进 B 档：接受 + 原样存下 + `recordUnsupported(bit41)`。
    //   变异 = 把 163 这一行注释掉（= 挪回拒绝档）。
    //   守卫：`d3d9p/render-state-domain` 的"合法 D3DRS"段 —— 每个合法值要么 `Set == S_OK`，
    //   要么"INVALIDCALL + recordUnsupported 命中"；163 两条都不满足 ⇒ 当场红。
    file: 'apps/emulator-lift/src/runtime/d3d9_pipeline.cpp',
    from: '        case 163:                                                    // PATCHEDGESTYLE\n',
    to: '        // case 163:  // 变异：把这个 B 档值挪进拒绝档（default ⇒ INVALIDCALL）\n',
    guard: 'ctest:d3d9-pipeline',
    ctest: 'd3d9-pipeline',
    buildDir: 'apps/emulator-lift/out',
    buildTarget: 'lift-d3d9-pipeline',
    what:
      '`D3DRS_PATCHEDGESTYLE=163` 从 B 档（接受并存下）挪进拒绝档 ⇒ `Set(163, …)` 返回 ' +
      'D3DERR_INVALIDCALL 且 bit41 不置位 ⇒ `d3d9p/render-state-domain` 的"合法 D3DRS"段当场红',
  },


  // ── 光照材质来源：`D3DRS_COLORVERTEX=0` 必须让顶点色**不参与**
  //    （`REQ-01M7460MRJ6D82PWG6P6MX9WG9`）─────────────────────────────────────────────
  {
    // ★ 代表"COLORVERTEX 只存不读"这一类错（本单修前的形状：`transformVertex` 只按
    //   `DIFFUSEMATERIALSOURCE == 1` 乘顶点色，完全不看 141）。
    //   变异 = 把选择函数的 141 门控拆掉 ⇒ COLORVERTEX=0 时仍返回 COLOR1 的顶点色。
    //   ★ 不用 `if (true)`：本工程 /W4 /WX，编译期恒真条件会走 C4127 ⇒ 变异体构建失败
    //     （记成"不可判定"而不是红）。`material_rgba != nullptr` 是运行时条件、且 `color_vertex`
    //     仍被用到（不引入未使用参数告警）。
    //   守卫：`tests/d3d9_pipeline.cpp` 的 `d3d9p/material-source` ① —— COLORVERTEX=0 +
    //   DIFFUSEMATERIALSOURCE=COLOR1 必须回材质蓝；变异后拿到顶点红。
    file: 'apps/emulator-lift/src/runtime/d3d9_pipeline.cpp',
    from: '    if (color_vertex != 0u) {',
    to: '    if (material_rgba != nullptr && color_vertex + 1u != 0u) {  // 变异：COLORVERTEX 不再门控',
    guard: 'ctest:d3d9-pipeline',
    ctest: 'd3d9-pipeline',
    buildDir: 'apps/emulator-lift/out',
    buildTarget: 'lift-d3d9-pipeline',
    what:
      '材质来源选择不再看 `D3DRS_COLORVERTEX` ⇒ `COLORVERTEX=0` 时顶点色仍参与光照（本单缺陷形态）' +
      '⇒ `d3d9p/material-source` ① 红：期望材质蓝 (0,0,1,1)，实际拿到顶点红 (1,0,0,1)',
  },

  // ── 颜色目标的**唯一判据**：`colorTargetOfState` 恒回后缓冲
  //    （`REQ-01M79ZRTXJCG303FKVEBFMNFPJ`）───────────────────────────────────────────
  {
    // ★ 代表"两份判据一份规则"这一类错：`Clear` / `unlockRect` 已收口到 `colorTargetOfState`，
    //   但它自己若把 arena 恒置 0（后缓冲），纹理 RT 的 Draw 会画进后缓冲镜像。
    //   守卫：`gpu-batch-order` 的 `d3d9p/render-to-texture` —— `rasterTargetBase()` 必须是
    //   **纹理像素**（变异后是后缓冲像素 ⇒ 当场红）。
    file: 'apps/emulator-lift/src/runtime/d3d9_pipeline.cpp',
    from: '    out->arena = plGet(cpu, pipeline_state, kPlColorArena);',
    to: '    out->arena = 0u;  // 变异：颜色目标恒回后缓冲（旁路判据）',
    guard: 'ctest:gpu-batch-order',
    ctest: 'gpu-batch-order',
    buildDir: 'apps/emulator-lift/out',
    buildTarget: 'lift-gpu-batch',
    what:
      '`colorTargetOfState` 的 arena 恒为 0（后缓冲）⇒ 绑定纹理 RT 的 Draw 把目标身份判成后缓冲 ' +
      '⇒ `d3d9p/render-to-texture` 的 `gpu_base == tex_pixels` 红（像素静默写错 surface）',
  },

  {
    // ★ 代表"**BaseVertexIndex 被忽略 / 只按 idx 取顶点**"这一类错
    //   （`REQ-01M79PP9S4BFBPKDPBQBDN5GGR`）：`vi = base + idx - min` 等价于旧的"忽略 min"，
    //   负 base 的合法索引区间 [min,min+n) 会被算成负顶点号 ⇒ 合法调用被假拒。
    //   守卫：`d3d9p/indexed-range` (a)(b)（base=-2/min=2、base=-1/min=1 必须 S_OK 且像素正确）。
    file: 'apps/emulator-lift/src/runtime/d3d9_pipeline.cpp',
    from:
      '        const std::int64_t vertex_index = static_cast<std::int64_t>(base_vertex) + static_cast<std::int64_t>(idx);',
    to:
      '        const std::int64_t vertex_index = static_cast<std::int64_t>(base_vertex) + static_cast<std::int64_t>(idx) - min64;',
    guard: 'ctest:d3d9-pipeline-gpu',
    ctest: 'd3d9-pipeline-gpu',
    buildDir: 'apps/emulator-lift/out',
    buildTarget: 'lift-d3d9-pipeline',
    what:
      '`vi = base + idx` 改回忽略 min（减去 min64）⇒ (a)(b) 的负 base 合法调用变成 vi<0 假拒 ' +
      '⇒ `d3d9p/indexed-range` (a)(b) 当场红',
  },

  {
    // ★ 代表"**无 GPU 时又静默降级**"这一类错（`REQ-01M78BF1FNNH6K8N8GEY6GQYXX`）：
    //   把启动期那道"没有 GPU ⇒ 退出 2 + 指路"改成恒不触发 ⇒ 子进程会继续走到"找不到执行源"
    //   （或真的开跑），stderr 里没有那句 GPU 拒绝文案。
    //   守卫：`lift/no-gpu-refuses-start`（`tests/gpu_batch.cpp` 的子进程臂；`SDL_VIDEODRIVER=dummy`
    //   强制"建不出 GPU device"，⛔ 不靠真拔显卡；后端在**执行源定位之前**建 ⇒ 不需要游戏资产）。
    //   ★ 2026-11-16 起该 CTest 的子进程实参是 **AOT 臂**（`$<TARGET_FILE:emulator-lift-aot>`）
    //   ⇒ 变异点必须跟着落到 `aot_main.cpp` 的同一条判据上；指向 `main.cpp` 会变成
    //   "变异施加在解释器上、守卫跑 AOT" ⇒ 变异存活（这条自检就假绿了）。
    file: 'apps/emulator-lift/src/aot/aot_main.cpp',
    from: '        if (!ok || !sdl->rasterActive()) {',
    to: '        if (false) {',
    guard: 'ctest:lift/no-gpu-refuses-start',
    ctest: 'lift/no-gpu-refuses-start',
    buildDir: 'apps/emulator-lift/out',
    buildTarget: 'lift-gpu-batch',
    what:
      '无 GPU 时不再拒绝启动（改回静默继续）⇒ 子进程 exit 2 的原因是"找不到执行源"而不是 GPU ' +
      '⇒ `lift/no-gpu-refuses-start`（AOT 臂）当场红',
  },

  {
    // ★ 代表"**underrun 插静音的判据被改回照读环**"这一类错（`REQ-01M75PTAC1JJ3G4QGQDRB1G4V8`
    //   的实时口径，设计 `.tmp/audio-phase/design.md` §1.4 / §1.6）：
    //   判据 = 写见证 `[stream+0x2468]`（fill 前后变了 = 这一刀写了环）；读指针第 n 次进入
    //   quarter q 时 `witness[q] == consumed[q]` ⇒ 这一圈 q 区没被重写过 ⇒ 该区交付 0。
    //   变异 = 把它改成恒 fresh（`false`）⇒ 第 2 圈照读上一圈的旧内容 = **重播**（正是本单的
    //   产品现象），且 `underrun_silence_events/bytes` 归零。
    //   守卫：`apps/emulator-lift/tests/audio.cpp` 的 `audio-device/underrun-silence-stale`
    //   （Virtual + `sound=true`，不注册 feeder ⇒ 天然 0 刀，恰好 2 圈）。
    //   ⛔ 不用内容比对当判据：guest 自己"放完 + memset 0"也是全 0（`guest-zeros` 用例钉住）。
    file: 'apps/emulator-lift/src/runtime/audio.cpp',
    from: '                    b.read_stale = (b.witness[quarter] == b.consumed[quarter]);',
    to: '                    b.read_stale = false;  // 变异：恒 fresh（环陈旧时照读上一圈的内容 = 重播）',
    guard: 'ctest:audio-device',
    ctest: 'audio-device',
    buildDir: 'apps/emulator-lift/out',
    buildTarget: 'lift-audio',
    what:
      '把"环陈旧 ⇒ 交付静音"的判据改成恒 fresh（照读环）⇒ 第 2 圈是斜坡重播、' +
      'underrun_silence_events/bytes 归零 ⇒ `audio-device/underrun-silence-stale` 当场红',
  },

  // ── 新守卫：`add_test` 的 flag 必须在目标源文件里有分派（`cmake-test-entry`）─────────────
  //   故障形态：给 `add_test` 加一个目标不认识的 flag ⇒ 二进制跑默认套件、退出码 0 ⇒
  //   `ctest` 绿，但那条用例什么新东西都没断言（比红危险）。这里把 `gpu-batch-order`
  //   故意改成带一个不存在的 `--gpu-bogus`：源文件里没有它的字符串字面量 ⇒ 守卫必须红。
  {
    file: 'apps/emulator-lift/CMakeLists.txt',
    from: 'add_test(NAME gpu-batch-order COMMAND lift-gpu-batch)',
    to: 'add_test(NAME gpu-batch-order COMMAND lift-gpu-batch --gpu-bogus)',
    guard: 'tools/test/cmake-test-entry.test.mjs',
    what:
      '给 `gpu-batch-order` 的 add_test 加一个不存在的 `--gpu-bogus`（目标源文件没有字符串字面量）⇒ ' +
      'cmake-test-entry 必须点名 "add_test gpu-batch-order / flag --gpu-bogus / tests/gpu_batch.cpp 找不到" 并红',
  },

  // ── `--cursor-warp`：宿主映射**忘了减 guest 窗口原点** ────────────────────────────────────
  //   代表这一类错：warp 的落点用 guest **屏幕**坐标当客户区坐标（少减 `primaryWindow()` 的 x/y）。
  //   ★ 这种错在 `win.x == win.y == 0` 的全屏式窗口上**完全看不见**（引擎那两处"屏幕中心"调用点后面
  //     紧接 `SetWindowPos(hwnd,-1,0,0,0,0,3)`）⇒ 只能靠非零窗口原点的守卫抓。
  //   判据：`apps/emulator-lift/tests/cursor_warp.cpp` 的
  //     · `cursor-warp/on-calls-once-with-client-coords`（窗口 (100,50)，期望 warp 收到 (220,190)）
  //     · `cursor-warp/no-clamp`（期望 (-105,850)）
  //   两处都会红。★ `from` 是**落地后实际的代码字节**（4 空格缩进）。
  {
    file: 'apps/emulator-lift/src/shims/user32_input.cpp',
    from: '    wm->cursorClientPos(hwnd, &cx, &cy);',
    to: '    wm->getCursorPos(&cx, &cy);  // 变异：忘了减 guest 窗口原点',
    guard: 'ctest:cursor-warp',
    ctest: 'cursor-warp',
    buildDir: 'apps/emulator-lift/out',
    buildTarget: 'lift-cursor-warp',
    what:
      'SetCursorPos 的宿主映射忘了减 guest 窗口原点（把屏幕坐标当客户区坐标送进 warp）⇒ ' +
      'cursor-warp/on-calls-once-with-client-coords 期望 (220,190) 实际 (320,240)、no-clamp 期望 ' +
      '(-105,850) 实际 (-5,900) ⇒ 当场红',
  },

  // ── AOT 臂：`--cursor-warp` **没进 `present_window_flag`** ────────────────────────────────
  //   故障形态 = 解析了 flag、也送进了 `SdlWindowOptions`，但**漏了**"它算窗口参数"这一格
  //   ⇒ `--cursor-warp --present headless` 不再被 headless 闸门拒绝（静默走到建窗口 / 找执行源），
  //   用户看到的还是"给了也没用"。★ 这是本单最轻的漏法：删掉 4 个字符即可，而且**没有任何**
  //   运行期症状（只在参数面）。
  //   判据：`tests/cli_aot_flags.cpp` 的 `headless-rejects-cursor-warp`（stderr 必须是
  //   "`--present headless` 不接受窗口参数（… --cursor-warp）"，⛔ 不含 `未知参数`）。
  //   ★ `from` 是**落地后实际的代码字节**（4 / 37 空格缩进，换行照源文件）。
  {
    file: 'apps/emulator-lift/src/aot/aot_main.cpp',
    from:
      '    const bool present_window_flag = present_driver_set || present_vsync_set || present_hidden ||\n' +
      '                                     present_screenshot_set || cursor_warp || overlay || !ui_script.empty() ||\n' +
      '                                     ui_input_set;',
    to:
      '    const bool present_window_flag = present_driver_set || present_vsync_set || present_hidden ||\n' +
      '                                     present_screenshot_set || overlay || !ui_script.empty() ||\n' +
      '                                     ui_input_set;  // 变异：cursor_warp 不算窗口参数',
    guard: 'ctest:cli-aot-flags',
    ctest: 'cli-aot-flags',
    buildDir: 'apps/emulator-lift/out',
    buildTarget: 'lift-cli-aot-flags',
    what:
      '把 AOT 臂 `present_window_flag` 里的 `|| cursor_warp` 删掉 ⇒ `--cursor-warp --present headless` ' +
      '不再被 headless 闸门拒绝（stderr 变成"找不到执行源 / GPU 拒绝"）⇒ ' +
      'cli-aot-flags/headless-rejects-cursor-warp 的"不含未知参数 + 含 --present headless"当场红',
  },

  // ── advapi32 注册表：`RegOpenKeyExA` 的 `phkResult` 参数校验 ──────────────────────────────
  //   故障形态：把"`phkResult == NULL` ⇒ 87、不写内存"整段去掉，退回"`if (out)` 才写 + 恒 0"
  //   ⇒ 非法参数又被答成 `ERROR_SUCCESS`（这正是 REQ-01M7ATN3WPB14TVZJWZXTPQW3V 修掉的那条）。
  //   ★ 判据 = `tests/advapi32_registry.cpp` 的 `reg-open-null-phkresult`：
  //     期望 0x57 / LastError=0x57，变异后拿到 0 / 0 ⇒ 当场红。
  //   ★ `from` 是**落地后实际的代码字节**（两段一起换，才能让 NULL 既不写内存也不报 87）。
  {
    file: 'apps/emulator-lift/src/shims/advapi32_registry.cpp',
    from:
      '    const std::uint32_t out = call.u32(4);\n' +
      '    if (out == 0) {\n' +
      '        // 实测 87（Win11 x64 / P/Invoke）；参数非法路径**一个字节都不写**。\n' +
      '        cpu.shim.last_error = 87;  // ERROR_INVALID_PARAMETER\n' +
      '        shimLogWarn("advapi32", "RegOpenKeyExA",\n' +
      '                    "phkResult 为 NULL ⇒ ERROR_INVALID_PARAMETER(87)，不写任何内存"\n' +
      '                    "（实测 Win11 x64 / P/Invoke；文档只写 phkResult 是接收句柄的指针）");\n' +
      '        call.result = 87;\n' +
      '        return;\n' +
      '    }\n' +
      '    cpu.mem->wr32(out, kFakeKey);',
    to:
      '    const std::uint32_t out = call.u32(4);\n' +
      '    if (out) cpu.mem->wr32(out, kFakeKey);  // 变异：phkResult 校验整段去掉（NULL 又"成功"）',
    guard: 'ctest:advapi32-registry',
    ctest: 'advapi32-registry',
    buildDir: 'apps/emulator-lift/out',
    buildTarget: 'lift-advapi32-registry',
    what:
      '去掉 RegOpenKeyExA 的 phkResult 参数校验（退回"if (out) 才写 + 恒 ERROR_SUCCESS"）⇒ ' +
      'reg-open-null-phkresult 期望 0x00000057 实际 0x00000000、LastError 期望 0x57 实际 0 ⇒ 当场红',
  },

  // ── PeekMessage 过滤：不匹配的消息**必须留在队列里**（REQ-01M7ATQ4XWSKPW1CKXAHQKNSA2）──────
  //   故障形态 = 修前的"逐条取走不匹配的"（注释当时写"留在队列里会死循环"）：过滤命中之前先把
  //   队首那条 pop 掉 ⇒ guest 永远看不到它（静默丢消息）。变异保留"找第一条匹配的"骨架，只在
  //   遇到不匹配时 pop_front 再放弃 ⇒ 判据必须在**过滤那条**上红。
  //   ★ 判据：`tests/user32_window.cpp` 的
  //     · `peek-filter-keeps-messages`：队列里只有 hwnd=B 的消息，`PeekMessageA(hwnd=A)` 的 0
  //       能骗过第一条断言，但随后 `PeekMessageA(hwnd=B)` 拿到 0 ⇒ 红（消息被吞了）；
  //     · `peek-filter-range`：队首在范围外、第二条在范围内 ⇒ 变异先把队首吞掉，第一条断言就红。
  //   ★ `from` 是**落地后实际的代码字节**（4 / 8 空格缩进）。
  {
    file: 'apps/emulator-lift/src/runtime/host_window.cpp',
    from:
      '    for (auto it = queue_.begin(); it != queue_.end(); ++it) {\n' +
      '        if (pred && !pred(*it)) continue;\n' +
      '        if (out) *out = *it;\n' +
      '        if (remove) queue_.erase(it);\n' +
      '        return true;\n' +
      '    }',
    to:
      '    for (auto it = queue_.begin(); it != queue_.end(); ++it) {\n' +
      '        if (pred && !pred(*it)) {\n' +
      '            if (remove) queue_.pop_front();  // 变异：过滤不匹配也 pop（旧实现的"吞消息"）\n' +
      '            break;\n' +
      '        }\n' +
      '        if (out) *out = *it;\n' +
      '        if (remove) queue_.erase(it);\n' +
      '        return true;\n' +
      '    }',
    guard: 'ctest:user32-window',
    ctest: 'user32-window',
    buildDir: 'apps/emulator-lift/out',
    buildTarget: 'lift-user32-window',
    what:
      '`peekMatching` 遇到不匹配的消息也 `pop_front`（旧实现的"吞消息"）⇒ PeekMessage 过滤会静默丢消息：' +
      '`peek-filter-keeps-messages` 的第二次 Peek(hwnd=B) 拿到 0、`peek-filter-range` 第一条断言拿到 0 ⇒ 当场红',
  },

  // ── 归属判据 `HostServices::ownsCpu`：**恒真** = 只判 `host` 非空 ────────────────
  //   故障形态（F1 修掉的那个**静默**缺陷）：`AudioEngine` 的 feeder 持一份 `X86` 的**整份拷贝**
  //   （`audio.cpp` 的 `audio_cpu_ = *cpu_`）⇒ `host` / `machine` 两个指针被一并复制 ⇒
  //   "`host != nullptr`" 对这份**非主线程的私有副本**同样成立。7 处 ddraw / d3d9 / pipeline / sprite
  //   判据里凡是没有第二道门的，就靠 `host->ownsCpu(cpu)` 挡住它；而 `SurfaceArena` / `GpuRaster`
  //   都**没有锁** ⇒ 放行 = 真数据竞争，且**静默**（不崩、不报、偶发错像素 / 堆结构损坏）。
  //   守卫：`apps/emulator-lift/tests/host_services_owns_cpu.cpp`（CTest `lift-host-services-owns-cpu`）：
  //   ① 真 owner 的那份 cpu ⇒ `ownsCpu == true`；② `X86 copy = machine.cpu()` ⇒ `host` **非空**但
  //   `ownsCpu(copy) == false`；③ 真实入口 `ddraw::createSurfaceState`（第一句就是 `requireMachine`）
  //   对副本**拒绝**（返回 0）而对 owner 成功。
  //   ★ 变异形态**必须吃掉参数**（`(void)cpu; return true;`）：`emulator-lift-runtime` 是 `/W4 /WX`，
  //     裸 `return true;` 会因 `-Wunused-parameter` **编不过** ⇒ 那会被记成「不可判定（构建失败）」，
  //     而不是「守卫红了」（实测：`error: unused parameter 'cpu' [-Werror,-Wunused-parameter]`）。
  //   ⛔ 也不用 `if (false)`：`/W4 /WX` 的 C4127（条件恒真）同样让工具记「无法判定」。
  //   ★ `buildDir` = `apps/emulator-lift/out`（§12.3「构建树只有一个」）。原先写的是 `out-g2`
  //     ——"本轮自己的构建目录"—— 那棵树**早就不存在**了 ⇒ 这条只会永远记「不可判定（其它）」，
  //     一次也证明不了"守卫会红"。
  {
    file: 'apps/emulator-lift/src/runtime/machine.h',
    from: '    bool ownsCpu(const X86& cpu) const override { return &cpu_ == &cpu; }',
    to: '    bool ownsCpu(const X86& cpu) const override { (void)cpu; return true; }  // 变异：恒真（副本也能过）',
    guard: 'ctest:lift-host-services-owns-cpu',
    ctest: 'lift-host-services-owns-cpu',
    buildDir: 'apps/emulator-lift/out',
    buildTarget: 'lift-host-services-owns-cpu',
    what:
      '`Machine::ownsCpu` 恒真（归属判据退化成"只判 host 非空"）⇒ 音频 feeder 的私有 `X86` 副本也能过 ' +
      '⇒ `lift-host-services-owns-cpu` 当场红：② 的两条 `ownsCpu(copy)` 失败 + ③ 的真实入口真的给副本' +
      '建出了一块（返回非 0，而不是被拒的 0）',
  },
  // ---- S1/S3 性能修（2026-11-15）：桩 EA 解析 = 槽位索引表，**存 id 不存槽号** ----
  // ★ 为什么这条值得一条变异：`idForStub` 的返回值就是 `ShimRegistry::invokeById` 的键；把"槽号"
  //   当 id 是**最像对的**那种错（真机上 id 与槽常常恰好相等，夹具里才明显不等），后果是**调错
  //   shim**（静默行为错，不是崩溃）。守卫在 `apps/emulator-lift/tests/shim_stub_index.cpp`：
  //   ① 逐槽 `byStubEa` / `idForStub` 与"扫 defs_ 取第一条"的定义相同；② 已登记 shim 必须走索引
  //   （`stubIndexSlowPathHits` 不动）；③ 页外 / 页内未对齐 ⇒ null；④ addRuntime 追加后旧槽仍指向
  //   同一条（表里存 id / 下标，⛔ 不存 `ShimDef*`）。
  // ★ 用例里**先登记两条无桩的 `add`** ⇒ 桩 id(2,3,4) ≠ 槽号(0,1,2) ⇒ "槽号当 id"必然不等价。
  // ★ `buildDir` = `apps/emulator-lift/out`（§12.3）。原先写 `out-perf1`（那棵树已不存在 ⇒ 永远「不可判定」）。
  {
    file: 'apps/emulator-lift/src/runtime/shims.cpp',
    from: '    return d != nullptr ? d->id : 0u;',
    to: '    return d != nullptr ? static_cast<std::uint32_t>((ea - stubPage()) >> 2) : 0u;  // 变异：把槽号当 id',
    guard: 'ctest:lift-shim-stub-index',
    ctest: 'lift-shim-stub-index',
    buildDir: 'apps/emulator-lift/out',
    buildTarget: 'lift-shim-stub-index',
    what:
      '`idForStub` 返回**槽号**而不是登记 id（本轮唯一的正确性约束就是"id = 注册序 ≠ 槽 = 发号序"）' +
      '⇒ `lift-shim-stub-index` 的逐槽等价当场红：`stub-index-every-slot` / `-after-growth` / ' +
      '`-direct-write` 里 `idForStub(ea)` 与 oracle 的 `def.id` 不相等',
  },
  // ---- `lift_one` 的 EA → Insn 解码缓存（2026-11-15）：**逐字节校验**是它唯一的正确性主张 ----
  // ★ 为什么这条值得一条变异：AOT 臂的 `lift_note_write` 是 **no-op**（`cpu.cache == nullptr`）
  //   ⇒ "guest 写代码页"没有任何钩子。解码缓存的逐字节比较是**这条缺口在解码层面唯一的自愈机制**；
  //   把它删掉（只按 EA 命中）⇒ guest 改了字节之后仍然执行**旧的** Insn（静默跑旧指令：不崩、
  //   不黑屏、`git diff` 也看不见）。
  // ★ 变异形态必须是"**没有**逐字节比较"而不是 `if (false)`：本工程 `/W4 /WX`，编译期恒假的条件
  //   （C4127 / `-Wunused-variable`）会让工具记「不可判定（构建失败）」而不是「守卫红了」。
  //   这里把 `same` 换成一个**运行时**恒真、编译器证明不了的量（`mem.committed(ea,1)` —— 进到这里
  //   之前已经要求过 `committed(ea, 15)`），并顺手删掉 `live`（否则它变成未使用变量）。
  // ★ 守卫：`apps/emulator-lift/tests/decode_cache.cpp`（CTest `lift-decode-cache`）的
  //   `smc-invalidate` 组 —— 改字节后仍命中 ⇒ `cache.invalidated` 停在 0，且返回的 Insn 是改前
  //   那一条（长度 5 / op=Mov / imm=0x12345678），而重新解码给的是长度 1 / op=Nop。
  // ★ `buildDir` = `apps/emulator-lift/out`（§12.3）。原先写 `out-liftone1`（那棵树已不存在 ⇒ 永远「不可判定」）。
  {
    file: 'apps/emulator-lift/src/runtime/decode_cache.cpp',
    from:
      '        const std::uint8_t* live = mem.host(ea);\n' +
      '        bool same = true;\n' +
      '        for (std::uint8_t i = 0; i < e.len; ++i) {\n' +
      '            if (live[i] != e.bytes[i]) {\n' +
      '                same = false;\n' +
      '                break;\n' +
      '            }\n' +
      '        }\n',
    to: '        bool same = mem.committed(ea, 1u);  // 变异：逐字节校验被删掉（只按 EA 命中 ⇒ 陈旧答案）\n',
    guard: 'ctest:lift-decode-cache',
    ctest: 'lift-decode-cache',
    buildDir: 'apps/emulator-lift/out',
    buildTarget: 'lift-decode-cache',
    what:
      '解码缓存命中时不再逐字节校验（只按 EA 命中）⇒ `lift-decode-cache` 的 `smc-invalidate` 红：' +
      '改字节后仍返回改前那条 Insn（长度 5 / op=Mov / imm=0x12345678），`invalidated` 停在 0 —— ' +
      '正是"AOT 臂没有 SMC 钩子"会退化成的那种静默跑旧代码',
  },  // ★ 代表 "x87 抬进 IR 之后**语义写错**"这一类错（检查点 2 / D49 的守卫 `lift-x87-diff`）。
  //   形态选 `fsubr` 的**操作数顺序**：它是"反向减法"（`ST(0) = mem/reg − ST(0)`），写反了
  //   仍然是一条合法的 `fsub`、仍然跑完不崩、300 帧的六项语义指纹也照旧**全绿**（实测：本轮
  //   把 `fsub`/`fsubr` 写反过，`tri` 只差 0.004%）—— 只有逐位差分能抓住它。
  //   判据：`tests/x87_diff.cpp` 的 `fsubr@00430AE6` 组 ⇒ `st(0)` 位型从 `BFF8…`（−1.5）变
  //   `3FF8…`（+1.5），且 guest 内存区随之不同。
  // ⚠ `buildDir` 写 `out`（§12.3：下一轮起唯一的树）。本轮（2026-11-15）实测用的是
  //   `out-d49`（同一份代码 + 同一条命令，红/绿都实测过），见 `.tmp/aot/decisions.md` D49 那条的追加。
  {
    file: 'apps/emulator-lift/src/aot/lift_gen_emit.cpp',
    from: '        case Op::Fsubr: r = bin("fsub", "double", b, a); break;\n',
    to: '        case Op::Fsubr: r = bin("fsub", "double", a, b); break;  // 变异：操作数顺序写反\n',
    guard: 'ctest:lift-x87-diff',
    ctest: 'lift-x87-diff',
    buildDir: 'apps/emulator-lift/out',
    buildTarget: 'lift-x87-diff',
    what:
      'x87 的 `fsubr` 操作数顺序写反（`b−a` → `a−b`）⇒ `lift-x87-diff` 的 `fsubr@00430AE6` 组红：' +
      '`st(0)` 位型 BFF8000000000000（−1.5）变 3FF8000000000000（+1.5），guest 内存区随之不同',
  },
  {
    // ★ 代表 "AOT 臂的逐帧 `[frame]` 读数从缺省档回来" 这一类错（2026-11-16 删 override / 收敛默认输出那轮）。
    //   守卫 `tools/test/aot-arm-shape.test.mjs` ② 的判据是：每条 `std::printf("[frame] …")` 必须落在
    //   `frame_log` / `prof_.on` 的条件里（同一行或前两行）。把闸门 `if (frame_log_on)` 换成 `if (true)`
    //   ⇒ 该 printf 与条件脱钩 ⇒ 缺省档每 Present 一行（300 帧 = 300 行）⇒ 守卫 ② 当场红。
    //   ⛔ 不改成"删掉 printf"：那是把读数整段删掉，守卫 ①（找不到 printf）也会红，但证明不了"闸门"这件事。
    file: 'apps/emulator-lift/src/aot/aot_run.cpp',
    from:
      '            if (frame_log_on) {\n' +
      '                std::printf("[frame] n=%llu hash=0x%016llX\\n", static_cast<unsigned long long>(n),',
    to:
      '            if (true) {  // 变异：逐帧 [frame] 的闸门去掉（缺省档每 Present 一行）\n' +
      '                std::printf("[frame] n=%llu hash=0x%016llX\\n", static_cast<unsigned long long>(n),',
    guard: 'tools/test/aot-arm-shape.test.mjs',
    what:
      '`[frame]` 逐帧 printf 的 `frame_log` 闸门被去掉（`if (frame_log_on)` → `if (true)`）⇒ ' +
      '缺省档每 Present 打一行（300 帧 = 300 行）⇒ aot-arm-shape ② 红：printf 不在 frame_log / prof_.on 条件里',
  },
  {
    // ★ 代表 "启动快照里的 guest 区域表从缺省档回来" 这一类错（2026-11-16 把区域表改 opt-in 那轮）。
    //   守卫 `tools/test/aot-arm-shape.test.mjs` ③ 的判据是**性质**：`regions   :` 与 `dumpRegions(`
    //   必须落在 `if (region_log) { … }` 的块**内**。把闸门换成 `if (true)` ⇒ 那两条打印不在
    //   任何 `region_log` 条件内 ⇒ 缺省档回到整块 28 行（`regions :` + 27 行段表）⇒ 守卫 ③ 当场红。
    //   ⛔ 不改"删掉这两条打印"：那样读数整段消失（丢判据面），证明不了"闸门"这件事。
    file: 'apps/emulator-lift/src/aot/aot_host.cpp',
    from:
      '    if (region_log) {\n' +
      '        std::fprintf(out, "regions   : %zu 段（committed=%llu 字节）\\n", mem_.regions().size(),',
    to:
      '    if (true) {  // 变异：区域表的 region_log 闸门去掉（缺省档打整块 28 行）\n' +
      '        std::fprintf(out, "regions   : %zu 段（committed=%llu 字节）\\n", mem_.regions().size(),',
    guard: 'tools/test/aot-arm-shape.test.mjs',
    what:
      '区域表 28 行的 `region_log` 闸门被去掉（`if (region_log)` → `if (true)`）⇒ 缺省档回到整块 28 行 ' +
      '（`regions   :` 1 行 + 27 行段表）⇒ aot-arm-shape ③ 红：那两条打印不在 `if (region_log)` 的块内',
  },

  // ── 输入投递层 S1（`REQ-01M7G7EMX3JR272D8FVM7JFY9D`；守卫 `tools/test/input-plan-shape.test.mjs`）──
  //   ★ 三条都只跑 `node --test`（⛔ 不加重建整棵树的 CTest 条目）：判据是**源码形态**，
  //     改一处文本就够；C++ 侧的关系断言（CTest `input-plan-read-points`）已由普通 CTest 覆盖。
  {
    // 守卫 ①：把"投递/推进"塞回 `Machine::run` 的块边界 `if`（= 用户点名的旧错误）。
    file: 'apps/emulator-lift/src/runtime/machine.cpp',
    from: '        if (harness_marker || harness_script) {\n',
    to:
      '        if (harness_marker || harness_script) {\n' +
      '            input::deliverLevel(*cpu_.windows, InputPoint::KeyLevel);  // 变异：块边界 pump 回来了\n',
    guard: 'tools/test/input-plan-shape.test.mjs',
    what:
      '在 `Machine::run` 的块边界 `if (harness_marker || harness_script)` 里塞一次 `input::deliverLevel(...)` ' +
      '⇒ 输入推进回到"每个基本块过一次"的老位置 ⇒ input-plan-shape ① 红（`input::` / `deliver(` 命中 ≠ 0）',
  },
  {
    // 守卫 ②：拿掉一个读点的投递入口 ⇒ "读输入态"谓词为真、"调了投递入口"为假（单边为真）。
    file: 'apps/emulator-lift/src/shims/user32_window.cpp',
    from: '    input::deliverLevel(*cpu.windows, InputPoint::KeyLevel);\n',
    to: '    // 变异：GetAsyncKeyState 的投递入口被拿掉（读点不再 flush-before-read）\n',
    guard: 'tools/test/input-plan-shape.test.mjs',
    what:
      '删掉 `shimGetAsyncKeyState` 的 `input::deliverLevel(...)` ⇒ 这条读点绕过投递层（"读影子键电平"=真，' +
      '"调了投递入口"=假）⇒ input-plan-shape ② 红并点名 `user32!GetAsyncKeyState`',
  },
  {
    // 守卫 ③：把关系断言写成恒真（两侧抄同一个表达式）⇒ "两个独立来源"退化成一份。
    file: 'apps/emulator-lift/tests/input_plan.cpp',
    from: '    const std::uint64_t shim_calls = defs[i] != nullptr ? defs[i]->call_count : 0;\n',
    to: '    const std::uint64_t shim_calls = delivered;  // 变异：关系断言退化成恒真\n',
    guard: 'tools/test/input-plan-shape.test.mjs',
    what:
      '把 CTest `input-plan-read-points` 的 `shim_calls` 直接抄成 `delivered`（`ShimDef::call_count` 那一侧消失）' +
      '⇒ 关系断言 \(deliverCalls == call_count\) 变成恒真 ⇒ input-plan-shape ③ 红',
  },

  // ── S4 + S2′（`--input-plan` 的 CLI 面 / 门 + 待抬起；守卫是**两条 CTest**）──
  //   ★ 两条都只改一处文本，然后重建 + 跑那一条 CTest（`buildDir` = 唯一那棵 lift 树）。
  {
    // CLI 面：计划文件不存在 ⇒ **exit 2 + 点名路径**（⛔ 不许静默忽略）。
    file: 'apps/emulator-lift/src/runtime/recipe.cpp',
    from:
      '    if (f == nullptr) {\n' +
      '        if (err) *err = "打不开配方：" + path;\n' +
      '        clear();\n' +
      '        return false;\n' +
      '    }\n',
    to:
      '    if (f == nullptr) {\n' +
      '        // 变异：打不开的配方被静默吞掉（当空配方继续跑）\n' +
      '        clear();\n' +
      '        return true;\n' +
      '    }\n',
    guard: 'ctest:cli-aot-flags',
    ctest: 'cli-aot-flags',
    buildDir: 'apps/emulator-lift/out',
    buildTarget: 'lift-cli-aot-flags',
    what:
      '`InputPlan::loadFile` 打不开文件时返回 true（空计划）⇒ `--input-plan /nonexistent` 不再 exit 2、' +
      'stderr 也不点名路径 ⇒ cli-aot-flags 的 `input-plan-missing-file` 红（"以为按了、其实没按"）',
  },
  {
    // 三条指令：`wait <ms>` 的 deadline 是**唯一的墙钟面** ⇒ 去掉"未到期就停"这一步，
    // `wait` 就退化成空操作（配方里的等待被静默吞掉）。
    file: 'apps/emulator-lift/src/runtime/input_plan.cpp',
    from:
      '            if (!wait_started_) {\n' +
      '                wait_started_ = true;\n' +
      '                wait_deadline_ms_ = now + s.wait_ms;\n' +
      '            }\n' +
      '            if (now < wait_deadline_ms_) return;\n',
    to:
      '            // 变异：wait 不再等（deadline 被忽略）\n' +
      '            (void)now;\n' +
      '            (void)wait_started_;\n' +
      '            (void)wait_deadline_ms_;\n' +
      '            return;\n',
    guard: 'ctest:input-plan-read-points',
    ctest: 'input-plan-read-points',
    buildDir: 'apps/emulator-lift/out',
    buildTarget: 'lift-input-plan-read-points',
    what:
      '`wait` 的"未到期就停"被去掉（`if (now < s.wait_deadline_ms) return;` → `if (false) return;`）⇒ ' +
      '`wait` 退化成空操作 ⇒ input-plan-read-points 的 `input-plan/gates-and-releases` 红' +
      '（注入时钟 1499ms 时 `fired` 应为 0，变异后立刻变 1）',
  },
  {
    // 第四条指令：位置门是**半开**区间（FROM 含、TO 不含）⇒ 改成闭区间后，`pos == TO` 也会通过。
    //   ★ 用单测（不是端到端）当判据：`input-plan/position-gate` 里有一条 `pos == TO` **不**命中
    //     的断言（0x0460 = 轮询体的末指令末尾）⇒ 这是 `REQ-01M7HBG0NYH957M9EK3JVMF5CC` 一族里
    //     "位置门"的代表变异（⛔ 不是每个常量一条）。
    file: 'apps/emulator-lift/src/runtime/recipe.cpp',
    from: '            return position.pos >= s.pos_from && position.pos < s.pos_to;\n',
    to: '            return position.pos >= s.pos_from && position.pos <= s.pos_to;  // 变异：TO 也含\n',
    guard: 'ctest:input-plan-read-points',
    ctest: 'input-plan-read-points',
    buildDir: 'apps/emulator-lift/out',
    buildTarget: 'lift-input-plan-read-points',
    what:
      '把 `await position` 的 `pos >= s.pos_to` 改成 `pos > s.pos_to`（半开区间变成闭区间）⇒ ' +
      '`pos == TO`（0x0460）也命中 ⇒ input-plan-read-points 的 `input-plan/position-gate` 红',
  },
  {
    // ★ **完成度判据**（`REQ-01M7HP2Q1N0N004M8R4RPABWSV` 的前置）：配方没走完（`pending != 0`）
    //   或某一步 `timeout` 到期 ⇒ **exit 5**。把它改成恒 0 ⇒ "配方没走完"重新变成**假绿面**
    //   （只打一行 stderr、退出码 0）—— 那正是"图 → 配方"改写会把 CTest 变绿的那条缝。
    //   ★ 用单测当判据（不是端到端）：`input-plan/completion` 直接断言
    //     `recipeOutcomeExitCode(Running) == 5`，⛔ 不依赖 GPU / 游戏安装。
    file: 'apps/emulator-lift/src/runtime/recipe.cpp',
    from: '    return o == RecipeOutcome::Done ? 0 : kRecipeIncompleteExitCode;\n',
    to: '    return o == RecipeOutcome::Done ? 0 : 0;  // 变异：完成度不再影响退出码（"没走完"重新变成假绿面）\n',
    guard: 'ctest:input-plan-read-points',
    ctest: 'input-plan-read-points',
    buildDir: 'apps/emulator-lift/out',
    buildTarget: 'lift-input-plan-read-points',
    what:
      '把完成度的退出码映射改成恒 0（`pending != 0` / `timeout` 到期 ⇒ exit 5 变成 ⇒ 0）⇒ ' +
      'input-plan-read-points 的 `input-plan/completion` 红（`recipeOutcomeExitCode(Running)` 应为 5、' +
      '`recipeOutcomeExitCode(TimedOut)` 应为 5）',
  },
  {
    // ★ **行内 `timeout` 真的被记下来**（2026-11-16 起 `timeout` 只能写在 `await …` 那一行的末尾；
    //   旧的跨行写法已删）。把"挂到 step 上"改成丢弃 ⇒ 门永远没有守卫 ⇒
    //   ① `input-plan/parse-errors` 的 `stepAt(0)->timeout_ms == 2500` 红；
    //   ② `input-plan/step-timeout` 的"超 1 ms 判到期"永远不发生 ⇒ 红。
    file: 'apps/emulator-lift/src/runtime/recipe.cpp',
    from:
      '                s.kind = StepKind::AwaitScript;\n' +
      '                s.script = tok[2];\n' +
      '                s.timeout_ms = inline_timeout;  // 行尾 `timeout <ms>`（0 = 没写）\n',
    to:
      '                s.kind = StepKind::AwaitScript;\n' +
      '                s.script = tok[2];\n' +
      '                (void)inline_timeout;  // 变异：行尾 timeout 被忽略（门永远没有守卫）\n',
    guard: 'ctest:input-plan-read-points',
    ctest: 'input-plan-read-points',
    buildDir: 'apps/emulator-lift/out',
    buildTarget: 'lift-input-plan-read-points',
    what:
      '把行尾 `timeout` 的落点丢掉（`s.timeout_ms = inline_timeout` → `(void)inline_timeout`）⇒ ' +
      '`await script TITLE timeout 2500` 的守卫恒 0 ⇒ input-plan-read-points 的 ' +
      '`input-plan/parse-errors`（解析后 `timeout_ms` 应为 2500）与 `input-plan/step-timeout`' +
      '（超 1 ms 应判到期）红',
  },
  {
    // ★ **`wait` 没有守卫**（2026-11-16 裁决：删掉 `ms + 缺省` 那条补丁）。把这条特判删掉 ⇒
    //   `stepTimeoutMs(wait, 5000)` 又变成 5000 ⇒ 超长 `wait` 会被假判 timeout（headless 下 guest
    //   时钟跑在墙钟前面时必然踩到）。判据 = `input-plan/step-timeout` 的两条 0 断言 + 图臂长等待。
    file: 'apps/emulator-lift/src/runtime/recipe.cpp',
    from: '    if (s.kind == StepKind::Wait) return 0;\n',
    to: '    // 变异：`wait` 又拿到缺省守卫（`ms + 缺省` 那条补丁回来了）\n',
    guard: 'ctest:input-plan-read-points',
    ctest: 'input-plan-read-points',
    buildDir: 'apps/emulator-lift/out',
    buildTarget: 'lift-input-plan-read-points',
    what:
      '删掉"`wait` 恒 0"的特判 ⇒ `stepTimeoutMs(wait, 5000)` 回到 5000 ⇒ input-plan-read-points 的 ' +
      '`input-plan/step-timeout` 红（`wait` 的守卫应为 0；图臂里 guest 时钟冲过 5000 就会被假判 timeout）',
  },
  {
    // fixtures 目录白名单：**OS 元数据**必须放过（Finder 会在 `corpus/fixtures/` 里重新生成 `.DS_Store`）。
    //   ★ 判据改成恒 false ⇒ 点文件 / `Thumbs.db` / `desktop.ini` 重新撞穿白名单，
    //     macOS 上 `pnpm verify` 又 499/500（删文件只是当次止血，下一趟 Finder 就复发）。
    file: 'tools/lib/samples.mjs',
    from:
      "  if (FIXTURE_OS_METADATA_FILES.includes(base.toLowerCase())) return 'os-metadata';\n" +
      "  if (base.startsWith('.')) return 'os-metadata';\n",
    to: "  if (false) return 'os-metadata';  // 变异：忽略 OS 元数据的判据恒 false\n",
    guard: 'tools/test/fixtures.test.mjs',
    what:
      '把"忽略 OS 元数据"的判据改成恒 false ⇒ `.DS_Store` / `Thumbs.db` / `desktop.ini` / `._*` 又算未登记 ⇒ ' +
      '`fixtures.test.mjs` 的白名单单测红（macOS 上 Finder 一碰目录就重新生成 `.DS_Store` ⇒ pnpm verify 499/500 复发）',
  },

  // ── `emu-ctl` 客户端（`tools/lib/emu-ctl.mjs`；守卫 `tools/test/emu-ctl.test.mjs`）──
  //   ★ 判据 9c：端口被**别的服务**占着时，`list` 必须靠 `hello.nonce` 对不上判死。
  {
    file: 'tools/lib/emu-ctl.mjs',
    from:
      '  if (hello.nonce !== entry.nonce) return `hello.nonce 与端点文件不一致（${JSON.stringify(hello.nonce)} ≠ ${JSON.stringify(entry.nonce)}）`;',
    to: '  // 变异：不再校验 hello.nonce（陌生服务也能冒充本实例）\n',
    guard: 'tools/test/emu-ctl.test.mjs',
    what:
      '去掉 `hello.nonce` 与端点文件的同一性校验 ⇒ 端口被别的服务占着时 `list` 会把陌生服务当成 alive ' +
      '（判据 9c 的客户端面失效）⇒ emu-ctl.test 的 helloMismatch nonce 断言红',
  },
  //   ★ ⛔ 不猜：多个 alive 必须退出码 4 + 全部列出；这条变异让它"自动挑第一个"。
  {
    file: 'tools/lib/emu-ctl.mjs',
    from:
      '  const alive = all.filter((e) => e.alive === true);\n' +
      '  if (alive.length === 1) return { ok: true, entry: alive[0] };\n' +
      "  return { ok: false, code: EXIT.instance, reason: alive.length === 0 ? 'none' : 'multiple', entries: all, session: null };",
    to:
      '  const alive = all.filter((e) => e.alive === true);\n' +
      '  if (alive.length >= 1) return { ok: true, entry: alive[0] };  // 变异：自动挑第一个\n' +
      "  return { ok: false, code: EXIT.instance, reason: 'none', entries: all, session: null };",
    guard: 'tools/test/emu-ctl.test.mjs',
    what:
      '把"恰好一个 alive 才自动选中"改成"有 alive 就挑第一个" ⇒ 同 root 两个实例时 `sample` 不报退出码 4 ' +
      '（判据 11 的客户端面失效）⇒ emu-ctl.test 的 selectEntry multiple 断言红',
  },
  //   ★ 判据 9(b)：`gc` 只删 `ctl.<pid>.<nonce8>.json` —— 去掉文件名严格匹配 ⇒ `keep.txt` 也会被删。
  {
    file: 'tools/lib/emu-ctl.mjs',
    from: "  if (!CTL_NAME_RE.test(String(item?.name ?? ''))) return { action: 'keep', why: 'name_mismatch' };\n",
    to: '  // 变异：不再要求文件名严格匹配（keep.txt 也会被当成待删项）\n',
    guard: 'tools/test/emu-ctl.test.mjs',
    what:
      '去掉 `gc` 的"文件名严格匹配 ctl.<pid>.<8hex>.json"判据 ⇒ `keep.txt`（connect 失败 + pid 死了）' +
      '也会被删（判据 9b 的 keep.txt 失效）⇒ emu-ctl.test 的 gcDecision name_mismatch 断言红',
  },
  //   ★ `click` 的参考坐标必须在本地校验（服务端也会拒，但客户端先挡住 = 省一次往返）。
  {
    file: 'tools/lib/emu-ctl.mjs',
    from: '  if (!Number.isSafeInteger(n) || n < 0 || n > limit) {\n',
    to: '  if (false) {  // 变异：不校验 click 坐标上界\n',
    guard: 'tools/test/emu-ctl.test.mjs',
    what:
      '去掉 `click` 坐标的 0..1279 / 0..719 闭区间校验 ⇒ 越界坐标被放到线上（本应本地退出码 2）' +
      '⇒ emu-ctl.test 的 click 越界 argv 断言红',
  },
  // ── 清单自身的静态度量：`buildTarget` 必须等于 `add_test(NAME … COMMAND …)` 推出的 target ──
  //   ★ 破坏 = 让"应为 / 实为"的比较恒不成立 ⇒ 填错的 target 又能静默通过 `--list`，而那条变异跑起来
  //     只会记「不可判定（构建失败）」—— 与"`buildDir` 指向已删构建树"同一种失效（永远证明不了守卫会红）。
  //   判据 = `tools/test/mutate-static.test.mjs` e)（注入假清单 + 假 CMakeLists，⛔ 不真编 C++）。
  {
    file: 'tools/mutate-check.mjs',
    from: '            if (d.target !== m.buildTarget) {',
    to: '            if (false) {  // 变异：buildTarget 与 add_test 推出的 target 不再比较',
    guard: 'tools/test/mutate-static.test.mjs',
    what:
      '`buildTarget` 不再与 `add_test(NAME … COMMAND …)` 推出的 target 比较（恒不成立）⇒ 填错的 target 又能' +
      '静默通过 `--list`，而那条变异跑起来只会记「不可判定（构建失败）」⇒ mutate-static 的 buildTarget 用例红',
  },
  // ── AOT 音频接线 S0–S2（`REQ-01M7G7GPBG9BK2360WSG6DBFNP` 的派生；守卫 = `tests/aot_audio_flags.cpp`）──
  //   ★ 这一对代表"档位判定 / 参数面**静默**失效"这一类错：两者都不会崩、不会改指纹，
  //     只会让"以为有声"或"以为有仪表"变成假的。
  {
    // S1：合法值被解析成 Off ⇒ "认不出就当 off" 那类静默失效（`--audio sdl` 等于没给）。
    file: 'apps/emulator-lift/src/runtime/audio_policy.h',
    from: '    if (audioTextEquals(value, "sdl")) return {true, AudioMode::Sdl, nullptr};',
    to: '    if (audioTextEquals(value, "sdl")) return {true, AudioMode::Off, nullptr};  // 变异：认不出就当 off',
    guard: 'ctest:aot-audio-flags',
    ctest: 'aot-audio-flags',
    buildDir: 'apps/emulator-lift/out',
    buildTarget: 'lift-aot-audio-flags',
    what:
      '`--audio sdl` 被解析成 `Off`（"认不出就当 off"）⇒ 用户以为开了真设备、其实整条音频是关的，' +
      '而**没有任何症状** ⇒ `aot-audio-flags` 的解析断言红',
  },
  // ── D3DX surface 拷贝（SC0000 第 1 步；守卫 = `tests/d3dx9_load_surface.cpp`，CTest `d3dx9-load-surface`）──
  //   ★ 这三条代表同一类错：**"看起来拷了、其实错了"** —— 半开当闭区间（多拷一圈）、尺寸不等硬拷（内容错）、
  //     跨序不换 R/B（红蓝互换）。三者都不崩、不改指纹，只有逐像素断言能抓。
  //   ★ 三条都实测红过（2026-11-17，实现者报的 failures 数写在 `what` 里）。
  {
    file: 'apps/emulator-lift/src/runtime/d3d9.cpp',
    from:
      '        *r = static_cast<std::int32_t>(cpu.mem->rd32(ea + 8));\n' +
      '        *b = static_cast<std::int32_t>(cpu.mem->rd32(ea + 12));',
    to:
      '        *r = static_cast<std::int32_t>(cpu.mem->rd32(ea + 8)) + 1;  // 变异：半开当闭区间\n' +
      '        *b = static_cast<std::int32_t>(cpu.mem->rd32(ea + 12)) + 1;',
    guard: 'ctest:d3dx9-load-surface',
    ctest: 'd3dx9-load-surface',
    buildDir: 'apps/emulator-lift/out',
    buildTarget: 'lift-d3dx9-load-surface',
    what:
      'rect 的 right/bottom 各 +1（半开当闭区间）⇒ 3×2 变 4×3：`load-surface+rect-semantics` 的逐像素断言红' +
      '（实测 45 failures）',
  },
  {
    file: 'apps/emulator-lift/src/runtime/d3d9.cpp',
    from: '    if (sw != dw || sh != dh) {',
    to: '    if (cpu.mem == nullptr && (sw != dw || sh != dh)) {  // 变异：1:1 判据失效',
    guard: 'ctest:d3dx9-load-surface',
    ctest: 'd3dx9-load-surface',
    buildDir: 'apps/emulator-lift/out',
    buildTarget: 'lift-d3dx9-load-surface',
    what:
      '源/目标矩形尺寸不等不再拒绝（静默按左上角硬拷）⇒ `load-surface+one-to-one` 的"响亮停 + 目标一字节不变"红' +
      '（实测 4 failures）',
  },
  {
    file: 'apps/emulator-lift/src/runtime/d3d9.cpp',
    from:
      '        if (so != dow) {\n' +
      '            plan.byte_map[0] = 2u;\n' +
      '            plan.byte_map[2] = 0u;\n' +
      '        }',
    to:
      '        if (so != dow) {\n' +
      '            // 变异：跨序不换 R/B\n' +
      '            plan.byte_map[0] = 0u;\n' +
      '            plan.byte_map[2] = 2u;\n' +
      '        }',
    guard: 'ctest:d3dx9-load-surface',
    ctest: 'd3dx9-load-surface',
    buildDir: 'apps/emulator-lift/out',
    buildTarget: 'lift-d3dx9-load-surface',
    what:
      '32bpp 跨序（`{21,22}↔{32,33}`）不换 R/B ⇒ `load-surface+format-pairs` 的 swapRbPlan 断言红（实测 8 failures）',
  },
  // ── S3 音频泵（`REQ-01M7JZM1QQ728KX1C0G2F232T1`；守卫 = `aot-audio-pump` / `audio-device`）──
  //   ★ 这三条代表"泵**进来了**但**不该进来**"这一类静默失效：缺省档每块边界都抢 audio mutex、
  //     或者把节流去掉（每次派发都过一遍的热路径）、或者账只记了一半。
  {
    // S3：丢掉 off 判据 ⇒ 缺省档每块边界都**真的进** `pump()`（抢 audio mutex / 推进混音 / 改设备时钟），
    //   而 stdout 仍可能看起来没变 —— 本单最该被钉住的**静默**破法。
    file: 'apps/emulator-lift/src/runtime/audio_pump_gate.h',
    from: '        if (!f.enabled) return false;             // ★ off 档：一次都不进 pump（本单最硬的一条）',
    to: '        if (false) return false;  // 变异：丢掉 off 判据（缺省档会进 pump）',
    guard: 'ctest:aot-audio-pump',
    ctest: 'aot-audio-pump',
    buildDir: 'apps/emulator-lift/out',
    buildTarget: 'lift-aot-audio-pump',
    what:
      'S3 泵闸门丢掉 `!f.enabled` ⇒ `--audio off`（缺省档）每块边界都进 `pump()`' +
      ' ⇒ `aot-audio-pump` 的 `s3-pump-gate-off-never-due` 红（实测 6 failures：pumped=6212 ≠ 0）',
  },
  {
    // S3：节流被去掉 ⇒ 块边界（每次派发都过一遍的热路径）每块都去抢 audio mutex。
    file: 'apps/emulator-lift/src/runtime/audio_pump_gate.h',
    from: '        return f.insns >= next_check_;',
    to: '        return true;  // 变异：不节流（每块都泵）',
    guard: 'ctest:aot-audio-pump',
    ctest: 'aot-audio-pump',
    buildDir: 'apps/emulator-lift/out',
    buildTarget: 'lift-aot-audio-pump',
    what:
      'S3 去掉 `kCheckStride` 节流（`due()` 恒真）⇒ 10 万块边界泵 100000 次而不是 25 次' +
      ' ⇒ `aot-audio-pump` 的 `s3-pump-gate-stride-throttles` 红（实测 4 failures：100000 ≠ 25）',
  },
  {
    // S3：D 泵那一刀不记 `fill_bytes`（回到"只有 feeder 那条记"的形状）⇒ AOT 唯一可达档里恒 0。
    file: 'apps/emulator-lift/src/runtime/audio.cpp',
    from:
      '                    if (c.mem->committed(stream + kStreamBytes, 4))\n' +
      '                        self->stats_.fill_bytes += c.mem->rd32(stream + kStreamBytes) / 4u;',
    to: '                    // 变异：D 泵这一刀不记 fill_bytes',
    guard: 'ctest:audio-device',
    ctest: 'audio-device',
    buildDir: 'apps/emulator-lift/out',
    buildTarget: 'lift-audio',
    what:
      'D 泵（主线程）那一刀不记 `fill_bytes` ⇒ AOT（恒 D 泵）"喂了多少字节"恒 0，而 `fills>0` 让人以为账是全的' +
      ' ⇒ `audio-device` 的 `audio-device/d-pump-fill-bytes` 红（实测改动前 2 failures）',
  },
  // ── AOT 真 feeder 线程（`REQ-01M7MHEZRQJSMAAWPESVW44DSK`；守卫 = `aot-audio-flags` / `audio-device`）──
  //   ★ 这四条代表本单最贵的四种**静默破法**：
  //     ① "AOT 形状（Sdl + 墙钟）不再进 thread 档" ⇒ 恒 D 泵、预读 600 ms（用户实测的 0.6 s 延迟）；
  //     ② thread 档仍用 600 ms 的 lead ⇒ 修了定档却没修延迟；
  //     ③ feeder 与主线程共用 TEB ⇒ `fs:[0]` 被另一条线程改写（BUG-22：Quit 的 C++ throw 派发到
  //        已复用的 record、eip 落到 guest 堆）；
  //     ④ 宿主给的布局不可用（0）时不再响亮失败 ⇒ 退回"静默 D 泵"（旧 bug 的形状）。
  {
    // ① 谓词：`audioThreadMode` 不再认 AOT 形状。
    file: 'apps/emulator-lift/src/runtime/audio_policy.h',
    from: '    return mode == AudioMode::Sdl && real_time_clock;',
    to: '    return mode == AudioMode::Sdl && real_time_clock && false;  // 变异：AOT 形状不再进 thread 档',
    guard: 'ctest:aot-audio-flags',
    ctest: 'aot-audio-flags',
    buildDir: 'apps/emulator-lift/out',
    buildTarget: 'lift-aot-audio-flags',
    what:
      '[audio-feeder-shape] 谓词不再认 AOT 形状（`audioThreadMode(Sdl,true)` 变 false）⇒ AOT 恒主线程 D 泵、预读 600 ms' +
      ' ⇒ `aot-audio-flags` 的 `s0-thread-mode-truth-table` 红（`(Sdl,true)` 那一格 want=1 / got=0）',
  },
  {
    // ② lead：thread 档仍用泵档的 600 ms。
    file: 'apps/emulator-lift/src/runtime/audio_policy.h',
    from: '    return thread_mode ? kThreadLeadBytes : kLeadBytes;',
    to: '    return thread_mode ? kLeadBytes : kLeadBytes;  // 变异：thread 档也用 600ms',
    guard: 'ctest:aot-audio-flags',
    ctest: 'aot-audio-flags',
    buildDir: 'apps/emulator-lift/out',
    buildTarget: 'lift-aot-audio-flags',
    what:
      '[audio-feeder-lead] `leadBytesFor(true)` 返回 115200（600 ms）而不是 19200（100 ms）⇒ thread 档的预读退回 600 ms' +
      ' ⇒ `aot-audio-flags` 的 `s0-lead-bytes-by-thread-mode` 红（19200 那两格）',
  },
  {
    // ③ BUG-22 回归口：feeder 与主线程共用 TEB。
    file: 'apps/emulator-lift/src/runtime/audio.cpp',
    from: '        audio_cpu_.fs_base = feeder_teb;',
    to: '        audio_cpu_.fs_base = cpu_->fs_base;  // 变异：共用主线程 TEB（BUG-22）',
    guard: 'ctest:audio-device',
    ctest: 'audio-device',
    buildDir: 'apps/emulator-lift/out',
    buildTarget: 'lift-audio',
    what:
      '[audio-feeder-teb] feeder 的 `fs_base` 指回主线程 TEB ⇒ 共用 `fs:[0]`（SEH 链头）⇒ BUG-22 复发' +
      ' ⇒ `audio-device` 的 `audio-device/feeder-teb-is-own` 红（`feederFsBaseForTest() != cpu.fs_base`）',
  },
  {
    // ④ 布局不可用不再响亮失败（旧 bug：静默退回 D 泵）。
    file: 'apps/emulator-lift/src/runtime/audio.cpp',
    from:
      '    feeder_layout_invalid_ = layout.feeder_teb == 0 || layout.feeder_stack_top == 0 ||\n' +
      '                             layout.feeder_stack_size == 0;',
    to: '    feeder_layout_invalid_ = false;  // 变异：布局不可用不再响亮失败（静默 D 泵）',
    guard: 'ctest:aot-audio-flags',
    ctest: 'aot-audio-flags',
    buildDir: 'apps/emulator-lift/out',
    buildTarget: 'lift-aot-audio-flags',
    what:
      '[audio-feeder-attach] 宿主给的 `GuestLayout` 不可用（feeder_teb=0）时 `begin(Sdl,墙钟)` 不再返回 false' +
      ' ⇒ 变成静默 D 泵（旧 bug 的形状）⇒ `aot-audio-flags` 的 `s0-feeder-layout-is-structural` 红',
  },
  // ── AOT 音频接线（`REQ-01M7K04EY6D3GQNB5DRWP0DYJ8` 洞 6；守卫 = `aot-audio-wiring`）──
  //   ★ 代表"**接线断了**"这类**自证式断言抓不到**的静默失效：`--sound` 从 CLI→引擎的路上被摘掉，
  //     而 `kAotSoundNotice` 那句 `muted=0` 照旧打印 —— 引擎其实是静音的。
  //   ★ 变异落在 `aot_main.cpp`（**产品 exe 的 TU**），守卫靠子进程跑那个 exe
  //     ⇒ `CMakeLists.txt` 里有 `add_dependencies(lift-aot-audio-wiring emulator-lift-aot)`，
  //       否则 `--target <守卫>` 只重建守卫 exe、产品 exe 停在变异前 ⇒ **假绿**。
  {
    file: 'apps/emulator-lift/src/aot/aot_main.cpp',
    from: '    if (!host.configureAudio(audio_parse.mode, audio_dump, sound, &err)) {',
    to: '    if (!host.configureAudio(audio_parse.mode, audio_dump, /*变异：--sound 接线删掉*/ false, &err)) {',
    guard: 'ctest:aot-audio-wiring',
    ctest: 'aot-audio-wiring',
    buildDir: 'apps/emulator-lift/out',
    buildTarget: 'lift-aot-audio-wiring',
    what:
      'AOT 臂 `--sound` 不进 `AudioEngine::configure`（传死 false）⇒ 摘要里 `audio : … muted=1`，' +
      '而 `kAotSoundNotice` 照旧声明 `muted=0` ⇒ `aot-audio-wiring` 的 `sound-virtual` / `sound-dump` 红' +
      '（实测 2 failures；`virtual-no-sound` 对照组仍绿 ⇒ 红的方向唯一）',
  },
  // ── S2 虚拟音频档的交付游标（`REQ-01M7K2J7FP60YHNGTEFR5EGGA7`）──
  //   ★ 判据：虚拟档的 `played_` 必须相对**这一轮引擎的起点**（`begin()` 记的 `origin_100ns_`），
  //     ⛔ 不是 `now100ns()` 的绝对值 —— AOT 的时钟是墙钟（`setRealTime(true)`）⇒ 绝对值 = **自开机**。
  //     退回绝对值 ⇒ `played_` 落在 8e10 量级 ⇒ 恒撞 `generateTo` 的越界门 ⇒ `mixed/pushed` 恒 0、
  //     dump 只有 44 B 头，而且**没有任何告警**（本单 S2 的原症状）。
  //   ★ 变异形态注意：删掉 `elapsed` 那行后 `origin_100ns_` 就只写不读 ⇒ clang 的
  //     `-Wunused-private-field`（`-Wall` 内）会把它算成未使用 ⇒ 编不过（工具会记「不可判定(构建失败)」）。
  //     所以用 `(void)origin_100ns_;` 保住一次读 —— 这是"运行时恒真"的形态，⛔ 不是 `if (false)`。
  //   守卫：`apps/emulator-lift/tests/audio.cpp` 的 `audio-device/epoch-relative-cursor`
  //   （注入"自开机 5 天 / 14 天"两档 raw ⇒ 游标必须都是 0.25 s 的 48000 B、dump 必须一致）。
  {
    file: 'apps/emulator-lift/src/runtime/audio.cpp',
    from: '        const std::uint64_t elapsed = now > origin_100ns_ ? now - origin_100ns_ : 0ull;\n' +
          '        const std::uint64_t new_played = elapsed * kBytesPerSec / kTicksPerSec;',
    to: '        (void)origin_100ns_;  // 变异：游标退回自开机绝对时间\n' +
        '        const std::uint64_t new_played = now * kBytesPerSec / kTicksPerSec;',
    guard: 'ctest:audio-device',
    ctest: 'audio-device',
    buildDir: 'apps/emulator-lift/out',
    buildTarget: 'lift-audio',
    what:
      '虚拟档的设备游标退回 `now100ns()` 的**绝对值**（自开机）⇒ `played_` 落在 8e10 量级、' +
      '恒撞 `generateTo` 的越界门 ⇒ `audio-device/epoch-relative-cursor` 的"游标 = 48000 B /' +
      ' dump 有 PCM / 不随 epoch 变"当场红（本单 S2 的原症状）',
  },
  // ── spawn 产品 exe 的 `buildTarget` 必须有 `add_dependencies`（假绿面）─────────────
  //   ★ 代表"**变异施加了、但产品 exe 没重建**"这一类错（`REQ-01M7K2J7VXKJMK1BJ3H7YCVJFJ`）：
  //     守卫 spawn 产品 exe，`cmake --build --target <守卫>` 只重建守卫 exe ⇒ 守卫跑的是变异前的
  //     产品 exe ⇒ 变异看起来"没红"（与"`buildDir` 指向已删构建树"同一种**不可观测**失效）。
  //   判据 = `tools/test/mutate-static.test.mjs` h)（假 CMake + 假清单）与 i)（真 CMake 删掉那一行），
  //   ⛔ 不真编 C++。
  {
    file: 'tools/mutate-check.mjs',
    // ★ `from` 跨两行写：单行文本会在**本条目自己的字符串字面量**里再出现一次 ⇒ `--list` 报"出现 2 次"。
    // ★ `to` **原样保留 from 那两行**，只在前面插一句 early return ⇒ 变异施加后清单里的 `from` 仍在文件里
    //   （否则守卫红的原因是"清单过期"，不是"新检查被削弱" —— 那种红什么也证明不了）。
    from: '  const foreign = sp.targets.filter((t) => t !== buildTarget);\n' +
          '  if (foreign.length === 0) return { applicable: false }; // 自 spawn / 不 spawn ⇒ 不需要依赖',
    to: '  return { applicable: false };  // 变异：spawn 产品 exe 的条目一律当"判据不适用"\n' +
        '  const foreign = sp.targets.filter((t) => t !== buildTarget);\n' +
        '  if (foreign.length === 0) return { applicable: false }; // 自 spawn / 不 spawn ⇒ 不需要依赖',
    guard: 'tools/test/mutate-static.test.mjs',
    what:
      '不再要求 spawn 产品 exe 的条目有 `add_dependencies(<buildTarget> <产品 exe>)`（spawn 判据一律当"不适用"）⇒ ' +
      '`--target <守卫>` 只重建守卫 exe 时产品 exe 停在变异前，而 `--list` 看起来完全正常 ⇒ ' +
      'mutate-static 的 spawn 依赖用例（h) 的四条失配 / i) 的覆盖计数）红',
  },
  // ── AOT `.ll` 的 target 函数属性（守卫 = CTest `lift-aot-ir-target-attrs`）─────────────
  //   ★ 代表"**属性掉了，而构建与运行都完全正常**"这一类静默失效：LLVM 的 LTO 内联器
  //     `areInlineCompatible()` 比的是**函数属性**（callee 的 feature 必须是 caller 的子集）。
  //     生成物不挂 `#0` ⇒ 与运行库 TU 判不相容 ⇒ `lift_tick` / `lift_note_write` 一条都内联不进
  //     AOT（`.tmp/lto/report.md` §5.2/§5.3 的实测），而**没有任何东西会喊**：
  //     构建成功、产物正常、60 帧语义指纹逐位相同，只有 LTO 的收益静默消失。
  //   ★ 变异形态：属性组 `attributes #0 = {…}` **照旧发射**（`emitPrologue` 那半边不动），只把
  //     `define` 行的 ` #0` 摘掉 ⇒ 组还在、没人引用 ⇒ **IR 仍然合法可编**（实测 clang 接受未引用的
  //     属性组）⇒ 于是红的一定是 `--verify-ir-target` 的第 ② 条，⛔ 不是"构建失败"那种不可判定。
  //   ★ `buildTarget` **故意不填**（= 全量构建）：这条用例的判据读的是 `out/aot/*.ll`，而
  //     `.ll` 由 **`emulator-lift-aot`** 的 custom command 生成、`emulator-lift-gen` 只是它的
  //     `DEPENDS`。⇒ `--target emulator-lift-gen` 只重链工具、**不重新生成 `.ll`** ⇒ 守卫读到旧产物
  //     ⇒ **假绿**；而 `--target emulator-lift-aot` 虽正确，却在 `staticManifestCheck` 里过不了
  //     （那里要求 `buildTarget` == `add_test(NAME <x> COMMAND <y>)` 推出的 `<y>` = `emulator-lift-gen`）。
  //     全量构建是本仓唯一"既重建工具又重建产物"的口径，代价只是这一条多编几秒。
  {
    file: 'apps/emulator-lift/src/aot/lift_gen_emit.cpp',
    from:
      '    put("define i32 @" + genFunctionName(comp_.root) + "(ptr %st, ptr %mem, i32 %ea, ptr %cpu)" +\n' +
      '        (ir_target_.empty() ? "" : " #0") + " {\\n");',
    to:
      '    put("define i32 @" + genFunctionName(comp_.root) + "(ptr %st, ptr %mem, i32 %ea, ptr %cpu)" +\n' +
      '        (ir_target_.empty() ? "" : "") + " {\\n");  // 变异：define 行不再挂 #0',
    guard: 'ctest:lift-aot-ir-target-attrs',
    ctest: 'lift-aot-ir-target-attrs',
    buildDir: 'apps/emulator-lift/out',
    what:
      '`define i32 @lift_cc_*` 行不再挂 `#0`（属性组照旧发射但没人引用）⇒ 函数上没有 target-cpu / ' +
      'target-features ⇒ LTO 的 areInlineCompatible 与运行库 TU 判不相容 ⇒ lift_tick / lift_note_write ' +
      '静默不内联（构建成功、语义指纹相同 ⇒ 只有收益消失）⇒ lift-aot-ir-target-attrs 的第 ② 条红',
  },

  // ── 🐞SC0000 `local 9c = 6`（SBUNKI 确认框按钮宽 334，应 247；`REQ-…` 见 .tmp/sc0000/locale-diag.md）──
  //   ★ 两条根因各代表一类静默失效，**判据都是 SDM / 微软文档**（⛔ 不是"当前取值"）：
  //     ① `bt/bts/btr/btc` 的**内存形**位偏移被截成低 5 位（寄存器形语义）⇒ 客侧 `_strcspn`
  //        的 32 字节位图跳错槽 ⇒ `"japanese"` 被切成 `"japa"` ⇒ `_setlocale` 失败。
  //        解释器是语义真源，AOT 发射器必须与它逐位一致（两臂各一条变异、**各打各的守卫**）。
  //     ② `GetCPInfo` 的 `CPINFO::LeadByte` 写到 +4（文档是 +6）⇒ 0x81–0x9F 的前导字节整段丢
  //        ⇒ `_mbstrlen` 按字节数数 ⇒ 对话框宽度按 6 个字符算。
  {
    file: 'apps/emulator-lift/src/runtime/exec_integer.cpp',
    from:
      '                dst.mem.disp += static_cast<std::uint32_t>(slot * static_cast<std::int32_t>(size));',
    to: '                (void)slot;  // 变异：不修正 disp（= 旧的"截位"语义）',
    guard: 'ctest:conformance',
    ctest: 'conformance',
    buildDir: 'apps/emulator-lift/out',
    buildTarget: 'lift-conformance',
    what:
      '解释器去掉**内存形**的 `disp` 修正（位偏移又只取低 5 位）⇒ conformance 的 `bt-mem-hi-slot` / ' +
      '`bt-mem-low-bit-only` / `bts-mem-slot` / `btr-mem-slot` / `btc-mem-slot` / `bt-mem-word` 当场红 ' +
      '（每个都按 SDM 的"地址偏移"算好了期望位置，不是"某个数字"）',
  },
  {
    file: 'apps/emulator-lift/src/aot/lift_gen_emit.cpp',
    from: '        mem_ea = bin("add", "i32", eaOf(in.op0().mem), delta);',
    to:
      '        mem_ea = bin("add", "i32", eaOf(in.op0().mem), bin("mul", "i32", delta, i32c(0u)));  // 变异：delta=0（= 旧的"截位"语义）',
    guard: 'ctest:lift-ops-diff',
    ctest: 'lift-ops-diff',
    buildDir: 'apps/emulator-lift/out',
    // ★ 只重建这个 target：`out/opsdiff/lift_aot.ll` 的 custom command 以 `emulator-lift-gen`
    //   为 DEPENDS ⇒ 重编工具 + 重新生成 IR + 重链守卫，一次做完（不碰 15 MB 的 AOT 产品臂）。
    buildTarget: 'lift-ops-diff',
    what:
      'AOT 发射器把内存形的地址增量折成 0（位偏移又只取低 5 位）⇒ `lift-ops-diff` 的 ' +
      '`bts@004F7C8D` / `bt@004F7CA8` 红（写入落在 `[esp+0]` 而不是 `[esp+12]`；`bt` 的位取值错 ⇒ ' +
      '块尾 `jae` 走另一条边 ⇒ 两臂的**下一条 EA** 不同）。★ 解释器臂不会红：语义真源是对的',
  },
  {
    file: 'apps/emulator-lift/src/shims/kernel32_bootstrap.cpp',
    from: '        buf[6] = 0x81;  // LeadByte[0]',
    to: '        buf[4] = 0x81;  // 变异：LeadByte[0] 写回 +4（旧布局）',
    guard: 'ctest:locale-ext',
    ctest: 'locale-ext',
    buildDir: 'apps/emulator-lift/out',
    buildTarget: 'lift-locale-ext',
    what:
      '`CPINFO::LeadByte` 写回 +4（旧布局）⇒ `locale-ext` 的 `get-cp-info-layout` 在 ' +
      '`DefaultChar[0]==0`（+4）与 `LeadByte[0]==0x81`（+6）两条上红 —— 判据是微软文档的字段偏移',
  },

  // ── D26 未对齐 imm32 扫描（`src/aot/closure_scan.h`；守卫 = `tests/closure_imm32.cpp`）──
  //   故障形态：把"任意偏移"退化成"4 字节对齐"（`++i` → `i += 4`）。这是**唯一的**静默失效点：
  //   扫描照跑、不报错，只是 `0x004F80E0` / `0x00513B90` / `0x0041389A` 三个入口不再进候选集
  //   ⇒ 派发表缺三行 ⇒ TITLE→Quit 又停在 `00513B90` / `0041389A`。
  //   判据：`tests/closure_imm32.cpp` 的 `closure-imm32/candidates`（候选集必须含这三个 EA）
  //   与 `closure-imm32/dispatch-table`（生成物必须含三行 `LIFT_AOT_ENTRY`）当场红。
  //   ★ `from` 是**落地后实际的代码字节**（8 空格缩进；`closure_scan.h` 里只出现一次）。
  {
    file: 'apps/emulator-lift/src/aot/closure_scan.h',
    from: '        for (std::uint64_t i = 0; i + 5 <= n; ++i) {',
    to:
      '        for (std::uint64_t i = 0; i + 5 <= n; i += 4) {  // 变异：只扫 4 字节对齐',
    guard: 'ctest:lift-closure-imm32',
    ctest: 'lift-closure-imm32',
    buildDir: 'apps/emulator-lift/out',
    buildTarget: 'lift-closure-imm32',
    what:
      '把未对齐 imm32 扫描退化成 4 字节对齐（`++i` → `i += 4`）⇒ 0x004F80E0 / 0x00513B90 / ' +
      '0x0041389A 的候选消失、派发表缺这三行 ⇒ `lift-closure-imm32` 当场红',
  },

  // ── GPU 路线 P0 · effect 资源读取器（`src/shims/d3dx9_effect.cpp`；守卫 = `tests/d3dx9_effect.cpp`）──
  //   故障形态：`findEntry` 的 id 偏一格（`id` → `id + 1`）。这是**最像"能跑"的**破坏：
  //   读取照样成功、目录树照样解析，只是取到的**是邻居的字节**（200 拿到 201 的 1077 B）。
  //   ⇒ 判据必须是**内容**（大小 + 首两字节 `//` + sha256），"读到了非空字节"永远发现不了它。
  {
    file: 'apps/emulator-lift/src/shims/d3dx9_effect.cpp',
    from: '    if (!res.findEntry(kRtRcData, id, &entry, &err)) {',
    to: '    if (!res.findEntry(kRtRcData, id + 1u, &entry, &err)) {  // 变异：id 偏一格',
    guard: 'ctest:d3dx9-effect',
    ctest: 'd3dx9-effect',
    buildDir: 'apps/emulator-lift/out',
    buildTarget: 'lift-d3dx9-effect',
    what:
      'RT_RCDATA 的 findEntry id 偏一格（`id` → `id+1`）⇒ 200 拿到 201 的字节（大小 1077 ≠ 892、' +
      'sha256 不符）⇒ `d3dx9-effect` 的 resources-pinned 当场红（"读到非空字节"发现不了这种破坏）',
  },

  // ⚠ P1 的那条变异（短路 `gpuEligible` 的 `effectDrawNotExecuted`）**已随 P1 的临时判据一起删除**：
  //   那个函数在 P2 不存在了 ⇒ 条目留着只会让 `--list` 的静态检查红（"from 片段已不存在"）。
  //   翻面后的两条在下（P2 的两条代表：**常量绑定** 与 **shader 算术**）。

  // ── GPU 路线 P2 ① · effect 的**常量表**没绑上（`src/shims/d3dx9_effect.cpp`）──
  //   故障形态：`effectDrawProgram` 把 `SetFloat("Alpha")` 的值丢成 0（`= value` → `= 0.0f`）。
  //   这是最像"能跑"的破坏：程序、纹理、UV、账本全对，只有 Alpha 恒 0 ⇒ 像素只是"颜色不对"。
  //   ⇒ 判据必须是**像素**（`fade-pixels` 的 Alpha=0.5/1 两档）与**常量表读数**（`effect-draw-recorded`）。
  {
    file: 'apps/emulator-lift/src/shims/d3dx9_effect.cpp',
    from: '        out->constants[constant_count] = value;',
    to: '        out->constants[constant_count] = 0.0f;  // 变异：常量没绑上（Alpha 恒 0）',
    guard: 'ctest:d3dx9-effect',
    ctest: 'd3dx9-effect',
    buildDir: 'apps/emulator-lift/out',
    buildTarget: 'lift-d3dx9-effect',
    what:
      'effect 的常量表不绑 `SetFloat` 的值（Alpha 恒 0）⇒ `d3dx9-effect` 的 fade-pixels 在 Alpha=0.5/1 ' +
      '两档像素上红、effect-draw-recorded 在 `constants[0]==0.5` 上红；★ 只断言"draw 成功了"会假绿',
  },

  // ── GPU 路线 P2 ② · Fade 的 `1-Alpha` 写成 `Alpha`（`src/runtime/d3d9_gpu.cpp` 的 **MSL** 块）──
  //   故障形态：alpha 归一化的插值方向反了（`c0.a*(1-Alpha)` → `c0.a*Alpha`）。
  //   ★ `from` 特意带上前一行 `tex1.sample(...)`：DXBC 块里有**同样三行**算术（两份源必须逐句一致），
  //     不带上下文就会命中 DXBC 那份 ⇒ 在 mac 上（只编 MSL）**假绿**。
  {
    file: 'apps/emulator-lift/src/runtime/d3d9_gpu.cpp',
    from:
      '    float4 c1 = tex1.sample(smp1, in.uv1 / in.invw);\n' +
      '    float4 ret = float4(0.0, 0.0, 0.0, 0.0);   // 源文本：float4 ret = 0;\n' +
      '    ret.a = c0.a * (1.0 - Alpha) + c1.a * Alpha;',
    to:
      '    float4 c1 = tex1.sample(smp1, in.uv1 / in.invw);\n' +
      '    float4 ret = float4(0.0, 0.0, 0.0, 0.0);   // 源文本：float4 ret = 0;\n' +
      '    ret.a = c0.a * Alpha + c1.a * Alpha;  // 变异：1-Alpha 写成 Alpha',
    guard: 'ctest:d3dx9-effect',
    ctest: 'd3dx9-effect',
    buildDir: 'apps/emulator-lift/out',
    buildTarget: 'lift-d3dx9-effect',
    what:
      'Fade 的 MSL 片元把 `c0.a*(1-Alpha)` 写成 `c0.a*Alpha` ⇒ ret.a / ColRate 全偏 ⇒ ' +
      '`d3dx9-effect` 的 fade-pixels 在三档像素上都红（容差 1 LSB 抓得住）；★ 只比"画没画"会假绿',
  },

  // ── GPU 路线 P3 ① · SlideBlur 的 **tap 数**少一个（`src/runtime/d3d9_gpu.cpp` 的 **MSL** 块）──
  //   源文本：`for( float i = -10.0f; i <= 10.0f; i++ )` ⇒ **21 个 tap**（上下界都含）。
  //   故障形态：`i <= 10` 写成 `i < 10`（20 tap）—— 画面仍然"糊"，只是少了一侧的采样，
  //   肉眼很难发现；而 10-tap/21-tap 的混淆正是这张单最容易犯的错（任务描述说"10-tap"，
  //   ZoomBlur 才是 10，SlideBlur 是 21）。
  //   ★ `from` 带上 `float(tap)` 那一行：DXBC 块里有**同样**的循环头（两份源逐句一致），
  //     不带上下文就会命中 DXBC 那份 ⇒ 在 mac 上（只编 MSL）**假绿**。
  {
    file: 'apps/emulator-lift/src/runtime/d3d9_gpu.cpp',
    from:
      '    for (int tap = -10; tap <= 10; ++tap) {\n' +
      '        const float fi = float(tap);',
    to:
      '    for (int tap = -10; tap < 10; ++tap) {  // 变异：上下界少一档（21 tap → 20 tap）\n' +
      '        const float fi = float(tap);',
    guard: 'ctest:d3dx9-effect',
    ctest: 'd3dx9-effect',
    buildDir: 'apps/emulator-lift/out',
    buildTarget: 'lift-d3dx9-effect',
    what:
      'SlideBlur 的 MSL 片元把 `i <= 10` 写成 `i < 10`（21 tap → 20 tap）⇒ 一侧的 10 个采样消失 ⇒ ' +
      '`d3dx9-effect` 的 blur-pixels 在 Angle=0 / Angle=90 两档上红（R 45→36、G 45→36 的 9 LSB 偏差）；' +
      '★ 只断言"糊了 / 画出来了"会假绿',
  },

  // ── GPU 路线 P3 ② · SlideBlur 的 **uv 步长**放大 10 倍（同一文件的 MSL 块）──
  //   源文本：`tLen = Length / max( Width, Height ) * 0.05f`。步长既是"模糊半径"也是 UV 单位。
  //   ★ 这是最像"能跑"的破坏之一：draw 成功、账本干净，只是采样点全跑到纹理外（CLAMP 之后
  //     绝大多数 tap 落在边缘 texel 上）⇒ 画面是一块偏色，不是模糊。
  {
    file: 'apps/emulator-lift/src/runtime/d3d9_gpu.cpp',
    from:
      '    float tLen = Length / max(Width, Height) * 0.05;\n' +
      '    float4 retCol = float4(0.0, 0.0, 0.0, 0.0);   // 源文本：float4 retCol = 0.0f;\n' +
      '    for (int tap = -10; tap <= 10; ++tap) {\n' +
      '        const float fi = float(tap);',
    to:
      '    float tLen = Length / max(Width, Height) * 0.5;  // 变异：uv 步长放大 10 倍\n' +
      '    float4 retCol = float4(0.0, 0.0, 0.0, 0.0);   // 源文本：float4 retCol = 0.0f;\n' +
      '    for (int tap = -10; tap <= 10; ++tap) {\n' +
      '        const float fi = float(tap);',
    guard: 'ctest:d3dx9-effect',
    ctest: 'd3dx9-effect',
    buildDir: 'apps/emulator-lift/out',
    buildTarget: 'lift-d3dx9-effect',
    what:
      'SlideBlur 的 MSL 片元把 `* 0.05` 写成 `* 0.5`（uv 步长放大 10 倍）⇒ 21 个 tap 几乎全部 CLAMP 到边缘 ' +
      'texel ⇒ `d3dx9-effect` 的 blur-pixels 在 Angle=0/90 两档上红（参考值的像素与边缘色差 > 1 LSB）；' +
      '★ 这条与 ① 一起钉住"步长 / 方向"这条算术',
  },

  // ── GPU 路线 P3 ③ · 203 的 **program 映射**把 SlideBlur 接到 ZoomBlur 上（跨平台那条）──
  //   ★ 上面两条只改 MSL 源文本（win32 上编的是 DXBC ⇒ 那两条在 win32 上不适用，与 P2 ② 同一条口径）。
  //     这一条改的是 `d3d9_pipeline.cpp` 的程序号映射：**两个平台都会红**。
  {
    file: 'apps/emulator-lift/src/runtime/d3d9_pipeline.cpp',
    from:
      '            case d3d9::EffectProgram::SlideBlurPs20:\n' +
      '                e.effect_draw.program = GpuEffectProgram::SlideBlurPs20;',
    to:
      '            case d3d9::EffectProgram::SlideBlurPs20:\n' +
      '                e.effect_draw.program = GpuEffectProgram::ZoomBlurPs20;  // 变异：映射接错',
    guard: 'ctest:d3dx9-effect',
    ctest: 'd3dx9-effect',
    buildDir: 'apps/emulator-lift/out',
    buildTarget: 'lift-d3dx9-effect',
    what:
      '203 的 program 映射把 SlideBlur 接到 ZoomBlur 的 shader 上 ⇒ `d3dx9-effect` 的 ' +
      'effect-draw-blur-recorded 在 `program == SlideBlurPs20` 上当场红（纯档、不依赖 GPU），' +
      'blur-pixels 也会红；★ 这条对 win32（DXBC）同样成立',
  },

  // ── A 单（`REQ-01M7NAW2QT3TG9RCFMKR052C83` · 202 Snow）① · 矩阵槽截断成 4 float ──
  //   故障形态：`SetMatrix("Proj")` 的 16 个 float 只写进前 4 个（`c < 16` → `c < 4`）。
  //   ★ 这是最像"能跑"的破坏：shim 不报错、`SetMatrix` 返回 S_OK、程序照样 recorded，
  //     只有**矩阵的 12 个元素恒 0** ⇒ 顶点被压成一条线（画面全错而账上无声）。
  //   ⇒ 判据必须是**矩阵的逐元素读数**（16 个）与**顶点对照**，⛔ "SetMatrix 成功了"发现不了。
  {
    file: 'apps/emulator-lift/src/shims/d3dx9_effect.cpp',
    from:
      '    for (std::uint32_t c = 0; c < 16; ++c) {\n' +
      '        std::uint32_t bits = 0;\n' +
      '        std::memcpy(&bits, &matrix[c], sizeof(bits));\n' +
      '        cpu.mem->wr32(state + kFxMatrix + c * 4u, bits);',
    to:
      '    for (std::uint32_t c = 0; c < 4; ++c) {  // 变异：矩阵槽截断成 4 float\n' +
      '        std::uint32_t bits = 0;\n' +
      '        std::memcpy(&bits, &matrix[c], sizeof(bits));\n' +
      '        cpu.mem->wr32(state + kFxMatrix + c * 4u, bits);',
    guard: 'ctest:d3dx9-effect',
    ctest: 'd3dx9-effect',
    buildDir: 'apps/emulator-lift/out',
    buildTarget: 'lift-d3dx9-effect',
    what:
      '`SetMatrix` 只写矩阵的 4 个 float（`c < 16` → `c < 4`）⇒ `Proj` 的后 12 个元素恒 0 ⇒ ' +
      '`d3dx9-effect` 的 effect-draw-snow-recorded 在 `sc.program.vs_matrix[i] == proj16[i]`（i≥4）与 ' +
      '`effectMatrixParamAt` 上当场红，snow-vertices 的单位/缩放对照也会红；' +
      '★ 只断言"SetMatrix 返回 S_OK / 程序 recorded"会假绿',
  },

  // ── A 单 ② · `ps_1_0 SnowPS` **漏乘 `Color`**（`src/runtime/d3d9_gpu.cpp` 的 **DXBC** 块）──
  //   源文本：`return tex2D(TexSmp, UV) * Color;`。故障形态：乘法没了（只剩纹理采样）。
  //   ★ 为什么打 **DXBC** 那份：win32 的 SDL_GPU 后端只编 DXBC（`LIFT_*` 下的 `_WIN32` 分支），
  //     改 MSL 那份在这台机器上**编译都不进**（那正是 P2②/P3① 两条变异在 win32 不适用的原因）。
  //     两份源必须逐句一致（注释里互相点名）⇒ 这条与那两条合起来覆盖双源。
  //   ★ 为什么"漏乘"是最像能跑的破坏：画面上雪照样是白色的（`Color` 的 w=sin 相位只调制亮度），
  //     控制行 `Color={0,0,0,1}`（该全 0）也不会变黑 —— 只有像素读回能抓到。
  {
    file: 'apps/emulator-lift/src/runtime/d3d9_gpu.cpp',
    from:
      'float4 PSMainSnow(FxVSOut i) : SV_Target {\n' +
      '    const float4 Color = g_fx[0];   // float4 Color;（uniform；没 SetVector 过 ⇒ 全 0）\n' +
      '    return g_fx_tex0.Sample(g_fx_smp0, i.uv0 / i.invw) * Color;',
    to:
      'float4 PSMainSnow(FxVSOut i) : SV_Target {\n' +
      '    const float4 Color = g_fx[0];   // float4 Color;（uniform；没 SetVector 过 ⇒ 全 0）\n' +
      '    return g_fx_tex0.Sample(g_fx_smp0, i.uv0 / i.invw);  // 变异：漏乘 Color',
    guard: 'ctest:d3dx9-effect',
    ctest: 'd3dx9-effect',
    buildDir: 'apps/emulator-lift/out',
    buildTarget: 'lift-d3dx9-effect',
    what:
      '202 的 DXBC 片元漏乘 `Color`（`texel * Color` → `texel`）⇒ `d3dx9-effect` 的 snow-pixels 在 ' +
      '`Color={0.5,0.25,0.75,1}`（R 128≠255）与 `Color={0,0,0,1}` 控制行（该全 0）两档上红；' +
      '★ 只断言"画出来了 / 有像素"会假绿',
  },

  // ── B 单 ① · 片元里**漏除 `invw`**（`src/runtime/d3d9_gpu.cpp` 的 **DXBC** 固定功能片元）──
  //   故障形态：`uv = (uv/w) ÷ (1/w)` 写成 `uv = uv/w` —— 顶点上乘过的 `1/w` 没人除回来 ⇒ 画出来是
  //   "仿射贴图"（近处拉长 / 纹理爬行）。这正是拆掉"凡 `invw != 1` 整批拒"之后**最像能跑**的破坏：
  //   draw 成功、账本干净（`skipped=0`），只有像素读回能抓到。
  //   ★ 为什么打 **DXBC** 那份：win32 的 SDL_GPU 后端只编 DXBC（MSL 那份在这台机器上不进编译）。
  //     两份源逐句一致（`d3d9_gpu.cpp` 的注释里互相点名）。
  {
    file: 'apps/emulator-lift/src/runtime/d3d9_gpu.cpp',
    from:
      'float4 PSMainTex(VSOut i) : SV_Target { return g_tex.Sample(g_samp, i.uv / i.invw) * (i.color / i.invw); }',
    to:
      'float4 PSMainTex(VSOut i) : SV_Target { return g_tex.Sample(g_samp, i.uv) * (i.color / i.invw); }  // 变异：漏除 invw',
    guard: 'ctest:d3d9-pipeline-gpu',
    ctest: 'd3d9-pipeline-gpu',
    buildDir: 'apps/emulator-lift/out',
    buildTarget: 'lift-d3d9-pipeline',
    what:
      '固定功能片元漏除 `invw`（`Sample(samp, uv/invw)` → `Sample(samp, uv)`）⇒ `d3d9-pipeline-gpu` 的 ' +
      'perspective-interpolation 在"透视批 == uv/w 参考"上红（采样坐标被乘 Σλ·1/w，本档偏 8+ texel）；' +
      '同一份破坏也让 `d3dx9-effect` 的 snow-pixels 透视档红；★ `w==1` 的批完全不受影响 ⇒ 只跑仿射用例会假绿',
  },

  // ── B 单 ② · **顶点没带逐顶点 `1/w`**（`src/runtime/d3d9_pipeline.cpp` 的 `push_tri`）──
  //   故障形态：`g.invw = v.invw` 写成 1 ⇒ 片元里的除法退化成功恒等，与①同样的"仿射贴图"。
  //   ★ 这一条是**纯 CPU 侧**的投放点（与①分开：一个在"携不携带"，一个在"除不除"）：
  //     `d3dx9-effect` 的 snow-vertices（pure，Mock 记录顶点，`saw_nonunit_invw`）也会红。
  {
    file: 'apps/emulator-lift/src/runtime/d3d9_pipeline.cpp',
    from: '            g.invw = static_cast<float>(v.invw);',
    to: '            g.invw = 1.0f;  // 变异：整批一个 invw（丢掉逐顶点 1/w）',
    guard: 'ctest:d3d9-pipeline-gpu',
    ctest: 'd3d9-pipeline-gpu',
    buildDir: 'apps/emulator-lift/out',
    buildTarget: 'lift-d3d9-pipeline',
    what:
      'GPU 顶点不带逐顶点 `1/w`（`g.invw = v.invw` → `1.0f`）⇒ `d3d9-pipeline-gpu` 的 ' +
      'perspective-interpolation 在透视批的 uv/color 上红；`d3dx9-effect` 的 snow-vertices（pure）也在 ' +
      '`saw_nonunit_invw` 上红；★ 只断言"`invw != 1` 不再拒批"会假绿（画出来的仍是仿射）',
  },

  // ── P4 · 删掉 202 的 `kBindings` 一行 ⇒ **启动守卫当场 Trap**（硬等式）──
  //   ★ 故障形态：caps 声明 ps_2_0/vs_1_0 ⇒ 引擎会建 202 ⇒ **声明在、实现集里少一条**。
  //     P4 之后这条由 `effectCapsGuard`（`GetDeviceCaps` 路径）在**启动时**抓住（`pipelineTrap` /
  //     `caps 实现集守卫`），⛔ 不再拖到某一帧、⛔ 也不是 warn。
  //     ★ 同一份破坏还会让 `effect-draw-snow-recorded` 的表长 / `implemented` 断言与
  //     snow-vertices / snow-pixels 的每一批同时红（多重证据，⛔ 不是"只红一处"）。
  //   ★ 这条不依赖平台（纯 CPU 侧的表）。
  {
    file: 'apps/emulator-lift/src/shims/d3dx9_effect.cpp',
    from:
      '    {kEffectIdSnowPs10, "Snow", "SnowPS", "ps_1_0", d3d9::EffectProgram::SnowPs10},',
    to: '    // 变异：删掉 202 的 binding（Snow 回到"程序不在实现集"）',
    guard: 'ctest:d3dx9-effect',
    ctest: 'd3dx9-effect',
    buildDir: 'apps/emulator-lift/out',
    buildTarget: 'lift-d3dx9-effect',
    what:
      '删掉 `kBindings` 里 202（Snow/SnowPS/ps_1_0）那一行 ⇒ caps 会让引擎建的 program 少一条 ⇒ ' +
      '`d3dx9-effect` 的 caps-programs-guard 在**启动时** Trap 并点名 missing=id=202/technique=Snow/' +
      'SnowPS/ps_1_0；同一份破坏还让 effect-draw-snow-recorded 的表长（seen==5）与 `implemented` 红、' +
      'snow-vertices / snow-pixels 的每一批被整批拒；★ 这条是"声明 == 实现"被撤掉的机械证据',
  },

  // ── P4 · 把启动守卫的硬等式**降级成 warn**（`shims/d3dx9_effect.cpp`）──
  //   故障形态：`required != implemented` 时不再停机器、只把原因留在 `why` 里继续跑 —— 这正是 P4
  //   要消灭的"警告 + 继续"。判据：`caps-programs-guard` 的两条注入用例（缺 202 PS / 缺 SnowVS）
  //   必须看到 `stop_kind == Trap` 且 `cpu.running == false`；⛔ "why 非空"发现不了这条降级。
  {
    file: 'apps/emulator-lift/src/shims/d3dx9_effect.cpp',
    from:
      '        if (why) *why = detail;\n' +
      '        effectCapsGuardTrap(cpu, detail);\n' +
      '        return false;\n' +
      '    }\n' +
      '    return true;',
    to:
      '        if (why) *why = detail;  // 变异：把硬等式降级成 warn（不停机器）\n' +
      '        return true;\n' +
      '    }\n' +
      '    return true;',
    guard: 'ctest:d3dx9-effect',
    ctest: 'd3dx9-effect',
    buildDir: 'apps/emulator-lift/out',
    buildTarget: 'lift-d3dx9-effect',
    what:
      '把 caps ⇒ 实现集的硬等式降级成 warn（不停机器、只写 why 就继续跑）⇒ `d3dx9-effect` 的 ' +
      'caps-programs-guard 在"缺 202 PS / 缺 SnowVS"两条注入用例上红（守卫返回 true / cpu 仍在跑）；' +
      '★ 只断言 why 非空会假绿',
  },

  // ── emulator-lift · `D3DXMatrixDecompose` 的四元数**共轭**（符号/方向） ──────────────────
  //   语义真源 = native d3dx9_43（134 例对拍表与复现命令见 docs/03-emulator/d3dx-math.md）。
  //   变异 = 写四元数时把 z 分量取反 ⇒ 对绕 z 的旋转输入（本文件两条用例都是 Rz）等于写共轭：
  //   native 的 RotZ(+90°) 给 **+**0.7071，共轭给 -0.7071。
  //   判据：`tests/d3dx9.cpp` 的 `d3dx9/matrix-decompose-rotation`（q.z 的**符号**，⛔ 不逐位断
  //   quat）与 `d3dx9/matrix-decompose-trs`（q.z = sin15°）当场红。
  //   ★ 不选"零 scale 返 S_OK"是因为那会同时改控制流、红点会落在更多断言上，说不清是哪一条
  //     判据抓住的；这条只动一个符号，红点唯一。
  {
    file: 'apps/emulator-lift/src/shims/d3dx9.cpp',
    from: '        wrF(cpu, rot_ea + 8u, rot.z);',
    to: '        wrF(cpu, rot_ea + 8u, -rot.z);  // 变异：四元数写共轭（z 取反）\n',
    guard: 'ctest:d3dx9',
    ctest: 'd3dx9',
    buildDir: 'apps/emulator-lift/out',
    buildTarget: 'lift-d3dx9',
    what:
      '`D3DXMatrixDecompose` 写四元数时把 z 取反（= 对绕 z 的旋转写**共轭**）⇒ ' +
      '`d3dx9/matrix-decompose-rotation` 的 q.z 期望 +0.7071 得 -0.7071、' +
      '`d3dx9/matrix-decompose-trs` 的 q.z 期望 sin15° 同样翻号 ⇒ 两条当场红；' +
      '合成检查（S·R(q)·T = M）会把同一个方向错误再报一遍',
  },

  // ── 墙钟 vs uptime（BUG-30）：`GetSystemTime` 退回"把 uptime 当 1970 epoch"的修前形状 ──
  //   ★ 代表"把两条读数又合成一条"这一类错：uptime 读数本身是单调的、`GetTickCount` 仍然对，
  //     所以**只有**"墙钟要真的是 epoch"那一半的断言会红（年份 / 月日逐位不符）。
  //   ⛔ 不选 `GetLocalTime` 的同一行：它还会经过时区，红了分不清是 uptime 混进来还是偏移错了。
  {
    file: 'apps/emulator-lift/src/shims/kernel32_time.cpp',
    from:
      '    writeSystemTime(cpu, call.u32(0), civilFrom100ns(cpu.clock->wall100ns()), "GetSystemTime");',
    to:
      '    writeSystemTime(cpu, call.u32(0), civilFrom100ns(cpu.clock->now100ns()), "GetSystemTime");  // 变异：退回 uptime',
    guard: 'ctest:kernel32-time',
    ctest: 'kernel32-time',
    buildDir: 'apps/emulator-lift/out',
    buildTarget: 'lift-kernel32-time',
    what:
      'GetSystemTime 退回 `now100ns()`（宿主 uptime）⇒ 注入的墙钟 2024-03-05T12:34:56.789Z 被展开成 ' +
      '1970-01-01 + 1 h ⇒ `kernel32-time` 的 wall-vs-uptime-split 在年份 / 月日 / 时分秒上红' +
      '（uptime 那一半仍绿，正是 BUG-30 的修前形状）',
  },

  // ── M0 · UI 画布原点（`REQ-01M7PEG94GXE2V10DZYCZ0B5SR`；守卫 = `tests/ui_canvas.cpp`）──
  //   ★ 代表"把 B2 的居中偏移丢掉"这一类静默错：16:9 时偏移本来就是 0，所以**只有**
  //     非 16:9 的两条断言会红（320×240 ⇒ (0,30)、1024×768 ⇒ (0,96)）。
  //     实现里把画布锚到画面左上角的那一刻，HUD 在 4:3 画面下会贴到黑边上沿 ——
  //     窗口像素测试也能看见，但这条纯函数是**最早**能红的一层（无 SDL / 无 GPU）。
  {
    file: 'apps/emulator-lift/src/ui/canvas.h',
    from: '    const int ox = (pic_w - cw) / 2;\n    const int oy = (pic_h - ch) / 2;',
    to: '    const int ox = 0;  // 变异：丢掉居中偏移\n    const int oy = 0;',
    guard: 'ctest:ui-canvas',
    ctest: 'ui-canvas',
    buildDir: 'apps/emulator-lift/out',
    buildTarget: 'lift-ui-canvas',
    what:
      '`uiCanvasOrigin` 丢掉 `((pic−ref·s)/2)` 的居中偏移 ⇒ 非 16:9 的 320×240 / 1024×768 用例' +
      '期望 (0,30)/(0,96) 得 (0,0) ⇒ ui-canvas 当场红；★ 16:9 用例仍绿（这正是"只测 16:9 会假绿"）',
  },

  // ── AOT fault 现场（`REQ-01M7PG086DG4BBPA40VCVZTP4X` A；守卫 = `tests/fault_report.cpp`）──
  //   ★ 代表"现场只打一部分"这一类错（BUG-32 的原始症状就是 fault 只有 3 行）：把 32 字栈缩成
  //     4 字 ⇒ 第 32 个字不见了 ⇒ `fault-report` 的 `A000001F` 断言当场红。寄存器 / eip / 块 EA
  //     的断言在同一个用例里，它们各自也能单独红。
  {
    file: 'apps/emulator-lift/src/runtime/fault_state.cpp',
    from: '    dumpGuestStack(cpu, out, 32);',
    to: '    dumpGuestStack(cpu, out, 4);  // 变异：32 字栈只打 4 字',
    guard: 'ctest:fault-report',
    ctest: 'fault-report',
    buildDir: 'apps/emulator-lift/out',
    buildTarget: 'lift-fault-report',
    what:
      'AOT fault 现场的栈只打 4 字（32 → 4）⇒ 第 32 个栈字 `A000001F` 消失 ⇒ ' +
      'fault-report 的合成 AOT 现场断言红（BUG-32 的"现场信息不全"同类）',
  },

  //   ★ 代表"fault 行只在有平台地址时出现"这一条结构性判据被去掉（BUG-32 的诊断缺口修反了）：
  //     safe-mode 也调 `printFaultEa(nullptr, …)` ⇒ 多出一条"无法换算"的 `fault ea` 行 ⇒
  //     `default-fault-text-unchanged` 的 `find("fault ea") == npos` 当场红（不是链接红）。
  {
    file: 'apps/emulator-lift/src/runtime/memory.cpp',
    from: '    if (plat != nullptr) printFaultEa(plat, stderr);',
    to: '    printFaultEa(plat, stderr);  // 变异：safe-mode 也打 fault ea',
    guard: 'ctest:fault-report',
    ctest: 'fault-report',
    buildDir: 'apps/emulator-lift/out',
    buildTarget: 'lift-fault-report',
    what: '「只有平台地址才打 fault ea」的门被去掉 ⇒ safe-mode 也多一条 fault ea ⇒ default-fault-text-unchanged 红',
  },

  // ── `--input-plan-trace`（`REQ-01M7PG086DG4BBPA40VCVZTP4X` B；守卫 = `tests/input_plan.cpp`）──
  //   ★ 代表"开档退化成关档"这一类错：门写反 / 被吞 ⇒ 诊断时**没有任何读数**，而配方照旧施加
  //     （`fired` 会涨、屏幕不动）—— 正是"配方 click 不被消费"查不出来的形状。
  {
    file: 'apps/emulator-lift/src/runtime/input_plan.cpp',
    from: '    if (trace_out_ == nullptr) return;  // 缺省档：一次判空、零输出',
    to: '    return;  // 变异：trace 永不打（开档退化成关档）',
    guard: 'ctest:input-plan-read-points',
    ctest: 'input-plan-read-points',
    buildDir: 'apps/emulator-lift/out',
    buildTarget: 'lift-input-plan-read-points',
    what:
      '`--input-plan-trace` 的 step 事件一行都不打 ⇒ input-plan-read-points 的 trace 用例' +
      '（开档有行 / 关档零行）当场红',
  },

  // ── `LIFT_WATCH`（BUG-32 的"谁在 P-8 写了 16 字节"；守卫 = `tests/fault_report.cpp`）──
  //   ★ 代表"watch 关闭门写反 ⇒ 开档退化成关档"这一类错：guest store 的观察点整条断掉，
  //     配方照旧跑、`follow` / `WRITE` 一条都不打 —— 诊断时会误读成"没人写那个槽"。
  //   ★ 为什么把门放在 `lift_note_write`：它是 AOT 臂**唯一**的 guest store 观察点（lifted store
  //     不经过任何可打的宿主函数）；shim 侧那条门在 `memory.h` 的内联 helper 里（同一条判据，但
  //     本条变异**不动**它 ⇒ `fault-watch-shim` 仍绿，正是"只有 guest 观察点断了"的形状）。
  {
    file: 'apps/emulator-lift/src/runtime/lift_abi.cpp',
    from: '    if (lift::watchOn()) lift::watchGuestWrite(static_cast<lift::X86*>(cpu)->eip, ea, n);',
    to: '    if (!lift::watchOn()) lift::watchGuestWrite(static_cast<lift::X86*>(cpu)->eip, ea, n);  // 变异：启用门反转',
    guard: 'ctest:fault-report',
    ctest: 'fault-report',
    buildDir: 'apps/emulator-lift/out',
    buildTarget: 'lift-fault-report',
    what:
      '`LIFT_WATCH` 的 watch 关闭门被反转（`if (watchOn())` → `if (!watchOn())`）⇒ 开档时 ' +
      '`lift_note_write` 的 follow / WRITE 一条都不打 ⇒ fault-report 的 `fault-watch-follow` 红' +
      '（断言 `follow pending` / `arm [P-8,P+8)` / `block=lift_cc_<块B>` 全部落空）',
  },

  // ── UI 脚本沙箱（A；守卫 = `tests/ui_script_quickjs.cpp` 的 sandbox 用例）──
  //   ★ 代表"漏掉 `Function.prototype.constructor` 的清除"这一类**静默沙箱洞**：只删
  //     `globalThis.Function` 时，脚本仍可 `(function(){}).constructor('return 1')()` 重新编译源码
  //     （0.10.1 实测）—— 界面照常画、没有崩溃、没有任何症状。
  //   ★ 为什么选这一条：它是"删一家不够"的唯一一处；其它几项（eval / Function / Math.random）
  //     各自删掉都会让 sandbox 用例的不同断言红，但只有这一条会把"能编译"重新打开。
  //   ★ 判据不变的红/绿：不删时 `(function(){}).constructor === Function` ⇒ 测试里那句
  //     `('return 1')()` 成功返回 1 ⇒ 记到的值不是 TypeError ⇒ 用例退出码非 0。
  {
    file: 'apps/emulator-lift/src/ui/script_quickjs.cpp',
    from: '        if (!deleteProperty(ctx, function_proto.raw(), "constructor", err)) ok = false;',
    to: '        // 变异：不清 Function.prototype.constructor（沙箱洞：.constructor 仍是 Function）',
    guard: 'ctest:ui-script-quickjs',
    ctest: 'ui-script-quickjs',
    buildDir: 'apps/emulator-lift/out',
    buildTarget: 'lift-ui-script-quickjs',
    what:
      '沙箱漏清 `Function.prototype.constructor` ⇒ `(function(){}).constructor` 仍是 Function 构造器 ⇒ ' +
      '脚本用 `("return 1")()` 重新编译源码（ui-script-quickjs 的 fn-ctor / global-ctor 断言不是 TypeError）⇒ 红',
  },

  // ── 默认 HUD 脚本（B；守卫 = `tests/ui_hud_script.cpp` 的逐行文案断言）──
  //   ★ 代表"HUD 少一行 / 版式漂了"这一类**静默**错误：窗口照开、底色照画、一条都不报，
  //     只是玩家少看到一条读数（`frame`）。判据只能是"脚本产出的文本逐行相等"。
  //   ★ 脚本是**运行时读**的外置文件（`scripts/ui/hud.js`）⇒ 变异不需要重编产品；但 mutation
  //     仍会建该 CTest 的可执行 target（`buildTarget`），这是可接受的代价。
  //   ★ 为什么是这一行：它**总是**存在（其它行都带"拿到才写"的开关），删掉必然让行数从 5 变 4
  //     ⇒ 断言立刻落空；改颜色（BG）则会同时让窗口像素判据红（那是另一条判据线）。
  {
    file: 'apps/emulator-lift/scripts/ui/hud.js',
    from: '  lines.push("frame " + game.frame);\n',
    to: '  // 变异：删掉 frame 那一行（HUD 少一条读数，界面照旧）\n',
    guard: 'ctest:ui-hud-script',
    ctest: 'ui-hud-script',
    buildDir: 'apps/emulator-lift/out',
    buildTarget: 'lift-ui-hud-script',
    what: '默认 HUD 少掉 `frame` 那一行 ⇒ 4 行而不是 5 行（ui-hud-script 的逐行断言落空）⇒ 红',
  },

  // ── 帧列表 / 栈顶脚本名（E；守卫 = `tests/ui_frames.cpp` 的 top / 顺序断言）──
  //   ★ 代表"栈顶定义漂成别的判据"这一类**静默**错误：列表照出、名字照解析、HUD 照画，
  //     只是 `top` 再也不是 `slot == cur`（例如"最高活动槽" / 恒 false）—— 玩家看到的脚本名
  //     会在槽乱序装载时变成另一格，而没有任何报错。
  //   ★ 为什么是这一行：`top` 是**唯一**把 `cur` 与列表连起来的判据；`engine_frames.cpp` 的
  //     其它行（活动判据 / 排序 / err 分支）各有独立断言，但只有这一行是"产品口径"本身
  //     （报告 U5：`top = cur` 是用户裁决，不是引擎事实）。改成恒 false 后
  //     `ui-frames` 的 `active-slots-and-names` 立刻在 `expectList` 的 top 列落空。
  {
    file: 'apps/emulator-lift/src/runtime/engine_frames.cpp',
    from: '        info.top = (slot == cur);',
    to: '        info.top = false;  // 变异：栈顶判据失效（列表照给，top 恒 false）',
    guard: 'ctest:ui-frames',
    ctest: 'ui-frames',
    buildDir: 'apps/emulator-lift/out',
    buildTarget: 'lift-ui-frames',
    what:
      '`top = (slot == cur)` 改成 `top = false` ⇒ 没有任何一项是栈顶（ui-frames 的 top 列断言落空）⇒ 红',
  },

  // ── 宿主桥的结果对象（`REQ-01M7QF01A00F9946DX0GPH23JQ` 切片 3；守卫 = `tests/ui_script_quickjs.cpp`）──
  //   ★ 代表"结束的结果没回到脚本"这一类**静默**错误：窗口关了、剪贴板也写了，但脚本拿到
  //     `undefined` ⇒ HUD 的状态行整行不出现（或一按就把叠层打死）—— 屏幕照画，没有任何日志。
  //   ★ 为什么落在这一行：`profileEnd` 的**唯一**返回值就是这里；`makeProfileEndResult` 仍被调用
  //     （只是把结果丢掉）⇒ 没有额外的编译告警会"帮忙"让守卫红 —— 红点只能是 `host-bridge` 的
  //     返回形状断言（`e.ok` on `undefined` ⇒ TypeError ⇒ render false）。
  {
    file: 'apps/emulator-lift/src/ui/script_quickjs.cpp',
    from:
      '        Value out = makeProfileEndResult(ctx, ok, ok ? text : std::string(), error);\n' +
      '        if (out.isException()) {\n' +
      '            qjs::clearException(ctx);\n' +
      '            return JS_ThrowTypeError(ctx, "host.profileEnd: 结果对象写入失败");\n' +
      '        }\n' +
      '        return out.take();',
    to:
      '        Value out = makeProfileEndResult(ctx, ok, ok ? text : std::string(), error);\n' +
      '        if (out.isException()) {\n' +
      '            qjs::clearException(ctx);\n' +
      '            return JS_ThrowTypeError(ctx, "host.profileEnd: 结果对象写入失败");\n' +
      '        }\n' +
      '        (void)out;\n' +
      '        return JS_UNDEFINED;  // 变异：profileEnd 不返回结果对象',
    guard: 'ctest:ui-script-quickjs',
    ctest: 'ui-script-quickjs',
    buildDir: 'apps/emulator-lift/out',
    buildTarget: 'lift-ui-script-quickjs',
    what:
      'ui-host-profile-end-result：`profileEnd` 不再把结果对象交回脚本（返回 undefined）⇒ ' +
      'ui-script-quickjs 的 host-bridge 在返回形状 / `e.ok` 断言上当场红',
  },

  // ── 帧列表的 `pos_ok`（切片 3；守卫 = `tests/ui_frames.cpp`）──
  //   ★ 代表"pos 读数没真的进表"这一类**静默**错误：帧列表照给、名字照解析、`top` 照对，
  //     只是 top 项永远没有 `pos`（HUD / 未来 `game.when` 看不到执行点）—— 没有任何报错。
  //   ★ 为什么落在这一行：`readScriptPositionAt` 的结果**只有这里**写进 `UiFrameInfo`；
  //     改成恒 false 之后 `ui-frames` 的 `pos-formula` / `pos-only-top` 当场红。
  {
    file: 'apps/emulator-lift/src/runtime/engine_frames.cpp',
    from: '            info.pos_ok = p.valid;',
    to: '            info.pos_ok = false;  // 变异：帧列表不再暴露位置',
    guard: 'ctest:ui-frames',
    ctest: 'ui-frames',
    buildDir: 'apps/emulator-lift/out',
    buildTarget: 'lift-ui-frames',
    what:
      'ui-frames-pos-ok：`info.pos_ok` 恒 false（帧列表不再暴露位置）⇒ ' +
      'ui-frames 的 pos-formula / pos-only-top 当场红（top 项应有 `0x3C + (pc − base)`）',
  },

  // ── 栅格化的宿主级 OOM 上界（P1；守卫 = `tests/ui_text.cpp` 的 `ui-text/huge-rect-rejected`）──
  //   ★ 代表"--ui 脚本给一个超大 w/h 就能触发**宿主级**超大分配"这一类**用户可触发**的错误：
  //     没有上界时 `ui.rect({w:2e9,h:2e9})` 抛 std::length_error（异常穿到 presenter ⇒ latch、
  //     停止提交），`1e6×1e6`（4 TB）则直接被系统 SIGKILL。这不是"画不出来"，是**宿主进程**的风险。
  //   ★ 为什么是这条常量：`rasterize` 分配前有**两道**门 —— 单边 8192 与总字节 64 MiB。
  //     面积型（1e6×1e6）会被第二道兜住 ⇒ 只放宽单边时它仍是 false（变异假绿）。
  //     所以用例另有一条 **1e6×1**（4 MB，面积远小于 64 MiB）的 witness：单边上限一旦放宽，
  //     它就真的栅格化成功 ⇒ `!rasterized` 落空。变异跑起来也**不会**去申请 4 TB
  //     （① 仍被 64 MiB 门拦住，② 只申请 4 MB）。
  {
    file: 'apps/emulator-lift/src/ui/raster.cpp',
    from: 'constexpr int kMaxRasterDim = 8192;',
    to: 'constexpr int kMaxRasterDim = 1 << 20;  // 变异：单边上限放宽 128 倍（面积上限仍在）',
    guard: 'ctest:ui-text',
    ctest: 'ui-text',
    buildDir: 'apps/emulator-lift/out',
    buildTarget: 'lift-ui-text',
    what: '单边上限 8192 放宽到 1<<20 ⇒ 1e6×1（4 MB）的 rect 被接受并栅格化 ⇒ huge-rect-rejected 的 `!rasterized` 落空 ⇒ 红',
  },

  // ── `--scale` 的**调用点**（守卫 = `tests/present_window.cpp` 的
  //    `present-window/scale-window-follows-frame`）──
  //   ★ 代表"有公式、但呈现汇点不乘 scale"这一类**静默**失效（2026-11-19 的现场）：
  //     `--scale` 只在 `SDL_CreateWindow` 生效；第一帧 Present 时 `SDL_SetWindowSize` 把窗口
  //     设回 guest 后缓冲尺寸（1280×720）⇒ 缩放被抹掉，无报错、无 readout 变化。
  //   ★ 为什么变异落在**调用点**而不是 `windowSizeForFrame`：纯公式测试（`tests/window_scale.cpp`）
  //     测得到公式、测不到"调用点不调用它"—— 而那正是这次的失效形态。
  //   ★ 为什么只改 SDL_Renderer 那一路：`present-window`（dummy + software）走的就是它；
  //     把乘子换成 1.0 ⇒ 期望 1920×1080 实得 1280×720（第二条 960×720 ⇒ 640×480 同时红）。
  {
    file: 'apps/emulator-lift/src/runtime/present_sdl.cpp',
    from: '            const WindowSize want = windowSizeForFrame(w, h, impl_->scale_report.scale);',
    to: '            const WindowSize want = windowSizeForFrame(w, h, kDefaultScale);  // 变异：丢掉 scale 乘子',
    guard: 'ctest:present-window',
    ctest: 'present-window',
    buildDir: 'apps/emulator-lift/out',
    buildTarget: 'lift-present-window',
    what:
      '呈现汇点把窗口尺寸设回**帧尺寸**（丢掉 `--scale` 乘子）⇒ 第一帧 Present 之后窗口 1280×720 ' +
      '而不是 1920×1080（scale-window-follows-frame 的 ww/wh 与 960×720 四条断言红）',
  },

  // ── 窗口报告文本化（`REQ-01M7QF01A00F9946DX0GPH23JQ` 切片 1；守卫 = `tests/window_prof.cpp`）──
  //   ★ 代表"抽函数时漏掉一段"这一类静默失效：`endWindow()` 的文本现在只有一份实现
  //     （`formatWindowReportText`），少打一行 ⇒ 金样逐字节断言 + 段落闭合断言同时点名。
  //   ★ 为什么落在 `window_prof.cpp`：文本的唯一实现在那里（头文件里改会重编整条 AOT 臂）。
  {
    file: 'apps/emulator-lift/src/aot/window_prof.cpp',
    from: '    out += formatLine("instrument  : %10.3f ms  %5.1f%%  （%.1f ns/取时 × %llu 次）\\n",\n                      ms(t.instrument), pct(t.instrument), w.clock_read_ns,\n                      static_cast<unsigned long long>(w.sampled * 3 + w.shim_calls * 2));',
    to: '    // 变异：删掉 instrument 行（报告少一段）',
    guard: 'ctest:aot-window-report-text',
    ctest: 'aot-window-report-text',
    buildDir: 'apps/emulator-lift/out',
    buildTarget: 'lift-aot-window-report',
    what:
      '删掉 `instrument` 行 ⇒ 报告文本少一段 ⇒ `aot-window-report-text` 的金样逐字节断言与' +
      '段落闭合断言当场红（stdout 逐字不变的证据失效）',
  },

  // ── `emulator-lift-gen` 的输出字节恒为 ASCII（F；守卫 = `tests/lift_gen_ascii_output.cpp`）──
  //   ★ 代表"构建期工具吐非 ASCII ⇒ ninja 捕获回放 / 管道 / 重定向消费者按本地码页解码 = 乱码"
  //     这一类**静默**缺陷：构建成功、退出码 0，只有日志不可读。
  //   ★ 为什么改 usage 首行：它在 `--help` 的**第一条**输出里（守卫的 ① 路径必然打到），且
  //     改回中文是**原缺陷的形态**（不是人造破坏）；`--help` 不需要任何资产 ⇒ 变异跑得起。
  //   ★ `buildTarget` 指**测试 target**（`ctestCommandTarget` 的唯一权威），而它
  //     `add_dependencies(... emulator-lift-gen)` ⇒ 产品 exe 会一起重建（否则变异落在产品 TU 里、
  //     守卫跑变异前的二进制 = 假绿；静态检查 `checkSpawnDependencies` 也钉着这一行）。
  {
    file: 'apps/emulator-lift/src/aot/lift_gen.cpp',
    from: '        "emulator-lift-gen - Shape A / T1a: entry EA set + PE bytes -> LLVM IR (`.ll`)\\n"',
    to: '        "emulator-lift-gen —— Shape A / T1a：入口 EA 集合 + PE 字节 → LLVM IR（`.ll`）\\n"',
    guard: 'ctest:gen-ascii-output',
    ctest: 'gen-ascii-output',
    buildDir: 'apps/emulator-lift/out',
    buildTarget: 'lift-gen-ascii-output',
    what:
      '把 usage 首行改回中文（原缺陷形态）⇒ `--help` 的 stdout 出现非 ASCII 字节 ⇒ ' +
      'gen-ascii-output 的"每个字节 < 0x80"断言当场红',
  },

  // ── DirectShow：`JoinFilterGraph` 的**成员槽**（守卫 = `tests/dshow_graph.cpp` 的
  //    `dshow-join-filter-graph`）──
  //   ★ 代表"看错字段"这一类**静默**失效（2026-11-19 的现场）：`[filter+0x34]` 是 m_pGraph
  //     （唯一写点 `0x4D02ED mov [edi+34h],eax`），`[filter+0x6C]` 是渲染器缓存的**输入 pin**
  //     （`GetPin` 懒建，`0x4D5B3C`）—— 在 AddFilter 里检查后者必然为 0 ⇒ 每次播片报一条假 Error。
  //   ★ 为什么改回 `0x6C` 能红（且不是恒真）：join 真的把 graph 写进了 `+0x34`，产品却去读
  //     `+0x6C`（那时 pin 还没建）⇒ 判定"join 没落盘" ⇒ 那条 Error 回来 ⇒
  //     `dshow-join-filter-graph` 的 `!hasErrorLine(log, "JoinFilterGraph")` 落空。
  {
    file: 'apps/emulator-lift/src/runtime/dshow/graph.cpp',
    from: 'constexpr std::uint32_t kFilterGraphField = 0x34; // m_pGraph（EA 锚 0x4D02ED）',
    to: 'constexpr std::uint32_t kFilterGraphField = 0x6C; // 变异：看错槽（那是 GetPin 缓存的输入 pin）',
    guard: 'ctest:dshow-graph',
    ctest: 'dshow-graph',
    buildDir: 'apps/emulator-lift/out',
    buildTarget: 'lift-dshow-graph',
    what:
      '`kFilterGraphField` 0x34→0x6C（把 m_pGraph 看成渲染器的输入 pin）⇒ join 已落盘的 +0x34 读不到、' +
      'AddFilter 之后每条 JoinFilterGraph 都误报"没落盘" ⇒ dshow-join-filter-graph 红',
  },

  // ── CMake 打印面：把一条 `message()` 改回中文（守卫 = `tools/test/cmake-print-ascii.test.mjs`）──
  //   ★ 这是"936 乱码"那一类缺陷的**原缺陷形态**（2026-11-19 现场：`build.ninja` 的 `DESC =` 直出 UTF-8）。
  //   ★ ⛔ **不需要 `buildTarget` / `buildDir`**：该守卫是 pure `.mjs`（只读仓库内文本），
  //     变异走 `node --test <file>`、不编 C++、不跑 ctest ⇒ 按 CTest 条目的形状填 `buildTarget` 反而会让
  //     `staticManifestCheck` 去找一个不存在的 `add_test`（那是给 CTest 条目用的映射）。
  {
    file: 'apps/emulator-lift/CMakeLists.txt',
    from: '        message(STATUS "input inventory: present  ${_lift_abs}")',
    to: '        message(STATUS "输入盘点：present  ${_lift_abs}")',
    guard: 'tools/test/cmake-print-ascii.test.mjs',
    what:
      '把 configure 期的 `input inventory: present` 那条 `message()` 改回中文（原缺陷形态）⇒ ' +
      'CMake 会打印的引号实参里又出现 UTF-8 ⇒ cmake-print-ascii 的"每字节 < 0x80"断言当场红',
  },

  // ── `D3DPRESENT_PARAMETERS.PresentationInterval` 的白名单（G；守卫 = `tests/present_interval.cpp`）──
  //   ★ 代表"读了不消费 ⇒ 静默按 IMMEDIATE 放行"这一类**静默**背离（2026-11-19 的现场）：
  //     guest 把配置改成 `display:PresentInterval=1/2` 时真机会被 vblank 挡住，我们会跑得更快，
  //     而**没有任何症状**（日志 / 像素 / 帧数都不会变）。裁决 = 白名单 + 响亮停，不做近似。
  //   ★ 为什么变异落在**判定函数体**而不是调用点：调用点只有一处（`readPresentParams`），
  //     判定一旦恒真，那个调用点就退化成"白读一个字段" —— 这正是修前的形态。
  //   ★ `(void)interval` 不是摆设：本工程是 `/W4 /WX`，删掉形参引用会炸成"未使用形参"
  //     ⇒ 变异变成**构建失败**，在自检里记"不可判定"而不是"守卫红了"。
  {
    file: 'apps/emulator-lift/src/runtime/d3d9.cpp',
    from: '    return interval == kPresentIntervalImmediate;',
    to: '    (void)interval;\n    return true;  // 变异：白名单恒接受（修前形态：读了不消费）',
    guard: 'ctest:present-interval',
    ctest: 'present-interval',
    buildDir: 'apps/emulator-lift/out',
    buildTarget: 'lift-present-interval',
    what:
      '把白名单判定改成恒 `true` ⇒ DEFAULT(0) / ONE(1) / TWO(2) / FOUR(4) / EIGHT(8) 全被接受 ⇒ ' +
      'present-interval 的 10 条 `!presentIntervalSupported(v)` 断言当场红',
  },

  // ── typed global：int 槽写成**明文**（守卫 = `tests/control_globals.cpp`）──
  //   ★ 代表"写了但没生效"这一类**静默**缺陷：引擎读到的是它 DEC 后的垃圾位模式 —— 工具回 ok:true、
  //     脚本里那个 global 却没有任何变化，而日志 / 截图都不会喊。analysis §2.4 / §3.3(d) 第 6 条。
  //   ★ 为什么落在**写**而不是读：读错只影响诊断；写错会**改坏 guest 状态**，而且看起来成功了。
  {
    file: 'apps/emulator-lift/src/runtime/script_globals.cpp',
    from: '    mem.wr32(slot.addr, encodeInt(value.u32, key));  // ★ ENC，⛔ 不是明文',
    to: '    mem.wr32(slot.addr, value.u32);  // 变异：写明文（引擎 DEC 出垃圾 ⇒ "写了但没生效"）',
    guard: 'ctest:lift-control-globals',
    ctest: 'lift-control-globals',
    buildDir: 'apps/emulator-lift/out',
    buildTarget: 'lift-control-globals',
    what:
      'script-globals-int-enc-plaintext：把 int/uint 的写入从 `ENC(value, key)` 换成**明文** ⇒ ' +
      'lift-control-globals 的 "槽里必须是 ENC / 读回原值"断言当场红（NaN 与 float 不受影响 ⇒ 红点必须点名 int 往返）',
  },

  // ── typed global：十六进制口径退回"自动识别十进制"（守卫 = `tests/control_globals.cpp`）──
  //   ★ 父级 2026-11-19 追加裁决：index / int-uint value **只收带 0x 前缀的十六进制**，纯十进制一律拒。
  //   ★ 为什么这是判据而不是风格：`b1b6` 是 hex raw，支持十进制就会把 `45494` 与 `0xb1b6` 当成两个
  //     都可能的意思（"0xB1B6 到底是 hex 还是十进制"的歧义回来）—— 静默写错槽比报错更糟。
  {
    file: 'apps/emulator-lift/src/runtime/control_ws.cpp',
    from: "    if (text[0] != '0' || (text[1] != 'x' && text[1] != 'X')) return false;",
    to:
      '    // 变异：退回到"自动识别十进制"（父级 2026-11-19 已裁决只收 0x 前缀）\n' +
      "    if (!(text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X'))) {\n" +
      '        try {\n' +
      '            std::size_t used = 0;\n' +
      '            const unsigned long v = std::stoul(text, &used, 10);\n' +
      '            if (used != text.size() || v > 0xFFFFFFFFul) return false;\n' +
      '            *out = static_cast<std::uint32_t>(v);\n' +
      '            return true;\n' +
      '        } catch (...) {\n' +
      '            return false;\n' +
      '        }\n' +
      '    }',
    guard: 'ctest:lift-control-globals',
    ctest: 'lift-control-globals',
    buildDir: 'apps/emulator-lift/out',
    buildTarget: 'lift-control-globals',
    what:
      'control-global-hex-only：把 `parseHex32Text` 的"只认 0x 前缀"改成"0x 或十进制都吃" ⇒ ' +
      'lift-control-globals 的 `parseGlobalIndexLiteral("45494") == type_mismatch` 断言当场红（十进制 index 被静默接受）',
  },

  // ── UI 脚本 ES module 的 rejection 面（切片 2 前半；守卫 = `tests/ui_script_modules.cpp`）──
  //   ★ 代表"module 求值返回的 Promise 没人查 rejection"这一类**静默**失效：脚本明明在顶层抛了，
  //     overlay 只是"不画"（load 走到入口检查才报一句没有 render），没有任何人看见真正的异常。
  //   ★ 为什么落在这一行：drain / rejection 检查是本单**唯一**把 module 异常带回宿主 err 的地方；
  //     `JS_Eval(MODULE)` 的失败在正常路径上是 rejected Promise 而不是 JS_EXCEPTION。
  //   ★ 判据不变的红/绿：改成恒 false 后 `thrower.js` 的 `module-boom` 不会出现在 err 里
  //     （改为入口缺失的错误）⇒ `module-throw-surfaced` 的 err 子串断言落空。
  {
    file: 'apps/emulator-lift/src/ui/script_quickjs.cpp',
    from: '            if (state == JS_PROMISE_REJECTED) {',
    to: '            if (false && state == JS_PROMISE_REJECTED) {  // 变异：静默吞掉 module 的 rejection',
    guard: 'ctest:ui-script-modules',
    ctest: 'ui-script-modules',
    buildDir: 'apps/emulator-lift/out',
    buildTarget: 'lift-ui-script-modules',
    what:
      'module 求值返回的 Promise 的 rejection 检查改成恒 false ⇒ module 顶层的异常被静默吞掉 ⇒ ' +
      'ui-script-modules 的 module-throw-surfaced 在 err 里看不到 `module-boom` ⇒ 红',
  },

  // ── 注入闸门（2026-11-19 裁决；守卫 = `tests/input_inject_visible.cpp` +
  //    `tests/input_plan.cpp`）──
  //   ★ 代表"按下没被按键电平读到就抬起"这一类**静默**失效：`inputSampleCount()` 把 `GetCursorPos`
  //     也算样本 ⇒ 位置读点满足闸门、`GetAsyncKeyState` 一次都没读过按下（现场读数
  //     `reason=forced samples==press_samples`）。这正是本轮要修掉的根因。
  //   ★ 判据不变的红/绿：改回总样本数之后，喂 ≥2 个**位置**读点（每个都 `noteInputSample` +1）
  //     就会抬起 ⇒ `inject/click-two-phase` 的"位置读点再多也不许抬起"当场红
  //     （`input-plan-read-points` 的 `gates-and-releases` ⑤ 同步红）。
  {
    // ★ C1a（2026-11-19）：闸门①的实现从 `input_plan.cpp` 搬进了 `PointerCore::releaseDue`
    //   （`runtime/pointer_core.cpp`），读数改走 `PointerPoint::key_level_reads` ⇒ 本条 `file`/`from`
    //   跟着指到新落点（机制点名与 `what` 不变）。
    file: 'apps/emulator-lift/src/runtime/pointer_core.cpp',
    from: '        const bool key_seen = pt.key_level_reads >=\n' +
          '                              r.press_key_reads + kKeyLevelReadsBeforeRelease;',
    to: '        const bool key_seen = wm.inputSampleCount() > r.press_samples + 1;  // 变异：闸门退回"总样本数"',
    guard: 'ctest:input-inject-visible',
    ctest: 'input-inject-visible',
    buildDir: 'apps/emulator-lift/out',
    buildTarget: 'lift-input-inject-visible',
    what:
      '抬起闸门从"按下之后至少 N 个 **KeyLevel** 读点"退回"总样本数前进两拍"（`inputSampleCount`，' +
      '它把 `GetCursorPos` 也算样本）⇒ 位置读点就能满足抬起 ⇒ input-inject-visible 的 ' +
      '`inject/click-two-phase`（位置读点再多也不许抬起）当场红',
  },

  // ── UI 命中顺序（切片 2 后半；守卫 = `tests/ui_hit.cpp`）──
  //   ★ 代表"点到被压住的那个按钮"这一类**静默**错误：画面照画、两个按钮都在、点击也不报错，
  //     只是命中了**先画**的那一个（HUD 里后画的面板 / 按钮压在先画的上面，视觉与行为不一致）。
  //   ★ 为什么落在这一行：`hitTest` 的**唯一**语义就是"从绘制序末尾往前找"（后画的先命中）；
  //     改成正向遍历之后，`ui-hit/order-and-bounds` 的 `hitTest(7,7)` 会从 `b` 变成 `a`。
  {
    file: 'apps/emulator-lift/src/ui/hit.cpp',
    from: '    for (auto it = regions.rbegin(); it != regions.rend(); ++it) {',
    to: '    for (auto it = regions.begin(); it != regions.end(); ++it) {  // 变异：命中改正序（先画的先命中）',
    guard: 'ctest:ui-hit',
    ctest: 'ui-hit',
    buildDir: 'apps/emulator-lift/out',
    buildTarget: 'lift-ui-hit',
    what:
      'ui-hit-order-reversed：`hitTest` 从**逆序**（后画的先命中）改成**正序** ⇒ 重叠区里点到被压住的 ' +
      '按钮 ⇒ ui-hit 的 order-and-bounds 在 `hitTest(7,7)` 上红（期望 `b`，得到 `a`）',
  },

  // ── UI 捕获路由：捕获中的 up 必须无条件吞掉（守卫 = `tests/ui_click_router.cpp`）──
  //   ★ 代表"guest 收到一个没有 down 的 up"这一类**静默**错误：HUD 吞了 down、把 up 放了过去，
  //     guest 的按钮电平 / 拖拽状态就卡在半截（而且没有任何报错）。
  //   ★ 为什么落在这一行：`handlePointer` 的 Up 分支里 `swallow = true` 是唯一让"捕获中的 up
  //     不进 guest"的地方；改成 false 之后，ui-click-router 的"捕获中的 up 必须吞掉"当场红。
  {
    file: 'apps/emulator-lift/src/ui/hit.cpp',
    from:
      '    if (!up_owed_) return decision;\n' +
      '    decision.swallow = true;\n' +
      '    up_owed_ = false;',
    to:
      '    if (!up_owed_) return decision;\n' +
      '    decision.swallow = false;  // 变异：捕获中的 up 未吞（guest 收到没有 down 的 up）\n' +
      '    up_owed_ = false;',
    guard: 'ctest:ui-click-router',
    ctest: 'ui-click-router',
    buildDir: 'apps/emulator-lift/out',
    buildTarget: 'lift-ui-click-router',
    what:
      'ui-click-up-not-swallowed：捕获中的左键 up 从"无条件吞掉"改成"放行给 guest" ⇒ ' +
      'ui-click-router 的 `捕获中的 up 必须吞掉` 断言当场红（guest 的 down/up 不对称）',
  },

  // ── UI 一次性 click：消费之后必须清掉（守卫 = `tests/ui_click_router.cpp`）──
  //   ★ 代表"同一次点击被重复消费"这一类**静默**错误：按钮的回调每帧都跑一次（PROFILE 会被
  //     连续开 / 关好几轮），而屏幕上什么都不报。
  //   ★ 为什么落在这一行：`takeClick` 是"一次性"的唯一落点（宿主每帧 render 前取走一次）；
  //     不清 pending 之后，ui-click-router 的"click 只能被消费一次"立刻红。
  {
    file: 'apps/emulator-lift/src/ui/hit.cpp',
    from: '    if (out != nullptr) *out = std::move(*pending_);\n    pending_.reset();',
    to:
      '    if (out != nullptr) *out = std::move(*pending_);\n' +
      '    // 变异：消费后不清 ⇒ 下一次 render 还看得到同一个 click',
    guard: 'ctest:ui-click-router',
    ctest: 'ui-click-router',
    buildDir: 'apps/emulator-lift/out',
    buildTarget: 'lift-ui-click-router',
    what:
      'ui-click-not-cleared：`ClickRouter::takeClick` 消费后不清 pending ⇒ 同一次点击被每次 render 重复消费 ⇒ ' +
      'ui-click-router 的 `click 只能被消费一次` 当场红',
  },

  // ── kit.js 的 hover 高亮分支（守卫 = `tests/ui_kit_script.cpp`）──
  //   ★ 代表"悬停没有任何视觉反馈"这一类**静默**错误：按钮照画、点击照常，只是没有高亮 ——
  //     用户以为鼠标没对上，而且没有任何日志 / 断言会喊。
  //   ★ 为什么落在这一行：三态里只有 `hovered` 这一条分支决定 hover 配色（active / disabled 各有
  //     自己的分支）；改成恒 idle 之后，ui-kit-script 的 `state=hover` 与"三态填充色互不相同"同时红。
  //   ★ JS 是**运行时读**的发行件 ⇒ 变异不需要重编产品；`buildTarget` 仍会建该 CTest 的可执行 target。
  {
    file: 'apps/emulator-lift/scripts/ui/kit.js',
    from: "  const state = disabled ? 'disabled' : (held ? 'active' : (hovered ? 'hover' : 'idle'));",
    to:
      "  const state = disabled ? 'disabled' : (held ? 'active' : (false ? 'hover' : 'idle'));" +
      '  // 变异：hover 高亮分支失效',
    guard: 'ctest:ui-kit-script',
    ctest: 'ui-kit-script',
    buildDir: 'apps/emulator-lift/out',
    buildTarget: 'lift-ui-kit-script',
    what:
      'ui-kit-hover-branch：把 Button 的 hover 分支改成恒 idle ⇒ 悬停不再高亮 ⇒ ' +
      'ui-kit-script 的 `state=hover` 断言与"三态填充色互不相同"当场红',
  },

  // ── D3DXSprite 的混合因子必须来自设备状态（`REQ-01M7R1ABAP86QVMNBZ5S4EDKD5`；
  //    守卫 = `tests/d3dx9_sprite_blend.cpp`）──
  //   ★ 代表"引擎的加性混合被静默当成别的混合"这一类**静默**错误：画面照出，只是那颗 sprite 的
  //     （几乎全黑的）背景被不透明地盖成黑块（SAVE05 的 120×120 黑框），没有任何日志 / 断言会喊。
  //   ★ 为什么落在这一行：`spriteBlendState` 是 sprite 路**唯一**把设备状态映射到
  //     `GpuRasterState` 的地方；忽略 rs ⇒ 又回到写死的 `SrcAlpha / InvSrcAlpha`（本单的缺陷本体）。
  //   ★ 措辞纪律：这件事**与影片没有关系**（同帧 dshow 影片只是相关读数，未直读）。
  //   ★ 判据不弱化：纯 ① 的 `dst_blend == One` 与接线 ① 的 `got.dst_blend == One` 直接红；
  //     附加红点 = 表达不了的因子从"false + why"退化成"静默成功"（wiring ④ 会看到它入队）。
  {
    file: 'apps/emulator-lift/src/runtime/d3dx9_sprite.cpp',
    from:
      'bool spriteBlendState(const d3d9::BlendStateSnapshot& rs, bool alpha_blend_flag, GpuRasterState* out,\n' +
      '                      std::string* why) {\n' +
      '    return d3d9::gpuBlendStateFor(rs.src_color, rs.dst_color, rs.color_op, rs.separate_alpha, rs.src_alpha,\n' +
      '                                  rs.dst_alpha, rs.alpha_op, alpha_blend_flag, out, why);\n' +
      '}',
    to:
      'bool spriteBlendState(const d3d9::BlendStateSnapshot&, bool alpha_blend_flag, GpuRasterState* out,\n' +
      '                      std::string*) {\n' +
      '    // 变异：忽略设备状态，退回写死的 SrcAlpha / InvSrcAlpha\n' +
      '    if (out == nullptr) return false;\n' +
      '    out->blend = alpha_blend_flag;\n' +
      '    out->separate_alpha_blend = alpha_blend_flag;\n' +
      '    out->src_blend = GpuBlendFactor::SrcAlpha;\n' +
      '    out->dst_blend = GpuBlendFactor::InvSrcAlpha;\n' +
      '    out->blend_op = GpuBlendOp::Add;\n' +
      '    out->src_blend_alpha = GpuBlendFactor::SrcAlpha;\n' +
      '    out->dst_blend_alpha = GpuBlendFactor::InvSrcAlpha;\n' +
      '    out->blend_op_alpha = GpuBlendOp::Add;\n' +
      '    return true;\n' +
      '}',
    guard: 'ctest:d3dx9-sprite-blend',
    ctest: 'd3dx9-sprite-blend',
    buildDir: 'apps/emulator-lift/out',
    buildTarget: 'lift-d3dx9-sprite-blend',
    what:
      'sprite-blend-state-ignored：`spriteBlendState` 忽略设备状态、退回写死的 `SrcAlpha/InvSrcAlpha` ⇒ ' +
      'd3dx9-sprite-blend 的纯 ① `dst_blend == One` 与接线 ① `got.dst_blend == One` 当场红；' +
      '表达不了的因子也从 false+why 退化成静默成功（接线 ④ 的"不入队 + skipped 涨"红）',
  },

  // ── D3DXSprite 的 `Begin(ALPHABLEND)` 必须像真 D3DX 那样设/恢复 27/19/20
  //    （`REQ-01M7R3YPQPNPBZ327H89Q8RTEE`；守卫 = `tests/d3dx9_sprite_blend.cpp`）──
  //   ★ 代表"普通 sprite 批次读到引擎残留的 ONE/ZERO ⇒ 透明黑 texel 写进后缓冲"这一类**静默**错误：
  //     画面照出，只是走廊石地 / 对话框底板全黑（对话框黑度 1.23% → 33.70%），没有任何日志会喊。
  //   ★ 为什么落在这一行：`Begin` 里那三句 `writeRenderState` 是"D3DX 自己先设好默认因子"的
  //     唯一落点；禁掉它 ⇒ 合成路读到 Begin 之前的 2/1（引擎从没替 sprite 设过 5/6）。
  //   ★ 判据不弱化：`begin-restores-blend-state` 的 `19==5` / `20==6` 与 `pixel-transparent-texel`
  //     的"透明 texel 不改背景"直接红。
  {
    file: 'apps/emulator-lift/src/runtime/d3dx9_sprite.cpp',
    from:
      '                for (int i = 0; i < 3; ++i) {\n' +
      '                    std::string write_why;\n' +
      '                    if (!d3d9::writeRenderState(cpu, dev.state, ids[i], values[i], &write_why)) {',
    to:
      '                for (int i = 0; i < 3; ++i) {\n' +
      '                    std::string write_why;\n' +
      '                    if (false && !d3d9::writeRenderState(cpu, dev.state, ids[i], values[i], &write_why)) {' +
      '  // 变异：Begin 不再设 SrcBlend/DestBlend',
    guard: 'ctest:d3dx9-sprite-blend',
    ctest: 'd3dx9-sprite-blend',
    buildDir: 'apps/emulator-lift/out',
    buildTarget: 'lift-d3dx9-sprite-blend',
    what:
      'sprite-begin-alphablend-state-writes：`Begin(ALPHABLEND)` 不再写 27/19/20（禁掉那三句 ' +
      '`writeRenderState`）⇒ 普通批次读到引擎残留的 `ONE/ZERO` ⇒ d3dx9-sprite-blend 的' +
      '"接管与恢复"（`19==5` / `20==6`）与"像素回归"（透明 texel 把背景写成黑）当场红',
  },

  // ── 句柄表的 payload 引用计数（`releasePayload` 不再扫全表；守卫 = CTest `sync-ext`）──
  //   ★ 代表"**关闭时机**判据被去掉"这一类错：`DuplicateHandle` 让两条句柄指向**同一个** payload，
  //     合同是"关一个**不销毁**、最后一个才销毁"。旧实现靠"再扫一遍全表找同 payload 的活句柄"，
  //     新实现靠 `add()` 时 +1、`releasePayload()` 里 -1 的计数。
  //     变异 = 计数分支退化成**无条件 destroy** ⇒ 关掉原件就把共享对象 delete 了，
  //     副本句柄从此指着已释放的内存（`SetEvent` 读野对象 / 第二次 close 再 free 一次）。
  //   ★ 为什么守卫是 `sync-ext` 的 `duplicate-handle`：它就是这条合同的原件（关 h1 后 h2 仍要能
  //     `SetEvent` / `WaitForSingleObject`）⇒ 该用例的断言（`f.ok`）当场红。
  //   ★ 同一语义面另由 `mechanism` 的 `handle-table-payload-refcount` 钉住（纯表、不启动 CPU）：
  //     无条件 destroy 会让"前两次 release 的 destroy 计数必须 =0"当场红。
  {
    file: 'apps/emulator-lift/src/runtime/handles.cpp',
    from:
      '    if (count <= 1u) {\n' +
      '        if (it != table.payload_refs_.end()) table.payload_refs_.erase(it);\n' +
      '        if (entry->destroy) entry->destroy(entry->payload);\n' +
      '    } else {\n' +
      '        it->second = count - 1u;\n' +
      '    }',
    to:
      '    // 变异：无条件 destroy（不看还有没有别的活句柄指着它）\n' +
      '    (void)count;\n' +
      '    if (it != table.payload_refs_.end()) table.payload_refs_.erase(it);\n' +
      '    if (entry->destroy) entry->destroy(entry->payload);',
    guard: 'ctest:sync-ext',
    ctest: 'sync-ext',
    buildDir: 'apps/emulator-lift/out',
    buildTarget: 'lift-sync-ext',
    what:
      'releasePayload 的引用计数分支退化成**无条件 destroy** ⇒ 关掉 `DuplicateHandle` 的原件就把共享对象' +
      '销毁了（副本句柄留野指针 / 再 close 一次 double free）⇒ sync-ext 的 duplicate-handle 当场红；' +
      '同一变异也会让 mechanism 的 handle-table-payload-refcount（前两次 release 的 destroy 计数必须 0）红',
  },

  // ── C0 能力表：`hello` / `caps=` / `controlOpName` 三处同源（`plugin-runtime.md` §4.9 / §5 C0）──
  //   ★ 为什么是这两条、且各代表一类：
  //     ① **表少一项** ⇒ `hello` / `caps=` / `controlOpName` 同时少一项。`caps=` 是**唯一没有 CTest
  //        覆盖**的面（`hello` 有 `tests/control_endpoint.cpp` 的逐字断言，`caps=` 连一条都没有 ——
  //        这正是它从 9 项漂到 6 项还活了很久的原因）⇒ 只有静态守卫能证明"caps= 真的跟着表走"。
  //     ② **hello 没枚举全表**（循环从第 1 项开始）⇒ 表 / caps= 都对，只有运行期 `hello` 少一项。
  //        静态守卫**不会**红（它只断言"hello 从表生成"）⇒ 这条只能由 CTest 抓，正好点明
  //        "hello 的内容有运行期那条腿兜着、caps= 没有"这个事实上的分工。
  //   ⛔ 不给每个能力各配一条（`AGENTS.md` §5.3：每类挑一条代表）。
  {
    file: 'apps/emulator-lift/src/runtime/capability.h',
    from: '    {ControlCapability::GlobalSet, "global-set"},\n',
    to: '    // 变异：能力表少一项（hello / caps= / controlOpName 同时少 `global-set`）\n',
    guard: 'tools/test/capability-matrix.test.mjs',
    what:
      '能力表删掉 `global-set` 一行（`hello` / `caps=` / `controlOpName` 三处同时少一项）⇒ ' +
      'capability-matrix A 组的"表的名字序列 = 合同 9 项且精确有序"当场红（`caps=` 没有 CTest 覆盖，' +
      '这是它唯一的守卫）。★ 同一条变异也会撞上 `capability.h` 里那条行数 `static_assert`（编译期）—— ' +
      '所以这里的价值是**不构建也能点名**：静态守卫不需要一棵构建树就能说清"表不再覆盖合同"',
  },

  {
    file: 'apps/emulator-lift/src/runtime/control_ws.cpp',
    from:
      '    j["capabilities"] = json::array();\n' +
      '    for (const plugin::ControlCapabilityRow& row : plugin::kControlCapabilities) {\n' +
      '        j["capabilities"].push_back(row.name);\n' +
      '    }\n',
    to:
      '    j["capabilities"] = json::array();\n' +
      '    // 变异：hello 少枚举第一项（表与 caps= 仍然对）\n' +
      '    for (std::size_t i = 1; i < plugin::kControlCapabilityCount; ++i) {\n' +
      '        j["capabilities"].push_back(plugin::kControlCapabilities[i].name);\n' +
      '    }\n',
    guard: 'ctest:lift-control-endpoint',
    ctest: 'lift-control-endpoint',
    buildDir: 'apps/emulator-lift/out',
    buildTarget: 'lift-control-endpoint',
    what:
      '`hello.capabilities` 的循环从第 1 项开始（少 `pause`，`caps=` 不受影响）⇒ 运行期"9 项 + 顺序"的' +
      '逐字断言（`tests/control_endpoint.cpp` 的 endpoint-surface）当场红；★ 静态守卫不会红 —— 那正是' +
      '"hello 有运行期那条腿、caps= 没有"的分工',
  },

  // ── 插件运行时 C1b：`TriggerRunner`（`plugin-runtime.md` §4.2 / §6 行 31–33、36–37）──
  //   ★ 为什么是这两条、且各代表一类（⛔ 不给每个常量配一条，`AGENTS.md` §5.3）：
  //     ① **一读点处理多个事件**：`atPoint` 的火分支"命中即 return"是"一读点最多一个事件"的
  //        **唯一**实现点。改成 `continue` ⇒ 同一读点会再扫下去、再调一次 `onTrigger` ——
  //        而每次回调的返回值是"完整替换集合"，第二次替换会覆盖第一次（合同 §4.2 413–415 点名
  //        不可定义）。红点 = `named-set` 的"读点 1 恰好一个事件（A 先于 B）"与"读点 3 没有事件"。
  //        ★ 这条变异**必须**落在 C++ 里：它是语义级联，静态守卫看不见。
  //     ② **Once 之后再返回同对象被接受**：`exhausted()` 里那条 Once 判据一消失，回调把刚触发的
  //        Once 对象放回就被当成"重新 arm"（`fires` 仍是 1，但没人看）⇒ 合同 §4.2 417–419 点名
  //        禁止的"计数被静默重置"。红点 = `rearm` 的第一段（期望 `Failed` + 点名 + "exhausted"）。
  //        ★ 这也正是 `TriggerState` 必须共享（而不是值字段）的理由：值字段下这条判据**永远看不到**
  //        回调递增过的计数 —— 那样这条变异与它的守卫会一起"静默变绿"。
  {
    file: 'apps/emulator-lift/src/runtime/trigger_runner.cpp',
    from:
      '        // ★ **一读点最多一个事件**：命中即 fire 并 return（⛔ 不许继续扫成同点级联）。\n' +
      '        if (!invokeOnTrigger(ctx, event, now_ms, err)) return Result::Failed;\n' +
      '        return Result::Fired;\n',
    to:
      '        // 变异：一读点处理多个事件（同点级联；第二次替换会覆盖第一次的返回集合）\n' +
      '        if (!invokeOnTrigger(ctx, event, now_ms, err)) return Result::Failed;\n' +
      '        continue;\n',
    guard: 'ctest:trigger-runner',
    ctest: 'trigger-runner',
    buildDir: 'apps/emulator-lift/out',
    buildTarget: 'lift-trigger-runner',
    what:
      '把 `atPoint` 火分支的"命中即 return"改成 `continue`（一读点处理多个事件）⇒ 同一读点会调两次 ' +
      '`onTrigger`，而每次返回的是"完整替换集合"⇒ 第二次覆盖第一次 ⇒ trigger-runner 的 ' +
      '`named-set` 红（"读点 1 恰好一个事件且是 A"、"读点 3 没有事件"当场对不上）',
  },
  {
    file: 'apps/emulator-lift/src/runtime/trigger_runner.cpp',
    from: '    if (trigger.mode == TriggerMode::Once && trigger.state->fires >= 1) return true;\n',
    to: '    // 变异：Once 之后再返回同对象被接受（"已耗尽"判据消失 ⇒ 预算被静默重置）\n',
    guard: 'ctest:trigger-runner',
    ctest: 'trigger-runner',
    buildDir: 'apps/emulator-lift/out',
    buildTarget: 'lift-trigger-runner',
    what:
      'Once 之后再返回同对象被接受（删掉 `exhausted()` 里的 Once 判据 ⇒ 已 fire 过的对象再放回被当成' +
      '"重新 arm"、预算被静默重置）⇒ trigger-runner 的 `rearm` 红（第一段期望 `Failed` + ' +
      '`failName()=="O"` + lastError 含 "exhausted"，变异后返回 `Fired` 且 Run 仍然活着：' +
      '合同 §4.2 417–419 点名禁止的"计数静默重置"）',
  },

  // ── 插件运行时 C1c：`EditSink` 的**立即**写（`plugin-runtime.md` §4.4a / §4.5）──
  //   ★ 代表"Edit 被排在 Input 后面"这一类**静默**偏离：合同 §4.4 明文是 `read → edit → input`
  //     且 `ctx.edit.global.set` **同步立即**（暂停 / 控制点也可）。改成"写进队列（延迟）"之后：
  //     回执仍然 `accepted`、没有任何报错，只是**同一个回调里紧接着的读回**拿到旧值 ——
  //     插件的 `global.get → |mask → global.set → 再看一眼` 会静默地读到过期数据。
  //   ★ 红点 = `trigger-runner/global-read-modify-write-click` 的"同拍读回 0x1210 / 0x1310"与
  //     "槽里是 ENC(新值)"。★ 这条**必须**落在 C++ 里：静态词表看不见"延迟一拍"。
  {
    file: 'apps/emulator-lift/src/runtime/edit_sink.cpp',
    from: '    const bool ok = writeGlobal(mem_, id, kind, value, &err);\n',
    to:
      '    // 变异：Edit 改成"写进队列"（深度 1 且从不 drain ⇒ 本拍读不到新值；回执照旧 accepted）\n' +
      '    (void)kind;\n' +
      '    (void)id;\n' +
      '    static lift::GlobalValue deferred;\n' +
      '    deferred = value;\n' +
      '    (void)deferred;\n' +
      '    const bool ok = true;\n' +
      '    err = GlobalError::None;\n',
    guard: 'ctest:trigger-runner',
    ctest: 'trigger-runner',
    buildDir: 'apps/emulator-lift/out',
    buildTarget: 'lift-trigger-runner',
    what:
      '`GuestEditSink::globalSet` 改成"写进队列"（延迟；回执仍然 `accepted`）⇒ trigger-runner 的 ' +
      '`global-read-modify-write-click` 红（"同一个回调里 Edit 之后立刻 globalGet 得 0x1210 / ' +
      '0x1310"与"槽里的原始 32 位 = ENC(新值)"当场对不上：拿到的是旧值 0x1200 / 0x1210）',
  },

  // ── 插件运行时 C1a/b/c 的**形状**守卫：`tools/test/core-shape.test.mjs`（`plugin-runtime.md` §6 行 1 / 5）──
  //   ★ 为什么这条是**纯静态**（无 `buildDir` / `ctest` / `buildTarget`）：被判据本身就是"核心源码文本里
  //     有没有长出配方 VM 的形状"——它读的是**源码文本**，不是任何 exe 的行为。⇒ 施加一处源码变异、
  //     跑一次 `node --test`（~1 s）就够；⛔ 不要为了"跟别的条目一样"给它挂一个 CTest（那会白建两遍）。
  //   ★ 为什么落在 `begin()` 的第一行：`core-shape` 扫的是白名单文件的**全部代码行**，加在哪个函数里都
  //     一样会被扫到；选 `begin()` 开头是因为它后面那行 `handler_ = &handler;` 是当前源码里**唯一**的
  //     子串（`from` 必须能唯一定位，否则变异会施加到别处 / 施加不上）。
  //   ★ 为什么变异是一行**合法 C++**：这条自检证明的是"代码里多出一个 `next_` 就会被点名"，
  //     ⛔ 不是"文件被写坏就会报错"（那不是守卫，是编译器）。
  {
    file: 'apps/emulator-lift/src/runtime/trigger_runner.cpp',
    from: '    handler_ = &handler;\n',
    to:
      '    const std::size_t next_ = 0;  // 变异：核心文件里又长出 plan 的 `next_` 状态\n' +
      '    (void)next_;\n' +
      '    handler_ = &handler;\n',
    guard: 'tools/test/core-shape.test.mjs',
    what:
      '核心文件里又长出 plan 的 `next_` 状态（往 `trigger_runner.cpp` 的 `begin()` 开头加一行合法的 ' +
      '`const std::size_t next_ = 0; (void)next_;`）⇒ core-shape 的禁用词表当场点名 `next_` ⇒ 红',
  },

  // ── C2a：首点的 `sampleNow`（`c2-decisions.md` D-C2-10）───────────────────────────────────
  //   ★ 代表"第一个读点只 `begin` 不采样"这一类错：`IntentExecutor::atPoint` 的早退在 **② 之后**
  //     （没有 armed trigger 时**根本没采样**）⇒ 少了这一行，`start` 里的 `ctx.now` 是空对象，
  //     而 Input 的 staged 语义**完全不变** ⇒ 只有"start 读 now"的断言能看出来（静默）。
  //   ★ 反证过：把这一行删掉，`plugin-runtime` 的 `plugin/start-now-and-staged-input` 红
  //     （JS 侧 `ctx.now.pos` 期望 4660，拿到 `undefined` ⇒ `ctx.fail`）；`plugin/start-now-requires-sample`
  //     那条反向断言也会失去意义（它靠"期望值不对"来证明 now 真的被读到）。
  {
    file: 'apps/emulator-lift/src/plugin/plugin_host.cpp',
    from: '    executor_.sampleNow(point);\n',
    to: '    // 变异：首点不再补采样 ⇒ `start` 里 `ctx.now` 没有读数（Input 的 staged 语义不变）\n',
    guard: 'ctest:plugin-runtime',
    ctest: 'plugin-runtime',
    buildDir: 'apps/emulator-lift/out',
    buildTarget: 'lift-plugin-runtime',
    what:
      '第一个读点不再 `sampleNow` ⇒ `start` 里的 `ctx.now` 是空的（`plugin-runtime` 的 ' +
      '`plugin/start-now-and-staged-input` 红：JS 侧期望 `ctx.now.script/pos` 有值、拿到 undefined）',
  },

  // ── C2b：`ctx.draw` 的不支持 style 字段（`c2-decisions.md` D-C2-3）─────────────────────────
  //   ★ 代表"声明了却静默忽略"这一类错：`stroke`/`width`/`alpha`/`align` 在 `Element` 里没有
  //     表示（`ui/element.h` 只有 fill/color/bold/z）⇒ 忽略掉的话插件**照样拿到 true**、
  //     只是画出来的东西少一圈描边 / 少一层透明度，**没有任何症状**（除非有人逐字段比对）。
  //   ★ 变异把拒绝分支变成永不进入（`blocked[i] == nullptr` 在循环里恒假）⇒ `render` 返回 true
  //     ⇒ `plugin-runtime` 的 `plugin/ui-draw-bad-style` 的"必须抛"一侧红。
  {
    file: 'apps/emulator-lift/src/ui/script_quickjs.cpp',
    from:
      '        if (!v.isUndefined()) {\n' +
      '            JS_ThrowTypeError(ctx, "ctx.draw: style.%s is not supported", blocked[i]);\n',
    to:
      '        if (blocked[i] == nullptr && !v.isUndefined()) {  // 变异：不支持的 style 字段被静默忽略\n' +
      '            JS_ThrowTypeError(ctx, "ctx.draw: style.%s is not supported", blocked[i]);\n',
    guard: 'ctest:plugin-runtime',
    ctest: 'plugin-runtime',
    buildDir: 'apps/emulator-lift/out',
    buildTarget: 'lift-plugin-runtime',
    what:
      '`ctx.draw` 的不支持 style 字段不再抛 TypeError（静默忽略 `stroke`/`width`/`alpha`/`align`）⇒ ' +
      '`plugin-runtime` 的 `plugin/ui-draw-bad-style` 红（`render` 返回 true，D-C2-3 的反面）',
  },

  // ── C2b-2：插件脚本接进窗口叠层 host（`present_sdl::adoptOverlayScript`）─────────────────
  //   ★ 代表"接进来了但每帧仍画旧脚本 / 什么都不画"这一类错：`drawOverlay` 的脚本选择一旦退回
  //     "只认 owned `ui_script`"，`adoptOverlayScript` 已经把 owned 丢掉 ⇒ 本帧没有脚本 ⇒
  //     `drawOverlay` 失败、叠层被 `disableOverlay` 关掉。症状**静默**：exit 仍 0、
  //     readout 仍 `render=yes` / `run=done`，只有窗口像素能看出来（`present_sdl.cpp` 的
  //     `activeUiScript()` 是 adopted 优先的**唯一**落点）。
  //   ★ 反证过（2026-11-20 / mac arm64）：施加这一处 ⇒ `plugin-overlay` 的 `render-up` /
  //     `render-down` 红（像素回到 guest 色、`overlayEnabled()` 仍是 true 之前就被关）。
  {
    file: 'apps/emulator-lift/src/runtime/present_sdl.cpp',
    from: '        return adopted_script != nullptr ? adopted_script : ui_script.get();\n',
    to: '        return ui_script.get();  // 变异：adopt 进来的插件脚本永远不被画\n',
    guard: 'ctest:plugin-overlay',
    ctest: 'plugin-overlay',
    buildDir: 'apps/emulator-lift/out',
    buildTarget: 'lift-plugin-overlay',
    what:
      '`drawOverlay` 只认 owned `ui_script`（adopt 的插件脚本被丢掉）⇒ `plugin-overlay` 的 ' +
      '`render-up` / `render-down` 红（叠层不画、像素回到 guest 色；exit 仍 0）',
  },

  // ── A′：`%st` 别名 `cpu_`（`p2-design.md` §1 / §5-4、6）──────────────────────────────────
  //   ★ 代表"别名判据写反 / 被删掉"这一类错：`regsAliasCpu` 是 `liftShimCommon` 与 `lift_one`
  //     共用的**唯一**判据。无条件 true ⇒ AOT 臂的跳过是正确的，但非别名调用方（生成物夹具 /
  //     音频 feeder 用真 `LiftRegs`）也在**过期寄存器**上执行、结果丢掉 ⇒ 静默错值。
  //   ★ 承重守卫 = `lift-ir-shim`：它用真 `LiftRegs`（`tests/lift_ir_shim.cpp`）并断言探针写的
  //     EDX（`kProbeEdx`）被**整块回读**带回 —— 正是"整块回读必须真的发生"的判据。
  //   ★ 反证过（2026-11-20 / mac arm64）：施加这一处 ⇒ `lift-ir-shim` 红（CTest 退出码非 0）。
  //   （⛔ 不用"把 eip/eflags 换回去"当变异：那是编译错，证明力弱。）
  {
    file: 'apps/emulator-lift/src/runtime/lift_abi.cpp',
    from: 'bool regsAliasCpu(const void* st, X86& cpu) { return st == liftRegsView(cpu); }\n',
    to:
      'bool regsAliasCpu(const void* /*st*/, X86& /*cpu*/) { return true; }  ' +
      '// 变异：别名判据删除 ⇒ 无条件跳过两处搬运\n',
    guard: 'ctest:lift-ir-shim',
    ctest: 'lift-ir-shim',
    buildDir: 'apps/emulator-lift/out',
    buildTarget: 'lift-ir-shim',
    what:
      '删掉别名判据（无条件跳过两处搬运）⇒ 非别名调用方（夹具 / feeder 的真 `LiftRegs`）在过期' +
      '寄存器上执行（`lift-ir-shim` 的探针 EDX 回读断言红）',
  },

  // ── C2c：`loadSave` 的槽位行坐标（`scripts/plugins/lib/save.js` 的 `click(300, rowY)`）────
  //   ★ 代表"链的坐标写错 / 用了公式而不是实测表"这一类错：`rowY` 多 60 之后，**光标 y** 与
  //     实测行表对不上，但整条链**照样走完**（终态仍是 FIELD、readout 仍 `run=done`）⇒ 纯文档 /
  //     只看结局的断言抓不到。红点 = `plugin-runtime` 的 `control-script/start-on-trigger-chain`
  //     第 ⑤ 拍（SBUNKI 命中后 `click(300,rowY)` 落地，光标 y 不是 `rowY`）。
  //   ★ 这条变异落在 **JS 库**里（不是 C++）：`plugin-runtime` 在运行期按 `LIFT_PLUGIN_LIB_DIR`
  //     直接读该文件 ⇒ 不需要重编；`buildTarget` 只是为了让自检走同一条"重建守卫 exe + 跑 ctest"
  //     的路径（重建是空操作）。
  {
    file: 'apps/emulator-lift/scripts/plugins/lib/save.js',
    from: '                    ctx.input.click(SLOT_X, rowY);\n',
    to: '                    ctx.input.click(SLOT_X, rowY + 60);  // 变异：点错槽位行\n',
    guard: 'ctest:plugin-runtime',
    ctest: 'plugin-runtime',
    buildDir: 'apps/emulator-lift/out',
    buildTarget: 'lift-plugin-runtime',
    what:
      '`loadSave` 的槽位点击从 `(300, rowY)` 变成 `(300, rowY+60)` ⇒ 光标 y 与实测行表不符' +
      '（`plugin-runtime` 的 `control-script/start-on-trigger-chain` 红；整条链仍能走完 ⇒ 只看' +
      '`run=done` 的断言抓不到）',
  },

  // ── `[present-readback]` Phase A：needs 谓词漏 `--dump-frame`（`d3d9.cpp` 的 want_guest）─────
  //   ★ 代表"回读的消费者少接一个"这一类错：`want_guest` 漏掉 `!dump_path.empty()` ⇒ `--dump-frame`
  //     那一帧**不下载**（materialized=false）⇒ dump 写成旧帧 / 被现实现拒写。旧判据
  //     （`tests/aot_overlay.cpp` 的两次 dump 对照）会**两边都旧、照绿**，所以承重守卫是新用例
  //     `present-readback/gate-dump-fresh`（BMP 像素必须是本帧 sentinel）。
  //   ★ 反证过（本机 / mac arm64）：`node tools/mutate-check.mjs --match present-readback` ⇒ 红。
  {
    file: 'apps/emulator-lift/src/runtime/d3d9.cpp',
    from:
      '    const bool want_guest = eager || want_hash || !dump_path.empty() ||\n' +
      '                            (sink != nullptr && sink->needsGuestPixels()) ||\n' +
      '                            m->presenter().wantsGuestPixels();',
    to:
      '    // 变异：needs 谓词漏掉 --dump-frame（那一帧不回读 ⇒ dump 写旧帧 / 被拒写）\n' +
      '    const bool want_guest = eager || want_hash ||\n' +
      '                            (sink != nullptr && sink->needsGuestPixels()) ||\n' +
      '                            m->presenter().wantsGuestPixels();',
    guard: 'ctest:present-readback',
    ctest: 'present-readback',
    buildDir: 'apps/emulator-lift/out',
    buildTarget: 'lift-present-readback',
    what:
      'needs 谓词漏 `!dump_path.empty()` ⇒ `--dump-frame` 那一帧不下载（dump 写旧帧 / 被拒写）⇒ ' +
      '`present-readback/gate-dump-fresh` 的 BMP sentinel 断言红',
  },

  // ── SurfaceArena 容量 / AGERC fallback base（2026-11-20 · SAVE07 战斗 `resource-exhausted`）───────
  //   ★ 代表"抬 arena 容量却没搬 AGERC fallback base"这一类错：arena 的上界一抬就吃掉旧的
  //     `0x30000000` ⇒ `loadAgercModule` 的 `fits()` 拒 ⇒ **启动即响亮报错**（看起来像"扩容把启动搞坏了"）。
  //   ★ 守卫 `apps/emulator-lift/tests/region_layout.cpp`（CTest `region-layout`；pure / 顶层无条件注册）
  //     只钉**关系**：所有命名区间两两不相交 + AGERC fallback base 不在任何区间里且 ≥ arena 上界。
  //   ★ 变异形态选"把 fallback base 改回 0x30000000"，⛔ 不是"把 arena 缩回 128 MiB"：arena 缩小时
  //     fallback base 仍在 arena 外 ⇒ 那不是布局错误、只是容量不够（容量是读数、不作断言，见 AGENTS.md §10）。
  {
    file: 'apps/emulator-lift/src/runtime/image.h',
    from: '    std::uint32_t agerc_fallback_base = 0x40000000u;',
    to: '    std::uint32_t agerc_fallback_base = 0x30000000u;  // 变异：落回 arena 里（[0x28000000,0x38000000)）',
    guard: 'ctest:region-layout',
    ctest: 'region-layout',
    buildDir: 'apps/emulator-lift/out',
    buildTarget: 'lift-region-layout',
    what:
      'AGERC 的 fallback base 改回 0x30000000 ⇒ 落进 arena [0x28000000,0x38000000) ⇒ ' +
      '`region-layout/agerc-fallback-outside` 红（"落进了 arena" + "base < arena 上界"两条），' +
      '`region-layout/shape` 与 `-pairwise-disjoint` 不受影响（arena 与其余区间仍不相交）',
  },

  // ── `[present-readback]` Phase B：`SdlWindow` 漏了 color clear 的转发（`present_sdl.cpp`）──────
  //   ★ 为什么这一条是**这一类错**的代表：`SdlWindow` 是 `GpuRaster` 的转发壳，每加一个虚函数它就要
  //     再转发一次。漏了不会编译红、不会报错、不改像素、**所有既有测试全绿** —— `d3d9::clear` 以为
  //     "清色已交给 GPU"，而后端基类实现是 no-op ⇒ 目标与 guest 一起丢掉这次清色
  //     （`REQ-01M73P3GXW22Y467YGYPQSM571` 的 Live2D 整层丢失就是这个形状）。
  //   ★ 守卫选 `gpu-batch/color-clear-forwarded`（pure / 顶层注册）：它经 `setGpuRasterForTest` 注入
  //     替身，**必须**走壳才看得见"漏转发"。直连后端（`m.setGpuRaster(&mock)`）是抓不到的。
  //   ★ 不选 `:1118` 提前返回那一条：那只证明真后端/`gpu-raster` 会红，而它需要 GPU 设备
  //     （无 GPU 的机器上 SKIP 77 ⇒ 变异自检会变成"不可判定"）。
  {
    file: 'apps/emulator-lift/src/runtime/present_sdl.cpp',
    from:
      'void SdlWindow::rasterRequestColorClear(std::uint32_t bgra8_pixel) {\n' +
      '    GpuRaster* r = impl_->raster();\n' +
      '    if (r != nullptr) r->rasterRequestColorClear(bgra8_pixel);\n' +
      '}',
    to:
      'void SdlWindow::rasterRequestColorClear(std::uint32_t bgra8_pixel) {\n' +
      '    (void)bgra8_pixel;  // 变异：漏转发（后端收不到 arm ⇒ 清色静默丢失，账上一切正常）\n' +
      '}',
    guard: 'ctest:gpu-batch-order',
    ctest: 'gpu-batch-order',
    buildDir: 'apps/emulator-lift/out',
    buildTarget: 'lift-gpu-batch',
    what:
      '删掉 `present_sdl.cpp` 里 `rasterRequestColorClear` 的转发 ⇒ 后端（替身）收不到 arm ⇒ ' +
      '`gpu-batch/color-clear-forwarded` 红（`color_clear_count` 期望 1 实得 0）',
  },
];

let mutationRunSeq = 0;

/**
 * 跑**一条**变异：预检基线 → 施加（journal）→ 跑守卫（CTest 带单测超时）→ **无论成败都还原 +
 * 全文比对** → 判四态。
 *
 * ★ 为什么抽成导出函数：`tools/test/mutate-timeout.test.mjs` 要用**假守卫命令**验
 *   "超时 ⇒ 不可判定（超时）+ 继续下一条 + 还原仍执行"，而真 CTest 要构建 C++（慢且本机可能没有构建目录）。
 *   注入点全在 `ctx`（`root` / `runCtest` / `rebuild` / `verifyCtest` / `journalPath` / `logDir` /
 *   `ctestTimeoutSec`），缺省就是真实实现 —— 主流程与工具行为不因可测试性而分叉。
 *
 * 四态（⛔ 不许混）：
 *   - 红：`ok:true`（守卫退出码非 0 = 抓住了变异）；
 *   - 绿：`ok:false`（守卫退出码 0 = 没抓住）；
 *   - 不可判定(超时)：`unverifiable:true, unverifiableKind:'timeout'`（**不是**红）；
 *   - 不可判定(其它)：`unverifiableKind:'other'`（构建失败 / 构建目录未配置 / node 守卫 skip）。
 *
 * @returns {Promise<{result:object|null, restoreProblem:string|null}>} `result:null` ⇒ 还原失败（调用方必须中止）
 *   ★ async：`runCtest` / `verifyCtest` 走 `runCtestOnce` → `runTimedCommand`，那条路必须让事件循环转。
 */
export async function runMutationEntry(m, ctx = {}) {
  const root = ctx.root ?? ROOT;
  const runCtest = ctx.runCtest ?? runCtestGuard;
  const rebuild =
    ctx.rebuild ?? ((dir) => buildCtestTargets(null, dir, { target: buildTarget }));
  const verifyCtest =
    ctx.verifyCtest ?? ((name, buildDir, opts) => runCtestOnce(name, null, buildDir, opts));
  const journalPath = ctx.journalPath ?? null;
  const journalEntries = ctx.journalEntries ?? [];
  const buildDir = m.buildDir ?? CTEST_BUILD_DIR;
  const timeoutSec = ctx.ctestTimeoutSec ?? m.ctestTimeoutSec ?? null;
  // ★ `buildTarget`（条目字段）⇒ 三次构建都只重建守卫生效的那个 target：
  //   基线 / 变异 / 还原各一次，全量重链 15 MB 的 exe 是这条自检**唯一的**大头。
  //   ⛔ 安全性由「一轮结束的全量重建」兜底（`finalizeBuildDirs`）—— 这里省掉的只是**这一轮的**
  //   "顺便把无关 exe 重链两遍"，不是"基线的正确性"。
  const buildTarget = m.buildTarget ?? null;
  const ctestOpts = { root, timeoutSec, buildTarget };
  const abs = path.join(root, m.file);
  const before = fs.readFileSync(abs, 'utf8');
  if (!before.includes(m.from)) {
    // ★ 这不是"守卫没抓住"，而是**清单过期**：`from` 已经不在源文件里 ⇒ 这次自检什么也没验到。
    return {
      result: { ...m, verdict: '⚠ 变异点没找到（源文件已变 ⇒ 清单过期）', ok: false, stale: true },
      restoreProblem: null,
    };
  }
  // ★ CTest 条目在**施加变异之前**先验基线：构建目录没配置 / 用例不存在 / 基线本来就红 / **超时** /
  //   **基线被 skip** ⇒ 这次变异什么都证明不了 ⇒ 记"无法判定"，⛔ 不记成"守卫没红"、更不记成"守卫红了"。
  if (m.ctest) {
    // ★ 基线这一跑也要留日志：`-V` 下的 `***Skipped` 是**唯一**能认出"基线根本没跑"的证据
    //   （基线被 skip ⇒ 变异那一跑同样会 skip ⇒ 本批什么都证明不了）。
    const preLogDir = ctx.logDir ?? path.join(root, '.tmp', 'mutate-check', 'logs');
    fs.mkdirSync(preLogDir, { recursive: true });
    const preLog = path.join(preLogDir, `base-${process.pid}-${++mutationRunSeq}.log`);
    let pre;
    let preLogText = '';
    const preFd = fs.openSync(preLog, 'w');
    try {
      pre = await runCtest(m.ctest, preFd, buildDir, ctestOpts);
    } finally {
      fs.closeSync(preFd);
    }
    try {
      preLogText = fs.readFileSync(preLog, 'utf8');
    } catch {
      preLogText = '';
    }
    fs.rmSync(preLog, { force: true });
    const preSkipped = !pre.unavailable && ctestSkippedFromLog(preLogText, pre.status ?? null);
    if (pre.unavailable || preSkipped || pre.status !== 0) {
      const why = pre.unavailable ?? (preSkipped ? `\`${m.ctest}\` 基线被 skip（SKIP_RETURN_CODE 77）` : `\`${m.ctest}\` 基线退出码 ${pre.status}`);
      return {
        result: {
          ...m,
          verdict: `⏭ **无法判定（${pre.timedOut ? '超时' : '其它'}）**（CTest 基线${preSkipped ? '被 skip' : '不成立'}：${why}）`,
          ok: false,
          unverifiable: true,
          unverifiableKind: pre.timedOut ? 'timeout' : 'other',
          ran: { skipReason: why },
        },
        restoreProblem: null,
      };
    }
  }
  // ★ **先写 journal 再动文件**（原子：临时文件 + rename）。这一条就是"崩溃安全"的全部：
  //   进程在下一行之后被 SIGKILL / 断电打断时，下一次启动能从 journal 里认出"盘上是变异文"
  //   并把原文写回去，而不是把变异态当成正常起点继续跑。
  const mutated = before.replace(m.from, m.to);
  const entry = { file: m.file, before, mutated, guard: m.guard };
  if (m.ctest) {
    // CTest 类条目：恢复源文件 ≠ 恢复 exe ⇒ 把重建所需的信息也记下来
    entry.ctest = m.ctest;
    entry.buildDir = buildDir;
  }
  if (journalPath) journalAdd(journalPath, journalEntries, entry);
  fs.writeFileSync(abs, mutated);
  let code = null;
  let ran = null; // {pass, fail, skipped, skipReason}
  let unavailable = null; // CTest 条目：构建/ctest 跑不起来
  let timedOut = false; // 其中"跑不起来"恰好是**超时**（四态里的第三态，⛔ 与"其它"分开）
  let ctestSkipped = false; // CTest 条目：守卫**被 skip**（SKIP_RETURN_CODE 77）⇒ 不可判定(其它)
  let restoreProblem = null;
  // ★ 子进程输出落 `.tmp/`（gitignored）—— ⛔ 不写系统临时目录：沙箱里那可能是唯一写不进去的地方
  const LOG_DIR = ctx.logDir ?? path.join(root, '.tmp', 'mutate-check', 'logs');
  fs.mkdirSync(LOG_DIR, { recursive: true });
  const log = path.join(LOG_DIR, `mutate-${process.pid}-${++mutationRunSeq}.log`);
  try {
    // ★ 沙箱里不能**捕获**子进程输出（管道要命名管道 ⇒ EPERM）⇒ 用**文件描述符重定向**拿同一份答案
    //   （`tools/corpus.mjs` 的 `runCapture()` 就是这么绕的）。为什么要这份输出：
    //   **守卫 skip 时退出码也是 0** —— 只看退出码会把"这台机器上没跑"误报成"守卫没红"（实测踩过：
    //   `emulator-numeric-ops` 的两条要 `dist/install`，本机没有 ⇒ 变异看起来"没抓住"，其实是没跑）。
    const fd = fs.openSync(log, 'w');
    try {
      if (m.ctest) {
        const r = await runCtest(m.ctest, fd, buildDir, ctestOpts);
        code = r.status ?? null;
        unavailable = r.unavailable ?? null;
        timedOut = Boolean(r.timedOut);
        // ★ **在 `finally` 删日志之前**读：`-V` 下的 `***Skipped` 是 skip 的唯一证据（ctest 退出码仍 0）。
        //   ⛔ 只看退出码会把 skip 记成「❌ 守卫没红」—— 那是假红，会让整档自检坏掉。
        if (!unavailable && !timedOut) ctestSkipped = ctestSkippedFromLog(fs.readFileSync(log, 'utf8'), code);
      } else {
        const r = spawnSync(process.execPath, ['--test', '--test-isolation=none', m.guard], {
          cwd: root,
          stdio: ['ignore', fd, fd],
        });
        code = r.status;
      }
    } finally {
      fs.closeSync(fd);
    }
    if (!m.ctest) {
      const text = fs.readFileSync(log, 'utf8');
      const num = (k) => Number(new RegExp(`^ℹ ${k} (\\d+)$`, 'm').exec(text)?.[1] ?? 0);
      const skipReason = /^﹣ .*?# (.+)$/m.exec(text)?.[1] ?? null;
      ran = { pass: num('pass'), fail: num('fail'), skipped: num('skipped'), skipReason };
    }
  } finally {
    fs.rmSync(log, { force: true });
    // ★ 无论成败（**含超时**）都还原；还原后**比对全文**（只比长度不够）
    fs.writeFileSync(abs, before);
    if (m.ctest) {
      // ★ 还原只还原了**源文件**；判据在 exe 里 ⇒ 必须重建回基线，否则下一次 ctest 会假红。
      //   顺手把「还原 ⇒ 再跑必须绿」也机械化：不绿 / 超时未结束就中止（退出码 2）。
      const build = rebuild(buildDir);
      if (build.error || build.status !== 0) {
        restoreProblem = '还原后重建失败（exe 仍是变异版）';
      } else {
        const run = await verifyCtest(m.ctest, buildDir, ctestOpts);
        if (run.timedOut) {
          restoreProblem = `还原后 \`${m.ctest}\` 超时未结束（基线判不了）`;
        } else if (run.unavailable || run.status !== 0) {
          restoreProblem = `还原后 \`${m.ctest}\` 仍红（基线没有恢复）${run.unavailable ? `：${run.unavailable}` : ''}`;
        }
      }
    }
  }
  if (restoreProblem) return { result: null, restoreProblem };
  if (fs.readFileSync(abs, 'utf8') !== before) {
    return { result: null, restoreProblem: `${m.file} 与施加前不一致` };
  }
  // ★ 只有"还原 + 全文比对通过"之后才把条目移出 journal（顺序反了就等于"以为还原了"）
  if (journalPath) journalDrop(journalPath, journalEntries, entry);
  if (m.ctest) {
    return {
      result: {
        ...m,
        code,
        ran: ctestSkipped
          ? { skipReason: '守卫被 skip（SKIP_RETURN_CODE 77：这台机器没有 GPU / 输入不在场）' }
          : unavailable
            ? { skipReason: unavailable }
            : null,
        // ★ 三支（与 node 分支 `:2876-2884` 对齐）：红 / 绿 / **无法判定(其它：守卫被 skip)**。
        //   ⛔ skip 不许记成「❌ 守卫没红」（假红）—— 也不许记成「✅ 守卫红了」（假绿）。
        verdict: unavailable
          ? timedOut
            ? `⏭ **无法判定（超时）**（${unavailable}）`
            : `⏭ **无法判定（其它）**（${unavailable}）`
          : ctestSkipped
            ? '⏭ **无法判定（其它）**（守卫 skip：SKIP_RETURN_CODE 77 ⇒ 这台机器没有 GPU / 输入不在场）'
            : code !== 0
              ? `✅ 守卫红了（CTest 退出码 ${code}）`
              : '❌ **守卫没红**（CTest 退出码 0）',
        // ★ 与 node 侧一致：**跑不起来 / 没跑（skip）≠ 守卫没抓住**。`unavailable`（构建目录缺失 /
        //   cmake 不可用 / 构建失败 / 超时）只是"这台机器上无法判定"，⛔ 不许记成 `failed` 并 exit 1 ——
        //   否则在没有 `apps/emulator-cxx/out` 的机器上，所有 CTest 条目都会被误报成"守卫坏了"。
        ok: !unavailable && !ctestSkipped && code !== 0,
        unverifiable: Boolean(unavailable || ctestSkipped),
        unverifiableKind: unavailable ? (timedOut ? 'timeout' : 'other') : ctestSkipped ? 'other' : null,
      },
      restoreProblem: null,
    };
  }
  const skipped = ran && ran.fail === 0 && ran.skipped > 0;
  return {
    result: {
      ...m,
      code,
      ran,
      verdict: code !== 0
        ? `✅ 守卫红了（退出码 ${code}）`
        : skipped
          ? `⏭ **无法判定（其它）**（守卫 skip，退出码 0）：${ran.skipReason ?? '（无原因）'}`
          : `❌ **守卫没红**（退出码 0${ran ? `，跑了 ${ran.pass} 条` : ''}）`,
      ok: code !== 0,
      unverifiable: Boolean(skipped),
      unverifiableKind: skipped ? 'other' : null,
    },
    restoreProblem: null,
  };
}

/**
 * @returns {Promise<number>} 退出码：0 = 全部按预期变红；1 = 有守卫没抓住破坏 / 清单过期；
 *   2 = 还原失败 / **内部错误**（已中止；工作树可能被污染）；3 = 启动自愈拒绝运行（journal 非法 /
 *   工作树内容与 journal 不符）；4 = **全部条目都无法判定（其它原因）**（本机没有可跑的守卫 ⇒ 不能算通过）；
 *   5 = **有 CTest 守卫超时**（不可判定(超时) ⇒ 不能算通过；⛔ 不许被 exit 0 掩盖）
 *   ★ async：`runMutationEntry` → `runCtestOnce` → `runTimedCommand` 的等待循环必须让事件循环转。
 */
export async function main(argv = process.argv.slice(2)) {
  // ★ `--match <子串>`：只跑匹配的变异（匹配 `file` / `what` / `guard`）。
  //   ★ 为什么必须有：本工具**会临时改写工作树里的文件** ⇒ 只想验"我这一域新加的守卫会不会红"时，
  //     不该被迫把全部变异跑一遍（窗口越长，越可能撞上别的进程在编辑）。技能文档里写的
  //     `pnpm tools mutate check --match shape` 原先**只是个不存在的 flag**（被静默忽略 ⇒ 跑全套）。
  const mi = argv.indexOf('--match');
  const match = mi >= 0 ? (argv[mi + 1] ?? null) : null;
  // ★ `--ctest-timeout <秒>`：全局覆盖 CTest 单测超时（缺省按守卫名分层，见 `CTEST_TIMEOUT_SEC`）。
  //   放在 `--help` 之前解析：错误的开关必须立刻红，而不是跑完全套才知道。
  const cti = argv.indexOf('--ctest-timeout');
  let ctestTimeoutSec = null;
  if (cti >= 0) {
    const raw = argv[cti + 1];
    const sec = Number(raw);
    if (raw === undefined || !Number.isFinite(sec) || sec <= 0) {
      console.error(`--ctest-timeout 只认正数秒（给的是 ${JSON.stringify(raw)}）`);
      return 1;
    }
    ctestTimeoutSec = sec;
  }
  // ★ `--tier <all|active|frozen|lift|js>`：全量 504 条里约一半是**冻结件**（`apps/emulator-new`
  //   与已废弃的 `apps/emulator`），跑它们只证明"冻结件没被改"；日常用 active / lift / js 分档，
  //   全量留给收口。缺省 all（不改变原有语义）。
  const ti = argv.indexOf('--tier');
  const tier = ti >= 0 ? (argv[ti + 1] ?? '') : 'all';
  if (!['all', 'active', 'frozen', 'lift', 'js'].includes(tier)) {
    console.error(`--tier 只认 all / active / frozen / lift / js（给的是 ${JSON.stringify(tier)}）`);
    return 1;
  }
  const isFrozen = (m) => m.file.startsWith('apps/emulator-new/') || m.file.startsWith('apps/emulator/');
  let selected = match ? MUTATIONS.filter((m) => `${m.file} ${m.what} ${m.guard}`.includes(match)) : MUTATIONS;
  if (tier === 'active') selected = selected.filter((m) => !isFrozen(m));
  else if (tier === 'frozen') selected = selected.filter(isFrozen);
  else if (tier === 'lift') selected = selected.filter((m) => m.file.startsWith('apps/emulator-lift/'));
  else if (tier === 'js') selected = selected.filter((m) => !m.guard.startsWith('ctest:'));
  // ★ `--help` 必须**只打用法就走**：它原先落到"跑全套变异"那条路（实测踩过一次）——
  //   而本工具**会临时改写工作树里的文件**，在有别的进程（人或 agent）正在编辑时跑，
  //   还原那一步可能把别人的改动一起写回去。
  if (argv.includes('--help') || argv.includes('-h')) {
    console.log(
      '用法：node tools/mutate-check.mjs [--list] [--match <子串>] [--tier all|active|frozen|lift|js]\n' +
        '                              [--ctest-timeout <秒>]\n\n' +
        '  不带参数 = 逐条施加变异 ⇒ 跑对应守卫 ⇒ 要求退出码非 0（最后逐条还原并比对全文）。\n' +
        '  --tier active 跳过冻结件（emulator-new / 已废弃 emulator）；lift 只跑 emulator-lift；js 跳过 CTest。\n' +
        '  --ctest-timeout <秒> 覆盖 CTest 守卫的**单测超时**。缺省按守卫名分层：\n' +
        '    普通 120 s；名字里含**独立段** `assets` / `external`（如 assets-gpu / external-boot）600 s。\n' +
        '    超时 ⇒ 收掉 ctest 与被守可执行组成的**进程树**、记「不可判定（超时）」并继续下一条；\n' +
        '    ⛔ 超时**不是**「守卫红了」。四态：红 / 绿 / 不可判定(超时) / 不可判定(其它)。\n' +
        '  ★ 条目可带 `optInEnv`（如 `LIFT_STRESS`）：那指向**配置期 opt-in** 的 CTest（注册落在\n' +
        '    `if(... AND DEFINED ENV{…})` 里）⇒ 没设该 env 时记「不可判定（本机未启用）」并打\n' +
        '    `⏭ 本机未启用`，⛔ 与「⛔ 守卫不存在」（注册被删，必须响）分开。要看它：设好 env ⇒\n' +
        '    重新 configure + 构建，再跑本工具。\n' +
        '  ⛔ **它会临时改写工作树里的文件** ⇒ 有别的进程正在编辑本仓时**不要**跑（还原会覆盖别人的改动）。\n' +
        '  --list 只列清单（不动任何变异文件；但每次启动都会先自愈上次中断留下的 journal）。\n' +
        '    ★ 它同时做**静态存在性检查**（`file` 在不在、`from` 还是不是源文件的子串、\n' +
        '      `buildTarget` 是否等于 `CMakeLists.txt` 里 `add_test(NAME … COMMAND …)` 推出的可执行 target；\n' +
        '      不构建、不施加变异）\n' +
        '      ⇒ 清单过期（源文件改名 / 搬家 / 文案改写）或 target 名填错在这里就非 0 退出，\n' +
        '        而不是等全量跑到那一条（填错的 target 会让那条变异永远记「不可判定（构建失败）」）。\n' +
        '  --match <子串> 只跑 file / what / guard 里含该子串的变异（例：--match shape）。\n' +
        `  ★ **日常只跑你这轮动到的那一条**（\`--match <该条 what 里的独特子串>\`）。不带 \`--match\` = **全量 ${MUTATIONS.length} 条**，\n` +
        `    其中 ${MUTATIONS.filter((m) => m.ctest).length} 条是 CTest 条目、每条要 **2 次增量构建**（变异前 / 还原后各一次）+ **3 次 ctest**\n` +
        '    （基线 / 变异 / 还原确认）⇒ 代价随条数线性放大，**十几分钟起步**；而且它会**临时改写工作树里的文件**\n' +
        '    ⇒ 别人还在编辑、或正在测墙钟 A/B 时 ⛔ 不要开。\n' +
        '    全量是**刻意扫**（改过清单结构 / 里程碑前），⛔ 不是"顺手跑一下"。\n\n' +
        '  退出码：0 全部按预期变红 / 1 有守卫没抓住或清单过期 / 2 还原失败 / 内部错误 / 3 启动自愈拒绝运行 /\n' +
        '          4 全部不可判定(其它) / 5 有不可判定(超时)。\n\n' +
        '  崩溃安全：动文件前把 {file,before,mutated,guard?} 原子写进 .tmp/mutate-check/journal.json；\n' +
        '  SIGINT/SIGTERM 同步恢复；SIGKILL/断电后下次启动自己恢复。判不了（盘上是第三种内容）⇒\n' +
        '  拒绝运行、退出码 3、journal 保留。',
    );
    return 0;
  }
  // ★ 启动自愈（--list 也做）：上次若是被 SIGKILL / 休眠 / 断电打断，工作树里可能还留着变异态。
  //   ⛔ 不先处理就继续跑 = 可能"在变异态上跑绿"；判不了就拒绝运行（一个字节也不写）。
  const journalPath = resolveJournalPath(ROOT);
  const recovery = recoverMutationJournal({ root: ROOT, journalPath });
  if (!recovery.ok) {
    console.error(
      `✗ 拒绝运行：启动自愈未完成（${recovery.reason}）。journal 保留在 ${journalPath}。\n` +
        '  修法：人工确认那些文件当前内容后，把 journal 里的 before/mutated 改对（或确认工作树已是原文再删掉 journal）后重试。',
    );
    return 3;
  }
  // ★ `--match` 一条都没选中时**两条路都必须报**（`--list` 与 check）：否则 `--list --match 打错的串`
  //   会打"0 条里验证了 0 条 / 静态检查通过"—— 一次**空验证**看起来像通过（正是本条要防的假绿）。
  if (match && selected.length === 0) {
    console.error(`没有匹配「${match}」的变异（--list 看清单）`);
    return 1;
  }
  if (argv.includes('--list')) {
    const gone = [];
    for (const m of selected) {
      const missing = guardMissing(m);
      // ★ `⛔ 守卫不存在` 是**统一的可 grep 标记**：`--list` 与 check 两条路都必须打它。
      //   原先这里只打 `⛔ ${missing}`（例如"⛔ 守卫文件不存在：…"）⇒ 同一件事有两种字面，
      //   守卫（`tools/test/mutate-journal.test.mjs` 的新用例）只能各配一个断言。
      // ★ 第三种状态（`{unverifiable}`：本机没启用的**配置期 opt-in** 守卫）单独打 `⏭ 本机未启用`：
      //   它既不是"⛔ 守卫不存在"（注册被删 ⇒ 必须响），也不是"跑了没红"。⛔ 不许混进前两类。
      const goneWhy = typeof missing === 'string' ? missing : null;
      const offWhy = missing !== null && typeof missing === 'object' ? missing.unverifiable : null;
      console.log(
        `${m.what}\n    ${m.file}  ⇒ 守卫 ${m.guard}` +
          (goneWhy ? `\n    ⛔ 守卫不存在 —— ${goneWhy}` : '') +
          (offWhy ? `\n    ⏭ 本机未启用 —— ${offWhy}` : ''),
      );
      if (goneWhy) gone.push({ m, missing: goneWhy });
    }
    if (gone.length) {
      console.error(
        `\n⛔ ${gone.length}/${selected.length} 条**指向不存在的守卫**（跑起来会让"守卫红了"变成假绿）：\n` +
          gone.map(({ m, missing }) => `  - ${m.file}（${missing}）`).join('\n'),
      );
    }
    // ★ 静态存在性检查（**只读**：不构建、不施加变异）：`file` 在不在、`from` 还是不是它的子串、
    //   以及带 `buildTarget` 的条目与 CMakeLists 的 `add_test(NAME … COMMAND …)` 映射一致。
    //   原先这一步只在"全量跑到那一条"时才发生（`⚠ 变异点没找到`）⇒ 清单可以静默过期一整轮，
    //   而 `--list` 看起来一切正常。现在 `--list` 一次就把全部失配报出来（判据见 `staticManifestCheck`）。
    const stat = staticManifestCheck(selected, ROOT);
    for (const s of stat.skipped) console.log(`    ⏭ 静态检查跳过：${s.file}（${s.why}）`);
    if (stat.problems.length) {
      console.error(
        `\n⛔ ${stat.problems.length}/${selected.length} 条**静态失配**` +
          '（`from` 已不是源文件的子串 / 出现次数 ≠ 1 / `buildTarget` ≠ `add_test(NAME … COMMAND …)` 推出的 target / ' +
          'spawn 产品 exe 但缺 `add_dependencies`：' +
          '全量跑到它只会记「变异点没找到」/「不可判定（构建失败）」或**静默只改第一处**，什么也没验到）：',
      );
      // ★ 逐条：`⛔ <file>: from 片段已不存在` + 从 `what` 取的一句话（定位用）
      for (const p of stat.problems) console.error(`  ⛔ ${p.file}: ${p.why}\n      ${p.what}`);
    }
    if (gone.length || stat.problems.length) return 1;
    console.log(
      `\n静态检查通过：${selected.length} 条里验证了 ${stat.checked} 条` +
        `（file 存在 + from 是源文件子串且**恰好出现 1 次**）` +
        (stat.targetChecked ? ` · buildTarget：${stat.targetChecked} 条与 CMakeLists 的 add_test 映射一致` : '') +
        (stat.spawnChecked + stat.spawnNotApplicable
          ? ` · spawn 产品 exe：${stat.spawnChecked} 条 add_dependencies 覆盖` +
            `（${stat.spawnNotApplicable} 条不 spawn 其它 target，判据不适用）`
          : '') +
        (stat.skipped.length ? ` · ${stat.skipped.length} 条不适用（多文件声明，已逐条报出）` : ' · 0 条不适用'),
    );
    return 0;
  }

  const results = [];
  // ★ 信号恢复要看的"当前已施加"清单 + journal 里的条目（两者同源，顺序一致）
  const journalEntries = [];
  // ★ 一轮结束的**全量重建回基线**（每个构建目录一次，⛔ 不带 `--target`）。
  //   为什么：条目带 `buildTarget` 时本轮只重建守卫那一个 target ⇒ `out/` 里其它 exe 可能停在
  //   **变异版**。谁拿它去测墙钟 A/B 就是静默错（没有症状）⇒ 收尾必须回到基线，失败就不许 exit 0。
  //   ⛔ 唯一不走这条的是"还原失败"（源文件可能还是变异文 ⇒ 全量构建会把变异编进去）——
  //     那条路保留 journal，由下次启动的 `recoverMutationJournal`（它自己全量重建）自愈。
  const buildDirs = [...new Set(selected.filter((m) => m.ctest).map((m) => m.buildDir ?? CTEST_BUILD_DIR))];
  const finish = (code) => {
    if (!buildDirs.length) return code;
    const fin = finalizeBuildDirs(null, buildDirs);
    if (!fin.ok) {
      console.error(
        `\n✗ **exe 未能重建回基线**（${fin.failedDir}：${fin.why}）⇒ 不能算通过：` +
          `\`${fin.failedDir}\` 里可能有停在变异版的 exe（⛔ 别拿它测 A/B）。`,
      );
      return 2;
    }
    console.log(`已全量重建回基线（${fin.dirs.length} 个构建目录，全量 target、不带 --target）`);
    return code;
  };
  installSignalGuards(journalPath);
  for (const m of selected) {
    // ★ 守卫不存在 ⇒ 这次变异**什么也证明不了**（`node --test <缺失文件>` 退出码 1 会被读成"守卫红了"）
    const gone = guardMissing(m);
    if (gone && typeof gone === 'string') {
      results.push({ ...m, verdict: `⛔ **守卫不存在**（${gone}）⇒ 本次变异什么也没证明`, ok: false, guardGone: true, missingWhy: gone });
      continue;
    }
    if (gone) {
      // ★ **配置期 opt-in 的守卫没启用** ⇒ 本机跑不了它。记「无法判定（其它）」，⛔ 不是"守卫没红"
      //   （更不是"红了"）：`runCtestOnce` 对未注册的用例只能拿到 `No tests were found` + 退出码 0。
      results.push({
        ...m,
        code: null,
        ran: { skipReason: gone.unverifiable },
        verdict: `⏭ **无法判定（其它）**（${gone.unverifiable}）`,
        ok: false,
        unverifiable: true,
        unverifiableKind: 'other',
      });
      continue;
    }
    // ★ 施加 → 跑守卫 → 还原 → 判四态，全在 `runMutationEntry` 里（导出给超时守卫用同一份实现）
    const { result, restoreProblem } = await runMutationEntry(m, {
      root: ROOT,
      journalPath,
      journalEntries,
      ctestTimeoutSec,
    });
    if (restoreProblem) {
      // ★ journal 条目**故意留着**：源文件或 exe 还没回到基线 ⇒ 下次启动靠它继续/拒绝
      // ★ ⛔ 这里**不做**收尾的全量重建：源文件可能还是变异文 ⇒ 全量构建会把变异编进去
      //   （比"exe 陈旧"更糟）。交给下次启动的 `recoverMutationJournal`（它全量重建）。
      console.error(`\n✗ **还原失败**：${m.file} —— ${restoreProblem}。已中止（工作树可能被污染，请查 git diff）`);
      return 2;
    }
    results.push(result);
  }

  console.log('守卫自检（改坏一处常量 ⇒ 指定守卫是否当场红）：\n');
  for (const r of results) console.log(`  ${r.verdict}  ${r.what}`);
  const stale = results.filter((r) => r.stale);
  const goneRows = results.filter((r) => r.guardGone);
  const failed = results.filter((r) => !r.ok && !r.unverifiable && !r.stale && !r.guardGone);
  // ★ 四态：**超时**与**其它不可判定**分开计数/分开展示（⛔ 两者不许混，更不许混进"红"）。
  const timedOutRows = results.filter((r) => r.unverifiableKind === 'timeout');
  const otherUnver = results.filter((r) => r.unverifiable && r.unverifiableKind !== 'timeout');
  const unver = [...timedOutRows, ...otherUnver];
  const redCount = results.length - failed.length - unver.length - stale.length - goneRows.length;
  console.log(
    `\n${redCount}/${results.length} 条按预期变红` +
      (unver.length
        ? ` · ${unver.length} 条**不可判定**（超时 ${timedOutRows.length} / 其它 ${otherUnver.length}；` +
          '⛔ 不是"红"，也不算失败）'
        : '') +
      '。',
  );
  if (timedOutRows.length) {
    console.log(
      '⏭ **不可判定（超时）** —— 守卫在单测超时内没结束（进程树已收掉，已继续下一条）：\n' +
        '   ★ 超时**不是**「守卫红了」（它什么也没证明）。先查该守卫是否真会挂死；确属慢档就调大 `--ctest-timeout`。',
    );
    for (const u of timedOutRows) console.log(`  - ${u.what}（${u.guard}）：${u.ran?.skipReason ?? '（无原因）'}`);
  }
  if (otherUnver.length) {
    console.log('⏭ **不可判定（其它）** —— 这些守卫在**本机**跑不起来（不是"没抓住"）：');
    for (const u of otherUnver) console.log(`  - ${u.what}（${u.guard}）：${u.ran?.skipReason ?? '（无原因）'}`);
  }
  if (goneRows.length) {
    // ★ 单独一类：**守卫不存在** ≠ "守卫没红"、≠ "无法判定"。它必须响亮，因为跑起来会变成假绿。
    console.error('⛔ 这些变异的**守卫不存在**（守卫文件被删 / CTest 注册被删）—— 变异"红了"也什么也没证明（必须从清单里删掉，或改指仍存在的守卫）：');
    for (const g of goneRows) console.error(`  - ${g.file} → ${g.missingWhy}`);
    return finish(1);
  }
  if (stale.length) {
    console.error('⚠ 这些变异的**施加点已经不存在**（源文件改过 ⇒ 清单过期，等于什么也没验到）：');
    for (const s of stale) {
      console.error(`  - ${s.file}：找不到 ${JSON.stringify(s.from)}（本该由 ${s.guard} 抓）`);
    }
  }
  if (failed.length) {
    console.error('★ 这些守卫**没抓住**已知的破坏 ⇒ 它们不是"会红的守卫"（断言恒真？或压根没验那个常量）：');
    for (const f of failed) console.error(`  - ${f.what}（${f.guard}）`);
    return finish(1);
  }
  if (timedOutRows.length) {
    // ★ 超时 ⇒ **不可判定**，但也不能被 exit 0 掩盖：它意味着"这条守卫会红"这件事没被证明。
    //   ⛔ 不许把它当"通过"（假绿），也不许把它当"守卫没抓住"（冤枉守卫）—— 单列退出码 5。
    console.error(
      `✗ ${timedOutRows.length} 条 CTest 守卫**超时未结束**（不可判定(超时)，不是"红"）` +
        ' ⇒ 不能算通过；先查该守卫是否真会挂死，确属慢档再用 `--ctest-timeout <秒>` 调大。',
    );
    return finish(5);
  }
  if (unver.length && unver.length === results.length) {
    // ★ 全部条目都"无法判定（其它）"（构建目录不存在 / 资产缺失）≠ 通过：旧实现这里仍 exit 0，
    //   在没建 out/ 的机器上 `pnpm test:mutation` 会被读成"全部红了"。
    console.error(
      '✗ 全部 %d 条变异都无法判定（本机没有可跑的守卫）⇒ 不能算通过；先构建对应域 / 备齐资产再跑。',
      results.length,
    );
    return finish(4);
  }
  return finish(stale.length ? 1 : 0);
}

const isDirectRun = process.argv[1] && path.resolve(process.argv[1]) === path.resolve(fileURLToPath(import.meta.url));
if (isDirectRun) {
  // ★ `main()` 现在是 async ⇒ 直接赋 Promise 会 `ERR_INVALID_ARG_TYPE`（`process.exitCode` 只认数字）。
  //   ⛔ `.catch` 绝不许落 0：那会把"内部错误"读成"全部按预期变红"。复用 2（还原失败 / 内部错误），
  //   与 `main()` 里还原失败那条同码同口径（工作树可能被污染 ⇒ 调用方本来就必须查 `git diff`）。
  main().then(
    (code) => {
      process.exitCode = code;
    },
    (err) => {
      console.error(`\n✗ 内部错误（工作树可能被污染，请查 git diff）：${err?.stack ?? err}`);
      process.exitCode = 2;
    },
  );
}

