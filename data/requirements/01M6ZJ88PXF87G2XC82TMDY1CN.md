# 🐞S2 emulator-lift · --raster gpu 的 D3DXSprite 路径把大背景画成横向条带（CPU sprite 正确）

- id: REQ-01M6ZJ88PXF87G2XC82TMDY1CN
- type: bug
- status: done
- parent: REQ-01M3TC7BK0J9TBQKGGWQ35A3NR
- verify: apps/emulator-lift/tests/gpu_batch.cpp#gpu-batch/unaligned-row-pitch
- repro: docs/03-emulator/defects.md#BUG-17
- done_reason: 根因：纹理上传的 staging 行距按 256 字节对齐，而 SDL_GPU/Metal 按目标区域宽度算源行距（pixels_per_row 的富余行距不生效）⇒ 每行错开 (对齐后−w*4) 字节，整张纹理被削成横条；w*4 是 256 倍数的纹理（1280/1664/2048）不可见，1120x672 的角色层必现。修法：staging 改紧密排列（row_pitch=rect.w*4）+ pixels_per_row/rows_per_layer=0（对齐交给 SDL）。守卫 apps/emulator-lift/tests/gpu_batch.cpp#gpu-batch/unaligned-row-pitch：40x20 纹理+逐像素图案+1:1 采样，实测修前 800 像素中 760 个不同、修后 0 个；--raster-sprites 缺省已恢复 1，EU-ROOM 帧 sha 与 CPU sprite 帧相同。缺陷史见 docs/03-emulator/defects.md 的 BUG-17。
- tags: [emulator-lift, gpu]

## 现象

`--raster gpu` 且 **D3DXSprite 走 GPU**（`--raster-sprites 1`）时，进 `OPTION` / `EU-ROOM` 后大背景被画成
**横向条带 / 撕裂**；同一帧的小 UI 面板正常。同一次运行把 sprite 交给 CPU（`--raster-sprites 0`）画面**完全正确**。

## 已排除（逐条实测，别再重复查）

- **不是回归**：在 `2bd8eaf`（`5e4fca3`/`8141659` 之前）重建 ⇒ 撕裂帧**逐字节相同**（sha256 `d0e0af01…`）。
- **不是 CPU/GPU 光栅不等价**：`LIFT_GPU_VERIFY=1` 逐批对照 42 批**全部 diff_pixels=0**。
- **不是纹理上传**：GPU 纹理读回 vs guest 源像素**逐字节相同**（1280x720 / 1664x1536 / 1280x1664 / 2048x2048 都比过）。
- **不是脏区/版本缓存**（强制整幅重传）、**不是合批/屏障**（逐条 flush、每帧 flush+读回再上传）。
- **不是 blend 开关**（关掉后 sprite 变不透明黑块 ⇒ blend 生效且语义正常）。
- **不是几何/UV**（日志里 quad / uv / 纹理尺寸 / serial 全对：全屏 quad + uv 0..1）。

⇒ 剩下的是 **GPU sprite 路径与 pipeline 路径的差异点**，未定位。

## 判据（修好后）

`--raster-sprites 1` 跑 `repo:apps/emulator-lift/scripts/interaction/room-enter.graph` 的帧，与
`--raster-sprites 0` 的帧**逐字节相同**（`sha256` 相等）；OPTION 同样。

## 现状（临时处置）

缺省已改成 `--raster-sprites 0`（取正确的一侧）；`1` 保留给复现 / A-B。
⛔ 不要在没定位前把缺省翻回 1。

## 读数

见 `repo:docs/03-emulator/defects.md` 的 BUG-17（含两条 sha256 对照）。
