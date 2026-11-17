# GPU 渲染路线：从 fixed-function 到 shader / D3DX FX（S0 取证 · S1 容器与 COM · S2 shader 面 · S3 画面验收）

- id: REQ-01M7MNPGETP2CQJ3ACMCGEHKF5
- type: req
- status: open
- parent: REQ-01M7JHSD5MWTB9N8WJC2Y2T69M
- tags: [emulator-lift, d3d9, gpu, dx9]

## 一句话

从"固定功能 + 软件模糊"走到"**真正的 GPU 渲染**"（shader / D3DX FX 路径）。**形态 = 构建期预转写**
（`REQ-01M7MNYPDXM2NEQR77DXYZM3CJ`）：引擎会提交的 shader / effect 是**有限固定**集合 ⇒ 构建期转成 MSL/DXBC +
生成"字节码 → 管线"映射表，运行时只查表绑定；⛔ 不做运行时通用翻译器，⛔ 未收录字节码必须响亮停。

## 为什么（读数）

- caps=0 ⇒ 引擎走软件模糊：转场段每帧最高 20.4e9 条、单段 37.9–39.5e9 条 / 7–18 s ⇒ 渐变塌成一帧。
- caps=3.0 ⇒ 引擎在转场第一帧调 `D3DXCreateEffectFromResourceA`（callsite `0x004B0798`，资源 201 = 等级 2；
  等级 1 = 资源 200；另有 203）⇒ 分路确认。
- 现有 `--raster gpu`（SDL_GPU + MSL / DXBC）只覆盖**固定功能**的等价物（内置 `kShaderSource`），没有用户 shader / FX 面。

## 阶段（每段都要有可再校验判据；⛔ 不许"看起来像"）

- **S0 盘点与取证**（`REQ-01M7MNPNW0F999WZYD6ZPBD7TE`）：把**有限集合**点清 —— 全部 `D3DXCreateEffect*` / `CreateVertexShader` /
  `CreatePixelShader` 的调用点与它们拿到的字节码（资源号 / 哈希 / 大小 / shader 版本 / 内嵌几段）；`ID3DXEffect` 被调的方法；
  现有后端缺口清单。
- **S1 容器与 COM 面**：FX 容器**也在构建期拆**（pass/state/参数表 → 生成的 C++ 表）；
  `D3DXCreateEffectFromResourceA` 合同（9 参、失败码、按哈希查表、未收录 ⇒ 响亮停）+ `ID3DXEffect` 的最小方法集。
- **S2 shader 面**：对集合里每个 shader 做**离线转写**（MSL / DXBC），生成映射表；运行时只绑定常量表 + 提交。
- **S3 画面验收**：该转场段的模糊由 GPU 完成（单帧 guest 指令回到 1e6 量级）+ 与 CPU oracle 的逐像素对比（黄金图）；
  之后 caps 按**实测能力**报 PS/VS 版本（⛔ 不提前报）。

## 判据（父单收口时）

- caps 报的值与实现能力一致（守卫：报 PS≥2.0 ⇒ `D3DXCreateEffectFromResourceA` 已实现且不响停）。
- 同一段转场：单帧 guest 指令从 2e10 回到 1e6 量级，画面与 CPU oracle 差异在阈值内。
- 映射表覆盖**S0 点清的全部** shader；漏一个 ⇒ 启动期/首次使用时响亮停并点名哈希。

## 有限集合的第一次盘点（2026-11-17，S0 进行中；资源由 S0 agent 从 PE/资源链抽出，父级读内容）

四个 effect 资源**都是 FX 源文本（HLSL）**，不是编译后的二进制（都以 `//====` 注释开头，892–1,776 B）：

| 资源 | 内容 | 说明 |
|---|---|---|
| 200 | `FadePS` + `technique Fade` | 两张纹理按 `Alpha` 交叉淡化（`ret.a = c0.a*(1-Alpha) + c1.a*Alpha`）；引擎效果**等级 1** |
| 201 | `FadePS` + `technique Fade` | 同上；引擎效果**等级 2**（转场第一帧要的就是这个） |
| 202 | `SnowPS` + `technique Snow` | 雪粒子 |
| 203 | `SlideBlurPS` + `ZoomBlurPS`，`technique SlideBlur` / `ZoomBlur`（`compile ps_2_0`） | **横向滑窗模糊**（"横向模糊"就是它）；软件 `BlurTexture` 是它的 CPU 回退 |

⇒ 预转写的对象是 **4 个 effect / 5 个 ps_2_0 级 shader**，而且拿到的是**源码**（不需要反汇编字节码）：
S2 的工作量 = 把这 5 个 HLSL 逐份转成 MSL/DXBC + 每个 technique 的 pass 状态表；S1 = 资源号/哈希 → 表的映射 + `ID3DXEffect`
（`SetFloat("Alpha")` / `SetTexture("Tex0"/"Tex1")` / `Begin/Pass/End`）的最小子集。
