# emulator-lift · GPU 合批不退化：脏矩形相交才切段 + 一帧只在读回/换 target 提交两次

- id: REQ-01M6X4KST5FJ3R4W06TRFZYCGR
- type: req
- status: done
- parent: REQ-01M6H6G6DSRG1ZRD810Z0HTH8V
- verify: apps/emulator-lift/tests/gpu_batch.cpp#gpu-batch/content-change
- done_reason: 三条判据：像素逐字节等价 + 每帧提交 2 次（1 画 + 1 读回）+ 脏矩形不相交不切段；守卫 gpu-batch/content-change 同时断言像素与提交计数，切段判据见 dirty-rect；未达 1–3 ms 的原因与「不做」的裁决见正文。
- tags: [emulator-lift, gpu, batch, m2]

## 交付（三条判据 + 两个顺带修掉的真 bug）

判据一：像素**逐字节等价**。核法：同一输入脚本跑两次、用 `--dump-frame <path>`（路径里的 `{frame}` 换成帧号）
导出同一帧，逐字节比对 / 算哈希 —— 判据是"逐字节相同"，不是"看起来一样"。
判据二：**提交计数 = 2/帧**（1 次画提交 + 1 次读回提交），⛔ 不是每条 draw 一次提交。
判据三：**脏矩形判据** —— 脏区与已入队 draw 的采样区**不相交 ⇒ 不切段**；相交、或写区未知 ⇒ 切段。
★ 切段 ≠ 提交：切段只多一个 command buffer 段，只有读回 / 换 target 才 Submit 一次。

两个顺带修掉的真 bug（都已有守卫，见下）：子矩形上传曾把矩形**外**的像素清空（只有整幅才允许 cycle）；
深度清除曾随每次颜色同步发生（现在只由 `Clear(ZBUFFER)` 与新 target 触发）。

## 守卫（都注册在 CTest 名 `gpu-batch` 下：`ctest -R gpu-batch`）

- `gpu-batch/content-change`（★ `verify` 指这条：它同时断言像素 oracle 与 `submits` 增量 = 2）：
  写红 → 画 → 写绿 → 画 ⇒ 左下仍红 / 右上绿。
- `gpu-batch/dirty-rect`：不相交 ⇒ 切段 0 次；相交 / 写区未知 ⇒ 切段；切段不许伴随提交。
- `gpu-batch/subrect-upload`：子矩形上传只动矩形内，矩形外的像素不变。
- `gpu-batch/depth-segments`：多段渲染下深度不被误清，且颜色同步不清深度。
- 注册位置 `repo:apps/emulator-lift/CMakeLists.txt`：`gpu-batch-order`（pure）/ `gpu-batch-barrier`（真 GPU 档，
  dummy 视频下 SKIP 77）。
- "它会红"怎么核：`repo:apps/emulator-lift/tests/gpu_batch.cpp` 每个用例的头注释都列了"判据会红的情形"，
  逐条改坏一处 ⇒ 对应用例必须红，再还原。

## 未达 1–3 ms 的原因与"不做"的裁决

- 残余成本**不在提交**：把纹理创建 / staging 上传搬出 `End` 只是把同一笔成本**挪位置**，一帧总量不变。
- 所以父级裁决：**不做**。本单证据只支持"提交次数不再随 draw 数增长"，⛔ 不支持也不声称装载帧总耗降到 1–3 ms。
- 现状怎么复核：看 GPU 统计里 `submits` / `cuts` / `uploads` 各自的增量，另把纹理创建与上传单独计时；
  要让总耗继续降，得先证明那两笔成本能真的变小（而不是搬家）。

## 不在本单

- TITLE 的 wall-clock 目标仍由 `REQ-01M6RQ2XNWRDV35E72D7G2ZV7D` 跟踪；本单不宣称它到目标。
- 不做把创建/上传搬出 `End` 的"优化"；不做多 stage / 16bpp / alpha test / 深度镜像等扩展。
