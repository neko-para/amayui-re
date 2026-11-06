# emulator-lift · GPU 光栅化后端（--raster gpu；phase 1 未对齐）

- id: REQ-01M6PAXW7EFCERJR6G2EEDN2A0
- type: req
- status: done
- parent: REQ-01M6H6G6DSRG1ZRD810Z0HTH8V
- verify: apps/emulator-lift/CMakeLists.txt#gpu-raster-gpu
- done_reason: GPU 光栅 phase 1（无纹理 XYZRHW + D3DXSprite）已接；真实 boot 100 帧 CPU 103.7s→31.8s。注意：当时没有 fallback 计数，“GPU 覆盖全部 D3D9 Draw”不成立；TITLE 的全屏纹理 + separate alpha 直到 REQ-01M6RH9CDPM8PG2SFEG0PYT46T 才走 GPU（2026-11-06）。
- tags: [emulator-lift d3d9 gpu raster]

## 目标

给 D3D9 fixed-function 增加**可切换的 GPU 光栅化**（`--raster gpu`），与 CPU 软光栅二选一；
缺省仍 CPU。GPU 只做光栅，T&L/裁剪/状态机/失败码留在 CPU。

## 已实现（可复现）

- 后端：独立 `SDL_GPUDevice`（DXBC，D3DCompile 运行时编译），BGRA8 颜色 + D32 深度；
  屏幕空间三角形 → NDC；深度测试/写、SRCALPHA/INVSRCALPHA、scissor、viewport。
- D3D9 热路径：`submitPrimitives` 接无纹理 XYZRHW+DIFFUSE；`GPU Draw 200 / 三角形 800`（100 帧）。
- D3DXSprite：`compositeBatch` 接纹理四边形（NEAREST、顶点色调制、ALPHABLEND）；`pTexture=0`
  用 1×1 白纹理。这是此前 0.41 s/帧的瓶颈。
- 入口：`--raster cpu|gpu`，`--raster gpu` 支持 `--present headless`（dummy 视频 + 离屏）与
  `--present window`；缺省 `--raster cpu` 不初始化 SDL 视频。
- 守卫：`gpu-raster` / `gpu-raster-gpu`（14 checks）；`ctest` 49/49；CPU 默认逐字节不变。

## 实测（真实 boot，headless，无 --real-time，100 帧）

| 配置 | 100 帧总耗时 | 相对 CPU |
|---|---:|---:|
| CPU 光栅 + CPU sprite | 103.7 s | 1.00× |
| GPU 光栅 + CPU sprite | 71.5 s | 1.45× |
| GPU 光栅 + GPU sprite（当前） | **31.8 s** | **3.26×** |
| CPU、跳过 sprite（隔离用） | 60.3 s | — |
| GPU、跳过 sprite（隔离用） | 30.1 s | 2.00× |

⇒ 当前 GPU 稳态约 **0.32 s/帧**（含约 25 s boot 的一次性摊销）；剩余主要是一次性 boot，
D3D9 + sprite 的每帧光栅已接近“无 sprite”下限。

## 像素判据（用户 2026-11-06 口径：先不管一致性）

- 第 1 帧 GPU 与 CPU hash 完全一致（`0xEBF024FEBE998B05`）。
- 第 2 帧起差异 **max 1 LSB**（141,521 / 921,600 像素，全在 1 个通道），来自 GPU blend 舍入；
  无结构性错误。CPU 缺省仍逐字节可复现；GPU 模式按此偏差登记，不用 hash 当唯一判据。
- 依据：用户明确“先不要管像素一致性之类的不重要的内容”。

## 验收状态

1. 真实 boot 前 4 帧：max diff ≤1 LSB（已测；非 0，已登记）。
2. `GPU Draw > 0`、GPU 覆盖全部 D3D9 Draw 与 D3DXSprite 合成（已测 200 Draw / 100 帧）。
3. 100 帧边际帧时间相对 CPU **3.26×**（≥2× 达标）。
4. CPU 缺省、`ctest` 全绿（49/49）。

## 非目标

- 不做纹理 stage 链 / 16bpp / index buffer / FLAT / alpha test；这些回退 CPU（已按 draw 决策）。
- 不改 CPU 路径的像素判据；不改 DDraw。
