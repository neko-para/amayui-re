# emulator-lift · TITLE 呈现下限：direct GPU present + sprite batching

- id: REQ-01M6RMSWFQBFSD9MBQXPD3M0EC
- type: req
- status: open
- parent: REQ-01M6RH9CDPM8PG2SFEG0PYT46T
- tags: [emulator-lift, gpu, present, performance, title]

## 现状（2026-11-06 实测）
TITLE 的 GPU 全屏四边形已走 GPU（`--gpu-strict` fallback=0，DrawPrimitive 10.6–11.4 ms），但整帧 wall 仍 0.16–0.23 s（约 4–6 fps），未达 33 ms。
| 项 | TITLE frames 64–80 |
|---|---:|
| wall | 155–228 ms |
| `com!IDirect3DDevice9::Present` | 86–141 ms（一轮 median 82，另一轮 108） |
| `ID3DXSprite::End` | 21–63 ms（median 26–30） |
| `DrawPrimitive` | 9–18 ms |
| guest | 15–28 ms |
## 根因
1. `d3d9::present` 在 GPU 光栅后把 3.6 MB target 读回 guest，再由 `SdlWindow::submitFrame` 用 `SDL_UpdateTexture` 上传到 SDL_Renderer、`SDL_RenderPresent` 呈现 ⇒ 每帧两次全帧搬运。即使 batch 已 fallback=0，Present 仍占 80–108 ms。
2. D3DXSprite 每个条目一次 `drawBatch`：独立 transfer buffer + command buffer + submit；TITLE 每帧约 24 次提交，`End` 占 26–30 ms。
## 验收
1. `--raster gpu --real-time --present window`，TITLE 渐变 wall ≤33 ms（≥30 fps），且 `--gpu-strict` 仍 exit 0、fallback=0。
2. Present：GPU target 直接呈现（SDL_GPU swapchain 或等价路径），**不在 Present 无条件做 GPU→guest 全帧读回**；`--dump-frame` / `--present-screenshot` / guest 真正读后缓冲时仍能按需同步。
3. Sprite：同一帧/同一状态的 sprite 合批，减少 command buffer / transfer buffer / submit 次数；可见像素与逐条路径一致。
4. TITLE 同批 CPU↔GPU 像素仍 ≤1 LSB（当前为 0）；无整层缺失/黑块/方向/alpha 错误。
5. 缺省 `--raster cpu` 逐字节不变；LOGO 不退化；CTest 全绿；真实 boot `--gpu-strict` 仍 exit 0。
6. 守卫：至少一条 TITLE 帧墙钟/呈现路径的机械判据（不能只靠人看），以及 sprite 合批前后像素/提交次数的对照。
## 非目标
- 不改变 GPU 光栅的准入/像素语义；不把“更快”当“可静默跳过 guest 读回”。
- 不在本单扩多 stage / 16bpp / alpha test / 深度镜像；这些仍按各自缺口跟踪。
