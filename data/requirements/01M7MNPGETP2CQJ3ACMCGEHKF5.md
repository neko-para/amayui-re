# GPU 渲染路线：从 fixed-function 到 shader / D3DX FX（S0 取证 · S1 容器与 COM · S2 shader 面 · S3 画面验收）

- id: REQ-01M7MNPGETP2CQJ3ACMCGEHKF5
- type: req
- status: open
- parent: REQ-01M7JHSD5MWTB9N8WJC2Y2T69M
- tags: [emulator-lift, d3d9, gpu, dx9]

## 一句话

从"固定功能 + 软件模糊"走到"**真正的 GPU 渲染**"（shader / D3DX FX 路径）：目标是 caps 如实报到 PS/VS ≥ 2.0、
引擎走 `D3DXCreateEffectFromResourceA` 的 FX 路，转场段的模糊由 GPU 完成（不再有 2e10 条/帧的软件滑窗）。

## 为什么（读数）

- caps=0 ⇒ 引擎走软件模糊：转场段每帧最高 20.4e9 条、单段 37.9–39.5e9 条 / 7–18 s ⇒ 渐变塌成一帧。
- caps=3.0 ⇒ 引擎在转场第一帧调 `D3DXCreateEffectFromResourceA`（callsite `0x004B0798`，资源 201）⇒ 分路确认。
- 现有 `--raster gpu`（SDL_GPU + MSL / DXBC）只覆盖**固定功能**的等价物，没有 shader / FX 面。

## 阶段（每段都要有可再校验判据；⛔ 不许"看起来像"）

- **S0 取证**：effect 资源 200/201/203 的格式与取法 · `ID3DXEffect` 的调用面 · 现有 GPU 后端缺在哪几处。
- **S1 容器与 COM 面**：`D3DXCreateEffectFromResourceA` 合同（9 参、失败返回码）+ `ID3DXEffect` 的最小方法集（创建/Pass/Begin/End）。
- **S2 shader 面**：shader 字节码（版本待 S0 钉）→ 后端（MSL/DXBC）的翻译 + 常量表绑定；caps 按真实能力报。
- **S3 画面验收**：该转场段的模糊由 GPU 完成（单帧 guest 指令回到 O(1e6) 量级）+ 与 CPU oracle 的逐像素对比（黄金图）。

## 判据（父单收口时）

- caps 报的值与实现能力一致（有一条守卫：报 PS≥2.0 ⇒ `D3DXCreateEffectFromResourceA` 必须已实现且不响停）。
- 同一段转场：单帧 guest 指令量从 2e10 回到 1e6 量级，且画面与 CPU oracle 的差异在阈值内。
