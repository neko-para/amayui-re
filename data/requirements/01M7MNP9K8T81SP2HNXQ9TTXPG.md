# ADR · 渲染目标 = 真正的 GPU 渲染（shader / D3DX FX 路径）；caps 报 0 只是当前能力，不是终点

- id: REQ-01M7MNP9K8T81SP2HNXQ9TTXPG
- type: decision
- status: done
- done_reason: ADR 本身即交付物：裁决（渲染目标 = 真 GPU 渲染，caps 报 0 只是当前能力）与依据（PS=0 时转场每帧 20.4e9 条 / caps=0xFFFF0300 后第一帧调 D3DXCreateEffectFromResourceA）已在正文定稿；其“实现后按真实能力报 caps”的后果已由 P4（REQ-01M7MXHYR1M72JW77J136GT3ZG，done）落地。
- tags: [emulator-lift, d3d9, gpu, dx9]

## 裁决

**渲染的目标形态 = 真正的 GPU 渲染（shader / D3DX FX 路径），不是"软件路径 + caps 报 0"。**
⇒ `GetDeviceCaps` 的 `PixelShaderVersion` / `VertexShaderVersion` 报 0 只是**当前实现能力**的如实反映，
⛔ 不是产品的终点形态；也不接受"为了画面像而临时把 caps 调高"（那会让引擎撞在未实现导入上，什么也测不了）。

## 依据（用户 2026-11-17 口径 + 本轮读数）

- 用户："预期要实现真正的 GPU 渲染"。
- 单变量实验：caps 报 PS/VS=0 ⇒ 引擎走软件模糊（`BlurTexture` 整数滑窗），转场段每帧最高 **20.4e9 条** guest 指令、
  单段合计 37.9–39.5e9 条 / 墙钟 7–18 s ⇒ 渐变塌成一帧（`REQ-01M7MJNGGWZKJ0SJKSPWNPSDBQ`）。
- 把 caps 改成 `0xFFFF0300` 后，引擎在**转场第一帧**调 `D3DXCreateEffectFromResourceA`
  （callsite `0x004B0798`，资源 201 = 效果等级 2，`Engine+0xB590` 只建一次）⇒ 分路由 caps 决定。
- ⇒ 真机（报 shader 支持的 GPU）走 FX 路；我们要复现的是那条路。

## 后果（口径）

1. `src/runtime/d3d9.cpp:520` 报 0 的那两行**不是永久裁决**；实现 shader/FX 之后要按真实支持能力报。
2. 在 shader/FX 落地前，caps 保持 0，且"软件路径的可见差异"（模糊形状/耗时/渐变节奏/音画不同步）必须**显式登记**为已知偏离，
   ⛔ 不许当成渲染 bug 去改时钟或改预算凑。
3. 分阶段落地，每段都要有可再校验的判据（守卫或读数）；路线图在 `REQ-01M7MP…`（父单）下。
