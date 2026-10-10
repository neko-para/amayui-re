/**
 * apps/emulator/src/model/engine-scalars.ts —— **引擎标量槽的寄存器堆**（★ 语义层：零 Node、零偏移）
 *
 * 启动链前段有一批 handler 的形状是"**读一个操作数 → 写引擎的某个 dword 标量**"（观察登记在知识层
 * `packages/age-format/src/engine/layout.mts` 的 `ENGINE_SCALAR_WRITES`）。不把值存下来，后面若有分支
 * 读同一个槽就会**静默走错**（不报错，只是路径不同）。键是知识层给的**稳定身份**（`Engine.d97058`），值是 u32。
 *
 * ★ 本层只保证"**写进去的东西，读出来还是它**"，**不解释**任何槽（没有名字/含义/读写规则）。
 *   口径与理由见知识台账：`data/ledger/`（域 `Emulator`，subject `model/engine-scalars-names-from-knowledge`）。
 * ★ **没写过的槽读 0**：引擎构造函数把 `Engine+0x5EC9C…0x5ECE8` 逐个写 0 —— 逐字证据 `.lst:33974-33994`
 *   （`33974 .text:00415C0F mov [esi+5EC9Ch],edi` … `33994 .text:00415C81 mov [esi+5ECE8h],edi`）。
 *   口径与理由见知识台账：`data/ledger/`（域 `Emulator`，subject `model/engine-scalars-default-zero`；
 *   取证 subject `Engine+0x5EC9C..0x5ECE8/ctor-zero-fill`，该条 `system` 域尚未填）。
 * ★ 与 int 池的初值 `encZero`（**非 0 的位模式**）**不是**一回事 —— 两条取证结论不一样，别混。
 * ★ 稀疏 + 快照：只存**被写过**的槽（`Map<name, u32>`）⇒ 快照与写入历史无关（按键升序）。
 */

/** 一个槽的值：u32 */
export type ScalarValue = number;

/**
 * ★★ **构造函数清零的那 20 格**的绝对偏移（`Engine+0x5EC9C..0x5ECE8`，步长 4）。
 *
 * 逐字证据：`.lst:33974-33993`（EA `0x415C0F..0x415C81`）连续 20 条 `mov [esi+5EC9Ch..5ECE8h], edi`，
 * 而 `edi` 在**同一函数里**被 `xor edi, edi` 清过（`.lst:33680`，其后到这批写之前没有别的 `edi` 写）
 * ⇒ **这些槽的"没写过"就是 0**。台账 subject：`Engine+0x5EC9C..0x5ECE8/ctor-zero-fill`。
 * ★ **本常量是给守卫用的可再校验锚**（`tools/test/emulator-engine-scalars.assets.test.mjs` 拿它去
 *   `.lst` 里逐字对；改这里 ⇒ 守卫当场红）。它**不是**模型行为的一部分：`read()` 的"没写过 ⇒ 0"
 *   是那 20 格的结果，本层不按格子存值（稀疏 `Map`，见类注释）。
 */
export const CTOR_ZERO_FILLED = Object.freeze(
  Array.from({ length: 20 }, (_, i) => 0x5ec9c + 4 * i),
);

/** 一份**规范化**的寄存器堆快照（纯数据 ⇒ 可 `JSON.stringify` 往返；键升序） */
export interface EngineScalarsSnapshot {
  /** `[名字, u32][]`，按名字升序 */
  values: [string, number][];
}

/**
 * 引擎标量寄存器堆。
 * ★ 名字**必须来自知识层**（`ENGINE_SCALAR_WRITES`）；本层不校验"这个名字认不认识"——
 *   那件事由守卫核（"模型引用的每个名字都要在 layout 表里"），放这里会让两处各判一次。
 */
export class EngineScalars {
  /** 名字 → u32（只记**被写过**的） */
  readonly values: Map<string, ScalarValue>;

  constructor(init?: Iterable<[string, ScalarValue]>) {
    this.values = new Map(init ?? []);
  }

  /** 读（没写过 ⇒ 0，见文件头：引擎把这片清零了） */
  read(name: string): ScalarValue {
    return this.values.get(name) ?? 0;
  }

  /** 写（u32 归一 —— 与池层的 int 一样，位模式只有一种表示） */
  write(name: string, value: number): void {
    this.values.set(name, value >>> 0);
  }

  /** 按位清（`&= ~mask` 那种形态；取证：`0x88` 会清 `Engine+0xAAB44` 的 `0x8000000` 位） */
  clearBits(name: string, mask: number): void {
    this.write(name, (this.read(name) & ~(mask >>> 0)) >>> 0);
  }

  /** 按位置（`|= mask`） */
  setBits(name: string, mask: number): void {
    this.write(name, (this.read(name) | (mask >>> 0)) >>> 0);
  }

  /** 写过一个没有（诊断用：能回答"这条路径依赖了几个标量槽"） */
  get size(): number {
    return this.values.size;
  }

  /** 规范化快照（键升序 ⇒ 同样状态给出同样字节） */
  snapshot(): EngineScalarsSnapshot {
    return { values: [...this.values.entries()].sort((a, b) => (a[0] < b[0] ? -1 : a[0] > b[0] ? 1 : 0)) };
  }

  /** 从快照**构造**新实例（不是往现有实例里灌 —— 与池/场景同口径） */
  static restore(snap: EngineScalarsSnapshot): EngineScalars {
    if (!Array.isArray(snap?.values)) throw new Error(`标量堆快照的 values 不是数组：${JSON.stringify(snap?.values)}`);
    const m = new Map<string, ScalarValue>();
    for (const [k, v] of snap.values) {
      if (typeof k !== 'string' || k === '') throw new Error(`标量堆快照里有非法键：${JSON.stringify(k)}`);
      if (!Number.isInteger(v) || v < 0 || v > 0xffffffff) throw new Error(`标量 ${k} 的值不是 u32：${v}`);
      if (m.has(k)) throw new Error(`标量堆快照里键重复：${k}（后写覆盖先写 = 静默丢一个槽）`);
      m.set(k, v);
    }
    return new EngineScalars(m);
  }
}

/**
 * ★ 状态分区（口径同 `model/pools.ts`）：**`engine` 类的字段恰好是快照的顶层键**。
 * `values` 是引擎态（它就是那些槽的内容）。
 */
export const STATE_PARTITION: Record<string, Record<string, string>> = {
  EngineScalars: { values: 'engine' },
};
