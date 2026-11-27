/**
 * tools/lib/experience.mjs — **用户体验锁**（`--user-run`）的判据（纯函数 / 只读，唯一一份）
 *
 * ★ 分层（AGENTS 工具层口径）：CLI（`tools/experience.mjs`）只做"参数 → 模型 → 输出"，
 *   判据放这里；别的工具（`tools/mutate-check.mjs`）import 本文件，⛔ 不复制读锁逻辑。
 *   CLI 仍 re-export 这些函数（`readLock` / `classifyLock` / `isExperienceActive`），
 *   所以"给别的工具 import"的入口与 `tools/experience.mjs` 的文档一致。
 *
 * ## 判活口径（唯一一条）
 * `process.kill(pid, 0)`：不抛 ⇒ 活；`ESRCH` ⇒ 死；`EPERM` ⇒ **活**（进程在，只是没权限发信号）。
 * ⇒ 锁文件存在**不等于** ACTIVE：用户上次 `std::_Exit(3)`（guest fault）会留下锁，但那个 pid 已经死了
 * ⇒ 判 **stale = INACTIVE**，`clear` 可以清掉。
 *
 * ## 额外兜底（锁缺失时也认）
 * 扫进程命令行里带 `--user-run` 的（POSIX `pgrep -fl -- '--user-run'`；win32 `Get-CimInstance`）。
 * 理由：用户可能用**旧二进制**跑游戏（没有写锁），或锁被中途删掉；命令行里的 flag 仍在。
 * ⛔ 不用 `pkill`（那会**杀**东西，本模块只读）。
 * ★ 代价：命令行兜底会把"命令行里恰好含 `--user-run` 的包装 shell"也算进来 —— 方向是**更保守**
 *   （宁可多报一个，也别误杀），不是判据错误。
 */
import { spawnSync } from 'node:child_process';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import process from 'node:process';

/** 锁文件名（`apps/emulator-lift/src/runtime/experience_lock.h` 的 `kLockFileName` 必须是同一个字面量）。 */
export const LOCK_FILE_NAME = 'amayui-re-experience.json';

/** 锁路径：`<os temp>/amayui-re-experience.json`（与 C++ 的 `std::filesystem::temp_directory_path()` 同一个目录）。 */
export function lockPath() {
  return path.join(os.tmpdir(), LOCK_FILE_NAME);
}

/**
 * pid 是否活着。判据 = `process.kill(pid, 0)`（⛔ 不发任何信号，只探测）。
 * `EPERM` = 进程存在但无权限 ⇒ **活**；`ESRCH` / 其它 ⇒ 死。
 */
export function pidAlive(pid) {
  const n = Number(pid);
  if (!Number.isInteger(n) || n <= 0) return false;
  try {
    process.kill(n, 0);
    return true;
  } catch (err) {
    return err?.code === 'EPERM';
  }
}

/**
 * 读锁文件（**不判活**）。
 * @param {{ path?: string }} [opts] 测试用：显式锁路径（缺省 `<os temp>/…`）。
 * @returns {{ok: true, state: 'ok', lock: object, path: string, text: string}
 *          | {ok: false, state: 'missing'|'unreadable'|'invalid', reason: string, path: string, text?: string}}
 */
export function readLock({ path: file = lockPath() } = {}) {
  let text;
  try {
    text = fs.readFileSync(file, 'utf8');
  } catch (err) {
    if (err?.code === 'ENOENT') {
      return { ok: false, state: 'missing', reason: `体验锁不存在（${file}）`, path: file };
    }
    return {
      ok: false,
      state: 'unreadable',
      reason: `体验锁读不出来（${file}）：${err?.message ?? err}`,
      path: file,
    };
  }
  let lock;
  try {
    lock = JSON.parse(text);
  } catch (err) {
    return {
      ok: false,
      state: 'invalid',
      reason: `体验锁不是合法 JSON（${file}）：${err?.message ?? err}`,
      path: file,
      text,
    };
  }
  if (lock === null || typeof lock !== 'object' || Array.isArray(lock)) {
    return {
      ok: false,
      state: 'invalid',
      reason: `体验锁顶层必须是对象（${file}）`,
      path: file,
      text,
    };
  }
  return { ok: true, state: 'ok', lock, path: file, text };
}

/**
 * 判锁：缺锁 / 坏 JSON / pid 已死 ⇒ inactive（**带 reason**）；pid 活 ⇒ active。
 * @param {ReturnType<typeof readLock>|object|null|undefined} lock `readLock()` 的返回值；
 *   也容忍直接传解析后的锁对象 / `null`。
 * @param {{ alive?: (pid: number) => boolean }} [opts] 测试注入判活（⛔ 真杀进程）。
 */
export function classifyLock(lock, { alive = pidAlive } = {}) {
  // 直接传解析后的锁对象（形状：没有 `ok` 字段但有 `pid`）也认。
  const read =
    lock !== null && typeof lock === 'object' && 'ok' in lock
      ? lock
      : lock === null || lock === undefined
        ? { ok: false, state: 'missing', reason: '体验锁不存在' }
        : { ok: true, state: 'ok', lock, path: lockPath() };

  if (read.ok !== true) {
    return {
      active: false,
      state: read.state ?? 'invalid',
      reason: read.reason ?? '体验锁不可用',
      pid: null,
      lock: null,
      path: read.path ?? lockPath(),
    };
  }
  const parsed = read.lock ?? {};
  const pid = parsed.pid;
  // ★ 锁的形状是 `"pid": <int>`（C++ 侧手写 JSON 写的是数字）⇒ 字符串 / 0 / 负数都不认：
  //   "pid 能被 Number() 转成整数"不等于"它是 pid"，放宽会让 `"pid":"abc"` 之类的坏锁静默变成 stale。
  if (typeof pid !== 'number' || !Number.isInteger(pid) || pid <= 0) {
    return {
      active: false,
      state: 'invalid',
      reason: `体验锁里的 pid 不是正整数：${JSON.stringify(parsed.pid)}`,
      pid: null,
      lock: parsed,
      path: read.path ?? lockPath(),
    };
  }
  if (!alive(pid)) {
    return {
      active: false,
      state: 'stale',
      reason: `体验锁的 pid=${pid} 已不在运行（陈旧锁）`,
      pid,
      lock: parsed,
      path: read.path ?? lockPath(),
    };
  }
  return {
    active: true,
    state: 'active',
    reason: `用户正在体验（pid=${pid}）`,
    pid,
    lock: parsed,
    path: read.path ?? lockPath(),
  };
}

/**
 * 命令行兜底：扫进程命令行里含 `--user-run` 的（只读，⛔ 不 kill）。
 * @returns {{ok: boolean, pids: {pid: number, command: string}[], error: string|null}}
 */
export function scanProcessesForUserRun({ platform = process.platform, spawn = spawnSync } = {}) {
  const command =
    platform === 'win32'
      ? {
          bin: 'powershell.exe',
          args: [
            '-NoProfile',
            '-Command',
            "Get-CimInstance Win32_Process | ? { $_.CommandLine -match '--user-run' } | % { $_.ProcessId }",
          ],
        }
      : { bin: 'pgrep', args: ['-fl', '--', '--user-run'] };

  let r;
  try {
    r = spawn(command.bin, command.args, {
      encoding: 'utf8',
      timeout: 5000,
      windowsHide: true,
      stdio: ['ignore', 'pipe', 'pipe'],
    });
  } catch (err) {
    return { ok: false, pids: [], error: `扫进程命令行失败（${command.bin}）：${err?.message ?? err}` };
  }
  if (r?.error) {
    return { ok: false, pids: [], error: `扫进程命令行失败（${command.bin}）：${r.error.message ?? r.error}` };
  }
  const out = String(r?.stdout ?? '');
  // `pgrep` 退出码 1 = 没有匹配（不是错误）；windows powershell 退出码 0 = 成功（可能没有输出）。
  if (r?.status !== 0 && r?.status !== 1) {
    return {
      ok: false,
      pids: [],
      error: `扫进程命令行失败（${command.bin} 退出 ${r?.status ?? 'null'}）：${String(r?.stderr ?? '').trim()}`,
    };
  }
  const pids = [];
  for (const line of out.split(/\r?\n/)) {
    const t = line.trim();
    if (!t) continue;
    const m = /^(\d+)(?:\s+(.*))?$/.exec(t);
    if (!m) continue;
    const pid = Number(m[1]);
    if (!Number.isInteger(pid) || pid <= 0 || pid === process.pid) continue;
    pids.push({ pid, command: (m[2] ?? '').trim() });
  }
  return { ok: true, pids, error: null };
}

/**
 * **唯一判据**：锁 ACTIVE，或（锁不可用时）命令行里有 `--user-run` ⇒ active。
 * @param {{ lockOptions?: {path?: string}, alive?: (pid:number)=>boolean,
 *           scan?: () => ReturnType<typeof scanProcessesForUserRun> }} [opts]
 */
export function isExperienceActive({ lockOptions = {}, alive = pidAlive, scan = scanProcessesForUserRun } = {}) {
  const read = readLock(lockOptions);
  const byLock = classifyLock(read, { alive });
  if (byLock.active) return { ...byLock, source: 'lock', processes: [] };

  // ★ 锁缺失 / 坏 / pid 已死 ⇒ 兜底扫命令行（用户可能用没写锁的旧二进制）。
  const s = scan();
  if (s.pids.length > 0) {
    return {
      active: true,
      state: 'active',
      source: 'process-scan',
      reason: `进程命令行里有 --user-run（pid=${s.pids.map((p) => p.pid).join(',')}）`,
      pid: s.pids[0].pid,
      lock: null,
      path: read.path,
      processes: s.pids,
    };
  }
  return {
    ...byLock,
    source: byLock.state === 'missing' ? 'none' : 'lock',
    processes: [],
    scan: { ok: s.ok, error: s.error },
  };
}

/** 一行摘要（guard / status / mutate-check 共用的**同一份**人读文案）。 */
export function summaryOf(result) {
  const lock = result.lock ?? {};
  const bits = [`pid=${result.pid ?? '-'}`];
  if (lock.started_at) bits.push(`started_at=${lock.started_at}`);
  if (lock.exe) bits.push(`exe=${lock.exe}`);
  return bits.join('  ');
}

/**
 * guard：ACTIVE ⇒ 退出码 2（打印原因 + 建议）；INACTIVE ⇒ 0。
 * `DSH_ALLOW_DURING_USER_RUN=1` ⇒ 明确放行（打印一行说明），仍返回 0。
 * @param {{ isActive?: () => object, env?: object, log?: {error: Function} }} [opts]
 */
export function guard({ isActive = () => isExperienceActive(), env = process.env, log = console } = {}) {
  const result = isActive();
  if (!result?.active) return 0;
  if (env.DSH_ALLOW_DURING_USER_RUN === '1') {
    log.error(`[experience] 用户正在体验（${summaryOf(result)}），但 DSH_ALLOW_DURING_USER_RUN=1 ⇒ 放行`);
    return 0;
  }
  log.error(`⛔ 用户正在体验（${summaryOf(result)}）⇒ 杀进程前先确认目标 pid，⛔ 不要动它（--user-run）。`);
  log.error('   ⇒ 等用户退出（或让用户自己结束体验）后再跑；');
  log.error('   ⇒ 只看状态：node tools/experience.mjs status --json');
  log.error('   ⇒ 明确知道在做什么才绕过：DSH_ALLOW_DURING_USER_RUN=1');
  return 2;
}
