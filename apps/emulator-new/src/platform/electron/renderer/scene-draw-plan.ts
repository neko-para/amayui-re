/**
 * `scene-draw-plan.ts` —— **绘制项消费者的纯规划器**（`CORE-OPS-TEXTURE-DRAW-CONSUMER`）。
 *
 * 一个绘制项在一帧里**发不发绘制请求、发的话实参是什么**，全部在这里决定：
 * 输入 = 条目快照 + 纹理槽解析结果 + 渲染目标 + 场景对象（缩放 / 偏移 / 显示模式）+ 场景时钟；
 * 输出 = `skip`（原因）+ `report`（错误通道上报，带纹理编号）+ 绘制实参（裁剪后矩形 / 三维位置 / 本步颜色 / 本步变换）。
 *
 * ★ **纯函数、无 Pixi、无 DOM**：所以它能用 `@env pure` 的守卫直接跑（`tools/test/emulator-new-scene-draw-plan.test.mjs`）。
 *   ⛔ 不要 import `pixi.js` / `document` / `node:*`；⛔ 也不要在这里画东西 —— 建节点是 `pixi-host.ts` 的事。
 *
 * ════════════════ 这个文件按 spec 的哪几条写（章节 `60-ops-texture.md`）════════════════
 *
 * 1. **有效位门**：条目首字第 0 位为 0 ⇒ 不画、不报错（`no-valid`）。
 * 2. **本轮已消费门**：第 16 位为 1 ⇒ 本轮不再画它（`consumed`）；清位不在这里（那是收尾扫描的活）。
 * 3. **纹理解析**：槽里没有对象 ⇒ 上报「绘制源纹理尚未创建」（带编号）+ 跳过（`missing-texture`）。
 *    ★ 纹理编号**不做范围检查**：本函数只调 `resolveSlot`，越界读到什么由调用方决定
 *    （读到 0 / 没有对象 ⇒ 走空槽；读到别的格 ⇒ 当纹理对象用）。⛔ 这里没有「编号非法」这条分支。
 * 4. **渲染目标**：没有目标 ⇒ 上报「绘制目标尚未创建」+ 跳过（`no-target`）。
 *    ★ **自引用不跳过**：引擎不比较源与目标 ⇒ 本函数也**不看** `target.id === item.textureId`。
 * 5. **裁剪**：把四条边夹进纹理对象**自己的逻辑矩形** `(0, 0, width, height)` —— 左 / 上取大、右 / 下取小；
 *    夹空（左 ≥ 右 或 上 ≥ 下）⇒ **静默**跳过（`empty-clip`，不上报、不建占位物）。
 *    ★ 进裁剪之前先过 **4.1 源矩形窗口**（延迟 / 持续时间 / 步进总数 / 每行步数 / 开关字）：三档
 *      （到期 / 延迟内 / 推进中），只有**到期**那一档把矩形写回记录（`rectWriteBack`）。
 * 5.1 **位置四步（顺序固定）**：裁剪补偿 → 缩放 + 半上取整（仅纹理编号 `[20, 30)`）→ 场景偏移 → 相加。
 *     末段是**两张适配矩阵的乘入门（互斥，每笔最多一张）**：见 {@link planDraw} 的注释与
 *     {@link RESERVED_VIEW_TARGET} —— ★ 目标编号 `38` / 纹理目标**不**乘视图矩阵（旧实现写反了）。
 * 6. **附加组派生**（位 2 = 1，在发请求**之前**）：颜色三角波混合 / 变换乘入并强制有效 / 源矩形按步进偏移并跨步标脏。
 *    **主绘制仍然只发一次** —— 本函数只返回**一个** `draw`。
 * 6.1 **场景自身变换**：条目键落在作用键区间且计数非 0、且调用方没传「跳过」⇒ 把场景自身记录的
 *    额外变换乘进本步变换并置「有效 = 1」。★ 命令档执行未建模 ⇒ 这一维目前只有接口参数。
 * 7. **请求**：裁剪后的源矩形 + 三维位置实参 + 本步变换 + 本步颜色。
 *    ★ 绘制入口返回失败的上报**不在这里**（本层拿不到返回码）⇒ `pixi-host.ts` 用 {@link drawFailedReport} 补上。
 *
 * ⛔ 本文件**不**决定顺序（那是 `scene-order.ts` 的 `CORE-SCENE-DRAW-ORDER`）、**不**建纹理、**不**改纹理槽。
 */
import type {
  DrawItemSnapshot,
  SceneViewSnapshot,
} from '../protocol.ts';

/* ─────────────────────────────── 常量（全是 spec 正文里的位 / 阈值） ─────────────────────────────── */

/** 条目首字的**第 0 位** = 「有效」（`0x1fb` / `0x23b` 置；`0x202` 的门读同一位） */
export const FLAG_VALID = 0x1;
/** 条目首字的**第 1 位** = 「有窗」（颜色窗 / 变换窗 / 源矩形窗**共用**同一位） */
export const FLAG_WINDOW = 0x2;
/** 条目首字的**第 2 位** = 附加组开关（`0x231`..`0x235` / `0x239` 置） */
export const FLAG_GROUP = 0x4;
/** 条目首字的**第 16 位** = 「本轮已消费」（命令档画过 / 占位；收尾扫描访问后清 0） */
export const FLAG_CONSUMED = 0x10000;

/** 「无颜色」（核心 `NO_COLOR` 的同一个位模式）；spec：本步颜色**默认是不透明白** */
export const NO_COLOR = -1;
/** 本步颜色的默认值 = **不透明白**（spec 第 7 条；透明度是颜色的一部分） */
export const DEFAULT_STEP_COLOR = 0xffffffff;

/** 纹理编号档 `[20, 30)` = **文本层画布带**（文本层号 0..9 自建 20+i 的离屏画布）：只有这一档做「位置 × 场景缩放 + 半上取整」与偏移的特殊档；画布按适配比预缩放，所以消费端用逆缩放矩阵抵回 */
export const SCREEN_BAND_START = 20;
export const SCREEN_BAND_END = 30;
/** 保留目标编号 38（领域名未定）：与后缓冲同档（视口原点 / 偏移按"后缓冲或 38"那一档） */
export const RESERVED_BACK_LIKE_TARGET = 38;
/**
 * 保留目标编号 39（领域名未定）：**与后缓冲同档乘场景视图矩阵**。
 * ★ 门是 `F == 0 && (D < 0 || D == 39) && tex ∉ [20, 30)`；
 *   ⛔ 旧实现把门写成"`D >= 0 && D != 39` ⇒ 乘视图矩阵"，那是**反的**（目标 38 与一切纹理目标不乘）。
 */
export const RESERVED_VIEW_TARGET = 39;

/** 半上取整的常量（`.data:0051D7F8 = 0.5`，spec：`floor(值 × 缩放 + 0.5)`，⛔ 不是四舍五入） */
const ROUND_HALF_UP_BIAS = 0.5;

/* ─────────────────────────────── 输入 ─────────────────────────────── */

/** 纹理槽解析结果：**只**给"槽里有没有对象 + 对象的逻辑宽高"（没有对象 ⇒ `resolveSlot` 返回 `null`） */
export interface DrawPlanSlot {
  /** 纹理对象**自己的逻辑宽**（创建时请求的宽；⛔ 不是表面实际宽度） */
  width: number;
  /** 纹理对象**自己的逻辑高** */
  height: number;
}

/** 当前渲染目标（与 `SceneSnapshot.target` 同口径） */
export interface DrawPlanTarget {
  /** 目标编号：`-1` = 后缓冲；`38` / `39` = 保留档（领域名未定） */
  id: number;
  /**
   * 场景对象里"当前渲染目标"的对象引用**是否非空**。
   * ★ 与编号是两件事：`-1` 是后缓冲（有目标）；编号 ≥ 0 但对象已被释放 ⇒ 也算"没有目标"（见残差）。
   */
  present: boolean;
}

/**
 * 场景自身变换（`CORE-OPS-TEXTURE-DRAW-CONSUMER` 6.1）的输入。
 * ★ 由 `0x229` 写的**作用键区间** + 由场景自身记录派生出的**额外变换矩阵**组成；
 *   派生（`sub_49AA30`）本实现**未建模** ⇒ `matrix` 给 `null`（规划器用单位阵并记缺口）。
 */
export interface DrawPlanSelfTransform {
  /** 作用键区间起点（+0x458） */
  keyStart: number;
  /** 作用键区间计数（+0x45C）；**0 ⇒ 不派生**额外变换 */
  keyCount: number;
  /** 额外变换（列主序 4×4）；`null` = 派生未建模 */
  matrix: number[] | null;
}

/** 规划器的输入 */
export interface DrawPlanInput {
  /** 条目快照（矩形 / 纹理编号 / 首字 / 两个浮点位 / 本步颜色 / 本步变换 / 附加组视图） */
  item: DrawItemSnapshot;
  /**
   * 纹理槽解析：纹理编号 → 槽（`null` = 槽里没有对象 / 该编号没有纹理对象）。
   * ★ **不做范围检查**（spec 第 3 条）：越界编号读到什么由调用方决定 —— 也就是说
   *   编号 1000 不会被这里判成"非法"，它只会走"调用方给了什么槽"这一条路。
   */
  resolveSlot: (textureId: number) => DrawPlanSlot | null;
  target: DrawPlanTarget;
  /**
   * 显示适配的档位输入（由 `display-fit.ts` 的 `computeDisplayFit` 填，见 `session.ts`）；
   * `null` = **没有显示适配上下文**（还没建窗口 / 图形 / 会话没拿到配置）⇒ 按"无相机 / 无缩放"档
   * （缩放按 1、偏移按 0），并记一条 `scene-view-unmodeled` 缺口。
   */
  view: SceneViewSnapshot | null;
  /** 场景时钟（毫秒）：附加组用它**自己的**周期（起点戳 + 周期），⛔ 不与主窗合并 */
  now: number;
  /**
   * 场景的**「强制完成」档**（`CORE-OPS-TEXTURE-DRAW-CONSUMER` 4.1：为 1 ⇒ 一律算**到期**）。
   * ★ 本实现还没有这一格的状态（`0xB5B0` 未建模）⇒ 调用方不传 = 0；
   *   ⛔ 不要把它与"持续时间 ≤ 0"混成同一档。
   */
  forcedComplete?: number;
  /**
   * 场景自身变换（6.1）；`null` / `undefined` = 本帧没有它（或计数为 0）⇒ 不乘。
   */
  selfTransform?: DrawPlanSelfTransform | null;
  /**
   * 命令档多传的**「跳过额外变换」**标志（6.1）。
   * ★ 本实现**命令档执行未建模** ⇒ 现在没有调用方会传 `true`（收尾扫描总不跳过）；
   *   这一维只作为接口存在（⛔ 不要假装命令档已经在跑）。
   */
  skipExtraTransform?: boolean;
}

/* ─────────────────────────────── 输出 ─────────────────────────────── */

/** 跳过原因（`null` = 要画） */
export type DrawSkipReason =
  /** 首字第 0 位为 0：静默不画（⛔ 不报错） */
  | 'no-valid'
  /** 首字第 16 位为 1：本轮已消费，静默不画 */
  | 'consumed'
  /** 纹理槽里没有对象：上报（带编号）后跳过 */
  | 'missing-texture'
  /** 没有渲染目标：上报后跳过 */
  | 'no-target'
  /** 源矩形夹到纹理逻辑矩形后为空：**静默**跳过（不上报、不建占位物） */
  | 'empty-clip';

/** 错误通道上报的文本类别（内容不在 spec 里；但必须**仍然上报**且**带纹理编号**） */
export type DrawReportCategory =
  /** 「绘制源纹理尚未创建」 */
  | 'draw-source-missing'
  /** 「绘制目标尚未创建」 */
  | 'draw-target-missing'
  /** 「绘制失败」（绘制入口返回失败；由平台侧补） */
  | 'draw-failed';

/** 一次错误通道上报 */
export interface DrawPlanReport {
  category: DrawReportCategory;
  /** ★ 必须带：否则日志里定位不到坏条目 */
  textureId: number;
  /** 说明文本（内容本版不规定） */
  text: string;
}

/** 裁剪后的源矩形（纹理逻辑坐标系） */
export interface DrawPlanRect {
  left: number;
  top: number;
  right: number;
  bottom: number;
}

/** 本步变换（列主序 4×4） */
export interface DrawPlanTransform {
  matrix: number[];
  /** `false` ⇒ 绘制入口**不应用**这份矩阵（记录里的「本步矩阵有效」格） */
  valid: boolean;
}

/** 一次绘制请求的实参 */
export interface DrawPlanArgs {
  textureId: number;
  /** **裁剪后**的源矩形 */
  rect: DrawPlanRect;
  /** 三维位置实参（第三格恒 0，不参与任何加工） */
  position: [number, number, number];
  /** 本步颜色（32 位 ARGB，含透明度；默认不透明白） */
  color: number;
  /** 本步变换 */
  transform: DrawPlanTransform;
}

/**
 * 实现缺口（**不是**引擎错误 ⇒ 不进错误通道，进诊断面板）。
 * ★ 出现这些说明"这一档我们还没建模"，⛔ 不许当成 spec 结论。
 */
export type DrawPlanNote =
  /** 没有场景相机模型 ⇒ 按"无相机 / 无缩放"档算（缩放 1、偏移 0） */
  | 'scene-view-unmodeled'
  /** 首字第 2 位置了，但没有附加组视图（缺字段）⇒ 不派生 */
  | 'group-unmodeled'
  /** 附加组的颜色起点戳是 0（未盖章）：引擎会盖章，本层没有回写路径 ⇒ 按 elapsed 0 算 */
  | 'group-color-unstamped'
  /** 附加组终色没有取值（没写方 / 未取证）⇒ 不混色 */
  | 'group-color-end-missing'
  /** 附加组的「变换通道」未取证是哪一张矩阵 ⇒ 不乘 */
  | 'group-transform-missing'
  /** 基础本步变换没有取值（本实现还没建模 `sub_49AA30` 的矩阵派生）⇒ 用单位阵且记缺口 */
  | 'transform-base-missing'
  /** 场景视图矩阵没有取值 ⇒ 不乘 */
  | 'transform-view-missing'
  /** 场景缩放是 0（无从取倒数 / 无从做除法）⇒ 这一档按 0 偏移、不乘逆缩放 */
  | 'scene-scale-zero'
  /** 首字第 1 位置了，但快照里没有源矩形窗口字段（缺投影）⇒ 4.1 不跑 */
  | 'rect-window-unmodeled'
  /** 窗起点是 0（未盖章）：引擎会盖章，本层没有回写路径 ⇒ 本帧按"盖章在 now"算 */
  | 'rect-window-unstamped'
  /** 每行步数为 0 且持续时间 > 0：整数除零后果 spec 不规定 ⇒ 记缺口并跳过需要除法的档 */
  | 'rect-window-divide-by-zero'
  /** 场景自身变换落在作用键区间内，但没有派生矩阵（`sub_49AA30` 未建模）⇒ 用单位阵 */
  | 'self-transform-matrix-unmodeled';

/** 规划结果 */
export interface DrawPlan {
  /** `null` = 要画；否则是静默 / 上报后跳过的原因 */
  skip: DrawSkipReason | null;
  /** 错误通道上报（只有 `missing-texture` / `no-target` 会带；`empty-clip` **不**上报） */
  report: DrawPlanReport | null;
  /** 绘制实参（`skip === null` 时非 `null`；跳过时**一律**是 `null` ⇒ 调用方没有东西可建） */
  draw: DrawPlanArgs | null;
  /** 附加组「跨步进 ⇒ 场景标脏」：本层没有回写路径，调用方负责记账 */
  markDirty: boolean;
  /**
   * **到期那一帧**要写回条目四条边的源矩形（4.1）；非到期帧**必须**是 `null`。
   * ★ 只有到期档回写；推进中 / 延迟内只改本帧实参。
   * ★ 本层没有回写路径（规划器是纯函数）⇒ 调用方负责落表**并**按 spec 把延迟 / 持续时间清 0
   *   （当前平台侧还没有这条通道，只记账）。
   * ★ 到期但开关字第 0 位 = 0 ⇒ 这里是**空矩形**（右 ← 左、下 ← 上），spec 说"把结果写回"。
   */
  rectWriteBack: DrawPlanRect | null;
  /**
   * **本笔乘入了几张显示适配矩阵**（`CORE-SCENE-DISPLAY-FIT` 的乘入门；互斥 ⇒ 只可能是 0 或 1）。
   * ★ 这张矩阵的值可能恰好是单位阵（固定分辨率模式下的逆缩放矩阵、T=0 的视图矩阵），
   *   只看 `transform.matrix` 分不出"乘了一次单位阵"与"没乘" ⇒ 这一格是那条合同的**唯一可观测量**
   *   （守卫 `emulator-new-scene-draw-plan.test.mjs` 的矩阵门用例靠它断言"恰好一张 / 一张都不乘"）。
   */
  adaptMatrixCount: number;
  /** 实现缺口（去重、按首次出现顺序） */
  notes: DrawPlanNote[];
}

/* ─────────────────────────────── 主函数 ─────────────────────────────── */

/**
 * 一个绘制项 → 一次（至多一次）绘制请求 / 一次跳过。
 *
 * ★ 判定次序是 spec 的读序：有效位 → 本轮已消费 → 纹理解析 → 渲染目标 → （附加组派生）→ 裁剪 → 位置 / 变换。
 *   把"裁剪判空"放在"空槽"之后是有意的：空槽**必须上报**，不能因为矩形退化就静默吞掉。
 */
export function planDraw(input: DrawPlanInput): DrawPlan {
  const { item, resolveSlot, target, view, now } = input;
  const notes: DrawPlanNote[] = [];
  const note = (code: DrawPlanNote): void => {
    if (!notes.includes(code)) notes.push(code);
  };

  let markDirty = false;
  /** 4.1 的到期写回（只有到期档会填；其它档必须保持 `null`） */
  let rectWriteBack: DrawPlanRect | null = null;
  /** 显示适配矩阵乘入次数（门是互斥的 ⇒ 0 或 1；见 {@link DrawPlan.adaptMatrixCount}） */
  let adaptMatrixCount = 0;
  /** 统一的返回口：跳过时 `draw` 一律为 `null`（调用方据此决定"什么都不建"） */
  const finish = (skip: DrawSkipReason | null, report: DrawPlanReport | null, draw: DrawPlanArgs | null): DrawPlan => ({
    skip,
    report,
    draw: skip === null ? draw : null,
    markDirty,
    rectWriteBack,
    adaptMatrixCount,
    notes,
  });

  // 1. 有效位门：为 0 ⇒ 不画、不报错
  if ((item.flags & FLAG_VALID) === 0) return finish('no-valid', null, null);
  // 2. 本轮已消费门
  if ((item.flags & FLAG_CONSUMED) !== 0) return finish('consumed', null, null);

  // 3. 纹理解析：空槽 ⇒ 上报 + 跳过。★ 不做范围检查（越界由 resolveSlot 自己决定读到什么）。
  const textureId = item.textureId;
  const slot = resolveSlot(textureId);
  if (slot === null) {
    return finish('missing-texture', {
      category: 'draw-source-missing',
      textureId,
      text: `绘制源纹理尚未创建（纹理 ${textureId}）`,
    }, null);
  }

  // 4. 渲染目标：没有目标 ⇒ 上报 + 跳过。★ 自引用**不**在这里判（引擎不比较源与目标）。
  if (!target.present) {
    return finish('no-target', {
      category: 'draw-target-missing',
      textureId,
      text: `绘制目标尚未创建（纹理 ${textureId}）`,
    }, null);
  }

  /* ── 4.1 源矩形窗口（在**裁剪与附加组之前**；只有到期帧写回记录） ── */
  let rect: DrawPlanRect = { left: item.left, top: item.top, right: item.right, bottom: item.bottom };
  if ((item.flags & FLAG_WINDOW) !== 0) {
    const window = item.sourceWindow ?? null;
    if (window === null) {
      // 位 1 与颜色窗共用；没有投影就说"这一档没建模"，⛔ 不按 0 窗口假装跑过
      note('rect-window-unmodeled');
    } else if ((window.duration | 0) > 0) {
      // "有窗 且 本步走到这一段" ⇒ **逐帧**标脏（4.1；与附加组的跨步标脏是两回事）
      markDirty = true;
      const delay = window.delay | 0;
      const duration = window.duration | 0;
      // 窗起点与颜色窗共用 `item.start`；0 = 未盖章 ⇒ 本步盖章（起点 ← now）。
      // ★ 盖章要落表 ⇒ 本层没有回写路径，只按"已盖章"算并记缺口（⛔ 不改快照）。
      const startCell = item.start ?? 0;
      const start = startCell === 0 ? now : startCell;
      if (startCell === 0) note('rect-window-unstamped');
      // 三档：按 **32 位无符号**比较（spec 原文）
      const startU = start >>> 0;
      const delayU = delay >>> 0;
      const deadline = (startU + delayU + (duration >>> 0)) >>> 0;
      const expired = input.forcedComplete === 1 || (now >>> 0) > deadline; // 相等算窗内（70-vm）
      const delayed = !expired && (now >>> 0) <= ((startU + delayU) >>> 0);
      const width = rect.right - rect.left;
      const height = rect.bottom - rect.top;
      if (expired) {
        // ★ 到期先把延迟与持续时间清 0 —— 那是**记录**上的写（调用方落表时做）；本层只算。
        if ((window.switchWord & 0x1) !== 0) {
          // 末帧：步序号 = 步进总数 − 1
          const delta = rectStepDelta((window.steps | 0) - 1, window.perRow | 0, width, height);
          if (delta === null) {
            // 整数除零后果 spec 不规定 ⇒ 这一档跳过：不改矩形、也不产出写回值
            note('rect-window-divide-by-zero');
          } else {
            rect = shiftRect(rect, delta);
            rectWriteBack = { ...rect };
          }
        } else {
          // 开关字第 0 位 = 0 ⇒ 矩形塌成空；spec："把结果写回条目的四条边"（空矩形也是结果）
          rect = { left: rect.left, top: rect.top, right: rect.left, bottom: rect.top };
          rectWriteBack = { ...rect };
        }
      } else if (delayed) {
        // 延迟内：开关字第 1 位 = 1 ⇒ 原样；= 0 ⇒ 塌成空。**不**写回
        if ((window.switchWord & 0x2) === 0) {
          rect = { left: rect.left, top: rect.top, right: rect.left, bottom: rect.top };
        }
      } else {
        // 推进中：步序号 = ⌊(now − 起点 − 延迟) × 步进总数 ÷ 持续时间⌋（无符号除法）；**不**写回
        const elapsed = ((now >>> 0) - startU - delayU) >>> 0;
        const step = rectStepIndex(elapsed, window.steps | 0, duration);
        const delta = rectStepDelta(step, window.perRow | 0, width, height);
        if (delta === null) note('rect-window-divide-by-zero');
        else rect = shiftRect(rect, delta);
      }
    }
  }

  /* ── 6. 附加组派生（在裁剪 / 位置 / 发请求**之前**改主绘制的输入） ── */
  let color = item.color < 0 ? DEFAULT_STEP_COLOR : item.color >>> 0;
  let matrix: number[] | null = item.transform ? [...item.transform.matrix] : null;
  let valid = item.transform?.valid ?? false;

  if ((item.flags & FLAG_GROUP) !== 0) {
    const group = item.group ?? null;
    if (group === null) {
      note('group-unmodeled');
    } else {
      // (a) 颜色：按附加组**自己的**周期（起点戳 + 周期）作三角波，朝附加组终色**逐字节**混合
      if (group.colorPeriod > 0) {
        if (group.colorEnd === null) {
          note('group-color-end-missing');
        } else {
          if (group.colorStart === 0) note('group-color-unstamped');
          const elapsed = group.colorStart === 0 ? 0 : now - group.colorStart;
          const weight = triangleWave(elapsed, group.colorPeriod);
          color = mixArgb(color, group.colorEnd, weight);
        }
      }
      // (b) 源矩形的四条边按步进一起偏移（左右同加、上下同加 ⇒ 同一张图里的另一个子矩形）
      if (group.stepX !== 0 || group.stepY !== 0) {
        rect = {
          left: rect.left + group.stepX,
          top: rect.top + group.stepY,
          right: rect.right + group.stepX,
          bottom: rect.bottom + group.stepY,
        };
      }
      // 跨步进 ⇒ 场景标脏（附加组自己的判据；4.1 的"有窗逐帧标脏"是另一条，两条都置同一个布尔）
      if (group.rectPeriod > 0 && group.lastStepIndex !== null) {
        const stepIndex = Math.floor((now - group.rectStart) / group.rectPeriod);
        if (stepIndex !== group.lastStepIndex) markDirty = true;
      }
      // (c) 变换：附加组通道乘进本步变换，并把「本步变换有效」**强制为 1**
      if (group.transform === null) note('group-transform-missing');
      else matrix = composeTransform(matrix ?? identity4(), group.transform);
      valid = true;
    }
  }

  /* ── 6.1 场景自身变换（6 之后、主绘制之前：乘进本步变换并把「有效」置 1） ── */
  // ★ 判据是**条目键**落在 `[起点, 起点 + 计数)` 内、且计数非 0（计数 0 ⇒ 不派生）；
  //   命令档可以传「跳过」来抑制这一步（本实现命令档未建模 ⇒ 没有调用方会传 true）。
  // ★ 矩阵乘入的**先后顺序**（与 5.1 的视图矩阵 / 附加组通道之间）spec 只写"乘进本步变换"，
  //   未规定 ⇒ 这里沿用附加组同一写法（⛔ 不要为了让某组守卫通过而自创次第）。
  if (input.skipExtraTransform !== true && input.selfTransform != null) {
    const self = input.selfTransform;
    if ((self.keyCount | 0) !== 0 && inSelfTransformRange(item.key, self.keyStart, self.keyCount)) {
      if (self.matrix === null) note('self-transform-matrix-unmodeled');
      matrix = composeTransform(matrix ?? identity4(), self.matrix ?? identity4());
      valid = true;
    }
  }

  /* ── 5. 裁剪：夹进纹理对象**自己的**逻辑矩形（不是表面实际尺寸） ── */
  const left = Math.max(rect.left, 0);
  const top = Math.max(rect.top, 0);
  const right = Math.min(rect.right, slot.width);
  const bottom = Math.min(rect.bottom, slot.height);
  // 夹空 ⇒ 静默跳过：不画、不上报、不建占位物
  if (left >= right || top >= bottom) return finish('empty-clip', null, null);

  /* ── 5.1 位置四步（顺序固定） ──
   * ★ 缩放 / 原点先过**取值器**：固定分辨率模式（`display:ScreenMode != 0`）⇒ 缩放短路成 1、
   *   原点短路成 0（spec「固定分辨率模式」：它**不进适配公式**，只在这里短路；公式在 `display-fit.ts`）。 */
  const inScreenBand = textureId >= SCREEN_BAND_START && textureId < SCREEN_BAND_END;
  if (view === null) note('scene-view-unmodeled');
  const fixedResolution = view?.fixedResolution === true;
  /** 场景的**屏幕形态副本** `F`（窗口态 1 / 占满屏态 0）；缺省 = 0（占满屏） */
  const screenForm = view?.screenForm ?? 0;
  const scaleX = view === null || view.noCamera || fixedResolution ? 1 : view.scaleX;
  const scaleY = view === null || view.noCamera || fixedResolution ? 1 : view.scaleY;
  // 偏移：窗口态**恒 0**（spec「折回画布坐标」）；固定分辨率模式 / 无相机同样按 0
  const offsetX = screenForm !== 0 || view === null || view.noCamera || fixedResolution ? 0 : view.offsetX;
  const offsetY = screenForm !== 0 || view === null || view.noCamera || fixedResolution ? 0 : view.offsetY;
  const displayMode = view?.displayMode ?? 0;

  // 第 1 步：裁剪补偿 —— 左 / 上被裁多少，位置就加回多少；右 / 下不补；第三格不参与
  let x = item.aux[0] + (left - rect.left);
  let y = item.aux[1] + (top - rect.top);
  const z = item.aux[2];

  // 第 2 步：缩放 + 半上取整（只由纹理编号决定，与目标编号无关；无相机时缩放 = 1 但取整照做）
  if (inScreenBand) {
    x = Math.floor(x * scaleX + ROUND_HALF_UP_BIAS);
    y = Math.floor(y * scaleY + ROUND_HALF_UP_BIAS);
  }

  // 第 3 步：场景偏移（整数原样 / 整数 ÷ 场景缩放 / 0，按目标编号与纹理编号分档）
  const offset = sceneOffset(target.id, displayMode, offsetX, offsetY, scaleX, scaleY, inScreenBand, note);
  // 第 4 步：相加（第三格不加）
  x += offset.x;
  y += offset.y;

  /* ── 5.1 末：同一条档位还决定这一步的变换（**两张适配矩阵，门互斥**：每笔最多乘一张） ──
   *   - 场景视图矩阵 ⇐ `F == 0` 且（`D < 0` 或 `D == 39`）且 `tex ∉ [20, 30)`；
   *   - 逆缩放矩阵   ⇐ `F == 0` 且 `D >= 0` 且 `D != 39` 且 `tex ∈ [20, 30)`。
   *   ★ `F` = 场景的屏幕形态副本（窗口态 1 / 占满屏态 0）；`F != 0` ⇒ 两张都不乘。
   *   ★ 目标 38 与一切纹理目标**不**乘视图矩阵（⛔ 旧实现写反了）；`D == 39` 与后缓冲同档。
   *   ★ 固定分辨率模式下缩放取值 = 1 ⇒ 逆缩放矩阵退化成单位阵，但**仍然乘一次**（数值无影响）。 */
  if (screenForm === 0) {
    if (target.id < 0 || target.id === RESERVED_VIEW_TARGET) {
      if (!inScreenBand) {
        if (view?.viewMatrix) {
          matrix = composeTransform(matrix ?? identity4(), view.viewMatrix);
          adaptMatrixCount += 1;
        } else {
          note('transform-view-missing');
        }
      }
    } else if (inScreenBand) {
      if (scaleX === 0 || scaleY === 0) {
        note('scene-scale-zero');
      } else {
        matrix = composeTransform(matrix ?? identity4(), scaling4(1 / scaleX, 1 / scaleY, 1));
        adaptMatrixCount += 1;
      }
    }
  }
  if (matrix === null) {
    matrix = identity4();
    if (valid) note('transform-base-missing');
  }

  return finish(null, null, {
    textureId,
    rect: { left, top, right, bottom },
    position: [x, y, z],
    color,
    transform: { matrix, valid },
  });
}

/**
 * 「绘制失败」上报（第 7 条：绘制入口返回失败 ⇒ 上报并跳过）。
 * ★ 本层拿不到绘制入口的返回码 ⇒ 由平台侧在 `catch` / 失败分支里调用它。
 */
export function drawFailedReport(textureId: number, detail?: string): DrawPlanReport {
  return {
    category: 'draw-failed',
    textureId,
    text: `绘制失败（纹理 ${textureId}）${detail ? `：${detail}` : ''}`,
  };
}

/* ─────────────────────────────── 纯数学 / 档位助手 ─────────────────────────────── */

/** 源矩形整体平移（四条边**同加** ⇒ 宽 / 高不变；spec 4.1：图集按行扫） */
export function shiftRect(rect: DrawPlanRect, delta: { x: number; y: number }): DrawPlanRect {
  return {
    left: rect.left + delta.x,
    top: rect.top + delta.y,
    right: rect.right + delta.x,
    bottom: rect.bottom + delta.y,
  };
}

/**
 * 推进中的**步序号**：`⌊(现在 − 起点 − 延迟) × 步进总数 ÷ 持续时间⌋`（4.1）。
 * ★ spec 只规定这一除法的"无符号 / 整数截断"；乘法的位宽未规定 ⇒ 用普通整数乘（⛔ 不假装 32 位回绕）。
 */
export function rectStepIndex(elapsed: number, steps: number, duration: number): number {
  if (!(duration > 0)) return 0;
  return Math.floor(((elapsed >>> 0) * (steps | 0)) / (duration >>> 0));
}

/**
 * 一步的矩形偏移：`Δx = 宽 × (步序号 mod 每行步数)`、`Δy = 高 × (步序号 ÷ 每行步数)`（4.1）。
 *
 * - `每行步数 === 0` ⇒ 返回 `null`：整数除零的后果 spec **本版不规定**（不夹取、不跳过）⇒
 *   调用方**记缺口并跳过该档**，⛔ 不许把 0 悄悄改成 1（那会掩盖真实的引擎行为）。
 * - 行号用 `Math.trunc`（有符号除法 = 截断），步序号为负时也不出 NaN（`steps = 0` 的到期末帧 = −1）。
 */
export function rectStepDelta(
  step: number,
  perRow: number,
  width: number,
  height: number,
): { x: number; y: number } | null {
  if (perRow === 0) return null;
  const column = step % perRow;
  const row = Math.trunc(step / perRow);
  return { x: width * column, y: height * row };
}

/**
 * 条目键是否落在场景自身变换的**作用键区间** `[起点, 起点 + 计数)`（6.1）。
 * ★ 用**有符号**比较：证据是 `jl` / `jge`（`.tmp/spec-draft/batch5/commands/evidence.md` §2.2）
 *   ⇒ 计数为负时区间为空（末键 = 起点 + 计数 用 32 位加法）。
 */
export function inSelfTransformRange(key: number, keyStart: number, keyCount: number): boolean {
  const start = keyStart | 0;
  const end = (start + (keyCount | 0)) | 0;
  const value = key | 0;
  return value >= start && value < end;
}

/**
 * 三角波：`0 → period/2 → 0` 的权重（0..1）。
 * ★ spec 只说"起点戳 + 周期的三角波"；**相位起点**（0 起还是峰起）与舍入口径没有逐字节复核
 *   （`.tmp/spec-draft/batch4/texconsumer/evidence.md` §3.4-2）⇒ 这里取"0 起、峰在中点"这个最简读法。
 */
export function triangleWave(elapsed: number, period: number): number {
  if (!(period > 0)) return 0;
  const phase = ((elapsed % period) + period) % period;
  return 1 - Math.abs((2 * phase) / period - 1);
}

/** 逐字节整数混合（截断；spec：颜色混合是**逐字节整数**、透明度是颜色的一部分） */
export function mixArgb(from: number, to: number, weight: number): number {
  const w = weight <= 0 ? 0 : weight >= 1 ? 1 : weight;
  const a = mixByte((from >>> 24) & 0xff, (to >>> 24) & 0xff, w);
  const r = mixByte((from >>> 16) & 0xff, (to >>> 16) & 0xff, w);
  const g = mixByte((from >>> 8) & 0xff, (to >>> 8) & 0xff, w);
  const b = mixByte(from & 0xff, to & 0xff, w);
  return ((a << 24) | (r << 16) | (g << 8) | b) >>> 0;
}

function mixByte(from: number, to: number, weight: number): number {
  const value = from + Math.trunc((to - from) * weight);
  return value < 0 ? 0 : value > 255 ? 255 : value;
}

/**
 * 第 3 步的分档（**严格按 spec 的原文**，⛔ 不要改成"统一 ÷ 缩放"）：
 *
 * - 目标是后缓冲（-1）或保留编号 38：显示模式 = 1 **且**目标是后缓冲 ⇒ (0, 0)；
 *   否则纹理编号 `[20, 30)` ⇒ 整数原样；否则 ⇒ 整数 ÷ 场景缩放。
 * - 其它目标（编号 ≥ 0 且 ≠ 38）：`[20, 30)` ⇒ (0, 0)；否则 ⇒ 整数 ÷ 场景缩放。
 * - `F != 0`（窗口态）/ 固定分辨率模式 / 无相机：调用方传进来的偏移已经是 0（这里不用再判）。
 *
 * ★ 传的是**取值器算过的**缩放与偏移（固定分辨率模式已在 `planDraw` 里短路成 1 / 0）。
 * ★ 缩放为 0 时"整数 ÷ 缩放"没有定义 ⇒ 记缺口后按 0（⛔ 不编一个数）。
 */
function sceneOffset(
  targetId: number,
  displayMode: number,
  offsetX: number,
  offsetY: number,
  scaleX: number,
  scaleY: number,
  inScreenBand: boolean,
  note: (code: DrawPlanNote) => void,
): { x: number; y: number } {
  const divide = (value: number, scale: number): number => {
    if (scale === 0) {
      // 缩放 0 时"整数 ÷ 缩放"没有定义 ⇒ 不编一个数，记缺口后按 0（未取证）
      note('scene-scale-zero');
      return 0;
    }
    return value / scale;
  };
  const backLike = targetId < 0 || targetId === RESERVED_BACK_LIKE_TARGET;
  if (backLike) {
    if (displayMode === 1 && targetId < 0) return { x: 0, y: 0 };
    if (inScreenBand) return { x: offsetX, y: offsetY }; // 整数原样
    return { x: divide(offsetX, scaleX), y: divide(offsetY, scaleY) };
  }
  if (inScreenBand) return { x: 0, y: 0 };
  return { x: divide(offsetX, scaleX), y: divide(offsetY, scaleY) };
}

/* ─────────────────────────────── 列主序 4×4 ─────────────────────────────── */

/** 单位阵（列主序，与核心的矩阵口径一致） */
export function identity4(): number[] {
  return [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1];
}

/** 列主序相乘：`left × right`（列向量约定 ⇒ **右先作用**） */
export function multiply4(left: readonly number[], right: readonly number[]): number[] {
  const out = new Array<number>(16).fill(0);
  for (let column = 0; column < 4; column += 1) {
    for (let row = 0; row < 4; row += 1) {
      let sum = 0;
      for (let k = 0; k < 4; k += 1) {
        sum += (left[k * 4 + row] ?? 0) * (right[column * 4 + k] ?? 0);
      }
      out[column * 4 + row] = sum;
    }
  }
  return out;
}

/** 组合两个变换：**先 `first` 再 `second`**（列向量约定 ⇒ `second × first`） */
export function composeTransform(first: readonly number[], second: readonly number[]): number[] {
  return multiply4(second, first);
}

/** 缩放矩阵（列主序）；逆场景缩放矩阵 = `scaling4(1/sx, 1/sy, 1)` */
export function scaling4(sx: number, sy: number, sz: number): number[] {
  const matrix = identity4();
  matrix[0] = sx;
  matrix[5] = sy;
  matrix[10] = sz;
  return matrix;
}
