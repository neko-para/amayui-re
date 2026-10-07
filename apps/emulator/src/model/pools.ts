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
 * ⇒ 那些是**逆向观察**，在 `packages/age-format/src/engine/layout.mts`（带 EA 出处、由守卫回语料复核）。
 *    **为什么**：模拟器是按建模语义**重新实现**的东西；一旦它内部写死"某字段在第几号字节"，
 *    它就变成"这份导出的附庸" —— 换一次导出/换一份镜像，实现就得跟着改。
 *    分开之后：**偏移可以被替换，语义不必跟着动**；而偏移错了守卫会红。
 *
 * ## 为什么这里必须依赖编解码（而不是"一个纯数组"）
 * int 族槽存的是**编码后的位模式** ⇒ 没有 `key` 一个 int 槽都读不出来（见 `value-codec.mts` 的头注）。
 * float 族**不编码**。这两条**不是**实现细节，而是"读出来对不对"的判据。
 *
 * ## 快照与恢复：**接缝已就位，但故意没有文件格式**
 * `snapshot()` / `restore()` 只保证一件事 —— **同样状态给出逐字节相同的纯数据**
 * （池名与下标升序、稀疏保真、必带 `key`）。★ 故意**不**定文件格式与版本号：
 * 今天还没有"状态"的第二个来源（没有引擎对象、没有执行循环），此刻定格式只能靠猜。
 * 口径见 `apps/emulator/README.md` 的「状态与快照」；注入点见本文件末尾的 `STATE_PARTITION`。
 *
 * ## 已知未核实（显式列出，不许当成事实用）
 * * `local_string` 池的**元素宽度 28 字节**（SSO）尚未逐字复核 ⇒ 标为未核实
 *   （见知识层 `packages/age-format/src/engine/layout.mts` 的 `ELEM_BYTES_VERIFIED`）。
 * * 各池的**真实元素数**（容量）：只有 int 池有二手数字，且是"同一字段两种记法"的混淆（见台账那条的更正记录）。
 *   所以本层**不写容量** —— 用稀疏 Map，`null` 表示"这里没东西"，而不是编一个数出来。
 */

import { decInt, encInt, encZero, intSlotOffset } from '@amayui/age-format/src/asm/value-codec.mts';
import { AddressSpace } from './address-space.ts';
import { bitsFromFloat, floatFromBits } from './float-bits.ts';
import type { Region } from './address-space.ts';

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

/**
 * 一个**可变字段**的类别。★ 闭集合，而且**每个自有字段必须归一类**
 * （守卫 `tools/test/emulator-state-partition.test.mjs` 用反射核对，新增字段忘了归类就红）。
 *
 * * `engine` —— 引擎态。**进**快照；两份快照逐字节可比。
 * * `derived` —— 由引擎态算得。不进快照：灌回去要么无意义、要么与引擎态打架。
 * * `diagnostic` —— 模型的**留痕**（例如"引擎不做越界检查"这件事本身）。不进快照：
 *   它不是引擎态，而且"恢复后重跑到第 N 帧"与"自然跑到第 N 帧"在它上面**必然**不同
 *   （前者没经历过那些访问）—— 混进去只会让等价判据变红，且红得没意义。
 * * `host` —— 宿主注入的东西（回调 / 桥）。不进快照：恢复不该把调用者的闭包换掉。
 */
export type StateClass = 'engine' | 'derived' | 'diagnostic' | 'host';

/**
 * 一份**池族快照**（纯数据 ⇒ 可 `JSON.stringify` 往返、可 diff、可进 git）。
 *
 * ★ 三条口径（都由守卫钉住）：
 * 1. **必带 `key`**：int 族槽里存的是**编码位模式**，没有 `key` 就不可解释 ——
 *    `value-codec.mts` 连默认值都不给（静默用 0 = 把"我不知道 key"伪装成"key 是 0"），快照同样**不许简写**。
 * 2. **规范化**：池名与下标一律**升序**。`Map` 的迭代顺序是**插入顺序**
 *    （实测：对已存在的键再 `set` **不**改变位置）⇒ 不排的话，"同一状态、不同写入历史"会给出不同字节；
 *    而恢复又按键序重建 ⇒ 与自然运行在顺序上分叉。
 * 3. **稀疏保真**：只记**在场**的槽。"这里没东西"与"这里的值是 0"是两件事 ——
 *    int 族的初值是 `enc_zero`，那**不是** 0。
 *
 * ★ **不存解码值**：那是第二份真源，会和位模式打架。要看解码值就在恢复出来的实例上调 `read()`。
 */
export interface PoolsSnapshot {
  key: number;
  /** 池名 → **按下标升序**的 `[下标, 位模式|原值]`；池名本身也按键升序 */
  pools: [string, [number, SlotValue][]][];
}

/** `oob` 的**摘要**（不是原始数组）—— 长度随执行步数涨，但"引擎没做的事"这件事必须留痕 */
export interface OobSummary {
  total: number;
  /** `kind` → 次数（按键升序） */
  byKind: [string, number][];
  /** 头 `limit` 条原始记录（便于定位） */
  first: OobRecord[];
}

/**
 * 6 个 local 池（语义；与 operand type 9..14 对应）。
 *
 * ★★ **`encoded` 已订正（2026-10，逐字判据）**：`ptr` / `stringPtr` **不是** `encoded`
 * —— 指针池的格内容就是**原始地址**，读的时候**不 DEC**，而是"取格 → 解引用 → 对解出的 dword DEC"。
 * 判据（`sub_41BF50` 的两支对比，锚 = EA）：
 * ```
 *   case 9（局部 int）0x41C01A: mov eax,[ecx+esi*8+5D8B4h] / mov edx,[eax+edx*4]
 *                              → rol 0Bh / xor [5EC8Ch] / ror 19h        ★ 取格后 **DEC**
 *   case 12（局部 ptr）0x41C049: mov eax,[ecx+esi*8+5D8C0h] / mov edx,[eax+edx*4]
 *                              / mov eax,[edx]                          ★ **不 DEC，直接当地址**
 *                              → rol 0Bh / xor [5EC8Ch] / ror 19h        ★ DEC 作用于**解引用出来的** dword
 * ```
 * ⇒ 若按"指针格也编码"实现，`lookup-array` → `save-int` 这条链会解引用 `ENC(地址)` = 垃圾
 * （而两条路都"看起来正常"）。
 */
export const LOCAL_POOLS: LocalPoolDef[] = [
  { name: 'int', kind: 'int', elemBytes: 4, encoded: true, typeTag: 9 },
  { name: 'float', kind: 'float', elemBytes: 4, encoded: false, typeTag: 10 },
  { name: 'string', kind: 'string', elemBytes: 28, encoded: false, typeTag: 11 },
  { name: 'ptr', kind: 'int', elemBytes: 4, encoded: false, typeTag: 12 },
  { name: 'floatPtr', kind: 'float', elemBytes: 4, encoded: false, typeTag: 13 },
  { name: 'stringPtr', kind: 'int', elemBytes: 4, encoded: false, typeTag: 14 },
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
 * local 池的 type tag 集合（**从 `LOCAL_POOLS` 派生，不另写一份**）。
 * ★ 消费方（操作数层）拿它来回答"这个 type 我认不认得" —— 而"认不认得"必须与
 *   "池定义里有哪些"同源，否则加一个池会让两层不一起动（症状是**静默走错分支**）。
 */
export const localPoolTypeTags = (): number[] => LOCAL_POOLS.map((p) => p.typeTag);

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
  /**
   * ★★ **迁移到区域的池**（只有 `kind === 'int'` 的那些：`int` / `ptr` / `stringPtr`）。
   *
   * 为什么是它们：这些池**本来存的就是位模式**（`write` 存 `encInt(...)`、`read` 做 `decInt`）
   * ⇒ 区域的一格（4 字节）**恰好**就是那个位模式 ⇒ 迁过去**不改语义**，只是"值存在哪"变了。
   * float 池存的是 JS 数值、string 池是 28 字节 SSO ⇒ 留到后面（见需求树）。
   *
   * ★ 只有构造时给了 `space` 才有内容；没给 ⇒ 走老的 `Map` 路径（**一套代码两条存储**，
   *   这是迁移中途的既成事实，不是终态）。终态见 ADR：**一份数据**。
   */
  readonly regions: Map<string, Region>;
  /** 承载区域的地址空间（`null` = 还没迁） */
  readonly space: AddressSpace | null;

  constructor(key: number, pools?: Map<string, Map<number, SlotValue>>, opts: { space?: AddressSpace | null } = {}) {
    this.key = key >>> 0;
    this.pools = pools ?? new Map(LOCAL_POOLS.map((p) => [p.name, new Map<number, SlotValue>()]));
    // ★ 不变式：实例必须**恰好**覆盖本类认得的池。`read()` 内部是 `this.pools.get(def.name)!.get(idx)`
    //   ⇒ 缺一个池就是**运行期崩**。这条放在构造器里 ⇒「快照缺池」与「有人手工塞一份半份池」
    //   两条路都在这里红，于是**凡是能构造出来的实例，`read()` 都是全的**。
    assertPoolCoverage(this.pools, LOCAL_POOLS.map((p) => p.name));
    this.oob = [];
    this.space = opts.space ?? null;
    this.regions = new Map();
    if (this.space) {
      for (const def of LOCAL_POOLS) {
        if (!MIGRATED_TO_REGION(def)) continue;
        // ★ 容量**初值 0**（决策 REQ-01M4B969TBWVERFCB1MXS2Q2E1：不编常数，按需增长）
        const r = this.space.alloc({ tag: `local:${def.name}`, elemBytes: def.elemBytes, capacity: 0 });
        this.regions.set(def.name, r);
        // ★ 把（可能来自快照的）Map 里的值**搬进区域**，搬完清空 ⇒ 数据只有一份。
        //   ★ float 族**必须过 `valueToCell`**：快照/Map 里存的是**数值**，而区域里要的是**位模式**
        //     （直接把数值写成 u32 会得到 1.5 → 1 —— 守卫当场抓到过）。
        const src = this.pools.get(def.name)!;
        for (const [idx, value] of src) {
          this.space.ensureCapacity(r, idx, `${def.name} 池：快照搬家`);
          this.space.writeU32(r.addressOf(idx), snapshotToCell(def, value));
        }
        src.clear();
      }
    }
  }

  /** 读一个槽（int 族过 DEC）。未初始化 ⇒ `null` */
  read(tag: number, idx: number): SlotValue | null {
    const def = localPoolByTypeTag(tag);
    if (!def) throw new Error(`不是 local 池的 operand type：${tag}`);
    const reg = this.regions.get(def.name);
    if (reg) {
      // ★ 读也要按需增长：局部池的容量来自脚本头，而**这份实例的头部计数可能没被喂进来**
      //   ⇒ 与其"编一个容量"或"悄悄返回 null"，不如**显式增长并留痕**（每次都会进副作用日志）。
      this.space!.ensureCapacity(reg, idx, `${def.name} 池：读第 ${idx} 格`);
      const raw = this.space!.readU32(reg.addressOf(idx));
      if (raw === null) return null;
      return cellToValue(def, raw, this.key);
    }
    const raw = this.pools.get(def.name)!.get(idx);
    if (raw === undefined) return null;
    // ★ Map 路径：编码族取解码值，其余（float/string）**就是值本身**
    if (!def.encoded) return raw;
    return decInt(raw as number, this.key);
  }

  /** 写一个槽（int 族过 ENC、float 族收敛成 float32）。返回值 = 落到"内存"里的位模式 */
  write(tag: number, idx: number, value: SlotValue): SlotValue {
    const def = localPoolByTypeTag(tag);
    if (!def) throw new Error(`不是 local 池的 operand type：${tag}`);
    const reg = this.regions.get(def.name);
    if (reg) {
      // ★ 区域路径：格内容 = **位模式**（float 族要过 `bitsFromFloat`，走 `valueToCell` 的统一口径）
      const bits = valueToCell(def, value, this.key);
      this.space!.ensureCapacity(reg, idx, `${def.name} 池：写第 ${idx} 格`);
      this.space!.writeU32(reg.addressOf(idx), bits);
      return bits;
    }
    // ★ Map 路径（迁移中途的兼容路径）：口径与迁移前**逐字一致** —— 编码族存位模式，其余存**值本身**
    //   （float 族在这里存的是 JS 数值；两条路"格内容"本就不同，换算只在区域路径做 —— 这一点由
    //    "两种存储的快照必须逐值相同"那条守卫钉住）
    const bits: SlotValue = def.encoded ? encInt(value as number, this.key) : value;
    this.pools.get(def.name)!.set(idx, bits);
    return bits;
  }

  /** 引擎的初值口径：int 族填 `enc_zero`（**不是 0**），其余填 0（float 的 0 位模式也是 0） */
  initZero(tag: number, count: number): void {
    const def = localPoolByTypeTag(tag);
    if (!def) throw new Error(`不是 local 池的 operand type：${tag}`);
    const init: SlotValue = def.encoded ? encZero(this.key) : 0;
    const reg = this.regions.get(def.name);
    if (reg) {
      // ★ 直接写**位模式**（不过 ENC）：`enc_zero` 是"已经编码好的 0"
      this.space!.ensureCapacity(reg, count, `${def.name} 池：initZero`);
      for (let i = 0; i <= count; i += 1) this.space!.writeU32(reg.addressOf(i), init as number);
      return;
    }
    for (let i = 0; i <= count; i += 1) this.pools.get(def.name)!.set(i, init);
  }

  /** 记一次"引擎不会检查"的访问（越界 / 未初始化）。★ 只记录，不改语义。 */
  noteOOB(tag: number, idx: number, kind: string): void {
    this.oob.push({ tag, idx, kind });
  }

  /**
   * 池 → **区域**（取址用）。没迁到区域 ⇒ **抛**（取址需要池已经有地址 —— 这正是 ADR 第 ② 步的意义）。
   * ⛔ 不许"顺手建一个"：那会让"这个池还没迁"与"这个池是空的"变得不可区分。
   */
  regionOf(tag: number): Region {
    const def = localPoolByTypeTag(tag);
    if (!def) throw new Error(`不是 local 池的 operand type：${tag}`);
    const r = this.regions.get(def.name);
    if (!r) {
      throw new Error(
        `局部池 ${def.name} 还没有区域 ⇒ **取址不可用**（ADR 第 ② 步的迁移只覆盖 kind === 'int' 的族）`,
      );
    }
    return r;
  }

  /**
   * 一份**规范化快照**（纯数据）。★ 口径见 `PoolsSnapshot` 的三条。
   * `oob` **不**进快照 —— 它是**诊断**，不是引擎态（见 `STATE_PARTITION`）。
   * ★ **格式与迁移前逐字相同**（`[池名, [下标, 位模式][]]`）：迁到区域的池从**区域**取出同样的位模式
   *   ⇒ 快照读法、分区表、恢复路径都不用改（这是"迁移不改外部形状"那条口径的落地）。
   */
  snapshot(): PoolsSnapshot {
    return { key: this.key, pools: canonicalPools(this.pools, this.regions, { key: this.key, kindOf: localKindOf }) };
  }

  /**
   * 从快照**构造**一份新实例。
   * ★ 不是"往现有实例里灌"：`key` 是只读的，而且"恢复"不该悄悄改掉调用者手上的对象。
   * ★ 快照里任何不认识的东西都**响亮失败**（未知池名 / 缺池 / 下标重复 / 值类型不符），不静默跳过。
   */
  static restore(snap: PoolsSnapshot, opts: { space?: AddressSpace | null } = {}): LocalPools {
    const pools = expandPools(snap, LOCAL_POOLS.map((p) => p.name), (n) => {
      const kind = localPoolKindOf(n);
      return kind === null ? null : kind === 'string' ? 'string' : 'number';
    });
    // ★ 给了 space ⇒ 构造器会把快照里的值**搬进区域并清空 Map**（数据只有一份）
    return new LocalPools(snap.key, pools, opts);
  }

  /** `oob` 的**摘要**（全量数组会随执行步数无限涨 ⇒ 不进快照，但它必须可被看见） */
  oobSummary(limit = 8): OobSummary {
    return summarizeOob(this.oob, limit);
  }
}

/**
 * 一个**全局池族**的视图。与 `LocalPools` 的差别：它是引擎级的（跨脚本保留），
 * 而且**不随装载脚本帧而重建**（旧仓 capability `script-frame-local-pool-lifecycle` 的口径）。
 */
export class GlobalPools {
  readonly key: number;
  readonly pools: Map<string, Map<number, SlotValue>>;
  /** ★ 见 `LocalPools.regions`：**只有存位模式的那两族**（`int` / `intRef`）迁到区域 */
  readonly regions: Map<string, Region>;
  /** 承载区域的地址空间（`null` = 还没迁） */
  readonly space: AddressSpace | null;

  constructor(key: number, pools?: Map<string, Map<number, SlotValue>>, opts: { space?: AddressSpace | null } = {}) {
    this.key = key >>> 0;
    this.pools = pools ?? new Map(GLOBAL_POOL_NAMES.map((n) => [n, new Map<number, SlotValue>()]));
    // ★ 同 `LocalPools`：认得的池一个都不许缺（`read()` 会因此运行期崩）
    assertPoolCoverage(this.pools, [...GLOBAL_POOL_NAMES]);
    this.space = opts.space ?? null;
    this.regions = new Map();
    if (this.space) {
      for (const name of GLOBAL_POOL_NAMES) {
        // ★ 判据 = "**格内容就是一个 4 字节位模式**"：int 族（过编解码）+ float 族（float32 位模式）。
        //   ⛔ 只剩 `string`/`stringRef`（28 字节）没迁 —— 它要 SSO + 长串的堆。
        if (GLOBAL_KIND[name] === 'string') continue;
        const r = this.space.alloc({ tag: `global:${name}`, elemBytes: 4, capacity: 0 });
        this.regions.set(name, r);
        const src = this.pools.get(name)!;
        const gdef = globalKindOf(name);
        for (const [idx, value] of src) {
          this.space.ensureCapacity(r, idx, `${name} 池：快照搬家`);
          this.space.writeU32(r.addressOf(idx), snapshotToCell(gdef, value));
        }
        src.clear();
      }
    }
  }

  /** `base + idx*4`（★ 下标不过编码 —— 它是纯算术，不参与 DEC/ENC） */
  static slotOffset(idx: number): number {
    return intSlotOffset(idx);
  }

  /** 池 → **区域**（取址用）；没迁 ⇒ 抛。口径同 `LocalPools.regionOf` */
  regionOf(name: string): Region {
    if (!this.pools.has(name)) throw new Error(`未知的全局池：${name}`);
    const r = this.regions.get(name);
    if (!r) throw new Error(`全局池 ${name} 还没有区域 ⇒ **取址不可用**（迁移只覆盖过编解码的族）`);
    return r;
  }

  /**
   * 读一个全局池槽（`int` / `intRef` 过 DEC）。未初始化 ⇒ `null`。
   * ★ 池名不认识就**抛**（与 `write` 对称）：静默返回 `null` 会让"池名写错"与"这个槽是空的"
   *   不可区分 —— 灌一份写错池名的快照，表现出来就是"那个池是空的"。
   */
  read(name: string, idx: number): SlotValue | null {
    const pool = this.pools.get(name);
    if (!pool) throw new Error(`未知的全局池：${name}`);
    const reg = this.regions.get(name);
    if (reg) {
      this.space!.ensureCapacity(reg, idx, `${name} 池：读第 ${idx} 格`);
      const raw = this.space!.readU32(reg.addressOf(idx));
      if (raw === null) return null;
      // ★ 与 `LocalPools.read` 同一条换算（float 族在这里也是位模式 ⇒ 要转回数值）
      return cellToValue(globalKindOf(name), raw, this.key);
    }
    const raw = pool.get(idx);
    if (raw === undefined) return null;
    return GLOBAL_ENCODED[name] ? decInt(raw as number, this.key) : raw;
  }

  /** 写一个全局池槽（`int` / `intRef` 过 ENC）。返回值 = 落到内存里的位模式 */
  write(name: string, idx: number, value: SlotValue): SlotValue {
    if (!this.pools.has(name)) throw new Error(`未知的全局池：${name}`);
    const reg = this.regions.get(name);
    if (reg) {
      const bits = valueToCell(globalKindOf(name), value, this.key);
      this.space!.ensureCapacity(reg, idx, `${name} 池：写第 ${idx} 格`);
      this.space!.writeU32(reg.addressOf(idx), bits);
      return bits;
    }
    const bits: SlotValue = GLOBAL_ENCODED[name] ? encInt(value as number, this.key) : value;
    this.pools.get(name)!.set(idx, bits);
    return bits;
  }

  /** 一份**规范化快照**（纯数据；口径同 `LocalPools.snapshot()`，格式**逐字相同**） */
  snapshot(): PoolsSnapshot {
    // ★ 这里必须用 **global** 的族查找（用错会在快照时抛"不是 local 池：intRef" —— 实测踩过）
    return { key: this.key, pools: canonicalPools(this.pools, this.regions, { key: this.key, kindOf: globalKindOf }) };
  }

  /**
   * 从快照**构造**一份新实例（同 `LocalPools.restore()`）。
   * ★ 值类型只做**粗校验**（`number | string`）：global 的 `*Ref` 到底装"引用"还是装"串"**未核实**
   *   （`GLOBAL_ENCODED` 说它**不过**编解码，而 local 的 `stringPtr` 是过编解码的 —— 两边不同形）
   *   ⇒ 这里**不按名字猜**，只挡住"明显不是值"的东西。
   */
  static restore(snap: PoolsSnapshot, opts: { space?: AddressSpace | null } = {}): GlobalPools {
    const pools = expandPools(snap, [...GLOBAL_POOL_NAMES], () => null);
    return new GlobalPools(snap.key, pools, opts);
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// 状态分区与快照接缝（★ 口径见 `apps/emulator/README.md` 的「状态与快照」）
// ─────────────────────────────────────────────────────────────────────────────

/**
 * ★ 状态分区的**可执行形式**：类名 → 字段名 → 类别。
 *
 * 口径：**`engine` 类的字段恰好就是快照的顶层键**。这条不是巧合 —— 它让"新增一个字段"
 * 变成一件必须表态的事（守卫会反射核对），而不是"顺手 `this.foo = 0`"。
 * ⇒ 以后加 handler 时，凡是要进快照的量都得先在这里出现。
 *
 * ★ 今天这张表只有两个池视图。**这是诚实的现状**：模型里还没有引擎对象、没有执行循环，
 *   所以"非确定源"（`0x60 random` 的 RNG 与重掷计数器）与"碰引擎状态的那 4 条"
 *   （`numeric-ops.ts` 的 `TOUCHES_ENGINE_STATE`）**尚无承载面** —— 它们一落地就必须进这张表。
 */
export const STATE_PARTITION: Record<string, Record<string, StateClass>> = {
  // ★ `space`：**注入的引用**（数据在 `Machine.space` 那一项里计账 ⇒ 这里只是"谁承载"）⇒ host。
  //   `regions`：池名 → 区域的索引，可**从空间按 tag 重算** ⇒ derived（不进快照）。
  //   ⛔ 两条都不是"第二份数据"：位模式只存在区域里（迁移后 Map 是空的）。
  LocalPools: { key: 'engine', pools: 'engine', oob: 'diagnostic', space: 'host', regions: 'derived' },
  GlobalPools: { key: 'engine', pools: 'engine', space: 'host', regions: 'derived' },
};

/** 池名 → 值的族（从 `LOCAL_POOLS` 派生，**不另写一份**）；不是 local 池 ⇒ `null` */
const localPoolKindOf = (name: string): LocalPoolDef['kind'] | null =>
  LOCAL_POOLS.find((p) => p.name === name)?.kind ?? null;

/** `Map` → **按键升序**的 `[k, v]` 对。★ 规范化：结果与**写入历史无关** */
function sortedPairs<V>(m: Map<number, V>): [number, V][] {
  return [...m.entries()].sort((a, b) => a[0] - b[0]);
}

/** 池族 → 规范化快照（★ 池名也排：`Map` 的迭代顺序是插入顺序，不是声明的顺序） */
/**
 * 哪些池**已经迁到区域**：`kind === 'int'`（= `int` / `ptr` / `stringPtr`）。
 *
 * ★ 判据是"**存的是位模式**"：这三族的 `write` 存 `encInt(...)`、`read` 做 `decInt`
 *   ⇒ 区域的一格（4 字节）恰好就是那个位模式。float 池存 JS 数值、string 池是 28 字节 SSO
 *   ⇒ 它们要额外的编码步骤，**留到后面**（需求树里点名了）。
 */
/**
 * 哪些池**已经迁到区域**：`int` 与 `float` 两族（都是 4 字节格）。
 *
 * ★ 判据是"**格内容就是一个 4 字节位模式**"：
 *   * `int` / `ptr` / `stringPtr`：格内容 = 编码后的位模式（或原始地址）；
 *   * `float` / `floatPtr`：格内容 = **float32 位模式**（`fstp dword ptr` 取证）。
 * ⛔ 只剩 **`string`（28 字节）**没迁 —— 它要 SSO 那块（见需求树 `REQ-01M4BEHCHZE5QDHCNTA77V8ZJG`）。
 */
const MIGRATED_TO_REGION = (def: localPoolDefShape): boolean => (def.kind === 'int' || def.kind === 'float') && def.elemBytes === 4;

/**
 * **格内容 → 快照里的值**。
 *
 * ★★ 它**不等于** `cellToValue`（`read()` 那条），差别是**故意的**：
 * 快照的口径是"**与 Map 路径存的东西逐值相同**"（`PoolsSnapshot` 写着 `[下标, 位模式][]`）——
 *   * 编码族（int）在 Map 路径里存的就是**位模式** ⇒ 快照也写位模式（**不做 DEC**）；
 *   * float 族在 Map 路径里存的是**数值** ⇒ 快照写数值；
 *   * ptr 族 `encoded: false` ⇒ 存的就是地址本身，两边一致。
 * ⛔ 若让快照走 `cellToValue`（带 DEC），int 族的快照会从位模式变成解码值 ⇒ **改了外部形状**
 *   （本仓实测被"两种存储的快照必须逐值相同"那条守卫抓到）。
 */
function cellToSnapshot(def: { kind: string }, bits: number): SlotValue {
  return def.kind === 'float' ? floatFromBits(bits) : bits;
}
function cellToValue(def: { kind: string; encoded: boolean }, bits: number, key: number): SlotValue {
  if (def.kind === 'float') return floatFromBits(bits);
  return def.encoded ? decInt(bits, key) : bits;
}

/** 值 → **格内容（位模式）**（`cellToValue` 的逆） */
function valueToCell(def: { kind: string; encoded: boolean }, value: SlotValue, key: number): number {
  if (def.kind === 'float') return bitsFromFloat(value as number) >>> 0;
  return (def.encoded ? encInt(value as number, key) : (value as number)) >>> 0;
}

/**
 * **快照里的值 → 格内容（位模式）**（`cellToSnapshot` 的逆）。
 * ★ 与 `valueToCell` 的差别同样**是故意的**：快照里编码族已经是**位模式** ⇒ 直接搬，⛔ **不再 ENC**
 *   （再编码一次 = 双重编码；本仓实测被"快照往返"那条守卫抓到）。
 */
function snapshotToCell(def: { kind: string }, value: SlotValue): number {
  return (def.kind === 'float' ? bitsFromFloat(value as number) : (value as number)) >>> 0;
}

/** 池定义里这条判据要用的最小形状（`LOCAL_POOLS` 的元素满足它） */
interface localPoolDefShape {
  kind: string;
  elemBytes: number;
}

/** 区域里的**位模式**（4 字节小端）—— 与 `Map` 路径存的值**逐位相同**，所以快照格式不用改 */
function bitsFromCell(bytes: Uint8Array): number {
  return (bytes[0] | (bytes[1] << 8) | (bytes[2] << 16) | (bytes[3] << 24)) >>> 0;
}

/**
 * 规范化的池快照：**迁到区域的池从区域取**，其余从 `Map` 取 —— 两边给出的是**同一种数据**
 * （`[下标, 位模式][]`），所以快照格式、分区表、恢复路径都不必改。
 */
function canonicalPools(
  pools: Map<string, Map<number, SlotValue>>,
  regions: Map<string, Region> | undefined,
  meta: { key: number; kindOf: (name: string) => { kind: string; encoded: boolean } },
): [string, [number, SlotValue][]][] {
  return [...pools.keys()].sort().map((n) => {
    const reg = regions?.get(n);
    if (!reg) return [n, sortedPairs(pools.get(n)!)] as [string, [number, SlotValue][]];
    // ★ 快照里的值必须是**换算后**的值（float 族尤其：格内容 = 位模式，快照要的是数值）
    //   ⇒ 与 `read()` 走同一条换算（`cellToValue`），否则两条路会分叉 —— 那正是 ADR 那条判据要防的。
    const def = meta.kindOf(n);
    const cells: [number, SlotValue][] = [];
    for (const [offset, bytes] of reg.cells) {
      cells.push([offset / reg.elemBytes, cellToSnapshot(def, bitsFromCell(bytes))]);
    }
    cells.sort((a, b) => a[0] - b[0]);
    return [n, cells] as [string, [number, SlotValue][]];
  });
}

/** `LOCAL_POOLS` 的族查找（快照换算要用；名字不认识 ⇒ 抛，⛔ 不许猜） */
const localKindOf = (name: string): { kind: string; encoded: boolean } => {
  const d = LOCAL_POOLS.find((p) => p.name === name);
  if (!d) throw new Error(`不是 local 池：${name}`);
  return { kind: d.kind, encoded: d.encoded };
};

/** global 池名 → 族（`float`/`floatRef` 是浮点族；`encoded` 仍由 `GLOBAL_ENCODED` 说了算） */
const GLOBAL_KIND: Record<string, string> = {
  int: 'int', intRef: 'int', float: 'float', floatRef: 'float', string: 'string', stringRef: 'string',
};

/** `GLOBAL_KIND` 的查找（不认识 ⇒ 抛） */
const globalKindOf = (name: string): { kind: string; encoded: boolean } => {
  const k = GLOBAL_KIND[name];
  if (!k) throw new Error(`未知的全局池：${name}`);
  return { kind: k, encoded: GLOBAL_ENCODED[name] === true };
};

/**
 * 快照 → 池族（**逐步校验，任何不认识的东西都抛**）。
 *
 * ★ 两处"响亮失败"分别挡的是：灌错池名（表现为"那个池是空的"，与"这个槽未初始化"不可区分）、
 *   以及下标重复（后写覆盖先写 ⇒ 静默丢一格）。
 * ★ 「缺池」**不在这里**判 —— 那是**构造器的不变式**（`assertPoolCoverage`）：凡能构造出来的实例，
 *   `read()` 都是全的。这样"手工塞半份池"那条路也一并被挡住。
 *
 * @param snap   快照（可能来自外部文件 ⇒ 一律当**不可信输入**）
 * @param known  本类认得的池名**全集**（快照不许出现集合外的名字）
 * @param kindOf 该池值的族：`'string'` / `'number'`，或 `null` = **只做粗校验**（`number | string` 都收）。
 *   ★ `LocalPools` 按 `kind` 判；global 一律 `null` —— 它的 `*Ref` 到底装什么**未核实**，不按名字猜。
 */
function expandPools(
  snap: PoolsSnapshot,
  known: string[],
  kindOf: (name: string) => 'string' | 'number' | null,
): Map<string, Map<number, SlotValue>> {
  if (!Number.isInteger(snap?.key)) {
    throw new Error(`快照缺 key（或不是整数）：${JSON.stringify(snap?.key)} —— int 族槽存的是编码位模式，没有 key 就不可解释`);
  }
  if (!Array.isArray(snap.pools)) throw new Error('快照的 `pools` 不是数组');
  const out = new Map<string, Map<number, SlotValue>>();
  for (const entry of snap.pools) {
    if (!Array.isArray(entry) || entry.length !== 2) throw new Error(`快照里的池项不是 [池名, 槽] 对：${JSON.stringify(entry)}`);
    const [name, pairs] = entry;
    if (!known.includes(name)) throw new Error(`快照里的池名不在本类里：${JSON.stringify(name)}`);
    if (out.has(name)) throw new Error(`快照里池名重复：${name}`);
    if (!Array.isArray(pairs)) throw new Error(`池 ${name} 的槽不是数组`);
    const want = kindOf(name);
    const m = new Map<number, SlotValue>();
    for (const pair of pairs) {
      if (!Array.isArray(pair) || pair.length !== 2) throw new Error(`池 ${name} 的槽不是 [下标, 值] 对：${JSON.stringify(pair)}`);
      const [idx, val] = pair;
      if (!Number.isInteger(idx) || idx < 0) throw new Error(`池 ${name} 的下标非法：${JSON.stringify(idx)}`);
      if (m.has(idx)) throw new Error(`池 ${name} 的下标重复：${idx}`);
      const isStr = typeof val === 'string';
      if (want !== null && want === 'string' && !isStr) throw new Error(`池 ${name}[${idx}] 的值类型不符（该池装串，实际 ${typeof val}）`);
      if (want !== null && want === 'number' && isStr) throw new Error(`池 ${name}[${idx}] 的值类型不符（该池装数，实际 ${typeof val}）`);
      if (!isStr && typeof val !== 'number') throw new Error(`池 ${name}[${idx}] 的值不是 number|string：${typeof val}`);
      m.set(idx, val);
    }
    out.set(name, m);
  }
  return out;
}

/** 不变式：一份池族必须**恰好**覆盖本类认得的池名（缺一个 ⇒ `read()` 运行期崩） */
function assertPoolCoverage(pools: Map<string, Map<number, SlotValue>>, known: string[]): void {
  const missing = known.filter((n) => !pools.has(n));
  if (missing.length) throw new Error(`缺池：${missing.join(' / ')}（本类认得 ${known.length} 个池，实例必须覆盖它们全部）`);
}

/** `oob` → 摘要（`kind` 升序 ⇒ 同样状态给出同样摘要） */
function summarizeOob(oob: OobRecord[], limit: number): OobSummary {
  const counts = new Map<string, number>();
  for (const r of oob) counts.set(r.kind, (counts.get(r.kind) ?? 0) + 1);
  return {
    total: oob.length,
    byKind: [...counts.entries()].sort((a, b) => (a[0] < b[0] ? -1 : a[0] > b[0] ? 1 : 0)),
    first: oob.slice(0, Math.max(0, limit)),
  };
}
