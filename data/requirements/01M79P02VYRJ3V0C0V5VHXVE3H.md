# emulator-lift · 声明式 Draw（Live2D 路径）没有画像素的守卫

- id: REQ-01M79P02VYRJ3V0C0V5VHXVE3H
- type: req
- status: open
- parent: REQ-01M6H6G6DSRG1ZRD810Z0HTH8V
- tags: [emulator-lift, d3d9, guard, live2d]

## 缺口

Live2D 的**声明式 Draw**（`IDirect3DVertexDeclaration9` 绑定 + 两条 stream + `DrawIndexedPrimitive`）已经实现
（`apps/emulator-lift/src/runtime/d3d9_pipeline.cpp` 的 `declLayoutOf` / `buildDeclVertices`），
但**没有一条守卫断言它真的画出了像素** —— 今天 17 个 `d3d9p/*` / `gpu-batch/*` 用例里没有 decl 用例。

## 为什么要单开

它是 M3（Live2D）能显示的唯一 draw 形态；一旦回归，表现是"模型整层消失"，而现有用例全绿（== 假绿口子）。

## 判据

- 新 CTest `d3d9p/vertex-decl-draw`：用声明（POSITION FLOAT3 @stream0 + TEXCOORD FLOAT2 @stream1）画一个覆盖已知区域的三角形，
  `--dump-frame` 或读回后断言该区域**非背景色**（粗粒度），且 `fallback_draws == 0`（⛔ 不许靠 CPU 回退变绿）。
- 守卫必须在"`drawPrecheck` 对 `kPlVertexDecl != 0` 直接失败"的旧行为下红。
- 变异：把 `buildDeclVertices` 的 stream 1 偏移写错（+1 字节）⇒ 颜色采样错 ⇒ 该用例红。

## 非目标

- 不扩多 stream / 多 UV 集 / 骨骼蒙皮：引擎未用到，用到时另开单。
