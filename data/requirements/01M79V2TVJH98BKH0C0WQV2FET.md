# emulator-lift · ID3DXSprite 合成目标硬取后缓冲：绑定 RT 时应写进 RT（缩略图全黑的最后一环）

- id: REQ-01M79V2TVJH98BKH0C0WQV2FET
- type: req
- status: done
- parent: REQ-01M6H6G6DSRG1ZRD810Z0HTH8V
- verify: apps/emulator-lift/tests/gpu_batch.cpp#d3d9p/sprite-render-target
- tags: [emulator-lift, d3d9, sprite, rtt]

## 缺口（`REQ-01M79MSMWGFEKF8J7C7VYWS4YP` 的停止线 3 改判方向）

GPU 的纹理 RT 已经能工作（准入放行 + 绑定 + 读回，守卫 `d3d9p/render-to-texture` / `-feedback` 绿，真后端臂 passed）。
但 `SAVE00.STH` 的 payload 仍全 0，根因是：**精灵（`ID3DXSprite`）合成时不看绑定的 render target**，硬取 `dev.arena`（后缓冲）。

- 实测（`save-graph`，macOS arm64 / RelWithDebInfo，复审独立复核）：RT 是 `1280x720 A8R8G8B8`，`GetRenderTargetData` 的源 arena
  **就是**绑定的 RT，但 `src_nz = 0 / 3,686,400`；RT 窗口内 = 2×`Clear(Z+black)` + **2 条 `DrawPrimitive`（rec=1，全屏无纹理的黑覆盖 quad）**
  + **77 次 `[d3dx9] End/Flush` 合成**（首条 921600 px = 满屏）。这些像素全进后缓冲（`sprite: draws=301258 gpu=301258 fallback=0`），
  原因是 sprite 两条路径都硬取 `dev.arena`。
- ⚠ 别用"RT 窗口内 Draw 计数 = 0"当判据：GPU 模式下 `DrawPrimitive` 的"写 0 个像素"是 CPU 计数，那 2 条黑 quad 就是这样被漏掉的。
- 代码落点：`apps/emulator-lift/src/runtime/d3dx9_sprite.cpp`（合成目标 / `compositeBatch`）与 `apps/emulator-lift/src/runtime/d3dx9.cpp` 的 sprite 批次；
  同族索引条目 `REQ-01M7485DCS1RBQSDDH6F68HS1J` 的 #1。

## 判据

- 绑定了纹理 RT 时，`ID3DXSprite::Draw` 的像素必须写进该 RT（后缓冲不变）；未绑定 RT 时行为与今天一致。
- 守卫 `d3d9p/sprite-render-target`：Mock/纯段断言合成目标基址 = RT 像素块（修前是 `dev.arena` ⇒ 红）；
  真后端臂（无 GPU ⇒ `LIFT_SKIP(77)`）把 sprite 画进 RT 再 `GetRenderTargetData` 读回 ⇒ 非全 0 且颜色在容差内。
- `save-graph` 的 `SAVE00.STH payload 非零字节 > 0` 断言由 `REQ-01M780VKKRFHJEX6VM74DRCYBM` 挂（本单只提供能力）。

## 非目标

- 不做多 RT / MRT / 跨帧保留；不改 sprite 的合批/顺序语义；不回退到 CPU 路径。
