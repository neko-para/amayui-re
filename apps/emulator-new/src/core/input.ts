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
  /** 登记时的当前帧调用层数 */
  depth: number;
}

/** 引擎侧输入状态（可观察的输入字 / 键总数 / 模式字 / 鼠标回调登记） */
export interface InputState {
  /** 32 位输入字（`0xff` 清后重采、`0x101` 采样后清零、平台采样写入） */
  word: number;
  /** 键总数（`0xfe` 写；决定 `0x100` 的扫描范围与"无按键"表项） */
  keyTotal: number;
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
    // ★ 初值本版不规定（spec 残差）；真实启动链里 SYSTEM4 的 0xfe 会写它。
    keyTotal: 0,
    mode: 0,
    mouse: { interval: 1, last: 0, target: -1, depth: -1 },
    samples: 0,
    mouseDispatches: 0,
    keyDispatches: 0,
  };
}
