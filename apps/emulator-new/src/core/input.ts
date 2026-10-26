/**
 * 输入的平台边界（`data/spec/chapters/60-ops-input.md` 的 `CORE-INPUT-*`）。
 *
 * ★ 核心只认这个接口，不认键盘 / 鼠标 / 手柄的具体形态：平台把「当前输入」归约成
 *   一个 32 位输入字、一个光标位置、一个按钮位图与一条鼠标事件队列。
 * ★ 所有方法都带 `now`（仿真 / 宿主毫秒）：平台按自己的时钟决定"这一刻有哪些事件"，
 *   核心不替平台排队。
 * ★ 没有输入后端时（`--script` 调试口 / 纯核心测试）调用方传 `undefined`：
 *   输入字恒 0、光标不可用、按钮位图 0、没有鼠标事件 —— 这是显式的"没有设备"，
 *   不是"悄悄补 0"。
 */

/** 一条鼠标事件记录（字段语义由平台定义；核心只原样传给脚本） */
export interface MouseEventRecord {
  x: number;
  y: number;
  flags: number;
  extra: number;
}

/** 平台输入后端 */
export interface InputHost {
  /** 采样一次按键 / 手柄状态，返回 32 位输入字 */
  sampleKeys(now: number): number;
  /** 当前光标位置（客户区）；`null` = 设备不可用 */
  cursor(now: number): { x: number; y: number } | null;
  /** 当前鼠标按钮位图；`null` = 设备不可用 */
  buttons(now: number): number | null;
  /** 取一条待处理鼠标事件；`null` = 没有事件 */
  nextMouseEvent(now: number): MouseEventRecord | null;
}

/** 鼠标回调的登记状态（`0xcc` / `0xcd`；引擎级一格） */
export interface MouseCallbackState {
  /** 轮询间隔（毫秒；0 写成 1） */
  interval: number;
  /** 上次派发时刻 */
  last: number;
  /** 登记的脚本位置（dword 下标）；-1 = 取消 */
  target: number;
  /** 登记时当前帧的脚本身份（frame.localDepth；打开脚本用的统一文件 id） */
  owner: number;
}

/**
 * **按住派发族**（`0x102` / `0x103` / `0x104` / `0x105`）的引擎级状态。
 * ★ 三格初值都是**合同值**（不是兜底）：`mask = 0`、`modulus = 0`、`contextKey = −1`
 *   （启动路径与 `0xfc` 都写它们）。⚠ 与 `keyTotal` 不同：那一格引擎**不建立**初值（UB）。
 * ★ 按住**游标**不在 `HoldState` 里，也**不是**按 `Frame` 对象的：它按**帧槽号**存在
 *   `Machine.holdCursorBySlot`（帧释放 / 重载不复位）。`HoldState` 只放引擎级那三格 + 计数。
 */
export interface HoldState {
  /** `0x105` ← ；`0x104` 用第 i 位（i = 键编号，与输入字同一套编号）决定返回点与是否夹上限。初值 0 */
  mask: number;
  /** `0x103` ← ；`0x104` 每条出口对游标取余。初值 0 ⇒ 第一次 `0x104` 在游标推进处除零 */
  modulus: number;
  /**
   * `0x104` 的"上一次决定游标上下文的编号"（引擎里是**进程级静态格**，不复位）。
   * ★ 本层一个 `Machine` 就是"一个引擎" ⇒ 放成字段与"跨引擎共享"行为等价。初值 −1（`.data` 初值）。
   * ⛔ 它**不按帧**，别塞进 `Frame`。
   */
  contextKey: number;
  /** 只记账：按住派发次数（CLI / 诊断用） */
  dispatches: number;
}

/** 按住派发族的初值（合同值，不是"没写就 0"的兜底） */
export function initialHoldState(): HoldState {
  return { mask: 0, modulus: 0, contextKey: -1, dispatches: 0 };
}

/** 引擎侧输入状态（可观察的输入字 / 键总数 / 模式字 / 鼠标回调登记） */
export interface InputState {
  /** 32 位输入字（`0xff` 清后重采、`0x101` 采样后清零、平台采样写入） */
  word: number;
  /** 键总数（`0xfe` 写；决定 `0x100` 的扫描范围与"无按键"表项） */
  keyTotal: number;
  /**
   * ★ 键总数是否已被 `0xfe` 写过（引擎**不建立**初值：构造 / 帧装载 / 重置都不写它）。
   * `false` 时读 `keyTotal` 是**未定义行为**：读者要留一条 `key-total-unset` 诊断。
   */
  keyTotalEstablished: boolean;
  /** 主循环模式字（`0x24e` 写；位 0 被等待服务读取） */
  mode: number;
  mouse: MouseCallbackState;
  /** 采样次数（只记账，供 CLI / 诊断） */
  samples: number;
  /** 鼠标回调派发次数（只记账） */
  mouseDispatches: number;
  /** 按键回调派发次数（只记账） */
  keyDispatches: number;
}

/** 输入状态的初值 */
export function initialInputState(): InputState {
  return {
    word: 0,
    // ★ 引擎**不建立**这一格的初值（唯一写点是 `0xfe`）⇒ 0 是**本实现的 UB 兜底，不是合同**；
    //   任何断言 / 日志都不得把它表述成"引擎启动后的值"。`0xfe` 之前读到它的读者
    //   （`0x100` / `0xff`）会经 `readKeyTotal` 记一条 `key-total-unset` 诊断。
    keyTotal: 0,
    keyTotalEstablished: false,
    mode: 0,
    mouse: { interval: 1, last: 0, target: -1, owner: -1 },
    samples: 0,
    mouseDispatches: 0,
    keyDispatches: 0,
  };
}
