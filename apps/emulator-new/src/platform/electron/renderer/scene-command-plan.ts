/**
 * `scene-command-plan.ts` —— **命令档的纯规划器**（`CORE-SCENE-DRAW-ORDER` / `CORE-OPS-SCENE-COMMAND-RECORDS`）。
 *
 * 一帧里命令档**按什么次序、对哪张表、发哪几趟请求、哪些条目算已消费**，全部在这里决定：
 * 输入 = 命令记录快照 + 场景时刻 + 场景自身键窗 + 三张表的键集合（种类 2 还要绘制项的**矩形**）
 * + 着色器能力档 + 两个场景格；
 * 输出 = 每条命令的阶段列表（切目标 / 区间之下 / 覆盖区间 / 拷贝 / 遮罩混合 / 合成）、本帧的已消费键、
 * 时间戳盖章、实现缺口诊断、以及「渲染步收尾是否清整张命令表」。
 *
 * ★ **纯函数、无 Pixi、无 DOM**：所以它能用 `@env pure` 的守卫直接跑
 *   （`tools/test/emulator-new-scene-command-plan.test.mjs`）。⛔ 不要 import `pixi.js` / `document` / `node:*`。
 * ★ **本文件只规划、不画**（S0 / S1）：阶段是"引擎会发什么请求"的**结构**，不是像素。
 *   平台侧仍未执行命令档 ⇒ `scene-order.ts` 的 `scene-command-pass-render-unmodeled` 显式记账，
 *   ⛔ 不许把"有阶段列表"说成"已经画过了"。
 *
 * ════════════════ 按 spec 的哪几条写 ════════════════
 *
 * 1. **命令键严格升序**：遍历序 = 命令表的排序键（不是插入次序）⇒ `commands` 输入允许乱序，
 *    本函数自己按 `commandKey` 升序排（守卫用乱序输入钉住）。
 * 2. **每条命令的前置（次序固定）**：取记录 → **时间戳为 0 ⇒ 盖成本帧场景时刻** →
 *    用**覆盖区间起点**与**场景自身键窗**算「跳过额外变换」→ 按种类分派。
 *    ★ 盖章写进输出 `stamps`（规划器不改输入，也不改 core 状态）。
 * 3. **空表无动作**：命令表为空 ⇒ 一个请求都不发、一个格都不写 ⇒ 输出全空（含诊断）。
 * 4. **三表合并序**：命令档只覆盖**网格 → 辅助表 A → 绘制项**，按键严格升序、同键按这个平键序；
 *    **辅助表 B 不参与**（它永远落到收尾扫描）⇒ 输入里根本没有辅助表 B。
 *    ★ 负键口径按表分：网格 / 辅助表 A 的负键按哨兵**不参与**；绘制项**不做符号检查**。
 * 5. **区间之下**（不是种类 1 独有）：
 *    - 种类 1：**仅首帧** ∧ 能力档 < 2，用该条唯一的区间；
 *    - 种类 0 / 2：**每帧的每块画布** ∧ 能力档 < 2，用本块那一段，**跳过落在另一段里的条目**；
 *    - 种类 3：**不发生**。
 *    它画的是"键**小于**本段起点"的条目、**不**记「本轮已消费」⇒ 这些条目同一帧会被发两次。
 * 6. **已消费**：覆盖区间 / 种类 1 的"只标记支"把条目记成「本轮已消费」；
 *    区间之下**不**记。输出 `consumedKeys` 就是收尾扫描要跳过的键（升序去重）。
 * 7. **种类 1 的第一帧门**：判据是**记录的时间戳 === 本帧场景时刻**（相等，不是"首次"）。
 *    首帧：区间之下（条件如上）→ 覆盖区间（画 + 标记）→ 整幅拷贝 → 每帧遮罩混合；
 *    之后：**零发射**只标记 → 每帧遮罩混合。
 *    ★ 时钟不前进时已盖章的命令**仍会**落回首帧支 —— 这是相等判据的后果，⛔ 不许"修"成别的判据。
 * 8. **种类 2 的中止**：找不到同键绘制项 ⇒ 清该命令的持续时间并**中止整档**
 *    （其后的命令一条都不规划，⛔ 不是 continue）。
 * 9. **命令表生命周期**：`clearTable = 本帧「有动画」=== 0 && 「强制完成」=== 0`
 *    （渲染步在收尾扫描**之后**按它整表清空）。★ 本规划器只**算出**这一格，⛔ 不改 core 状态。
 * 10. **种类 2 的抹除矩形来自同键绘制项**（不是命令自己的操作数）：条目在绘制项表里但矩形没进快照
 *     （`tables.itemRects` 没有它）⇒ 记 `scene-command-wipe-rect-unmodeled` 且**不产笔**
 *     （⛔ 不编矩形）；矩形有 ⇒ `经过时间 = now − (盖过章的时间戳 + 延迟)`、类型 / 分割幅 / 持续时间
 *     取自记录，交给 `wipePattern`，笔挂在 `composite.strokes` 上，`wipe-*` 未定档原样转成
 *     `scene-command-wipe-*` 规划诊断。★ 只算矩形，⛔ 一笔都没落。
 *
 * ════════════════ 明确不在这层的东西（⛔ 不假装） ════════════════
 *
 * - **像素操作**：离屏画布的建 / 切目标 / 表面拷贝 / 逐像素遮罩 / 效果技法 —— 一律没有执行；
 *   阶段只写"发到哪个目标、用哪一段区间"，平台侧仍然是未建模（见 `scene-command-pass-render-unmodeled`）。
 * - **种类 2 的笔只算不画**：`composite.strokes` 是"引擎会把哪些矩形从哪块画布搬走"的**结构**；
 *   ⛔ 平台侧一个都没画（`pixi-host.ts` 不读 `strokes`），也⛔ 别把"有笔"说成"抹除过"。
 *   落点（视野偏移）/ 两块画布的内容 / 混合档与颜色仍不在本层。
 * - **有效位门**：输入是**键集合**（不是条目记录）⇒ 这里只能施加"未消费"门；
 *   「有效」这一位由收尾扫描 / 将来的命令档执行层在 `planDraw` 里读 `flags` 施加（⛔ 不声称这里做过）。
 * - **辅助表 A 的键**：快照里没有这张表 ⇒ `auxA: null` 表示**未建模**（⛔ 不是空表：
 *   空表会静默让三表合并序在同一键上错位）；这一趟照常规划，但记 `scene-command-aux-a-unmodeled`。
 * - **能力档**：`shaderTier: null` = 未建模 ⇒ 按"≥ 2"档跑（区间之下不发生）并记
 *   `scene-command-tier-unmodeled`；⛔ 不把"没建模"实现成"能力档 2"。
 * - **进度 / 插值量**：种类 0 的进度、种类 3 的四对插值量要等执行层；规划器只给阶段结构。
 * - **种类 2 的「类型 < 0 ⇒ 场景「强制完成」← 1」副作用**：核心没有这一格（`forcedComplete`
 *   只是输入，恒投影 0）⇒ 本层**不产生**这个副作用，⛔ 不假装它已经生效。
 * - **种类 3 的效果技法**：合成阶段只有一条，内部没建模。
 * - **种类 2 的落点与混合**：笔的矩形有了（`strokes`），但两笔的**落点**（抹除矩形左上角 /
 *   该笔矩形左上角 − 视野偏移）、两块画布的内容、混合档与颜色仍归执行层（⛔ 不在图案里再改一次符号）。
 */
import type { SceneCommandSnapshot } from '../protocol.ts';
import { inSelfTransformRange } from './scene-draw-plan.ts';
import { wipePattern } from './wipe-pattern.ts';
import type { WipeDiagnostic, WipeRect, WipeStroke } from './wipe-pattern.ts';

/* ─────────────────────────────── 常量 ─────────────────────────────── */

/** 两块离屏画布的**纹理槽号**（引擎里就是 36 / 37；与屏幕同尺寸，不进脚本纹理编号空间） */
export const CANVAS_A = 36;
export const CANVAS_B = 37;

/** 命令种类（记录首格；`0x223` / `0x24d` / `0x24f` / `0x250`·`0x251`） */
export const KIND_TEXTURE_OVERLAY = 0;
export const KIND_MOVIE_OVERLAY = 1;
export const KIND_WIPE = 2;
export const KIND_BLUR = 3;

/* ─────────────────────────────── 输入 ─────────────────────────────── */

/**
 * 三张同键表的**键集合**（⛔ 不含辅助表 B —— 它不参与命令档）。
 * ★ 这里只收"键"，不收条目记录：规划器决定**哪几个键进哪一趟**，条目的有效位 / 纹理 / 实参
 *   仍由执行层（`planDraw`）判。
 * ★ 唯一的例外是 {@link itemRects}：种类 2 的抹除矩形**不是**"发给某个条目的请求"，而是**合成阶段
 *   的几何**（spec：抹除矩形来自同键绘制项）⇒ 那一格必须进来。
 */
export interface CommandTableKeys {
  /** 网格表的键（负键按哨兵处理 ⇒ 不参与） */
  meshes: readonly number[];
  /**
   * 辅助表 A 的键；`null` = **未建模**（快照里没有这张表）。
   * ⛔ 不许传 `[]` 当"空表"：那会让三表合并序在同一键上静默错位。
   */
  auxA: readonly number[] | null;
  /** 绘制项表的键（**不做符号检查** ⇒ 负键照常参与，按升序排最前） */
  items: readonly number[];
  /**
   * **绘制项的矩形**（键 → 四条边）：种类 2 的**抹除矩形**从这里取
   * （spec：「抹除矩形不来自本命令的操作数，而来自绘制项表里与本命令同键的那条绘制项」）。
   * ★ 键集合仍是 {@link items}，这里只回答"哪些键**有**矩形" —— 条目在表里但这里没有它 ⇒
   *   记 `scene-command-wipe-rect-unmodeled` 并**不产笔**（⛔ 不拿 0 当矩形）。
   * ★ `undefined` = 调用方没有这份投影（后果同"一条都没有"：种类 2 记缺口）；
   *   ⛔ 不许用它表达"空表"（表有没有条目看 {@link items}）。
   */
  itemRects?: ReadonlyMap<number, WipeRect>;
}

/** 规划器的输入 */
export interface CommandPlanInput {
  /** 命令记录快照（允许乱序；规划器按命令键升序排） */
  commands: readonly SceneCommandSnapshot[];
  /** 本帧场景时刻（毫秒）—— 盖章值 + 种类 1 的第一帧判据 */
  now: number;
  /** 场景自身记录的**作用键窗** `[start, start + count)`（`0x229` 写） */
  selfKeyWindow: { start: number; count: number };
  /** 三张表的键集合 */
  tables: CommandTableKeys;
  /**
   * **着色器能力档**：0 / 1 / 2（2 = 支持 PS 2.0），决定「区间之下」是否发生；
   * `null` = 未建模（⛔ 不是 2：按 ≥2 跑**并**记 `scene-command-tier-unmodeled`）。
   */
  shaderTier: number | null;
  /** 本帧「有动画」场景格（0 / 1） */
  animating: number;
  /** 本帧「强制完成」场景格（0 / 1；核心还没有这一格 ⇒ 调用方传 0 并记账） */
  forcedComplete: number;
}

/* ─────────────────────────────── 输出 ─────────────────────────────── */

/**
 * 一个阶段的身份（按执行次序排列）。
 * ★ `mark` 是种类 1 的"只标记支"：**零发射**但把覆盖区间的条目各记一次已消费。
 */
export type CommandPlanStage =
  /** 区间之下（键 < 本段起点；不记已消费） */
  | 'below-range'
  /** 覆盖区间（画 + 记已消费） */
  | 'covered'
  /** 只标记（种类 1 非首帧：零发射） */
  | 'mark'
  /** 整幅表面拷贝（种类 1 首帧：离屏画布 → 影片纹理） */
  | 'copy'
  /** 每帧尾段的逐像素遮罩混合（种类 1） */
  | 'mask-blend'
  /** 合成到命令自己的纹理 */
  | 'composite';

/** 一个阶段的目标 */
export type CommandPlanTarget = typeof CANVAS_A | typeof CANVAS_B | 'own' | 'movie' | 'none';

/** 一个阶段（只写结构：目标 / 用哪一段 / 发哪些键 / 是否置已消费 / 是否真的发请求） */
export interface CommandPlanPhase {
  stage: CommandPlanStage;
  /** `36` / `37` = 离屏画布；`own` = 命令自己的纹理；`movie` = 记录里的影片纹理（种类 1）；`none` = 不切目标 */
  target: CommandPlanTarget;
  /** 用哪一段区间：`A` / `B`（种类 0 / 2）/ `single`（种类 1 / 3）；合成 · 拷贝 · 遮罩混合 = `null` */
  segment: 'A' | 'B' | 'single' | null;
  /**
   * 这一趟要发出的**表键**（三表合并序：网格 → 辅助表 A → 绘制项；同键按这个平键序）。
   * ★ 同键的多条目会出现多次（一个条目一个键）—— 执行层才能区分它们。
   */
  tableKeys: number[];
  /** 这一趟是否把这些键记成「本轮已消费」 */
  consumed: boolean;
  /** 这一趟是否**真的发出**绘制请求（种类 1 的只标记支 = `false`） */
  draw: boolean;
  /**
   * **种类 2 的抹除笔**（只有 `composite` 阶段有；按绘制顺序：一族恰好 2 笔、条带族每带 2 笔）。
   * ★ 这是"引擎会发什么请求"的**结构**，⛔ 不是"已经画过了"（平台侧仍未执行命令档）。
   * ★ 三态：`undefined` = 这一帧**算不出来**（没有同键绘制项的矩形 ⇒ 已记
   *   `scene-command-wipe-rect-unmodeled`）；`[]` = **算过但一笔都不发**（持续时间 0 / 分割幅 0 /
   *   类型 < 0 / 类型 > 11，各带自己的诊断）；非空 = 本帧要画的笔。
   */
  strokes?: WipeStroke[];
}

/** 一条已规划的命令 */
export interface PlannedCommand {
  /** 命令种类（0 / 1 / 2 / 3）；未知种类不进 `commands`，只在诊断里出现 */
  kind: number;
  /** 命令键（命令表的排序键） */
  commandKey: number;
  /** **盖章之后**的时间戳（输入 0 ⇒ 本帧场景时刻） */
  timestamp: number;
  /** 是否走「第一帧」支（**只有种类 1 有这一档**；判据 = 盖章后的时间戳 === now） */
  firstFrame: boolean;
  /**
   * 「跳过额外变换」（`0x229` 的作用键窗粗判）：**覆盖区间起点**落在 `[keyStart, keyStart + keyCount)` 内。
   * ★ 粗判只看**起点**：起点在窗外但区间跨进窗内 ⇒ 本标志为 `false`，窗内的条目**仍**乘额外变换
   *   （逐条目的判据在执行层）。⛔ 不要把它写成"区间与键窗相交"。
   */
  skipExtraTransform: boolean;
  /** 按次序的阶段（中止命令 = 空数组） */
  phases: CommandPlanPhase[];
  /** 本命令是否**中止整趟命令档**（种类 2 找不到同键绘制项） */
  abort: boolean;
}

/** 一条时间戳盖章（输入是 0、被盖成 `timestamp` 的命令） */
export interface CommandPlanStamp {
  commandKey: number;
  timestamp: number;
}

/** 实现缺口诊断（**不是**引擎错误 ⇒ 不进错误通道；由调用方转成诊断面板的一行） */
export type CommandPlanDiagnosticKind =
  /** 能力档未建模 ⇒ 按"≥ 2"跑（区间之下不发生） */
  | 'scene-command-tier-unmodeled'
  /** 辅助表 A 没有进快照 ⇒ 命令档少一条腿、三表合并序可能缺键 */
  | 'scene-command-aux-a-unmodeled'
  /** 种类 2 缺同键绘制项 ⇒ 清持续时间 + 中止整档（spec 的中止路径） */
  | 'scene-command-wipe-rect-missing'
  /** 种类 2 的同键绘制项在表里，但**抹除矩形**没进快照 ⇒ ⛔ 不编矩形、不产笔 */
  | 'scene-command-wipe-rect-unmodeled'
  /** 种类 2 的持续时间 ≤ 0：spec 保持未定（步长为 0 的除零后果）⇒ 本帧不产笔 */
  | 'scene-command-wipe-duration-zero'
  /** 种类 2 的分割幅 ≤ 0：写入侧已夹到 ≥ 1 ⇒ 出现即上游没夹（步数 / 条带数无从算）⇒ 不产笔 */
  | 'scene-command-wipe-split-zero'
  /** 种类 2 的类型 > 11（写入侧已校验 ⇒ 出现即上游坏数据）⇒ 落不到任何图案、不产笔 */
  | 'scene-command-wipe-mode-out-of-range'
  /** 记录首格不是 0..3（本实现不认识）⇒ 停止规划（⛔ 不猜它画什么） */
  | 'scene-command-kind-unknown';

export interface CommandPlanDiagnostic {
  kind: CommandPlanDiagnosticKind;
  /** 相关命令键（整档级缺口 = `null`） */
  commandKey: number | null;
}

/** 规划结果 */
export interface SceneCommandPlan {
  /** 本帧要执行的命令（命令键升序；中止命令含在内，其后的命令不在内） */
  commands: PlannedCommand[];
  /** 本帧被记成「本轮已消费」的键（升序、去重）⇒ 收尾扫描要跳过它们 */
  consumedKeys: number[];
  /** 本帧盖上去的时间戳（命令键升序） */
  stamps: CommandPlanStamp[];
  /** 实现缺口（按出现次序去重） */
  diagnostics: CommandPlanDiagnostic[];
  /**
   * 渲染步在**收尾扫描之后**是否要清整张命令表：
   * 本帧「有动画」= 0 **且**「强制完成」= 0 ⇒ `true`（整表清空，不是"删已完成的那条"）。
   * ★ 规划器只算这一格；清表动作属于渲染步（本层没有 core 写路径）。
   */
  clearTable: boolean;
}

/* ─────────────────────────────── 主函数 ─────────────────────────────── */

/**
 * 命令表 → 本帧的命令档计划。
 *
 * ★ **纯函数**：不读时钟、不改输入、不改 core 状态；盖章只是输出。
 * ★ 判定次序按 spec：空表 → 能力档缺口 → 每条命令（排序 → 取记录 → 盖章 → 键窗粗判 → 分派）。
 */
export function planSceneCommandPass(input: CommandPlanInput): SceneCommandPlan {
  const { now, tables, selfKeyWindow, animating, forcedComplete } = input;
  const clearTable = animating === 0 && forcedComplete === 0;
  // 空表无动作：一个请求都不发、一个格都不写 ⇒ 连诊断也不产生（没有命令档就谈不上缺口）
  if (input.commands.length === 0) {
    return { commands: [], consumedKeys: [], stamps: [], diagnostics: [], clearTable };
  }

  const diagnostics: CommandPlanDiagnostic[] = [];
  const note = (kind: CommandPlanDiagnosticKind, commandKey: number | null = null): void => {
    if (diagnostics.some((entry) => entry.kind === kind && entry.commandKey === commandKey)) return;
    diagnostics.push({ kind, commandKey });
  };

  // 能力档未建模：按"≥ 2"跑（区间之下不发生）并记账；⛔ 不把 null 当成 2 这个"结论"。
  if (input.shaderTier === null) note('scene-command-tier-unmodeled');
  const tier = input.shaderTier ?? 2;
  const belowRangeAllowed = tier < 2;
  // 辅助表 A 缺键：命令档少一条腿 ⇒ 照常规划，但必须显式记账（⛔ 不静默当空表）
  if (tables.auxA === null) note('scene-command-aux-a-unmodeled');

  /** 三表合并序（升序；同键 网格 → 辅助表 A → 绘制项） */
  const merged = mergeTableKeys(tables);
  /** 本帧已被命令档画过并置位的键（收尾扫描要跳过） */
  const consumed = new Set<number>();

  const planned: PlannedCommand[] = [];
  const stamps: CommandPlanStamp[] = [];
  const commands = [...input.commands].sort((a, b) => (
    a.commandKey < b.commandKey ? -1 : a.commandKey > b.commandKey ? 1 : 0
  ));

  for (const command of commands) {
    if (command.kind !== KIND_TEXTURE_OVERLAY
      && command.kind !== KIND_MOVIE_OVERLAY
      && command.kind !== KIND_WIPE
      && command.kind !== KIND_BLUR) {
      // 不认识的种类：结构无从规划 ⇒ 停在这里（后面的命令也不猜）
      note('scene-command-kind-unknown', command.commandKey);
      break;
    }

    // ① 时间戳为 0 ⇒ 盖成本帧场景时刻（本地副本与记录都取这个值；本层只输出）
    let timestamp = command.timestamp;
    if (timestamp === 0) {
      timestamp = now;
      stamps.push({ commandKey: command.commandKey, timestamp: now });
    }
    // ② 「跳过额外变换」：覆盖区间**起点**落在场景自身键窗里（计数 0 ⇒ 区间为空 ⇒ 恒 false）
    const skipExtraTransform = inSelfTransformRange(command.rangeA.start, selfKeyWindow.start, selfKeyWindow.count);
    const firstFrame = command.kind === KIND_MOVIE_OVERLAY && timestamp === now;

    // ③ 种类 2 的前置：找同键绘制项（缺失 ⇒ 清持续时间 + 中止整档，发生在任何切目标之前）
    if (command.kind === KIND_WIPE && !tables.items.includes(command.ownerKey)) {
      note('scene-command-wipe-rect-missing', command.commandKey);
      planned.push({
        kind: command.kind,
        commandKey: command.commandKey,
        timestamp,
        firstFrame: false,
        skipExtraTransform,
        phases: [],
        abort: true,
      });
      break; // ⛔ 不是 continue：中止整趟命令档
    }

    // ④ 种类 2 的抹除笔：矩形来自**同键绘制项**（缺矩形 ⇒ 记缺口 + 不产笔，⛔ 不编矩形）
    const wipeStrokes = command.kind === KIND_WIPE
      ? planWipeStrokes(command, now, timestamp, tables, note)
      : null;

    const phases = command.kind === KIND_MOVIE_OVERLAY
      ? planMovieOverlayPhases(command, merged, consumed, firstFrame, belowRangeAllowed)
      : command.kind === KIND_BLUR
        ? planBlurPhases(command, merged, consumed)
        : planTwoCanvasPhases(command, merged, consumed, belowRangeAllowed, wipeStrokes);

    planned.push({
      kind: command.kind,
      commandKey: command.commandKey,
      timestamp,
      firstFrame,
      skipExtraTransform,
      phases,
      abort: false,
    });
  }

  return {
    commands: planned,
    // 升序去重：收尾扫描按 key 访问，集合的次序不重要，但输出稳定便于守卫与诊断
    consumedKeys: [...consumed].sort((a, b) => (a < b ? -1 : a > b ? 1 : 0)),
    stamps,
    diagnostics,
    clearTable,
  };
}

/* ─────────────────────────────── 按种类的阶段序列 ─────────────────────────────── */

/**
 * 种类 0（纹理覆盖）/ 种类 2（抹除）：**两块画布 × 两段区间** + 合成。
 * 每块画布：切目标 → 设备就绪门 → [能力档 < 2：区间之下] → 覆盖区间（画 + 置已消费）→ 收尾。
 * ★ 区间 A **只**喂第一块、区间 B **只**喂第二块 —— ⛔ 不许合成一段。
 * ★ 种类 2 的图案笔挂在**合成**阶段的 `strokes` 上（种类 0 没有这一格）。
 */
function planTwoCanvasPhases(
  command: SceneCommandSnapshot,
  merged: readonly MergedKey[],
  consumed: Set<number>,
  belowRangeAllowed: boolean,
  wipeStrokes: WipeStroke[] | null,
): CommandPlanPhase[] {
  const phases: CommandPlanPhase[] = [];
  addCanvasPhases(phases, merged, consumed, belowRangeAllowed, CANVAS_A, 'A', command.rangeA, command.rangeB);
  addCanvasPhases(phases, merged, consumed, belowRangeAllowed, CANVAS_B, 'B', command.rangeB, command.rangeA);
  // 合成：两块画布 → 命令自己的纹理（种类 0 的进度出口在这一段的内部，仍未建模）
  const composite: CommandPlanPhase = {
    stage: 'composite',
    target: 'own',
    segment: null,
    tableKeys: [],
    consumed: false,
    draw: true,
  };
  // 种类 2：算过就挂（`[]` = 算过但一笔都不发）；`null` = 算不出来 ⇒ 不挂，⛔ 不编
  if (wipeStrokes !== null) composite.strokes = wipeStrokes;
  phases.push(composite);
  return phases;
}

/**
 * 种类 3（模糊转场）：**单段 + 合成**，每帧都跑。
 * ⛔ 没有「区间之下」（结构上就没有这一趟），也没有第一帧门。
 */
function planBlurPhases(
  command: SceneCommandSnapshot,
  merged: readonly MergedKey[],
  consumed: Set<number>,
): CommandPlanPhase[] {
  const phases: CommandPlanPhase[] = [];
  const covered = coveredKeys(merged, command.rangeA);
  phases.push({
    stage: 'covered',
    target: CANVAS_A,
    segment: 'single',
    tableKeys: covered,
    consumed: true,
    draw: true,
  });
  for (const key of covered) consumed.add(key);
  // 合成：离屏画布 → 命令自己的纹理（效果技法 / 退路都在这段内部；本层不选路径）
  phases.push({ stage: 'composite', target: 'own', segment: null, tableKeys: [], consumed: false, draw: true });
  return phases;
}

/**
 * 种类 1（影片覆盖）：**第一帧门 + 整幅拷贝 + 每帧遮罩混合**。
 *
 * - 首帧：区间之下（仅能力档 < 2）→ 覆盖区间（画 + 置已消费）→ 拷贝到影片纹理 → 遮罩混合；
 * - 之后：**零发射**的只标记支（仍把覆盖区间的条目记成已消费）→ 遮罩混合。
 * ★ 遮罩混合同样在首帧发生（每帧一次，两路共同），⛔ 不要写成只在"之后"。
 */
function planMovieOverlayPhases(
  command: SceneCommandSnapshot,
  merged: readonly MergedKey[],
  consumed: Set<number>,
  firstFrame: boolean,
  belowRangeAllowed: boolean,
): CommandPlanPhase[] {
  const phases: CommandPlanPhase[] = [];
  const covered = coveredKeys(merged, command.rangeA);
  if (firstFrame) {
    if (belowRangeAllowed) {
      // 区间之下只在首帧、且能力档 < 2；单段 ⇒ 没有"另一段"可跳过
      const below = belowRangeKeys(merged, command.rangeA.start, null, consumed);
      phases.push({
        stage: 'below-range',
        target: CANVAS_A,
        segment: 'single',
        tableKeys: below,
        consumed: false,
        draw: true,
      });
    }
    phases.push({
      stage: 'covered',
      target: CANVAS_A,
      segment: 'single',
      tableKeys: covered,
      consumed: true,
      draw: true,
    });
    for (const key of covered) consumed.add(key);
    // 整幅表面拷贝：**离屏画布 → 影片纹理**（⛔ 方向不是"用某编号画影片纹理"）
    phases.push({ stage: 'copy', target: 'movie', segment: null, tableKeys: [], consumed: false, draw: true });
  } else {
    // 只标记支：一个绘制请求都不发（包括影片纹理那次效果绘制），但仍置已消费
    phases.push({
      stage: 'mark',
      target: 'none',
      segment: 'single',
      tableKeys: covered,
      consumed: true,
      draw: false,
    });
    for (const key of covered) consumed.add(key);
  }
  // 尾段：每帧一次（遮罩混合；源 = 离屏画布、目标 = 命令自己的影片纹理）
  phases.push({ stage: 'mask-blend', target: 'movie', segment: null, tableKeys: [], consumed: false, draw: true });
  return phases;
}

/** 一块画布：切目标 → [区间之下] → 覆盖区间（+ 置已消费）→ 收尾 */
function addCanvasPhases(
  phases: CommandPlanPhase[],
  merged: readonly MergedKey[],
  consumed: Set<number>,
  belowRangeAllowed: boolean,
  canvas: typeof CANVAS_A | typeof CANVAS_B,
  segment: 'A' | 'B',
  range: SceneCommandSnapshot['rangeA'],
  other: SceneCommandSnapshot['rangeA'],
): void {
  if (belowRangeAllowed) {
    // 区间之下：键 < 本段起点、门 = 未消费、**跳过落在另一段里的条目**；不置已消费
    const below = belowRangeKeys(merged, range.start, other, consumed);
    phases.push({
      stage: 'below-range',
      target: canvas,
      segment,
      tableKeys: below,
      consumed: false,
      draw: true,
    });
  }
  const covered = coveredKeys(merged, range);
  phases.push({
    stage: 'covered',
    target: canvas,
    segment,
    tableKeys: covered,
    consumed: true,
    draw: true,
  });
  for (const key of covered) consumed.add(key);
}

/* ─────────────────────────── 种类 2 的抹除笔（算矩形，不落笔） ─────────────────────────── */

/** `wipePattern` 的未定 / 非法档 → 规划诊断（一一对应；⛔ 不合并成一条"抹除参数有问题"） */
const WIPE_DIAGNOSTIC_KIND: { readonly [K in WipeDiagnostic]: CommandPlanDiagnosticKind } = {
  'wipe-split-zero': 'scene-command-wipe-split-zero',
  'wipe-duration-zero': 'scene-command-wipe-duration-zero',
  'wipe-mode-out-of-range': 'scene-command-wipe-mode-out-of-range',
};

/**
 * 种类 2 的合成阶段要发的笔（**只算矩形**；`wipe-pattern.ts` 是唯一一份图案算式）。
 *
 * - **矩形来自同键绘制项**（`tables.itemRects.get(command.ownerKey)`；spec：抹除矩形不来自本命令的操作数），
 *   ⛔ 不拿 0 / 命令自己的格去编一个；
 * - `经过时间 = now − (盖过章的时间戳 + 延迟)`（`timestamp` 是**调用方盖过章**的值：输入 0 已经变成 now）；
 * - 类型 / 分割幅 / 持续时间直接取自记录；
 * - 返回 `null` = **算不出来**（条目在表里、但矩形没进快照）⇒ 调用方**不挂** `strokes`，只记
 *   `scene-command-wipe-rect-unmodeled`；返回 `[]` = **算过但这一帧一笔都不发**（未定档 / 类型越界 /
 *   类型 < 0 —— 后者是 spec 的**正常**产物，不记诊断）。
 * ★ `wipePattern` 的诊断（`wipe-*`）**原样转成**规划诊断并带上命令键，⛔ 不吞掉。
 */
function planWipeStrokes(
  command: SceneCommandSnapshot,
  now: number,
  timestamp: number,
  tables: CommandTableKeys,
  note: (kind: CommandPlanDiagnosticKind, commandKey?: number | null) => void,
): WipeStroke[] | null {
  const rect = tables.itemRects?.get(command.ownerKey);
  if (rect === undefined) {
    note('scene-command-wipe-rect-unmodeled', command.commandKey);
    return null;
  }
  const result = wipePattern({
    mode: command.mode,
    rect: { left: rect.left, top: rect.top, right: rect.right, bottom: rect.bottom },
    split: command.split,
    elapsed: now - (timestamp + command.delay),
    duration: command.duration,
  });
  for (const diagnostic of result.diagnostics) {
    note(WIPE_DIAGNOSTIC_KIND[diagnostic], command.commandKey);
  }
  return result.strokes;
}

/* ─────────────────────────────── 三表合并序 / 区间筛选 ─────────────────────────────── */

/** 表身份（只用于同键平键序：0 = 网格、1 = 辅助表 A、2 = 绘制项） */
type CommandTableId = 0 | 1 | 2;

/** 合并序里的一项（保留"来自哪张表"只是为了同键平键序；输出只用 `key`） */
interface MergedKey {
  key: number;
  table: CommandTableId;
}

/**
 * 三表合并序：**键严格升序**；同键**网格 → 辅助表 A → 绘制项**。
 * ★ 负键口径按表分：网格 / 辅助表 A 的负键**不参与**；绘制项**不做符号检查**。
 * ★ 同表内保持输入的相对次序（`Array.prototype.sort` 稳定）。
 */
/** 合并序的 table 秩：**辅助表 A → 网格 → 绘制项**（辅助表 B 不参与命令档）。 */
const MERGE_AUX_A = 0;
const MERGE_MESH = 1;
const MERGE_ITEMS = 2;

export function mergeTableKeys(tables: CommandTableKeys): MergedKey[] {
  const entries: MergedKey[] = [];
  if (tables.auxA !== null) {
    for (const key of tables.auxA) {
      if (key < 0) continue;
      entries.push({ key, table: MERGE_AUX_A });
    }
  }
  for (const key of tables.meshes) {
    if (key < 0) continue;
    entries.push({ key, table: MERGE_MESH });
  }
  for (const key of tables.items) entries.push({ key, table: MERGE_ITEMS });
  entries.sort((a, b) => {
    if (a.key !== b.key) return a.key < b.key ? -1 : 1;
    return a.table - b.table;
  });
  return entries;
}

/** 覆盖区间 `[start, start + count)`：三表合并序里的键（含同键的多个条目） */
function coveredKeys(merged: readonly MergedKey[], range: { start: number; count: number }): number[] {
  const out: number[] = [];
  for (const entry of merged) {
    if (entry.key >= range.start && entry.key < range.start + range.count) out.push(entry.key);
  }
  return out;
}

/**
 * 区间之下：键**小于**本段起点、**未消费**、且**不在另一段区间里**的条目键（三表合并序）。
 * ★ `other === null` ⇒ 单段命令，没有"另一段"可跳过。
 * ★ 门里的「有效」这一位不在键集合里 ⇒ 本层只能施加「未消费」；⛔ 不声称这里判过有效位。
 */
function belowRangeKeys(
  merged: readonly MergedKey[],
  start: number,
  other: { start: number; count: number } | null,
  consumed: ReadonlySet<number>,
): number[] {
  const out: number[] = [];
  for (const entry of merged) {
    if (entry.key >= start) break; // merged 升序 ⇒ 后面的都 ≥ start
    if (consumed.has(entry.key)) continue;
    if (other !== null && entry.key >= other.start && entry.key < other.start + other.count) continue;
    out.push(entry.key);
  }
  return out;
}
