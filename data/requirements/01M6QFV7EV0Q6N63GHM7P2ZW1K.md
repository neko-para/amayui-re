# emulator-lift · GPU 光栅化 macOS 后端（SDL_GPU/MSL）

- id: REQ-01M6QFV7EV0Q6N63GHM7P2ZW1K
- type: req
- status: done
- parent: REQ-01M6H6G6DSRG1ZRD810Z0HTH8V
- verify: apps/emulator-lift/CMakeLists.txt#gpu-raster-gpu
- tags: [emulator-lift, gpu, raster, macos, metal]

# 交付

`--raster gpu` 在 macOS 上可用：SDL_GPU 设备走 **Metal**，shader 用 **MSL 源码**，
与 Windows 的 D3DCompile → DXBC 并列；两条路都必须真的画三角形 / D3DXSprite 纹理四边形并读回 guest 后缓冲。

## 为什么

现状是 `d3d9_gpu.cpp` 只在 `_WIN32` 下建 `SDL_GPU_SHADERFORMAT_DXBC` 设备，
macOS 走到 `createSdlGpuRaster()` 直接失败（`--present window --raster gpu` 打不开）。
SDL3 已带 Metal 后端，缺的只是平台 shader 与格式选择，不需要引入原生 Metal 框架。

## 判据

- `ctest --test-dir apps/emulator-lift/out -R gpu-raster-gpu` 在 macOS 上通过（不是 SKIP）。
- 测试同时覆盖无纹理三角形与纹理四边形；纹理断言检查 sampler + 顶点色调制后的 RGB。
- 完整启动：`--present window --raster gpu --max-frames 10` 日志出现
  `raster : sdl-gpu/metal/MSL`，且没有 `GPU ... 失败 / 回退 CPU`。
- `--raster cpu` 的缺省路径与逐字节判据不变；GPU 只是可选 sink。
- 诊断名不许被 renderer 名覆盖（`--raster gpu` 时 `rasterName()` 必须是实际后端）。

## 落点

- `repo:apps/emulator-lift/src/runtime/d3d9_gpu.cpp`：按平台选 shader 格式；MSL 顶点/片段入口；
  公共 sampler 创建。
- `repo:apps/emulator-lift/src/runtime/present_sdl.cpp`：`SdlWindow::rasterDrawTextured` 必须转发给
  GPU 后端，不许再固定返回“不支持”。
- `repo:apps/emulator-lift/tests/gpu_raster.cpp`：纹理路径守卫；无 GPU 设备时 SKIP 77。
- `repo:apps/emulator-lift/README.md`：平台口径（Windows=DXBC / macOS=MSL）。

## 不做

- 不直接调 Metal / AppKit；仍由 SDL_GPU 抽象。
- 不为 Linux 增加 SPIR-V 后端（当前不在本单范围）。
- 不把 DXBC 当 Metal 的输入，也不在运行期做 HLSL→MSL 转译。
- 不改变 CPU 光栅的缺省语义与 A4 可复现口径。
