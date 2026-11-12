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
 * 它同时做**静态存在性检查**：每条 `from` 是不是还在源文件里。判据抽成导出函数
 * `staticManifestCheck`，守卫在 `tools/test/mutate-static.test.mjs`；清单过期的根因见那里的注释）。
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
    return { error: new Error(`子进程在写出退出码之前就结束了（${command}）`) };
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
 */
function buildCtestTargets(logFd, buildDir = CTEST_BUILD_DIR, { attempts = 3, delayMs = 800 } = {}) {
  let last = null;
  let prev = null;
  for (let i = 1; i <= attempts; i += 1) {
    last = spawnSync('cmake', ['--build', buildDir], { cwd: ROOT, stdio: ctestStdio(logFd) });
    if (!last.error && last.status === 0) {
      if (i > 1) {
        console.error(
          `（重建 ${buildDir} 第 ${i} 次才成功；上一次 ${prev?.error ? prev.error.message : `退出码 ${prev?.status}`}，` +
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
 * 只跑**一条** CTest（不构建），带**单测超时**。
 * 抽出来是为了"还原后复验基线"与"变异后跑守卫"走同一份超时逻辑（⛔ 不许一处有一处没有）。
 * ★ async 是因为 `runTimedCommand` 的等待循环必须让事件循环转（见那边注释）。
 * @returns {Promise<{status:number}|{timedOut:true, timeoutSec:number, unavailable:string, reason:'timeout'}|{unavailable:string, reason:string}>}
 */
async function runCtestOnce(name, logFd, buildDir, opts = {}) {
  const timeoutSec = opts.timeoutSec ?? defaultCtestTimeoutSec(name);
  const args = opts.args ?? ['--test-dir', buildDir, '-R', `^${name}$`, '--output-on-failure'];
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
    const build = (opts.build ?? buildCtestTargets)(logFd, buildDir);
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
  // ★ **配置期 opt-in** 的守卫（`m.optInEnv`，例：注册在 `if(... AND DEFINED ENV{LIFT_ASSETS})` 里）：
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
 *
 * @param {Array<{file:string, from:string, what:string}>} mutations 清单（真清单或其副本）
 * @param {string} [root] 仓库根（测试用临时根目录）
 * @returns {{checked:number, problems:Array<{file:string, what:string, why:string}>,
 *            skipped:Array<{file:string, what:string, why:string}>}}
 *   `checked` = **真验过**（file 可读 + `from` 是子串）的条数；`problems` = 失配；
 *   `skipped` = 静态检查不适用的条目（附原因）。
 */
export function staticManifestCheck(mutations, root = ROOT) {
  const problems = [];
  const skipped = [];
  let checked = 0;
  for (const m of mutations) {
    const file = String(m?.file ?? '');
    const what = String(m?.what ?? '');
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
  }
  return { checked, problems, skipped };
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
    '**CTest 类条目**（`guard: \'ctest:<名字>\'`）会重建 + 跑那一条 CTest（还原后也重建成基线）；★ 看条目的 `buildDir` 决定构建哪个工程（缺省 `apps/emulator-cxx/out`，emulator-lift 条目显式写 `apps/emulator-lift/out`）；★ 单测超时缺省按守卫名分层（普通 120 s；名字含独立段 `assets` / `external` 600 s；`--ctest-timeout <秒>` 覆盖）⇒ 超时 = **不可判定（超时）**、收进程树、继续下一条，⛔ 不是"守卫红了"；★ 条目带 `optInEnv`（如 `LIFT_ASSETS`）= 该 CTest 是**配置期 opt-in**（注册在 `if(... AND DEFINED ENV{…})` 里）⇒ 没设该 env 时记「不可判定（本机未启用）」、⛔ 不混进「⛔ 守卫不存在」（注册被删）',
    '**预写日志** `.tmp/mutate-check/journal.json`（gitignored；动文件前原子落盘，逐条还原后移除，全清时删除）—— 中途被 SIGKILL / 断电打断后，下次启动靠它自愈或拒绝运行',
  ],
  access: 'r（读原文进内存）→ **w journal（预写日志，原子：临时文件 + rename）**→ rw（写入变异）→ **w（还原，且比对全文）**；CTest 类条目额外重建该条目的 `buildDir`（缺省 `apps/emulator-cxx/out`；生成物，已 gitignore）；★ 启动时先自愈 journal（判不了就拒绝运行，退出码 3，journal 保留）；★ 不碰旧仓',
  tool: 'tools/mutate-check.mjs',
};

export const OPERATIONS = [
  { name: 'check', argv: [], mutates: true, summary: '★ 逐条施加变异 ⇒ 跑指定守卫（`node --test` 用例，或 `ctest:<名字>` 的 CTest）⇒ 要求退出码非 0，最后**逐条还原并比对全文**（会同步恢复 SIGINT/SIGTERM；SIGKILL 后下次启动靠 journal 自愈）；★ CTest 带**单测超时**：超时 = 不可判定（超时）、收掉进程树、继续下一条（退出码 5），⛔ 不记成"守卫红了"' },
  { name: 'list', argv: ['--list'], mutates: false, summary: '只列变异清单（不动任何变异文件；但**启动时仍会先自愈 journal** —— 上次留下的变异态不该因为"只看清单"而继续存在）；★ 顺带做**静态存在性检查**：`file` 存在 + `from` 是该文件内容的子串（不构建、不施加变异），失配 ⇒ 非 0 退出' },
];

export { MUTATIONS };

/**
 * 变异清单：每条 = 一处"已知的破坏" + 应该抓住它的守卫。
 * ★ 口径（`AGENTS.md` §10 / `docs/03-emulator/README.md` §0.1）：**每个守卫留一条代表**。
 *   条目的职责是证明"这个守卫**会**红"（不然它会烂成恒真断言），⛔ **不是**给每个缺陷 / 每个常量
 *   各配一条 —— 那样清单会随修 bug 的轮次线性膨胀，而每条都要花一次构建 + 一次实跑来"自证"。
 *   新增守卫时补一条代表；修好一个缺陷时**不要**顺手再补一条（那条缺陷的历史进 `defects.md`）。
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
    guard: 'tools/test/ledger.pe.external.test.mjs',
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
    what: 'stdcall 不再弹参 ⇒ 栈每次调用涨 argc*4 ⇒ shim-abi 的 esp 平衡断言红',
  },

  {
    // ★ 代表 "GPU 准入少了一项" 这一类错（REQ-01M737D3BHNMRY97T9A02DARCC）：
    //   `gpuEligible` 里 `MAG != MIN` 的准入被删（判据恒假）⇒ `MAG=POINT + MIN=LINEAR` 的批
    //   重新进入 GPU 队列，且回退账变空 —— CPU 参考按"任一 LINEAR ⇒ 双线性"、GPU 各用各的，
    //   两边都"成功"、整批像素不同，而 `--gpu-strict` 看不见（不是 fallback）。
    //   守卫：`apps/emulator-lift/tests/gpu_batch.cpp` 的
    //   `gpu-batch/sampler-filter-eligibility`（pure，`gpu-batch-order` 跑它）。
    file: 'apps/emulator-lift/src/runtime/d3d9_pipeline.cpp',
    from: '        if (ss.mag_filter != ss.min_filter) {',
    // ★ 不用 `if (false)`：改成"永不成立的合取"⇒ 准入实质失效（等价于删掉这一条），
    //   同时避开 `/W4 /WX` 下常量条件的告警（C4127 在本工程是否触发不靠猜）。
    to: '        if (ss.mag_filter != ss.min_filter && ss.mag_filter == 0xFFFFFFFFu) {',
    guard: 'ctest:gpu-batch-order',
    ctest: 'gpu-batch-order',
    buildDir: 'apps/emulator-lift/out',
    what:
      '删掉 `gpuEligible` 的采样过滤准入（MAG != MIN 不再拒绝）⇒ `MAG=POINT + MIN=LINEAR` 的批' +
      '重新交给 GPU（queued_draws 0→1）且回退账变空 ⇒ gpu-batch/sampler-filter-eligibility 当场红',
  },


  {
    // ★ 代表 "stateless 判定被放宽" 这一类错。`active < 2u` ⇒ `slot[0] == 1`（已启动 / 在播）也被当成
    //   惰性 ⇒ **有写支**被短路：回退断言、零写断言、替身哨兵同时失守。
    //   ★ 这里不用 `if (false)` 的原因**不是编译**：本工程 `/W4 /WX` 下 `if (false)` **实测能编过**
    //     （清单里就有条目把判据短路成 `if (false …)`，照样构建通过、守卫照样红；早先"C4127 ⇒ 构建红"
    //     的说法不成立）。原因是**表达**：`if (false)` 只是"判据整条消失"，而这条 `what` 说的是
    //     **放宽**到"已启动"支 ⇒ `active < 2u` 精确表达那种放宽，守卫红的方向才与 `what` 对得上。
    file: 'apps/emulator-lift/src/runtime/native_voice.cpp',
    from: '        if (active == 0u) continue;',
    to: '        if (active < 2u) continue;',
    guard: 'ctest:lift-voice-poll',
    ctest: 'lift-voice-poll',
    buildDir: 'apps/emulator-lift/out',
    what:
      'Voice::Tick 的 stateless 判定放宽到"已启动"支 ⇒ 有写（首 tick / 到期）被短路：' +
      'lift-voice-poll 的"必须回退 / 零写 / 解释器真的跑了"当场红',
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
    what:
      'THREAD_LOCAL 退回空（softfloat 状态进程级共享）⇒ barrier 强制交错下两条线程的异常位同时在场 ' +
      '⇒ 一个 take 读到对方的位、另一个读到 0 ⇒ fpu-thread-flags 每次必红（实测 10/10）',
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
    what:
      'InsertNode 的平局判据从严格 > 放宽成 >=（等长也替换 match_pos）⇒ 先入树的候选被顶掉：' +
      'lzss/tie-strict-greater 的定点字节 EE F0 变 F0 F0，当场红（实测见 .tmp/reveq/…/report.md）',
  },

  {
    // ★ 代表 "`_memcpy` 的重叠接管把**方向判据写反**"这一类错（本单）。
    //   判据：`apps/emulator-lift/tests/native_override.cpp` 的 `memcpy-ovl/*` —— 三类重叠
    //   （`dst<src` / `dst>src` / `dst==src`）× n ∈ {0,1,3,4,7,8,64,4096}，oracle = 解释器跑
    //   **guest 真字节**（assets 档另有 `memcpy-overlap/*` 用 `0x4E74B0` 的真字节，同一批形状）。
    //   guest 在 `dst > src` 且重叠时走**后向**（EA 0x4E7670：`std; rep movsd; cld`），
    //   `dst <= src` 时走**前向**（EA 0x4E74F4 起）⇒ 把这一行改成 `>` 就是"两支互换"（= 一律后向），
    //   `dst<src` 与 `dst>src` 两类当场红。
    //   ★ 为什么挑"方向写反"而不是"重叠 ⇒ 回退"：后者的断言是 `fallbacks==0`，红了只说明"没接管"；
    //     方向写反才是本项唯一真正难的那一半（见 `native_overrides.h` 的 `_memcpy` 契约段）。
    //   ★ 为什么不用 `std::memmove` 当变异：它在**语义上**与 guest 一致（guest 重叠时就等价于 memmove），
    //     换上去不会红 —— 那正说明"memmove 是对的"，但它不可复核（宿主 libc 的分块/方向不在语料里）。
    file: 'apps/emulator-lift/src/runtime/native_crt.cpp',
    from: '    if (dst < src) {\n        copyForwardBytes(d, s, n);',
    to: '    if (dst > src) {\n        copyForwardBytes(d, s, n);',
    guard: 'ctest:native-override',
    ctest: 'native-override',
    buildDir: 'apps/emulator-lift/out',
    what:
      '重叠软实现的方向判据写反（`dst<src` 也走成后向循环）⇒ memcpy-ovl/dst<src-* 与 dst>src-* 的' +
      '逐字节差分当场红（实测见 .tmp/audio-perf/memcpy.md 的变异表）',
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
    what:
      '`rep movs*` 快路径的方向闸门写反（正/逆涂抹互换）⇒ 逆向重叠的 4 B 分块把源字节涂抹：' +
      'rep-bulk 的 diff 矩阵逐字节差分（34 例）与 hits/declines 断言当场红',
  },

  {
    // ★ 代表 "x87 override 的运算顺序 / `fchs` 被弄丢" 这一类错（REQ-01M6NZSAAD0WCNBE3RKMFF84R5）。
    //   判据：`apps/emulator-lift/tests/native_override_assets.cpp` 的 ④ = `sub_4CCC60` 的**函数级真字节
    //   两臂**（同一份 guest 机器码跑解释器当 oracle，再跑 native 实现；比 dst / 寄存器 / x87 状态字）。
    //   `x87ChangeSign(b)` 是 `.lst` 0x4CCCC8 `fchs` 的唯一落点（B = -(r*sy*sin)）⇒ 注释掉它，
    //   B 的符号翻正 ⇒ 每个点的 dst[i] 都变 ⇒ 当场红。
    //   ★ 为什么挑这一条而不是 sin/cos 对调：`.lst` 0x4CCC80/0x4CCC84 的两次求值互相独立
    //   （都在同一个 θ 上，各自只写自己的栈槽）⇒ 那种"变异"不会改变任何 guest 可见状态，红得没意义。
    file: 'apps/emulator-lift/src/runtime/native_affine.cpp',
    from: '    x87ChangeSign(b);',
    to: '    // x87ChangeSign(b);',
    guard: 'ctest:native-override-assets',
    ctest: 'native-override-assets',
    buildDir: 'apps/emulator-lift/out',
    what:
      'B 系数（`fchs` @0x4CCCC8）不再取号 ⇒ 每个顶点的 dst[i] 符号项错 ⇒ ' +
      'native-override-assets 的 sub_4CCC60 两臂逐字节当场红',
  },

  {
    // ★ 代表 "MDCT/FFT 内核的旋转符号被弄错" 这一类错（sub_4E0B40，**数值容忍档**）。
    //   判据：`apps/emulator-lift/tests/native_override_assets.cpp` 的 `mdct/*` —— 同一份
    //   `mdct_lookup` + 同一份输入跑**解释器臂**（真 guest 字节 = oracle）与 native 臂，
    //   逐值比 `max|Δ| <= 1e-6 * peak`。把第一段旋转的 `-` 改成 `+` ⇒ 输出整段错 ⇒ 当场红。
    //   ★ 为什么挑这一条而不是"蝶形里某个乘加顺序"：顺序类改动只差 1 ulp，容差档**本来**就允许，
    //   红不了（也不该红）；符号改动是 O(1) 的差，才配当这一档的代表。
    file: 'apps/emulator-lift/src/runtime/native_mdct.cpp',
    from: '            oX[1] = store(ld(iX[0]) * ld(T[3]) - ld(iX[2]) * ld(T[2]));',
    to: '            oX[1] = store(ld(iX[0]) * ld(T[3]) + ld(iX[2]) * ld(T[2]));',
    guard: 'ctest:native-override-assets',
    ctest: 'native-override-assets',
    buildDir: 'apps/emulator-lift/out',
    what:
      '`mdct_backward` 第一段旋转的符号翻正 ⇒ 输出整段错（O(1) 量级）⇒ ' +
      'native-override-assets 的 mdct 两臂差分当场红',
  },

  // ★ 下面三条代表 PCM 段（`sub_48D610` 的 `0x48D767..0x48D89A`）的三类**静默错**
  //   （REQ-01M75W6BB3PY3MF42ZTS23KTBS）。判据都在 `apps/emulator-lift/tests/native_override_assets.cpp`
  //   的 `pcm/*`（真字节两臂：解释器 = 同一 EA 的 oracle ⇒ 逐字节比 dst + `[ebp-108h]` + 出口状态 + insns）。
  //   ★ 三条都**只在值表含非满幅 / 负的非 clamp 样本时才红**：满幅值表会被 clamp 掩盖
  //     （`0.5*32767` vs `0.5*32768` 都落在范围内；负数一律 clamp 就看不到有符号判据）。
  {
    // M1：乘数 32767.0 → 32768.0（`.lst` 0x5291B8 的 dq 是 32767.0）。
    //   0.5 → 16383 (0x3FFF) vs 16384 (0x4000) ⇒ dst 逐字节差。
    //   ★ G7 的**字面量**（校验 guest 的 ST(0)）故意不跟着变：否则变异会把闸门打掉、只看到 Declined。
    file: 'apps/emulator-lift/src/runtime/native_pcm.cpp',
    from: 'constexpr double kPcmScale = 32767.0;',
    to: 'constexpr double kPcmScale = 32768.0;',
    guard: 'ctest:native-override-assets',
    ctest: 'native-override-assets',
    buildDir: 'apps/emulator-lift/out',
    what:
      'PCM 段的乘数从 32767.0 改成 32768.0 ⇒ 非满幅样本（±0.5 / 0.1）的 int16 逐字节差 ⇒ ' +
      'native-override-assets 的 `pcm/value-table` 当场红（满幅值表会被 clamp 掩盖 ⇒ 假绿）',
  },

  {
    // M2：clamp 判据改**无符号**（guest 是 `cmp eax,7FFFh/jle` + `cmp eax,0FFFF8000h/jge`，两条有符号）。
    //   `-0.5` ⇒ 低 32 位 `0xFFFFC001`（有符号 −16383，**不该** clamp）；无符号比较把它当 >0x7FFF
    //   ⇒ clamp 成 0x7FFF ⇒ dst 差。
    file: 'apps/emulator-lift/src/runtime/native_pcm.cpp',
    from: '        if (ax > 0x7FFF) {',
    to: '        if (static_cast<std::uint32_t>(ax) > 0x7FFFu) {',
    guard: 'ctest:native-override-assets',
    ctest: 'native-override-assets',
    buildDir: 'apps/emulator-lift/out',
    what:
      'PCM 段的 clamp 判据从有符号改成无符号 ⇒ 负数样本被当成 >0x7FFF 上界 clamp ⇒ ' +
      'native-override-assets 的 `pcm/value-table`（含 −0.5 / −131073）逐字节当场红',
  },

  {
    // M3：不写 `[ebp-108h]`（`0x48D877 mov dword ptr [ebp-108h],1`）。
    //   ★ 这个标志**不喂 PCM 字节**（只喂段外的 `wsprintfA("Clipping in frame %ld")`）⇒ 任何只看
    //     dst / WAV 的断言都抓不到；必须显式把 `[ebp-108h]` 放进被比较的状态（`pcm/clip-flag/*`）。
    file: 'apps/emulator-lift/src/runtime/native_pcm.cpp',
    from: '        if (clipped) mem.wr32(ebp - 0x108u, 1u);',
    to: '        if (clipped) { /* 变异：不写 [ebp-108h] */ }',
    guard: 'ctest:native-override-assets',
    ctest: 'native-override-assets',
    buildDir: 'apps/emulator-lift/out',
    what:
      'PCM 段不再写 `[ebp-108h]` 的 clip 标志 ⇒ 只喂调试打印的那个槽恒 0 ⇒ ' +
      'native-override-assets 的 `pcm/clip-flag/*`（有 clip ⇒ 期望 1）当场红（dst 字节完全不变）',
  },

  {
    file: 'apps/emulator-lift/src/shims/kernel32_bootstrap.cpp',
    from: '            wide.push_back(u);\n            i += static_cast<std::size_t>(n);',
    to: '            (void)u;\n            wide.push_back(static_cast<char16_t>(p[i]));\n            ++i;',
    guard: 'ctest:codepage',
    ctest: 'codepage',
    buildDir: 'apps/emulator-lift/out',
    what: 'MultiByteToWideChar 退回逐字节直通（0x82A0 → U+0082 U+00A0）⇒ codepage 的 932 定点断言红',
  },


  {
    file: 'apps/emulator-lift/src/runtime/cp932.cpp',
    from: 'if (b0 < 0x80 || (b0 >= 0xA1 && b0 <= 0xDF) || b0 == 0x80 || b0 == 0xA0 || (b0 >= 0xFD)) {',
    to: 'if (b0 < 0x80) {',
    guard: 'ctest:strfmt',
    ctest: 'strfmt',
    buildDir: 'apps/emulator-lift/out',
    what: '半角片假名 / 8 个单字节例外不再走单字节表 ⇒ strfmt 的 CP932 定点断言红（说明那张表真的被用）',
  },


  {
    file: 'apps/emulator-lift/src/runtime/memory.cpp',
    from: '    return addr >= base && (addr - base) < GuestMemory::kSpace;',
    to: '    return true;  // 变异：永远当 guest fault',
    guard: 'ctest:fault-report',
    ctest: 'fault-report',
    buildDir: 'apps/emulator-lift/out',
    what: '归因恒为 guest ⇒ 宿主 AV 又被报成上一条 guest 访存（`access :` 行回来了）⇒ fault-report 红',
  },

  {
    file: 'apps/emulator-lift/src/shims/kernel32_string.cpp',
    from: '    cpu.mem->writeBytes(buf, text.data(), text.size());',
    to: '    (void)text;  // 变异：A 版只回长度、不写字节',
    guard: 'ctest:locale-ext',
    ctest: 'locale-ext',
    buildDir: 'apps/emulator-lift/out',
    what: 'GetLocaleInfoA 不写字节 ⇒ A 的空串 != W 的 CP932 投影 ⇒ locale-ext 的 get-locale-info-a 红',
  },

  {
    file: 'apps/emulator-lift/src/shims/kernel32_file.cpp',
    from: '    st->readable = want_read;  // ⛔ 不再有 "|| (!want_write)"：access=0 不授予读',
    to: '    st->readable = want_read || (!want_write);  // 变异：access=0 又按可读处理',
    guard: 'ctest:file-io',
    ctest: 'file-io',
    buildDir: 'apps/emulator-lift/out',
    what: 'access=0 又按可读处理 ⇒ 只查属性的句柄能读 ⇒ file-io 的 access-zero-open 红',
  },

  {
    file: 'apps/emulator-lift/src/shims/kernel32_sync.cpp',
    from: '    if ((e->access & mask) != 0) return true;',
    to: '    if (true) return true;  // 变异：访问位检查失效',
    guard: 'ctest:sync-ext',
    ctest: 'sync-ext',
    buildDir: 'apps/emulator-lift/out',
    what: '同步对象访问位检查失效 ⇒ 零访问位句柄也能改状态 ⇒ sync-ext 的 duplicate-access-and-pseudo 红',
  },


  {
    file: 'apps/emulator-lift/src/shims/kernel32_sync.cpp',
    from: '    shimAbort("kernel32", api,\n              "需要等待：hHandle=0x%08X 未 signaled，dwMilliseconds=%u（0x%08X）⇒ 本模拟器不阻塞；"\n              "请人工检查该调用场景并设计等待方案（SCOPE §1 等待条目）",\n              h, timeout, timeout);',
    to: '    return kWaitTimeout;  // 变异：退回 WAIT_TIMEOUT',
    guard: 'ctest:shims-core',
    ctest: 'shims-core',
    buildDir: 'apps/emulator-lift/out',
    what: '未 signaled 的等待又静默返回 WAIT_TIMEOUT ⇒ shims-core 的 unsignaled-wait-traps 红（子进程没有 abort）',
  },


  {
    file: 'apps/emulator-lift/src/runtime/heap.cpp',
    from: '    const std::uint64_t need64 = static_cast<std::uint64_t>(bytes) + opt_.header;\n    if (need64 > 0xFFFFFFFFull) {',
    to: '    const std::uint32_t need64 = bytes + opt_.header;  // 变异：退回 32 位算\n    if (false) {',
    guard: 'ctest:memory-pages',
    ctest: 'memory-pages',
    buildDir: 'apps/emulator-lift/out',
    what: '堆请求的 bytes+header 退回 32 位 ⇒ 0xFFFFFFF8 请求切出 size=0 的块并返回非空指针 ⇒ memory-pages 的 heap-request-overflow 红',
  },


  // ── D3D9 生命周期：**回收资格**不能来自"最近 init 的那台 Machine"（`activeMachine()`）──

  {
    file: 'apps/emulator-lift/tests/base_coverage.cpp',
    from: '    o.until_ea = 0x004BA890u;',
    to: '    o.until_ea = 0x00401000u;  // 变异：提前停（没走到 L2）',
    guard: 'ctest:base-coverage',
    ctest: 'base-coverage',
    buildDir: 'apps/emulator-lift/out',
    what: 'A5 只判"动态缺口为空"在跑空时恒真 ⇒ 本变异把运行提前停在入口附近 ⇒ base-coverage 的 reached-L2/insns 哨兵红',
  },

  {
    file: 'apps/emulator-lift/src/runtime/decode.cpp',
    from: '    //   （machine.cpp 里 `left_block == false` 的续跑路径）。\n    return true;',
    to: '    //   （machine.cpp 里 `left_block == false` 的续跑路径）。\n    return false;  // 变异：长块分片当错误',
    guard: 'ctest:dispatch',
    ctest: 'dispatch',
    buildDir: 'apps/emulator-lift/out',
    what: '>256 条无控制流的直路又当成 DecodeError ⇒ dispatch 的 long-block-splits 红（语料里 0x415DF6 的 554 条块）',
  },


  {
    file: 'apps/emulator-lift/src/runtime/machine.cpp',
    from: '    shims_.setTrace(opts.trace_shims, opts.trace_out ? opts.trace_out : stdout);',
    to: '    shims_.setTrace(opts.trace_shims, stdout);  // 变异：忽略 trace_out',
    guard: 'ctest:boot-trace',
    ctest: 'boot-trace',
    buildDir: 'apps/emulator-lift/out',
    what: 'trace_out 被忽略 ⇒ A4 的两份 trace 文件都是空 ⇒ boot-trace 的 trace-bytes-identical 红（不允许"两边都空"假绿）',
  },

  {
    // ★ 2026-11-10 重定向（REQ-01M70KGDXSKDVK7MCJYFABXERH）：清理点**搬了家** ——
    //   `setGameDir` 不再清 `--game-dir/.lift-tmp`（那是对安装树的写，2026-11-05 明令禁止，
    //   现源码那行只剩注释），改由 `Vfs::setAppDataDir` 清 `<--appdata>/Local/Temp`。
    //   不变量是同一条（A4：Temp 目录不许跨运行累积 ⇒ `GetTempFileNameA(uUnique=0)` 序号必须从 0 起），
    //   守卫也还是 `file-path-ext` 的 `temp-dir-cleared`（该用例里两半都钉了：setGameDir **不许**删、
    //   setAppDataDir 必须删）。
    file: 'apps/emulator-lift/src/runtime/vfs.cpp',
    from: '    std::error_code ec;\n    fs::remove_all(pathFromUtf8(app_data_dir_) / "Local" / "Temp", ec);',
    to: '    std::error_code ec;\n    (void)ec;  // 变异：不清理 <--appdata>/Local/Temp',
    guard: 'ctest:file-path-ext',
    ctest: 'file-path-ext',
    buildDir: 'apps/emulator-lift/out',
    what: 'setAppDataDir 不清理 <--appdata>/Local/Temp ⇒ file-path-ext 的 temp-dir-cleared 红（GetTempFileNameA 序号会跨运行累积）',
  },

  {
    file: 'apps/emulator-lift/tests/decode_crosscheck.cpp',
    from: '            ++len_checked;\n            const std::uint32_t expect = e.next_ea - e.ea;',
    to: '            const std::uint32_t expect = e.next_ea - e.ea;  // 变异：不统计真正比过长度的行',
    guard: 'ctest:decode-crosscheck',
    ctest: 'decode-crosscheck',
    buildDir: 'apps/emulator-lift/out',
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
    what: '节 raw 声明区间越过文件末尾又静默接受（退回"只看要拷的字节"）⇒ pe-loader 的 raw-padding-beyond-eof 红（半载入仍返回 true）',
  },


  // ── emulator-lift：本轮新落的 5 组守卫（input-injection / guest-token-stress /
  //    gdi-font-metrics-cache / audio-device / gpu-batch-*）──
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
    what:
      'GetAsyncKeyState 恒返回 0（丢掉 keyDown 的最高位）⇒ input-injection 的 ' +
      '`testEaxImm(0x8000)` 那几条当场红（注入路径的"按键真的被看见"没有别的判据）',
  },

  // ── 交互 / 识别 / SSE 的三条新守卫（每条一处已知破坏，红了才说明守卫真的会红）──
  {
    file: 'apps/emulator-lift/src/runtime/interaction_graph.cpp',
    from: '            return sampler.changeCount() > changes_at_start;',
    // ⚠ `(void)changes_at_start;` 是必须的：变异后那个形参没人用了，而本工程开着
    //   `-Wunused-parameter -Werror` ⇒ 少了这一行就会"构建失败"，变异自检判"无法判定"（实测）。
    to:
      '            (void)changes_at_start;\n' +
      '            return sampler.last().changed;  // 变异：只看最后两帧',
    guard: 'ctest:interaction-graph',
    ctest: 'interaction-graph',
    buildDir: 'apps/emulator-lift/out',
    what:
      '`screen changed` 退回 `last().changed`（两次判定之间帧会推进好几帧 ⇒ 漏掉中间那次变化）' +
      '⇒ interaction-graph 的 `changed-counts-window` 当场红',
  },

  // ── 交互图的可选按钮词（图语法 `click <x> <y> [left|right]` / `click center [left|right]`）──

  {
    file: 'apps/emulator-lift/src/runtime/decode.cpp',
    from: '    if (sseMovsdByOperands(zi, zops, key)) {',
    to: '    if (false && sseMovsdByOperands(zi, zops, key)) {  // 变异：不按操作数形态分派',
    guard: 'ctest:conformance',
    ctest: 'conformance',
    buildDir: 'apps/emulator-lift/out',
    what:
      '`F2 0F 10`（SSE2 标量 movsd）退回 `Op::Movs` ⇒ 静默按串操作执行（改 [edi] / 推进 esi·edi）；' +
      'conformance 的 `sse-movsd-*` 三条红',
  },


  {
    file: 'apps/emulator-lift/tests/conformance.cpp',
    from: '    return r[lift::kEsp] == before.r[lift::kEsp] + 4u;',
    to: '    return r[lift::kEsp] == before.r[lift::kEsp];  // 变异：哨兵 ret 的栈效果没被排除',
    guard: 'ctest:conformance-rejects-hollow-case',
    ctest: 'conformance-rejects-hollow-case',
    buildDir: 'apps/emulator-lift/out',
    what:
      '恒等判据退回"含哨兵 ret"的旧口径 ⇒ 空心夹具不再被判红（负控 CTest `conformance-rejects-hollow-case` 红）',
  },

  {
    file: 'apps/emulator-lift/src/runtime/guest_token.cpp',
    // ★ 2026-11-10 重定向（REQ-01M6XVZG4T9WY8SQ1WS7P8RNW9 的 M1）：`acquire()` 的取锁点为了记
    //   "等令牌"（`waits()` / `waitNs()`）改成了 `try_lock` 探测 + `blockWaitNs`，原片段
    //   （`mutex_.lock()` 直接收尾）在现源码里 0 次。施加点仍是**同一处**（唯一取锁点的那段收尾），
    //   破坏形态仍是"不取锁、不登记持有者、不记并发"，守卫仍是 `guest-token-stress`。
    // ★ 第三次改形态（2026-11-10，REQ-01M70S2WR4S8MN2JDWJRJD6WFD）：**"acquire 整个空实现"这种
    //   破坏在 guest-token-stress 上会挂死**（实测：两条线程无锁并发改 `std::map` ⇒ 空链表成环 ⇒
    //   `lift-guest-token.exe` 跑满 20+ 分钟 CPU 不退出）。→ `runCtestGuard` 现在**有单测超时**
    //   （`REQ-01M71B4JXCGWR3MV1HNB6T8D05`：超时 ⇒ 收进程树 + 记「不可判定（超时）」+ 继续下一条，
    //   整轮 harness 不再被卡住），但超时**不是**「守卫红了」的证据，而且白等 120 s 仍不划算。
    //   ⇒ 施加点改成**同一不变量族里确定性、不碰锁**的一处：`check()` 不再累加
    //   `violations_`。它守的正是 `--assert-guest-token` 的**判据来源**（计数真的在数），
    //   而且红得快（毫秒级）：`guest-token/arena-outside` 的正控要求"令牌之外 find ⇒ violations==1
    //   + first-site 点名"，计数被短路后当场红。⛔ 不断言具体数值以外的任何东西。
    //   ★ "不取锁"这一形态仍被 `guest-token-stress` 的 `maxConcurrent()==1` 守着（去锁的实现
    //   会让它变 0/2），只是不再由**这条**变异来演示（代价是挂死，见上）。
    from:
      '    violations_.fetch_add(1, std::memory_order_relaxed);\n' +
      '    const char* expected = nullptr;\n' +
      '    first_site_.compare_exchange_strong(expected, site,',
    to:
      '    violations_.fetch_add(0, std::memory_order_relaxed);  // 变异：违规不再计数\n' +
      '    const char* expected = nullptr;\n' +
      '    first_site_.compare_exchange_strong(expected, site,',
    guard: 'ctest:guest-token-stress',
    ctest: 'guest-token-stress',
    buildDir: 'apps/emulator-lift/out',
    what:
      '`GuestToken::check` 不再累加 `violations_`（`--assert-guest-token` 的判据来源被短路）⇒ ' +
      'guest-token-stress 的 `guest-token/arena-outside` 正控红（令牌之外 find 之后期望 violations==1、' +
      '实际 0）⇒ 违规计数确实在数，而不是"恒 0 所以恒绿"（旧形态"acquire 空实现"会挂死，见上）',
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
    what:
      '`WindowManager::find` 入口不再调 `checkMainThread("window.find")` ⇒ worker 线程碰 wm 时 ' +
      '`offThread()` 不涨、`firstOffThreadSite()` 仍是 nullptr ⇒ window-manager-token 的 ' +
      '`window-manager-token/off-thread-find` 红（`>=1` 与站点名两条同时失配）',
  },

  {
    file: 'apps/emulator-lift/src/runtime/font.cpp',
    from: '    const auto cached = impl_->metrics_cache.find(key);',
    to:
      '    // 变异：忽略键 —— 恒返回表里第一条\n' +
      '    const auto cached = impl_->metrics_cache.empty() ? impl_->metrics_cache.end() : impl_->metrics_cache.begin();',
    guard: 'ctest:gdi-font-metrics-cache',
    ctest: 'gdi-font-metrics-cache',
    buildDir: 'apps/emulator-lift/out',
    what:
      '`metrics()` 的缓存忽略键（恒返回第一条）⇒ 小字号的度量复用给大字号 ⇒ ' +
      'gdi-font-metrics-cache 的 `cache/size-change` 红（键里少一维正是这条守卫守的东西）',
  },

  {
    file: 'apps/emulator-lift/src/runtime/audio.cpp',
    from: '        std::memset(out.data(), 0, out.size());',
    to: '        // 变异：不覆盖最终交付缓冲（默认静音失效，音频原样送出）',
    guard: 'ctest:audio-device',
    ctest: 'audio-device',
    buildDir: 'apps/emulator-lift/out',
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
    what: '裸标识符（如未映射的 `cld`）又默认判数据 ⇒ mnemonics 的 lst-classify 红（A2 的假绿口子回来了）',
  },

  // ── D3D9 `SetRenderTarget` 必须把 viewport 回写成**新目标的全幅** ─────────────────────────
  //   （REQ-01M7372FBT0WBXAS2Z1NESBRFG）
  //   判据：`apps/emulator-lift/tests/d3d9_pipeline.cpp` 的 `d3d9p/set-render-target-viewport` ——
  //   绑定纹理表面后 `GetViewport` 必须是纹理尺寸（32×16）、切回后缓冲后必须是后缓冲尺寸（64×64）。
  //   ★ 变异 = 让回写 helper **一开始就返回**（= 修前形状：SetRenderTarget 只写 colorSurface/Arena）。
  //     ⛔ 不写 `if (false)` / `if (true)`：本工程 `/W4 /WX`，编译期常量条件会变成"构建失败"
  //     （记成不可判定）而不是"守卫红了"；这里用 `cpu.mem != nullptr` —— 它是**成员指针、运行时恒真**，
  //     ⛔ 方向别写反成 `== nullptr`（那样 `return` 永远不执行，变异等于没施加 = 假绿）。
  //   ★ 只短路 helper（不回退调用点）：调用点仍在，证明的正是"回写真的落到了 kPlViewport"，
  //     不是"某处少调了一次"这种形状。
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
    what:
      '`releaseIf` 不再校验代际（按 EA 直接减引用）⇒ 陈旧持有者会把**复用同一 EA 的新对象**Release 掉 ⇒ ' +
      'mechanism 的 com-generation-stale-holder 红（新对象的 refcount / 对象表项被减）',
  },


  // ── emulator-lift：**块级指令数**必须按块累计、并与窗口 insns 闭合 ─────────────────────
  //   （`REQ-01M6XVZ8AXJNG492WA2QAMTA35` 的第 ③ 步）
  //   ★ 破坏 = dispatcher 把"这个块执行了几条"传成 0（**累加点还在、但传错了数**）⇒ 每行 `insns=0`、
  //     闭合差 = 整个窗口的 insns。这正是"按块累计没接上 / 接在了错误粒度"的样子，而**墙钟表照旧全绿**
  //     （self / incl / calls 一个都不变）—— 就是这个变异要证明"它会红"的东西。
  //   ★ 用运行时传入的 0 而不是 `if (false)`：`/W4 /WX` 下恒假条件会让变异体编译失败，harness 会把它
  //     记成"无法判定"而不是红（同一文件里另两条条目因此也用运行时值）。
  //   ★ 判据：`profile-window` 的 `profile-window-block-ea-edges` ④ 那一组 ——
  //     每行 `insns>0` / 打印行之和==汇总合计 / window==Σblock 三条一起红（实测见 round4.md）。
  {
    file: 'apps/emulator-lift/src/runtime/machine.cpp',
    from: '            window_prof_.addBlockInsns(b->ea, cpu_.insns - insns_before_block);',
    to:
      '            window_prof_.addBlockInsns(b->ea, 0ull);  ' +
      '// 变异：块级指令数不再累计（墙钟表照旧全绿）',
    guard: 'ctest:profile-window',
    ctest: 'profile-window',
    buildDir: 'apps/emulator-lift/out',
    what:
      'dispatcher 把每块的执行条数传成 0 ⇒ 块级 insns 全为 0、与窗口 insns 的闭合差 = 整个窗口' +
      '⇒ profile-window 的 `profile-window-block-ea-edges` 中"每行 insns>0 / 行和==合计 / window==Σblock"一起红',
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
    what:
      'Apply 不写回（修前形状）⇒ d3d9 的 stateblock-capture-apply 红：Apply 之后渲染状态 / ' +
      '纹理 / FVF / 变换 / 采样器 / 纹理级 / 视口 / 裁剪矩形仍停在"改后"的值（实测 24 条断言红）',
  },


  // ── emulator-lift · marker 事件源（`TriggerKind::Marker`）──

  {
    file: 'apps/emulator-lift/src/runtime/input_script.cpp',
    from: '    if (!s.valid) {\n        ++marker_invalid_;\n        return false;\n    }',
    to: '    if (!s.valid || s.name_degraded) {  // 变异：名字降级 ⇒ 不触发\n        ++marker_invalid_;\n        return false;\n    }',
    guard: 'ctest:input-marker',
    ctest: 'input-marker',
    buildDir: 'apps/emulator-lift/out',
    what:
      '名字降级改成"不触发"⇒ input-marker 的 `marker-name-base-zero-degrades` / ' +
      '`marker-id-out-of-range-degrades` 红（降级必须仍按 id 触发 + 一条警告）',
  },


  // ── emulator-lift · DDraw surface 销毁路径的**归属**判据 ──
  //   缺陷形态：判据拿进程级 `activeMachine()`（会被别的 Machine / 析构合法改写）⇒ 判据一失败，
  //   那块 arena 就**永久没有回收入口**（magic 已先被清，谁也再进不了 destroySurfaceState）。
  //   变异 = 把判据换回旧口径；`tests/ddraw.cpp` 的 `release-owner-not-active` 会红。
  //   ★ 不写 `if (false)` 那类短路：`/W4 /WX` 的 C4127 会把构建弄失败（会被记成"无法判定"）。
  {
    file: 'apps/emulator-lift/src/runtime/ddraw_surface.cpp',
    from: '    Machine* m = machineOf(cpu);\n',
    to:
      '    Machine* m = activeMachine();  // 变异：判据换回进程级 g_active（修复前的口径）\n' +
      '    if (m != nullptr && &m->cpu() != &cpu) m = nullptr;\n',
    guard: 'ctest:ddraw',
    ctest: 'ddraw',
    buildDir: 'apps/emulator-lift/out',
    what:
      'destroySurfaceState 的归属判据换回 activeMachine() ⇒ ddraw 的 release-owner-not-active 红' +
      '（另一台 Machine 抢过 g_active 时 arena 的 surfaceCount / bytesInUse 停在"建完之后"，' +
      '且默认级别打出 [warn] "arena … 无法回收" ⇒ 协议上等于永久泄漏）',
  },

  // ── 时间线 **0 点**（`--anchor-script <id>`）：采样门控必须问"marker 动作 **或** 0 点配置" ──────
  //   ★ 退回 `hasMarkerActions()`（= 本单修前的样子）⇒ "只有 `--anchor-script`、没有 marker 动作"的
  //     脚本**一次也不采样** ⇒ 0 点永不闩锁 ⇒ 收尾假报 `origin-missed:`
  //     （`REQ-01M6WS9G1EQRZ0EYM21V0DPZVY` 三条"最容易漏"之一）。
  //   ⛔ 不用 `if (false)`：那只是"门控整条消失"；退回 `hasMarkerActions()` 精确表达**修前的判据**，
  //     守卫红的方向（"只有 anchor 的脚本一次也没采样"）才与 `what` 对得上。
  //   判据：`tests/input_anchor.cpp` 的 `anchor/samples-without-marker-action`（`originSet()` /
  //   `published() >= 1` / `r.reason == Stopped` 全红）与 `anchor/passing-other-scripts-is-not-a-failure`。
  {
    file: 'apps/emulator-lift/src/runtime/machine.cpp',
    from: '            if (input_script_.needsIdentitySamples() &&',
    to: '            if (input_script_.hasMarkerActions() &&  // 变异：门控退回只认 marker 动作',
    guard: 'ctest:input-anchor',
    ctest: 'input-anchor',
    buildDir: 'apps/emulator-lift/out',
    what:
      '时间线 0 点的采样门控退回 `hasMarkerActions()`（`REQ-01M6WS9G1EQRZ0EYM21V0DPZVY` 的 L6）⇒ ' +
      '"只有 `--anchor-script`、没有 marker 动作"的脚本一次也不采样 ⇒ 0 点永不闩锁、收尾假报 ' +
      '`origin-missed:` ⇒ input-anchor 的 `anchor/samples-without-marker-action` 与 ' +
      '`anchor/passing-other-scripts-is-not-a-failure` 当场红',
  },

  // ── `--time-limit 0` = **不设墙钟上限**（`REQ-01M711JN018QDSWK4726Y1A0KV`）──────────────
  //   ★ 破坏 = 把"0 = 不限"的判据退回 `secs <= 0.0`（= 本单修前的样子）。后果不是崩溃，而是
  //     **usage 骗人**：help 说"0 = 不限，缺省"，代码却 exit 2 ⇒ 探索脚本里写死的 `--time-limit 0`
  //     换一份二进制就整轮跑不起来（实测 188 ms 就退，连运行摘要都没有）。
  //   ★ 判据在 **CLI 级** CTest（解析 → 运行 → 摘要 → 退出码整条链路）：`tests/cli_time_limit.cpp`
  //     的 `zero-equals-omit` —— `--time-limit 0` 必须与**省略该参数**同退出码、且 stop / insns /
  //     blocks 读数逐字相同；破坏后那一组当场红（`bad--1` / `bad-abc` 两条仍绿：它们守反方向）。
  //   ⛔ 不用 `if (false)`：那只是"判据整条消失"；`secs <= 0.0` 精确表达**修前那种错**。
  {
    file: 'apps/emulator-lift/src/main.cpp',
    from: "            if (end == text || *end != '\\0' || !std::isfinite(secs) || secs < 0.0) {",
    to: "            if (end == text || *end != '\\0' || secs <= 0.0) {  // 变异：0 又变成非法（help 重新骗人）",
    guard: 'ctest:cli-time-limit',
    ctest: 'cli-time-limit',
    buildDir: 'apps/emulator-lift/out',
    what:
      '`--time-limit` 的"0 = 不限"退回 `<= 0.0`（本单修前）⇒ cli-time-limit 的 zero-equals-omit 红：' +
      '`--time-limit 0` 又 exit 2 而 usage 仍声称"0 = 不限"',
  },
  // ── `--dump-frame` 的目录必须在开跑前建好（`REQ-01M73YBCC3DZ05Y9NSDTRKTMMY`）────────────────
  //   ★ 破坏 = 删掉 `create_directories`（保留 `(void)dump_dir;` 让编译通过）= 本单修前的样子：
  //     目录不存在时进程照跑，直到别的停止条件（真游戏实测 **546 s**；目录建好后同一命令 7 s）。
  //     纯档 CLI 用例没有 Present ⇒ 它抓的是"目录没建出来"（①）与"建不出来却 exit 0"（②）。
  //   ⛔ 不用 `if (false)`：`(void)dump_dir;` 精确表达"那道闸被摘掉"，且不引入未使用变量告警。
  {
    file: 'apps/emulator-lift/src/main.cpp',
    from: '            std::filesystem::create_directories(dump_dir, ec);',
    to: '            (void)dump_dir;  // 变异：不建目录（= 修前）',
    guard: 'ctest:cli-dump-frame',
    ctest: 'cli-dump-frame',
    buildDir: 'apps/emulator-lift/out',
    what:
      '`--dump-frame` 的目录不再预建（= 本单修前）⇒ cli-dump-frame 的 creates-missing-dir 红：' +
      '子进程 exit 0 但目录不存在（真游戏里它会空转到别的停止条件，实测 546 s）',
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
    what:
      '`fs:[0]` 链走到底时不再报 `StopReason::UnhandledException`，而是把那一层派发悄悄摘掉 ⇒ ' +
      '`seh` 的 B 组（两层 record 都返 0 必须非 ok 停机）红：理由变成 decode-error、' +
      '`unhandledCount()` 也停在 0（"异常没人接"重新变成静默继续）',
  },
  // ── 派发层固定税：**诊断开关被静默忽略** ─────────────────────────────────────────────
  //   ★ 缺陷形态：`LIFT_TOKEN_DIAG_SKIP_LOCK=1` 看起来"设了"、实际什么都不做 ⇒ A/B 两臂是同一份
  //     行为，而读数差被当成"锁的成本"（最糟的一类：结论建立在没生效的开关上）。
  //   ★ 判据两处，都不靠计时：摘要行必须是 `token-diag : skip-lock=1`，且**另一条线程能同时拿到
  //     令牌**（`maxConcurrent()==2`）—— 后者只有真的绕过 mutex 才可能成立。
  //   ⛔ 不用 `if (false)`：`return false` 精确表达"环境变量被忽略"，且不引入 C4127 风险。
  {
    file: 'apps/emulator-lift/src/runtime/guest_token.cpp',
    from: '    if (std::strcmp(v, "0") == 0) return false;\n    return true;',
    to:
      '    if (std::strcmp(v, "0") == 0) return false;\n' +
      '    return false;  // 变异：诊断开关永远关着（LIFT_TOKEN_DIAG_SKIP_LOCK 被静默忽略）',
    guard: 'ctest:dispatch-tax',
    ctest: 'dispatch-tax',
    buildDir: 'apps/emulator-lift/out',
    what:
      '诊断开关读点恒返回 false ⇒ 打开 `LIFT_TOKEN_DIAG_SKIP_LOCK` 什么也不发生：' +
      '`dispatch-tax/diag-on` 的 `token-diag : skip-lock=1` 与 `diag-really-skips` 的' +
      '`maxConcurrent()==2` 当场红（A/B 两臂其实是同一份行为）',
  },

  // ── 本工具自己（`tools/mutate-check.mjs`）：**超时后"等根进程真的消失"退回阻塞式 sleep** ──────────
  //   ★ 缺陷形态（`REQ-01M71YHT35RMW1XA6CCSNT4RNM`）：`sleepSync`（`Atomics.wait`）挡住事件循环 ⇒
  //     libuv 的 `waitpid` 跑不到 ⇒ 根进程停在 **zombie**，而 POSIX 的 `process.kill(pid, 0)` 对 zombie
  //     **仍然成功** ⇒ 这段等待等的是永远收不到的东西（实测：每条挂死变异白等满 5 s）。
  //   ★ 为什么必须有一条指向**本工具自己**：它坏掉时**所有**变异的结论都不可信，而"白等 5 s"与
  //     "等到了"在输出里长得一样（= 静默）。这里只回退**超时那条**（25 ms）。
  //   ★ 正常结束那条（10 ms）**不守**：复审实测把它单独回退成 `sleepSync(10)` 后 5/5 仍绿 ——
  //     主循环的 `await` 已经把尸收完，快路径**没有可观测效应**（不值得为它配断言/条目，
  //     ⛔ 也别为它加"看起来在守"的恒真断言）。
  //   ⚠ **自指条目**：`from` 在这份文件里出现**两次** —— 真代码（在本条目之前）+ 本条 `from` 字面量自己。
  //     `before.replace()` 取**第一处** ⇒ 施加在代码上（正确）。若哪天把代码行挪到清单之后，它就会改到
  //     清单字面量上 ⇒ 代码没变 ⇒ 守卫**不会红** ⇒ 记「❌ 守卫没红」并 exit 1（响亮，⛔ 不会假绿）。
  {
    file: 'tools/mutate-check.mjs',
    from: '      while (Date.now() < deadline && isProcessAlive(child.pid)) await sleep(25);',
    to: '      while (Date.now() < deadline && isProcessAlive(child.pid)) sleepSync(25);',
    guard: 'tools/test/mutate-timeout.test.mjs',
    what:
      '超时后"等根进程消失"退回阻塞式 `sleepSync` ⇒ POSIX 上等的是永远收不到的 zombie' +
      '（每条挂死变异白等 5 s；超时守卫 a) 的墙钟预算当场红）',
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
    what:
      'LOD 恒取 0（`mipLevelForLod` 的 `lod + 0.5` 变成 `lod*0.0 + 0.5`）⇒ 缩小时仍只采 level 0：' +
      'd3d9-mip 的 2:1 / 4:1 级号断言当场红（实测见 .tmp/reveq/REQ-01M73G2724479XAVS176WBZ65Q/report.md）',
  },


  {
    // ★ 代表 "x87 float→int64 的**截断方向**被改掉" 这一类错（REQ-01M75PW4MXS06MN6FKGXB36ETH，`__ftol2_sse`）。
    //   `__ftol2` 的修正环（`.lst` 0x4E8319..0x4E833B 的 `fstp dword` + `add 7FFFFFFFh` + `sbb/adc`）
    //   把 `fistp` 的"按 RC 就近取整"拉回**向零截断** ⇒ 实现里的 `x87ToI64(x, truncate=true)`
    //   是这一项语义的唯一落点；改成 `false`（就近）⇒ `±1.5` / `±2.7` 这类小数部分 ≥0.5 的输入
    //   整数值差 1。
    //   守卫：`apps/emulator-lift/tests/native_override_assets.cpp` 的 `ftol/*` 边界表
    //   （解释器臂 = 真字节 oracle，逐值比 `EDX:EAX`）。
    //   ★ 为什么挑这一条而不是"忘弹栈"：截断是**本项的核心语义**，而"忘弹栈"更多是框架 ABI 的
    //     共性错；另外这条变异**只有在边界表带非整数/非 2 的幂时才红**（`±0.4` 那种两边都给 0）
    //     ⇒ 它同时钉住"边界表必须有判别力"这件事。
    file: 'apps/emulator-lift/src/runtime/native_crt.cpp',
    from: '    const std::int64_t n = x87ToI64(x, /*truncate=*/true);',
    to: '    const std::int64_t n = x87ToI64(x, /*truncate=*/false);',
    guard: 'ctest:native-override-assets',
    ctest: 'native-override-assets',
    buildDir: 'apps/emulator-lift/out',
    what:
      '`__ftol2_sse` 的截断改成"按 RC 就近取整"（`x87ToI64(x, false)`）⇒ `±1.5`/`±2.7` 差 1：' +
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
    what:
      '`sub_4E1570` 失败支的 `adv(read)` 改成 `adv(codelengths[lo])`（两条分支的字节数写成同一个）' +
      '⇒ 位游标错位：native-override-assets 的 `huff/fail-len-gt-read` 当场红' +
      '（位读三 dword 的 endbit / ptr 与解释器臂不同）',
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
    what:
      '`removeBuffer` 不再有界等"正在跑的那一刀 fill"（erase/dropPending 之后直接返回）⇒ ' +
      '`audio-device/remove-waits-inflight` 的 `fill_waits == 1` / 耗时 ≥ 15 ms 当场红',
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
  const rebuild = ctx.rebuild ?? ((buildDir) => buildCtestTargets(null, buildDir));
  const verifyCtest =
    ctx.verifyCtest ?? ((name, buildDir, opts) => runCtestOnce(name, null, buildDir, opts));
  const journalPath = ctx.journalPath ?? null;
  const journalEntries = ctx.journalEntries ?? [];
  const buildDir = m.buildDir ?? CTEST_BUILD_DIR;
  const timeoutSec = ctx.ctestTimeoutSec ?? m.ctestTimeoutSec ?? null;
  const ctestOpts = { root, timeoutSec };
  const abs = path.join(root, m.file);
  const before = fs.readFileSync(abs, 'utf8');
  if (!before.includes(m.from)) {
    // ★ 这不是"守卫没抓住"，而是**清单过期**：`from` 已经不在源文件里 ⇒ 这次自检什么也没验到。
    return {
      result: { ...m, verdict: '⚠ 变异点没找到（源文件已变 ⇒ 清单过期）', ok: false, stale: true },
      restoreProblem: null,
    };
  }
  // ★ CTest 条目在**施加变异之前**先验基线：构建目录没配置 / 用例不存在 / 基线本来就红 / **超时** ⇒
  //   这次变异什么都证明不了 ⇒ 记"无法判定"，⛔ 不记成"守卫没红"、更不记成"守卫红了"。
  if (m.ctest) {
    const pre = await runCtest(m.ctest, null, buildDir, ctestOpts);
    if (pre.unavailable || pre.status !== 0) {
      const why = pre.unavailable ?? `\`${m.ctest}\` 基线退出码 ${pre.status}`;
      return {
        result: {
          ...m,
          verdict: `⏭ **无法判定（${pre.timedOut ? '超时' : '其它'}）**（CTest 基线不成立：${why}）`,
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
        ran: unavailable ? { skipReason: unavailable } : null,
        verdict: unavailable
          ? timedOut
            ? `⏭ **无法判定（超时）**（${unavailable}）`
            : `⏭ **无法判定（其它）**（${unavailable}）`
          : code !== 0
            ? `✅ 守卫红了（CTest 退出码 ${code}）`
            : '❌ **守卫没红**（CTest 退出码 0）',
        // ★ 与 node 侧一致：**跑不起来 ≠ 守卫没抓住**。`unavailable`（构建目录缺失 / cmake 不可用 /
        //   构建失败 / 超时）只是"这台机器上无法判定"，⛔ 不许记成 `failed` 并 exit 1 —— 否则在没有
        //   `apps/emulator-cxx/out` 的机器上，所有 CTest 条目都会被误报成"守卫坏了"。
        ok: !unavailable && code !== 0,
        unverifiable: Boolean(unavailable),
        unverifiableKind: unavailable ? (timedOut ? 'timeout' : 'other') : null,
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
        '  ★ 条目可带 `optInEnv`（如 `LIFT_ASSETS`）：那指向**配置期 opt-in** 的 CTest（注册落在\n' +
        '    `if(... AND DEFINED ENV{…})` 里）⇒ 没设该 env 时记「不可判定（本机未启用）」并打\n' +
        '    `⏭ 本机未启用`，⛔ 与「⛔ 守卫不存在」（注册被删，必须响）分开。要看它：设好 env ⇒\n' +
        '    重新 configure + 构建，再跑本工具。\n' +
        '  ⛔ **它会临时改写工作树里的文件** ⇒ 有别的进程正在编辑本仓时**不要**跑（还原会覆盖别人的改动）。\n' +
        '  --list 只列清单（不动任何变异文件；但每次启动都会先自愈上次中断留下的 journal）。\n' +
        '    ★ 它同时做**静态存在性检查**（`file` 在不在、`from` 还是不是源文件的子串；不构建、不施加变异）\n' +
        '      ⇒ 清单过期（源文件改名 / 搬家 / 文案改写）在这里就非 0 退出，而不是等全量跑到那一条。\n' +
        '  --match <子串> 只跑 file / what / guard 里含该子串的变异（例：--match shape）。\n\n' +
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
    // ★ 静态存在性检查（**只读**：不构建、不施加变异）：`file` 在不在、`from` 还是不是它的子串。
    //   原先这一步只在"全量跑到那一条"时才发生（`⚠ 变异点没找到`）⇒ 清单可以静默过期一整轮，
    //   而 `--list` 看起来一切正常。现在 `--list` 一次就把全部失配报出来（判据见 `staticManifestCheck`）。
    const stat = staticManifestCheck(selected, ROOT);
    for (const s of stat.skipped) console.log(`    ⏭ 静态检查跳过：${s.file}（${s.why}）`);
    if (stat.problems.length) {
      console.error(
        `\n⛔ ${stat.problems.length}/${selected.length} 条**静态失配**` +
          '（`from` 已不是源文件的子串 ⇒ 清单过期：全量跑到它只会记「变异点没找到」，什么也没验到）：',
      );
      // ★ 逐条：`⛔ <file>: from 片段已不存在` + 从 `what` 取的一句话（定位用）
      for (const p of stat.problems) console.error(`  ⛔ ${p.file}: ${p.why}\n      ${p.what}`);
    }
    if (gone.length || stat.problems.length) return 1;
    console.log(
      `\n静态检查通过：${selected.length} 条里验证了 ${stat.checked} 条` +
        `（file 存在 + from 是源文件子串）` +
        (stat.skipped.length ? ` · ${stat.skipped.length} 条不适用（多文件声明，已逐条报出）` : ' · 0 条不适用'),
    );
    return 0;
  }

  const results = [];
  // ★ 信号恢复要看的"当前已施加"清单 + journal 里的条目（两者同源，顺序一致）
  const journalEntries = [];
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
    return 1;
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
    return 1;
  }
  if (timedOutRows.length) {
    // ★ 超时 ⇒ **不可判定**，但也不能被 exit 0 掩盖：它意味着"这条守卫会红"这件事没被证明。
    //   ⛔ 不许把它当"通过"（假绿），也不许把它当"守卫没抓住"（冤枉守卫）—— 单列退出码 5。
    console.error(
      `✗ ${timedOutRows.length} 条 CTest 守卫**超时未结束**（不可判定(超时)，不是"红"）` +
        ' ⇒ 不能算通过；先查该守卫是否真会挂死，确属慢档再用 `--ctest-timeout <秒>` 调大。',
    );
    return 5;
  }
  if (unver.length && unver.length === results.length) {
    // ★ 全部条目都"无法判定（其它）"（构建目录不存在 / 资产缺失）≠ 通过：旧实现这里仍 exit 0，
    //   在没建 out/ 的机器上 `pnpm test:mutation` 会被读成"全部红了"。
    console.error(
      '✗ 全部 %d 条变异都无法判定（本机没有可跑的守卫）⇒ 不能算通过；先构建对应域 / 备齐资产再跑。',
      results.length,
    );
    return 4;
  }
  return stale.length ? 1 : 0;
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

