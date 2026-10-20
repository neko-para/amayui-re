/**
 * CLI：只读 `data/spec` 的参考实现。
 *
 *   # 真实入口：安装目录 → SYS4INI.BIN（ALF 索引）→ 统一文件 id → 松散文件 / 归档切片
 *   node apps/emulator-new/src/main.ts --install <dir> --key <u32> [--root-id 0] [--stop-at <opcode>]
 *
 *   # 调试入口：直接给一份脚本文件
 *   node apps/emulator-new/src/main.ts --script <file> --key <u32> [--stop-at <opcode>]
 *   node apps/emulator-new/src/main.ts --install <dir> --decode [--json]
 *
 * 默认模式：**执行**。遇到没有实现的 opcode 就停在它前面并报缺口（未实现即抛错，不 no-op）。
 * `--decode`：只解码走查（不执行），用于检验 CORE-ISA 的结构规则。
 * ★ `--key` 是整型编码的运行期 key（规范：不是常量、不许有默认值）⇒ 执行模式必须显式给。
 */
import { Install, readFile } from './host.ts';
import { loadOpcodeTable } from './spec.ts';
import { codeLimit, parseHeader, walk } from './decode.ts';
import type { Gap } from './decode.ts';
import { HANDLERS } from './handlers.ts';
import { Machine } from './machine.ts';
import type { Diagnostic, Stop } from './machine.ts';

/** 一份脚本的"结构 + 执行"报告（decode / run / 缺 key 三种形态共用；可选字段由 mode 决定） */
export interface ScriptReport {
  bytes: number;
  signature: string;
  version: string;
  headerLength: number;
  tables: { len: number; off: number }[];
  bodyLength: number;
  codeLimit: number;
  decoded: number;
  mode: 'decode' | 'run';
  stop: string;
  gap: Gap | null;
  key?: string | null;
  steps?: number;
  ticks?: number;
  atMs?: number;
  stopText?: string;
  executed?: number;
  diagnostics?: Diagnostic[];
}

interface Args {
  install: string | null;
  script: string | null;
  rootId: number;
  decodeOnly: boolean;
  json: boolean;
  strict: boolean;
  key: number | null;
  stopAt: number | null;
  tickMs: number;
  uninitializedFloat: 'throw' | 'zero';
}

interface RunOpts {
  decodeOnly?: boolean;
  key?: number;
  stopAtOpcode?: number;
  maxSteps?: number;
  maxTicks?: number;
  tickMs?: number;
  uninitializedFloat?: 'throw' | 'zero';
}

function parseU32(text: string, what: string): number {
  const v = /^0x/i.test(text) ? Number.parseInt(text.slice(2), 16) : Number.parseInt(text, 10);
  if (!Number.isInteger(v) || v < 0 || v > 0xffffffff) throw new Error(`${what} 必须是 u32：${text}`);
  return v >>> 0;
}

function parseArgs(argv: string[]): Args {
  const out: Args = {
    install: null, script: null, rootId: 0, decodeOnly: false, json: false, strict: false,
    key: null, stopAt: null, tickMs: 16, uninitializedFloat: 'throw',
  };
  for (let i = 0; i < argv.length; i += 1) {
    const a = argv[i];
    if (a === '--install') out.install = argv[++i] ?? null;
    else if (a === '--script') out.script = argv[++i] ?? null;
    else if (a === '--root-id') out.rootId = parseU32(argv[++i] ?? '', '--root-id');
    else if (a === '--decode') out.decodeOnly = true;
    else if (a === '--json') out.json = true;
    else if (a === '--strict') out.strict = true;
    else if (a === '--key') out.key = parseU32(argv[++i] ?? '', '--key');
    else if (a === '--stop-at') out.stopAt = parseU32(argv[++i] ?? '', '--stop-at');
    else if (a === '--tick-ms') out.tickMs = parseU32(argv[++i] ?? '', '--tick-ms');
    else if (a === '--uninitialized-float') {
      const v = argv[++i];
      if (v !== 'throw' && v !== 'zero') throw new Error('--uninitialized-float 只能是 throw / zero');
      out.uninitializedFloat = v;
    } else throw new Error(`unknown argument: ${a}`);
  }
  if (out.install && out.script) throw new Error('--install 与 --script 只能给一个');
  return out;
}

function describeStop(stop: Stop): string {
  switch (stop.kind) {
    case 'exit':
      return '退出';
    case 'stop-opcode':
      return `命中停止条件 opcode 0x${stop.opcode.toString(16)}（第 ${stop.index} 条）`;
    case 'unimplemented':
      return `未实现 opcode 0x${stop.opcode.toString(16)}（第 ${stop.index} 条 · 偏移 ${stop.offset} · argc ${stop.argc}）`;
    case 'unknown-opcode':
      return `指令表里没有 opcode 0x${stop.opcode.toString(16)}（第 ${stop.index} 条 · 偏移 ${stop.offset}）`;
    case 'truncated':
      return `指令越界（第 ${stop.index} 条 · 偏移 ${stop.offset}）`;
    case 'step-limit':
      return `步数上限（第 ${stop.index} 步）`;
    case 'tick-limit':
      return `时钟上限（第 ${stop.index} 步）`;
  }
}

/** 执行 / 解码一份脚本字节（入口无关） */
export function analyzeBytes(buf: Buffer, opts: RunOpts = {}): ScriptReport {
  const table = loadOpcodeTable();
  const header = parseHeader(buf);
  const body = buf.subarray(header.headerLength);
  const limit = codeLimit(header, body.length);
  const walked = walk(body, table, limit);
  const base = {
    bytes: buf.length,
    signature: header.signature,
    version: header.version,
    headerLength: header.headerLength,
    tables: header.tables,
    bodyLength: body.length,
    codeLimit: limit,
    decoded: walked.instructions.length,
  };

  if (opts.decodeOnly) {
    return {
      ...base,
      mode: 'decode',
      stop: walked.gap ? walked.gap.reason : walked.stop,
      gap: walked.gap,
    };
  }

  if (opts.key === undefined) {
    // 不执行就不需要 key；但也不许把它当成"执行成功"
    const firstUnimplemented = walked.instructions.find((x) => !HANDLERS.has(x.opcode)) ?? null;
    return {
      ...base,
      mode: 'run',
      key: null,
      steps: 0,
      stop: 'missing-key',
      gap: firstUnimplemented
        ? { index: firstUnimplemented.index, offset: firstUnimplemented.offset, opcode: firstUnimplemented.opcode, argc: firstUnimplemented.argc, reason: 'unimplemented' }
        : null,
    };
  }

  const machine = new Machine({
    body, table, key: opts.key, tickMs: opts.tickMs, uninitializedFloat: opts.uninitializedFloat,
  });
  const r = machine.run({ stopAtOpcode: opts.stopAtOpcode, maxSteps: opts.maxSteps, maxTicks: opts.maxTicks });
  return {
    ...base,
    mode: 'run',
    key: `0x${opts.key.toString(16)}`,
    steps: r.steps,
    ticks: machine.ticks,
    atMs: machine.now,
    stop: r.stop.kind,
    stopText: describeStop(r.stop),
    executed: r.ip,
    diagnostics: machine.diagnostics,
    gap:
      r.stop.kind === 'unimplemented'
        ? { index: r.stop.index, offset: r.stop.offset, opcode: r.stop.opcode, argc: r.stop.argc, reason: 'unimplemented' }
        : null,
  };
}

/** 调试入口：直接读一份脚本文件 */
export function analyze(file: string, opts: RunOpts = {}): ScriptReport & { source: Record<string, unknown> } {
  return { source: { kind: 'script', file }, ...analyzeBytes(readFile(file), opts) };
}

/** 真实入口：安装目录 → 索引 → 统一文件 id → 松散文件 / 归档切片 */
export function analyzeInstall(dir: string, rootId: number, opts: RunOpts = {}): ScriptReport & { source: Record<string, unknown> } {
  const install = Install.open(dir);
  const script = install.readById(rootId);
  const result = analyzeBytes(script.bytes, opts);
  return {
    source: {
      kind: 'install',
      dir,
      index: install.indexFile,
      indexSignature: install.index.signature,
      indexEntries: install.entryCount,
      archives: install.index.archives,
      id: script.id,
      name: script.name,
      side: script.side,
      entryLength: script.entryLength,
      actualLength: script.bytes.length,
      archive: script.archive,
    },
    ...result,
  };
}

function main() {
  const args = parseArgs(process.argv.slice(2));
  const usage = [
    '用法：node apps/emulator-new/src/main.ts --install <dir> --key <u32> [--root-id 0] [--stop-at <opcode>] [--tick-ms N] [--uninitialized-float throw|zero] [--json] [--strict]',
    '      node apps/emulator-new/src/main.ts --script <file> --key <u32> [--stop-at <opcode>] [--json] [--strict]',
    '      node apps/emulator-new/src/main.ts --install <dir> --decode [--json]',
  ].join('\n');
  if (!args.install && !args.script) {
    console.error(usage);
    process.exit(2);
  }
  const opts: RunOpts = {
    decodeOnly: args.decodeOnly,
    key: args.key ?? undefined,
    stopAtOpcode: args.stopAt ?? undefined,
    tickMs: args.tickMs,
    uninitializedFloat: args.uninitializedFloat,
  };
  const result = args.install
    ? analyzeInstall(args.install, args.rootId, opts)
    : analyze(args.script as string, opts);

  if (args.json) {
    console.log(JSON.stringify(result, null, 2));
  } else {
    const src = result.source as Record<string, unknown>;
    if (src.kind === 'install') {
      console.log(`安装        ${src.dir}`);
      console.log(`索引        ${src.indexSignature} · ${(src.archives as string[]).length} 个归档 · ${src.indexEntries} 条目`);
      console.log(`脚本        id ${src.id} · ${src.name} · ${src.side === 'loose' ? '松散文件' : `归档 ${src.archive}`} · ${src.actualLength} 字节（TOC 登记 ${src.entryLength}）`);
    } else {
      console.log(`文件        ${src.file}`);
    }
    console.log(`头          ${result.signature} ${result.version.trim()} · 头长 ${result.headerLength} · 表 ${result.tables.length} 组`);
    console.log(`body        ${result.bodyLength} 字节 · 代码区上限 ${result.codeLimit}`);
    if (result.mode === 'decode') {
      console.log(`解码        ${result.decoded} 条 · 停因 ${result.stop}`);
    } else if ('stopText' in result) {
      console.log(`执行        ${result.steps} 条 · 停因 ${result.stopText}`);
      console.log(`时钟        ${result.ticks} 格 · ${result.atMs}ms · 诊断 ${result.diagnostics?.length ?? 0} 条`);
    } else {
      console.log(`执行        未启动（缺 --key）· 缺口 ${result.gap ? `opcode 0x${result.gap.opcode.toString(16)}` : '无'}`);
    }
    if (result.gap) {
      console.log(`缺口        ${result.mode === 'decode' ? '解码' : '未实现'} @ 第 ${result.gap.index} 条 · 偏移 ${result.gap.offset} · opcode 0x${result.gap.opcode.toString(16)}${result.gap.argc === null ? '' : ` · argc ${result.gap.argc}`} · ${result.gap.reason}`);
    } else {
      console.log('缺口        无');
    }
  }
  if (args.strict && (result.gap || result.stop === 'missing-key')) process.exit(1);
}

if (process.argv[1] && process.argv[1].endsWith('main.ts')) main();
