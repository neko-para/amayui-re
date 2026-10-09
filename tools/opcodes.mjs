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
  DEFAULT_HANDLER,
  DISPATCH_BASE,
  DOMAIN,
  OPERATIONS,
  deriveTo,
  describe,
  describeText,
  extractHandlers,
  listingPath,
  loadSourceTable,
  report,
} from './lib/opcodes.mjs';
import { DEFAULT_MANIFEST, loadManifest } from './lib/manifest.mjs';

export { DOMAIN, OPERATIONS, describe, describeText };

const HERE = path.dirname(fileURLToPath(import.meta.url));
const REPO_ROOT = path.resolve(HERE, '..');
/** 派生表落点（消费者：packages/age-format/src/asm/opcodes.mts 的 `OPCODE_TABLE` —— ESM JSON import，不走 fs） */
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
  node tools/opcodes.mjs --handlers                # ★ opcode → handler 的机械查询（从语料分派表提取）
  node tools/opcodes.mjs --handlers --opcode 0x1a8 # 只查一条
  node tools/opcodes.mjs --handlers --gaps         # 指令表里有、而分派表里没有的（⇒ 引擎走缺省 = 抛"不支持"）
  node tools/opcodes.mjs --describe                # 自描述：口径 / 字段 / 不变量

★ 来源**登记在清单里**（\`${SOURCE_ENTRY}\`），不硬编码旧仓路径 ⇒ 换机器 / 旧仓搬家 /
  旧仓移除都只改登记一处，派生链不断。
★ 派生只抽四列（opcode / argc / name / aliases）；handler 与 status **留在旧仓**：
  handler 是"从引擎机械复核 argc"的指针、status 是人工自述标签，两者都属知识层
  （见 docs/00-origin/knowledge-rebuild.md；K3 通过前不得进新仓台账）。
★ \`--handlers\` 与上面那条**不矛盾**：它不复述旧仓的 handler 列，而是**从本仓语料重新提取**一遍
  （分派表基址 \`Engine+0xA509C\`，\`opcode = (表项偏移 − 基址)/4\`）—— 这正是"argc 的权威来自引擎"
  那条口径的第一步：先能机械点名 handler，才谈得上按 handler 体复核 argc。
`;

/** 语料侧的口径与提取逻辑都在模型里（见 `lib/opcodes.mjs` 的「语料侧」一节） */
export { DISPATCH_BASE, DEFAULT_HANDLER, extractHandlers, listingPath };

function parseArgs(argv) {
  const out = { action: null, write: false, json: false, source: null, target: null, opcode: null, gaps: false };
  const takesValue = new Set(['source', 'target', 'opcode']);
  for (let i = 0; i < argv.length; i += 1) {
    const a = argv[i];
    if (['--report', '--derive', '--handlers', '--describe', '--help', '-h'].includes(a)) out.action = a.replace(/^--?/, '');
    else if (a === '--write') out.write = true;
    else if (a === '--json') out.json = true;
    else if (a === '--gaps') out.gaps = true;
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
 * `--handlers`：**从语料重新提取** opcode → handler（不复述旧仓的 handler 列）。
 * 三种用法：默认全表 · `--opcode N` 查一条 · `--gaps` 只列"指令表里有、而分派表里没有"的那些
 * （那些走**缺省 handler** = 引擎会抛「このコマンドはサポートされていません．」⇒ 这是实现清单）。
 */
function handlersAction(args) {
  const lst = listingPath(REPO_ROOT);
  if (!lst) {
    process.stderr.write('反汇编语料不在场：corpus/disasm/files/*.lst 不存在（先 `pnpm tools disasm build`）\n' +
      '★ 缺事实 ⇒ 报错，不"跳过" —— 这条查询的**全部意义**就是"回语料核一遍"。\n');
    return 2;
  }
  const { byOpcode, rows, problems } = extractHandlers(fs.readFileSync(lst, 'utf8'));
  if (problems.length) {
    process.stderr.write(`分派表结构有问题（${problems.length} 条），拒绝给出结论：\n${problems.slice(0, 8).map((p) => `  · ${p}`).join('\n')}\n`);
    return 1;
  }
  const table = JSON.parse(fs.readFileSync(DEFAULT_TARGET, 'utf8'));
  const nameOf = (op) => table.find((e) => e.opcode === op)?.name ?? '';
  const gaps = table.map((e) => e.opcode).filter((op) => op > 0 && !byOpcode.has(op)).sort((a, b) => a - b);

  if (args.opcode !== null) {
    const op = Number(args.opcode);
    if (!Number.isInteger(op) || op <= 0) throw new Error(`--opcode 要一个正整数（收了 ${args.opcode}）`);
    const sym = byOpcode.get(op);
    if (args.json) {
      process.stdout.write(`${JSON.stringify({ opcode: op, name: nameOf(op), handler: sym ?? DEFAULT_HANDLER, isDefault: !sym, tableOffset: `0x${(DISPATCH_BASE + op * 4).toString(16)}` }, null, 2)}\n`);
      return 0;
    }
    process.stdout.write(`opcode      0x${op.toString(16)}（${op}）${nameOf(op) ? ` ${nameOf(op)}` : ''}\n`);
    process.stdout.write(`表项偏移    0x${(DISPATCH_BASE + op * 4).toString(16)}（= 0x${DISPATCH_BASE.toString(16)} + ${op}*4）\n`);
    process.stdout.write(`handler     ${sym ?? `${DEFAULT_HANDLER} ← **缺省**（分派表里没有这一条 ⇒ 引擎抛「不支持」）`}\n`);
    return 0;
  }

  if (args.gaps) {
    if (args.json) { process.stdout.write(`${JSON.stringify({ gaps }, null, 2)}\n`); return 0; }
    process.stdout.write(`指令表里有、而分派表里没有的 opcode：${gaps.length} 个（这些走缺省 handler = 引擎抛「不支持」）\n`);
    for (const op of gaps) process.stdout.write(`  0x${op.toString(16).padStart(4, '0')}  ${nameOf(op) || '(无名)'}\n`);
    return 0;
  }

  if (args.json) {
    process.stdout.write(`${JSON.stringify({ listing: path.relative(REPO_ROOT, lst), base: `0x${DISPATCH_BASE.toString(16)}`, defaultHandler: DEFAULT_HANDLER, assignments: rows, handlers: [...byOpcode.entries()].sort((a, b) => a[0] - b[0]), gaps }, null, 2)}\n`);
    return 0;
  }
  process.stdout.write(`语料        ${path.relative(REPO_ROOT, lst)}\n`);
  process.stdout.write(`表基址      0x${DISPATCH_BASE.toString(16)}　判据 opcode = (表项偏移 − 基址)/4\n`);
  process.stdout.write(`赋值行      ${rows} 条 ⇒ 有 handler 的 opcode ${byOpcode.size} 个；指令表 ${table.length} 条里 ${gaps.length} 个走缺省\n`);
  process.stdout.write('opcode  助记符          handler\n');
  for (const [op, sym] of [...byOpcode.entries()].sort((a, b) => a[0] - b[0])) {
    process.stdout.write(`  0x${op.toString(16).padStart(4, '0')}  ${(nameOf(op) || '(无名)').padEnd(14)} ${sym}\n`);
  }
  return 0;
}

/**
 * 旧表来源的绝对路径：**从清单条目解析**（`knowledge/opcode-table-source` 的 `origin[0]`）。
 * 清单的 `roots.oldRepo` 是唯一写绝对路径的地方（与 corpus 的其它消费者同一口径）。
 */
function sourcePathFromManifest(override) {
  if (override) return path.resolve(override);
  const manifest = loadManifest(DEFAULT_MANIFEST);
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
  // ★ `handlers` 不依赖旧表来源 ⇒ 放在"来源必须存在"那道检查**之前**
  if (args.action === 'handlers') return handlersAction(args);

  const sourceAbs = args.source ? sourcePathFromManifest(args.source) : sourcePathFromManifest(null);
  const targetAbs = path.resolve(args.target ?? DEFAULT_TARGET);
  if (!fs.existsSync(sourceAbs)) {
    process.stderr.write(`来源不在场：${sourceAbs}\n（它登记在清单的 ${SOURCE_ENTRY} 条目里；旧仓是只读来源。缺事实 ⇒ 报错，不"跳过"）\n`);
    return 2;
  }

  const raw = loadSourceTable(sourceAbs);
  if (args.action === 'report') {
    const r = report(raw);
    const manifest = loadManifest(DEFAULT_MANIFEST);
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
