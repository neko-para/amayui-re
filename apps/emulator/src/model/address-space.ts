/**
 * apps/emulator/src/model/address-space.ts —— **地址空间**（★ 语义层核心：零 Node、零镜像偏移）
 *
 * ## 为什么需要它（决策：ADR「模拟器要有统一地址空间」）
 * 用户口径：**要像真正的引擎一样驱动游戏** ⇒ 凡是游戏**确实用到**的能力都必须支持。而"按语义槽建模"
 * **表达不出指针** —— 取证（锚 = EA）：
 *
 * ```
 * global-ptr 读：mov edx,[ecx+5D818h] / mov eax,[edx+eax*4] / mov edx,[eax]
 *                └─ 池基址 ─────────┘ └─ 取第 idx 格 = 一个指针 ─┘ └─ 解引用 ─┘
 * create-mesh ：mov eax,[ecx]        ; ★ 第 i 个顶点 = 操作数解出的**地址** + 4*i
 * ```
 *
 * ⇒ "池里那一格装的是**地址**、值在 `[地址]` 里"要求模型里存在**地址**这个概念
 *   （此前 `type 6/7/8/c/d/e` 一律响亮失败，就是因为没有它）。
 *
 * ## ★★ 形状：**扁平 + 合成 + 稀疏**（另两种形状 —— 结构化指针 / 继续不支持 —— 被否掉的理由见台账）
 * * ✅ **扁平**：`地址 = base + index*elemBytes` ⇒ 引擎的 `base + 4*i` / `lea` / 指针加减原样成立。
 *   口径与理由见知识台账：`data/ledger/`（域 `Emulator`，subject `model/address-space-flat`）。
 * * ✅ **合成**：地址按分配顺序发号（`ORIGIN` 起 bump）⇒ 同脚本 + 同环境 ⇒ 同一批地址、快照可比。
 *   口径与理由见知识台账：`data/ledger/`（域 `Emulator`，subject `model/address-space-synthetic`）。
 * * ✅ **稀疏**：格子按需出现（`cells: Map<偏移, 字节>`）—— 池容量无取证也不必先编一个；"没东西"≠"值是 0"。
 *   口径与理由见知识台账：`data/ledger/`（域 `Emulator`，subject `model/address-space-sparse-null-not-zero`）。
 *
 * ## ★ 本层**只管字节**：不认 DEC/ENC、不认 SSO、不认容器
 * 值的"含义"（int 族要过编解码、字符串是 28 字节小对象…）属于**池层 / 容器层**。
 * 把编解码塞进本层会让"取地址"也顺手解码 —— 而引擎的取址原语明确**不解码**
 * （取证：`sub_42AEA0` 的 case 3 逐字是 `mov ecx,[eax]` / `lea eax,[edx+ecx*4]`，没有任何位运算）。
 *
 * ## ★ 越界 / 未映射 / 未对齐一律**响亮失败**（不许补 0）
 * 引擎越界时**没有边界检查**（那是 UB，不是可模仿的行为）⇒ `unmapped` / `misaligned` / `unwritten`
 * 三种都变成**带地址的**响亮失败，并都进 `diagnostics`（计数）⇒ "依赖了未写过的格子"看得见。
 * 口径同 `model/address-space-sparse-null-not-zero`（台账 `data/ledger/`）。
 *
 * ## ★ 本批的诚实缺口（登记在需求树，不在这里糊过去）
 * * **区域的容量从哪来**：`alloc` 的 `capacity` 是**必填**的（本层不编默认值）。
 *   当前没有任何调用方传它，因为头部 6 个计数 → 6 个池的映射还没逐字取证
 *   （装载器 `.text:0040F222..0040F288` 把计数写进 帧+0x1C..+0x30，而 `LOCAL_POOL_SLOTS`
 *   的池序是 int/float/string/ptr/floatPtr/stringPtr —— 两者是否同序**未核**）。
 * * **`*_alt` 池（memflip）**：`Engine+0x5D804/0x5D80C/…` 那 6 个"另一半"是成对交换的缓冲
 *   （取证：`.text:0041AAD9..0041AB66` 六对 `[5D800]↔[5D804]` 交换，开关是配置键
 *   `set:EnableMemFlip`）⇒ 它们是**同一批数据的两个区域**，本层能表达（两个区域 + 一次交换），
 *   但"何时交换"未取证 ⇒ 不实现。
 * * **容器句柄（`0x8003/5/9/B`）**：它们要"懒分配 + 登记"（`Engine+0x5D860/0x5D870`、
 *   帧+0x5D904/0x5D908）。本层提供了它们的**存储**（区域 + 地址），但登记表与懒分配策略在容器层。
 */

/** 区域里一个格子的原始字节 */
export type CellBytes = Uint8Array;

/** `alloc` 的输入（★ `capacity` 必填：本层不编默认值 —— 编了就把"不知道"伪装成"知道"） */
export interface RegionSpec {
  /** 归属（人类可读；用于诊断与快照）：`local-int@3` / `global-float` / `container-0x8003[7]` … */
  tag: string;
  /** 一个格子几字节（int/float/ptr = 4；local_string 的 SSO = 28） */
  elemBytes: number;
  /** 能寻址几个格子（**必填**，见文件头上的缺口） */
  capacity: number;
  /**
   * ★ 本区域独占的**地址窗口**大小（字节）。缺省 `REGION_STRIDE`。
   *
   * ⛔ **不许所有池都用一个值**：窗口按**池的用途**给（见 `pools.ts` 的 `SPAN_*`），
   *    总面积由 `alloc` 的 u32 预算检查兜底。
   *    口径与理由见知识台账：`data/ledger/`（域 `Emulator`，subject `model/address-space-region-window`）。
   */
  span?: number;
}

/** 一次"引擎不会检查"的访问记录（诊断；不进快照） */
export interface MemoryDiagnostic {
  kind: 'unmapped' | 'misaligned' | 'unwritten';
  /** 出问题的地址（**合成地址**，只在本趟运行内有意义） */
  address: number;
  /** 次数 */
  count: number;
  /** 最近一次的归属说明（哪条指令问的） */
  note: string;
}

/**
 * 一段可寻址区域 = 一个**稀疏的**格子数组。
 * ★ `tag` / `base` / `elemBytes` / `capacity` 是**不可变的身份**（分配时定，之后不动）；
 *   只有 `cells` 会变 ⇒ 它是唯一需要进快照的字段（由分区守卫核）。
 */
export class Region {
  readonly tag: string;
  /** 合成基址（`ORIGIN` 起按分配顺序 bump）—— 只在本趟运行内唯一 */
  readonly base: number;
  readonly elemBytes: number;
  /**
   * 容量（格数）。★ 不再是 `readonly`：本层按**决策 `REQ-01M4B969TBWVERFCB1MXS2Q2E1`**
   * 采取"**初值 0 + 按需增长、每次增长留痕**" ——
   * 依据是两条已登记事实：① 引擎对池的下标访问**没有上界检查**（越界是 UB，不是语义）；
   * ② 全局池的容量字段在整份 `.lst` 里**无 store** ⇒ 按"必须拿到来源"会永久卡住。
   * ★ 增长**只增不减**（`growTo` 拒绝缩小）；初值 `0` = **不编任何常数**。
   */
  capacity: number;
  /**
   * ★ 本区域独占的**地址窗口**（字节）—— **不可变身份**（基址按整个窗口推进 ⇒ 区域绝不重叠）。
   *   口径与理由见知识台账：`data/ledger/`（域 `Emulator`，subject `model/address-space-region-window`）。
   */
  readonly span: number;
  /** 偏移 → 该格的原始字节（长度恒等于 `elemBytes`）。★ 稀疏：没写过的格子**不在这里** */
  readonly cells: Map<number, CellBytes>;

  constructor(tag: string, base: number, elemBytes: number, capacity: number, cells?: Map<number, CellBytes>, span = REGION_STRIDE) {
    if (!Number.isInteger(elemBytes) || elemBytes <= 0) throw new Error(`区域的 elemBytes 必须是正整数：${elemBytes}`);
    if (!Number.isInteger(capacity) || capacity < 0) throw new Error(`区域的 capacity 必须是非负整数：${capacity}`);
    if (!Number.isInteger(span) || span <= 0) throw new Error(`区域的 span 必须是正整数：${span}`);
    this.span = span;
    this.tag = tag;
    this.base = base;
    this.elemBytes = elemBytes;
    this.capacity = capacity;
    this.cells = cells ?? new Map();
  }

  /**
   * 把容量涨到至少 `capacity` 格（**只增不减**）。
   * @returns 真的涨了吗（没涨 ⇒ `false`，调用方不必留痕）
   * ★ ⛔ 不许实现成"悄悄放过去"：每次真涨都要由调用方**留痕**（见 `AddressSpace` 的增长钩子）。
   */
  growTo(capacity: number): boolean {
    if (!Number.isInteger(capacity) || capacity < 0) throw new Error(`growTo 的容量必须是非负整数：${capacity}`);
    if (capacity <= this.capacity) return false; // ★ 缩 = 拒绝（静默丢弃已写过的格子是不可逆的坏）
    this.capacity = capacity;
    return true;
  }

  /** 区域字节长（末地址 = `byteLength - 1`） */
  get byteLength(): number {
    return this.elemBytes * this.capacity;
  }

  /** 末地址之后的第一个地址（半开区间的右端） */
  get end(): number {
    return this.base + this.byteLength;
  }

  /** 第 `index` 格的地址（★ **引擎的 `base + elemBytes*index` 就是这个**） */
  addressOf(index: number): number {
    return this.base + this.elemBytes * index;
  }

  /** 地址 → 格内偏移；不属于本区域 / 未对齐 ⇒ `null` */
  offsetOf(address: number): number | null {
    if (!Number.isInteger(address) || address < this.base || address >= this.end) return null;
    const off = address - this.base;
    if (off % this.elemBytes !== 0) return null;
    return off;
  }

  /** 这一格被写过吗（★ "没东西" vs "值是 0" 的判据） */
  has(offset: number): boolean {
    return this.cells.has(offset);
  }

  /** 写一整格（**拷贝**：调用方改自己那份不该影响这里） */
  put(offset: number, bytes: CellBytes): void {
    if (bytes.length !== this.elemBytes) {
      throw new Error(`区域 ${this.tag} 的格子宽 ${this.elemBytes}，收到 ${bytes.length} 字节`);
    }
    this.cells.set(offset, bytes.slice(0));
  }

  /** 快照：只记**在场**的格子（偏移升序 ⇒ 与写入历史无关） */
  snapshot(): { cells: [number, number[]][] } {
    const asc = [...this.cells.entries()].sort((a, b) => a[0] - b[0]);
    return { cells: asc.map(([off, b]) => [off, [...b]]) };
  }
}

/** 一份**规范化**的地址空间快照（纯数据 ⇒ 可 `JSON.stringify` 往返） */
export interface AddressSpaceSnapshot {
  /** 下一个可用基址（恢复后要接着往下发，**不许**重发已经发过的地址） */
  baseCursor: number;
  /** 区域按 `base` 升序；每个带自己的格子 */
  regions: { tag: string; base: number; elemBytes: number; capacity: number; cells: [number, number[]][] }[];
}

/** 合成地址的起点。★ 它**不是**任何真实地址，只是"本层发的地址从这里开始" */
export const ORIGIN = 0x10000000;

/**
 * **一个区域独占的地址窗口大小的缺省值**（`0x0040_0000` = **4 MiB**）。
 *
 * ## 这个数由两条约束夹出来（不是拍的）
 * 1. **地址必须装进 4 字节格**（指针池的一格就是 4 字节）⇒ `ORIGIN + k*STRIDE ≤ 0xFFFFFFFF`。
 *    `load-frame` 的逐字把帧深上限定在 **40**，而每帧建 6 个局部 + 6 个全局池区域 ⇒ 区域数上限
 *    ≈ `40*6 + 6 = 246` ⇒ `ORIGIN + 246*4MiB ≈ ORIGIN + 984 MiB`，**远小于 4 GiB**（很宽松）；
 * 2. 窗口内容量上限 = `STRIDE / elemBytes` ⇒ 4 字节元素 = **1,048,576 格**。这一条**故意不够用**：
 *    ★ 全局数值族（`int` / `intRef` / `float` / `floatRef`）由 `pools.ts` 的
 *    `SPAN_GLOBAL_NUMERIC = 48 MiB` **覆盖**这个缺省值（本仓实测写到第 **7,355,801** 格 ≈29 MB）；
 *    真不够时 `ensureCapacity` **响亮失败**，由人决定拆区域还是换口径。
 *
 * ★ 它**不影响**任何语义：基址只通过 `addressOf` 被比较，别处不依赖具体值。窗口语义（基址按整个窗口
 *   推进、区域不许重叠）见知识台账：`data/ledger/`（域 `Emulator`，subject `model/address-space-region-window`）。
 * ★ 订正（2026-10）：本块原写"取 2 的幂 **16 MiB**（`0x0100_0000`）"，与代码的 `0x00400000`（4 MiB）
 *   分叉；现已与代码对齐，u32 预算口径见台账 subject `model/address-space-u32-budget-stride`。
 */
export const REGION_STRIDE = 0x00400000;

/**
 * **一个地址空间**。持有若干区域，回答"这个地址落在哪一格的哪个偏移"。
 *
 * ★ 与 `pools.ts` 的关系：池的 int / float 族**已经**把存储搬进区域（一位模式只有一份，
 *   迁移后 `Map` 为空）。口径与理由见知识台账：`data/ledger/`
 *   （域 `Emulator`，subject `model/pools-data-in-region-one-copy`）。
 */
export class AddressSpace {
  /** 区域（按 `base` 升序 —— 分配顺序即升序，因为基址是 bump 出来的） */
  readonly regions: Region[];
  /** 下一个可用基址 */
  baseCursor: number;
  /** 诊断（不进快照：它不是引擎态，而且会随访问增长） */
  readonly diagnostics: Map<string, MemoryDiagnostic>;
  /**
   * **增长钩子**（可选）：每次真的增长时回调一次。
   * ★ 存在的理由：本层的策略是"按需增长"，而"增长"**必须留痕** ——
   *   否则它会退化成"悄悄把越界放过去"（那正是本仓最忌讳的静默）。钩子由 `Machine` 接到副作用日志。
   * ⛔ 本层**不**自己发日志（`model/` 不依赖 `host/effects`）。
   */
  onGrow: ((e: { tag: string; index: number; from: number; to: number; note: string }) => void) | null;

  constructor(init?: { regions?: Region[]; baseCursor?: number; onGrow?: AddressSpace['onGrow'] }) {
    this.regions = init?.regions ?? [];
    this.baseCursor = init?.baseCursor ?? ORIGIN;
    this.diagnostics = new Map();
    this.onGrow = init?.onGrow ?? null;
  }

  /**
   * 确保某区域**至少**容得下第 `index` 格（**只增不减**），并把这次增长交给钩子留痕。
   *
   * ★ 为什么不让 `readCell`/`writeCell` 自动增长：那样"越界"就会变成"悄悄扩容" ——
   *   而引擎那边越界是 **UB**（无上界检查，见台账），本层要**响亮失败**。
   *   ⇒ 增长必须是**显式**的一步（由池层在它知道自己在干什么时调用）。
   * @returns 真的涨了吗
   */
  ensureCapacity(region: Region, index: number, note = ''): boolean {
    if (!Number.isInteger(index) || index < 0) throw new Error(`ensureCapacity 的下标必须是非负整数：${index}`);
    const need = index + 1;
    if (need <= region.capacity) return false;
    // ★★ **不许越出本区域自己的地址窗口**（`[base, base + region.span)`）：越过它就必然**重叠下一个
    //   区域** ⇒ "写进 A 的值从 B 的地址读到"（静默串数据）⇒ 宁可响亮失败，由人决定拆区域还是加大 span。
    if (need * region.elemBytes > region.span) {
      throw new Error(
        `区域 ${region.tag} 需要 ${need * region.elemBytes} 字节 > 它自己的地址窗口（${region.span} B）` +
        ` ⇒ 增长会与下一个区域重叠（**静默串数据**）⇒ 拒绝。${note ? `（${note}）` : ''}`,
      );
    }
    const from = region.capacity;
    if (!region.growTo(need)) return false;
    this.onGrow?.({ tag: region.tag, index, from, to: region.capacity, note });
    return true;
  }

  /** 地址版：确保该地址落在一个**容得下它**的区域里（地址不属于任何区域 ⇒ 抛，不许"顺便建一个"） */
  ensureAddress(address: number, note = ''): void {
    // ★★ 按**窗口**找区域，不是按"当前 byteLength"：地址落在窗口内而在容量外 = "还没长到"
    //   ⇒ **按需增长**（决策 REQ-01M4B969TBWVERFCB1MXS2Q2E1）；落在所有窗口外 = 未映射 ⇒ 抛（响亮）。
    const hit = this.regionByWindow(address);
    if (!hit) throw new Error(`ensureAddress：0x${address.toString(16)} 不落在任何区域窗口里${note ? `（${note}）` : ''}`);
    const index = Math.floor((address - hit.base) / hit.elemBytes);
    this.ensureCapacity(hit, index, note);
  }

  /**
   * 地址 → **窗口内**的区域（不看 `capacity`）。`null` = 落在所有窗口之外（未映射）。
   * ★ 与 `regionAt` 的分工：`regionAt` 答"这一格**现在在**吗"（受 `byteLength` 限制，用于真实访问）；
   *   `regionByWindow` 答"这个地址**归谁管**"（用于按需增长）。两者混用会让"窗口内的地址被当成未映射"
   *   ⇒ 那条路只能抛、永远涨不起来。口径与理由见知识台账：`data/ledger/`
   *   （域 `Emulator`，subject `model/address-space-region-window`）。
   */
  regionByWindow(address: number): Region | null {
    for (const r of this.regions) {
      if (Number.isInteger(address) && address >= r.base && address < r.base + r.span) return r;
    }
    return null;
  }

  /**
   * 分配一个区域。返回的区域**已经**带基址（发号是这一步的全部副作用）。
   * ★ 基址按 `elemBytes` 对齐（`4`/`28` 都不许跨格子），并按 8 向上取整推进 ——
   *   于是"发到哪了"只取决于**分配顺序与容量**，与宿主无关。
   */
  alloc(spec: RegionSpec): Region {
    const align = Math.max(8, spec.elemBytes);
    const span = spec.span ?? REGION_STRIDE;
    const base = Math.ceil(this.baseCursor / align) * align;
    const r = new Region(spec.tag, base, spec.elemBytes, spec.capacity, undefined, span);
    this.regions.push(r);
    this.regions.sort((a, b) => a.base - b.base);
    // ★★ 推进**一个完整的地址窗口**，不是按当前字节长：`capacity === 0` 的区域 `byteLength === 0`
    //   ⇒ 下一个区域会拿到同一个基址；而**增长**会越过邻居的基址 ⇒ 静默串数据。窗口语义见知识台账：
    //   `data/ledger/`（域 `Emulator`，subject `model/address-space-region-window`）。
    // ★★ 而且**整个空间必须装进 4 字节格**（指针池的一格就是 4 字节）⇒ 越界就抛，不许静默截断
    //   （实测：截断后 `actual` 与 `expected` 正好差 `2^32`）。口径见知识台账：`data/ledger/`
    //   （域 `Emulator`，subject `model/address-space-u32-budget-stride`）。
    if (base + span > 0x1_0000_0000) {
      throw new Error(
        `地址空间用尽：再发一个区域（tag=${spec.tag}，span=${span}）会让基址越过 u32（0x${this.baseCursor.toString(16)}）—— ` +
        '指针池的一格只有 4 字节 ⇒ 地址必须装进 u32。见 address-space.ts 里 span/REGION_STRIDE 的推导。',
      );
    }
    this.baseCursor = base + span;
    return r;
  }

  /** 按 tag 找区域（诊断/守卫用；**同名多个** ⇒ 抛，免得悄悄取第一个） */
  regionByTag(tag: string): Region {
    const hits = this.regions.filter((r) => r.tag === tag);
    if (hits.length === 0) throw new Error(`没有这个 tag 的区域：${tag}`);
    if (hits.length > 1) throw new Error(`tag 不唯一：${tag}（${hits.length} 个）—— 取第一个会掩盖命名错误`);
    return hits[0];
  }

  /** 地址 → `{区域, 偏移}`；不落在任何区域 ⇒ `null`。★★ **这是元素粒度的**（偏移必须是 `elemBytes` 的整数倍）*/
  resolve(address: number): { region: Region; offset: number } | null {
    for (const r of this.regions) {
      const off = r.offsetOf(address);
      if (off !== null) return { region: r, offset: off };
    }
    return null;
  }

  /**
   * 地址 → `{区域, 字节偏移}`，**字节粒度**（不做对齐检查）。
   * ★ 分工：`readBytes` 要按字节走（28 字节字符串里第 3 个字节也是合法地址），而 `resolve` 是
   *   元素粒度的（"取第 i 格"）；两者混用会让"按字节读一段"永远失败。
   */
  regionAt(address: number): { region: Region; byteOffset: number } | null {
    for (const r of this.regions) {
      if (Number.isInteger(address) && address >= r.base && address < r.end) {
        return { region: r, byteOffset: address - r.base };
      }
    }
    return null;
  }

  /** 读一个 dword（**未映射 / 未对齐 ⇒ 抛**；未写过 ⇒ `null`，由调用方决定） */
  readU32(address: number, note = ''): number | null {
    const b = this.readCell(address, note);
    if (b === null) return null;
    return new DataView(b.buffer, b.byteOffset, 4).getUint32(0, true);
  }

  /** 写一个 dword（**未映射 / 未对齐 ⇒ 抛**；未写过 ⇒ 先建那一格） */
  writeU32(address: number, value: number, note = ''): void {
    const hit = this.#mustResolve(address, note);
    if (hit.region.elemBytes !== 4) {
      throw new Error(`区域 ${hit.region.tag} 的格子宽 ${hit.region.elemBytes}，不能用 writeU32（地址 0x${address.toString(16)}）`);
    }
    const buf = new Uint8Array(4);
    new DataView(buf.buffer).setUint32(0, value >>> 0, true);
    hit.region.put(hit.offset, buf);
  }

  /** 读一整格（未写过 ⇒ `null`） */
  readCell(address: number, note = ''): CellBytes | null {
    const hit = this.#mustResolve(address, note);
    const b = hit.region.cells.get(hit.offset);
    if (!b) {
      this.#diag('unwritten', address, note);
      return null;
    }
    return b.slice(0);
  }

  /** 写一整格 */
  writeCell(address: number, bytes: CellBytes, note = ''): void {
    const hit = this.#mustResolve(address, note);
    hit.region.put(hit.offset, bytes);
  }

  /** 读一段连续字节（可跨格；**跨区域 ⇒ 抛** —— 那说明这段数据不在同一个块里） */
  readBytes(address: number, length: number, note = ''): Uint8Array {
    if (!Number.isInteger(length) || length < 0) throw new Error(`长度必须是非负整数：${length}`);
    const start = this.regionAt(address);
    if (!start) {
      this.#diag('unmapped', address, `readBytes(${length}) ${note}`.trim());
      throw new Error(`readBytes 的起点 0x${address.toString(16)} 不落在任何区域里${note ? `（${note}）` : ''}`);
    }
    const out = new Uint8Array(length);
    for (let i = 0; i < length; i += 1) {
      const at = this.regionAt(address + i);
      if (!at || at.region !== start.region) {
        this.#diag('unmapped', address + i, `readBytes(${length}) @0x${address.toString(16)} ${note}`.trim());
        throw new Error(
          `readBytes 越出区域：地址 0x${(address + i).toString(16)}（起点区域 ${start.region.tag}）—— ` +
          `引擎会安静地读到相邻内存，那是 UB，本层不模仿`,
        );
      }
      // ★ 字节粒度 → 找到它所属的**格子**，再取格内第几个字节
      const cellOffset = Math.floor(at.byteOffset / at.region.elemBytes) * at.region.elemBytes;
      const cell = at.region.cells.get(cellOffset);
      out[i] = cell ? cell[at.byteOffset - cellOffset] : 0; // 字节粒度上"没写过"就是 0
    }
    return out;
  }

  /** 诊断摘要（键升序 ⇒ 同样访问序列给出同样摘要） */
  diagnosticsSorted(): MemoryDiagnostic[] {
    return [...this.diagnostics.values()].sort((a, b) =>
      (a.kind + a.address.toString(16) < b.kind + b.address.toString(16) ? -1 : 1));
  }

  /** 规范化快照（★ `engine` 类字段恰好是它的顶层键：`regions` / `baseCursor`） */
  snapshot(): AddressSpaceSnapshot {
    return {
      baseCursor: this.baseCursor,
      regions: [...this.regions]
        .sort((a, b) => a.base - b.base)
        .map((r) => ({ tag: r.tag, base: r.base, elemBytes: r.elemBytes, capacity: r.capacity, cells: r.snapshot().cells })),
    };
  }

  /** 从快照**构造**新实例（不是往现有实例里灌 —— 与池模型同口径） */
  static restore(snap: AddressSpaceSnapshot): AddressSpace {
    if (!Number.isInteger(snap?.baseCursor)) throw new Error(`快照缺 baseCursor：${JSON.stringify(snap?.baseCursor)}`);
    if (!Array.isArray(snap.regions)) throw new Error('快照的 regions 不是数组');
    const regions = snap.regions.map((r) => {
      if (!Array.isArray(r.cells)) throw new Error(`区域 ${r.tag} 的 cells 不是数组`);
      const cells = new Map<number, CellBytes>();
      for (const [off, arr] of r.cells) {
        if (!Number.isInteger(off) || off < 0) throw new Error(`区域 ${r.tag} 的偏移非法：${off}`);
        if (cells.has(off)) throw new Error(`区域 ${r.tag} 的偏移重复：${off}（后写覆盖先写 = 静默丢一格）`);
        cells.set(off, Uint8Array.from(arr));
      }
      return new Region(r.tag, r.base, r.elemBytes, r.capacity, cells);
    });
    // ★ 恢复后基址不许重发：`baseCursor` 直接取自快照，且必须**不小于**所有区域的末端
    const end = regions.reduce((m, r) => Math.max(m, r.end), ORIGIN);
    if (snap.baseCursor < end) {
      throw new Error(`快照的 baseCursor ${snap.baseCursor} 小于已有区域的末端 ${end} ⇒ 会重发已用过的地址`);
    }
    return new AddressSpace({ regions, baseCursor: snap.baseCursor });
  }

  /** 解析失败 ⇒ 抛（带地址 + 两种失败的区别）；成功 ⇒ 交给调用方 */
  #mustResolve(address: number, note: string): { region: Region; offset: number } {
    if (!Number.isInteger(address) || address < 0) throw new Error(`地址必须是非负整数：${address}`);
    for (const r of this.regions) {
      if (address >= r.base && address < r.end) {
        const off = r.offsetOf(address);
        if (off === null) {
          this.#diag('misaligned', address, note);
          throw new Error(
            `地址 0x${address.toString(16)} 未对齐：区域 ${r.tag} 的格子宽 ${r.elemBytes}，偏移 ${address - r.base} ` +
            `${note ? `（${note}）` : ''}—— 引擎会读到跨格子的半字，那是 UB，本层不模仿`,
          );
        }
        return { region: r, offset: off };
      }
    }
    this.#diag('unmapped', address, note);
    throw new Error(
      `地址 0x${address.toString(16)} 不落在任何区域里（已知 ${this.regions.length} 个区域）` +
      `${note ? `（${note}）` : ''} —— 引擎会安静地读到别处，那是 UB，本层不模仿`,
    );
  }

  #diag(kind: MemoryDiagnostic['kind'], address: number, note: string): void {
    const key = `${kind}@${address}`;
    const prev = this.diagnostics.get(key);
    if (prev) { prev.count += 1; if (note) prev.note = note; return; }
    this.diagnostics.set(key, { kind, address, count: 1, note });
  }
}

/**
 * ★ 状态分区（口径与 `model/pools.ts` 的同一张表一致）：**`engine` 类的字段恰好是快照的顶层键**。
 * * `AddressSpace`：`regions` 与 `baseCursor` 是引擎态（内存内容 + 发号进度）；`diagnostics` 是诊断。
 * * `Region`：只有 `cells` 会变 ⇒ 唯一进快照的字段；其余是分配时定死的身份。
 *   口径与理由见知识台账：`data/ledger/`（域 `Emulator`，subject `model/state-partition-engine-is-snapshot-keys`）。
 */
export const STATE_PARTITION: Record<string, Record<string, string>> = {
  AddressSpace: { regions: 'engine', baseCursor: 'engine', diagnostics: 'diagnostic', onGrow: 'host' },
  // ★ `span` 与 `base`/`elemBytes`/`capacity` 同类：**不可变身份**（分配时定）⇒ derived（不进快照）
  Region: { tag: 'derived', base: 'derived', elemBytes: 'derived', capacity: 'derived', span: 'derived', cells: 'engine' },
};
