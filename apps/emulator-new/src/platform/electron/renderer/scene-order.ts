/**
 * 场景绘制顺序（**引擎行为；`CORE-SCENE-DRAW-ORDER` 已写无命令记录时的部分**）。
 *
 * ★ **绘制顺序是引擎行为，属于 spec 的范围**：引擎在渲染步进里遍历场景、决定先画谁；
 *   "最终像素 / D3D 调用格式"才是平台边界。⛔ 不能因为 `0x20c` 把步进请求交给渲染子系统，
 *   就说"谁先谁后由平台自由选择"。
 * ★ 本版实现的档：**没有命令记录时**的**收尾扫描**——按键严格升序、同键**网格先于绘制项**。
 * ★ **负键口径按表分**（`CORE-SCENE-DRAW-ORDER` 的纠错点）：收尾扫描用 **0** 当"该表已没有可出条目"
 *   的哨兵 ⇒ **网格 / 辅助表 B / 辅助表 A** 的负键条目按哨兵处理（不参与）；**绘制项是最后一个候选，
 *   它的键不做符号检查** ⇒ 负键绘制项**仍会被发出**（按 key 升序，因此排在最前）。
 *   ⛔ 不许把「负键不参与」写成不区分表的通则。
 * ★ **命令档（有命令记录时的第一档）现在是"规划 + 可按种类开闸执行"**：`scene-command-plan.ts`
 *   是纯规划器，`scene-command-exec.ts` 产出 op 序列并由 `pixi-host.present()` 在**收尾扫描之前**
 *   派发（⛔ 缺省关闸 ⇒ 零 op ⇒ 产品画面与"只规划"逐字节一致）。本文件负责三件事：
 *   投影（`commandPlanInput` / `commandExecCommandsOf`）、种类闸门（`gateCommandPlanByKinds`）、
 *   诊断（`scene-command-pass-planned` / `scene-command-pass-executed` / `…-render-unmodeled`）。
 *   ⛔ 仍然不许把"规划出来了"说成"已经画对了"：copy / composite 只有结构，像素与进度未建模。
 *
 * ★ 观察到：TITLE 用 key **0** 的全屏黑色网格当背景、key 10/100/… 的绘制项当前景、
 *   key **200000** 的透明网格当覆盖层；LOGO 用 key 200002/200003 的网格与 key 200000 的影片项。
 *   按 key 升序既让 TITLE 的黑色背景落到最下面，也保持 LOGO 的影片覆盖层在最后。
 * ★ 相同 key 时**网格先于绘制项**（网格先发出，绘制项压在上面）；同类内部保持表内顺序。
 */
import type { DrawItemSnapshot, MeshSnapshot, SceneCommandSnapshot, SceneSnapshot } from '../protocol.ts';
import { planSceneCommandPass } from './scene-command-plan.ts';
import type { CommandPlanInput, PlannedCommand, SceneCommandPlan } from './scene-command-plan.ts';
import type { CommandExecCommand } from './scene-command-exec.ts';
import type { WipeRect } from './wipe-pattern.ts';

export type SceneDrawable =
  | { kind: 'item'; key: number; item: DrawItemSnapshot }
  | { kind: 'mesh'; key: number; mesh: MeshSnapshot };

/** 把绘制项与网格合成一条按 key 升序的绘制序列（同 key 稳定） */
export function orderedDrawables(scene: SceneSnapshot): SceneDrawable[] {
  const entries: { drawable: SceneDrawable; seq: number }[] = [];
  // ★ 绘制项**不做符号检查**（它是最后候选）⇒ 负键绘制项照常进序列，按 key 升序排在最前。
  scene.drawItems.forEach((item, index) => {
    entries.push({ drawable: { kind: 'item', key: item.key, item }, seq: index });
  });
  const itemCount = scene.drawItems.length;
  // ★ 网格（以及快照里没有的辅助表 A / B）的负键按 0 哨兵处理 ⇒ 不参与选择。
  scene.meshes.forEach((mesh, index) => {
    if (mesh.key < 0) return;
    entries.push({ drawable: { kind: 'mesh', key: mesh.key, mesh }, seq: itemCount + index });
  });
  entries.sort((a, b) => {
    if (a.drawable.key !== b.drawable.key) return a.drawable.key < b.drawable.key ? -1 : 1;
    // 同键：网格先发出（绘制项后发出，压在上面）；同类保持表内顺序。
    if (a.drawable.kind !== b.drawable.kind) return a.drawable.kind === 'mesh' ? -1 : 1;
    return a.seq - b.seq;
  });
  return entries.map((entry) => entry.drawable);
}

/* ──────────────────────────── 快照的剥离（释放纹理后用） ──────────────────────────── */

/**
 * 从一份场景快照里剥掉某个**纹理 id** 的全部引用（`0x1fa` 释放纹理之后用）。
 *
 * ★ 只删 `textures` 与 `drawItems`：网格**不引用纹理**（只有顶点色）⇒ 一律保留。
 *   所以剥完之后的快照仍会画出"那一页的底色网格"—— 这在**帧边界**呈现是对的
 *   （下一帧本来就要重新扫描），在**中途**呈现就是错的：引擎不会在 `0x1fa` 那一刻呈现。
 * ⇒ **本函数是纯函数，它不决定什么时候呈现**；"不中途呈现"是 `pixi-host.present` 的纪律
 *   （见 `releaseTexture` 的注释）。守卫用例把"网格保留"这条钉住，防止有人以为剥完就干净了。
 *
 * ★ 为什么不在核心侧删绘制项：`0x1fa` **不删**绘制项（`60-ops-texture.md`），那是 `0x1f7` 的事；
 *   这里剥的是**渲染进程手里那份已过期的快照**（核心已经前进了，快照还没换）。
 */
export function scrubSceneTexture(scene: SceneSnapshot, textureId: number): SceneSnapshot {
  return {
    ...scene,
    textures: scene.textures.filter((texture) => texture.id !== textureId),
    drawItems: scene.drawItems.filter((item) => item.textureId !== textureId),
  };
}

/** 这份快照里还有没有引用该纹理 id 的绘制项（诊断用：影片覆盖层 / 释放后自检） */
export function snapshotReferencesTexture(scene: SceneSnapshot, textureId: number): boolean {
  return scene.textures.some((texture) => texture.id === textureId)
    || scene.drawItems.some((item) => item.textureId === textureId);
}

/* ──────────────────────────── 命令档的规划（S0 / S1：只规划，不画） ──────────────────────────── */

/**
 * 诊断 kind：命令档**已规划**（命令数 / 阶段数 / 覆盖键数）。
 * ★ 它替代了旧的 `scene-command-order-unmodeled`：命令档的结构现在有了规划器，不再是整块空白；
 *   但"规划出来了"⛔ 不等于"执行了"—— 另一条诊断专门记账这一点。
 */
export const SCENE_COMMAND_PASS_PLANNED = 'scene-command-pass-planned';

/** 诊断 kind：命令档**仍未建模的部分**（copy / composite 只有结构；像素 / 进度 / 遮罩仍是残差） */
export const SCENE_COMMAND_PASS_RENDER_UNMODELED = 'scene-command-pass-render-unmodeled';

/**
 * 诊断 kind：命令档**已执行**（本帧真的按 op 序列派发过；`commandExecKinds` 开闸时才可能出现）。
 * ★ 与 `SCENE_COMMAND_PASS_PLANNED` 的分工：那条说"算了什么"，这条说"真的发了几笔"。
 * ★ 缺省关闸 ⇒ 这条**不会出现**（零 op ⇒ 没有可报的执行）。
 */
export const SCENE_COMMAND_PASS_EXECUTED = 'scene-command-pass-executed';

/**
 * 把场景快照投影成规划器的输入（**唯一**一份投影；⛔ 别在调用方各自拼）。
 *
 * ★ **辅助表 A 恒 `null`**：快照里根本没有这张表（`SceneState.auxTables` 没有进协议）⇒
 *   `null` = 未建模。⛔ 不许写成 `[]`（空表 != 没有这张表：前者会让三表合并序在同一键上静默错位）。
 * ★ 三张表只给**键**（规划器决定"哪几个键进哪一趟"；条目有效位 / 纹理 / 实参仍由执行层判）。
 * ★ 例外是**种类 2 的抹除矩形**：它不是"发给条目的请求"，而是合成阶段的几何 ⇒ 除键之外还要把
 *   绘制项的**四条边**投影过去（`itemRects`）。四条边本来就由 `session.ts` 的 `snapshot()` 从
 *   `machine.scene.drawItems` 投进快照 ⇒ 这里只是换一个形状，⛔ 不另算一份。
 */
export function commandPlanInput(scene: SceneSnapshot): CommandPlanInput {
  return {
    commands: scene.commands,
    now: scene.clock,
    selfKeyWindow: { start: scene.selfRecord.keyStart, count: scene.selfRecord.keyCount },
    tables: {
      meshes: scene.meshes.map((mesh) => mesh.key),
      auxA: null,
      items: scene.drawItems.map((item) => item.key),
      itemRects: itemRectsOf(scene),
    },
    shaderTier: scene.shaderTier,
    animating: scene.animating,
    forcedComplete: scene.forcedComplete,
  };
}

/**
 * 绘制项 → 抹除矩形（种类 2 的 `tables.itemRects`）。
 *
 * ★ **缺项不给**：四条边里有一条不是有限数 ⇒ 这一项**不进**表（⇒ 种类 2 记
 *   `scene-command-wipe-rect-unmodeled` 并不产笔），⛔ 不拿 0 顶缺。
 *   本实现的 `session.ts` 快照总给四个数，这条判据是给"半份快照"（诊断 / 测试夹具）用的。
 */
function itemRectsOf(scene: SceneSnapshot): ReadonlyMap<number, WipeRect> {
  const rects = new Map<number, WipeRect>();
  for (const item of scene.drawItems) {
    const { left, top, right, bottom } = item;
    if (!Number.isFinite(left) || !Number.isFinite(top) || !Number.isFinite(right) || !Number.isFinite(bottom)) continue;
    rects.set(item.key, { left, top, right, bottom });
  }
  return rects;
}

/** 场景快照 → 本帧命令档计划（纯函数；⛔ 不改快照、不碰 Pixi） */
export function planCommandPassForScene(scene: SceneSnapshot): SceneCommandPlan {
  return planSceneCommandPass(commandPlanInput(scene));
}

/* ──────────────────────────── 命令档的执行侧投影 / 种类闸门（S2a-2） ──────────────────────────── */

/**
 * 命令快照 → 执行计划器的 `kind` / `range` 输入（**唯一**一份投影；⛔ 别在调用方各自拼）。
 *
 * ★ 规划器只保留派生出来的阶段（不保留 `kind` / `range`）⇒ 执行器要求再给一份，按 `commandKey` 对齐。
 * ★ 只投影这三样：执行器**不猜**别的格（进度 / 持续时间属 S2b 的 `advanceCommands`）。
 * ★ ⛔ 不许在这里补默认值：`kind` 不认识的命令由执行器记 `scene-command-exec-kind-unknown`。
 */
export function commandExecCommandsOf(scene: SceneSnapshot): CommandExecCommand[] {
  return scene.commands.map((command) => ({
    commandKey: command.commandKey,
    kind: command.kind,
    rangeA: { start: command.rangeA.start, count: command.rangeA.count },
    rangeB: { start: command.rangeB.start, count: command.rangeB.count },
    progress: commandProgress(command, scene.clock, scene.forcedComplete === 1),
    rect: command.rect,
    flag: command.flag,
    mask: command.mask,
  }));
}

/**
 * 种类 0 / 3 的进度。
 * ★ 两者的边界口径**不同**：
 *   - 种类 0：未开始（now <= ts+delay）⇒ 0；到期 / duration<=0 ⇒ 1；否则线性。
 *   - 种类 3：**不夹取**——已过为负时按同一公式外推（会得到起值之外的值）；只有到期 ⇒ 1。
 */
function commandProgress(command: SceneCommandSnapshot, now: number, forcedComplete: boolean): number {
  if (forcedComplete) return 1;
  const start = command.timestamp + command.delay;
  if (command.kind === 3) {
    if (command.timestamp === 0) return 0;
    if (command.duration <= 0 || now >= start + command.duration) return 1;
    return (now - start) / command.duration;
  }
  if (command.timestamp === 0 || now <= start) return 0;
  if (command.duration <= 0 || now >= start + command.duration) return 1;
  return (now - start) / command.duration;
}

/**
 * **按种类开闸**（`PixiRenderHostOptions.commandExecKinds` 的实现）：不在列表里的命令**不产 op**。
 *
 * ★ 闸门必须在**执行计划之前**：先过滤 `plan.commands`，再让执行器产 op —— 这样"未启用的种类"
 *   连 op / 诊断都不会出现（⛔ 不是"产了不执行"，那会让画布与收尾扫描各画一次 ⇒ 静默丢画 / 重影）。
 * ★ 空列表 ⇒ `commands` 清空（产品零回退）；`consumedKeys` / `stamps` 原样保留 —— 本层不动它们
 *   （consumed 跳过与 core 盖章是 S2a-3 的写回）。
 * ★ 纯函数：`plan` 只读，返回新对象。
 */
export function gateCommandPlanByKinds(
  plan: SceneCommandPlan,
  kinds: readonly number[],
): SceneCommandPlan {
  if (kinds.length === 0) return { ...plan, commands: [] };
  const enabled = new Set(kinds);
  return { ...plan, commands: plan.commands.filter((command) => enabled.has(command.kind)) };
}

/**
 * 命令档的诊断记录器（**每种只报一次**）。
 *
 * ★ 它**调用规划器**（纯函数）来算诊断内容，⛔ 不执行命令档：执行点在 `pixi-host.present()`
 *   （那里才知道 `commandExecKinds` 开没开闸），执行统计由 `pixi-host` 报
 *   `scene-command-pass-executed`。这里只报"规划了什么 + 哪些部分仍未建模"。
 * ★ 收尾扫描（`orderedDrawables`）仍是**唯一**的"画"来源（S2a-2 不接 consumed 跳过）。
 */
export class SceneOrderNotes {
  private readonly reported = new Set<string>();

  /** 本帧要上报的诊断文本（没有新诊断 ⇒ 空数组） */
  next(scene: SceneSnapshot): string[] {
    const plan = planCommandPassForScene(scene);
    // 空命令表 ⇒ 命令档无动作 ⇒ 连"规划了什么"都没有可报的
    if (plan.commands.length === 0) return [];
    const out: string[] = [];
    if (!this.reported.has(SCENE_COMMAND_PASS_PLANNED)) {
      this.reported.add(SCENE_COMMAND_PASS_PLANNED);
      const phases = plan.commands.reduce((sum, command) => sum + command.phases.length, 0);
      const gaps = [...new Set(plan.diagnostics.map((entry) => entry.kind))];
      out.push(
        `${SCENE_COMMAND_PASS_PLANNED}：命令 ${plan.commands.length} 条 / 阶段 ${phases} 段 / 覆盖键 ${plan.consumedKeys.length} 个`
          + `${gaps.length > 0 ? ` · 缺口 ${gaps.join('、')}` : ''}`,
      );
    }
    if (!this.reported.has(SCENE_COMMAND_PASS_RENDER_UNMODELED)) {
      this.reported.add(SCENE_COMMAND_PASS_RENDER_UNMODELED);
      out.push(
        `${SCENE_COMMAND_PASS_RENDER_UNMODELED}：命令档**可按种类开闸执行**（缺省关闸 ⇒ 本帧零 op）`
          + '；copy / composite 只有结构（进度恒 1 / 不透明），遮罩 · 抹除笔 · 效果技法仍未建模',
      );
    }
    return out;
  }
}

/**
 * 手动开闸用：URL 查询串 `?commandExecKinds=0,1,2` → 种类列表。
 * ★ 只接受 0..3 的十进制整数；未知项丢弃（⛔ 不猜、不把整串当数字）；
 *   空 / 缺省 ⇒ []（产品零回退）。
 */
export function parseCommandExecKinds(raw: string | null): number[] {
  if (raw === null || raw.trim() === '') return [];
  const out: number[] = [];
  for (const part of raw.split(',')) {
    const value = Number(part.trim());
    if (!Number.isInteger(value) || value < 0 || value > 3) continue;
    if (!out.includes(value)) out.push(value);
  }
  return out;
}
