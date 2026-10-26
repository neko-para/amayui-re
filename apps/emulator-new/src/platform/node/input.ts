/**
 * 平台层（Node / CLI）：**脚本化输入后端**（`60-ops-input.md` 的 `InputHost`）。
 *
 * 用途：让 CLI 在无人值守 / 无窗口时也能驱动 TITLE 的输入循环。输入不是"猜"出来的：
 * 它由一份**指令式脚本**给出（`at_ms` + `move` / `down` / `up` / `key ... down|up`），
 * 平台按仿真时钟把它们归约成核心要的四个量。
 *
 * ★ 这是**测试/仿真**装置，不是引擎语义：它不改变 `data/spec` 的任何合同。
 * ★ 键编号是引擎的**输入位号**（0..31），不是 Windows VK 码；物理键映射不在本层。
 * ★ 没有 `--input` 时 CLI 不创建它 ⇒ 核心看到 `inputHost === undefined`（没有设备）。
 */

import type { InputHost, TouchPointRecord } from '../../core/input.ts';

export type InputEvent =
  | { at: number; kind: 'move'; x: number; y: number }
  | { at: number; kind: 'button'; button: number; down: boolean }
  | { at: number; kind: 'key'; key: number; down: boolean }
  | { at: number; kind: 'wheel'; delta: number };

export interface ScriptedInputStats {
  /** 已应用的脚本事件数 */
  applied: number;
  /** 最后一条事件的 `at`（没有事件时为 0） */
  lastEventAt: number;
  /** 脚本是否已全部应用 */
  exhausted: boolean;
}

/**
 * 解析指令式输入脚本：
 *
 * ```
 * # 注释
 * 100 move 640 360
 * 200 down 1
 * 260 up 1
 * 300 key 13 down
 * 360 key 13 up
 * ```
 *
 * 分隔：换行或 `;`；`at` 是仿真毫秒（非负整数）。
 */
export function parseInputScript(text: string): InputEvent[] {
  const events: InputEvent[] = [];
  const lines = text.split(/[\n;]/);
  for (const [index, raw] of lines.entries()) {
    const line = raw.replace(/#.*$/, '').trim();
    if (!line) continue;
    const parts = line.split(/\s+/);
    const at = Number.parseInt(parts[0] ?? '', 10);
    if (!Number.isInteger(at) || at < 0) throw new Error(`输入脚本第 ${index + 1} 行：at 必须是非负整数：${line}`);
    const command = parts[1];
    if (command === 'move') {
      const x = Number.parseInt(parts[2] ?? '', 10);
      const y = Number.parseInt(parts[3] ?? '', 10);
      if (!Number.isInteger(x) || !Number.isInteger(y)) throw new Error(`输入脚本第 ${index + 1} 行：move 需要 x y：${line}`);
      events.push({ at, kind: 'move', x, y });
    } else if (command === 'down' || command === 'up') {
      const button = Number.parseInt(parts[2] ?? '', 10);
      if (!Number.isInteger(button) || button < 0 || button > 31) throw new Error(`输入脚本第 ${index + 1} 行：按钮编号必须是 0..31：${line}`);
      events.push({ at, kind: 'button', button, down: command === 'down' });
    } else if (command === 'key') {
      const key = Number.parseInt(parts[2] ?? '', 10);
      const state = parts[3];
      if (!Number.isInteger(key) || key < 0 || key > 31) throw new Error(`输入脚本第 ${index + 1} 行：键编号必须是 0..31：${line}`);
      if (state !== 'down' && state !== 'up') throw new Error(`输入脚本第 ${index + 1} 行：key 需要 down / up：${line}`);
      events.push({ at, kind: 'key', key, down: state === 'down' });
    } else if (command === 'wheel') {
      const delta = Number.parseInt(parts[2] ?? '', 10);
      if (!Number.isInteger(delta)) throw new Error(`输入脚本第 ${index + 1} 行：wheel 需要整数增量：${line}`);
      events.push({ at, kind: 'wheel', delta });
    } else {
      throw new Error(`输入脚本第 ${index + 1} 行：未知命令 ${JSON.stringify(command)}：${line}`);
    }
  }
  // 稳定排序：同一时刻按书写顺序
  return events
    .map((event, order) => ({ event, order }))
    .sort((a, b) => (a.event.at - b.event.at) || (a.order - b.order))
    .map((x) => x.event);
}

/** 脚本化输入后端：按 `now` 惰性地应用事件 */
export class ScriptedInputHost implements InputHost {
  private readonly events: InputEvent[];
  private cursorIndex = 0;
  private readonly keys = new Set<number>();
  private x = 0;
  private y = 0;
  private cursorAvailable = false;
  private buttonsMask = 0;
  private applied = 0;
  private lastEventAt = 0;

  constructor(events: InputEvent[]) {
    this.events = events;
  }

  /** 应用所有 `at <= now` 的事件（幂等：已应用的不再应用） */
  private apply(now: number): void {
    while (this.cursorIndex < this.events.length && this.events[this.cursorIndex].at <= now) {
      const event = this.events[this.cursorIndex];
      this.cursorIndex += 1;
      this.applied += 1;
      this.lastEventAt = event.at;
      if (event.kind === 'move') {
        this.x = event.x;
        this.y = event.y;
        this.cursorAvailable = true;
      } else if (event.kind === 'button') {
        if (!this.cursorAvailable) {
          // 没有先 move：按 (0,0) 建立一个可用光标（CLI 的显式仿真选择）
          this.cursorAvailable = true;
        }
        if (event.down) this.buttonsMask |= 1 << event.button;
        else this.buttonsMask &= ~(1 << event.button);
      } else if (event.kind === 'key') {
        if (event.down) this.keys.add(event.key);
        else this.keys.delete(event.key);
      } else if (event.kind === 'wheel') {
      }
    }
  }

  sampleKeys(now: number): number {
    this.apply(now);
    let mask = 0;
    for (const key of this.keys) mask |= 1 << key;
    return mask >>> 0;
  }

  cursor(now: number): { x: number; y: number } | null {
    this.apply(now);
    return this.cursorAvailable ? { x: this.x, y: this.y } : null;
  }

  buttons(now: number): number | null {
    this.apply(now);
    return this.buttonsMask >>> 0;
  }

  nextTouchPoint(now: number): TouchPointRecord | null {
    this.apply(now);
    return null;  // 桌面鼠标没有触点缓冲；TITLE 会回退到 0x109 / 0x108。
  }

  /** 统计（CLI 收口 / 报告用） */
  stats(): ScriptedInputStats {
    return {
      applied: this.applied,
      lastEventAt: this.lastEventAt,
      exhausted: this.cursorIndex >= this.events.length,
    };
  }

  /** 不推进时钟、只看事件是否已耗尽（CLI 的 `onIteration` 用） */
  get exhausted(): boolean {
    return this.cursorIndex >= this.events.length;
  }

  /** 最后一条已应用事件的 `at`（没有事件时为 0） */
  get appliedUntil(): number {
    return this.lastEventAt;
  }
}
