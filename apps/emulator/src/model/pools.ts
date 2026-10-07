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

  constructor(key: number, pools?: Map<string, Map<number, SlotValue>>) {
    this.key = key >>> 0;
    this.pools = pools ?? new Map(LOCAL_POOLS.map((p) => [p.name, new Map<number, SlotValue>()]));
    // ★ 不变式：实例必须**恰好**覆盖本类认得的池。`read()` 内部是 `this.pools.get(def.name)!.get(idx)`
    //   ⇒ 缺一个池就是**运行期崩**。这条放在构造器里 ⇒「快照缺池」与「有人手工塞一份半份池」
    //   两条路都在这里红，于是**凡是能构造出来的实例，`read()` 都是全的**。
    assertPoolCoverage(this.pools, LOCAL_POOLS.map((p) => p.name));
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

  /**
   * 一份**规范化快照**（纯数据）。★ 口径见 `PoolsSnapshot` 的三条。
   * `oob` **不**进快照 —— 它是**诊断**，不是引擎态（见 `STATE_PARTITION`）。
   */
  snapshot(): PoolsSnapshot {
    return { key: this.key, pools: canonicalPools(this.pools) };
  }

  /**
   * 从快照**构造**一份新实例。
   * ★ 不是"往现有实例里灌"：`key` 是只读的，而且"恢复"不该悄悄改掉调用者手上的对象。
   * ★ 快照里任何不认识的东西都**响亮失败**（未知池名 / 缺池 / 下标重复 / 值类型不符），不静默跳过。
   */
  static restore(snap: PoolsSnapshot): LocalPools {
    const pools = expandPools(snap, LOCAL_POOLS.map((p) => p.name), (n) => {
      const kind = localPoolKindOf(n);
      return kind === null ? null : kind === 'string' ? 'string' : 'number';
    });
    return new LocalPools(snap.key, pools);
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

  constructor(key: number, pools?: Map<string, Map<number, SlotValue>>) {
    this.key = key >>> 0;
    this.pools = pools ?? new Map(GLOBAL_POOL_NAMES.map((n) => [n, new Map<number, SlotValue>()]));
    // ★ 同 `LocalPools`：认得的池一个都不许缺（`read()` 会因此运行期崩）
    assertPoolCoverage(this.pools, [...GLOBAL_POOL_NAMES]);
  }

  /** `base + idx*4`（★ 下标不过编码 —— 它是纯算术，不参与 DEC/ENC） */
  static slotOffset(idx: number): number {
    return intSlotOffset(idx);
  }

  /**
   * 读一个全局池槽（`int` / `intRef` 过 DEC）。未初始化 ⇒ `null`。
   * ★ 池名不认识就**抛**（与 `write` 对称）：静默返回 `null` 会让"池名写错"与"这个槽是空的"
   *   不可区分 —— 灌一份写错池名的快照，表现出来就是"那个池是空的"。
   */
  read(name: string, idx: number): SlotValue | null {
    const pool = this.pools.get(name);
    if (!pool) throw new Error(`未知的全局池：${name}`);
    const raw = pool.get(idx);
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

  /** 一份**规范化快照**（纯数据；口径同 `LocalPools.snapshot()`） */
  snapshot(): PoolsSnapshot {
    return { key: this.key, pools: canonicalPools(this.pools) };
  }

  /**
   * 从快照**构造**一份新实例（同 `LocalPools.restore()`）。
   * ★ 值类型只做**粗校验**（`number | string`）：global 的 `*Ref` 到底装"引用"还是装"串"**未核实**
   *   （`GLOBAL_ENCODED` 说它**不过**编解码，而 local 的 `stringPtr` 是过编解码的 —— 两边不同形）
   *   ⇒ 这里**不按名字猜**，只挡住"明显不是值"的东西。
   */
  static restore(snap: PoolsSnapshot): GlobalPools {
    const pools = expandPools(snap, [...GLOBAL_POOL_NAMES], () => null);
    return new GlobalPools(snap.key, pools);
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
  LocalPools: { key: 'engine', pools: 'engine', oob: 'diagnostic' },
  GlobalPools: { key: 'engine', pools: 'engine' },
};

/** 池名 → 值的族（从 `LOCAL_POOLS` 派生，**不另写一份**）；不是 local 池 ⇒ `null` */
const localPoolKindOf = (name: string): LocalPoolDef['kind'] | null =>
  LOCAL_POOLS.find((p) => p.name === name)?.kind ?? null;

/** `Map` → **按键升序**的 `[k, v]` 对。★ 规范化：结果与**写入历史无关** */
function sortedPairs<V>(m: Map<number, V>): [number, V][] {
  return [...m.entries()].sort((a, b) => a[0] - b[0]);
}

/** 池族 → 规范化快照（★ 池名也排：`Map` 的迭代顺序是插入顺序，不是声明的顺序） */
function canonicalPools(pools: Map<string, Map<number, SlotValue>>): [string, [number, SlotValue][]][] {
  return [...pools.keys()].sort().map((n) => [n, sortedPairs(pools.get(n)!)]);
}

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
