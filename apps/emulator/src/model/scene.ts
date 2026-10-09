/**
 * apps/emulator/src/model/scene.ts —— **场景模型**（★ 语义层，零镜像布局、零 Node）
 *
 * ## ★★ 为什么 headless 也需要"场景模型"（这条区分是本模块存在的全部理由）
 * "headless = 所有渲染都是副作用日志"很容易被读成"**不建任何渲染状态**" —— 那是错的：
 * `wait`（`0x21C`）的等待门问的是"引擎还有没有没跑完的动画"，而那条判据的输入就是**计时窗**；
 * 把场景整个扔掉 ⇒ 等待门永远立即放行 ⇒ 版权页那 5 秒的窗**被静默删掉**（那是把观测面做没了）。
 * ⇒ 场景模型（绘制项 / 纹理槽 / 网格 / 计时窗）**有**、是**引擎态**、进快照；**呈现**（像素与声音）
 * **没有**，由 `present` 能力的**缺席**表达 —— **没有像素 ≠ 没有状态**。
 * 口径与理由见知识台账：`data/ledger/`（域 `Emulator`，subject `model/scene-headless-keeps-engine-state`）。
 *
 * ## ★ 它**不含**任何镜像布局（本仓硬口径，由守卫钉住）
 * ❌ 没有 `+0x38` / `+0x60` 这类帧内偏移 ❌ 没有 EA ❌ 没有 IDA 符号。这里的每个字段都是**语义名**
 * （`delayMs` / `workingArgb` / `window.startMs`），偏移与它们的对应关系属于知识层
 * （`packages/age-format/src/engine/`），带 EA 出处、由守卫回语料复核。
 *
 * ## ★ 未初始化用 `null`，不用 0（"0 是一个合法值"）
 * 空纹理槽是 `null` 而不是 `imgid 0`；计时窗起点是 `startMs: number | null`（`null` = 还没锁存、
 * 下一帧才起算）—— 虚拟时钟的**第一帧就是 0**，拿 0 当哨兵会让第一帧的窗被判成"已经跑了 0 毫秒"。
 * 口径同上（subject `model/scene-headless-keeps-engine-state`）。
 *
 * ## 颜色一律是 **ARGB 的 u32**（`>>> 0`）
 * 引擎按 32 位色存；这里也存 u32（不做通道拆分）—— 拆开就有了第二份真源，
 * 而通道拆分的规则（clamp / 负值回退）属于**指令**，不属于存储。
 */

/** 一块纹理槽的绑定状态 */
export interface TextureSlot {
  /** 绑定的资源 id（`set-texture` 的 op1）；`null` = **空槽**（不是"id 为 0"） */
  imgid: number | null;
  /** 是**程序化离屏表面**（`create-texture` 建的）还是来自文件的图（`set-texture` 建的） */
  offscreen: boolean;
  width: number;
  height: number;
  /** `set-texture` 的 op3 归一化后的颜色；`null` = 该绑定没给颜色 */
  colorArgb: number | null;
}

/** 一个**计时窗**（颜色动画）：引擎逐帧在 from → to 之间按 (delay, dur) 插值 */
export interface ColorWindow {
  delayMs: number;
  durMs: number;
  fromArgb: number;
  toArgb: number;
  /** 窗起点（虚拟时刻，毫秒）。★ `null` = **还没锁存**（引擎下一帧才起算）—— 不许用 0 当哨兵 */
  startMs: number | null;
}

/** 一个绘制项（`draw-texture` 建/更新的东西；`handle` 同时是**层序键**：越小越先画） */
export interface DrawItem {
  handle: number;
  /** 引用的纹理槽号（`draw-texture` 的 op2） */
  slot: number;
  /** 源矩形 */
  src: { x: number; y: number; w: number; h: number };
  /** 目标位置 */
  dst: { x: number; y: number };
  /** 当前工作色（diffuse，ARGB）—— `set-draw-color-alpha` 会直接改它 */
  workingArgb: number;
  /**
   * `set-draw-color-alpha` 的 `op2` **原样存下来**。
   * ★ 取证：它落在绘制项的某一个 dword 字段上，而那个字段的**读取点没拿到** ⇒
   *   **语义未定**。⛔ 不许叫它 "blend mode"（那是旧仓文档的命名，没有判据）。
   */
  param: number;
  /** 颜色窗（`set-draw-color` 建的）；`null` = 没有窗 */
  window: ColorWindow | null;
}

/** 一个顶点网格（`create-mesh` 建的） */
export interface Mesh {
  handle: number;
  vertexCount: number;
  mode: number;
  /** 逐顶点色（ARGB）。长度 = `vertexCount`（由 `create-mesh` 的解码结果灌进来） */
  vertexColors: number[];
  /**
   * `set-vertex-color` 的 `op2` **原样存下来**。
   * ★ 取证：它落在网格项的一个 dword 字段上，**读取点没拿到** ⇒ 语义未定。
   *   ⛔ 不许当成"顶点下标"：取证明确说那一位不是逐顶点的写（逐顶点色是 `create-mesh` 灌的数组）。
   */
  param: number;
  /** FROM 色（`set-vertex-color` 写的那个"当前色"） */
  fromArgb: number;
  /** 颜色窗（`set-vertex-color-alpha` 建的）；`null` = 没有窗 */
  window: ColorWindow | null;
}

/** 一份**规范化**的场景快照（纯数据 ⇒ 可 `JSON.stringify` 往返） */
export interface SceneSnapshot {
  /** 槽号升序 */
  slots: [number, TextureSlot][];
  /** 绘制项按 handle 升序 */
  items: [number, DrawItem][];
  /** 网格按 handle 升序 */
  meshes: [number, Mesh][];
}

/** 一个计时窗的单点求值（**纯函数**）：给定虚拟时刻，问"跑完了吗 / 进行到哪" */
export interface WindowProgress {
  /** 还没锁存 ⇒ pending 为真（它**欠着一件事没做**，不是"已经做完"） */
  pending: boolean;
  /** 已经跑了多久（相对窗起点；未锁存 ⇒ `null`） */
  elapsedMs: number | null;
}

/**
 * 场景模型。
 *
 * ★ 三个容器都是 `Map`：**`handle` / 槽号是稀疏的**（手柄值都是脚本算出来的大数，
 * 例如版权页的 `0x30d40`），用数组会立刻撞上"编一个容量出来"的错。
 */
export class SceneModel {
  /** 槽号 → 绑定状态（**没有** = 空槽；不写 `null` 占位，免得"空"有两种表示） */
  readonly slots: Map<number, TextureSlot>;
  /** handle → 绘制项 */
  readonly items: Map<number, DrawItem>;
  /** handle → 网格 */
  readonly meshes: Map<number, Mesh>;

  constructor(init?: { slots?: Map<number, TextureSlot>; items?: Map<number, DrawItem>; meshes?: Map<number, Mesh> }) {
    this.slots = init?.slots ?? new Map();
    this.items = init?.items ?? new Map();
    this.meshes = init?.meshes ?? new Map();
  }

  // ── 纹理槽 ────────────────────────────────────────────────────────────────

  /** `set-texture`：把资源 id 绑到槽（覆盖旧绑定） */
  bindTexture(slot: number, imgid: number, colorArgb: number | null): TextureSlot {
    const s: TextureSlot = { imgid, offscreen: false, width: 0, height: 0, colorArgb };
    this.slots.set(slot, s);
    return s;
  }

  /** `create-texture`：建一块**离屏**空白表面（不是从文件读的图） */
  createTexture(slot: number, width: number, height: number): TextureSlot {
    const s: TextureSlot = { imgid: null, offscreen: true, width, height, colorArgb: null };
    this.slots.set(slot, s);
    return s;
  }

  /** `release-texture`：释放该槽（**删掉**，不是写一个空值进去） */
  releaseTexture(slot: number): boolean {
    return this.slots.delete(slot);
  }

  // ── 绘制项 ────────────────────────────────────────────────────────────────

  /**
   * `draw-texture`：建/更新一个绘制项。
   * ★ 更新时**保留**已有的工作色与颜色窗：脚本常常"先设色、后作画"（版权页正是如此 ——
   *   先把颜色动画排在 handle 上，再 `draw-texture` 把图元建出来）。整体替换会把窗丢掉，
   *   而那**不会报错**，只会让那个 5 秒的窗消失、等待门提前放行。
   */
  drawTexture(
    handle: number,
    slot: number,
    src: { x: number; y: number; w: number; h: number },
    dst: { x: number; y: number },
  ): DrawItem {
    const prev = this.items.get(handle);
    const item: DrawItem = {
      handle,
      slot,
      src: { ...src },
      dst: { ...dst },
      workingArgb: prev?.workingArgb ?? 0xffffffff,
      param: prev?.param ?? 0,
      window: prev?.window ?? null,
    };
    this.items.set(handle, item);
    return item;
  }

  /**
   * `detach-texture`：`count <= 1` ⇒ 摘掉单个 `handle`；`count > 1` ⇒ 摘掉
   * **`[handle, handle + count)`** 区间（★ 区间是**左闭右开**，且 `count` 不是"数量减一"）。
   * @returns 真的被摘掉的 handle（升序）
   */
  detach(handle: number, count: number): number[] {
    const removed: number[] = [];
    if (count > 1) {
      for (const h of [...this.items.keys()].sort((a, b) => a - b)) {
        if (h >= handle && h < handle + count) { this.items.delete(h); removed.push(h); }
      }
      for (const h of [...this.meshes.keys()].sort((a, b) => a - b)) {
        if (h >= handle && h < handle + count) { this.meshes.delete(h); removed.push(h); }
      }
      return removed;
    }
    if (this.items.delete(handle)) removed.push(handle);
    if (this.meshes.delete(handle)) removed.push(handle);
    return removed;
  }

  // ── 网格 ──────────────────────────────────────────────────────────────────

  createMesh(handle: number, vertexCount: number, mode: number): Mesh {
    const prev = this.meshes.get(handle);
    const m: Mesh = {
      handle,
      vertexCount,
      mode,
      vertexColors: new Array(Math.max(0, vertexCount)).fill(0xffffffff),
      param: prev?.param ?? 0,
      fromArgb: prev?.fromArgb ?? 0xffffffff,
      window: prev?.window ?? null,
    };
    this.meshes.set(handle, m);
    return m;
  }

  // ── 颜色：工作色与计时窗 ───────────────────────────────────────────────────

  /**
   * `set-draw-color-alpha`：改**工作色**（FROM）与那个**语义未定**的 `param`。
   * ★ **不建窗、不动已有的窗**（取证：它既不置"有窗"标志、也不碰 delay/dur）——
   *   所以它可以随时用来做 hover 高亮/回退，而正在跑的窗会从新的 FROM 继续插值。
   */
  setDrawColorAlpha(handle: number, param: number, argb: number): boolean {
    const it = this.items.get(handle);
    if (!it) return false;
    it.param = param >>> 0;
    it.workingArgb = argb >>> 0;
    return true;
  }

  /** `set-draw-color`：在绘制项上排一个**计时窗**（op2 = delay、op3 = dur、目标色 = op5|op4） */
  setDrawColorWindow(handle: number, delayMs: number, durMs: number, argb: number): boolean {
    const it = this.items.get(handle);
    if (!it) return false;
    it.window = { delayMs, durMs, fromArgb: it.workingArgb >>> 0, toArgb: argb >>> 0, startMs: null };
    return true;
  }

  /**
   * `set-vertex-color`：改网格的 **FROM 色**与那个**语义未定**的 `param`。
   * ★ 取证：它**不建窗**（不置"有窗"标志、不碰 delay/dur），并且**不是逐顶点写**
   *   （逐顶点色是 `create-mesh` 灌进来的数组）。⇒ 上一版把它当成"按下标写某一顶点的色"是错的。
   */
  setMeshColor(handle: number, param: number, argb: number): boolean {
    const m = this.meshes.get(handle);
    if (!m) return false;
    m.param = param >>> 0;
    m.fromArgb = argb >>> 0;
    return true;
  }

  /** `set-vertex-color-alpha`：在网格上排一个**计时窗**（op2 = delay、op3 = dur） */
  setVertexColorWindow(handle: number, delayMs: number, durMs: number, argb: number): boolean {
    const m = this.meshes.get(handle);
    if (!m) return false;
    m.window = { delayMs, durMs, fromArgb: m.fromArgb >>> 0, toArgb: argb >>> 0, startMs: null };
    return true;
  }

  // ── 帧推进：锁存 → 求值 → 完成 ─────────────────────────────────────────────

  /** 全部计时窗（绘制项 + 网格），按 handle 升序 */
  windows(): { kind: 'item' | 'mesh'; handle: number; window: ColorWindow }[] {
    const out: { kind: 'item' | 'mesh'; handle: number; window: ColorWindow }[] = [];
    for (const h of [...this.items.keys()].sort((a, b) => a - b)) {
      const w = this.items.get(h)!.window;
      if (w) out.push({ kind: 'item', handle: h, window: w });
    }
    for (const h of [...this.meshes.keys()].sort((a, b) => a - b)) {
      const w = this.meshes.get(h)!.window;
      if (w) out.push({ kind: 'mesh', handle: h, window: w });
    }
    return out;
  }

  /**
   * **锁存**：给还没起算的窗设起点（引擎在排窗之后的**下一帧**才起算）。
   * @returns 本次被锁存的窗（供日志）
   */
  latchWindows(atMs: number): { kind: 'item' | 'mesh'; handle: number; window: ColorWindow }[] {
    const latched = [];
    for (const w of this.windows()) {
      if (w.window.startMs === null) { w.window.startMs = atMs; latched.push(w); }
    }
    return latched;
  }

  /** 一个窗在给定时刻的进度（**纯函数**，不改状态） */
  static progressOf(w: ColorWindow, atMs: number): WindowProgress {
    // ★ 未锁存 ⇒ pending（它欠着一件事没做）。"还没开始"不是"已经做完"。
    if (w.startMs === null) return { pending: true, elapsedMs: null };
    const elapsed = atMs - w.startMs;
    return { pending: elapsed < w.delayMs + w.durMs, elapsedMs: elapsed };
  }

  /**
   * **帧推进**：把已经跑完的窗收尾（工作色 ← 目标色、窗清空）。
   * @returns 本次收尾的窗（供 `render.anim.done` 日志）
   */
  completeWindows(atMs: number): { kind: 'item' | 'mesh'; handle: number; window: ColorWindow }[] {
    const done: { kind: 'item' | 'mesh'; handle: number; window: ColorWindow }[] = [];
    for (const w of this.windows()) {
      const p = SceneModel.progressOf(w.window, atMs);
      if (p.pending) continue;
      if (w.kind === 'item') {
        const it = this.items.get(w.handle);
        if (it) { it.workingArgb = w.window.toArgb >>> 0; it.window = null; }
      } else {
        const m = this.meshes.get(w.handle);
        if (m) { m.vertexColors = m.vertexColors.map(() => w.window.toArgb >>> 0); m.window = null; }
      }
      done.push(w);
    }
    return done;
  }

  /** **"引擎还有没有没跑完的动画"** —— 等待门（`0x400`）的判决输入 */
  hasPendingWindows(atMs: number): boolean {
    for (const w of this.windows()) if (SceneModel.progressOf(w.window, atMs).pending) return true;
    return false;
  }

  // ── 快照 ──────────────────────────────────────────────────────────────────

  /** 规范化快照：槽号与 handle 一律**升序**（`Map` 的迭代顺序是插入顺序 ⇒ 不排就与写入历史有关） */
  snapshot(): SceneSnapshot {
    const asc = <V>(m: Map<number, V>): [number, V][] => [...m.entries()].sort((a, b) => a[0] - b[0]);
    return { slots: asc(this.slots), items: asc(this.items), meshes: asc(this.meshes) };
  }

  /** 从快照**构造**新实例（不是往现有实例里灌 —— 与池模型同口径） */
  static restore(snap: SceneSnapshot): SceneModel {
    return new SceneModel({
      slots: new Map(snap.slots ?? []),
      items: new Map(snap.items ?? []),
      meshes: new Map(snap.meshes ?? []),
    });
  }
}
