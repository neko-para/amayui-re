/**
 * `scene-command-exec.ts` —— **命令档的执行计划器**（纯函数；无 Pixi、无 DOM、不写 core）。
 *
 * 分工：规划器（`scene-command-plan.ts`）回答"这一段的**哪些条目**要画"；
 * 本文件回答"这一帧对**哪块画布**、按什么次序做**哪几件事**"：
 * 切目标 → 清画布 → 把条目发进去 → 整幅拷贝 / 合成回命令自己的纹理。
 * 输出的 op 序列就是平台执行层（S2a 之后接 `pixi-host`）的**唯一输入**。
 *
 * ★ **纯函数**：不读时钟、不改输入、不改 `plan` 里的任何数组；⛔ 不 import `pixi.js` / `document` / `node:*`。
 * ★ **两段分工**（S2a-2）：
 *   ① `planCommandExecution` 只产出**结构与次序**（⛔ 不建 RenderTexture、⛔ 不调 `planDraw`、
 *      ⛔ 不写 `consumedKeys`、⛔ 不改 core 状态）；
 *   ② `runCommandOps` 把 op 序列**逐条派发**给一个**注入的** `CommandCanvasHost`（真实实现在
 *      `pixi-host.ts`）—— 本文件仍然不 import Pixi / DOM，所以守卫能用假宿主断言次序。
 *   像素 / 时序仍然是后面的切片（S2b / S2c），所以「执行」这一层只保证**谁先谁后、发到哪**。
 * ★ **产品零回退**：`enabled` 缺省 = `false` ⇒ **一个 op 都不产**（连诊断都没有），
 *   于是 `runCommandOps` 一次宿主调用都没有 ⇒ 渲染步与今天逐字节一致。
 *   只有 `enabled === true` 才按种类闸门产 op。
 *
 * ════════════════ 按种类的闸门（本轮的口径） ════════════════
 *
 * | 种类 | 阶段 → op |
 * |---|---|
 * | 1 影片覆盖 | **只首帧**（`firstFrame`）：画布 A 切目标 → 清 → [区间之下：还要 `deviceReady`] → 覆盖区间 → 切回自己的纹理 → **copy A→own**。非首帧 = 零 op（只标记支），遮罩混合本轮不产 |
 * | 0 纹理覆盖 | 画布 A 切目标 → 清 → [区间之下] → 覆盖区间；画布 B 同上；切回自己的纹理 → **composite（A + B → own）** |
 * | 2 抹除 | 同种类 0（两块画布各一趟）。★"跳过另一段"是**规划器**的事：B 段的 `entries` 里不会有 A 段的条目，本层⛔ 不再筛一遍 |
 * | 3 模糊转场 | 画布 A 切目标 → 清 → 覆盖区间（**结构上没有区间之下**）；切回自己的纹理 → **composite（A → own）** |
 *
 * ★ `below-range` 的 emit **只含本段起点之前的键**：规划器已经这么筛过，本层再施加一次
 *   （`entry.key < 本段起点`），越界的条目**丢掉并记诊断**——因为"起点之后的条目被区间之下画一次、
 *   又被覆盖区间画一次"正是 spec 警告的同帧两次。
 * ★ `deviceReady` 只作用于**种类 1 的区间之下**（切片口径，见 `slice-plan.md` 的 S2a-1）；
 *   缺省 `false`（未建模 ⇒ 不假设就绪）。被门挡住时记 `scene-command-exec-device-not-ready`，
 *   ⛔ 不静默少画。
 * ★ 闸门里的「能力档 < 2」这一半**由规划器体现**（档 ≥ 2 ⇒ 它根本不产 `below-range` 阶段），
 *   所以执行器不再要一份 `tier` 输入 —— ⛔ 不要让"同一档位"有两个真源。
 * ★ **每块画布用前先清**，且**各清一次**：`clear` 与 `target` 成对出现（即使这一段一个条目都没有）。
 *
 * ════════════════ 本轮明确**不产**的 op（类型先占位） ════════════════
 *
 * - `present`：op 闭集合里有这一格，但一帧的最终呈现仍归 `pixi-host.present()`，本层不产。
 * - `mask-blend`（种类 1 每帧尾段）/ `strokes`（种类 2 的抹除笔）/ 进度（种类 0/3）：
 *   都属于 S2b / S2c，需要协议扩快照格（`rect` / `flag` / 插值量）或效果接口 ⇒ 本轮连类型都不编。
 * - `emit` 的目标**只有画布 A / B / own**：命令档⛔ 不画到后缓冲（那会被同帧呈现看见 ⇒ 白闪）。
 */
import {
  KIND_BLUR,
  KIND_MOVIE_OVERLAY,
  KIND_TEXTURE_OVERLAY,
  KIND_WIPE,
} from './scene-command-plan.ts';
import type { WipeStroke } from './wipe-pattern.ts';
import type {
  CommandPlanEntry,
  CommandPlanPhase,
  CommandPlanStage,
  PlannedCommand,
  SceneCommandPlan,
} from './scene-command-plan.ts';

/* ─────────────────────────────── op（闭集合） ─────────────────────────────── */

/**
 * 一个 op 的目标：两块离屏画布，或**命令自己的纹理**。
 * ★ 名字（不是 36 / 37）⇒ 本层与"槽号"解耦；`own` = 记录里的 `textureId`（种类 1 是影片纹理）。
 */
export type ExecTarget = 'canvasA' | 'canvasB' | 'own';

/**
 * 命令档的**执行 op 闭集合**（按 `ops` 里的次序执行）。
 * ★ `commandKey` 是回溯用的：执行层要按它取记录（自己的纹理 / 进度 / 持续时间）。
 * ★ 闭集合的意思：执行层只认这六种；⛔ 不许在这里塞"某个条目的实参"——那是 `planDraw` 的事。
 */
export type ExecOp =
  /** 切当前渲染目标（⛔ 不改 `pixi-host.currentTarget`：那是 `0x20d` 的历史状态） */
  | { kind: 'target'; target: ExecTarget; commandKey: number }
  /** 清空一块画布（用前先清；两块画布各清一次） */
  | { kind: 'clear'; target: ExecTarget; commandKey: number }
  /**
   * 把条目发进目标（平键序原样保序；网格 / 绘制项 / 辅助表 A 由 `table` 分派）。
   * ★ `skipExtraTransform` 是**命令自己的** 6.1 标志（`PlannedCommand.skipExtraTransform`）；
   *   收尾扫描恒 `false`（它没有命令记录）。⛔ 别让执行层自己再判一次。
   */
  | {
    kind: 'emit';
    into: ExecTarget;
    entries: CommandPlanEntry[];
    skipExtraTransform: boolean;
    commandKey: number;
  }
  /** 整幅表面拷贝：种类 1 首帧，**画布 A → 命令自己的纹理**（方向 ⛔ 不可反） */
  | { kind: 'copy'; from: ExecTarget; to: ExecTarget; commandKey: number }
  /** 合成：把一块或两块画布合到命令自己的纹理（种类 0 / 2 = 两块、种类 3 = 一块） */
  | { kind: 'composite'; sources: ExecTarget[]; into: ExecTarget; progress: number; path: 'fallback'; commandKey: number }
  /** 种类 2 的抹除笔：按 `strokes` 顺序逐笔从画布搬子矩形到 own（落点 offset 0 档） */
  | { kind: 'strokes'; into: ExecTarget; strokes: WipeStroke[]; commandKey: number }
  /** 种类 1 的遮罩混合（均匀系数；非均匀未建模） */
  | { kind: 'mask'; from: ExecTarget; into: ExecTarget; rect: { left: number; top: number; right: number; bottom: number }; coeff: number; commandKey: number }
  /** 一帧的最终呈现（本轮**不产**；占位是为了让执行层不必再改闭集合） */
  | { kind: 'present'; target: ExecTarget; commandKey: number };

/* ─────────────────────────────── 输入 ─────────────────────────────── */

/**
 * 一条命令的**执行侧输入**：规划器不保留 `kind` / `range`（它只保留派生出来的阶段），
 * 所以执行器要求调用方把这两样再给一份。★ 按 `commandKey` 对齐，次序无关。
 */
export interface CommandExecCommand {
  /** 命令键（命令表的排序键；与 `plan.commands[].commandKey` 对齐） */
  commandKey: number;
  /** 命令种类（0 / 1 / 2 / 3）——**种类闸门**按它走 */
  kind: number;
  /** 覆盖区间 A（起点 / 计数）——区间之下只画**起点之前**的条目 */
  rangeA: { start: number; count: number };
  /** 覆盖区间 B（种类 0 / 2 的第二段；单段命令给空区间） */
  rangeB: { start: number; count: number };
  /** 种类 1：遮罩矩形（没有 ⇒ 不产 mask op） */
  rect?: { left: number; top: number; right: number; bottom: number };
  /** 种类 1：遮罩标志（0 / 1；两时刻互补语义仍未定） */
  flag?: number;
  /** 种类 1：当前遮罩系数（0 / 255；未给则按 flag 推） */
  mask?: number;
  /** 种类 0 / 3 的**进度**（调用方按盖章后的 timestamp / delay / duration / now 算好；0..1） */
  progress: number;
}

export interface CommandExecInput {
  /** 规划器结果（阶段 + 条目 + 已消费键 + 盖章；本层只读） */
  plan: SceneCommandPlan;
  /** 每命令的 `kind` / `range`（与 `plan.commands` 按 `commandKey` 对齐） */
  commands: readonly CommandExecCommand[];
  /** **总闸**：缺省 / `false` ⇒ 零 op（产品零回退）；只有显式 `true` 才产 */
  enabled?: boolean;
  /** 设备就绪门（只作用于种类 1 的区间之下）；缺省 `false` = 未建模 ⇒ 不假设就绪 */
  deviceReady?: boolean;
}

/* ─────────────────────────────── 输出 ─────────────────────────────── */

/** 实现缺口诊断（**不是**引擎错误 ⇒ 不进错误通道；由调用方转成诊断面板的一行） */
export type CommandExecDiagnosticKind =
  /** 输入里没有这条命令的 `kind` / `range` ⇒ ⛔ 不猜种类、不产 op */
  | 'scene-command-exec-command-unmodeled'
  /** 输入的种类与规划器给出的一致不了 ⇒ ⛔ 不猜哪边对、不产 op */
  | 'scene-command-exec-kind-mismatch'
  /** 规划器给了不认识的种类（本执行器不认识）⇒ ⛔ 不猜它做什么 */
  | 'scene-command-exec-kind-unknown'
  /** 种类 1 的区间之下被设备就绪门挡住（`deviceReady` 缺省 `false`）⇒ 少画一趟，⛔ 不静默 */
  | 'scene-command-exec-device-not-ready'
  /** 种类 1 的遮罩只按均匀系数近似（逐像素遮罩未建模） */
  | 'scene-command-mask-uniform'
  /** 种类 0 / 3 的合成走退路（效果接口未建模）⇒ 只记账，不假装技法 */
  | 'scene-command-effect-unmodeled'
  /** 区间之下的条目里出现"键 ≥ 本段起点"的（规划器不该这么给）⇒ 丢掉，避免同帧两次 */
  | 'scene-command-exec-below-range-out-of-range';

export interface CommandExecDiagnostic {
  kind: CommandExecDiagnosticKind;
  /** 相关命令键（整档级缺口 = `null`） */
  commandKey: number | null;
}

/** 执行计划的结果：**只有结构与次序**，没有一个像素被画 */
export interface CommandExecResult {
  /** 按执行次序排列的 op；`enabled !== true` 时恒为空 */
  ops: ExecOp[];
  /** 实现缺口（按出现次序去重） */
  diagnostics: CommandExecDiagnostic[];
}

/* ─────────────────────────────── 主函数 ─────────────────────────────── */

/**
 * 规划结果 + 每命令的 `kind` / `range` → 本帧命令档的 op 序列。
 *
 * ★ **纯函数**：不读时钟、不改输入（`emit` 的 entries 是拷贝）、不改 core 状态。
 * ★ 总闸在函数第一行：`enabled !== true` ⇒ **零 op、零诊断**（不是"产了但没人执行"）。
 * ★ 中止命令（`abort`）= 阶段为空 ⇒ 零 op；⛔ 不给它补一趟清画布。
 * ★ 一条命令的 op 次序：`target → clear → [emit 区间之下] → emit 覆盖区间 → … → target own → copy / composite`。
 */
export function planCommandExecution(input: CommandExecInput): CommandExecResult {
  // 产品零回退：闸没开 ⇒ 什么都不产（连诊断都不产：这不是"缺口"，是"这一层今天不接"）
  if (input.enabled !== true) return { ops: [], diagnostics: [] };

  const progressByKey = new Map<number, number>();
  for (const command of input.commands) progressByKey.set(command.commandKey, command.progress);
  const diagnostics: CommandExecDiagnostic[] = [];
  const note = (kind: CommandExecDiagnosticKind, commandKey: number | null = null): void => {
    if (diagnostics.some((entry) => entry.kind === kind && entry.commandKey === commandKey)) return;
    diagnostics.push({ kind, commandKey });
  };

  const ops: ExecOp[] = [];
  const deviceReady = input.deviceReady === true;
  /** 输入按命令键索引（乱序无关；对齐不了 ⇒ 记缺口、不猜） */
  const byKey = new Map<number, CommandExecCommand>();
  for (const command of input.commands) byKey.set(command.commandKey, command);

  for (const planned of input.plan.commands) {
    // 中止命令：规划器给了空阶段（清持续时间 + 中止整档发生在任何切目标之前）⇒ 零 op
    if (planned.abort === true) continue;

    const source = byKey.get(planned.commandKey);
    if (source === undefined) {
      note('scene-command-exec-command-unmodeled', planned.commandKey);
      continue;
    }
    if (source.kind !== planned.kind) {
      note('scene-command-exec-kind-mismatch', planned.commandKey);
      continue;
    }

    if (planned.kind === KIND_MOVIE_OVERLAY) {
      planMovieOverlay(ops, planned, source, deviceReady, note);
    } else if (planned.kind === KIND_TEXTURE_OVERLAY || planned.kind === KIND_WIPE) {
      planTwoCanvas(ops, planned, source, note);
    } else if (planned.kind === KIND_BLUR) {
      planSingleCanvas(ops, planned, progressByKey.get(planned.commandKey) ?? 0, note);
    } else {
      note('scene-command-exec-kind-unknown', planned.commandKey);
    }
  }

  return { ops, diagnostics };
}

/* ─────────────────────────────── 按种类的 op 序列 ─────────────────────────────── */

/** 区间条目 → op 的 `entries`（**拷贝**：执行层不许改到规划器的数组） */
function copyEntries(entries: readonly CommandPlanEntry[]): CommandPlanEntry[] {
  return entries.map((entry) => ({ key: entry.key, table: entry.table }));
}

/** 发一趟条目（空的一趟**不发请求**：`emit` 只表示"真有条目要画"） */
function emit(
  ops: ExecOp[],
  commandKey: number,
  into: ExecTarget,
  entries: readonly CommandPlanEntry[],
  skipExtraTransform: boolean,
): void {
  if (entries.length === 0) return;
  ops.push({ kind: 'emit', into, entries: copyEntries(entries), skipExtraTransform, commandKey });
}

/** 按 `stage` + `segment` 取一个阶段（同一条命令里这三元组唯一） */
function phaseOf(
  planned: PlannedCommand,
  stage: CommandPlanStage,
  segment: 'A' | 'B' | 'single',
): CommandPlanPhase | undefined {
  return planned.phases.find((phase) => phase.stage === stage && phase.segment === segment);
}

/**
 * 区间之下的条目：**只留键 < 本段起点**的。
 * ★ 规划器已经筛过；这里再筛一次是因为"起点之后的条目被区间之下画一次、又被覆盖区间画一次"
 *   正是 spec 警告的同帧两次 ⇒ 执行层不允许把这种条目发出去（越界时记诊断，⛔ 不静默）。
 * ★ 三态：没有这一趟 / 没有 `entries`（旧形状）⇒ 空数组；有 ⇒ 保序过滤。
 */
function belowRangeEntriesOf(
  phase: CommandPlanPhase | undefined,
  segmentStart: number,
  commandKey: number,
  note: (kind: CommandExecDiagnosticKind, commandKey?: number | null) => void,
): CommandPlanEntry[] {
  if (phase?.entries === undefined) return [];
  const kept = phase.entries.filter((entry) => entry.key < segmentStart);
  if (kept.length !== phase.entries.length) {
    note('scene-command-exec-below-range-out-of-range', commandKey);
  }
  return kept;
}

/**
 * 一块画布的一趟：切目标 → 清空 → [区间之下] → 覆盖区间。
 * ★ `clear` 与 `target` **成对**出现（这一段一个条目都没有时也清：spec 是"用前先清"）。
 * ★ `below` 为 `undefined` ⇒ 这一趟没有区间之下（能力档 ≥ 2 / 种类 3 / 非首帧）。
 */
function pushCanvasPass(
  ops: ExecOp[],
  commandKey: number,
  canvas: ExecTarget,
  below: readonly CommandPlanEntry[] | undefined,
  covered: readonly CommandPlanEntry[] | undefined,
  skipExtraTransform: boolean,
): void {
  ops.push({ kind: 'target', target: canvas, commandKey });
  ops.push({ kind: 'clear', target: canvas, commandKey });
  if (below !== undefined) emit(ops, commandKey, canvas, below, skipExtraTransform);
  if (covered !== undefined) emit(ops, commandKey, canvas, covered, skipExtraTransform);
}

/**
 * 种类 1（影片覆盖）：**只首帧**发请求。
 * 首帧：画布 A 一趟（区间之下还要 `deviceReady`）→ 切回 own → **copy A→own**；
 * 之后的帧：零 op（只标记支），遮罩混合本轮不产。
 * ★ 方向是**画布 → 影片纹理**（spec 明禁"用某编号画影片纹理"的写法，⛔ 别把 `from` / `to` 写反）。
 */
function planMovieOverlay(
  ops: ExecOp[],
  planned: PlannedCommand,
  source: CommandExecCommand,
  deviceReady: boolean,
  note: (kind: CommandExecDiagnosticKind, commandKey?: number | null) => void,
): void {
  const firstFrame = planned.firstFrame === true;

  const belowPhase = phaseOf(planned, 'below-range', 'single');
  let below: CommandPlanEntry[] | undefined;
  if (belowPhase !== undefined) {
    if (deviceReady) {
      below = belowRangeEntriesOf(belowPhase, source.rangeA.start, planned.commandKey, note);
    } else {
      note('scene-command-exec-device-not-ready', planned.commandKey);
    }
  }
  const covered = phaseOf(planned, 'covered', 'single');
  if (firstFrame) {
    pushCanvasPass(ops, planned.commandKey, 'canvasA', below, covered?.entries, planned.skipExtraTransform);
    ops.push({ kind: 'target', target: 'own', commandKey: planned.commandKey });
    ops.push({ kind: 'copy', from: 'canvasA', to: 'own', commandKey: planned.commandKey });
  }
  if (source.rect !== undefined) {
    note('scene-command-mask-uniform', planned.commandKey);
    const mask = source.mask ?? (source.flag === 1 ? 0 : 255);
    const coeff = mask === 255 ? 254 / 255 : 0;
    ops.push({ kind: 'target', target: 'own', commandKey: planned.commandKey });
    ops.push({ kind: 'mask', from: 'canvasA', into: 'own', rect: { ...source.rect }, coeff, commandKey: planned.commandKey });
  }
}

/**
 * 种类 0 / 2（纹理覆盖 / 抹除）：**两块画布 × 各自那一段** + 合成。
 * ★ 区间 A 只喂画布 A、区间 B 只喂画布 B（规划器已经保证"跳过另一段里的条目"，本层不重算）。
 * ★ 合成在两趟之后、且是最后一个 op（两块画布 → 命令自己的纹理）。
 */
function planTwoCanvas(
  ops: ExecOp[],
  planned: PlannedCommand,
  source: CommandExecCommand,
  note: (kind: CommandExecDiagnosticKind, commandKey?: number | null) => void,
): void {
  const belowA = phaseOf(planned, 'below-range', 'A');
  const belowB = phaseOf(planned, 'below-range', 'B');
  pushCanvasPass(
    ops,
    planned.commandKey,
    'canvasA',
    belowA === undefined ? undefined : belowRangeEntriesOf(belowA, source.rangeA.start, planned.commandKey, note),
    phaseOf(planned, 'covered', 'A')?.entries,
    planned.skipExtraTransform,
  );
  pushCanvasPass(
    ops,
    planned.commandKey,
    'canvasB',
    belowB === undefined ? undefined : belowRangeEntriesOf(belowB, source.rangeB.start, planned.commandKey, note),
    phaseOf(planned, 'covered', 'B')?.entries,
    planned.skipExtraTransform,
  );
  ops.push({ kind: 'target', target: 'own', commandKey: planned.commandKey });
  if (planned.kind === KIND_TEXTURE_OVERLAY) note('scene-command-effect-unmodeled', planned.commandKey);
  const strokes = planned.phases.find((phase) => phase.stage === 'composite')?.strokes ?? [];
  if (planned.kind === KIND_WIPE && strokes.length > 0) {
    ops.push({ kind: 'strokes', into: 'own', strokes: [...strokes], commandKey: planned.commandKey });
    return;
  }
  ops.push({
    kind: 'composite',
    sources: ['canvasA', 'canvasB'],
    into: 'own',
    progress: source.progress,
    path: 'fallback',
    commandKey: planned.commandKey,
  });
}

/**
 * 种类 3（模糊转场）：**单段 + 合成**，每帧都跑。
 * ★ 结构上**没有区间之下**（⛔ 别给它补一趟：那会把键 < 起点的条目多画一次）。
 * ★ 效果技法不在本轮：composite 只是"把画布 A 合到自己的纹理"这一步的结构。
 */
function planSingleCanvas(
  ops: ExecOp[],
  planned: PlannedCommand,
  progress: number,
  note: (kind: CommandExecDiagnosticKind, commandKey?: number | null) => void,
): void {
  note('scene-command-effect-unmodeled', planned.commandKey);
  const covered = phaseOf(planned, 'covered', 'single');
  pushCanvasPass(ops, planned.commandKey, 'canvasA', undefined, covered?.entries, planned.skipExtraTransform);
  ops.push({ kind: 'target', target: 'own', commandKey: planned.commandKey });
  ops.push({
    kind: 'composite',
    sources: ['canvasA'],
    into: 'own',
    progress,
    path: 'fallback',
    commandKey: planned.commandKey,
  });
}

/* ─────────────────────────────── 执行（注入宿主） ─────────────────────────────── */

/**
 * 命令档的**执行宿主**：平台侧真正要做的那五件事（与 `ExecOp` 一一对应的闭集合）。
 *
 * ★ 为什么是接口而不是直接写进 `pixi-host`：`pixi-host.ts` 一 import 就拉进 `pixi.js`
 *   （要 WebGL / DOM）⇒ pure 守卫根本 import 不了它。把"执行"抽成"对注入宿主的派发"之后，
 *   守卫可以给一个**假宿主**并断言 op 的次序 / 目标 / 方向（⛔ 不测像素）。
 * ★ 真实实现在 `pixi-host.ts` 的 `PixiRenderHost`（画布 36/37 → RenderTexture）。
 * ★ `commandKey` 一路带着：执行层要按它取"命令自己的纹理"（`own` 的解析表）。
 */
export interface CommandCanvasHost {
  /** 切当前渲染目标（⛔ 不改平台侧的 `currentTarget`：那是 `0x20d` 的历史状态） */
  selectTarget(target: ExecTarget, commandKey: number): void;
  /** 清空一块画布（涂掉上一帧的残影；只对画布 36/37 有意义） */
  clearCanvas(target: ExecTarget, commandKey: number): void;
  /** 把条目发进目标（`skipExtraTransform` = 该命令的 6.1 标志） */
  emitEntries(
    target: ExecTarget,
    entries: readonly CommandPlanEntry[],
    skipExtraTransform: boolean,
    commandKey: number,
  ): void;
  /** 整幅表面拷贝（方向 = `from` → `to`，⛔ 不可反） */
  copySurface(from: ExecTarget, to: ExecTarget, commandKey: number): void;
  /** 合成（`sources` 按**给定次序**画：先 A 后 B，进度恒 1 / 不透明 —— 本轮只做结构） */
  composite(sources: readonly ExecTarget[], into: ExecTarget, progress: number, commandKey: number): void;
  strokes(strokes: readonly WipeStroke[], into: ExecTarget, commandKey: number): void;
  mask(from: ExecTarget, into: ExecTarget, rect: { left: number; top: number; right: number; bottom: number }, coeff: number, commandKey: number): void;
}

/** 一次命令档执行的计数（写给 `SceneBuildStats`） */
export interface CommandOpRunResult {
  /** 真正派发到宿主的 op 数（`commandPassOps`；⛔ 不含 `present` 这种本层不执行的占位） */
  ops: number;
  /** 其中发进命令画布（`canvasA` / `canvasB`）的 `emit` 次数（`commandPassCanvasDraws`） */
  canvasDraws: number;
  /** 发进命令画布的条目总数（诊断用：一次 emit 可能带好几条） */
  canvasEntries: number;
  /** 本层**不执行**的 op（`present`：一帧的最终呈现仍归 `pixi-host.present()`） */
  skipped: number;
}

/**
 * 按 `ops` 的次序逐条派发给宿主。
 *
 * ★ **纯**：不建 RT、不碰 Pixi、不读时钟 —— 宿主做什么由注入决定，所以守卫能看见**次序本身**。
 * ★ `present` op **不派发**（它属于 `pixi-host.present()` 的收尾；本层产不出它，出现即记 skipped）。
 * ★ ⛔ 不要在这里"顺手"实现拷贝 / 合成：那是 `pixi-host` 的平台动作（像素级，守卫测不到）。
 */
export function runCommandOps(ops: readonly ExecOp[], host: CommandCanvasHost): CommandOpRunResult {
  const result: CommandOpRunResult = { ops: 0, canvasDraws: 0, canvasEntries: 0, skipped: 0 };
  for (const op of ops) {
    switch (op.kind) {
      case 'target':
        host.selectTarget(op.target, op.commandKey);
        break;
      case 'clear':
        host.clearCanvas(op.target, op.commandKey);
        break;
      case 'emit':
        host.emitEntries(op.into, op.entries, op.skipExtraTransform, op.commandKey);
        if (op.into === 'canvasA' || op.into === 'canvasB') {
          result.canvasDraws += 1;
          result.canvasEntries += op.entries.length;
        }
        break;
      case 'copy':
        host.copySurface(op.from, op.to, op.commandKey);
        break;
      case 'composite':
        host.composite(op.sources, op.into, op.progress, op.commandKey);
        break;
      case 'strokes':
        host.strokes(op.strokes, op.into, op.commandKey);
        break;
      case 'mask':
        host.mask(op.from, op.into, op.rect, op.coeff, op.commandKey);
        break;
      case 'present':
        // ⛔ 不在这里呈现：最终呈现是 present() 的收尾扫描，不是命令档的 op
        result.skipped += 1;
        continue;
    }
    result.ops += 1;
  }
  return result;
}
