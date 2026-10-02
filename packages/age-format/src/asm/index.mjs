/**
 * packages/age-format/src/asm/index.mjs —— AGE 脚本汇编器（ASM）的**唯一入口**（只做 re-export）
 *
 * 来源：旧仓 `天結/scripts/asm/`（`age-shared.mjs` + `disassembler.mjs` + `reassembler.mjs` + `opcodes.json`）。
 *
 * 用法：
 * ```js
 * import { disassemble, assemble, defaultCodec } from '../asm/index.mjs';
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
} from './opcodes.mjs';

export {
  decodeCp932, encodeCp932, canEncodeCp932, firstUnencodable,
  decodeUtf16Le, encodeUtf16Le, codecFor, parseCodepage,
  isLeadByte, isTrailByte, isGaijiCodePoint,
  defaultCodec, CP_932, CP_UTF8, CP_936, CP_UTF16,
  GAIJI_LO, GAIJI_HI, GAIJI_LEAD_LO, GAIJI_LEAD_HI,
} from './codec.mjs';

export { disassemble, readHeader, S5_SIG4 } from './disassemble.mjs';
export { assemble, writeHeaderBytes } from './assemble.mjs';

export {
  getTypeLabel, getType, isControlFlowOpcode, isArrayOpcode, isLabelArgument,
  FIELD_OFFSETS, FIELD_NAMES, fieldBlockShift,
  hex, labelHex,
} from './types.mjs';
