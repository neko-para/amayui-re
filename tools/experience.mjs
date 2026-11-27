#!/usr/bin/env node
/**
 * tools/experience.mjs — **用户体验锁**（`--user-run`）的 CLI（判据在 `tools/lib/experience.mjs`）
 *
 * ## 它解决什么
 * 用户本人在跑游戏体验时，agent 的 `kill` / `pkill` 会把他打断。⇒ 用户启动时加 `--user-run`
 * （`apps/emulator-lift/src/aot/aot_main.cpp`），它写一个**机器级**锁文件
 * `<os temp>/amayui-re-experience.json`（两个工作树共用同一份，AGENTS §14.5）。
 * ★ 用户口径 2026-11-27：体验构建**永远**走 `out-user/` ⇒ 构建 / 测试**不需要**让路；
 *   本工具**只**用于"杀进程前认人"（AGENTS §17 的 kill 纪律）。
 *
 * ## 用法
 * ```bash
 * node tools/experience.mjs status [--json]   # ACTIVE / INACTIVE + pid / started_at / exe / argv
 * node tools/experience.mjs guard             # ACTIVE ⇒ exit 2；INACTIVE ⇒ exit 0
 * node tools/experience.mjs clear [--force]   # 只清"pid 已死"的锁；--force 才清活锁
 * ```
 * 经派发器也认：`pnpm tools experience status|guard|clear`（`main()` 同时收 `status` 与 `--status`）。
 *
 * ## 导出（给别的工具 import，⛔ 不复制读锁逻辑）
 * `readLock()` / `classifyLock(lock, { alive })` / `isExperienceActive()` —— 实现在
 * `tools/lib/experience.mjs`（分层守卫要求 CLI 只 import `./lib/*`），这里**原样 re-export**。
 */
import fs from 'node:fs';
import path from 'node:path';
import process from 'node:process';
import { fileURLToPath } from 'node:url';

import { classifyLock, guard, isExperienceActive, readLock } from './lib/experience.mjs';

export {
  LOCK_FILE_NAME,
  classifyLock,
  guard,
  isExperienceActive,
  lockPath,
  pidAlive,
  readLock,
  scanProcessesForUserRun,
  summaryOf,
} from './lib/experience.mjs';

export const DOMAIN = {
  id: 'experience',
  title: '用户体验锁（`--user-run` → `<os temp>/amayui-re-experience.json`）：**杀进程前认人**（⛔ 不拦构建）',
  data: [
    '`<os temp>/amayui-re-experience.json`（**机器级**；两个工作树共用；唯一写入口 = `emulator-lift-aot --user-run`）',
    '运行中进程的命令行（锁缺失时的兜底：含 `--user-run` 的那个实例）',
  ],
  access: '`clear` 是唯一写入口（只清 pid 已死的锁；`--force` 才清活锁）；其余只读',
  tool: 'tools/experience.mjs',
};

export const OPERATIONS = [
  { name: 'status', argv: ['--status'], mutates: false, summary: '打印 ACTIVE / INACTIVE + pid / started_at / exe / argv（`--json` 机器可读）' },
  { name: 'guard', argv: ['--guard'], mutates: false, summary: '杀进程前的检查：ACTIVE ⇒ 退出码 2；INACTIVE ⇒ 0（⛔ 不拦构建/测试；`DSH_ALLOW_DURING_USER_RUN=1` 放行）' },
  { name: 'clear', argv: ['--clear'], mutates: true, summary: '清"pid 已死 / 坏 JSON"的残留锁；活锁要 `--force`' },
];

/** `status`：人读 / `--json`。ACTIVE / INACTIVE **都是退出码 0**（这是只读查询）。 */
function cmdStatus(rest) {
  const json = rest.includes('--json');
  const result = isExperienceActive();
  if (json) {
    console.log(
      JSON.stringify(
        {
          active: result.active,
          source: result.source,
          state: result.state,
          reason: result.reason,
          path: result.path,
          pid: result.pid ?? null,
          started_at: result.lock?.started_at ?? null,
          exe: result.lock?.exe ?? null,
          argv: Array.isArray(result.lock?.argv) ? result.lock.argv : null,
          cwd: result.lock?.cwd ?? null,
          game_dir: result.lock?.game_dir ?? null,
          appdata: result.lock?.appdata ?? null,
          processes: result.processes ?? [],
          scan: result.scan ?? null,
        },
        null,
        2,
      ),
    );
    return 0;
  }
  if (!result.active) {
    console.log(`INACTIVE —— ${result.reason}`);
    console.log(`  锁文件：${result.path}`);
    console.log('  清理残留锁：node tools/experience.mjs clear');
    if (result.scan && result.scan.ok === false) console.log(`  ⚠ 命令行兜底扫描失败：${result.scan.error}`);
    return 0;
  }
  console.log(`ACTIVE —— ${result.reason}`);
  console.log(`  pid=${result.pid}  started_at=${result.lock?.started_at ?? '-'}`);
  console.log(`  exe=${result.lock?.exe ?? '-'}`);
  console.log(`  argv=${JSON.stringify(result.lock?.argv ?? [])}`);
  if (result.lock) {
    console.log(`  cwd=${result.lock.cwd ?? '-'}  game_dir=${result.lock.game_dir ?? '-'}  appdata=${result.lock.appdata ?? '-'}`);
  }
  if (result.source === 'process-scan') {
    console.log(`  来源=命令行兜底（锁文件不可用）：${result.processes.map((p) => p.pid).join(', ')}`);
  }
  console.log(`  锁文件：${result.path}`);
  return 0;
}

/** `clear`：只清"pid 已死 / 坏 JSON"的锁；活锁要 `--force`。 */
function cmdClear(rest) {
  const force = rest.includes('--force');
  const read = readLock();
  if (read.state === 'missing') {
    console.log('没有锁文件（已干净）');
    return 0;
  }
  if (read.ok) {
    const c = classifyLock(read);
    if (c.active && !force) {
      console.error(`⛔ 锁是**活的**（pid=${c.pid} 正在运行）⇒ 不清。`);
      console.error('   等用户自己退出，或确认后 `clear --force`（那会删掉一个正在体验实例的标记）。');
      return 1;
    }
    fs.rmSync(read.path, { force: true });
    console.log(
      c.active
        ? `已**强制**清除活锁（pid=${c.pid}）：${read.path}`
        : `已清除陈旧锁（${c.reason}）：${read.path}`,
    );
    return 0;
  }
  // 坏 JSON / 不可读：认不出 pid ⇒ "pid 已死"这件事证明不了；不可读更不该瞎删。
  if (read.state === 'unreadable' && !force) {
    console.error(`⛔ 锁读不出来（${read.reason}）⇒ 不清；确认后用 --force。`);
    return 1;
  }
  fs.rmSync(read.path, { force: true });
  console.log(`已清除无法判活的锁（${read.state}）：${read.path}`);
  return 0;
}

const HELP = `node tools/experience.mjs <动作> [选项]

  status [--json]   查看用户体验锁：ACTIVE / INACTIVE + pid / started_at / exe / argv
  guard             ACTIVE ⇒ 退出码 2（**只给"杀进程前"用**；⛔ 不再拦构建 / 测试 / 变异自检）；INACTIVE ⇒ 0
  clear [--force]   清锁：只清"pid 已死 / 坏 JSON"的残留锁；活锁要 --force

  env DSH_ALLOW_DURING_USER_RUN=1   明确允许"用户正在体验也照做"（guard 放行，打印一行说明）

★ 锁是**机器级**的：<os temp>/amayui-re-experience.json（两个工作树共用）。
★ 锁缺失时也认命令行里的 --user-run（pgrep / Get-CimInstance 兜底；⛔ 不 kill 任何进程）。
`;

export async function main(argv = process.argv.slice(2)) {
  const args = argv.filter((a) => a !== '');
  const first = args[0];
  if (first === undefined || first === '--help' || first === '-h' || first === 'help') {
    console.log(HELP);
    return 0;
  }
  const op = String(first).replace(/^--/, '');
  const rest = args.slice(1);
  if (op === 'status') return cmdStatus(rest);
  if (op === 'guard') return guard();
  if (op === 'clear') return cmdClear(rest);
  console.error(`未知动作：${first}（可用：status / guard / clear）\n\n${HELP}`);
  return 2;
}

const isDirectRun = process.argv[1] && path.resolve(process.argv[1]) === path.resolve(fileURLToPath(import.meta.url));
if (isDirectRun) {
  main().then(
    (code) => {
      process.exitCode = code ?? 0;
    },
    (err) => {
      console.error(`ERROR: ${err?.stack ?? err}`);
      process.exitCode = 2;
    },
  );
}
