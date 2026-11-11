# emulator-lift · GPU 黄金图回归集（不依赖 CPU oracle 的画面回归）

- id: REQ-01M73G49T01W1CQDSME83ES0YD
- type: req
- status: done
- parent: REQ-01M6H6G6DSRG1ZRD810Z0HTH8V
- verify: apps/emulator-lift/tests/golden_image.cpp#golden/compare-one-block-fails
- tags: [emulator-lift gpu golden-image test]

## 一句话
按新口径（`docs/00-origin/decisions.md` §8.1：CPU=oracle / GPU=product），GPU 需要一条**不依赖 CPU** 的回归：
给 3–4 个 fixture 产出**黄金图**，GPU 与"上一次被接受的画面"比。今天没有任何东西能做到这件事。

## 现状（缺口）
- `--gpu-strict` 只断言"零回退"，**不判像素**；
- `LIFT_GPU_VERIFY` 是**调试**工具：比的是 CPU，且只覆盖"带纹理的批 + 只比颜色"（`d3d9_pipeline.cpp:3541-3576`）；
- ⇒ 只要不崩、不回退，**画错了也没有任何东西会红**（TITLE 那块矩形就是这么漏过来的）。

## 做法
1. fixture（各一帧）：`TITLE 首帧` · `固定功能覆盖层（fade 黑幕）` · `sprite 批` · `渲染到纹理（SetRenderTarget 到纹理后再 sprite）`。
2. 产出：`--present-screenshot` 或 `--dump-frame` 的 PNG，**入库走 LFS**，作为"已接受画面"。
3. 判据：逐像素 `max_channel_diff ≤ 容差`（跨驱动允许 ≤1 LSB；容差写进用例名/注释，⛔ 不要断言"当前 hash"）。
4. 注册：**非 pure**（要 GPU 设备）；不可用时 `t.skip` 并**打印原因**（⛔ 不许静默通过）。

## 守卫
- 黄金图回归 CTest：GPU 可用 ⇒ 与黄金图比；不可用 ⇒ SKIP + 原因。
- 把某张纹理的上传"少写一行"⇒ 该用例必须红（用变异实测，别只声称）。

## 未做
- 不做 CPU↔GPU 全量逐像素（已由口径取消）。
- 不追求跨驱动**字节级**一致（只做容差）。
- 不覆盖非确定性场景（`--real-time` / 输入脚本时序）。
