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
import { loadOpcodeTable, instructionForOpCode, opcodeLabel } from './opcodes.mjs';
import { defaultCodec, CP_932 } from './codec.mjs';
import {
  getTypeLabel, isControlFlowOpcode, isLabelArgument, hex, labelHex,
  FIELD_NAMES, FIELD_OFFSETS, fieldBlockShift,
} from './types.mjs';

export { getType, isArrayOpcode } from './types.mjs';

// ───────────────────────────────────────────────────────── 头部结构

const S4_SIG = Buffer.from('SYS4', 'latin1');
/** v5 签名前 4 字节 = UTF-16LE 的 "SY" */
export const S5_SIG4 = Buffer.from([0x53, 0x00, 0x59, 0x00]);

const HEADER_LEN_V4 = 0x3c;
const HEADER_LEN_V5 = 0x44;

/** 从签名起点读 13 个 u32 数值字段；`shift` 见 `types.mjs` 的 `fieldBlockShift` */
function parseNumericFields(buf, sigStart, shift) {
  const fields = {};
  for (let i = 0; i < FIELD_NAMES.length; i++) fields[FIELD_NAMES[i]] = buf.readUInt32LE(sigStart + shift + FIELD_OFFSETS[i]);
  return fields;
}

/**
 * 解析脚本头。
 * @returns {{fields: object, isVer5: boolean, length: number, signature: string, sigBytes: Buffer}}
 */
export function readHeader(buf) {
  if (buf.length < 4) throw new Error('file too small');
  const sig4 = buf.subarray(0, 4);
  if (sig4.equals(S4_SIG)) {
    return {
      fields: parseNumericFields(buf, 0, fieldBlockShift(false)),
      isVer5: false,
      length: HEADER_LEN_V4,
      // 8 字节签名按 latin1 逐字节保真（只用于显示 / 回写）
      signature: buf.subarray(0, 8).toString('latin1'),
      sigBytes: Buffer.from(buf.subarray(0, 8)),
    };
  }
  if (sig4.equals(S5_SIG4)) {
    const sig16 = buf.subarray(0, 16);
    // v5 签名字节里带 NUL（UTF-16LE 编码），latin1 保真；显示用去掉尾部 NUL 的 UTF-16 解读
    return {
      fields: parseNumericFields(buf, 0, fieldBlockShift(true)),
      isVer5: true,
      length: HEADER_LEN_V5,
      signature: decodeUtf16Sig(sig16),
      sigBytes: Buffer.from(sig16),
    };
  }
  throw new Error('Could not determine header version!');
}

function decodeUtf16Sig(sig16) {
  const u16 = [];
  for (let p = 0; p + 1 < sig16.length; p += 2) u16.push(sig16.readUInt16LE(p));
  return String.fromCharCode(...u16).replace(/\u0000+$/, '');
}

// ───────────────────────────────────────────────────────── 文本输出

function disassembleHeader(header) {
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

function disassembleInstruction(header, instr) {
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
      s += '[' + arg.data_array.data.map(hex).join(' ') + ']';
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

function writeScriptFile(header, instructions) {
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
export function disassemble(bin, { codec = defaultCodec, table, strict = true, report } = {}) {
  if (!Buffer.isBuffer(bin)) throw new Error('disassemble: bin 必须是 Buffer');
  const tbl = table || loadOpcodeTable();
  const header = readHeader(bin);
  const { fields } = header;
  const headerLen = header.length;
  const cp = codec || defaultCodec;

  /** 字符串区越界读取：严格模式抛错，宽松模式记一笔并产出 `U+FFFD`/0 */
  const oob = (what, offset) => {
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
    const opCode = bin.readUInt32LE(pos);
    pos += 4;
    if (opCode === 0x0) throw new Error(`Offset 0x${byteOffset.toString(16)} bad opcode : 0`);

    const def = instructionForOpCode(tbl, opCode);
    if (!def) throw new Error(`Unknown instruction : 0x${opCode.toString(16)} at 0x${byteOffset.toString(16)}`);
    if (def.argc === null) throw new Error(`Unknown argc for opcode 0x${opCode.toString(16)} at 0x${byteOffset.toString(16)}`);

    const instr = { def, args: [], byteOffset, offset: (byteOffset - headerLen) >> 2 };

    for (let current = 0; current < def.argc; current++) {
      if (pos + 8 > bin.length) { oob(`指令 0x${opCode.toString(16)} 的操作数 ${current}`, pos); break; }
      const type = bin.readUInt32LE(pos); pos += 4;
      const rawData = bin.readUInt32LE(pos); pos += 4;
      const arg = { type, raw_data: rawData, text: undefined, bytes: undefined, data_array: null };

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
            const ch = bin.readUInt16LE(p); p += 2;
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
          const length = bin.readUInt32LE(arrayOffset);
          const data = [];
          for (let i = 0; i < length; i++) {
            const at = arrayOffset + 4 + i * 4;
            if (at + 4 > bin.length) { oob('数组块元素', at); break; }
            data.push(bin.readUInt32LE(at));
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
function invertBytes(src) {
  const out = Buffer.allocUnsafe(src.length);
  for (let i = 0; i < src.length; i++) out[i] = src[i] ^ 0xff;
  return out;
}

export { CP_932 };
