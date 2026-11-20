# P4 · BMP/pixel 收口（support/bmp + support/pixel）

- id: REQ-01M7W7SD3VJMYCANC9P61QMCJT
- type: req
- status: open
- parent: REQ-01M7W7RSC34YJTNMK3ESYVXZY2
- tags: [emulator-lift, tests, bmp]

## 做什么

- `aot_overlay` 的 `BmpPixel` 改 `support/bmp`（读 BMP 头 / 像素 / 行对齐）；
- `gpu_arm.h`、`d3d9_pipeline`、`d3dx9` 的通道差改 `support/pixel`（BGR↔RGBA 等）；
- `control_snapshot` 的**整文件逐字节相等保持**（那是"同一帧字节相同"的判据，不是像素比较）。

## 判绿

- 新增 `support-bmp` 自检（顶层 CTest）：2×2、24/32bpp、正/负高度、宽非 4 倍数、BGR 顺序。
- `ctest -R 'aot-overlay|control-snapshot|support-bmp'` 通过，且**像素期望零改动**（只换 helper，不改断言值）。

## 边界

- ⛔ 不把逐字节相等改成容差；⛔ 不改任何像素期望。
