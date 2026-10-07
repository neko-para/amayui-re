/**
 * packages/age-format/src/asm/index.mts —— AGE 脚本（ASM）的**工具侧总入口**（只做 re-export）
 *
 * 来源：旧仓 `天結/scripts/asm/`（`age-shared.mjs` + `disassembler.mjs` + `reassembler.mjs` + `opcodes.json`）。
 *
 * 用法：
 * ```js
 * import { disassemble, assemble, OPCODE_TABLE } from '../asm/index.mts';
 * const text = disassemble(fs.readFileSync('SC0000.BIN'));   // Uint8Array(Buffer 是子类) -> string
 * const bin  = assemble(text);                               // string -> Uint8Array
 * bytesEqual(bin, fs.readFileSync('SC0000.BIN'));            // true（往返逐字节相同）
 * ```
 * ★ **本层不碰文件系统**（读写都在调用方，指令表随模块自带）—— 见 `opcodes.mts` 头注。
 *
 * ★ **与 `runtime.mts` 的分工**（原来那条分界线是"谁碰 `node:fs`"，现在 `src/asm/**` **一个 `node:` 都没有**，
 *   所以分界变成了**范围**）：
 *   · `runtime.mts` = **运行期要的那一份**（头部 + 指令表 + 字节原语 + 类型）—— 模拟器核心用它，
 *     于是"反汇编器 / 重汇编器"不会被拖进前端 bundle；
 *   · 本文件 = **全部**（再加上反汇编 / 重汇编 / 码页编解码）—— 给 `tools/**` 与本包 `cli.mjs` 用。
 *
 * 本文件刻意**不含逻辑** —— 免得"到底哪份实现是真的"这种问题出现在同一个包里。
 */
export {
  instructionForOpCode, instructionForLabel, instructionForToken,
  opcodeLabel, normalizedOpcode, instructionByteLength, buildOpcodeTable, OPCODE_TABLE,
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

export { disassemble } from './disassemble.mts';
/** ★ 头部结构单独一个模块（零 Node）—— 见 `header.mts` 头注；模拟器核心只 import 这一份 */
export { readHeader, S5_SIG4, HEADER_LEN_V4, HEADER_LEN_V5 } from './header.mts';
/** ★ 字节源 / 脚本头的类型也从入口再导出（模拟器的迭代系统就用它） */
export type { ByteSource, ByteView, Header, HeaderFields } from './header.mts';
export { assemble, writeHeaderBytes } from './assemble.mts';

/** ★ 字节原语也从入口再导出：消费方要比字节时**不必** `Buffer.from()` 抄一份（见 `bytes.mts` 头注） */
export {
  bytesEqual, concatBytes, copyBytes, decodeLatin1, encodeLatin1, toBytes, ByteReader, ByteWriter,
} from './bytes.mts';

export {
  getTypeLabel, getType, isControlFlowOpcode, isArrayOpcode, isLabelArgument,
  FIELD_OFFSETS, FIELD_NAMES, fieldBlockShift,
  hex, labelHex,
} from './types.mts';
