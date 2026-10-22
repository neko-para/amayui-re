/**
 * 影片播放的平台边界（`60-ops-wait.md` 的 `CORE-OPS-MOVIE` / `70-vm.md` 的 `CORE-VM-MOVIE`）。
 *
 * 核心只发请求、读回“当前位置 / 停止位置 / 是否已结束”，不碰解码器、纹理表面或音频。
 * 平台实现（Electron 的 pl_mpeg WASM）负责把帧写进纹理槽并提供时钟；CLI 没有实现 ⇒ 影片位一直
 * 保持门关闭（与“未建模”一致，不伪造播放完成）。
 */
export interface MovieOpenRequest {
  slot: number;
  resourceId: number;
  /** `0x20f` 的第 3 个操作数原样；位含义见规范 */
  option: number;
}

/** 平台侧影片句柄（核心不解释其形状） */
export type MovieHandle = object;

export interface MoviePoll {
  /** 当前播放位置（秒） */
  currentTime: number;
  /** 停止位置（秒；通常是时长） */
  stopTime: number;
  /** 后端报告的“已结束”；取不到时不要伪造 true */
  ended: boolean;
}

export interface MovieHost {
  /** 建后端并开始异步打开；返回句柄（失败返回 null，核心走同步失败路径） */
  open(request: MovieOpenRequest): MovieHandle | null;
  /** 开始播放（幂等；后端尚未就绪时由平台自己排队） */
  play(handle: MovieHandle): void;
  /** 读当前状态；返回 null = 取不到（核心按“尚未结束”处理） */
  poll(handle: MovieHandle): MoviePoll | null;
  /** 循环：回到开头并重新播放 */
  rewind(handle: MovieHandle): void;
  /** 停止并释放播放后端；不释放纹理对象 */
  close(handle: MovieHandle): void;
  /** 正常收尾的“填纹理”请求；像素效果本版不规定 */
  fillTexture(textureId: number): void;
}
