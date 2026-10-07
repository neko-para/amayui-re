/**
 * apps/emulator/src/host/clock.ts —— **时钟抽象**（★ 核心层：零 Node 依赖）
 *
 * ## 为什么时钟必须注入（而不是 `Date.now()`）
 * 引擎里"等几秒"是**常态**：版权页的淡入淡出窗是 `delay 4500 + dur 500` 毫秒，
 * 主循环在那段时间里**不派发脚本指令**（`wait` 的 `0x400` 等待门）。
 * headless 跑一次 LOGO 若要真等 5 秒，回归就无从谈起；而"干脆别等"又会把
 * **门的语义整个删掉**（那是把观测面做没了，不是加速）。
 * ⇒ 正解是**虚拟时钟**：时间由宿主注入，headless 前端每帧直接 `advance(1000/60)`，
 *   于是"5 秒的窗"在几毫秒的墙上时间里**原样跑完**，而门、窗、完成判据**一条不少**。
 *
 * ## ★ 两个坑（都是旧仓实测）
 * 1. **不许用 `0` 当"还没起步"的哨兵**：虚拟时钟的**第一帧就是 0**。
 *    "没起步"必须用 `null` 表示（那是另一个状态，不是某个时刻）。
 * 2. **时钟只回答"现在几点"**，不回答"该不该跑下一帧" —— 后者是帧步长与主循环策略，
 *    属于环境（`Environment.frameMs`）。把两者混起来会让"帧边界"随调用点变化。
 */

/**
 * 一个时刻源（毫秒）。单调不减由实现保证 —— 主循环的"窗是否跑完"依赖它。
 *
 * ★ `advance` 是**可选的**，而且它的缺失有确切含义：
 * * **虚拟时钟**（headless）实现它：只有驱动说"前进"时时间才动 ⇒ 5 秒的窗在几毫秒的墙上时间里跑完；
 * * **真实时钟**（Electron 窗口）**不实现**它：时间自己会走。
 * ⇒ 驱动（`vm/machine.ts` 的 `tick()`）写成"有就推、没有就算"。
 * ⚠ 代价（必须知道）：真实时钟下 `tick()` 不推进时间 ⇒ 等待门会**忙等**，
 *   所以那种前端要在自己的帧循环里让出（等下一帧/等垂直同步），不能裸转。
 */
export interface Clock {
  nowMs(): number;
  /** 推进时间（**虚拟时钟专用**）；返回推进后的时刻 */
  advance?(ms: number): number;
}

/**
 * 一份**虚拟**时钟：时间只在宿主说"前进"时才前进。
 *
 * 刻意**没有**"每帧自动前进"的逻辑：前进多少是**帧步长**（环境的一部分），
 * 而"什么时候前进"是主循环的事。时钟只负责记账 —— 于是"同一脚本 + 同一初始时刻 + 同一帧步长
 * ⇒ 逐字节相同的运行"成立。
 */
export class VirtualClock implements Clock {
  /** 当前虚拟时刻（毫秒）。从 0 起 —— ★ 0 是**合法时刻**，不是"未起步"哨兵 */
  atMs = 0;

  constructor(startMs = 0) {
    if (!Number.isFinite(startMs) || startMs < 0) throw new Error(`虚拟时钟的起点必须是非负有限数：${startMs}`);
    this.atMs = startMs;
  }

  nowMs(): number {
    return this.atMs;
  }

  /** 前进（**负数即抛**：时间不倒流，否则"窗是否跑完"会永久翻转） */
  advance(ms: number): number {
    if (!Number.isFinite(ms) || ms < 0) throw new Error(`时钟只能前进：收到 ${ms}`);
    this.atMs += ms;
    return this.atMs;
  }

  /** 直接设一个时刻（**只允许向前**；恢复快照时用来对齐） */
  setTo(ms: number): void {
    if (!Number.isFinite(ms) || ms < this.atMs) throw new Error(`时钟只能向前设置：${ms} < ${this.atMs}`);
    this.atMs = ms;
  }
}
