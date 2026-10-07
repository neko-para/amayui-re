/**
 * packages/age-format/src/asm/header.mts —— **脚本头结构**（★ 零 Node 依赖）
 *
 * ## 为什么从 `disassemble.mts` 拆出来
 * 原先 `readHeader` 住在 `disassemble.mts` 里，而那个模块还带着 `disassemble()` 与码页编解码。
 * 拆开之后，运行期子集（`asm/runtime.mts`）就能"只要头部、不拉反汇编器"。
 * （★ 当初拆它的**触发原因**是那条链上带着 `node:fs`；那个根因现在已经拔掉 —— 指令表改成随模块自带的
 * ESM JSON import，见 `opcodes.mts` 头注。拆分的**本身**仍然成立：这是**范围**问题，不是平台问题。）
 *
 * ## 这一层是什么
 * `字节 → 脚本头` 的**格式层**观察：v4（前 4 字节 `SYS4`）头 `0x3C`、v5（UTF-16LE 的 `SY`）头 `0x44`，
 * 两者的 13 个 u32 数值字段同在"签名之后"（v4 偏移 8 起、v5 偏移 16 起，见 `types.mts` 的 `fieldBlockShift`）。
 * ❌ 不解释任何字段的**游戏语义**（那是知识层的事）。
 *
 * ## 非显然口径
 * 1. **头部长度看签名**，不看文件长度；签名不认识 ⇒ 抛 `Could not determine header version!`。
 * 2. 签名要**逐字节保真**地留住（`sigBytes`）：v5 的签名里带 NUL，走字符串往返会出偏差。
 * 3. v5 签名的**显示形式**是 UTF-16 解读 + 去掉尾部 NUL；v4 是 latin1 逐字节。
 */
import { ByteReader, bytesEqual, copyBytes, decodeLatin1, encodeLatin1 } from './bytes.mts';
import { FIELD_NAMES, FIELD_OFFSETS, fieldBlockShift } from './types.mts';

/** v4 签名（`SYS4`，8 字节里前 4 字节是它、后面补空格） */
const S4_SIG: Uint8Array = encodeLatin1('SYS4');
/** v5 签名前 4 字节 = UTF-16LE 的 `SY` */
export const S5_SIG4: Uint8Array = Uint8Array.of(0x53, 0x00, 0x59, 0x00);

/** v4 头部长度（`0x3C`） */
export const HEADER_LEN_V4 = 0x3c;
/** v5 头部长度（`0x44`） */
export const HEADER_LEN_V5 = 0x44;

/**
 * **字节源** = 一段可随机访问的字节。★ 曾经它是个"必须带 `readUInt32LE` / `equals` / `toString(enc)`
 * 的结构性接口"—— 那等于**要求调用方交一个 Buffer 形状的东西**，正是核心不可移植的根源。
 * 现在就是 `Uint8Array`（`Buffer` 是它的子类 ⇒ Node 侧照旧），读取走 `ByteReader`（`bytes.mts`）。
 */
export type ByteSource = Uint8Array;
/** ★ 旧名保留为别名（模拟器侧曾 import 的是 `ByteView`） */
export type ByteView = ByteSource;

/** 脚本头里那 13 个 u32 数值字段（键名见 `FIELD_NAMES`，顺序见 `FIELD_OFFSETS`） */
export type HeaderFields = Record<string, number>;

/**
 * 脚本头。v4 与 v5 的**共同形状**：`isVer5` / `length` 区分两者。
 * ★ 类型住在实现旁边（消费方 `import type { Header } from '@amayui/age-format/src/asm/runtime.mts'`）。
 */
export interface Header {
  readonly fields: HeaderFields;
  readonly isVer5: boolean;
  /** v4 = 60（`0x3C`）· v5 = 68（`0x44`） */
  readonly length: number;
  /** 签名的**可显示形式**（v4 按 latin1 · v5 按 UTF-16） */
  readonly signature: string;
  /** 签名的**原始字节**（8 或 16 字节） */
  readonly sigBytes?: Uint8Array;
}

/** 从签名起点读 13 个 u32 数值字段；`shift` 见 `types.mts` 的 `fieldBlockShift` */
function parseNumericFields(rd: ByteReader, sigStart: number, shift: number): HeaderFields {
  const fields: HeaderFields = {};
  for (let i = 0; i < FIELD_NAMES.length; i += 1) {
    fields[FIELD_NAMES[i]] = rd.u32(sigStart + shift + FIELD_OFFSETS[i]);
  }
  return fields;
}

/** v5 签名的显示形式：按 u16 解读（去掉尾部 NUL） */
function decodeUtf16Sig(sig16: Uint8Array): string {
  const rd = new ByteReader(sig16);
  const u16: number[] = [];
  for (let p = 0; p + 1 < sig16.length; p += 2) u16.push(rd.u16(p));
  return String.fromCharCode(...u16).replace(/\u0000+$/, '');
}

/** 解析脚本头；签名不认识 ⇒ 抛 `Could not determine header version!` */
export function readHeader(bytes: Uint8Array): Header {
  if (bytes.length < 4) throw new Error('file too small');
  const rd = new ByteReader(bytes);
  if (bytesEqual(bytes.subarray(0, 4), S4_SIG)) {
    return {
      fields: parseNumericFields(rd, 0, fieldBlockShift(false)),
      isVer5: false,
      length: HEADER_LEN_V4,
      // 8 字节签名按 latin1 逐字节保真（只用于显示 / 回写）
      signature: decodeLatin1(bytes.subarray(0, 8)),
      sigBytes: copyBytes(bytes, 0, 8),
    };
  }
  if (bytesEqual(bytes.subarray(0, 4), S5_SIG4)) {
    const sig16 = bytes.subarray(0, 16);
    // v5 签名字节里带 NUL（UTF-16LE 编码），原始字节保真；显示用去掉尾部 NUL 的 UTF-16 解读
    return {
      fields: parseNumericFields(rd, 0, fieldBlockShift(true)),
      isVer5: true,
      length: HEADER_LEN_V5,
      signature: decodeUtf16Sig(sig16),
      sigBytes: copyBytes(sig16),
    };
  }
  throw new Error('Could not determine header version!');
}
