#!/usr/bin/env node
/**
 * tools/mutate-check.mjs —— **守卫自检**：改坏一个常量，确认对应守卫**当场红**
 *
 * ## 为什么需要它
 * 本仓的纪律是"守卫要**红得有意义**"。但"写了守卫"和"守卫真的会红"是两件事 ——
 * 一个恒真的断言、一个把 `expected` 抄成 `actual` 的断言，都能"一直绿"。
 * 本工具用**变异测试**把这件事变成可复跑的判据：对每个关键常量施加一处已知的破坏，
 * 要求**指定的守卫文件退出码非 0**。
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
 * 用法：`pnpm test:mutation`（也可 `node tools/mutate-check.mjs --list` 只看变异清单）
 */
import fs from 'node:fs';
import path from 'node:path';
import process from 'node:process';
import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');

// ★ CTest 类守卫：行为断言在 `apps/emulator-cxx` 的 CTest 里，`node --test` 跑不了它。
//   这类条目必须**先重建再跑**（判据在 exe 里）；还原源文件之后也必须重建成基线，
//   否则下一次 `ctest` 会假红（源文件已还原、二进制里还留着变异）。
const CTEST_BUILD_DIR = 'apps/emulator-cxx/out';
const ctestStdio = (logFd) => (logFd === null ? 'ignore' : ['ignore', logFd, logFd]);

/** 同步睡 `ms`（用 Atomics.wait 而不是 Promise：本工具的主流程是同步的 spawnSync 序列） */
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
 * 构建 + 跑**一条** CTest。
 * ★ `buildDir` 可按条目覆盖（`apps/emulator-lift/out` 与 `apps/emulator-cxx/out` 是两个独立工程）。
 * @returns {{status:number}|{unavailable:string}} `unavailable` = 本机跑不起来（没配置构建目录 / 没 cmake）。
 */
function runCtestGuard(name, logFd, buildDir = CTEST_BUILD_DIR) {
  // ★ 先看构建目录有没有**配置过**：没有 CMakeCache.txt 时 `cmake --build` 只会给一句笼统的
  //   "not a build directory"，那不是"守卫没红"，而是"这台机器上跑不了"（原因要写进输出）。
  if (!fs.existsSync(path.join(ROOT, buildDir, 'CMakeCache.txt'))) {
    return {
      unavailable: `构建目录未配置：${buildDir} 里没有 CMakeCache.txt（先 \`cmake --preset windows-x64\`）`,
    };
  }
  const build = buildCtestTargets(logFd, buildDir);
  if (build.error) return { unavailable: `cmake 不可用：${build.error.message}` };
  if (build.status !== 0) {
    return { unavailable: `构建失败（cmake --build ${buildDir}，退出码 ${build.status}）` };
  }
  const run = spawnSync(
    'ctest',
    ['--test-dir', buildDir, '-R', `^${name}$`, '--output-on-failure'],
    { cwd: ROOT, stdio: ctestStdio(logFd) },
  );
  if (run.error) return { unavailable: `ctest 不可用：${run.error.message}` };
  return { status: run.status };
}

/**
 * 这条变异的**守卫还在不在**（`null` = 在 / 判不了；字符串 = 不存在的原因）。
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
  return declared ? null : `CTest 未注册：${buildDir}/CTestTestfile.cmake 里没有 add_test([=[${name}]=])`;
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
    '**CTest 类条目**（`guard: \'ctest:<名字>\'`）会重建 + 跑那一条 CTest（还原后也重建成基线）；★ 看条目的 `buildDir` 决定构建哪个工程（缺省 `apps/emulator-cxx/out`，emulator-lift 条目显式写 `apps/emulator-lift/out`）',
    '**预写日志** `.tmp/mutate-check/journal.json`（gitignored；动文件前原子落盘，逐条还原后移除，全清时删除）—— 中途被 SIGKILL / 断电打断后，下次启动靠它自愈或拒绝运行',
  ],
  access: 'r（读原文进内存）→ **w journal（预写日志，原子：临时文件 + rename）**→ rw（写入变异）→ **w（还原，且比对全文）**；CTest 类条目额外重建该条目的 `buildDir`（缺省 `apps/emulator-cxx/out`；生成物，已 gitignore）；★ 启动时先自愈 journal（判不了就拒绝运行，退出码 3，journal 保留）；★ 不碰旧仓',
  tool: 'tools/mutate-check.mjs',
};

export const OPERATIONS = [
  { name: 'check', argv: [], mutates: true, summary: '★ 逐条施加变异 ⇒ 跑指定守卫（`node --test` 用例，或 `ctest:<名字>` 的 CTest）⇒ 要求退出码非 0，最后**逐条还原并比对全文**（会同步恢复 SIGINT/SIGTERM；SIGKILL 后下次启动靠 journal 自愈）' },
  { name: 'list', argv: ['--list'], mutates: false, summary: '只列变异清单（不动任何变异文件；但**启动时仍会先自愈 journal** —— 上次留下的变异态不该因为"只看清单"而继续存在）' },
];

export { MUTATIONS };

/**
 * 变异清单：每条 = 一处"已知的破坏" + 应该抓住它的守卫。
 * ★ 加新守卫时顺手加一条 —— 否则"这个守卫会红"只是个声称。
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

  // ── origin 的 machine-local 档（★ 抹掉这一档 ⇒ 本机现场产物又被判红，缺件机器回到"什么都写不了"）──
  {
    file: 'tools/lib/manifest.mjs',
    from: "export const isMachineLocal = (o) => o?.disposition === 'machine-local';",
    to: 'export const isMachineLocal = () => false;',
    guard: 'tools/test/corpus-manifest.test.mjs',
    what: 'machine-local 档整条失效（#3 缺件重新硬红、#4 又要求 pin、`--scan` 又给本机产物补 pin）⇒ 缺件机器上 `corpus validate` 长期红且写不进去',
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

  {
    file: 'tools/lib/manifest.mjs',
    from: '          } else if (NOT_CARRYING.has(e.storage)) {',
    to: '          } else if (false) {',
    guard: 'tools/test/corpus-manifest.test.mjs',
    what: '★ external-only / deferred 的外部来源缺件不再只 warn ⇒ 没有旧仓/安装树的机器上 corpus validate 永远红（把"本机不可校验"当成"清单坏了"）',
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

  {
    // ★ 位 / 格残差要消费足迹里的**写统计**（`writer` 记录）；不消费 ⇒ 写者执行过也仍是"判不了"。
    file: 'tools/lib/residual-footprint.mjs',
    from: 'export function classifyResidual(row, entry, footprint, { writers = footprint?.writers } = {}) {',
    to: 'export function classifyResidual(row, entry, footprint, { writers } = {}) {  // 变异：不消费足迹里的 writer 记录',
    guard: 'tools/test/spec-residual-footprint.test.mjs',
    what:
      '位 / 格残差不消费足迹里的 `writer` 记录（写者执行过也不进「路径内」）⇒ 写统计成了摆设 ⇒ 足迹守卫当场红',
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

  // ── 平台路径的缺省值必须来自清单（★ 缺键必须**响亮失败**，不许退回 `undefined`）──
  //   ★ 悄悄退回 `undefined` ⇒ 换机器时表现为"素材没到位"（错误指向素材），而不是"清单该覆盖"。
  {
    file: 'tools/lib/ui-bake/bake.mjs',
    from: '  if (!roots.gameInstall) throw new Error(',
    to: '  if (false) throw new Error(',
    guard: 'tools/test/manifest-local.test.mjs',
    what: '清单里缺 `roots.gameInstall` 时不再响亮失败 ⇒ 退回 `undefined`，换机器时表现为"素材没到位"而不是"清单该覆盖"',
  },

  // ── emulator-lift（★ 新命令类型：`buildDir` 指到它自己的 out/；判据在它自己的 CTest 里）──
  {
    file: 'apps/emulator-lift/src/runtime/exec_integer.cpp',
    from: '                case Op::Add: {\n                    const std::uint32_t r = a + b;',
    to: '                case Op::Add: {\n                    const std::uint32_t r = a + b + 1u;',
    guard: 'ctest:conformance',
    ctest: 'conformance',
    buildDir: 'apps/emulator-lift/out',
    what: '整数 add 变成 a+b+1 ⇒ conformance 的加法用例当场红（判据在 exe 里 ⇒ 必须先重建）',
  },

  {
    file: 'apps/emulator-lift/src/runtime/shims.cpp',
    from: 'call.stdcall_bytes = (def.conv == Conv::Stdcall) ? static_cast<std::uint32_t>(def.argc) * 4u : 0u;',
    to: 'call.stdcall_bytes = (def.conv == Conv::Stdcall) ? 0u : 0u;',
    guard: 'ctest:shim-abi',
    ctest: 'shim-abi',
    buildDir: 'apps/emulator-lift/out',
    what: 'stdcall 不再弹参 ⇒ 栈每次调用涨 argc*4 ⇒ shim-abi 的 esp 平衡断言红',
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
    file: 'apps/emulator-lift/src/shims/kernel32_bootstrap.cpp',
    from: '    if (used_default_ea != 0) cpu.mem->wr32(used_default_ea, used_default ? 1u : 0u);',
    to: '    if (used_default_ea != 0) cpu.mem->wr32(used_default_ea, used_default ? 0u : 1u);  // 变异：回报取反',
    guard: 'ctest:codepage',
    ctest: 'codepage',
    buildDir: 'apps/emulator-lift/out',
    what: 'WideCharToMultiByte 的 lpUsedDefaultChar 回报取反 ⇒ codepage 的 default-char 用例红',
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
    file: 'apps/emulator-lift/src/shims/kernel32_string.cpp',
    from: '} else if (sp.zero && numeric && sp.prec < 0) {',
    to: '} else if ((sp.zero && numeric && sp.prec < 0) && false) {',
    guard: 'ctest:strfmt',
    ctest: 'strfmt',
    buildDir: 'apps/emulator-lift/out',
    what: 'wsprintfA 的 0 填充失效 ⇒ %08X 变成空格填充 ⇒ strfmt 的宽度断言红',
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
    from: '        shimAbort("kernel32", api,\n                  "需要等待：bWaitAll=TRUE 且至少一个对象未 signaled，dwMilliseconds=%u（0x%08X）⇒ "\n                  "本模拟器不阻塞；请人工检查该调用场景并设计等待方案（SCOPE §1 等待条目）",\n                  timeout, timeout);',
    to: '        return kWaitTimeout;  // 变异：bWaitAll 的等待退回 TIMEOUT',
    guard: 'ctest:shims-core',
    ctest: 'shims-core',
    buildDir: 'apps/emulator-lift/out',
    what: 'bWaitAll=TRUE 的未 signaled 等待又静默返回 WAIT_TIMEOUT ⇒ shims-core 的多对象子进程没有 abort',
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
    file: 'apps/emulator-lift/src/shims/kernel32_sync.cpp',
    from: '    if (obj->count == 0 || obj->owner != cpu.shim.thread_id) {\n        cpu.shim.last_error = kErrNotOwner;\n        shimLog("kernel32", "ReleaseMutex",',
    to: '    if (false) {  // 变异：非持有者释放也放行\n        cpu.shim.last_error = kErrNotOwner;\n        shimLog("kernel32", "ReleaseMutex",',
    guard: 'ctest:shims-core',
    ctest: 'shims-core',
    buildDir: 'apps/emulator-lift/out',
    what:
      'ReleaseMutex 不再校验持有者 ⇒ 未持有的互斥量也返回 TRUE（不再 FALSE + ERROR_NOT_OWNER(288)）' +
      '⇒ shims-core 的 mutex-release-owner 红',
  },

  {
    file: 'apps/emulator-lift/src/runtime/vfs.cpp',
    from: '            e.create_time_100ns = times.create_100ns;\n            e.access_time_100ns = times.access_100ns;',
    to: '            e.create_time_100ns = times.write_100ns;  // 变异：退回 mtime 近似\n            e.access_time_100ns = times.write_100ns;',
    guard: 'ctest:file-io',
    ctest: 'file-io',
    buildDir: 'apps/emulator-lift/out',
    what: '三个时间又退回同一个 mtime ⇒ file-io 的 find-data-times 红（SetFileTime 设的两个不同过去值不再区分）',
  },

  {
    file: 'apps/emulator-lift/src/runtime/vfs.cpp',
    from: '    out->create_100ns = toWindows100ns(nsOf(st.st_birthtimespec));',
    to: '    out->create_100ns = toWindows100ns(nsOf(st.st_ctimespec));  // 变异：退回 ctime',
    guard: 'ctest:file-io',
    ctest: 'file-io',
    buildDir: 'apps/emulator-lift/out',
    what: 'macOS 的 creation 又退回 ctime ⇒ file-io 的 find-data-times 红（SetFileTime 设的 creation 读不回来）',
  },

  {
    file: 'apps/emulator-lift/src/shims/kernel32_file.cpp',
    from: '    if (seekAccessDenied(cpu, st, call.u32(0), api)) {\n        call.result = 0;\n        return;\n    }',
    to: '    if (false) {  // 变异：不校验 seek 的访问位\n        call.result = 0;\n        return;\n    }',
    guard: 'ctest:file-io',
    ctest: 'file-io',
    buildDir: 'apps/emulator-lift/out',
    what: 'SetFilePointerEx 不校验访问位 ⇒ access=0 的句柄也能 seek ⇒ file-io 的 access-zero-open 红',
  },

  {
    file: 'apps/emulator-lift/src/shims/kernel32_file.cpp',
    from: '        exists && (disposition == kCreateAlways || disposition == kOpenAlways);',
    to: '        false && (disposition == kCreateAlways || disposition == kOpenAlways);  // 变异：不置 183',
    guard: 'ctest:file-io',
    ctest: 'file-io',
    buildDir: 'apps/emulator-lift/out',
    what: 'CREATE_ALWAYS / OPEN_ALWAYS 命中已存在文件不置 last_error=183 ⇒ file-io 的 create-always-last-error 红',
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

  {
    file: 'apps/emulator-lift/src/runtime/memory.cpp',
    from: '    const std::uint64_t end = (end64 + kPageMask) & ~static_cast<std::uint64_t>(kPageMask);',
    to: '    const std::uint32_t end = static_cast<std::uint32_t>((end64 + kPageMask) & ~static_cast<std::uint64_t>(kPageMask));  // 变异：退回 32 位',
    guard: 'ctest:memory-pages',
    ctest: 'memory-pages',
    buildDir: 'apps/emulator-lift/out',
    what: 'commit 的排他上界退回 uint32 ⇒ 最后一页（end=4 GiB）截断成 0 ⇒ memory-pages 的 commit-last-page 红',
  },

  // ── D3D9 生命周期：**回收资格**不能来自"最近 init 的那台 Machine"（`activeMachine()`）──
  //   ★ 退回 `activeMachine()` ⇒ 另一台 Machine 抢走 g_active（或 `~Machine` 清空）时，
  //     纹理 / 后缓冲 / 深度的 arena 像素块永久失去回收入口。
  //   判据：`tests/d3d9.cpp` 的 `d3d9/arena-reclaim-under-other-machine`（修前 4 条红）。
  {
    file: 'apps/emulator-lift/src/runtime/machine.cpp',
    from: '    if (cpu.machine != nullptr) return cpu.machine;',
    to: '    if (false) return cpu.machine;  // 变异：退回 activeMachine() 判据',
    guard: 'ctest:d3d9',
    ctest: 'd3d9',
    buildDir: 'apps/emulator-lift/out',
    what:
      '回收资格退回 activeMachine() ⇒ 另一台 Machine 抢走 g_active 后纹理 / 后缓冲 / 深度的 arena ' +
      '像素块永不回收 ⇒ d3d9 的 arena-reclaim-under-other-machine 红',
  },

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
    file: 'apps/emulator-lift/src/shims/user32_menu.cpp',
    from: '    shimLog("user32", "SetMenu", "菜单整体 no-op ⇒ 返回 TRUE（SCOPE §1；没有 Win32 菜单栏）");\n    call.result = 1;',
    to: '    shimLog("user32", "SetMenu", "菜单整体 no-op ⇒ 返回 TRUE（SCOPE §1；没有 Win32 菜单栏）");\n    call.result = 0;  // 变异：菜单 no-op 返回值错',
    guard: 'ctest:shim-abi',
    ctest: 'shim-abi',
    buildDir: 'apps/emulator-lift/out',
    what: 'SetMenu 的 no-op 返回 FALSE ⇒ shim-abi 的 menu-modal-noop 红（SCOPE §1 要求 TRUE）',
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
    file: 'apps/emulator-lift/src/runtime/vfs.cpp',
    from: '    std::error_code tmp_ec;\n    fs::remove_all(p / ".lift-tmp", tmp_ec);',
    to: '    std::error_code tmp_ec;\n    (void)tmp_ec;  // 变异：不清理 .lift-tmp',
    guard: 'ctest:file-path-ext',
    ctest: 'file-path-ext',
    buildDir: 'apps/emulator-lift/out',
    what: 'setGameDir 不清理 .lift-tmp ⇒ file-path-ext 的 temp-dir-cleared 红（GetTempFileNameA 序号会跨运行累积）',
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
    file: 'apps/emulator-lift/src/runtime/x86.cpp',
    from: '    noteGuestWrite(cache, a, m.size);',
    to: '    (void)0;  // 变异：不通知块缓存（SMC 失效失效）',
    guard: 'ctest:dispatch',
    ctest: 'dispatch',
    buildDir: 'apps/emulator-lift/out',
    what: 'guest 写代码页不失效块缓存 ⇒ dispatch 的 smc-invalidates-block 红（第二次调用读到旧 immediate）',
  },

  {
    file: 'apps/emulator-lift/src/runtime/image.cpp',
    from: '        if (copy && raw_end > img.bytes.size()) {',
    to: '        if (false) {  // 变异：节 raw 越界静默跳过',
    guard: 'ctest:pe-loader',
    ctest: 'pe-loader',
    buildDir: 'apps/emulator-lift/out',
    what: '节 raw 范围越界又静默不拷贝 ⇒ pe-loader 的 section-raw-out-of-range 红（半载入仍返回 true）',
  },

  {
    file: 'apps/emulator-lift/src/runtime/exec.cpp',
    from: '    return ((0x6996u >> v) & 1u) == 0;',
    to: '    return ((0x6996u >> v) & 1u) != 0;  // 变异：PF 又反了',
    guard: 'ctest:conformance',
    ctest: 'conformance',
    buildDir: 'apps/emulator-lift/out',
    what: 'PF 的偶校验查表又取反 ⇒ conformance 的 test-pf-even / or-pf-odd 红（PF 原先一条整数断言都没有）',
  },

  {
    file: 'apps/emulator-lift/src/runtime/exec_string.cpp',
    from: '        if (cpu.df()) {\n            reg -= step;',
    to: '        if (false) {  // 变异：忽略 DF\n            reg -= step;',
    guard: 'ctest:conformance',
    ctest: 'conformance',
    buildDir: 'apps/emulator-lift/out',
    what: '串操作忽略 DF ⇒ conformance 的 rep-movsd-df 红（DF=1 分支以前从未执行过）',
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
    file: 'apps/emulator-lift/src/runtime/exec_fpu.cpp',
    from: '    if ((cpu.eaOf(op.mem) & 15u) == 0u) return true;',
    to: '    if (true) return true;  // 变异：对齐检查恒真（未对齐静默按 movups 搬）',
    guard: 'ctest:dispatch',
    ctest: 'dispatch',
    buildDir: 'apps/emulator-lift/out',
    what: '未对齐 movaps / movdqa 不再响亮失败 ⇒ dispatch 的 `unaligned-*` 两条红（内存被静默改写）',
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
    from:
      '    mutex_.lock();\n' +
      '    owner_.store(me, std::memory_order_relaxed);\n' +
      '    depth_ = 1;\n' +
      '    noteTake();\n}',
    to:
      '    // 变异：`acquire` 空实现 —— 不取锁、不登记持有者、不记并发\n' +
      '    (void)me;\n}',
    guard: 'ctest:guest-token-stress',
    ctest: 'guest-token-stress',
    buildDir: 'apps/emulator-lift/out',
    what:
      '`GuestToken::acquire` 空实现（不取锁）⇒ 两线程并发改 `GuestHeap::blocks_`：' +
      'guest-token-stress 的 `maxConcurrent()==1`（令牌把执行流串起来的唯一判据）与 `violations()==0` 当场红',
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

  // ── D3D9 渲染目标的生命周期：**绑定期间像素块不许被回收** ──
  //   ★ 只 addRef surface 对象（本仓修前的样子）⇒ 纹理在"仍被绑定为 RT"时被 guest Release 会走
  //     `destroyTextureState` 回收像素块，流水线继续往已释放 / 已被别人复用的 arena 写。
  //   判据：`tests/d3d9_pipeline.cpp` 的 `d3d9p/render-target-lifetime`（本变异实测 10 条红：
  //   ① 像素块还活着、② 新占用者的 4 个 texel 不被改写、④ 换 target 时成对回收、⑥ 同 owner 互切）。
  //   ★ 变异**只短路"对 owner 的 addRef"**（`if (false && ...)`）：owner 仍然找得到、失效检查
  //     也仍然通过 ⇒ 退化成的正是"绑定只保 surface 对象"那种形状，而不是顺手命中"没有 owner"的
  //     防御分支（后者证明的是另一个不变量）。
  {
    file: 'apps/emulator-lift/src/runtime/d3d9_pipeline.cpp',
    from: '        if (owner != 0 && !cpu.com->addRef(cpu, owner)) {',
    to: '        if (false && !cpu.com->addRef(cpu, owner)) {  // 变异：RT 绑定不再保像素块',
    guard: 'ctest:d3d9-pipeline',
    ctest: 'd3d9-pipeline',
    buildDir: 'apps/emulator-lift/out',
    what:
      'RT 绑定不再对像素块 owner 做语义 addRef ⇒ 绑定期间 Release 纹理就把 arena 回收了 ⇒ ' +
      'd3d9-pipeline 的 render-target-lifetime 红（`find(a_arena)` / 新占用者像素 / 成对回收）',
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

  {
    file: 'apps/emulator-lift/src/runtime/d3d9_pipeline.cpp',
    from: '            if (owner_generation == 0 || owner_now != owner_generation) {',
    to: '            if (false) {  // 变异：短路 owner 代际校验',
    guard: 'ctest:d3d9-pipeline',
    ctest: 'd3d9-pipeline',
    buildDir: 'apps/emulator-lift/out',
    what:
      'SetRenderTarget 不再校验 owner 的代际 ⇒ 与"复用同一对象 EA + 同一 `\'TEX1\'` 状态块 EA"的新纹理' +
      '静默绑上、对它 addRef 并 markTextureContentUnknown ⇒ d3d9-pipeline 的' +
      ' stale-owner-generation-known-deviation 红（① 那条断言本身就标了**已知偏离**：真机应当绑成功）',
  },

  // ── D3DX 资源耗尽 **≠** unimplemented（误诊回归）──────────────────────────────────────
  //   ★ `from` 必须带上下文：单行 `        if (out_of_memory) {` 在该文件出现 **3** 次
  //     （纹理 / 纹理由内存 / sprite），`replace` 只换第一处 ⇒ 单行片段会把变异施加到**别**的分支上。
  //   ★ 退回 `d3dxTrap` ⇒ `stop_why` 变成 "unimplemented D3DX9 call" 且 `unimplemented_calls` ++
  //     ⇒ `d3dx9` 的 `create-sprite-oom` 只在 ②③ 红（①④ 仍绿：出参在分配前就清了 0、状态块在
  //     `createSpriteState` 内部就还掉了）—— 红点精确指向误诊，不是靠"调用失败"这种恒真条件。
  //   ⚠ 本条目用 `d3dxTrap` 形态，而不是短路成 `if (false)`：要演的**正是**"退回 unimplemented 口径"；
  //     顺带避开常量条件（`/W4 /WX` 下 C4127 的风险未在本机 Windows 验证 ⇒ 断言成"一定会构建失败"是不诚实的）。
  {
    file: 'apps/emulator-lift/src/shims/d3dx9.cpp',
    from:
      '    const std::uint32_t state = d3dx9::createSpriteState(cpu, device, &why, &out_of_memory);\n' +
      '    if (state == 0) {\n' +
      '        if (out_of_memory) {\n' +
      '            // ★ 同一口径：guest 堆不够 ≠ "D3DXCreateSprite 没实现"（⛔ 不记缺口、不写 unimplemented）。\n' +
      '            d3dxResourceStop(cpu, "D3DXCreateSprite", "%s", why.c_str());',
    to:
      '    const std::uint32_t state = d3dx9::createSpriteState(cpu, device, &why, &out_of_memory);\n' +
      '    if (state == 0) {\n' +
      '        if (out_of_memory) {\n' +
      '            // 变异：资源耗尽退回 unimplemented 口径（误诊回归）\n' +
      '            d3dxTrap(cpu, "D3DXCreateSprite", "%s", why.c_str());',
    guard: 'ctest:d3dx9',
    ctest: 'd3dx9',
    buildDir: 'apps/emulator-lift/out',
    what:
      'D3DXCreateSprite 的资源耗尽分支退回 d3dxTrap ⇒ stop_why 变成 "unimplemented D3DX9 call" 且 ' +
      'unimplemented_calls ++ ⇒ d3dx9 的 create-sprite-oom 在 ②③ 红（①④ 仍绿：出参早已清 0、' +
      '状态块在 createSpriteState 内部就还掉了），正好证明判据不是靠"调用失败"这种恒真条件',
  },

  // ── 汇入点的**新停机原因分类**：资源耗尽不许再被归成 `Stopped`(ok) ─────────────────────
  //   ★ 变异把 `ResourceExhausted` 分支短路掉（用 `&& cpu_.running`：进这段时 running 必为 false，
  //     所以等价于 `if (false)`，但**不是**编译期常量 —— 避开 `/W4 /WX` 的 C4127 风险）。
  //   ★ 走 Trap 分支也不命中（stop_kind 是 ResourceExhausted）⇒ 退化成旧行为 `Stopped`(ok)。
  //   判据：`create-sprite-oom` 的 ②（reason / !ok / 名字 / exitCodeForRun）当场红；
  //   这条同时守住"新原因真的会从汇入点流出来"，而不只是"枚举里多了一个值"。
  {
    file: 'apps/emulator-lift/src/runtime/machine.cpp',
    from: '                    if (cpu_.stop_kind == HostStopKind::ResourceExhausted) {',
    to:
      '                    if (cpu_.stop_kind == HostStopKind::ResourceExhausted && cpu_.running) {  ' +
      '// 变异：短路资源耗尽分支',
    guard: 'ctest:d3dx9',
    ctest: 'd3dx9',
    buildDir: 'apps/emulator-lift/out',
    what:
      '汇入点跳过 ResourceExhausted ⇒ 资源耗尽退化成 Stopped(ok) ⇒ d3dx9 的 create-sprite-oom ' +
      '在 rr.reason / !rr.ok() / stopReasonName / exitCodeForRun 四处红（旧行为回归）',
  },

  // ── emulator-lift：**仪器化不许改执行流**（口径不变量；measurement.md M-02）───────────────
  //   ★ 这是**敏感性变异**（不是"可能发生的 bug"）：它证明 `profile-instrumentation-invariant`
  //     的断言有牙 —— 块计时顺手改掉 `cpu_.insns`，等于"仪器把被测量的指令数也改了"。
  //   ★ 该行被 `opts.profile` 短路 ⇒ **B 臂（仅 --profile）就命中**，不必依赖窗口 profiler armed；
  //     A（全关）不命中 ⇒ A≠B ⇒ 四臂全等的断言当场红。
  //   ★ `from` 逐字（12 空格缩进 + 整行）在同一文件里只出现 1 次；⛔ 别缩成
  //     `if (opts.profile || window_prof_.armed())`（那样以后多一处复制就会静默改错地方）。
  {
    file: 'apps/emulator-lift/src/runtime/machine.cpp',
    from: '            if (opts.profile || window_prof_.armed()) b->ns += block_ns;',
    to:
      '            if (opts.profile || window_prof_.armed()) {\n' +
      '                b->ns += block_ns;\n' +
      '                --cpu_.insns;  // 变异：让"仪器"顺带改掉被测量的指令数（敏感性变异）\n' +
      '            }',
    guard: 'ctest:profile-window',
    ctest: 'profile-window',
    buildDir: 'apps/emulator-lift/out',
    what:
      '块计时顺手改 cpu_.insns ⇒ profile-window 的 profile-instrumentation-invariant 红' +
      '（同一程序开/关 profiler 的 insns 必须全等；仪器只许花时间、不许改执行流）',
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

  // ── emulator-lift：后缓冲 surface 的 LockRect / UnlockRect（REQ-01M6ZDSGQY1RXX3N3WJKA8D26H）──
  //   ★ 每一条的 `from` 都带**只属于后缓冲路**的上下文（纹理 lockRect 里有同名行 ⇒ 单行会改错地方）。
  //   ★ 判据在 `tests/d3d9.cpp` 的 `backbuffer-lockrect` 与 `tests/gpu_batch.cpp` 的
  //     `backbuffer-lock-barrier`；后者注册在 pure 的 `gpu-batch-order`（顶层 add_test）。
  {
    file: 'apps/emulator-lift/src/runtime/d3d9.cpp',
    from:
      '    const std::uint32_t bits = gs->ea + static_cast<std::uint32_t>(top) * gs->pitch +\n' +
      '                               static_cast<std::uint32_t>(left) * bpp;',
    to: '    const std::uint32_t bits = gs->ea;  // 变异：pBits 不偏到矩形左上',
    guard: 'ctest:d3d9',
    ctest: 'd3d9',
    buildDir: 'apps/emulator-lift/out',
    what:
      'backbuffer LockRect 的 pBits 去掉矩形偏移（写成像素首字节）⇒ 真机的逐行 memcpy 会把整幅 ' +
      '备份图写错位置；d3d9 的 backbuffer-lockrect 在 pBits 与像素穿透两处红',
  },

  {
    file: 'apps/emulator-lift/src/runtime/d3d9.cpp',
    from:
      '    const std::uint32_t bits = gs->ea + static_cast<std::uint32_t>(top) * gs->pitch +\n' +
      '                               static_cast<std::uint32_t>(left) * bpp;\n' +
      '    cpu.mem->wr32(locked_rect_ea + 0, gs->pitch);',
    to:
      '    const std::uint32_t bits = gs->ea + static_cast<std::uint32_t>(top) * gs->pitch +\n' +
      '                               static_cast<std::uint32_t>(left) * bpp;\n' +
      '    cpu.mem->wr32(locked_rect_ea + 0, static_cast<std::uint32_t>((right - left) * bpp));  ' +
      '// 变异：Pitch 用子矩形宽',
    guard: 'ctest:d3d9',
    ctest: 'd3d9',
    buildDir: 'apps/emulator-lift/out',
    what:
      'backbuffer LockRect 的 Pitch 写成子矩形宽（真机 `0x4A5018 add edi,[ebp+var_44]` 依赖整幅行距）' +
      '⇒ d3d9 的 backbuffer-lockrect 的 Pitch 与 pBits 断言红',
  },

  {
    file: 'apps/emulator-lift/src/runtime/d3d9.cpp',
    from:
      '    if (gs->d3d9_locked) {\n' +
      '        if (why) *why = "D3D9：LockRect 在已锁的后缓冲上（D3DERR_INVALIDCALL）";\n' +
      '        return false;\n' +
      '    }',
    to: '    // 变异：删掉"已锁再锁"检查',
    guard: 'ctest:d3d9',
    ctest: 'd3d9',
    buildDir: 'apps/emulator-lift/out',
    what:
      'backbuffer LockRect 不再拒绝"已锁再锁"⇒ d3d9 的 backbuffer-lockrect ③ 红（真机会答 ' +
      'D3DERR_INVALIDCALL；静默允许会让 guest 在解锁时丢掉第一次锁的矩形语义）',
  },

  {
    file: 'apps/emulator-lift/src/runtime/d3d9.cpp',
    from:
      '    if (GpuRaster* gpu = m->gpuRaster(); gpu != nullptr && gpu->rasterActive() && gpu->rasterDirty()) {\n' +
      '        std::uint8_t* base = m->surfaces().pixels(*gs);\n' +
      '        if (base == nullptr) {\n' +
      '            if (broken) *broken = true;\n' +
      '            if (why) *why = "D3D9：后缓冲像素指针为空（状态崩坏）";\n' +
      '            return false;\n' +
      '        }\n' +
      '        std::string gerr;\n' +
      '        if (gpu->rasterTargetBase() != nullptr && gpu->rasterTargetBase() == base) {\n' +
      '            if (!gpu->rasterSyncToGuest(base, gs->pitch, gs->width, gs->height, &gerr)) {\n' +
      '                if (broken) *broken = true;\n' +
      '                if (why) *why = "D3D9：LockRect 前 GPU 像素读回失败：" + gerr;\n' +
      '                return false;\n' +
      '            }\n' +
      '        } else if (!gpu->rasterFlush(&gerr)) {\n' +
      '            // 队列里的绘制不写这块像素 ⇒ flush 失败不影响本次锁的像素；但要出声（⛔ 不静默）。\n' +
      '            d3d9Warn("LockRect(backbuffer)", "GPU 队列 flush 失败：%s（锁的像素不是当前目标，继续）",\n' +
      '                     gerr.c_str());\n' +
      '        }\n' +
      '    }',
    to: '    // 变异：删掉锁前的 GPU 屏障（guest 可能拿到过期像素 / 别的 target 的镜像）',
    guard: 'ctest:gpu-batch-order',
    ctest: 'gpu-batch-order',
    buildDir: 'apps/emulator-lift/out',
    what:
      'backbuffer LockRect 删掉锁前 GPU 屏障 ⇒ gpu-batch-order 的 backbuffer-lock-barrier 红' +
      '（syncToGuest 不再发生；⚠ 只注册 `--gpu` 那条会在无 GPU 机器上 SKIP 77 逃逸）',
  },

  {
    file: 'apps/emulator-lift/src/runtime/d3d9.cpp',
    from:
      '    if (!gs->d3d9_lockable) {\n' +
      '        if (why) *why = "D3D9：后缓冲没有 D3DPRESENTFLAG_LOCKABLE_BACKBUFFER ⇒ 不能锁";\n' +
      '        return false;\n' +
      '    }',
    to: '    // 变异：删掉 LOCKABLE_BACKBUFFER 检查',
    guard: 'ctest:d3d9',
    ctest: 'd3d9',
    buildDir: 'apps/emulator-lift/out',
    what:
      'backbuffer LockRect 不再检查 D3DPRESENTFLAG_LOCKABLE_BACKBUFFER ⇒ d3d9 的 ' +
      'backbuffer-lockrect 的 no-lockable 变体红（真机 `0x498F98` 明确设了 1）',
  },

  // ── emulator-lift · marker 事件源（`TriggerKind::Marker`）──
  //   ★ 三条判据各一条：边沿（值变才发）/ 降级不静默（读不到名字仍按 id 触发）/ 超时响亮失败。
  {
    file: 'apps/emulator-lift/src/runtime/input_script.cpp',
    from: '    if (s.cur == marker_cur_ && s.id == marker_id_) return false;  // 同一 (cur,id) 重复出现 ⇒ 0 事件',
    to: '    if (false && s.cur == marker_cur_ && s.id == marker_id_) return false;  // 变异：恒触发',
    guard: 'ctest:input-marker',
    ctest: 'input-marker',
    buildDir: 'apps/emulator-lift/out',
    what: '边沿判据去掉（同一 (cur,id) 也发事件）⇒ input-marker 的 `marker-no-change-zero-events` 红',
  },

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

  {
    file: 'apps/emulator-lift/src/runtime/input_script.cpp',
    from: '    if (marker_timeout_ms_ != 0 && waited >= marker_timeout_ms_) {',
    to: '    if (false && marker_timeout_ms_ != 0 && waited >= marker_timeout_ms_) {  // 变异：超时永不触发',
    guard: 'ctest:input-marker',
    ctest: 'input-marker',
    buildDir: 'apps/emulator-lift/out',
    what: '超时分支短路 ⇒ input-marker 的 `marker-timeout-loud-failure` 红（等不到名字却继续跑 = 静默）',
  },
];

/**
 * 入口（派发器 `tools/cli.mjs` 与直接运行都走这里）。
 * @returns {number} 退出码：0 = 全部按预期变红；1 = 有守卫没抓住破坏 / 清单过期；
 *   2 = 还原失败（已中止）；3 = 启动自愈拒绝运行（journal 非法 / 工作树内容与 journal 不符）；
 *   4 = **全部条目都无法判定**（本机没有可跑的守卫 ⇒ 不能算通过）
 */
export function main(argv = process.argv.slice(2)) {
  // ★ `--match <子串>`：只跑匹配的变异（匹配 `file` / `what` / `guard`）。
  //   ★ 为什么必须有：本工具**会临时改写工作树里的文件** ⇒ 只想验"我这一域新加的守卫会不会红"时，
  //     不该被迫把全部变异跑一遍（窗口越长，越可能撞上别的进程在编辑）。技能文档里写的
  //     `pnpm tools mutate check --match shape` 原先**只是个不存在的 flag**（被静默忽略 ⇒ 跑全套）。
  const mi = argv.indexOf('--match');
  const match = mi >= 0 ? (argv[mi + 1] ?? null) : null;
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
      '用法：node tools/mutate-check.mjs [--list] [--match <子串>] [--tier all|active|frozen|lift|js]\n\n' +
        '  不带参数 = 逐条施加变异 ⇒ 跑对应守卫 ⇒ 要求退出码非 0（最后逐条还原并比对全文）。\n' +
        '  --tier active 跳过冻结件（emulator-new / 已废弃 emulator）；lift 只跑 emulator-lift；js 跳过 CTest。\n' +
        '  ⛔ **它会临时改写工作树里的文件** ⇒ 有别的进程正在编辑本仓时**不要**跑（还原会覆盖别人的改动）。\n' +
        '  --list 只列清单（不动任何变异文件；但每次启动都会先自愈上次中断留下的 journal）。\n' +
        '  --match <子串> 只跑 file / what / guard 里含该子串的变异（例：--match shape）。\n\n' +
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
  if (argv.includes('--list')) {
    const gone = [];
    for (const m of selected) {
      const missing = guardMissing(m);
      // ★ `⛔ 守卫不存在` 是**统一的可 grep 标记**：`--list` 与 check 两条路都必须打它。
      //   原先这里只打 `⛔ ${missing}`（例如"⛔ 守卫文件不存在：…"）⇒ 同一件事有两种字面，
      //   守卫（`tools/test/mutate-journal.test.mjs` 的新用例）只能各配一个断言。
      console.log(`${m.what}\n    ${m.file}  ⇒ 守卫 ${m.guard}${missing ? `\n    ⛔ 守卫不存在 —— ${missing}` : ''}`);
      if (missing) gone.push({ m, missing });
    }
    if (gone.length) {
      console.error(
        `\n⛔ ${gone.length}/${selected.length} 条**指向不存在的守卫**（跑起来会让"守卫红了"变成假绿）：\n` +
          gone.map(({ m, missing }) => `  - ${m.file}（${missing}）`).join('\n'),
      );
      return 1;
    }
    return 0;
  }
  if (match && selected.length === 0) {
    console.error(`没有匹配「${match}」的变异（--list 看清单）`);
    return 1;
  }

  const results = [];
  // ★ 信号恢复要看的"当前已施加"清单 + journal 里的条目（两者同源，顺序一致）
  const journalEntries = [];
  installSignalGuards(journalPath);
  for (const m of selected) {
    const abs = path.join(ROOT, m.file);
    // ★ 守卫不存在 ⇒ 这次变异**什么也证明不了**（`node --test <缺失文件>` 退出码 1 会被读成"守卫红了"）
    const gone = guardMissing(m);
    if (gone) {
      results.push({ ...m, verdict: `⛔ **守卫不存在**（${gone}）⇒ 本次变异什么也没证明`, ok: false, guardGone: true, missingWhy: gone });
      continue;
    }
    const before = fs.readFileSync(abs, 'utf8');
    if (!before.includes(m.from)) {
      // ★ 这不是"守卫没抓住"，而是**清单过期**：`from` 已经不在源文件里 ⇒ 这次自检什么也没验到。
      //   必须与"守卫没红"分开报 —— 否则真实症状（清单没跟上源文件）会被读成"守卫是假的"。
      results.push({ ...m, verdict: '⚠ 变异点没找到（源文件已变 ⇒ 清单过期）', ok: false, stale: true });
      continue;
    }
    // ★ CTest 条目在**施加变异之前**先验基线：构建目录没配置 / 用例不存在 / 基线本来就红 ⇒
    //   这次变异什么都证明不了 ⇒ 记"无法判定"，⛔ 不记成"守卫没红"。
    if (m.ctest) {
      const pre = runCtestGuard(m.ctest, null, m.buildDir ?? CTEST_BUILD_DIR);
      if (pre.unavailable || pre.status !== 0) {
        const why = pre.unavailable ?? `\`${m.ctest}\` 基线退出码 ${pre.status}`;
        results.push({
          ...m,
          verdict: `⏭ **无法判定**（CTest 基线不成立：${why}）`,
          ok: false,
          unverifiable: true,
          ran: { skipReason: why },
        });
        continue;
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
      entry.buildDir = m.buildDir ?? CTEST_BUILD_DIR;
    }
    journalAdd(journalPath, journalEntries, entry);
    fs.writeFileSync(abs, mutated);
    let code = null;
    let ran = null; // {pass, fail, skipped, skipReason}
    let unavailable = null; // CTest 条目：构建/ctest 跑不起来
    let restoreProblem = null;
    // ★ 子进程输出落 `.tmp/`（gitignored）—— ⛔ 不写系统临时目录：沙箱里那可能是唯一写不进去的地方
    const LOG_DIR = path.join(ROOT, '.tmp', 'mutate-check', 'logs');
    fs.mkdirSync(LOG_DIR, { recursive: true });
    const log = path.join(LOG_DIR, `mutate-${process.pid}-${results.length}.log`);
    try {
      // ★ 沙箱里不能**捕获**子进程输出（管道要命名管道 ⇒ EPERM）⇒ 用**文件描述符重定向**拿同一份答案
      //   （`tools/corpus.mjs` 的 `runCapture()` 就是这么绕的）。为什么要这份输出：
      //   **守卫 skip 时退出码也是 0** —— 只看退出码会把"这台机器上没跑"误报成"守卫没红"（实测踩过：
      //   `emulator-numeric-ops` 的两条要 `dist/install`，本机没有 ⇒ 变异看起来"没抓住"，其实是没跑）。
      const fd = fs.openSync(log, 'w');
      try {
        if (m.ctest) {
          const r = runCtestGuard(m.ctest, fd, m.buildDir ?? CTEST_BUILD_DIR);
          code = r.status ?? null;
          unavailable = r.unavailable ?? null;
        } else {
          const r = spawnSync(process.execPath, ['--test', '--test-isolation=none', m.guard], { cwd: ROOT, stdio: ['ignore', fd, fd] });
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
      // ★ 无论成败都还原；还原后**比对全文**（只比长度不够）
      fs.writeFileSync(abs, before);
      if (m.ctest) {
        // ★ 还原只还原了**源文件**；判据在 exe 里 ⇒ 必须重建回基线，否则下一次 ctest 会假红。
        //   顺手把「还原 ⇒ 再跑必须绿」也机械化：不绿就中止（退出码 2）。
        const build = buildCtestTargets(null, m.buildDir ?? CTEST_BUILD_DIR);
        if (build.error || build.status !== 0) {
          restoreProblem = '还原后重建失败（exe 仍是变异版）';
        } else {
          const run = spawnSync('ctest', ['--test-dir', m.buildDir ?? CTEST_BUILD_DIR, '-R', `^${m.ctest}$`],
                                { cwd: ROOT, stdio: 'ignore' });
          if (run.error || run.status !== 0) {
            restoreProblem = `还原后 \`${m.ctest}\` 仍红（基线没有恢复）`;
          }
        }
      }
    }
    if (restoreProblem) {
      // ★ journal 条目**故意留着**：源文件或 exe 还没回到基线 ⇒ 下次启动靠它继续/拒绝
      console.error(`\n✗ **还原失败**：${m.file} —— ${restoreProblem}。已中止（工作树可能被污染，请查 git diff）`);
      return 2;
    }
    if (fs.readFileSync(abs, 'utf8') !== before) {
      console.error(`\n✗ **还原失败**：${m.file} 与施加前不一致 —— 已中止（工作树可能被污染，请查 git diff）`);
      return 2;
    }
    // ★ 只有"还原 + 全文比对通过"之后才把条目移出 journal（顺序反了就等于"以为还原了"）
    journalDrop(journalPath, journalEntries, entry);
    if (m.ctest) {
      results.push({
        ...m,
        code,
        ran: unavailable ? { skipReason: unavailable } : null,
        verdict: unavailable
          ? `⏭ **无法判定**（${unavailable}）`
          : code !== 0
            ? `✅ 守卫红了（CTest 退出码 ${code}）`
            : '❌ **守卫没红**（CTest 退出码 0）',
        // ★ 与 node 侧一致：**跑不起来 ≠ 守卫没抓住**。`unavailable`（构建目录缺失 / cmake 不可用 /
        //   构建失败）只是"这台机器上无法判定"，⛔ 不许记成 `failed` 并 exit 1 —— 否则在没有
        //   `apps/emulator-cxx/out` 的机器上，所有 CTest 条目都会被误报成"守卫坏了"。
        ok: !unavailable && code !== 0,
        unverifiable: Boolean(unavailable),
      });
      continue;
    }
    const skipped = ran && ran.fail === 0 && ran.skipped > 0;
    results.push({
      ...m,
      code,
      ran,
      verdict: code !== 0
        ? `✅ 守卫红了（退出码 ${code}）`
        : skipped
          ? `⏭ **无法判定**（守卫 skip，退出码 0）：${ran.skipReason ?? '（无原因）'}`
          : `❌ **守卫没红**（退出码 0${ran ? `，跑了 ${ran.pass} 条` : ''}）`,
      ok: code !== 0,
      unverifiable: Boolean(skipped),
    });
  }

  console.log('守卫自检（改坏一处常量 ⇒ 指定守卫是否当场红）：\n');
  for (const r of results) console.log(`  ${r.verdict}  ${r.what}`);
  const stale = results.filter((r) => r.stale);
  const goneRows = results.filter((r) => r.guardGone);
  const failed = results.filter((r) => !r.ok && !r.unverifiable && !r.stale && !r.guardGone);
  const unver = results.filter((r) => r.unverifiable);
  console.log(`\n${results.length - failed.length - unver.length - stale.length - goneRows.length}/${results.length} 条按预期变红${unver.length ? ` · ${unver.length} 条**无法判定**（守卫在本机跑不起来 ⇒ 不能算"没红"，也不算失败）` : ''}。`);
  if (unver.length) {
    console.log('⏭ 这些守卫在**本机**跑不起来（不是"没抓住"）—— 要它有意义就得先备齐它的语料/资产/构建目录：');
    for (const u of unver) console.log(`  - ${u.what}（${u.guard}）：${u.ran?.skipReason ?? '（无原因）'}`);
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
  if (unver.length && unver.length === results.length) {
    // ★ 全部条目都"无法判定"（构建目录不存在 / 资产缺失）≠ 通过：旧实现这里仍 exit 0，
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
if (isDirectRun) process.exitCode = main();

