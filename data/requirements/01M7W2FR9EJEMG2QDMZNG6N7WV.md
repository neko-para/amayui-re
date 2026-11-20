# Present 回读 Phase B：Clear 走 GPU 侧 + guest 后缓冲降为派生（放弃帧边界权威）

- id: REQ-01M7W2FR9EJEMG2QDMZNG6N7WV
- type: req
- status: open
- parent: REQ-01M7JHSEZVMYB4XHEN32V7V6HJ
- tags: [emulator-lift, perf, gpu, present, semantics]

## 一句话

放弃「guest 后缓冲在**每个帧边界**权威」这条**我们的模型**（⛔ 不是 DX9 语义），把每帧整幅 GPU→guest 回读从默认路径拿掉：
`Clear` 改成 GPU 侧的 color-clear 请求，guest 像素只在 7 个**显式读点** + 诊断点**就地**材质化。

## 前提

- Phase A（父单候选 1/2：hash 并进下载、诊断 opt-in、复用 transfer buffer / 拆 fence）先落地并留下干净读数；
  本单在 Phase A 读数出来前 ⛔ 不排期。
- 先量三处 `<待核>`（见下）再决定是否保留 rect `Clear` 的材质化分支。

## 为什么不是 DX9 语义

- 真 DX9 后缓冲在 GPU，CPU 不可见；CPU 读回只有 `GetRenderTargetData`（`d3d9.cpp:2237`）与
  lockable back buffer 的 `LockRect`（`:1748`）两条路。
- shim 已在这 7 处各有一道**显式**屏障：`GetRenderTargetData` / `LockRect` / `UpdateSurface` /
  `LoadSurfaceFromSurface` / 纹理 `LockRect` / `SetRenderTarget` 旧目标 / `Clear`。
- 每帧回读的真实消费者只有：renderer 回退路径的 sink（`present_sdl.cpp:1283,1330`）与诊断（hash / dump / freeze / sampler）。
- overlay / UI / 插件 / screenshot **不读** guest 像素：`src/ui/**` 对 arena 零引用；直出叠层只画 swapchain
  （`d3d9_gpu.cpp:1760-1772`）；开/关 overlay 的 `--dump-frame` 逐字节相同（`tests/aot_overlay.cpp:590-596`）。
  ⇒ ⛔ 不为它们加默认回读。

## 改动（设计）

- `d3d9.cpp:762-770` 的 `Clear` 屏障 → `rasterRequestColorClear(rects, color)`（形状照 `gpu_raster.h:469` 的深度版）：
  全幅 ⇒ 该 pass 的 `LOADOP_CLEAR`；带 rect ⇒ 保留现状材质化 + 计数（`<待核>`：本引擎是否真用 rect Clear）。
- `rasterFlush`（`d3d9_gpu.cpp:1468`）的 `dirty_` 语义改为「guest 镜像陈旧」而不是「本帧必须下载」；
  `downloadInto` 仍是唯一置假处（`:809`）。
- guest 后缓冲从「权威」降为「派生」：只在上面 7 个读点 + 诊断点材质化。
- 诊断点就地补算（Phase A 已把 `lastHash` 惰性化 ⇒ 本单不重复设计）。

## 先量再判（Phase A 读数里一起抓）

1. SAVE05 移动窗口的 `!dirty` 帧占比与 `[gpu-submits] upload=`（现读数抓了 `readback=`，没抓 `upload=`）。
2. 本引擎是否真的用**带 rect** 的 `Clear` ⇒ 决定 rect 分支要不要保留材质化。
3. CPU 回退（renderer sink）是否逐帧触发 ⇒ 决定回读能否真的归零。

## 判据

- `LIFT_GPU_SUBMITS=1` 的 `[gpu-submits] readback=` 在「直出 + 无诊断」档**归零**；
  `Clear` 子桶回到 ~0.4 ms/帧（现状读数 `hpa1.out:2095`）。
- 语义指纹逐位同：`exit` + `frames` + `stop @ EA` + arena 三元组 + `table misses`（`AGENTS.md §12.6`）。
- 像素等价：固定帧 `--dump-frame` 逐字节比；**新判据** = dump 首帧必须含非背景像素
  （防「触发漏了 ⇒ 两次都旧帧 ⇒ 现判据照绿」）。
- ⛔ 不写「vsync 档变快」：`sink` 仍是 `SDL_WaitAndAcquireGPUSwapchainTexture`（`d3d9_gpu.cpp:1699-1705`）。

## 边界

- ⛔ 不改 DX 语义：7 个显式读点的行为必须逐位不变。
- ⛔ 不动 Phase A 的诊断 opt-in 设计；⛔ 不合并进默认档之前必须先过上面的固定帧像素对照。
- ⛔ 脏矩形回读：已裁不立项（本场景全屏重绘，近零收益）。

## 未做 / 待核

- 与 `SetRenderTarget` 旧目标屏障的交互（`d3d9_pipeline.cpp:1985-2014`）：实现时复核。
- `Clear` 的 rect 分支读数：待 `#2` 量完再定。
- 消费者与 7 个读点的证据：`docs/03-emulator/present-readback.md`（Phase A 文档）；本单只留 Phase B 的边界与判据。
