/**
 * 脚本头与指令切分（CORE-ISA 的可执行投影）。
 * ★ 规则来自 `data/spec/chapters/10-format.md`；这里不引用任何分析产物。
 */
import { decodeLatin1, readU32LE } from './bytes.ts';
import type { OpcodeRow } from './spec.ts';

const FIXED = 32;

export interface Header {
  signature: string;
  version: string;
  counts: number[];
  subHeaderLength: number;
  headerLength: number;
  tables: { len: number; off: number }[];
}

export interface Instruction {
  index: number;
  offset: number;
  opcode: number;
  argc: number;
  operands: { type: number; raw: number }[];
}

export interface Gap {
  index: number;
  offset: number;
  opcode: number;
  argc: number | null;
  reason: string;
}

export function parseHeader(buf: Uint8Array): Header {
  if (buf.length < FIXED) throw new Error('file-too-small');
  const signature = decodeLatin1(buf, 0, 4);
  if (signature !== 'SYS4' && signature !== 'SYS3') throw new Error(`bad-signature:${signature}`);
  const version = decodeLatin1(buf, 4, 8);
  const counts: number[] = [];
  for (let i = 0; i < 6; i += 1) counts.push(readU32LE(buf, 8 + 4 * i));
  // 版本串 < "080 " ⇒ 老布局：没有子头，头长 32（与装载器一致）
  const subHeaderLength = version < '080 ' ? 0 : readU32LE(buf, FIXED);
  if (subHeaderLength !== 0 && (subHeaderLength < 4 || subHeaderLength % 4 !== 0)) {
    throw new Error(`bad-subheader-length:${subHeaderLength}`);
  }
  const headerLength = FIXED + subHeaderLength;
  if (headerLength > buf.length) throw new Error('header-past-eof');
  const words: number[] = [];
  for (let i = 0; i < (subHeaderLength - 4) / 4; i += 1) words.push(readU32LE(buf, FIXED + 4 + 4 * i));
  const tables: { len: number; off: number }[] = [];
  for (let i = 0; i + 1 < words.length; i += 2) tables.push({ len: words[i], off: words[i + 1] });
  return { signature, version, counts, subHeaderLength, headerLength, tables };
}

/** 代码区起点（body 的第 0 字节）；三张表在 body 尾部，代码区是表之前的字节 */
export function codeLimit(header: Header, bodyLength: number): number {
  const first = header.tables[0];
  const bytes = first ? first.off * 4 : bodyLength;
  return bytes > bodyLength ? bodyLength : bytes;
}

/**
 * 直线解码：按 `4 + 8 × argc` 切指令，直到越界 / 未知 opcode / 到达代码区上限。
 * ★ 这不是"执行"：指令流是否走进数据取决于控制流，直线解码只是结构走查（见 CORE-ISA-EXEC-END）。
 */
export function walk(body: Uint8Array, table: Map<number, OpcodeRow>, limit: number, max = 1_000_000) {
  const instructions: Instruction[] = [];
  let p = 0;
  for (let index = 0; index < max; index += 1) {
    if (p + 4 > limit) return { instructions, gap: null as Gap | null, stop: 'code-region-end' };
    const opcode = readU32LE(body, p);
    const row = table.get(opcode);
    if (!row) return { instructions, gap: { index, offset: p, opcode, argc: null, reason: 'opcode-not-in-table' }, stop: 'unknown-opcode' };
    const len = 4 + 8 * row.argc;
    if (p + len > limit) return { instructions, gap: { index, offset: p, opcode, argc: row.argc, reason: 'truncated' }, stop: 'truncated' };
    const operands: { type: number; raw: number }[] = [];
    for (let k = 0; k < row.argc; k += 1) {
      operands.push({ type: readU32LE(body, p + 4 + 8 * k), raw: readU32LE(body, p + 8 + 8 * k) });
    }
    instructions.push({ index, offset: p, opcode, argc: row.argc, operands });
    p += len;
  }
  return { instructions, gap: null as Gap | null, stop: 'instruction-limit' };
}
