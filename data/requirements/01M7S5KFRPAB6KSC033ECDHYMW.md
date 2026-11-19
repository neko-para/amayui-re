# AOT · GPU 直出每帧整幅回读（5.6–7.1 ms/帧，含阻塞 fence + 每帧新建 transfer buffer）：优化候选与裁决

- id: REQ-01M7S5KFRPAB6KSC033ECDHYMW
- type: req
- status: open
- parent: REQ-01M7JHSEZVMYB4XHEN32V7V6HJ
- tags: [emulator-lift, perf, gpu, present]

## 现象 / 读数

GPU 直出 + 窗口模式下，每个 Present 都做一次**整幅 GPU→guest 回读**（1280×720×32bpp = 3,686,400 B）：

- 用户 profile：`Present 965.684 ms / N=114`，分解 `readback 821.127 / hash 83.537 / sink 61.019 ms`
  （readback 占墙钟 29.8%）；
- 本机 OPTION 复现：`Present 2528.294 ms / N=369`，`readback 2077.967 / hash 274.225 / sink 175.022 ms`
  （≈5.6 ms/帧）。

⇒ 回读是**每帧固定成本**，与场景无关；`DeleteObject` 那条（另一张单）修完后它就是最大的单项。

## 构成（读代码）

- `src/runtime/d3d9.cpp` 的 `present()`：`gpu_dirty` 时 `gpu->rasterSyncToGuest(...)`；
  `!dirty` 的帧由 sink `directUploadGuest` 把 guest 上传到 target ⇒ 「直出 + guest 镜像」结构上**每帧恰好一次整幅搬运**。
- `src/runtime/d3d9_gpu.cpp` 的 `rasterSyncToGuest` → `rasterFlush` + `downloadInto`：
  `SDL_DownloadFromGPUTexture` + **每帧一次阻塞 `SDL_WaitForGPUFences`** + **每帧新建/释放一张 3.5 MiB transfer buffer**
  + 逐行 memcpy + 逐像素清 X 字节。
- `hash`：`HeadlessPresenter::present` 是所有档的唯一汇点，guest 像素哈希在 sink 之前**无条件**算
  （`present.h` 的 `hash_ns` 注释里的 "HeadlessPresenter" 是类名遗留）——窗口档照付（约 0.74 ms/帧）。
- `overlay` **不增加**回读（`d3d9_gpu.cpp` 显式禁止叠层碰 target；`drawOverlay` 只吃宿主读数）。

## 候选优化（按语义风险排序；实施前要先裁决）

1. **零语义风险**：hash 合并进下载那一趟（同一份 guest 像素，省一次遍历）≈ 0.74 ms/帧（本机）。
2. **语义不变，需实测**：复用 download transfer buffer + 延迟/合并 fence 等待（现在每帧新建 tb、每帧阻塞等）；
   可能把「等」摊掉，但要注意与下一帧 GPU 队列的依赖。
3. **改 guest 可见语义（必须另开语义单）**：懒镜像 —— 只在真消费者（`arena.hash` / `--dump-frame` /
   `GetRenderTargetData` / lock）存在时读回。收益上限 = readback 全额，但「guest 后缓冲在 shim 边界权威」这条要重写。
4. ⛔ 脏矩形读回：本场景全屏重绘（`End` 每条 sprite），近零收益，不立项。

## 判据（observation，⛔ 不进 CTest）

- `LIFT_GPU_SUBMITS=1` 的 `[gpu-submits] readback=…` 每帧计数；四臂对照：窗口无 overlay / 窗口 + overlay /
  headless / vsync=1 ⇒ 证明 overlay 无关、swapchain 无关。
- 两个 draw 量不同的场景比 `readback ms/帧` ⇒ 分辨「等 fence」vs「拷贝」。
- 目标（实施时定）：同一 `scenario2.plan` 下 `Present` 从 5.6 ms/帧 降到什么量级 —— 先取候选 1/2 的读数再定，
  ⛔ 不先写一个数字进判据。

## 边界

- ⛔ 不为提速改 guest 可见语义（候选 3 走语义单）。
- ⛔ 不动 `DeleteObject` / 句柄表（另一张单）。
- 本单只到「候选 1/2 的读数 + 裁决」；没有读数前不排期。
