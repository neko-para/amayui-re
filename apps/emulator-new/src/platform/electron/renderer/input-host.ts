/**
 * Electron renderer 的输入后端（`60-ops-input.md` 的 `InputHost`）。
 *
 * ★ 这是**平台实现**：把 DOM 的鼠标 / 键盘事件归约成核心要的「光标 / 按钮位图 / 输入字 / 鼠标事件」。
 * ★ 物理键 → 引擎输入位号的映射**不在 spec 里**（`CORE-INPUT-WORD` 的残差）。本实现用一组**调试映射**
 *   （`Digit0..9` → 0..9，`KeyA..C` → 10..12），只用于让 TITLE 的按键回调可被驱动；
 *   ⛔ 它不是引擎的真实按键表，换一台机器/换一套配置不应把它当合同。
 * ★ 鼠标事件按引擎的「位图 + 事件队列」两路提供：`buttons()` 是当前状态，`nextMouseEvent()` 是边沿事件。
 *   鼠标移动只更新光标，不排队事件（避免每像素一条事件）。
 */
import type { InputHost, MouseEventRecord } from '../../../core/input.ts';

/** 调试用物理键 → 引擎输入位号（不是 spec 合同；见文件头） */
const DEBUG_KEY_MAP: Readonly<Record<string, number>> = {
  Digit0: 0,
  Digit1: 1,
  Digit2: 2,
  Digit3: 3,
  Digit4: 4,
  Digit5: 5,
  Digit6: 6,
  Digit7: 7,
  Digit8: 8,
  Digit9: 9,
  KeyA: 10,
  KeyB: 11,
  KeyC: 12,
};

function mappedKey(code: string): number | null {
  return Object.prototype.hasOwnProperty.call(DEBUG_KEY_MAP, code) ? DEBUG_KEY_MAP[code] : null;
}

/** DOM 鼠标按钮（0=左、1=中、2=右）→ 引擎按钮位号（0=左、1=右；中键忽略） */
function buttonIndex(button: number): number | null {
  if (button === 0) return 0;
  if (button === 2) return 1;
  return null;
}

export class DomInputHost implements InputHost {
  private readonly canvas: HTMLCanvasElement;
  private readonly keys = new Set<number>();
  private readonly queue: MouseEventRecord[] = [];
  private x = 0;
  private y = 0;
  private cursorAvailable = false;
  private buttonsMask = 0;

  private readonly onMouseMove = (event: MouseEvent): void => {
    const rect = this.canvas.getBoundingClientRect();
    const width = this.canvas.width || rect.width || 1;
    const height = this.canvas.height || rect.height || 1;
    this.x = Math.round(((event.clientX - rect.left) / Math.max(1, rect.width)) * width);
    this.y = Math.round(((event.clientY - rect.top) / Math.max(1, rect.height)) * height);
    this.cursorAvailable = true;
  };

  private readonly onMouseDown = (event: MouseEvent): void => {
    const bit = buttonIndex(event.button);
    if (bit === null) return;
    this.buttonsMask |= 1 << bit;
    this.queue.push({ x: this.x, y: this.y, flags: 0x11, extra: bit });
    event.preventDefault();
  };

  private readonly onMouseUp = (event: MouseEvent): void => {
    const bit = buttonIndex(event.button);
    if (bit === null) return;
    this.buttonsMask &= ~(1 << bit);
    this.queue.push({ x: this.x, y: this.y, flags: 0x12, extra: bit });
    event.preventDefault();
  };

  private readonly onKeyDown = (event: KeyboardEvent): void => {
    const key = mappedKey(event.code);
    if (key === null) return;
    this.keys.add(key);
    event.preventDefault();
  };

  private readonly onKeyUp = (event: KeyboardEvent): void => {
    const key = mappedKey(event.code);
    if (key === null) return;
    this.keys.delete(key);
    event.preventDefault();
  };

  constructor(canvas: HTMLCanvasElement) {
    this.canvas = canvas;
    canvas.addEventListener('mousemove', this.onMouseMove);
    canvas.addEventListener('mousedown', this.onMouseDown);
    canvas.addEventListener('mouseup', this.onMouseUp);
    window.addEventListener('keydown', this.onKeyDown);
    window.addEventListener('keyup', this.onKeyUp);
  }

  /** 解绑监听（窗口销毁 / 会话结束） */
  destroy(): void {
    this.canvas.removeEventListener('mousemove', this.onMouseMove);
    this.canvas.removeEventListener('mousedown', this.onMouseDown);
    this.canvas.removeEventListener('mouseup', this.onMouseUp);
    window.removeEventListener('keydown', this.onKeyDown);
    window.removeEventListener('keyup', this.onKeyUp);
  }

  sampleKeys(): number {
    let mask = 0;
    for (const key of this.keys) mask |= 1 << key;
    return mask >>> 0;
  }

  cursor(): { x: number; y: number } | null {
    // 第一次 move 之前不伪造光标位置（避免把"还没动"当成"在 (0,0)"）。
    return this.cursorAvailable ? { x: this.x, y: this.y } : null;
  }

  buttons(): number | null {
    return this.buttonsMask >>> 0;
  }

  nextMouseEvent(): MouseEventRecord | null {
    return this.queue.shift() ?? null;
  }
}
