/**
 * CLI：只读 `data/spec` 的参考实现。
 *   node apps/emulator-new/src/main.ts --script <file> [--decode] [--json] [--strict]
 *
 * 默认模式：按指令表切指令，遇到**没有实现**的 opcode 就报缺口（未实现即抛错）。
 * `--decode`：只解码走查（不执行），用于检验 CORE-ISA 的结构规则。
 */
import fs from 'node:fs';

import { loadOpcodeTable } from './spec.ts';
import { codeLimit, parseHeader, walk } from './decode.ts';
import { HANDLERS } from './handlers.ts';

interface Args {
  script: string | null;
  decodeOnly: boolean;
  json: boolean;
  strict: boolean;
}

function parseArgs(argv: string[]): Args {
  const out: Args = { script: null, decodeOnly: false, json: false, strict: false };
  for (let i = 0; i < argv.length; i += 1) {
    const a = argv[i];
    if (a === '--script') out.script = argv[++i] ?? null;
    else if (a === '--decode') out.decodeOnly = true;
    else if (a === '--json') out.json = true;
    else if (a === '--strict') out.strict = true;
    else throw new Error(`unknown argument: ${a}`);
  }
  return out;
}

export function analyze(file: string, opts: { decodeOnly?: boolean } = {}) {
  const buf = fs.readFileSync(file);
  const table = loadOpcodeTable();
  const header = parseHeader(buf);
  const body = buf.subarray(header.headerLength);
  const limit = codeLimit(header, body.length);
  const walked = walk(body, table, limit);
  const firstUnimplemented = opts.decodeOnly ? null : (walked.instructions.find((x) => !HANDLERS.has(x.opcode)) ?? null);
  const runGap = firstUnimplemented
    ? {
        index: firstUnimplemented.index,
        offset: firstUnimplemented.offset,
        opcode: firstUnimplemented.opcode,
        argc: firstUnimplemented.argc,
        reason: 'unimplemented',
      }
    : null;
  return {
    file,
    signature: header.signature,
    version: header.version,
    headerLength: header.headerLength,
    tables: header.tables,
    bodyLength: body.length,
    codeLimit: limit,
    decoded: walked.instructions.length,
    stop: walked.gap ? walked.gap.reason : walked.stop,
    gap: opts.decodeOnly ? walked.gap : runGap,
    mode: opts.decodeOnly ? 'decode' : 'run',
  };
}

function main() {
  const args = parseArgs(process.argv.slice(2));
  if (!args.script) {
    console.error('用法：node apps/emulator-new/src/main.ts --script <file> [--decode] [--json] [--strict]');
    process.exit(2);
  }
  const result = analyze(args.script, { decodeOnly: args.decodeOnly });
  if (args.json) {
    console.log(JSON.stringify(result, null, 2));
  } else {
    console.log(`文件        ${result.file}`);
    console.log(`头          ${result.signature} ${result.version.trim()} · 头长 ${result.headerLength} · 表 ${result.tables.length} 组`);
    console.log(`body        ${result.bodyLength} 字节 · 代码区上限 ${result.codeLimit}`);
    console.log(`解码        ${result.decoded} 条 · 停因 ${result.stop}`);
    if (result.gap) {
      console.log(`缺口        ${result.mode === 'decode' ? '解码' : '未实现'} @ 第 ${result.gap.index} 条 · 偏移 ${result.gap.offset} · opcode 0x${result.gap.opcode.toString(16)}${result.gap.argc === null ? '' : ` · argc ${result.gap.argc}`} · ${result.gap.reason}`);
    } else {
      console.log('缺口        无');
    }
  }
  if (args.strict && result.gap) process.exit(1);
}

if (process.argv[1] && process.argv[1].endsWith('main.ts')) main();
