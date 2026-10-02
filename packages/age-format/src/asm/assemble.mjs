/**
 * packages/age-format/src/asm/assemble.mjs —— **反汇编文本 → AGE 脚本字节码**（纯函数，不碰文件系统）
 *
 * 来源（旧仓只读参照，逐行对照移植）：
 *   · `天結/scripts/asm/reassembler.mjs` —— `assemble` / `parseHeader` / `parseMultipleArguments` /
 *     `nextCodeLine`
 *   · `天結/scripts/asm/age-shared.mjs` —— `writeHeaderBytes` / `getType` / `utf16ToCp`
 *
 * ## 职责边界
 * 只做**格式层**：文本 ↔ 字节。本文件不解释指令含义，只按 `argc` 切参数、按文本形状分流参数种类。
 *
 * ## 非显然口径（与反汇编器逐条对应，改一边必须改另一边）
 * 1. **头部只回写 6 个 `local_vars` 字段**：`sub_header_length` 固定 0x1C，三张表的 length/offset
 *    由指令区**重新算**（它们本就是派生物）。所以"反汇编 → 重汇编"能逐字节相同的前提是：
 *    原文件的 `sub_header_length` 就是 0x1C、且三张表的内容确实是"用到的序号升序去重"。
 * 2. **`label_XXXXXXXX` 是绝对字节偏移**（= `headerLen + 指令序号*4`）。重汇编先收集 label →
 *    指令序号，再把 label 操作数从"绝对偏移"改写成"指令序号"；引用了不存在的 label ⇒ 抛错。
 * 3. **字符串区排版**（必须与反汇编器的读取方式互为逆）：
 *    v4：每字节 `b ^ 0xFF` 写入，`0xFF` 终止，然后按 4 字节对齐补 `0xFF`；
 *    v5：每 u16 `c ^ 0xFFFF` 写入，`0xFFFF` 终止 + 对齐。**补齐字节数是旧仓经验值**
 *    （终止后补到 4 字节边界），不要"优化"成别的数量 —— 那是逐字节相同的一部分。
 * 4. **数组块在 footer 最前面**：块 = `[长度][元素…]`，`raw_data` = 块起点相对 headerLen 的 u32 序号。
 *    随后是三张表（`0x71` / `0x03` / `0x8F` 三类指令的序号升序）；三张表的 offset 依次首尾相接。
 * 5. **参数分流靠文本形状，不靠指令名**：`(类型 值)` = 带标签变量；`"…"` = 字符串；
 *    `label_XXXX` = label；`[…]` = 数组；否则 = 裸十六进制标量。所以反汇编输出必须能被这张正则吃回。
 * 6. **类型表里保留了 `unknown0x8003` 一族的历史别名**（旧仓口径）：反汇编只写出 `0x8003` 这种写法，
 *    但老文本里可能用 `unknown0x8003` ⇒ 两条都认。
 */
import {
  loadOpcodeTable, instructionForToken, instructionByteLength,
} from './opcodes.mjs';
import { defaultCodec, encodeUtf16Le } from './codec.mjs';
import {
  getType, FIELD_NAMES, FIELD_OFFSETS, fieldBlockShift,
} from './types.mjs';

const HEADER_LEN_V4 = 0x3c;
const HEADER_LEN_V5 = 0x44;

/** 参数解析：与旧仓同一条正则（顺序即优先级） */
const RE_PARSE_ARGS = /\((\w+?\-?\w+?\-?\w+?) ([0-9a-fA-F]+)\)|(".*?")|label_([0-9a-fA-F]+)|\[(.+?)\]|([0-9a-fA-F]+)/g;

/** 把一行参数串切成若干"参数原文"，每项 6 个捕获组（与 `RE_PARSE_ARGS` 对齐） */
function parseMultipleArguments(line) {
  const out = [];
  RE_PARSE_ARGS.lastIndex = 0;
  let m;
  while ((m = RE_PARSE_ARGS.exec(line)) !== null) {
    out.push([m[1] || '', m[2] || '', m[3] || '', m[4] || '', m[5] || '', m[6] || '']);
    if (m[0].length === 0) RE_PARSE_ARGS.lastIndex++;
  }
  return out;
}

/** 从反汇编文本的第 2/3 行读签名与 `local_vars` */
function parseHeader(lines) {
  const sigLine = lines[1] || '';
  const sigStart = sigLine.indexOf('= ') + 2;
  const signature = sigLine.slice(sigStart, sigStart + 8).padEnd(8, ' ');
  const isVer5 = (signature[3] || '') === '5';
  const length = isVer5 ? HEADER_LEN_V5 : HEADER_LEN_V4;

  const lvLine = lines[2] || '';
  const lvMatch = lvLine.match(/local_vars\s*=\s*\{\s*([^}]*)/);
  const tokens = (lvMatch ? lvMatch[1].trim().split(/\s+/) : []).filter(Boolean);
  if (tokens.length < 6) {
    throw new Error(`Header is corrupted, there should be 6 local_vars, but could only read ${tokens.length}`);
  }

  return {
    fields: {
      local_integer_1: parseInt(tokens[0], 16),
      local_floats: parseInt(tokens[1], 16),
      local_strings_1: parseInt(tokens[2], 16),
      local_integer_2: parseInt(tokens[3], 16),
      unknown_data: parseInt(tokens[4], 16),
      local_strings_2: parseInt(tokens[5], 16),
      sub_header_length: 0x1c,
      table_1_length: 0,
      table_1_offset: 0,
      table_2_length: 0,
      table_2_offset: 0,
      table_3_length: 0,
      table_3_offset: 0,
    },
    isVer5,
    length,
    signature,
  };
}

/**
 * 组装头部字节（`readHeader` 的逆）。
 * v4 写 0x3C 字节、v5 写 0x44 字节；签名优先用 `header.sigBytes`（原样回写，避免 NUL 往返出偏差），
 * 没有 `sigBytes` 时按 `header.signature` 字符串编（v5 = UTF-16LE，v4 = latin1）。
 * 13 个数值字段的顺序 / 偏移 / v5 位移全部来自 `types.mjs`（与读侧共用同一张表）。
 */
export function writeHeaderBytes(header) {
  const { fields, isVer5, signature, sigBytes } = header;
  const out = isVer5 ? Buffer.alloc(HEADER_LEN_V5) : Buffer.alloc(HEADER_LEN_V4);
  if (isVer5) {
    const raw = Buffer.isBuffer(sigBytes) && sigBytes.length >= 16
      ? sigBytes.subarray(0, 16)
      : encodeUtf16Le((String(signature || 'SYS5501 ').replace(/\u0000+$/, '') || 'SYS5501 ').padEnd(8, ' '));
    raw.copy(out, 0, 0, Math.min(16, raw.length));
  } else {
    const raw = Buffer.isBuffer(sigBytes) && sigBytes.length >= 8
      ? sigBytes.subarray(0, 8)
      : Buffer.from(String(signature || '        ').padEnd(8, ' '), 'latin1');
    raw.copy(out, 0, 0, Math.min(8, raw.length));
  }
  const shift = fieldBlockShift(isVer5);
  for (let i = 0; i < FIELD_NAMES.length; i++) out.writeUInt32LE(fields[FIELD_NAMES[i]] >>> 0, shift + FIELD_OFFSETS[i]);
  return out;
}

/** 跳过空行 / `//` 行注释 / 成对块注释，返回下一条要处理的指令行 */
function nextCodeLine(lines, i) {
  while (i < lines.length) {
    let line = lines[i];
    if (line === '' || line.startsWith('//')) { i++; continue; }
    if (line.startsWith('/*')) {
      while (i < lines.length && !line.includes('*/')) { i++; line = lines[i]; }
      if (i >= lines.length) return null;
      line = line.slice(line.indexOf('*/') + 2);
      if (line === '') { i++; continue; }
      return { line, next: i + 1 };
    }
    return { line, next: i + 1 };
  }
  return null;
}

/**
 * 反汇编文本 → AGE 脚本字节码，返回 Buffer。
 *
 * `text` 是反汇编文本（UTF-8 读入的字符串）。
 * `opts.codec` 缺省 CP932（脚本 v5 的字符串走 UTF-16LE，与此无关）；
 * `opts.table` 缺省在本目录加载 `instruction-set.json`（格式层四列；由 `pnpm tools opcodes derive` 派生）。
 */
export function assemble(text, { codec = defaultCodec, table } = {}) {
  if (typeof text !== 'string') throw new Error('assemble: text 必须是 string');
  const tbl = table || loadOpcodeTable();
  const cp = codec || defaultCodec;
  const lines = text.split(/\r?\n/);
  const header = parseHeader(lines);
  const headerLen = header.length;

  const instructions = [];
  const labelToOffset = new Map();
  const labelArguments = [];
  const stringArguments = [];
  const arrayArguments = [];
  const instr3Offsets = new Set();
  const instr71Offsets = new Set();
  const instr8fOffsets = new Set();

  let dataArrayEnd = headerLen;
  let lineCount = 6;

  let i = 4;
  let nxt;
  while ((nxt = nextCodeLine(lines, i)) !== null) {
    const { line, next } = nxt;
    i = next;

    const m = line.match(/^[\w\-_]+/);
    if (!m) throw new Error(`Failed to parse line ${lineCount}: ${line}`);
    const instrToken = m[0];

    if (instrToken.startsWith('label_')) {
      labelToOffset.set(parseInt(instrToken.slice(6), 16), dataArrayEnd);
      continue;
    }

    const def = instructionForToken(tbl, instrToken);
    if (!def) throw new Error(`Unknown instruction : ${instrToken} on line ${lineCount}`);
    if (def.argc === null) throw new Error(`Unknown argc for instruction ${instrToken}`);

    const instr = { def, args: [], byteOffset: dataArrayEnd, offset: (dataArrayEnd - headerLen) >> 2 };

    if (def.argc > 0) {
      const argStr = line.substring(instrToken.length + 1);
      const parsed = parseMultipleArguments(argStr);
      if (def.argc !== parsed.length) {
        throw new Error(
          `Argument mismatch for ${instrToken} on line ${lineCount}. ` +
          `Expected ${def.argc} args but found ${parsed.length}.`
        );
      }
      for (const a of parsed) {
        const arg = { type: 0, raw_data: 0, text: undefined, bytes: undefined, data_array: null };
        const idx = [instructions.length, instr.args.length];
        if (a[0] !== '') {
          arg.type = getType(a[0]);
          arg.raw_data = parseInt(a[1], 16);
        } else if (a[2] !== '') {
          const content = a[2].slice(1, -1);
          arg.type = 2;
          if (header.isVer5) arg.bytes = encodeUtf16Le(content);
          else arg.bytes = cp.encode(content);
          stringArguments.push(idx);
        } else if (a[3] !== '') {
          arg.type = 0;
          arg.raw_data = parseInt(a[3], 16);
          labelArguments.push(idx);
        } else if (a[4] !== '') {
          const data = [];
          for (const p of a[4].split(' ')) if (p !== '') data.push(parseInt(p, 16));
          arg.type = 0;
          arg.data_array = { length: data.length, data };
          arrayArguments.push(idx);
        } else if (a[5] !== '') {
          arg.type = 0;
          arg.raw_data = parseInt(a[5], 16);
        } else {
          throw new Error(`Bad argument for ${instrToken} on line ${lineCount}.`);
        }
        instr.args.push(arg);
      }
    }

    if (def.opcode === 0x3) instr3Offsets.add(dataArrayEnd);
    else if (def.opcode === 0x71) instr71Offsets.add(dataArrayEnd);
    else if (def.opcode === 0x8f) instr8fOffsets.add(dataArrayEnd);

    dataArrayEnd += instructionByteLength(def);
    lineCount++;
    instructions.push(instr);
  }

  // label 操作数：绝对字节偏移 → 指令序号
  for (const [instrIdx, argIdx] of labelArguments) {
    const arg = instructions[instrIdx].args[argIdx];
    const target = labelToOffset.get(arg.raw_data);
    if (target === undefined) throw new Error(`Unknown label reference 0x${arg.raw_data.toString(16)}`);
    arg.raw_data = (target - headerLen) >> 2;
  }

  // 字符串区
  const stringData = [];
  let currentStringOffset = dataArrayEnd;
  for (const [instrIdx, argIdx] of stringArguments) {
    const arg = instructions[instrIdx].args[argIdx];
    const bytes = arg.bytes || Buffer.alloc(0);
    if (header.isVer5) {
      const charCount = bytes.length / 2;
      arg.raw_data = (currentStringOffset - headerLen) >> 2;
      currentStringOffset += (charCount + 1) * 2;
      for (let b = 0; b < bytes.length; b++) stringData.push(bytes[b] ^ 0xff);
      const padding = 4 - (currentStringOffset % 4);
      for (let k = 0; k < padding + 2; k++) stringData.push(0xff);
      currentStringOffset += padding;
    } else {
      arg.raw_data = (currentStringOffset - headerLen) >> 2;
      currentStringOffset += bytes.length + 1;
      for (const b of bytes) stringData.push(b ^ 0xff);
      const padding = 4 - (currentStringOffset % 4);
      for (let k = 0; k < padding + 1; k++) stringData.push(0xff);
      currentStringOffset += padding;
    }
  }

  // footer：数组块 + 三张表
  const footerData = [];
  let currentArrayOffset = (currentStringOffset - headerLen) >> 2;
  for (const [instrIdx, argIdx] of arrayArguments) {
    const arg = instructions[instrIdx].args[argIdx];
    arg.raw_data = currentArrayOffset;
    footerData.push(arg.data_array.length);
    currentArrayOffset += arg.data_array.length + 1;
    footerData.push(...arg.data_array.data);
  }

  const toIndex = (off) => (off - headerLen) >> 2;
  const sorted = (set) => [...set].map(toIndex).sort((a, b) => a - b);

  const instr71Vec = sorted(instr71Offsets);
  footerData.push(...instr71Vec);
  header.fields.table_1_length = instr71Vec.length;
  header.fields.table_1_offset = currentArrayOffset;

  const instr3Vec = sorted(instr3Offsets);
  footerData.push(...instr3Vec);
  header.fields.table_2_length = instr3Vec.length;
  header.fields.table_2_offset = header.fields.table_1_offset + header.fields.table_1_length;

  const instr8fVec = sorted(instr8fOffsets);
  footerData.push(...instr8fVec);
  header.fields.table_3_length = instr8fVec.length;
  header.fields.table_3_offset = header.fields.table_2_offset + header.fields.table_2_length;

  // 写字节
  const parts = [writeHeaderBytes(header)];
  const code = Buffer.alloc(dataArrayEnd - headerLen);
  let pos = 0;
  for (const instr of instructions) {
    code.writeUInt32LE(instr.def.opcode, pos); pos += 4;
    for (const arg of instr.args) {
      code.writeUInt32LE(arg.type >>> 0, pos); pos += 4;
      code.writeUInt32LE(arg.raw_data >>> 0, pos); pos += 4;
    }
  }
  parts.push(code);
  parts.push(Buffer.from(stringData));
  const footer = Buffer.alloc(footerData.length * 4);
  for (let k = 0; k < footerData.length; k++) footer.writeUInt32LE(footerData[k] >>> 0, k * 4);
  parts.push(footer);

  return Buffer.concat(parts);
}
