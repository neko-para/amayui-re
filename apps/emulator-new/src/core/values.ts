/**
 * 值的编解码与五个操作数访问原语。
 * ★ 规则来自 `data/spec/chapters/40-pools.md`：整型族按运行期 key 编解码；浮点族裸 32 位；
 *   类型标签的支持集合按「支持集合与不支持的组合」；越界/未取证一律响亮失败。
 * ★ 本文件不碰文件系统、不认识脚本头。
 */
import { decodeLatin1 } from './bytes.ts';
import type { Machine, Operand } from './machine.ts';
import { arrayStrideFor, isPointerTag, poolBase, resolveAddress, slotAddress } from './address.ts';
import type { PoolTag, Resolved } from './address.ts';

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

/**
 * 运行期整型编码 key：引擎在初始化期生成、保证非 0、每实例一份。
 * ★ 规范只要求“运行期随机生成一个非 0 值”；这里优先用 Web Crypto，没有则退回 Math.random。
 * ★ 显式 key 仍可由测试 / `--key` 注入（用于可复现的位模式），但**不是**必需输入。
 */
export function generateIntKey(): number {
  const cryptoObj = globalThis.crypto;
  if (cryptoObj && typeof cryptoObj.getRandomValues === 'function') {
    const buf = new Uint32Array(1);
    do { cryptoObj.getRandomValues(buf); } while (buf[0] === 0);
    return buf[0] >>> 0;
  }
  let v = 0;
  do { v = (Math.floor(Math.random() * 0x100000000) >>> 0); } while (v === 0);
  return v;
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
    // 指针族（6/7/12/13）：解引用一次，解码发生在**目标格**上（指针槽本身不编解码）
    case 6: case 7: case 12: case 13:
      return decodeInt(rawBitsAt(m, deref(m, op)), m.key);
    default:
      return unsupported('read-int', op);
  }
}

/** 读浮点：支持 0 / 1 / 3 / 4 / 9 / 10；其余标签响亮失败 */
/**
 * 取址原语（值族 3 / 4 / 9 / 10：全局 / 局部 × 整型 / 浮点池的槽）。
 * ★ 地址是 `(池, 槽号)`：池下标寻址就是"地址 + 4×i"（CORE-POOL-PRIMITIVE-ENTRY）。
 * ★ 引用族（6 / 8 / 12 / 14）的"取地址"返回槽内容，不是槽地址 ⇒ 在调用点单独处理；
 *   其余标签（立即数 / 字符串 / 容器句柄）在这里响亮失败。
 */
export interface Address {
  pool: 'globalInt' | 'localInt' | 'globalFloat' | 'localFloat';
  index: number;
}

export function readAddress(m: Machine, ops: Operand[], k: number): Address {
  const op = operandAt(ops, k);
  switch (op.type) {
    case 3:
      return { pool: 'globalInt', index: op.raw };
    case 4:
      return { pool: 'globalFloat', index: op.raw };
    case 9:
      return { pool: 'localInt', index: op.raw };
    case 10:
      return { pool: 'localFloat', index: op.raw };
    default:
      return unsupported('address', op);
  }
}

/**
 * 从地址读出第 `wordOffset` 个 32 位字。
 * ★ 整型池按整型语义解码；浮点池是**裸 32 位位模式**（不做数值转换 —— 数组是逐字拷贝的）。
 */
export function readIntAt(m: Machine, addr: Address, wordOffset: number): number {
  const index = addr.index + wordOffset;
  if (addr.pool === 'globalFloat') return m.globalFloat.get(index) ?? 0;
  if (addr.pool === 'localFloat') return m.frame.localFloat.get(index) ?? 0;
  const map = addr.pool === 'globalInt' ? m.globalInt : m.frame.localInt;
  return decodeInt(map.get(index) ?? encodeInt(0, m.key), m.key);
}

/**
 * 往地址 `addr` 起第 `wordOffset` 个 dword **写裸位模式**（不做族转换）。
 * ★ 这是"连写 N 格"（`0x6c fill-zero` / `0x2d8 set-array-to`）的底层：
 *   整型池里存的是 `ENC(key, v)` 的位模式、浮点池里存的是**裸 32 位**（与 `readIntAt` 的读法对称）。
 */
export function writeBitsAt(m: Machine, addr: Address, wordOffset: number, bits: number): void {
  const index = addr.index + wordOffset;
  const v = bits >>> 0;
  if (addr.pool === 'globalInt') m.globalInt.set(index, v);
  else if (addr.pool === 'localInt') m.frame.localInt.set(index, v);
  else if (addr.pool === 'globalFloat') m.globalFloat.set(index, v);
  else m.frame.localFloat.set(index, v);
}

/** 从**数值地址**读第 `wordOffset` 个 dword 的裸位模式（memcpy 用；字符串记录不可按字拷 ⇒ 抛）。 */
export function readRawAt(m: Machine, addr: number, wordOffset: number): number {
  const r = resolveAddress(addr);
  const i = r.index + wordOffset;
  switch (r.tag) {
    case 3: return (m.globalInt.get(i) ?? 0) >>> 0;
    case 9: return (m.frame.localInt.get(i) ?? 0) >>> 0;
    case 4: return (m.globalFloat.get(i) ?? 0) >>> 0;
    case 10: return (m.frame.localFloat.get(i) ?? 0) >>> 0;
    case 6: return (m.globalIntRef.get(i) ?? 0) >>> 0;
    case 7: return (m.globalFloatRef.get(i) ?? 0) >>> 0;
    case 8: return (m.globalStringRef.get(i) ?? 0) >>> 0;
    case 12: return (m.frame.localPtr.get(i) ?? 0) >>> 0;
    case 13: return (m.frame.localFloatPtr.get(i) ?? 0) >>> 0;
    case 14: return (m.frame.localStringPtr.get(i) ?? 0) >>> 0;
    default: throw new Error(`地址 0x${addr.toString(16)} 指向字符串记录，不能按 4 字节字读`);
  }
}

/** 往**数值地址**写第 `wordOffset` 个 dword 的裸位模式（与 {@link readRawAt} 对称）。 */
export function writeRawAt(m: Machine, addr: number, wordOffset: number, bits: number): void {
  const r = resolveAddress(addr);
  const i = r.index + wordOffset;
  const v = bits >>> 0;
  switch (r.tag) {
    case 3: m.globalInt.set(i, v); return;
    case 9: m.frame.localInt.set(i, v); return;
    case 4: m.globalFloat.set(i, v); return;
    case 10: m.frame.localFloat.set(i, v); return;
    case 6: m.globalIntRef.set(i, v); return;
    case 7: m.globalFloatRef.set(i, v); return;
    case 8: m.globalStringRef.set(i, v); return;
    case 12: m.frame.localPtr.set(i, v); return;
    case 13: m.frame.localFloatPtr.set(i, v); return;
    case 14: m.frame.localStringPtr.set(i, v); return;
    default: throw new Error(`地址 0x${addr.toString(16)} 指向字符串记录，不能按 4 字节字写`);
  }
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
    case 6: case 7: case 12: case 13:
      return f32(rawBitsAt(m, deref(m, op)));
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
    // 指针族（6/7/12）：写原语要**解引用**，编码发生在目标格上
    case 6: case 7: case 12:
      writeRawBitsAt(m, deref(m, op), encodeInt(value | 0, m.key));
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
    // 指针族（6/7/12/13）：解引用后写；13 可写浮（写整型不支持 13，见支持集合）
    case 6: case 7: case 12: case 13:
      writeRawBitsAt(m, deref(m, op), bitsOfF32(Math.fround(value)));
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
 * 读文本：`55-config.md` 的脚本访问只用到全局字符串池（type 5）；`0x192 set-string` 的源侧
 * 还用到 **type 2（内联字符串）**（启动链里 5 处，见 `INITCONFIG0.BIN`）。
 * ★ 其余类型（type 11 局部字符串、type 8/14 指针族）尚未建模 ⇒ 响亮失败，不补空串。
 */
/**
 * 指针族的槽（类型 6/7/8 全局、12/13/14 局部）：槽里装的是**地址**，不编解码。
 * ★ 指针槽本身**不参与编解码**（`40-pools.md`）：写进去什么位模式，读出来就是什么。
 */
function pointerPool(m: Machine, tag: PoolTag): Map<number, number> {
  switch (tag) {
    case 6: return m.globalIntRef;
    case 7: return m.globalFloatRef;
    case 8: return m.globalStringRef;
    case 12: return m.frame.localPtr;
    case 13: return m.frame.localFloatPtr;
    case 14: return m.frame.localStringPtr;
    default: throw new Error(`不是指针族：0x${tag.toString(16)}`);
  }
}

/** 指针槽里的地址（未写过 ⇒ 0；引擎的池是 VirtualAlloc 出来的零页） */
export function pointerSlotAddress(m: Machine, tag: PoolTag, index: number): number {
  return pointerPool(m, tag).get(index) ?? 0;
}

/**
 * 取址原语（**数值地址**）：`40-pools.md` 的「取址」。
 * * 值族（3/4/5/9/10/11）⇒ 那一格（字符串为那条记录）的地址；
 * * 引用 / 指针族（6/7/8/12/13/14）⇒ **槽里装的地址**（指针槽不解引用）。
 */
export function addressOfOperand(m: Machine, ops: Operand[], k: number): number {
  const op = operandAt(ops, k);
  if (isPointerTag(op.type)) return pointerSlotAddress(m, op.type as PoolTag, op.raw);
  switch (op.type) {
    case 3: case 4: case 5:
      return slotAddress(op.type as PoolTag, op.raw);
    case 9: case 10: case 11:
      return slotAddress(op.type as PoolTag, op.raw, m.cur);
    default:
      return unsupported('address', op);
  }
}

/** 地址处的裸 32 位（整型池 = 编码后的位模式；浮点池 = 浮点位模式；指针池 = 地址本身） */
function rawBitsAt(m: Machine, r: Resolved): number {
  switch (r.tag) {
    case 3: return (m.globalInt.get(r.index) ?? encodeInt(0, m.key)) >>> 0;
    case 9: return (m.frame.localInt.get(r.index) ?? encodeInt(0, m.key)) >>> 0;
    case 4: return (m.globalFloat.get(r.index) ?? 0) >>> 0;
    case 10: return (m.frame.localFloat.get(r.index) ?? 0) >>> 0;
    case 6: return (m.globalIntRef.get(r.index) ?? 0) >>> 0;
    case 7: return (m.globalFloatRef.get(r.index) ?? 0) >>> 0;
    case 8: return (m.globalStringRef.get(r.index) ?? 0) >>> 0;
    case 12: return (m.frame.localPtr.get(r.index) ?? 0) >>> 0;
    case 13: return (m.frame.localFloatPtr.get(r.index) ?? 0) >>> 0;
    case 14: return (m.frame.localStringPtr.get(r.index) ?? 0) >>> 0;
    default:
      throw new Error(`地址 0x${slotAddress(r.tag, r.index, r.frameSlot).toString(16)} 指向字符串记录（池 0x${r.tag.toString(16)}）—— 按 4 字节格读写没有定义`);
  }
}

function writeRawBitsAt(m: Machine, r: Resolved, bits: number): void {
  const v = bits >>> 0;
  switch (r.tag) {
    case 3: m.globalInt.set(r.index, v); return;
    case 9: m.frame.localInt.set(r.index, v); return;
    case 4: m.globalFloat.set(r.index, v); return;
    case 10: m.frame.localFloat.set(r.index, v); return;
    case 6: m.globalIntRef.set(r.index, v); return;
    case 7: m.globalFloatRef.set(r.index, v); return;
    case 8: m.globalStringRef.set(r.index, v); return;
    case 12: m.frame.localPtr.set(r.index, v); return;
    case 13: m.frame.localFloatPtr.set(r.index, v); return;
    case 14: m.frame.localStringPtr.set(r.index, v); return;
    default:
      throw new Error(`不能用 4 字节格写字符串记录（池 0x${r.tag.toString(16)}）`);
  }
}

/** 往一个**数值地址**写一个裸 32 位（`0x64` 的逐格写；地址必须先能解析到某个池窗口） */
export function writeBitsToAddress(m: Machine, addr: number, bits: number): void {
  writeRawBitsAt(m, resolveAddress(addr), bits);
}

/** 写一个指针槽（`0x61` / `0x12c` 的落点）：只接受引用 / 指针族，写**地址本身** */
export function writePointerSlot(m: Machine, ops: Operand[], k: number, addr: number): void {
  const op = operandAt(ops, k);
  if (!isPointerTag(op.type)) {
    throw new Error(`写指针槽只接受引用 / 指针族（6/7/8/12/13/14），实际 type 0x${op.type.toString(16)} —— 引擎抛「操作数类型异常」`);
  }
  pointerPool(m, op.type as PoolTag).set(op.raw, addr >>> 0);
}

/** 指针解引用的落点（读 / 写原语用） */
function deref(m: Machine, op: Operand): Resolved {
  const addr = pointerSlotAddress(m, op.type as PoolTag, op.raw);
  return resolveAddress(addr);
}

export function readText(m: Machine, ops: Operand[], k: number): string {
  const op = operandAt(ops, k);
  if (op.type === 5) return m.globalString.get(op.raw) ?? '';
  // type 2：内联字符串。文件偏移 = headerLen + 4·raw，而 `m.body` **已经切掉头** ⇒ 在 body 里就是 4·raw。
  if (op.type === 2) return inlineStringAt(m.body, op.raw >>> 0);
  throw new Error(`text-unsupported-operand-type:0x${op.type.toString(16)}`);
}

/**
 * type 2（内联字符串）的读法（取证：台账 `01M4B3DQVB3G390N2E6E2S3Q2T`，判据是
 * `INITCONFIG0.BIN` 的 5 条 `set-string` 解出字体名）；本层按既有口径存 **latin1 字节串**，
 * 不做 cp932 转码（与 `globalString` 的口径一致：字符串就是字节串）。
 *
 * - 偏移 = `4 × raw`（`body` 已切头，与 label 的换算同源）；
 * - 逐字节 `^0xFF`，解出 0 即止（原始字节是 `0xFF`）；
 * - 越界 / 无终止符 ⇒ 响亮失败（⛔ 不许一路读到末尾 —— 地址算错时最容易这样）。
 */
function inlineStringAt(body: Uint8Array, raw: number): string {
  const off = 4 * raw;
  if (off >= body.length) {
    throw new Error(`内联字符串的偏移越出脚本体：raw=${raw} ⇒ off=${off}（body ${body.length} B）`);
  }
  const out: number[] = [];
  for (let p = off; p < body.length; p += 1) {
    const c = body[p] ^ 0xff;
    if (c === 0) return decodeLatin1(Uint8Array.from(out));
    out.push(c);
    if (out.length > 0x10000) throw new Error(`内联字符串没有终止符（>65536 B，raw=${raw}）—— 偏移口径错了？`);
  }
  throw new Error(`内联字符串读到末尾都没有终止符（raw=${raw}，off=${off}）`);
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
