#!/usr/bin/env node
/**
 * tools/opcodes.mjs —— **CLI**：AGE 脚本指令表的对账与派生（唯一写入口）
 *
 * 经派发器：`pnpm tools opcodes <report|derive|describe> [args…]`
 * 也可独立跑：`node tools/opcodes.mjs --report`
 *
 * 模型在 `lib/opcodes.mjs`（口径与"为什么只抽四列"都写在那里）；本文件只做"参数 → 模型 → 输出 + 落盘计划"。
 */
import fs from 'node:fs';
import path from 'node:path';
import process from 'node:process';
import { fileURLToPath } from 'node:url';

import {
  DOMAIN,
  OPERATIONS,
  deriveTo,
  describe,
  describeText,
  loadSourceTable,
  report,
} from './lib/opcodes.mjs';
import { DEFAULT_MANIFEST } from './lib/manifest.mjs';

export { DOMAIN, OPERATIONS, describe, describeText };

const HERE = path.dirname(fileURLToPath(import.meta.url));
const REPO_ROOT = path.resolve(HERE, '..');
/** 派生表落点（消费者：packages/age-format/src/asm/opcodes.mjs 的 `loadOpcodeTable`） */
const DEFAULT_TARGET = path.join(REPO_ROOT, 'packages', 'age-format', 'src', 'asm', 'instruction-set.json');
/**
 * ★ 旧表来源**登记在清单里**（`knowledge/opcode-table-source`），不硬编码旧仓路径：
 *   来源一旦被登记，"换机器 / 旧仓搬家 / 旧仓移除"都只改登记一处；派生器只认登记。
 *   这也是清单的意义 —— 它是 lockfile：**记来源与去向**。
 */
const SOURCE_ENTRY = 'knowledge/opcode-table-source';

const HELP = `tools/opcodes.mjs —— AGE 脚本指令表（登记来源 → 格式层四列的机械派生）

  node tools/opcodes.mjs --report                  # 对账：来源条目 / 旧表条目数 / 将丢弃哪些字段与取值
  node tools/opcodes.mjs --derive [--write]        # 派生并写入（缺省 dry-run）
  node tools/opcodes.mjs --derive --source <文件>   # 换一份来源（诊断用；缺省按清单条目解析）
  node tools/opcodes.mjs --describe                # 自描述：口径 / 字段 / 不变量

★ 来源**登记在清单里**（\`${SOURCE_ENTRY}\`），不硬编码旧仓路径 ⇒ 换机器 / 旧仓搬家 /
  旧仓移除都只改登记一处，派生链不断。
★ 派生只抽四列（opcode / argc / name / aliases）；handler 与 status **留在旧仓**：
  handler 是"从引擎机械复核 argc"的指针、status 是人工自述标签，两者都属知识层
  （见 docs/00-origin/knowledge-rebuild.md；K3 通过前不得进新仓台账）。
`;

function parseArgs(argv) {
  const out = { action: null, write: false, json: false, source: null, target: null };
  const takesValue = new Set(['source', 'target']);
  for (let i = 0; i < argv.length; i += 1) {
    const a = argv[i];
    if (['--report', '--derive', '--describe', '--help', '-h'].includes(a)) out.action = a.replace(/^--?/, '');
    else if (a === '--write') out.write = true;
    else if (a === '--json') out.json = true;
    else if (a.startsWith('--')) {
      const k = a.slice(2);
      if (!takesValue.has(k)) throw new Error(`不认识的选项：${a}`);
      out[k] = argv[++i];
    } else throw new Error(`多余的位置参数：${a}`);
  }
  if (!out.action) out.action = 'report';
  return out;
}

/**
 * 旧表来源的绝对路径：**从清单条目解析**（`knowledge/opcode-table-source` 的 `origin[0]`）。
 * 清单的 `roots.oldRepo` 是唯一写绝对路径的地方（与 corpus 的其它消费者同一口径）。
 */
function sourcePathFromManifest(override) {
  if (override) return path.resolve(override);
  const manifest = JSON.parse(fs.readFileSync(DEFAULT_MANIFEST, 'utf8'));
  const entry = manifest.entries.find((e) => e.id === SOURCE_ENTRY);
  if (!entry) throw new Error(`清单里没有 ${SOURCE_ENTRY} 条目 ⇒ 派生器的来源丢了（这是真错误，不是"跳过"）`);
  const origin = (entry.origin ?? [])[0];
  if (!origin) throw new Error(`${SOURCE_ENTRY} 没有登记 origin`);
  const root = manifest.roots?.[origin.root];
  if (typeof root !== 'string') throw new Error(`${SOURCE_ENTRY} 的 origin.root "${origin.root}" 不在 roots 里`);
  return path.join(root, origin.path);
}

export function main(argv = process.argv.slice(2)) {
  const args = parseArgs(argv);
  if (args.action === 'help') {
    process.stdout.write(HELP);
    return 0;
  }
  if (args.action === 'describe') {
    process.stdout.write(args.json ? `${JSON.stringify(describe(), null, 2)}\n` : describeText());
    return 0;
  }

  const sourceAbs = args.source ? sourcePathFromManifest(args.source) : sourcePathFromManifest(null);
  const targetAbs = path.resolve(args.target ?? DEFAULT_TARGET);
  if (!fs.existsSync(sourceAbs)) {
    process.stderr.write(`来源不在场：${sourceAbs}\n（它登记在清单的 ${SOURCE_ENTRY} 条目里；旧仓是只读来源。缺事实 ⇒ 报错，不"跳过"）\n`);
    return 2;
  }

  const raw = loadSourceTable(sourceAbs);
  if (args.action === 'report') {
    const r = report(raw);
    const manifest = JSON.parse(fs.readFileSync(DEFAULT_MANIFEST, 'utf8'));
    const entry = manifest.entries.find((e) => e.id === SOURCE_ENTRY);
    const wantHash = (entry?.origin ?? [])[0]?.sha256;
    if (args.json) {
      process.stdout.write(`${JSON.stringify({ sourceEntry: SOURCE_ENTRY, sourcePath: sourceAbs, wantHash, ...r }, null, 2)}\n`);
      return 0;
    }
    process.stdout.write(`来源条目    ${SOURCE_ENTRY}\n`);
    process.stdout.write(`来源路径    ${sourceAbs}\n`);
    if (wantHash) process.stdout.write(`登记 sha256 ${wantHash}\n`);
    process.stdout.write(`条目        ${r.entries}\n`);
    process.stdout.write(`字段计数    ${Object.entries(r.fieldCounts).map(([k, v]) => `${k}=${v}`).join(' · ')}\n`);
    process.stdout.write(`格式层保留  ${r.formatFields.join(' / ')}\n`);
    process.stdout.write(`★ 继承观测 ${r.inheritedFields.map((f) => `${f}(${r.fieldCounts[f]} 条)`).join(' / ') || '（无）'}\n`);
    if (r.inheritedFields.includes('handler')) {
      process.stdout.write(`  其中 handler   ${r.handlerCount} 条、${r.distinctHandlers} 个不同引擎函数\n`);
      process.stdout.write(`                ⇒ 与 argc **同类**（都是观测）；格式层不用它，但登记在 knowledge/opcode-handlers\n`);
    }
    process.stdout.write(`★ 丢弃      ${r.droppedFields.map((f) => `${f}(${r.fieldCounts[f]} 条)`).join(' / ') || '（无）'}\n`);
    if (r.droppedFields.includes('status')) {
      process.stdout.write(`  status 取值    ${r.statusValues.map((s) => JSON.stringify(s)).join(' | ')}\n`);
      process.stdout.write(`                ⇒ 人工自述标签（无机械复核路径）；留痕见 knowledge/opcode-status-labels\n`);
    }
    process.stdout.write(`助记符      ${r.namedEntries} 条有名字 / 别名条目 ${r.entriesWithAliases} 条\n`);
    process.stdout.write(`目标        ${targetAbs}\n`);
    return 0;
  }

  if (args.action === 'derive') {
    const r = report(raw);
    const bytes = Buffer.byteLength(JSON.stringify(raw));
    process.stdout.write(`来源        ${sourceAbs}（${r.entries} 条，${bytes} B）\n`);
    process.stdout.write(`丢弃        ${r.droppedFields.join(' / ') || '（无）'}\n`);
    if (!args.write) {
      const preview = deriveTo(raw, path.join(process.env.TEMP ?? '/tmp', `opcodes-preview-${process.pid}.json`));
      process.stdout.write(`派生预览    ${preview.ok ? `${preview.entries} 条 / ${preview.bytes} B` : `失败：${preview.reason}`}\n`);
      process.stdout.write(`\n（dry-run）加 --write 落盘到 ${targetAbs}。\n`);
      return preview.ok ? 0 : 1;
    }
    const res = deriveTo(raw, targetAbs);
    if (!res.ok) {
      process.stderr.write(`派生失败：${res.reason}\n`);
      return 1;
    }
    process.stdout.write(`已写入      ${targetAbs}（${res.entries} 条 / ${res.bytes} B）\n`);
    return 0;
  }

  process.stderr.write(`未知动作：${args.action}\n${HELP}`);
  return 2;
}

const isDirectRun = process.argv[1] && path.resolve(process.argv[1]) === path.resolve(fileURLToPath(import.meta.url));
if (isDirectRun) {
  try {
    process.exitCode = main();
  } catch (err) {
    process.stderr.write(`ERROR: ${err.message}\n`);
    process.exitCode = 2;
  }
}
