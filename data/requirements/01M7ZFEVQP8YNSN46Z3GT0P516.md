# D3D9 点/线的像素选择（floor vs floor(c+0.5)）与 clipScreen 的半像素：缺像素 oracle，未取证

- id: REQ-01M7ZFEVQP8YNSN46Z3GT0P516
- type: req
- status: open
- parent: REQ-01M7JHSD5MWTB9N8WJC2Y2T69M
- tags: [emulator-lift, d3d9, raster, unverified]

## 一句话

D3D9 半像素（`BUG-38` / 提交 `a46a929`）收口时**同族的三处残余**留在光栅器里：`expand_point` / `expand_line` 选哪个像素仍不是 D3D9 的像素中心口径、`clipScreen` 比 D3D9 的像素范围左 / 上各紧半像素。**现在没有复现件** ⇒ 本单只定"要什么证据才能改"，⛔ 不是「顺手改一下」。

## 现状（代码事实）

- `repo:apps/emulator-lift/src/runtime/d3d9_pipeline.cpp` 的 `expand_point`：取 `floor(sv.x)` / `floor(sv.y)` 当该像素的左上角；D3D9 口径应是 `floor(c+0.5)`（像素中心在整数坐标 ⇒ 点坐标 c 覆盖像素 `c±0.5`）。
- 同一处的 `expand_line`：端点对齐 `floor(c)+0.5`（像素中心格）—— 与 `expand_point` 用的是**两套**口径。
- `clipScreen`（同一文件；现在是 `[vp, vp+size]`）：D3D9 的像素覆盖范围是 `[vp−0.5, vp+size−0.5]` ⇒ 我们的左 / 上各紧半像素。
- 影响面**未取证**：只影响落在像素边界附近（小数部分 ≈ 0.5）的点 / 线，以及贴着视口左 / 上边的图元。轴对齐 1 px 的覆盖不变 ⇒ 现有 `d3d9-gpu` / `gpu-batch-*` 全绿**不构成"没问题"的证据**。

## 要什么证据才能改（准入）

一条 **GPU 像素 oracle**（形状同 `d3d9p/rt-roundtrip-identity`：数学必然、不靠"看着对"）：

1. 用 MSDN 的点 / 线光栅化规则（`PointSize` / 线的像素中心与覆盖规则）给出**期望像素**，至少覆盖两个边界样本：坐标小数部分 `0.5`（如 `x=8.5`）与 `0.5±ε`（如 `8.4999` / `8.5001`）；
2. 期望值要有**独立来源**（CPU 参考实现或 D3D9 真机像素），⛔ 不能从我们现在的实现反推；
3. 再让 GPU 臂逐像素比对，并证明改前红 / 改后绿（变异条目同期加）。

⚠ 没有这条 oracle 之前**不动代码**：改它就是改 guest 可见语义（`AGENTS.md` §12.8 / §6 准入门）。

## 复现（现状 = 全绿，说明这族没被踩到）

```bash
cd apps/emulator-lift
ctest --test-dir out -R '^(d3d9-gpu|d3d9-pipeline-gpu|gpu-batch-order|gpu-batch-barrier)$' --output-on-failure
```

本单要的是**边界附近**的新用例（上面第 1–3 条），⛔ 不是让现有用例变红。

## 待核 / 边界

- 同族已 done 的那条 = `REQ-01M7ZFF3CXRB0Y6PJ119DFJZK4`（BUG-38：屏幕→NDC 的 +0.5，已修；本单是它未覆盖的三处）。
- 右 / 下边那半像素差是否可见**未取证**（本次只说左 / 上）。
- overlay 混合路（宿主 UI）**刻意**保持边界口径 ⇒ 不算进本单。
