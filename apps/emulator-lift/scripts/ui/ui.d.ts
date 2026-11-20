/**
 * emulator-lift · UI 脚本的**作者用类型声明**（A：QuickJS 内核）
 *
 * ⛔ 本文件不参与构建、不进产品、不进 CMake、不进 `pnpm typecheck` 的六份工程。
 * 它只是给写 `scripts/ui/*.ts`（产出单个 `.js`）的作者的编辑器提示，真源 = `src/ui/script.h`
 * 与 `src/ui/script_quickjs.cpp` 的宿主函数。改运行时名字时**同时**改这里（⛔ 不是反过来）。
 *
 * 入口约定：脚本是 UTF-8、strict；入口必须是
 *
 *     globalThis.render = function render() { … };
 *
 * 两种脚本形状（宿主自动识别，旧 `--ui` 的 classic 语义逐字不变）：
 * - **classic**：顶层 `function render(){}` 那种老写法；⛔ 没有 `import`。
 * - **module**：`import { Button } from './kit.js';` —— 相对路径、只能在本脚本所在目录的子树里；
 *   bare specifier / 绝对路径 / `data:` / `..` 逃逸一律加载期拒绝。module 里**仍要显式**写
 *   `globalThis.render = …`。
 *
 * `scripts/ui/ui.d.ts` 声明宿主全局 `ui` / `game` / `host`，外加入口 `render` 的**类型**
 * （供 `jsconfig.json` 的 `checkJs` 用；运行时 `render` 由脚本自己赋值，不是宿主注入的值）。
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
  /**
   * 命中键（切片 2 后半）：**非空** ⇒ 这个矩形是一个命中区，命中区 = 它的 **resolved rect**
   * （缩放 / 居中 / DPR 之后的最终窗口像素）。空 / 缺省 = 不可命中（纯背景）。
   *
   * ★ 命中顺序 = 上一帧**成功 present** 的绘制序**逆序**（后画的先命中）；没有成功帧 ⇒ 不命中。
   * ★ 这里只是原语：按钮的外观 / 布局 / 状态在 `./kit.js` 的 `Button`。
   */
  key?: string;
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
  /**
   * 三键电平。★ C2b-2：窗口装配现在有**生产读数源**（`WindowManager::mouseButtons()`，
   * 与 guest 输入同一张 `InputDevice` 电平表）⇒ `--overlay` / `--ui` 脚本也会拿到这个键。
   * 没有源（headless / 没有 WindowManager）⇒ **缺键**，⛔ 不是 `false`（`mouse.buttons?.left ?? false`）。
   */
  readonly buttons?: {
    readonly left: boolean;
    readonly right: boolean;
    readonly middle: boolean;
  };
}

/**
 * `game.click` 的载荷：**一次性**对象（宿主每帧最多交来一次；`click_ok` 为真的那一帧才有值，
 * 下一次 render 就是 `undefined`）。`x` / `y` = **抬起点**（窗口像素，与命中区同一空间）。
 */
interface UiClick {
  readonly key: string;
  readonly x: number;
  readonly y: number;
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
  /**
   * 脚本内位置（label 坐标 = `0x3C + (pc − base)`；与 `await position` 同一坐标系、同一公式）。
   *
   * ★ **只有 `top === true` 的那一项有**（判据 = `slot === cur`，与 `top` 同一个布尔）；
   *   非 top 帧的 `+0x04` 是"该脚本被挂起时保存的续延 PC"，**不是**执行点 ⇒ 这一版直接
   *   不暴露（`undefined`，用 `'pos' in f` 判）。读不到时同样没有这个键（⛔ 不编 0）。
   * ★ 采样时刻 = Present（不是块边界）：对 `pos >= X` 这类单调条件没有区别；窄区间
   *   `[from,to)` 可能"两次 Present 之间整个穿过"而漏掉（本版不承诺边沿）。
   */
  pos?: number;
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
  /**
   * 一次性点击（切片 2 后半）：**这一帧**有一次"在某个 key 上按下并在同一 rect 内抬起"才有值。
   * 消费点 = 那次 render；下一次 render 一定是 `undefined`（⛔ 不要缓存后跨帧用）。
   */
  readonly click: UiClick | undefined;
  /**
   * 指针当前在哪个 key 的命中区上（只读 key；空白处 ⇒ `undefined`）。
   * ⛔ 不是"鼠标坐标"（那是 `game.mouse`，guest 客户区像素）。
   */
  readonly hover: string | undefined;
  /** 当前**被捕获**的 key（左键在这个 key 上按下、还没抬起 ⇒ 有值；否则 `undefined`）。 */
  readonly active: string | undefined;
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

/** `host.profileBegin(label)` 的结果（所有键恒存在；`error` 空串 = 没有错）。 */
interface UiProfileBeginResult {
  /** true = 窗口已开；false = 没开（多半是"已有一段开着"，`error` 给原因） */
  readonly ok: boolean;
  /** 人可读的原因（UTF-8；没有错时是空串） */
  readonly error: string;
}

/**
 * `host.profileEnd()` 的结果（所有键恒存在）。
 *
 * ★ **结束 ≠ 复制成功**：`ok:true` + `error` 非空 = "窗口关了、`text` 在，但没进剪贴板"
 *   （宿主内部写系统剪贴板；`text` **不回滚** —— 需求单明文"INVALID 仍复制"）。
 *   `ok:false` ⇒ 没有开着的窗口（`text` 为空串）。
 */
interface UiProfileEndResult {
  readonly ok: boolean;
  /** 结论文本（`formatWindowReportText` 的全文）；`ok:false` 时是空串 */
  readonly text: string;
  readonly error: string;
}

/**
 * 宿主能力（**方法表**，像 `ui`；⛔ 不是读数）。
 *
 * ★ 对象**恒存在**；缺接线（非窗口臂 / 测试替身）是**调用时**的 `TypeError`，不是 `undefined`。
 * ★ **只在 `render` 内可调用**；副作用就发生在 render 里（⛔ 不在 `handleEvent`）。
 * ★ 用法错 ⇒ `TypeError`；领域失败 ⇒ **返回值**（`ok:false`，不抛 —— HUD 要显示"失败"并继续画）。
 * ★ 只在窗口档可达：`--present headless` 与 `--overlay` 同给是 `exit 2`。
 * ★ 这一版只有这两项；`reload()` / `time_budget_ms()`（`docs/03-emulator/overlay.md` §3.5 的
 *   目标 API）**尚未实现**。
 */
declare const host: {
  /** 开始一段窗口 profile；`label` = UTF-8（默认 HUD 传 `"overlay"`）。 */
  profileBegin(label: string): UiProfileBeginResult;
  /** 结束这一段窗口 profile，并**由宿主**把结论文本写进系统剪贴板。 */
  profileEnd(): UiProfileEndResult;
};

/**
 * 入口（脚本必须显式赋给 `globalThis.render`）。宿主**每帧**调一次；调用前 display list 已清空。
 *
 * ★ `host.*` 的副作用发生在 **render 里**（⛔ 不在 `handleEvent`）；同一次点击只有这一帧看得到。
 * ★ 只是类型声明：`jsconfig.json` 的 `checkJs` 用它，让 `globalThis.render = function …` 类型通过。
 *   运行时这个全局**不存在**（脚本自己赋；宿主没赋 ⇒ 加载期硬失败）。
 */
declare var render: () => void;
