/**
 * 值的编解码与五个操作数访问原语。
 * ★ 规则来自 `data/spec/chapters/40-pools.md`：整型族按运行期 key 编解码；浮点族裸 32 位；
 *   类型标签的支持集合按「支持集合与不支持的组合」；越界/未取证一律响亮失败。
 * ★ 本文件不碰文件系统、不认识脚本头。
 */
import type { Machine, Operand } from './machine.ts';

const u32 = (x: number): number => x >>> 0;

function rol(x: number, n: number): number {
  const k = n & 31;
  return u32((x << k) | (x >>> (32 - k)));
}

function ror(x: number, n: number): number {
  const k = n & 31;
  return u32((x >>> k) | (x << (32 - k)));
}

/** 解码：`ror32(rol32(bits, 11) XOR key, 25)` */
export function decodeInt(bits: number, key: number): number {
  return ror(u32(rol(u32(bits), 11) ^ u32(key)), 25) | 0;
}

/** 编码：`rol32(ror32(value, 7) XOR key, 21)`（`decodeInt` 的逆） */
export function encodeInt(value: number, key: number): number {
  return rol(u32(ror(u32(value), 7) ^ u32(key)), 21);
}

/** 单精度位模式 → JS 数（读原语返回 64 位浮点，值相同） */
function f32(bits: number): number {
  const buf = new DataView(new ArrayBuffer(4));
  buf.setUint32(0, u32(bits), true);
  return buf.getFloat32(0, true);
}

function bitsOfF32(v: number): number {
  const buf = new DataView(new ArrayBuffer(4));
  buf.setFloat32(0, v, true);
  return buf.getUint32(0, true);
}

/** 向零截断（写浮点到整型族、读整型从浮点族、读整型从立即浮点都是这条） */
function truncToInt(v: number): number {
  return Math.trunc(v) | 0;
}

function operandAt(ops: Operand[], k: number): Operand {
  const op = ops[k - 1];
  if (!op) {
    // 一条指令读了超出 argc 的操作数 ⇒ 实现/指令表不一致，不许补值
    throw new Error(`operand-out-of-range:${k}/${ops.length}`);
  }
  return op;
}

function unsupported(kind: string, op: Operand): never {
  throw new Error(`${kind}:unsupported-operand-type:0x${op.type.toString(16)}`);
}

/** 读整型：支持 0 / 1 / 3 / 4 / 9 / 10；其余标签响亮失败（"没取证，不是坏数据"） */
export function readInt(m: Machine, ops: Operand[], k: number): number {
  const op = operandAt(ops, k);
  switch (op.type) {
    case 0:
      return op.raw | 0;
    case 1:
      return truncToInt(f32(op.raw));
    case 3:
      return decodeInt(m.globalInt.get(op.raw) ?? encodeInt(0, m.key), m.key);
    case 4:
      return truncToInt(readFloatCell(m, m.globalFloat, op.raw));
    case 9:
      return decodeInt(m.frame.localInt.get(op.raw) ?? encodeInt(0, m.key), m.key);
    case 10:
      return truncToInt(readFloatCell(m, m.frame.localFloat, op.raw));
    default:
      return unsupported('read-int', op);
  }
}

/** 读浮点：支持 0 / 1 / 3 / 4 / 9 / 10；其余标签响亮失败 */
/**
 * 取址原语（只实现 3 / 9：全局 / 局部整型槽）。
 * ★ 地址是 `(池, 槽号)`：池下标寻址就是"地址 + 4×i"（CORE-POOL-PRIMITIVE-ENTRY）。
 * ★ 其它族要等它们的取址语义成篇；遇到就响亮失败。
 */
export interface Address {
  pool: 'globalInt' | 'localInt';
  index: number;
}

export function readAddress(m: Machine, ops: Operand[], k: number): Address {
  const op = operandAt(ops, k);
  switch (op.type) {
    case 3:
      return { pool: 'globalInt', index: op.raw };
    case 9:
      return { pool: 'localInt', index: op.raw };
    default:
      return unsupported('address', op);
  }
}

/** 从地址读出第 `wordOffset` 个 32 位字（按整型语义解码；未写过的格 = ENC(0)） */
export function readIntAt(m: Machine, addr: Address, wordOffset: number): number {
  const index = addr.index + wordOffset;
  const map = addr.pool === 'globalInt' ? m.globalInt : m.frame.localInt;
  return decodeInt(map.get(index) ?? encodeInt(0, m.key), m.key);
}

export function readFloat(m: Machine, ops: Operand[], k: number): number {
  const op = operandAt(ops, k);
  switch (op.type) {
    case 0:
      return op.raw | 0;
    case 1:
      return f32(op.raw);
    case 3:
      return decodeInt(m.globalInt.get(op.raw) ?? encodeInt(0, m.key), m.key);
    case 4:
      return readFloatCell(m, m.globalFloat, op.raw);
    case 9:
      return decodeInt(m.frame.localInt.get(op.raw) ?? encodeInt(0, m.key), m.key);
    case 10:
      return readFloatCell(m, m.frame.localFloat, op.raw);
    default:
      return unsupported('read-float', op);
  }
}

/** 写整型：支持 3 / 4 / 9 / 10；其余标签响亮失败 */
export function writeInt(m: Machine, ops: Operand[], k: number, value: number): void {
  const op = operandAt(ops, k);
  switch (op.type) {
    case 3:
      m.globalInt.set(op.raw, encodeInt(value | 0, m.key));
      return;
    case 4:
      m.globalFloat.set(op.raw, bitsOfF32(Math.fround(value)));
      return;
    case 9:
      m.frame.localInt.set(op.raw, encodeInt(value | 0, m.key));
      return;
    case 10:
      m.frame.localFloat.set(op.raw, bitsOfF32(Math.fround(value)));
      return;
    default:
      unsupported('write-int', op);
  }
}

/** 写浮点：支持 3 / 4 / 9 / 10；其余标签响亮失败 */
export function writeFloat(m: Machine, ops: Operand[], k: number, value: number): void {
  const op = operandAt(ops, k);
  switch (op.type) {
    case 3:
      m.globalInt.set(op.raw, encodeInt(truncToInt(value), m.key));
      return;
    case 4:
      m.globalFloat.set(op.raw, bitsOfF32(Math.fround(value)));
      return;
    case 9:
      m.frame.localInt.set(op.raw, encodeInt(truncToInt(value), m.key));
      return;
    case 10:
      m.frame.localFloat.set(op.raw, bitsOfF32(Math.fround(value)));
      return;
    default:
      unsupported('write-float', op);
  }
}

/**
 * 浮点池的槽：规范只说"不保证初值"。
 * ★ 默认 `throw`：不许替引擎补值（否则"不知道"被伪装成 0）。
 * ★ `zero`：**显式的参考实现策略** —— 读成 0.0，并把这次读取记进诊断（不静默）。
 *   它只用于"要跑通真实脚本、但语料确实读未写槽"的场景（调用方必须显式选它）。
 */
function readFloatCell(m: Machine, map: Map<number, number>, index: number): number {
  const bits = map.get(index);
  if (bits === undefined) {
    if (m.uninitializedFloat === 'zero') {
      m.diagnostics.push({ kind: 'uninitialized-float', index });
      return 0;
    }
    throw new Error(`uninitialized-float-slot:${index}`);
  }
  return f32(bits);
}

/**
 * 读文本：`55-config.md` 的脚本访问只用到全局字符串池（type 5）。
 * ★ 其余类型（含 type 11 局部字符串、type 2 内联字符串）尚未建模 ⇒ 响亮失败，不补空串。
 */
export function readText(m: Machine, ops: Operand[], k: number): string {
  const op = operandAt(ops, k);
  if (op.type === 5) return m.globalString.get(op.raw) ?? '';
  throw new Error(`text-unsupported-operand-type:0x${op.type.toString(16)}`);
}

/** 写文本：只支持 type 5（全局字符串池）；其余类型响亮失败 */
export function writeText(m: Machine, ops: Operand[], k: number, value: string): void {
  const op = operandAt(ops, k);
  if (op.type === 5) {
    m.globalString.set(op.raw, value);
    return;
  }
  throw new Error(`text-unsupported-operand-type:0x${op.type.toString(16)}`);
}
