/**
 * emulator-lift · UI 脚本的**作者用类型声明**（A：QuickJS 内核）
 *
 * ⛔ 本文件不参与构建、不进产品、不进 CMake、不进 `pnpm typecheck` 的六份工程。
 * 它只是给写 `scripts/ui/*.ts`（产出单个 `.js`）的作者的编辑器提示，真源 = `src/ui/script.h`
 * 与 `src/ui/script_quickjs.cpp` 的宿主函数。改运行时名字时**同时**改这里（⛔ 不是反过来）。
 *
 * 入口约定：脚本是 UTF-8、单文件、strict、无 `import`；入口必须是
 *
 *     globalThis.render = function render() { … };
 *
 * `scripts/ui/ui.d.ts` 声明宿主全局 `ui` 与 `game`，外加入口 `render` 的**类型**（供 `jsconfig.json`
 * 的 `checkJs` 用；运行时 `render` 由脚本自己赋值，不是宿主注入的值）。
 */

/** `ui.rect({ … })`：参考像素（1280×720 画布）的轴对齐矩形。 */
interface UiRectOptions {
  /** 左上角 x（参考像素） */
  x: number;
  /** 左上角 y（参考像素） */
  y: number;
  /** 宽（参考像素） */
  w: number;
  /** 高（参考像素） */
  h: number;
  /** `#RRGGBB` / `#RRGGBBAA`；缺省 `#000000FF` */
  fill?: string;
  /** 绘制序（同 z 按调用序）；缺省 0 */
  z?: number;
}

/** `ui.text({ … })`：一行 UTF-8 文本（字形来自宿主字体）。 */
interface UiTextOptions {
  x: number;
  y: number;
  /** 文本顶部的 x / y 是参考像素的绝对位置 */
  text: string;
  /** 参考像素字号；缺省 16 */
  size?: number;
  /** `#RRGGBB` / `#RRGGBBAA`；缺省 `#FFFFFFFF` */
  color?: string;
  /** 是否用粗体 face；缺省 false */
  bold?: boolean;
  /** 绘制序；缺省 0 */
  z?: number;
}

interface UiSize {
  w: number;
  h: number;
}

interface UiMouse {
  x: number;
  y: number;
}

/** `game.worst`：最近 3 s 的最坏一帧（墙上 / guest / shim 三段）。 */
interface UiWorst {
  wall_ms: number;
  /** guest 未计时时**没有这个键**（`'guest_ms' in game.worst === false`） */
  guest_ms?: number;
  shim_ms: number;
}

/**
 * `game.local_frames()` 的一项：guest 帧区（40 槽）里**已装载**的一格。
 *
 * ★ 语义（取证锚见宿主仓库 `.tmp/ui-qjs/frames-research.md`，⛔ 不要在这里重新定义）：
 *   - **活动** = 记录 `+0x00`（脚本体基址）非 0 且已提交；⛔ 不是"id 非 0"、也⛔ 不是"0..cur 全算"。
 *   - **top** = `slot === cur`（引擎没有独立的栈顶变量；`cur` 是唯一执行点）。
 *   - **顺序** = top-first：`cur`（若活动）第一，其余活动槽按**槽号降序**（最小版不走 parent 链）。
 */
interface FrameInfo {
  /** 槽号 0..39 */
  slot: number;
  /** 记录 `+0x3C`（统一文件 id）的 **uint32 原值**（高位置 1 也照原样，⛔ 不转成 -1） */
  script_id: number;
  /** 记录 `+0x38`（parent）的 **uint32 原值**：`4294967295` = 根，`4294967285`/`4294967286` = 特殊装载哨兵 */
  parent: number;
  /** 名字；名字表读不到时是降级形态 `id:0x%08X`（见 `name_degraded`） */
  script: string;
  /** `script` 是降级形态（⛔ 不代表 id / 这一项无效） */
  name_degraded: boolean;
  /** 这一格就是当前执行点 `cur`；整表最多一项为真（`cur` 那一格不活动 ⇒ 全 false） */
  top: boolean;
}

/** 每帧只读读数（宿主注入；没有值的项是 `undefined`，⛔ 不是 0）。 */
interface UiFrameReadings {
  /** 帧号（总是有值） */
  readonly frame: number;
  readonly fps: number | undefined;
  readonly mouse: UiMouse | undefined;
  /** 已格式化好的脚本身份载荷（名字或 `id:0x%08X（降级）`） */
  readonly script: string | undefined;
  readonly worst: UiWorst | undefined;
  /**
   * 活动帧列表（**top-first**：`cur` 若活动则第一，其余按槽号降序）。
   *
   * 宿主**按需查询**（不是每帧推表）：只在 `render` 里可调用。读不到 ⇒ `undefined`
   * （⛔ 不编空数组 / 空对象冒充读数）。单槽读不到 ⇒ 那一项不进表（保守跳过，不是猜）。
   * 这是 `game` 上**唯一**的方法（其余是只读 getter）；它是不可写 / 不可枚举 / 不可配置的。
   */
  local_frames(): FrameInfo[] | undefined;
}

/** display list 的构造入口（只有两个）。 */
declare const ui: {
  rect(options: UiRectOptions): void;
  text(options: UiTextOptions): void;
  /** 走宿主 `FontEngine`；宿主没有字体时抛 TypeError。 */
  measure(text: string, size: number): UiSize;
};

/** 只读读数。属性是 getter（无 setter）⇒ strict 下赋值抛 TypeError。 */
declare const game: UiFrameReadings;

/**
 * 入口（脚本必须显式赋给 `globalThis.render`）。宿主**每帧**调一次；调用前 display list 已清空。
 *
 * ★ 只是类型声明：`jsconfig.json` 的 `checkJs` 用它，让 `globalThis.render = function …` 类型通过。
 *   运行时这个全局**不存在**（脚本自己赋；宿主没赋 ⇒ 加载期硬失败）。
 */
declare var render: () => void;
