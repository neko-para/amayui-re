/**
 * packages/age-format/src/asm/disassemble.mjs —— **AGE 脚本字节码 → 反汇编文本**（纯函数，不碰文件系统）
 *
 * 来源（旧仓只读参照，逐行对照移植）：
 *   · `天結/scripts/asm/disassembler.mjs` —— `disassemble` / `disassembleHeader` /
 *     `disassembleInstruction` / `writeScriptFile`
 *   · `天結/scripts/asm/age-shared.mjs` —— 头部结构（`readHeader` / `writeHeaderBytes`）、
 *     操作数类型标签（`getTypeLabel`）、控制流与 label 判据（`isControlFlowOpcode` / `isLabelArgument`）
 *
 * ## 职责边界
 * 只做**格式层**：字节 ↔ 文本。文本里出现的字段/指令**不代表任何游戏语义结论**，
 * 本文件也不解释它们 —— 反汇编输出只是重汇编的输入（两者必须互逆）。
 *
 * ## 非显然口径
 * 1. **头部长度看签名**：v4（前 4 字节 `SYS4`）头 0x3C；v5（前 4 字节 `53 00 59 00`，即 UTF-16LE `SY`）
 *    头 0x44。两者的数值字段**同在偏移 +8 的 13 个 u32**（v4 从 8 起、v5 从 16 起）。
 * 2. **指令区终点由三张表的最小 offset 决定**：`dataArrayEnd = headerLen + min(table_1/2/3_offset) << 2`；
 *    字符串区与数组块**另外**会把 `dataArrayEnd` 往前压（`Math.min`）。反汇编靠这个边界停，
 *    重汇编则由"文本里有多少行"决定产出多少 —— 两边一致才可能逐字节相同。
 * 3. **字符串是按位取反存的**：v4 每字节 `b ^ 0xFF`，`0xFF` 作终止；v5 每 u16 `c ^ 0xFFFF`，`0xFFFF` 作终止。
 *    v4 的字节串按码页解，v5 的 u16 串**不再过码页**（本身就是 UTF-16LE 码位）。
 * 4. **label 显示成绝对字节偏移**（`0x8C` 这类控制流指令的 label 操作数）：`label_%08x`，
 *    值 = `headerLen + (raw_data << 2)`。注意 label 是**绝对文件偏移**，不是指令序号。
 * 5. **`0x64` 数组指令的第 2 个操作数**（`argc>1`、`type==0`）不是标量而是"数组块引用"：
 *    块在 `headerLen + raw_data*4`，首 u32 是元素个数，随后是元素。它同样会把 `dataArrayEnd` 前压。
 * 6. **本轮不移植 header 里的 delta/probe 机制**（旧仓 Node 版也没有；见旧仓 C++ `disassembler.cpp`
 *    的那条分支）。反汇编输出只保留"重汇编需要的最小集"。
 */
import { instructionForOpCode, opcodeLabel, OPCODE_TABLE } from './opcodes.mts';
import type { OpcodeDef, OpcodeTable } from './opcodes.mts';
import { defaultCodec, CP_932 } from './codec.mts';
import { ByteReader } from './bytes.mts';
import {
  getTypeLabel, isControlFlowOpcode, isLabelArgument, hex, labelHex,
} from './types.mts';
/** ★ 头部结构（含 `readHeader` / 类型）住在 `header.mts`（零 Node）；这里再导出，保持既有 import 路径可用 */
import { readHeader, S5_SIG4, HEADER_LEN_V4, HEADER_LEN_V5 } from './header.mts';
import type { ByteSource, ByteView, Header, HeaderFields } from './header.mts';

export { getType, isArrayOpcode } from './types.mts';
export { readHeader, S5_SIG4, HEADER_LEN_V4, HEADER_LEN_V5 };
export type { ByteSource, ByteView, Header, HeaderFields };

// ───────────────────────────────────────────────────────── 指令区

/** 一条指令的操作数（反汇编会**逐个字段填**，所以这里是可变的） */
export interface InstrArg {
  type: number;
  raw_data: number;
  /** 字符串操作数解出来的文本（v4 过码页 / v5 直接 UTF-16） */
  text?: string;
  /** 原始字节（诊断用） */
  bytes?: Uint8Array;
  /** 数组块（type 2 / opcode `0x64` 的第 2 操作数用） */
  data_array?: { length: number; data: number[] } | null;
}

/** 一条指令（反汇编会往 `args` 里 push，所以数组是**可变**的） */
export interface Instr {
  def: OpcodeDef;
  args: InstrArg[];
  /** 指令起点（**dword 下标**，相对头部末尾） */
  offset: number;
  [k: string]: unknown;
}

// ───────────────────────────────────────────────────────── 文本输出

function disassembleHeader(header: Header): string {
  const { fields } = header;
  let s = '==Binary Information - do not edit==\n';
  s += 'signature = ' + header.signature;
  s += '\nlocal_vars = { ';
  s += hex(fields.local_integer_1) + ' ';
  s += hex(fields.local_floats) + ' ';
  s += hex(fields.local_strings_1) + ' ';
  s += hex(fields.local_integer_2) + ' ';
  s += hex(fields.unknown_data) + ' ';
  s += hex(fields.local_strings_2);
  s += ' }\n';
  s += '====\n\n';
  return s;
}

function disassembleInstruction(header: Header, instr: Instr): string {
  let s = opcodeLabel(instr.def);
  if (instr.args.length > 0) s += ' ';
  let x = 0;
  for (const arg of instr.args) {
    const typeLabel = getTypeLabel(arg.type);
    if (typeLabel !== '') {
      s += '(' + typeLabel + ' ' + hex(arg.raw_data) + ')';
    } else if (arg.type === 2) {
      s += '"' + arg.text + '"';
    } else if (instr.def.opcode === 0x64 && arg.type === 0) {
      s += `[${(arg.data_array?.data ?? []).map(hex).join(' ')}]`;
    } else if (isControlFlowOpcode(instr.def.opcode)) {
      if (isLabelArgument(instr, x)) {
        s += 'label_' + labelHex(header.length + (arg.raw_data << 2));
      } else {
        s += hex(arg.raw_data);
      }
    } else {
      s += hex(arg.raw_data);
    }
    if (x < instr.args.length - 1) s += ' ';
    x++;
  }
  s += '\n';
  return s;
}

function writeScriptFile(header: Header, instructions: readonly Instr[]): string {
  const labels = new Set();
  for (const instr of instructions) {
    if (isControlFlowOpcode(instr.def.opcode)) {
      let x = 0;
      for (const arg of instr.args) {
        if (isLabelArgument(instr, x)) labels.add(arg.raw_data);
        x++;
      }
    }
  }
  let out = disassembleHeader(header);
  for (const instr of instructions) {
    if (labels.has(instr.offset)) {
      out += '\nlabel_' + labelHex(header.length + (instr.offset << 2)) + '\n';
    }
    out += disassembleInstruction(header, instr);
  }
  return out;
}

// ───────────────────────────────────────────────────────── 入口

/**
 * AGE 脚本字节码 → 反汇编文本（UTF-8 字符串）。
 *
 * `bin` 是脚本字节码；`opts.codec` 缺省 CP932（`codec.mjs` 的 `defaultCodec`），`opts.table` 缺省加载指令表。
 * `opts.strict`（缺省 true）= 字符串区读越界时抛错（旧仓 Node 版会由 Buffer 抛 `RangeError`，
 * 这里显式抛同义错误）；设 false 则尽量反汇编、越界处填 `U+FFFD` 并把它记进 `opts.report`（诊断用）。
 */
/** `disassemble` 的选项（只声明**本函数真正读**的那几个键） */
export interface DisassembleOptions {
  /** 码页编解码器；缺省 `codec.mts` 的 `defaultCodec`（CP932） */
  codec?: { decode(bytes: Uint8Array): string } | null;
  /** 指令表；缺省用本包自带的 `OPCODE_TABLE` */
  table?: OpcodeTable;
  /** true（缺省）= 字符串区越界即抛；false = 尽量反汇编、越界处填 `U+FFFD` 并记进 `report` */
  strict?: boolean;
  /** 诊断收集器（`strict:false` 时越界项往里塞） */
  report?: { push(item: unknown): void; outOfRange?: number };
}

export function disassemble(bin: Uint8Array, { codec = defaultCodec, table, strict = true, report }: DisassembleOptions = {}): string {
  if (!(bin instanceof Uint8Array)) throw new Error('disassemble: bin 必须是 Uint8Array（Buffer 是它的子类）');
  const rd = new ByteReader(bin);
  const tbl = table || OPCODE_TABLE;
  const header = readHeader(bin);
  const { fields } = header;
  const headerLen = header.length;
  const cp = (codec ?? defaultCodec) as { decode(bytes: Uint8Array): string };

  /** 字符串区越界读取：严格模式抛错，宽松模式记一笔并产出 `U+FFFD`/0 */
  const oob = (what: string, offset: number): void => {
    if (strict) {
      throw new Error(
        `${what} 越界：偏移 0x${offset.toString(16)} 已在文件外（文件 ${bin.length} 字节）` +
        ` —— 该脚本的字符串区/表被截断，或表 offset 与文件长度不自洽`
      );
    }
    if (report) report.outOfRange = (report.outOfRange || 0) + 1;
  };

  const minTableOffset = Math.min(fields.table_1_offset, fields.table_2_offset, fields.table_3_offset);
  let dataArrayEnd = headerLen + (minTableOffset << 2);
  if (dataArrayEnd > bin.length) {
    oob('三张表的 offset 指向的指令区终点', dataArrayEnd - 1);
    dataArrayEnd = Math.min(dataArrayEnd, bin.length);
  }

  const instructions = [];
  let pos = headerLen;

  while (pos < dataArrayEnd) {
    const byteOffset = pos;
    if (pos + 4 > bin.length) { oob('指令 opcode', pos); break; }
    const opCode = rd.u32(pos);
    pos += 4;
    if (opCode === 0x0) throw new Error(`Offset 0x${byteOffset.toString(16)} bad opcode : 0`);

    const def = instructionForOpCode(tbl, opCode);
    if (!def) throw new Error(`Unknown instruction : 0x${opCode.toString(16)} at 0x${byteOffset.toString(16)}`);
    if (def.argc === null) throw new Error(`Unknown argc for opcode 0x${opCode.toString(16)} at 0x${byteOffset.toString(16)}`);

    const instr: Instr = { def, args: [] as InstrArg[], offset: (byteOffset - headerLen) >> 2 };

    for (let current = 0; current < def.argc; current++) {
      if (pos + 8 > bin.length) { oob(`指令 0x${opCode.toString(16)} 的操作数 ${current}`, pos); break; }
      const type = rd.u32(pos); pos += 4;
      const rawData = rd.u32(pos); pos += 4;
      const arg: InstrArg = { type, raw_data: rawData, text: undefined, bytes: undefined, data_array: null };

      if (type === 2) {
        const stringOffset = headerLen + (rawData << 2);
        dataArrayEnd = Math.min(dataArrayEnd, stringOffset);
        const curOff = pos;
        if (header.isVer5) {
          // v5：u16 序列按位取反，0xFFFF 终止；本身已是 UTF-16LE 码位（不过码页）
          const utf16 = [];
          let p = stringOffset;
          for (;;) {
            if (p + 2 > bin.length) { oob('v5 字符串', p); break; }
            const ch = rd.u16(p); p += 2;
            if (ch === 0xffff) break;
            utf16.push(ch ^ 0xffff);
          }
          arg.text = String.fromCharCode(...utf16);
        } else {
          // v4：字节按位取反（^0xFF），0xFF 终止，再按脚本码页解 —— 一次切出再整体解码
          const term = bin.indexOf(0xff, stringOffset);
          if (term < 0) {
            oob('v4 字符串终止符 0xFF', bin.length);
            arg.text = cp.decode(invertBytes(bin.subarray(stringOffset)));
          } else {
            arg.text = cp.decode(invertBytes(bin.subarray(stringOffset, term)));
          }
        }
        pos = curOff;
      } else if (def.opcode === 0x64 && current === 1) {
        const arrayOffset = headerLen + (rawData << 2);
        dataArrayEnd = Math.min(dataArrayEnd, arrayOffset);
        const curOff = pos;
        if (arrayOffset + 4 > bin.length) {
          oob('数组块头', arrayOffset);
          arg.data_array = { length: 0, data: [] };
        } else {
          const length = rd.u32(arrayOffset);
          const data = [];
          for (let i = 0; i < length; i++) {
            const at = arrayOffset + 4 + i * 4;
            if (at + 4 > bin.length) { oob('数组块元素', at); break; }
            data.push(rd.u32(at));
          }
          arg.data_array = { length, data };
        }
        pos = curOff;
      }

      if (type < 0 || (type > 0xe && type < 0x8003) || type > 0x800b) {
        throw new Error(
          `Pos : ${pos.toString(16)} -> Opcode : ${def.opcode.toString(16)}, argument ${current}\n` +
          `Unknown type : ${type.toString(16)}\nValue : ${rawData.toString(16)}`
        );
      }
      instr.args.push(arg);
    }
    instructions.push(instr);
  }

  return writeScriptFile(header, instructions);
}

/** `b ^ 0xFF` 整段取反（不就地改输入） */
function invertBytes(src: Uint8Array): Uint8Array {
  const out = new Uint8Array(src.length);
  for (let i = 0; i < src.length; i++) out[i] = src[i] ^ 0xff;
  return out;
}

export { CP_932 };
