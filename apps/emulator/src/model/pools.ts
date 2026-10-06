/**
 * apps/emulator/src/model/pools.ts —— **池模型**（批 R1 迭代点 ④）★ 语义层，零镜像布局
 *
 * ## 这一层是什么
 * 它回答的是**引擎的数据模型**，不是"引擎把东西放在哪"：
 * * 有哪几族池（local 6 个 + global 6 名）、各自装得下什么、谁要过 DEC/ENC、operand type tag 是几号；
 * * 按 index 取槽、写槽、初始化、越界留痕。
 *
 * ## ★ 它**不含**任何镜像布局（这是本层的硬口径）
 * ❌ 没有绝对地址（`Engine+0x5D880` 之类）
 * ❌ 没有帧内偏移（`+0x38` 之类）
 * ❌ 没有 EA / 反汇编片段
 * ⇒ 那些是**逆向观察**，在 `packages/age-format/src/engine/layout.mjs`（带 EA 出处、由守卫回语料复核）。
 *    **为什么**：模拟器是按建模语义**重新实现**的东西；一旦它内部写死"某字段在第几号字节"，
 *    它就变成"这份导出的附庸" —— 换一次导出/换一份镜像，实现就得跟着改。
 *    分开之后：**偏移可以被替换，语义不必跟着动**；而偏移错了守卫会红。
 *
 * ## 为什么这里必须依赖编解码（而不是"一个纯数组"）
 * int 族槽存的是**编码后的位模式** ⇒ 没有 `key` 一个 int 槽都读不出来（见 `value-codec.mjs` 的头注）。
 * float 族**不编码**。这两条**不是**实现细节，而是"读出来对不对"的判据。
 *
 * ## 已知未核实（显式列出，不许当成事实用）
 * * `local_string` 池的**元素宽度 28 字节**（SSO）尚未逐字复核 ⇒ 标为未核实
 *   （见知识层 `packages/age-format/src/engine/layout.mjs` 的 `ELEM_BYTES_VERIFIED`）。
 * * 各池的**真实元素数**（容量）：只有 int 池有二手数字，且是"同一字段两种记法"的混淆（见台账那条的更正记录）。
 *   所以本层**不写容量** —— 用稀疏 Map，`null` 表示"这里没东西"，而不是编一个数出来。
 */

import { decInt, encInt, encZero, intSlotOffset } from '@amayui/age-format/src/asm/value-codec.mts';

/** 一个槽里装得下的东西 */
export type SlotValue = number | string;

/** 一个 local 池的**语义定义**（★ 只有语义：没有帧内计数槽/基址槽 —— 那是布局） */
export interface LocalPoolDef {
  /** 池名（本仓起的稳定名；不是引擎里的符号） */
  name: string;
  /** 值的族：int 族过编解码，float / string 原样 */
  kind: 'int' | 'float' | 'string';
  /** 一个元素占几字节（`string` 是定长 SSO 记录） */
  elemBytes: number;
  /** 是否过 DEC/ENC */
  encoded: boolean;
  /** operand type tag（9..14 与这 6 个池一一对应） */
  typeTag: number;
}

/** 一次"引擎不会检查"的访问记录 */
export interface OobRecord {
  tag: number;
  idx: number;
  kind: string;
}

/** 6 个 local 池（语义；与 operand type 9..14 对应） */
export const LOCAL_POOLS: LocalPoolDef[] = [
  { name: 'int', kind: 'int', elemBytes: 4, encoded: true, typeTag: 9 },
  { name: 'float', kind: 'float', elemBytes: 4, encoded: false, typeTag: 10 },
  { name: 'string', kind: 'string', elemBytes: 28, encoded: false, typeTag: 11 },
  { name: 'ptr', kind: 'int', elemBytes: 4, encoded: true, typeTag: 12 },
  { name: 'floatPtr', kind: 'float', elemBytes: 4, encoded: false, typeTag: 13 },
  { name: 'stringPtr', kind: 'int', elemBytes: 4, encoded: true, typeTag: 14 },
];

/** global 池族的**语义名**（`*Ref` 是从别处取来的一份引用；`*_alt` 是 memflip 的第二份缓冲） */
export const GLOBAL_POOL_NAMES = ['int', 'float', 'string', 'intRef', 'floatRef', 'stringRef'] as const;
export type GlobalPoolName = (typeof GLOBAL_POOL_NAMES)[number];

/** global 里哪些名要过 DEC/ENC */
export const GLOBAL_ENCODED: Record<string, boolean> = {
  int: true, intRef: true, float: false, floatRef: false, string: false, stringRef: false,
};

/** 按 operand type tag 找一个 local 池（9..14）；不是 local 池的 tag ⇒ `null` */
export const localPoolByTypeTag = (tag: number): LocalPoolDef | null =>
  LOCAL_POOLS.find((p) => p.typeTag === tag) ?? null;

/**
 * 一个**帧**的 local 池视图。
 *
 * ★ 故意的两个取舍：
 * 1. **用稀疏 `Map`**（不是定长数组）：容量未知 ⇒ 不许编一个数出来。未初始化读 ⇒ `null`（**不是 0**
 *    —— 引擎对 int 族的初值是 `enc_zero`，那是个非 0 的位模式）。
 * 2. **不做越界检查**：引擎也没有 ⇒ 但"引擎没做的事"必须**显式留痕**（`noteOOB`），而不是悄悄替它 clamp。
 */
export class LocalPools {
  /** int 族编解码用的 key（`ENC`/`DEC` 的第二参；引擎里它是运行期赋值的，不是常数） */
  readonly key: number;
  readonly pools: Map<string, Map<number, SlotValue>>;
  readonly oob: OobRecord[];

  constructor(key: number) {
    this.key = key >>> 0;
    this.pools = new Map(LOCAL_POOLS.map((p) => [p.name, new Map<number, SlotValue>()]));
    this.oob = [];
  }

  /** 读一个槽（int 族过 DEC）。未初始化 ⇒ `null` */
  read(tag: number, idx: number): SlotValue | null {
    const def = localPoolByTypeTag(tag);
    if (!def) throw new Error(`不是 local 池的 operand type：${tag}`);
    const raw = this.pools.get(def.name)!.get(idx);
    if (raw === undefined) return null;
    if (!def.encoded) return raw;
    return decInt(raw as number, this.key);
  }

  /** 写一个槽（int 族过 ENC）。返回值 = 落到"内存"里的位模式 */
  write(tag: number, idx: number, value: SlotValue): SlotValue {
    const def = localPoolByTypeTag(tag);
    if (!def) throw new Error(`不是 local 池的 operand type：${tag}`);
    const bits: SlotValue = def.encoded ? encInt(value as number, this.key) : value;
    this.pools.get(def.name)!.set(idx, bits);
    return bits;
  }

  /** 引擎的初值口径：int 族填 `enc_zero`（**不是 0**），其余填 0 */
  initZero(tag: number, count: number): void {
    const def = localPoolByTypeTag(tag);
    if (!def) throw new Error(`不是 local 池的 operand type：${tag}`);
    const init: SlotValue = def.encoded ? encZero(this.key) : 0;
    for (let i = 0; i <= count; i += 1) this.pools.get(def.name)!.set(i, init);
  }

  /** 记一次"引擎不会检查"的访问（越界 / 未初始化）。★ 只记录，不改语义。 */
  noteOOB(tag: number, idx: number, kind: string): void {
    this.oob.push({ tag, idx, kind });
  }
}

/**
 * 一个**全局池族**的视图。与 `LocalPools` 的差别：它是引擎级的（跨脚本保留），
 * 而且**不随装载脚本帧而重建**（旧仓 capability `script-frame-local-pool-lifecycle` 的口径）。
 */
export class GlobalPools {
  readonly key: number;
  readonly pools: Map<string, Map<number, SlotValue>>;

  constructor(key: number) {
    this.key = key >>> 0;
    this.pools = new Map(GLOBAL_POOL_NAMES.map((n) => [n, new Map<number, SlotValue>()]));
  }

  /** `base + idx*4`（★ 下标不过编码 —— 它是纯算术，不参与 DEC/ENC） */
  static slotOffset(idx: number): number {
    return intSlotOffset(idx);
  }

  /** 读一个全局池槽（`int` / `intRef` 过 DEC）。未初始化 ⇒ `null` */
  read(name: string, idx: number): SlotValue | null {
    const raw = this.pools.get(name)?.get(idx);
    if (raw === undefined) return null;
    return GLOBAL_ENCODED[name] ? decInt(raw as number, this.key) : raw;
  }

  /** 写一个全局池槽（`int` / `intRef` 过 ENC）。返回值 = 落到内存里的位模式 */
  write(name: string, idx: number, value: SlotValue): SlotValue {
    if (!this.pools.has(name)) throw new Error(`未知的全局池：${name}`);
    const bits: SlotValue = GLOBAL_ENCODED[name] ? encInt(value as number, this.key) : value;
    this.pools.get(name)!.set(idx, bits);
    return bits;
  }
}
