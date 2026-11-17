# caps 报 PixelShaderVersion=0 ⇒ 引擎走软件模糊（每帧 2e10 条，真机不可能）：实现 D3DX FX 路径还是接受差异（待用户裁决）

- id: REQ-01M7MNJ5V5FR759S6KWVJWGREY
- type: req
- status: done
- parent: REQ-01M7JHSD5MWTB9N8WJC2Y2T69M
- done_reason: 已由 ADR REQ-01M7MNP9K8T81SP2HNXQ9TTXPG 裁决：走真正的 GPU 渲染（shader / D3DX FX 路径）；路线与阶段在 REQ-01M7MNPGETP2CQJ3ACMCGEHKF5，取证在 S0 子单
- tags: [emulator-lift, d3d9, fx, caps]

## 一句话

我们 `GetDeviceCaps` 报 **PixelShaderVersion / VertexShaderVersion = 0**（`src/runtime/d3d9.cpp:520`，
注释自述"本线没有 shader"）⇒ 引擎走**软件模糊**（`BlurTexture` 的整数滑窗，每帧最高 20.4e9 条，
见 `REQ-01M7MJNGGWZKJ0SJKSPWNPSDBQ`）。**20e9 条/帧在真机上也跑不赢** ⇒ 真机必然走的是另一条路。

## 实验（2026-11-17，一次性，已还原）

只把 caps 的 PS/VS 版本改成 3.0（`D3DPS_VERSION(3,0)` = `0xFFFF0300`），其余不变，跑 `links/raw` 的同一段：

| | caps = 0（现状） | caps = 3.0（实验） |
|---|---|---|
| `D3DXCreateEffectFromResourceA` | **0 次**（600.60 s / 14,122 帧 / 843.9e9 条跑满） | **12.74 s 就调到**（`stop: unimplemented @ eip=7FFE0060`，`insns=5.73e9 / frames=1525`） |
| callsite / 实参 | — | `0x004B0798`；资源 **201** = 效果等级 2；`Engine+0xB590` 只建一次 |

⇒ **分路确认（单变量、有 EA 链）**：`GetDeviceCaps` 的 `D3DCAPS9.PixelShaderVersion`（caps 缓冲 `[ebp+var_140]`
与门 `[ebp+var_74]` 相差 `0xCC`）决定引擎去不去建 effect。
⚠ 证据强度要分清：**"分路"是已证**；"FX 路取代软件模糊"是**同帧推论**（改后 run 在建 effect 那帧就停了，
看不到替代过程）；"真机走 FX 路"是**推断**（本段单帧 2e10 条 guest 指令真机也跑不赢），**没有真机读数**。

## 要裁决的事（⛔ agent 不能替用户拍板）

1. **实现 FX/shader 路径**（`D3DXCreateEffectFromResourceA` + 执行 FX 字节码 + 像素着色）—— 大单，
   但只有这样才和真机的渲染路径一致（本段"横向模糊"才会是 GPU 语义，不再有 2e10 条/帧）。
2. **接受软件路径**（caps 保持 0）：画面与真机**不同**（模糊的形状/耗时/渐变节奏都不同），需要在文档里写成显式裁决，
   并且"渐变塌成一帧"就不再是缺陷而是 caps 的后果。

未裁决前：⛔ 不要为了让画面像而临时改 caps（会让引擎撞在未实现导入上）。

## 判据（收口时）

- 选 1 ⇒ 有 `D3DXCreateEffectFromResourceA` 的合同用例（合法/非法参数、返回码）+ 至少一条 FX 程序的执行读数；
  那一段不再有 2e10 条级的单帧计算。
- 选 2 ⇒ `docs/03-emulator/` 里有一条显式裁决（caps 不报 shader 的理由 + 与真机的可见差异清单），
  并且派生单（渐变塌陷）按"不是缺陷"收口。
