/**
 * Electron 呈现策略：**模拟器不支持全屏**（显式决策）。
 *
 * ★ 引擎的 `screenMode = 0` 是“占满屏 / 独占全屏”的请求；本模拟器不实现显示器模式切换，
 *   也不做全屏缩放。平台把这条请求降级成一个 **`set:WinX` × `set:WinY` 的普通窗口**：
 *   `exStyle` 清 0、客户区尺寸改为虚拟画面尺寸、使用平台默认位置。
 * ★ 这是**模拟器产品决策**，不是 spec 结论；spec 仍把 `06-window` 的请求原样交给平台。
 * ★ 纯函数：只做参数变换，不碰 Electron / DOM；便于用 pure 测试钉住。
 */
import type { WindowCreateParams } from '../../../core/window.ts';

export interface PresentationPolicy {
  /** 模拟器不支持全屏：把占满屏请求降级为窗口（当前固定 true） */
  ignoreFullscreen: boolean;
  /** 降级窗口的客户区尺寸（来自 `set:WinX` / `set:WinY`） */
  width: number;
  height: number;
}

/** 平台默认位置哨兵（core 的 `DEFAULT_WINDOW_POSITION`，不导出；这里只作为数值使用） */
const DEFAULT_WINDOW_POSITION = 0x80000000;

/**
 * 把“占满屏”请求改成普通窗口。
 * ★ 只改 `exStyle` 为 0 的判据（`window-host.createWindow` 用它区分 fullscreen），
 *   同时把 width/height/clientWidth/clientHeight 改成虚拟尺寸；窗口态参数原样返回。
 */
export function applyPresentationPolicy(
  params: WindowCreateParams,
  policy: PresentationPolicy,
): WindowCreateParams {
  if (!policy.ignoreFullscreen) return params;
  if (Number(params.exStyle) === 0) return params;
  const width = Math.max(1, Math.round(policy.width));
  const height = Math.max(1, Math.round(policy.height));
  return {
    ...params,
    width,
    height,
    clientWidth: width,
    clientHeight: height,
    x: DEFAULT_WINDOW_POSITION,
    y: DEFAULT_WINDOW_POSITION,
    exStyle: 0,
  };
}
