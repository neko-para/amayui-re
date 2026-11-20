# AOT · GPU 直出每帧整幅回读（5.6–7.1 ms/帧，含阻塞 fence + 每帧新建 transfer buffer）：优化候选与裁决

- id: REQ-01M7S5KFRPAB6KSC033ECDHYMW
- type: req
- status: doing
- parent: REQ-01M7JHSEZVMYB4XHEN32V7V6HJ
- tags: [emulator-lift, perf, gpu, present]

## 现象 / 读数

GPU 直出 + 窗口模式下，每个 Present 都做一次**整幅 GPU→guest 回读**（1280×720×32bpp = 3,686,400 B）：

- 用户 profile：`Present 965.684 ms / N=114`，分解 `readback 821.127 / hash 83.537 / sink 61.019 ms`
  （readback 占墙钟 29.8%）；
- 本机 OPTION 复现：`Present 2528.294 ms / N=369`，`readback 2077.967 / hash 274.225 / sink 175.022 ms`
  （≈5.6 ms/帧）；
- hidden SAVE05（`.tmp/save05-bench/logs/hpa1.out:2084`，N=195）：`Present 1715.401 ms (22.6%)`
  = readback **7.93** / hash **0.748** / sink **0.117** ms/帧。

⇒ 回读是**每帧固定成本**，与场景无关。

## 构成（读代码）

- `src/runtime/d3d9.cpp` 的 `present()`：`gpu_dirty` 时 `gpu->rasterSyncToGuest(...)`；
  `!dirty` 的帧由 sink `directUploadGuest` 把 guest 上传到 target ⇒ 「直出 + guest 镜像」结构上**每帧恰好一次整幅搬运**。
- `src/runtime/d3d9_gpu.cpp` 的 `rasterSyncToGuest` → `rasterFlush` + `downloadInto`：
  `SDL_DownloadFromGPUTexture` + **每帧一次阻塞 `SDL_WaitForGPUFences`** + **每帧新建/释放一张 3.5 MiB transfer buffer**
  + 逐行 memcpy + 逐像素清 X 字节。
- `hash`：`HeadlessPresenter::present` 是所有档的唯一汇点，guest 像素哈希在 sink 之前**无条件**算
  （`present.cpp:120-123`）——窗口档照付（0.748 ms/帧）。
- `overlay` **不增加**回读（`d3d9_gpu.cpp` 显式禁止叠层碰 target；`drawOverlay` 只吃宿主读数）。

## 裁决（2026-11-21 · 评估全文 `.tmp/present-readback/evaluation.md`）

- 候选 1（hash 并进下载）+ 2（复用 transfer buffer / 拆 fence）+ 诊断 opt-in ⇒ **Phase A**（零语义）。
  ⚠ **单独做 A 不会降 readback 总量**：`Clear` 屏障（`d3d9.cpp:762-770`）在下一帧开头重新材质化
  （`rasterFlush` 只置 `dirty_`，只有 `downloadInto` 置假），成本从 `Present` 迁到 `Clear`。
- 候选 3（懒镜像）⇒ **Phase B**：`REQ-01M7W2FR9EJEMG2QDMZNG6N7WV`（放弃「guest 后缓冲在帧边界权威」模型）。
- 候选 4（脏矩形）维持**不立项**（全屏重绘、近零收益）。
- 口径确认：每帧回读**不是** DX9 语义（CPU 读回只有 `GetRenderTargetData` / `LockRect` 两条路）；
  7 个显式读点各有屏障；overlay/UI/插件/screenshot 不读 guest 像素（硬证据见评估 §2）。

## 判据（observation，⛔ 不进 CTest）

- `LIFT_GPU_SUBMITS=1` 的 `[gpu-submits] readback=…` 每帧计数；四臂对照：窗口无 overlay / 窗口 + overlay /
  headless / vsync=1 ⇒ 证明 overlay 无关、swapchain 无关。
- 两个 draw 量不同的场景比 `readback ms/帧` ⇒ 分辨「等 fence」vs「拷贝」。
- 目标：Phase A 后 Present ≈0.87 ms/帧 + 一次/run 的 hash；Phase B 后 ≈0.12（仅 sink）。
  ⛔ 不先写一个数字进判据；主判据用**无仪器墙钟**（`--profile` 会强制逐帧 hash）。

## 边界

- ⛔ 不为提速改 guest 可见语义（候选 3 走 Phase B 单）。
- ⛔ 不动 `DeleteObject` / 句柄表（另一张单）。
- 本单只到「候选读数 + 裁决」；Phase A 实施与读数是它的验收。
