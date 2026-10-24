/**
 * `display-fit.ts` —— **显示适配的纯函数**（`CORE-SCENE-DISPLAY-FIT`）。
 *
 * 引擎在**图形初始化**与**显示设置改变**两个时刻把一组显示适配参数算好放在渲染器上；
 * 绘制条目时按它取「缩放 / 偏移 / 视图矩阵」。这些参数**不是场景状态、不是脚本状态、不是存档状态**
 * ⇒ 本文件只吃纯数据、只吐纯数据：⛔ 不 import `pixi.js` / `document` / `node:*`，⛔ 不持有渲染器。
 *
 * ════════════════ 按 spec 的哪几条写（`data/spec/chapters/60-ops-scene.md`）════════════════
 *
 * 1. **输入**：后缓冲 `(BW, BH)`、`vfOn`（`display:VirtualFullScreen != 0`）、`T`（`display:VirtualFullScreenType`）、
 *    `A`（`display:AspectMode`）、参考尺寸 `(refW, refH)`、**当前画布** `(canvasW, canvasH)`。
 *    ★ 画布是**输入也是输出**：VF 关那条路要用"上一轮的画布"算居中（spec 伪代码里的 `canvas` 就是它），
 *      VF 开时它被本次公式覆写。⛔ 不要做成"画布由调用方另算"（那样 `(CW − BW) / 2` 会拿到错的 CW）。
 * 2. **算术约定**：`trunc` = 向零截断（`Math.trunc`，⛔ 不是 floor / 四舍五入）；
 *    `half(x) = trunc(x × 0.5)`（只在原点里用）；中间量按双精度；
 *    `canvasW = trunc(canvasH × (1 / t))` 算的是 **`1/t` 再乘**（⛔ 不是 `canvasH / t`，边界差 1 像素）；
 *    比较 `rH ≤ rW` **含等号**（相等算"高受限"）。
 * 3. **三分支**（`T = 0 / 1 / 2` 的原点 / 视口 / 缩放 / `view`）与 **VF 关**的 1:1 居中。
 * 4. **固定分辨率模式不进这条公式**：它只在**取值器**里把缩放短路成 1、原点短路成 0
 *    （那一步在绘制入口 `scene-draw-plan.ts` 里；本函数照公式算原值）。
 * 5. **退化出口**（`vfOn` 且 `T ∉ {0,1,2}`）：只把 `view` 置单位阵，原点 / 视口 / 缩放 **保持旧值**
 *    ⇒ 本函数用 `input.previous` 带回旧值（没有上一轮 ⇒ 中性值），并用 `degenerate` 标出来
 *    （⛔ 不许把"保持旧值"实现成"清 0"）。
 *
 * ★ 证据与建议的落点：`.tmp/spec-draft/batch8/display-fit/impl-notes.md` §1–§3
 *   （生产者 `sub_497880`；本文只实现**算术**，不把 EA / 偏移写进语义层）。
 * ★ `view` 是**列主序** 4×4（与 `scene-draw-plan.ts` 的矩阵口径一致；只有对角线，无平移）。
 */

/* ─────────────────────────────── 输入 / 输出 ─────────────────────────────── */

/**
 * 上一轮的适配结果（**退化出口**要用：`T ∉ {0,1,2}` 时原点 / 视口 / 缩放保持旧值）。
 * ★ 显式传进来，⛔ 不在模块里存全局状态（本文件是纯函数）。
 */
export interface DisplayFitPrevious {
  originX: number;
  originY: number;
  viewportW: number;
  viewportH: number;
  scaleX: number;
  scaleY: number;
}

/** `computeDisplayFit` 的输入（全部来自配置 / 图形宿主，**没有一条来自脚本或存档**） */
export interface DisplayFitInput {
  /** 后缓冲宽 `BW`（屏幕像素；本实现 = 当前 Pixi 画布宽，见 `session.ts` 的调用点） */
  screenW: number;
  /** 后缓冲高 `BH` */
  screenH: number;
  /** 虚拟全屏是否开：配置 `display:VirtualFullScreen` **非 0** 即开 */
  vfOn: boolean;
  /** 虚拟全屏类型 `T`（0 / 1 / 2；`vfOn === false` ⇒ 用不到，调用方传 0） */
  vfType: number;
  /** 宽高比模式 `A`（0 = 按比值适配；非 0 = 画布直接取后缓冲） */
  aspectMode: number;
  /** 参考宽（VF 开 = max(桌面, `set:WinX`) 逐分量；VF 关 = `display:FullScreenWidth`，缺则客户区） */
  refW: number;
  /** 参考高（同上，`set:WinY` / `display:FullScreenHeight`） */
  refH: number;
  /** **当前画布宽**（VF 关时由调用方摆好；VF 开时被本次公式覆写） */
  canvasW: number;
  /** **当前画布高** */
  canvasH: number;
  /**
   * 上一轮的适配结果；只有**退化出口**（`vfOn` 且 `T ∉ {0,1,2}`）会用到。
   * 缺省 ⇒ 该出口用中性值（原点 0、视口 = 输入画布、缩放 1）并把 `degenerate` 置 true。
   */
  previous?: DisplayFitPrevious;
}

/** 适配结果（渲染器上那几格的领域视图） */
export interface DisplayFit {
  /** 画布宽（写回；VF 关 / 退化出口时 = 输入值） */
  canvasW: number;
  /** 画布高 */
  canvasH: number;
  /** 视口原点 X（整数；画布坐标） */
  originX: number;
  /** 视口原点 Y */
  originY: number;
  /** 视口宽 */
  viewportW: number;
  /** 视口高 */
  viewportH: number;
  /** 内容缩放 X（浮点） */
  scaleX: number;
  /** 内容缩放 Y */
  scaleY: number;
  /** 场景视图矩阵（**列主序** 4×4；VF 关 / `T === 0` / 退化出口 = 单位阵） */
  view: number[];
  /**
   * 退化出口（`vfOn` 且 `T ∉ {0,1,2}`）为 true ⇒ 原点 / 视口 / 缩放是**旧值**（不是本次算出来的）。
   * ★ 调用方据此决定"不要覆盖上一轮结果"；⛔ 不要把它当成 `T` 的第三种取值。
   */
  degenerate: boolean;
}

/* ─────────────────────────────── 纯函数 ─────────────────────────────── */

/**
 * 显示适配：按 spec 的主公式算 `(canvas, 原点, 视口, 缩放, view)`。
 *
 * ★ 严格照伪代码的**逐格写法**：⛔ 不要"化简"成通用拟合（例如 `min(kx, ky)` 反推原点：
 *   两个分支的基准量不同 —— T=0/1 用画布 `CW/CH`，T=2 用参考 `refW/refH`）。
 * ★ `trunc` 走 `Math.trunc`：`(1000 − 1023) / 2 = −11.5` ⇒ **−11**（⛔ floor 会给 −12）。
 */
export function computeDisplayFit(input: DisplayFitInput): DisplayFit {
  const BW = input.screenW;
  const BH = input.screenH;
  const T = input.vfType | 0;
  const A = input.aspectMode | 0;

  // ── 出口 0：虚拟全屏关 ⇒ 缩放 (1,1) + 画布在屏幕里居中（矩阵是调用方置的单位阵）──
  if (!input.vfOn) {
    const ox = half(input.canvasW - BW);
    const oy = half(input.canvasH - BH);
    return {
      canvasW: input.canvasW,
      canvasH: input.canvasH,
      originX: ox,
      originY: oy,
      viewportW: input.canvasW - 2 * ox,
      viewportH: input.canvasH - 2 * oy,
      scaleX: 1,
      scaleY: 1,
      view: identity4(),
      degenerate: false,
    };
  }

  // ── 公共前导：先把 view 置单位阵（T=0 / 退化出口都不再写它）──
  const view = identity4();

  // ── 退化出口：T ∉ {0,1,2} ⇒ 只写 view，其余格保持旧值（⛔ 不是清 0）──
  if (T !== 0 && T !== 1 && T !== 2) {
    const previous = input.previous ?? null;
    return {
      canvasW: input.canvasW,
      canvasH: input.canvasH,
      originX: previous?.originX ?? 0,
      originY: previous?.originY ?? 0,
      viewportW: previous?.viewportW ?? input.canvasW,
      viewportH: previous?.viewportH ?? input.canvasH,
      scaleX: previous?.scaleX ?? 1,
      scaleY: previous?.scaleY ?? 1,
      view,
      degenerate: true,
    };
  }

  const kx = input.refW / BW;
  const ky = input.refH / BH;
  let CW = BW;
  let CH = BH;
  let ox = 0;
  let oy = 0;
  let vw = 0;
  let vh = 0;
  let sx = 1;
  let sy = 1;

  // ── 画布（T=0 / T=1 同一份；T=2 用参考尺寸）──
  if (T === 0 || T === 1) {
    if (A === 0) {
      const t = input.refH / input.refW;
      CH = Math.trunc(BW * t);
      if (CH < BH) {
        CH = BH;
        // ★ 算的是 `1/t` 再乘（⛔ 不是 `BH / t`；边界上可能差 1 像素）
        CW = Math.trunc(BH * (1 / t));
      }
    } else {
      CW = BW;
      CH = BH;
    }
  } else {
    CW = input.refW;
    CH = input.refH;
  }
  // ★ 两个比值必须在画布块**之后**取（A != 0 时 CH 已被改成 BH ⇒ rH = 1；顺序错了 sy 会错）
  const rW = CW / BW;
  const rH = CH / BH;

  if (T === 0) {
    // ── T=0：位置不缩放、矩阵恒单位阵 ──
    if (A === 0) {
      if (rH <= rW) {
        ox = half(CW - BW * rH);
        oy = 0;
        vw = CW - 2 * ox;
        vh = CH;
      } else {
        ox = 0;
        oy = half(CH - BH * rW);
        vw = CW;
        vh = CH - 2 * oy;
      }
    } else {
      ox = 0;
      oy = 0;
      vw = CW;
      vh = BH;
    }
  } else if (T === 1) {
    // ── T=1：矩阵 = Scaling(被取到的那个参考比)；位置缩放 = 同一个比值 ──
    if (A === 0) {
      if (rH <= rW) {
        ox = half(CW - BW * rH);
        oy = 0;
        vw = CW - 2 * ox;
        vh = CH;
        view[0] = ky;
        view[5] = ky;
        sx = rH;
        sy = rH;
      } else {
        ox = 0;
        oy = half(CH - BH * rW);
        vw = CW;
        vh = CH - 2 * oy;
        view[0] = kx;
        view[5] = kx;
        sx = rW;
        sy = rW;
      }
    } else {
      ox = 0;
      oy = 0;
      vw = CW;
      vh = BH;
      view[0] = kx;
      view[5] = ky;
      // A != 0 ⇒ 画布 = 后缓冲 ⇒ rW = rH = 1（照公式取值，⛔ 不写死 1）
      sx = rW;
      sy = rH;
    }
  } else {
    // ── T=2：画布 = 参考尺寸，位置缩放 = min 比值（基准量是 refW / refH，⛔ 不是 CW / CH）──
    if (A === 0) {
      if (ky <= kx) {
        ox = half(input.refW - BW * ky);
        oy = 0;
        vw = input.refW - 2 * ox;
        vh = input.refH;
        view[0] = ky;
        view[5] = ky;
        sx = ky;
        sy = ky;
      } else {
        ox = 0;
        oy = half(input.refH - BH * kx);
        vw = input.refW;
        vh = input.refH - 2 * oy;
        view[0] = kx;
        view[5] = kx;
        sx = kx;
        sy = kx;
      }
    } else {
      ox = 0;
      oy = 0;
      vw = input.refW;
      vh = input.refH;
      view[0] = kx;
      view[5] = ky;
      sx = kx;
      sy = ky;
    }
  }

  return {
    canvasW: CW,
    canvasH: CH,
    originX: ox,
    originY: oy,
    viewportW: vw,
    viewportH: vh,
    scaleX: sx,
    scaleY: sy,
    view,
    degenerate: false,
  };
}

/* ─────────────────────────────── 纯数学助手 ─────────────────────────────── */

/** `half(x) = trunc(x × 0.5)`（spec 的算术约定第 2 条；⛔ 不是 `x / 2` 再 floor） */
function half(x: number): number {
  return Math.trunc(x * 0.5);
}

/** 单位阵（列主序） */
function identity4(): number[] {
  return [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1];
}
