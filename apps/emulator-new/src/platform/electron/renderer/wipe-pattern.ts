/**
 * `wipe-pattern.ts` —— **种类 2（`0x24f`）的 12 种抹除图案**的纯函数
 * （`CORE-OPS-SCENE-COMMAND-RECORDS`「12 种抹除图案（条件表）」；`data/spec/chapters/60-ops-scene.md` §种类 2）。
 *
 * ★ **纯函数、无 Pixi、无 DOM、无 `node:*`**：所以它能被 `@env pure` 的守卫直接跑
 *   （`tools/test/emulator-new-wipe-pattern.test.mjs`）。⛔ 不要在这里画东西 —— 落笔是 `pixi-host.ts` 的事。
 * ★ 图案**只决定"矩形怎么切"**（spec §1.5 / `draft-rule.md` §2）：
 *   它不决定抹除矩形从哪来（同键绘制项）、不决定键区间（区间 A / B 各喂一块画布）、不决定何时画、
 *   不决定落点（视野偏移只影响"落点"，不影响矩形）、不决定混合与颜色（12 模式完全一样）。
 *
 * ════════════════ 一族（0..3）：整幅、按"道"跳 ════════════════
 *
 * - `步数 = ⌊轴长 ÷ S⌋ + 2`、`步长 = 持续时间 ÷ 步数`、`步序 = ⌊经过时间 ÷ 步长⌋`（**1 / 3 再 +1**）；
 * - `让出量 = S × 步序`（量化到 `S` 的整数倍，⛔ 不是逐像素）；
 * - 轴 = 宽（0 / 1）或高（2 / 3）；偶数从左 / 上长、奇数从右 / 下长；
 * - **恰好 2 笔**：第 1 笔 = 还没让出的那段（画布 A）、第 2 笔 = 让出区（画布 B）。
 *
 * ════════════════ 二族（4..7）：条带、同相、逐像素 ════════════════
 *
 * - `条带数 = ⌊轴长 ÷ S⌋ + 1`、带宽恒为 `S`；
 * - `步长 = 持续时间 ÷ (S + 1)`、`步序 = ⌊经过时间 ÷ 步长⌋`（单位是**像素**）；
 * - 每条带第 2 笔宽 = 步序，并**钳在本带内**（`0..S`）；方向 4 左→右 / 5 右→左 / 6 上→下 / 7 下→上。
 *
 * ════════════════ 三族（8..11）：条带、错相、逐像素 ════════════════
 *
 * - `步长 = 持续时间 ÷ (S + 条带数)`；第 `i` 条带的让出量 = `步序 − i`（第 i 带晚 i 个像素步）；
 *   非正 ⇒ 该带第 2 笔**空**（这里输出零宽矩形：空矩形 = 绘制入口直接返回，属**入口**性质，见 spec U3）。
 *
 * ════════════════ 条带网格（二 / 三族共用） ════════════════
 *
 * 每条带宽都是 `S`、`余数 = 轴长 mod S`，网格**不与矩形对齐**（`draft-rule.md` §1.4）：
 *
 * | 模式组 | 锚点 | 由此产生的越界 |
 * |---|---|---|
 * | 4 / 8（沿宽，左→右） | 最后一条带的**右缘** = 矩形右缘 | 第 0 带左缘 = `左缘 − (S − 余数)` |
 * | 6 / 10（沿高，上→下） | 最后一条带的**下缘** = 矩形下缘 | 第 0 带上缘 = `上缘 − (S − 余数)` |
 * | 5 / 9（沿宽，右→左） | 第 0 带的**左缘** = `右缘 − 余数` | 第 0 带右缘 = `右缘 + (S − 余数)` |
 * | 7 / 11（沿高，下→上） | 第 0 带的**上缘** = `下缘 − 余数` | 第 0 带下缘 = `下缘 + (S − 余数)` |
 *
 * ⇒ 总覆盖 `S × 条带数 ≥ 轴长`，**总会多出 ≤ S−1**（`余数 = 0` 时也多算一条带）。
 * ⛔ 这里**不**把网格裁到抹除矩形：spec 明确"只按渲染器矩形裁剪"，矩形外的像素照画（U2 未定的是**可见后果**）。
 *
 * ════════════════ 职责边界（⛔ 别往这里加） ════════════════
 *
 * - **抹除矩形从哪来**：命令档按键去绘制项表找同键绘制项、用它自己的位置与四条边算（缺条目 ⇒ 清持续时间 +
 *   **中止整趟**命令档，不是 continue）⇒ 那是 `scene-command-plan.ts` 的事。
 * - **时间戳 / 延迟**：`经过时间 = 场景时刻 − (时间戳 + 延迟)` 由调用方算好传进来（本函数只认 `elapsed`）。
 *   时间戳的盖章（写表清 0 + 首帧盖成场景时刻）也在调用方。
 * - **`mode < 0`**（只可能来自"分割幅 < 1"那条错误记录）：本函数**一笔都不画**，但「强制完成」副作用是
 *   命令档的事（⛔ 不在这里置位）。★ 这一档是**正常**产物 ⇒ 不记诊断。
 * - **落点**：每笔除矩形外还带两个位置三元组（抹除矩形左上角 / 该笔矩形左上角 − 视野偏移），
 *   两者只影响落点 ⇒ 由落笔侧算（本函数只给 `rect`）。
 * - **两块画布怎么装内容、什么时候切渲染目标、到期清持续时间、置「有动画」**：全在调用方。
 *
 * ════════════════ 两处**保持未定**（⛔ 不许自己补齐） ════════════════
 *
 * - `持续时间 = 0` ⇒ 步长 = 0（除零）的后果 spec **不定**（U1）。本函数**返回空 + 记
 *   `wipe-duration-zero`**，⛔ **不**把它定义成"立即完成"或"永不开始"。
 * - `分割幅 ≤ 0` ⇒ `wipe-split-zero`（写入侧已夹到 ≥ 1；走到这里说明调用方没夹）。
 * - ★ 附带一档：`经过时间 < 0`（延迟还没过）时，spec 的 `步序` 写着**不夹取**
 *   （`60-ops-scene.md`「12 种抹除图案（条件表）」第 4 条：`步序 = ⌊经过时间 ÷ 步长⌋`（不夹取）），
 *   所以一族按公式**原样外推**（让出量为负 ⇒ 第 1 笔越出矩形、第 2 笔反向）。条带族的"钳在本带内"是
 *   图案自己的规则 ⇒ 那两族在负区**会**被钳到 0。两者口径不同，**不是**笔误。
 */

/* ─────────────────────────────── 输入 / 输出 ─────────────────────────────── */

/** 一块画布（**领域名未定**：spec U4 ⇒ 只说"第一块 / 第二块"；⛔ 不叫前景 / 背景 / 新 / 旧） */
export type WipeCanvas = 'A' | 'B';

/** 第一块画布（区间 A 的内容；机器侧纹理槽 36） */
export const CANVAS_A: WipeCanvas = 'A';
/** 第二块画布（区间 B 的内容；机器侧纹理槽 37） */
export const CANVAS_B: WipeCanvas = 'B';

/** 12 种图案 = 类型编号 `0..11`（两端闭）；`−1` 是"分割幅 < 1"那条错误记录的产物 */
export const MODE_LAST = 11;

/** 一个矩形（屏幕像素；左上 / 右下，右下**不含**；与绘制入口的矩形同口径） */
export interface WipeRect {
  left: number;
  top: number;
  right: number;
  bottom: number;
}

/** 一笔 = "从哪块画布搬哪个矩形"（源画布坐标 == 目标坐标；落点由落笔侧算） */
export interface WipeStroke {
  canvas: WipeCanvas;
  rect: WipeRect;
}

/**
 * 诊断（**不是**引擎错误 ⇒ 不进错误通道）：出现这些说明这一帧落进了 spec **保持未定**的区间。
 * ⛔ 不许把它们当成"我们知道了引擎怎么做"。
 */
export type WipeDiagnostic =
  /** `分割幅 ≤ 0`：写入侧会夹到 1；走到这里说明调用方没夹（步长 / 条带数都无从算） */
  | 'wipe-split-zero'
  /** `持续时间 ≤ 0`：spec U1「步长为 0（除零）的后果」**保持未定** */
  | 'wipe-duration-zero'
  /** `类型 > 11` 或非有限数：写入侧已校验（`∉ [0, 11]` 一律不写表）⇒ 出现即上游坏数据 */
  | 'wipe-mode-out-of-range';

/**
 * 图案的输入 = spec 的**五样**（抹除矩形 / 分割幅 / 经过时间 / 持续时间）：
 * 前两样是"切哪儿"、后两样把时间换成"让出多少"。
 * ★ `经过时间` 由调用方按 `场景时刻 − (时间戳 + 延迟)` 算好；**可为负**（延迟还没过）。
 */
export interface WipeInput {
  /** 类型 / 模式（`0..11`；`< 0` = 错误记录，一笔不画） */
  mode: number;
  /** 抹除矩形（来自同键绘制项；本函数**不**再裁到任何东西） */
  rect: WipeRect;
  /** 分割幅 `S`；调用方保证 `≥ 1`（写入时已夹） */
  split: number;
  /** 经过时间 `= 场景时刻 − (时间戳 + 延迟)`（毫秒；可为负） */
  elapsed: number;
  /** 持续时间（毫秒；`≤ 0` ⇒ 保持未定，见 {@link WipeDiagnostic}） */
  duration: number;
}

/** 结果：**按绘制顺序**排的笔 + 诊断 */
export interface WipeResult {
  /**
   * 本帧要画的笔，**顺序即绘制顺序**：
   * - 一族 ⇒ 恰好 2 笔（`A`、`B` 各一）；
   * - 二 / 三族 ⇒ 每带 2 笔、共 `条带数 × 2` 笔（每一带内部仍然**先 A 后 B**）。
   * ★ 空数组 = 这一帧一笔都不画（非法 / 未定档）。
   */
  strokes: WipeStroke[];
  /** spec 保持未定的档（去重、按判定顺序） */
  diagnostics: WipeDiagnostic[];
}

/* ─────────────────────────────── 主函数 ─────────────────────────────── */

/**
 * 12 种图案的唯一入口（纯函数）：**一笔都不落，只算矩形**。
 *
 * 判定次序：未定档（分割幅 / 持续时间）→ 类型范围 → 分三族算笔。
 * ★ 先判未定档是**有意**的：`S = 0` 时"步数 / 条带数"根本算不出来，不能靠"类型 < 0 反正不画"糊过去。
 */
export function wipePattern(input: WipeInput): WipeResult {
  const { rect, split, elapsed, duration } = input;
  const diagnostics: WipeDiagnostic[] = [];

  // 未定档：返回空 + 记诊断。⛔ 不定义成"立即完成 / 永不开始"。
  if (!(split > 0)) diagnostics.push('wipe-split-zero');
  if (!(duration > 0)) diagnostics.push('wipe-duration-zero');
  if (diagnostics.length > 0) return { strokes: [], diagnostics };

  const mode = Math.trunc(input.mode);
  // 类型 < 0：落不到任何图案（一笔不画）；「强制完成」副作用不在这里（是命令档的事）。
  if (mode < 0) return { strokes: [], diagnostics };
  if (!Number.isFinite(mode) || mode > MODE_LAST) {
    diagnostics.push('wipe-mode-out-of-range');
    return { strokes: [], diagnostics };
  }

  const strokes = mode <= 3
    ? wholeStrokes(mode, rect, split, elapsed, duration)
    : stripeStrokes(mode, rect, split, elapsed, duration);
  return { strokes, diagnostics };
}

/* ─────────────────────────────── 一族（0..3）─────────────────────────────── */

/**
 * 一族：整幅、按"道"跳。
 *
 * `步数 = ⌊轴长 ÷ S⌋ + 2`、`步长 = 持续时间 ÷ 步数`、`让出量 = S × 步序`；
 * **步序的相位**：0 / 2 用 `⌊经过时间 ÷ 步长⌋`，1 / 3 用它 **+1**（⇒ 同一时刻模式 1 比模式 0 多让一道）。
 * ★ 加法是**相位**，⛔ 不是 `Math.ceil` / 四舍五入（`ceil(0)` = 0，而观测是 `floor(0) + 1` = 1）。
 * ★ `步序` 按 spec **不夹取**（上限不裁）；负的经过时间因而给出负的让出量（见文件头"保持未定"附注）。
 */
function wholeStrokes(
  mode: number,
  rect: WipeRect,
  split: number,
  elapsed: number,
  duration: number,
): WipeStroke[] {
  const alongWidth = mode === 0 || mode === 1;
  const axisLength = alongWidth ? rect.right - rect.left : rect.bottom - rect.top;
  const stepCount = Math.floor(axisLength / split) + 2;
  const step = duration / stepCount;
  const phase = mode === 0 || mode === 2 ? 0 : 1;
  const index = Math.floor(elapsed / step) + phase;
  const reveal = split * index;

  const { left, top, right, bottom } = rect;
  switch (mode) {
    // 0：让出区从左缘向右长
    case 0:
      return pair(
        { left: left + reveal, top, right, bottom },
        { left, top, right: left + reveal, bottom },
      );
    // 1：让出区从右缘向左长（相位 +1）
    case 1:
      return pair(
        { left, top, right: right - reveal, bottom },
        { left: right - reveal, top, right, bottom },
      );
    // 2：让出区从上缘向下长
    case 2:
      return pair(
        { left, top: top + reveal, right, bottom },
        { left, top, right, bottom: top + reveal },
      );
    // 3：让出区从下缘向上长（相位 +1）
    default:
      return pair(
        { left, top, right, bottom: bottom - reveal },
        { left, top: bottom - reveal, right, bottom },
      );
  }
}

/**
 * 一对笔：**先画布 A（区间 A）、后画布 B（区间 B）**，两笔**相接不重叠**。
 * ★ 顺序是 spec 的观测（`draft-rule.md` §1.5）：换过来只在有重叠的输入上可见 ⇒ 正常输入看不出差别，
 *   所以顺序只能靠断言守（守卫 `emulator-new-wipe-pattern` 的"每带先 A 后 B"用例）。
 */
function pair(first: WipeRect, second: WipeRect): WipeStroke[] {
  return [
    { canvas: CANVAS_A, rect: first },
    { canvas: CANVAS_B, rect: second },
  ];
}

/* ─────────────────────────────── 二 / 三族（4..11）─────────────────────────────── */

/**
 * 二族（4..7，同相）/ 三族（8..11，错相）：把抹除矩形沿一条轴切成 `条带数` 条宽 `S` 的带。
 *
 * - 二族：`步长 = 持续时间 ÷ (S + 1)`，每条带的让出量都 = 步序（**像素**），钳在 `0..S`；
 * - 三族：`步长 = 持续时间 ÷ (S + 条带数)`，第 `i` 带的让出量 = `步序 − i`，钳在 `0..S`（非正 ⇒ 第 2 笔空）。
 * - 每条带**恰好 2 笔**（先 A 后 B）；让出量走满后第 1 笔退化成零宽（空矩形 = 入口直接返回，U3）。
 */
function stripeStrokes(
  mode: number,
  rect: WipeRect,
  split: number,
  elapsed: number,
  duration: number,
): WipeStroke[] {
  const alongWidth = mode % 4 < 2;
  const forward = mode % 4 === 0 || mode % 4 === 2;
  const staggered = mode >= 8;

  const axisStart = alongWidth ? rect.left : rect.top;
  const axisEnd = alongWidth ? rect.right : rect.bottom;
  const axisLength = axisEnd - axisStart;

  // 网格：条带数 / 余数（余数 = 轴长 mod S，落在 [0, S)）
  const stripeCount = Math.floor(axisLength / split) + 1;
  const remainder = axisLength - split * Math.floor(axisLength / split);

  const step = staggered ? duration / (split + stripeCount) : duration / (split + 1);
  const index = Math.floor(elapsed / step);

  const strokes: WipeStroke[] = [];
  for (let stripe = 0; stripe < stripeCount; stripe += 1) {
    // 锚点：正向（4/8、6/10）最后一带贴远边；反向（5/9、7/11）第 0 带贴远边（越出 S − 余数）
    const start = forward
      ? axisEnd - split * (stripeCount - stripe)
      : axisEnd - remainder - split * stripe;
    const end = start + split;
    // 让出量：二族同相、三族第 i 带晚 i 个像素步；两族都**钳在本带内**
    const reveal = clamp(staggered ? index - stripe : index, 0, split);

    if (alongWidth) {
      strokes.push(
        { canvas: CANVAS_A, rect: { left: forward ? start + reveal : start, top: rect.top, right: forward ? end : end - reveal, bottom: rect.bottom } },
        { canvas: CANVAS_B, rect: { left: forward ? start : end - reveal, top: rect.top, right: forward ? start + reveal : end, bottom: rect.bottom } },
      );
    } else {
      strokes.push(
        { canvas: CANVAS_A, rect: { left: rect.left, top: forward ? start + reveal : start, right: rect.right, bottom: forward ? end : end - reveal } },
        { canvas: CANVAS_B, rect: { left: rect.left, top: forward ? start : end - reveal, right: rect.right, bottom: forward ? start + reveal : end } },
      );
    }
  }
  return strokes;
}

/** 夹到 `[low, high]`（条带族的"钳在本带内"） */
function clamp(value: number, low: number, high: number): number {
  if (value < low) return low;
  if (value > high) return high;
  return value;
}
