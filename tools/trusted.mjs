#!/usr/bin/env node
/**
 * tools/trusted.mjs — **被信任子系统**（`tools/trusted.json`）：组内文件自 `at` 起没改过 ⇒ 该组 CTest 不跑。
 *
 * 经派发器：`pnpm tools trusted <status|check|describe|mark> […]`
 * 也可独立跑：`node tools/trusted.mjs --status`
 *
 * ★ 判据只有一条：`git diff --quiet <at> -- <globs>` 退出 0 ⇒ **frozen** ⇒ configure 时
 *   `apps/emulator-lift/CMakeLists.txt` 段尾把该组 CTest 标 `DISABLED TRUE`（`-D LIFT_ALL_TESTS=ON` 强制全跑）。
 *   一个子系统一个 `at`（不存逐文件 sha256）⇒ 清单**不可能漂移**：文件一改就 `changed`。
 * ★ 组 → CTest 名字的映射**只在 CMake 段尾**（不在这里，也不新增 `add_test`）：判据是**注册位置**（AGENTS §12.4）。
 * ★ `check` 是**一条**轻守卫：每个 glob 至少命中一个 `git ls-files` 文件 ——
 *   文件被删 / 改名后 glob 空集，该组会**永久 frozen**（整组静默不跑），这才是要喊的那种坏。
 */
import fs from 'node:fs';
import path from 'node:path';
import process from 'node:process';
import { fileURLToPath } from 'node:url';

import { REPO_ROOT } from './lib/paths.mjs';
import { runCapture } from './lib/exec.mjs';

export const DOMAIN = {
  id: 'trusted',
  title: '被信任子系统（`tools/trusted.json`：组内文件自 `at` 起没改 ⇒ CTest 标 `DISABLED`）',
  data: [
    '`tools/trusted.json`（每组的 `globs` + `at` = 最后一个可信提交；一个子系统一个数）',
    '`apps/emulator-lift/CMakeLists.txt` 段尾（读本文件；`-D LIFT_ALL_TESTS=ON` 强制全跑）',
  ],
  access: '`mark --write` 是唯一写入口（把某组的 `at` 写成当前 `HEAD`）；其余只读',
  tool: 'tools/trusted.mjs',
};

export const OPERATIONS = [
  { name: 'status', argv: ['--status'], mutates: false, summary: '每组的 at / glob 命中文件数 / frozen|changed' },
  { name: 'check', argv: ['--check'], mutates: false, summary: '每个 glob 至少命中一个 `git ls-files` 文件（红 ⇒ 退出码 1）' },
  { name: 'describe', argv: ['--describe'], mutates: false, summary: '字段 / 判据 / 操作（schema 真源）' },
  { name: 'mark', argv: ['--mark'], mutates: true, summary: '把某组的 `at` 写成当前 HEAD（缺省 dry-run；`--write` 落盘）' },
];

export const TRUSTED_FILE = 'tools/trusted.json';

const HEX40 = /^[0-9a-f]{40}$/;

export function readTrusted(root = REPO_ROOT) {
  return JSON.parse(fs.readFileSync(path.join(root, TRUSTED_FILE), 'utf8'));
}

/** 组名 → {globs, at} 的形状问题（空数组 / 非 40-hex） */
export function shapeProblems(data) {
  const problems = [];
  if (data === null || typeof data !== 'object' || Array.isArray(data)) {
    return [`${TRUSTED_FILE} 顶层必须是对象（形如 {"<组>": {"globs": [...], "at": "<40-hex>"}}）`];
  }
  for (const [name, g] of Object.entries(data)) {
    if (!Array.isArray(g?.globs) || g.globs.length === 0) problems.push(`${name}: globs 必须是非空数组`);
    if (!HEX40.test(g?.at ?? '')) problems.push(`${name}: at 必须是 40 位小写 git sha，实际 ${JSON.stringify(g?.at)}`);
  }
  return problems;
}

/** glob 命中的 tracked 文件（git ls-files）。跑不起来时回 error，调用方决定红/警告 */
export function globFiles(globs, root = REPO_ROOT) {
  const r = runCapture(root, 'git', ['ls-files', '--', ...globs]);
  if (r.error) return { files: [], error: r.error };
  if (r.code !== 0) return { files: [], error: `git ls-files 退出 ${r.code}` };
  return { files: r.out.split('\n').map((s) => s.trim()).filter(Boolean) };
}

/** 与 CMake 段尾**同一条**判据：`git diff --quiet <at> -- <globs>` */
export function groupState(group, root = REPO_ROOT) {
  const r = runCapture(root, 'git', ['diff', '--quiet', group.at, '--', ...group.globs]);
  if (r.error) return { state: 'unknown', why: `git 跑不起来：${r.error}` };
  if (r.code === 0) return { state: 'frozen' };
  if (r.code === 1) return { state: 'changed' };
  return { state: 'unknown', why: `git diff 退出 ${r.code}（at 可能不存在 / globs 写坏）` };
}

/** `check`：每个 glob 至少命中一个 tracked 文件（否则该组永久 frozen） */
export function check(root = REPO_ROOT) {
  let data;
  try {
    data = readTrusted(root);
  } catch (err) {
    return [`${TRUSTED_FILE} 读不了 / 不是 JSON：${err.message}`];
  }
  const problems = shapeProblems(data);
  if (problems.length > 0) return problems;
  for (const [name, g] of Object.entries(data)) {
    for (const glob of g.globs) {
      const { files, error } = globFiles([glob], root);
      if (error) problems.push(`${name}: ${glob} ⇒ ${error}`);
      else if (files.length === 0) problems.push(`${name}: glob 没有命中任何 tracked 文件 ⇒ 该组会永久 frozen（被删 / 改名了？）：${glob}`);
    }
  }
  return problems;
}

export function describe() {
  return {
    file: TRUSTED_FILE,
    fields: [
      { name: '<组>.globs', type: 'string[]', meaning: '该子系统的文件集（git pathspec；`**` 递归、`*` 段内通配）' },
      { name: '<组>.at', type: '40-hex git sha', meaning: '该组最后一个可信提交；`git diff --quiet <at> -- <globs>` 退出 0 ⇒ frozen' },
    ],
    invariants: [
      { id: 'glob-nonempty', text: '每个 glob 至少命中一个 `git ls-files` 文件', enforcedBy: ['tools/test/trusted.test.mjs', 'pnpm tools trusted check'] },
      { id: 'at-shape', text: 'at 是 40 位小写 sha', enforcedBy: ['pnpm tools trusted check'] },
      { id: 'safe-direction', text: 'git 不可用 / JSON 坏了 ⇒ CMake 全跑（不 DISABLE）', enforcedBy: ['apps/emulator-lift/CMakeLists.txt 段尾'] },
    ],
    operations: OPERATIONS.map(({ name, mutates, summary }) => ({ name, mutates, summary })),
    writePath: '`pnpm tools trusted mark <组> --write` 是唯一写入口（写当前 HEAD）',
    forceRun: '`-D LIFT_ALL_TESTS=ON`（configure）或 `ctest -R <名字>`（单条强制跑）',
  };
}

export function describeText() {
  const d = describe();
  const L = [`${d.file} —— 被信任子系统（schema 真源：node tools/trusted.mjs --describe）`, ''];
  L.push('字段');
  for (const f of d.fields) L.push(`  ${f.name}　${f.type}　${f.meaning}`);
  L.push('', '不变量');
  for (const inv of d.invariants) L.push(`  ${inv.id}　${inv.text}　（守：${inv.enforcedBy.join(' / ')}）`);
  L.push('', '操作');
  for (const o of d.operations) L.push(`  ${o.name}${o.mutates ? '(会写)' : ''}　${o.summary}`);
  L.push('', `写入口　${d.writePath}`, `强制全跑　${d.forceRun}`);
  return `${L.join('\n')}\n`;
}

export function statusRows(root = REPO_ROOT) {
  const data = readTrusted(root);
  const problems = shapeProblems(data);
  const rows = [];
  for (const [name, g] of Object.entries(data)) {
    const { files, error } = globFiles(g.globs, root);
    const st = groupState(g, root);
    rows.push({ name, at: g.at, files: files.length, state: st.state, why: st.why ?? error });
  }
  return { rows, problems };
}

export function mark(name, { write = false, root = REPO_ROOT } = {}) {
  const data = readTrusted(root);
  if (!data[name]) throw new Error(`没有这个组：${name}（现有：${Object.keys(data).join(' / ')}）`);
  const r = runCapture(root, 'git', ['rev-parse', 'HEAD']);
  const head = r.out.trim();
  if (r.error || r.code !== 0 || !HEX40.test(head)) throw new Error(`取不到当前 HEAD：${r.error ?? head}`);
  const before = data[name].at;
  data[name].at = head;
  if (write) {
    fs.writeFileSync(path.join(root, TRUSTED_FILE), `${JSON.stringify(data, null, 2)}\n`, 'utf8');
  }
  return { name, before, after: head, write };
}

const HELP = `tools/trusted.mjs — 被信任子系统（组内文件没改 ⇒ CTest 不跑）

  --status                     每组：at / glob 命中文件数 / frozen|changed
  --check                      每个 glob 至少命中一个 tracked 文件（红 ⇒ 退出码 1）
  --describe                   字段 / 不变量 / 操作（schema 真源）
  --mark <组> [--write]        把该组的 at 写成当前 HEAD（缺省 dry-run）
`;

export async function main(argv = process.argv.slice(2)) {
  const args = argv.filter((a) => a !== '');
  if (args.includes('--help') || args.includes('-h')) {
    process.stdout.write(HELP);
    return 0;
  }
  if (args.includes('--describe')) {
    process.stdout.write(describeText());
    return 0;
  }
  if (args.includes('--check')) {
    const problems = check();
    if (problems.length === 0) {
      process.stdout.write('trusted: check OK\n');
      return 0;
    }
    process.stderr.write(`trusted: check 失败（${problems.length}）\n  - ${problems.join('\n  - ')}\n`);
    return 1;
  }
  const mi = args.indexOf('--mark');
  if (mi >= 0) {
    const name = args[mi + 1];
    if (!name) {
      process.stderr.write('--mark 需要一个组名\n');
      return 2;
    }
    const write = args.includes('--write');
    try {
      const r = mark(name, { write });
      process.stdout.write(`trusted: ${r.name} 的 at ${r.before} ⇒ ${r.after}${write ? '（已写盘）' : '（dry-run；加 --write 落盘）'}\n`);
      return 0;
    } catch (err) {
      process.stderr.write(`trusted: ${err.message}\n`);
      return 2;
    }
  }
  if (args.length === 0 || args.includes('--status')) {
    const { rows, problems } = statusRows();
    for (const p of problems) process.stderr.write(`trusted: ${p}\n`);
    process.stdout.write('被信任子系统（frozen ⇒ configure 时该组 CTest 标 DISABLED；-D LIFT_ALL_TESTS=ON 强制全跑）\n');
    for (const r of rows) {
      const extra = r.why ? `（${r.why}）` : '';
      process.stdout.write(`  ${r.name.padEnd(17)} at ${r.at.slice(0, 12)}  ${r.state.padEnd(8)} 文件 ${r.files}${extra}\n`);
    }
    return problems.length > 0 ? 1 : 0;
  }
  process.stderr.write(`未知参数：${args.join(' ')}\n\n${HELP}`);
  return 2;
}

const isDirectRun = process.argv[1] && path.resolve(process.argv[1]) === path.resolve(fileURLToPath(import.meta.url));
if (isDirectRun) {
  main()
    .then((code) => {
      process.exitCode = code ?? 0;
    })
    .catch((err) => {
      process.stderr.write(`ERROR: ${err.stack ?? err.message}\n`);
      process.exitCode = 2;
    });
}
