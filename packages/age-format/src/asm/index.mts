/**
 * packages/age-format/src/asm/index.mjs —— AGE 脚本汇编器（ASM）的**唯一入口**（只做 re-export）
 *
 * 来源：旧仓 `天結/scripts/asm/`（`age-shared.mjs` + `disassembler.mjs` + `reassembler.mjs` + `opcodes.json`）。
 *
 * 用法：
 * ```js
 * import { disassemble, assemble, defaultCodec } from '../asm/index.mts';
 * const text = disassemble(fs.readFileSync('SC0000.BIN'));           // Buffer -> string
 * const bin  = assemble(text);                                      // string -> Buffer
 * bin.equals(fs.readFileSync('SC0000.BIN'));                        // true（往返逐字节相同）
 * ```
 * 两个入口都**不碰文件系统**：读写在调用方。`codec` 缺省 CP932；v5 脚本的字符串区自己走 UTF-16LE。
 *
 * 本文件刻意**不含逻辑** —— 免得"到底哪份实现是真的"这种问题出现在同一个包里。
 */
export {
  loadOpcodeTable, instructionForOpCode, instructionForLabel, instructionForToken,
  opcodeLabel, normalizedOpcode, instructionByteLength, OPCODES_JSON,
} from './opcodes.mts';
/** ★ 类型也从入口再导出：消费方只需要认识 `@amayui/age-format/src/asm/index.mts` 一个门 */
export type { OpcodeDef, OpcodeTable } from './opcodes.mts';

export {
  decodeCp932, encodeCp932, canEncodeCp932, firstUnencodable,
  decodeUtf16Le, encodeUtf16Le, codecFor, parseCodepage,
  isLeadByte, isTrailByte, isGaijiCodePoint,
  defaultCodec, CP_932, CP_UTF8, CP_936, CP_UTF16,
  GAIJI_LO, GAIJI_HI, GAIJI_LEAD_LO, GAIJI_LEAD_HI,
} from './codec.mts';

export { disassemble, readHeader, S5_SIG4 } from './disassemble.mts';
/** ★ 字节源 / 脚本头的类型也从入口再导出（模拟器的迭代系统就用它） */
export type { ByteSource, ByteView, Header, HeaderFields } from './disassemble.mts';
export { assemble, writeHeaderBytes } from './assemble.mts';

export {
  getTypeLabel, getType, isControlFlowOpcode, isArrayOpcode, isLabelArgument,
  FIELD_OFFSETS, FIELD_NAMES, fieldBlockShift,
  hex, labelHex,
} from './types.mts';
