/**
 * packages/age-format/src/asm/bytes.mts —— **字节原语**（★ 零 Node 依赖）
 *
 * ## 为什么单开这一个文件
 * 本包原先处处用 Node 的 `Buffer`。但 **AGE 脚本这一层必须能跑在浏览器里** —— 模拟器的核心是前端的，
 * 而 `Buffer` 在浏览器里是**未定义全局** ⇒ 要么引 polyfill，要么踩运行期炸弹。
 * 旧仓正是踩过的（`src/live2d/moc.ts`）："`Buffer is not defined` 的运行时炸弹，
 * **而 Node 侧的测试（tsx）有 `Buffer` ⇒ 全绿，抓不到这个分叉**"。
 *
 * ⇒ 本文件只许用 **Web 标准**：`Uint8Array` / `DataView`。
 * ★ `Buffer` 是 `Uint8Array` 的**子类**（实测 `Buffer instanceof Uint8Array === true`）⇒
 *   入参放宽成 `Uint8Array` 时**Node 侧调用方一个都不用改**；
 *   但返回值从 `Buffer` 收窄成 `Uint8Array` 时，用 `Buffer` 专有方法的地方**必须逐个改**
 *   —— 而且 `pnpm typecheck` **找不全**，见下面三条。
 *
 * ## ★ 三个"tsc 抓不到"的静默陷阱（换类型时务必人肉过一遍，实测数据在每条后面）
 * 1. **`.toString()` 语义不同**：`new Uint8Array([1,2,3]).toString()` = `"1,2,3"`，
 *    而 `Buffer.from([1,2,3]).toString()` = `"\u0001\u0002\u0003"`。⇒ `x.toString('hex')` 收敛成
 *    `"…,…"`，而"自己和自己比"的断言**永远绿**（本仓 `asm.assets.test.mjs` 原先就有一处）。
 * 2. **`.slice()` 语义不同**：`Uint8Array.prototype.slice` 是**拷贝**，`Buffer.prototype.slice` 是**视图**
 *    （≡ `subarray`）。想要视图的写法必须显式写 `subarray`（两边语义一致）。
 * 3. **`new DataView(b.buffer)` 忽略 `b.byteOffset`**：`subarray` 共享底层 `ArrayBuffer`
 *    ⇒ 必须 `new DataView(b.buffer, b.byteOffset, b.byteLength)`，否则**读错位**。
 *    （旧仓 `src/util/bytes.ts` 的 `ByteView` 注释就是为这条写的。）
 *
 * ## 审计用的搜法（改完返回值类型后跑）
 * `rg '\.equals\(|\.toString\(|\.slice\('` —— 逐处确认接收者**不是**本包 `assemble()` / `readHeader()`
 * 一类函数的返回值。`tools/**` 是 `.mjs`（不进 `tsc`）⇒ 那里的漏网只会在运行期炸。
 */
/** 逐字节比较（`TypedArray` **没有** `equals`，这是唯一的"能力缺口"） */
export const bytesEqual = (a: Uint8Array, b: Uint8Array): boolean => {
  if (a === b) return true;
  if (a.length !== b.length) return false;
  for (let i = 0; i < a.length; i += 1) if (a[i] !== b[i]) return false;
  return true;
};

/** 依次拼接（`TypedArray` **没有** `concat`） */
export const concatBytes = (parts: readonly Uint8Array[]): Uint8Array => {
  let total = 0;
  for (const p of parts) total += p.length;
  const out = new Uint8Array(total);
  let at = 0;
  for (const p of parts) {
    out.set(p, at);
    at += p.length;
  }
  return out;
};

/** **拷贝**一段（★ 不是视图：视图请用 `subarray`） */
export const copyBytes = (b: Uint8Array, begin?: number, end?: number): Uint8Array =>
  (begin === undefined ? b.slice() : b.slice(begin, end));

/** 把"像字节的东西"归一成 `Uint8Array`（`Buffer` / `Uint8Array` 都是；其它走 `Uint8Array.from`） */
export const toBytes = (b: Uint8Array | readonly number[]): Uint8Array =>
  (b instanceof Uint8Array ? b : Uint8Array.from(b));

/** 字节 → latin1 字符串（`charCode === 字节值`；`Buffer.toString('latin1')` 的等价物） */
export const decodeLatin1 = (b: Uint8Array): string => {
  let s = '';
  for (let i = 0; i < b.length; i += 1) s += String.fromCharCode(b[i]);
  return s;
};

/** latin1 字符串 → 字节（截低 8 位；`Buffer.from(s,'latin1')` 的等价物） */
export const encodeLatin1 = (s: string): Uint8Array => {
  const out = new Uint8Array(s.length);
  for (let i = 0; i < s.length; i += 1) out[i] = s.charCodeAt(i) & 0xff;
  return out;
};

/**
 * 小端读取器。★ **一次构造一个**，在循环里复用（别每次读都 `new DataView`）。
 * `byteOffset` / `byteLength` 一定要带上 —— 否则 `subarray` 出来的视图会读错位（见文件头陷阱 3）。
 */
export class ByteReader {
  readonly bytes: Uint8Array;
  readonly #dv: DataView;

  constructor(b: Uint8Array) {
    this.bytes = b;
    this.#dv = new DataView(b.buffer, b.byteOffset, b.byteLength);
  }

  /** 读 u16（小端） */
  u16(offset: number): number {
    return this.#dv.getUint16(offset, true);
  }

  /** 读 u32（小端，返回 `0..2^32-1` 无符号） */
  u32(offset: number): number {
    return this.#dv.getUint32(offset, true);
  }
}

/** 小端写入器（自己分配；`bytes` 就是要写进的那段） */
export class ByteWriter {
  readonly bytes: Uint8Array;
  readonly #dv: DataView;

  constructor(length: number) {
    this.bytes = new Uint8Array(length);
    this.#dv = new DataView(this.bytes.buffer);
  }

  /** 写 u16（小端） */
  u16(offset: number, value: number): void {
    this.#dv.setUint16(offset, value & 0xffff, true);
  }

  /** 写 u32（小端；入参按 uint32 收） */
  u32(offset: number, value: number): void {
    this.#dv.setUint32(offset, value >>> 0, true);
  }
}
