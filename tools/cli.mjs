#!/usr/bin/env node
/**
 * tools/cli.mjs — 工具层的**唯一入口**
 *
 *   pnpm tools                      → 打印**派生的域地图**：域 → 数据 → 读写 → 操作
 *   pnpm tools <域>                 → 只看该域（数据 / 不变量 / 操作 + 摘要）
 *   pnpm tools <域> <动作> [args…]   → **薄转发**到该域的工具
 *   pnpm tools --json               → 机器可读地图
 *
 * ★ **地图不是手写的**：每个工具自己声明 `DOMAIN`（我动哪片数据）与 `OPERATIONS`（我有哪些操作），
 *   本文件只把它们聚合起来 —— 新增工具只需在自己的模块里声明，**不必改 package.json，也不会漏进地图**
 *   （由 `tools/test/cli.test.mjs` 守）。
 * ★ 契约（`docs/00-origin/decisions.md` §7）：本文件**只转发，不实现任何规则**；写操作一律落在各工具自己的写入口里。
 */
import path from 'node:path';
import process from 'node:process';
import { fileURLToPath } from 'node:url';

export const REPO_ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');

/** 域注册表：**只登记模块**；域 id / 数据 / 操作全部从模块自己的声明读 */
const MODULES = ['./corpus.mjs', './fixtures.mjs', './requirements.mjs', './disasm-recode.mjs', './old-repo-inventory.mjs', './opcodes.mjs'];

export async function loadDomains() {
  const out = [];
  for (const m of MODULES) {
    const mod = await import(m);
    if (!mod.DOMAIN || !Array.isArray(mod.OPERATIONS)) {
      throw new Error(`${m} 没有自我声明（缺 DOMAIN / OPERATIONS）—— 见 tools/README.md`);
    }
    out.push({ module: m, mod, ...mod.DOMAIN, operations: mod.OPERATIONS });
  }
  return out;
}

/** 终端里不要 Markdown 记号 */
const plain = (s) => String(s).replace(/\*\*/g, '').replace(/`/g, '');

export function mapJson(domains) {
  return {
    domains: domains.map((d) => ({
      id: d.id,
      title: d.title,
      tool: d.tool,
      data: d.data,
      access: d.access,
      operations: d.operations.map((o) => ({ name: o.name, mutates: o.mutates, summary: o.summary })),
    })),
    note: '地图由各工具的 DOMAIN/OPERATIONS 派生；`pnpm tools <域> <动作> [args]` 转发到对应工具。',
  };
}

export function mapText(domains) {
  const L = [];
  L.push('工具层域地图（由各工具的自我声明派生 —— node tools/cli.mjs）');
  L.push('');
  for (const d of domains) {
    L.push(`${d.id}　${plain(d.title)}`);
    for (const x of d.data) L.push(`  数据    ${plain(x)}`);
    L.push(`  读写    ${plain(d.access)}`);
    L.push(
      `  操作    ${d.operations.map((o) => `${o.name}${o.mutates ? '(会写)' : ''}`).join(' · ')}`,
    );
    L.push(`  例      pnpm tools ${d.id} ${d.operations.find((o) => !o.mutates)?.name ?? d.operations[0].name}`);
    L.push('');
  }
  L.push('用法   pnpm tools <域> <动作> [args…]     ← 位置参数与 flag 直接跟在后面（不必 `--`）');
  L.push('       pnpm tools <域>                    ← 只看该域：数据 / 不变量 / 操作 + 摘要');
  L.push('       pnpm tools --json                  ← 机器可读地图');
  L.push('门禁   pnpm test                           （全仓测试；不属于任何域）');
  return `${L.join('\n')}\n`;
}

const HELP = `tools/cli.mjs — 工具层入口（地图 + 薄转发）

  pnpm tools                              域地图：域 → 数据 → 读写 → 操作
  pnpm tools <域>                         该域详情（数据 / 不变量 / 操作）
  pnpm tools <域> <动作> [args…]           转发到该域工具（例如 pnpm tools corpus validate）
  pnpm tools --json                       机器可读地图
`;

export async function main(argv = process.argv.slice(2)) {
  const args = argv.filter((a) => a !== '');
  const domains = await loadDomains();

  if (args.length === 0 || args[0] === 'list') {
    process.stdout.write(args.includes('--json') ? `${JSON.stringify(mapJson(domains), null, 2)}\n` : mapText(domains));
    return 0;
  }
  if (args[0] === '--json') {
    process.stdout.write(`${JSON.stringify(mapJson(domains), null, 2)}\n`);
    return 0;
  }
  if (args[0] === '--help' || args[0] === '-h') {
    process.stdout.write(HELP);
    return 0;
  }

  const [id, opName, ...rest] = args;
  const d = domains.find((x) => x.id === id);
  if (!d) {
    process.stderr.write(`未知域：${id}\n\n${mapText(domains)}`);
    return 2;
  }
  if (!opName) {
    // 该域详情：优先用它自己的 describe（数据 / 不变量 / 操作）
    process.stdout.write(typeof d.mod.describeText === 'function' ? d.mod.describeText() : mapText([d]));
    return 0;
  }
  const op = d.operations.find((o) => o.name === opName);
  if (!op) {
    process.stderr.write(`域 ${id} 没有动作「${opName}」。可用：${d.operations.map((o) => o.name).join(' / ')}\n`);
    return 2;
  }
  return await d.mod.main([...op.argv, ...rest]);
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
