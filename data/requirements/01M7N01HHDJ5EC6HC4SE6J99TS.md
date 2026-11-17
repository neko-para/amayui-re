# P3 · 203 的 SlideBlur(21-tap)/ZoomBlur(10-tap) 真画（MSL+DXBC 双源）⇒ 转场从纯黑变成被模糊的塔

- id: REQ-01M7N01HHDJ5EC6HC4SE6J99TS
- type: req
- status: done
- parent: REQ-01M7MNPGETP2CQJ3ACMCGEHKF5
- verify: apps/emulator-lift/tests/d3dx9_effect.cpp#d3dx9-effect/blur-pixels
- tags: [emulator-lift, d3d9, shader]

## 一句话

203 的两个 technique（`SlideBlur` / `ZoomBlur`）**真画出来了**（MSL + DXBC 双源）；转场那段**从纯黑变成"被横向模糊的塔"**。
202（Snow，`vs_1_0`）继续具名拒绝。

## 前置读数：203 的 `Tex0` 来源（不撞反馈环）

`[this+0xA668]` = 纹理表 **slot 36**（基址 `0xA5D8 + 36*4`）= 引擎自建的**全屏 `D3DUSAGE_RENDERTARGET` 纹理**：
`sub_4A6D00 → sub_4A2C10(this,36,w,h,mode) → sub_48AB20`(NormalTexture) `+ sub_48AC40 → D3DXCreateTexture(device,w,h,1,usage=1,fmt 21/22,0)`。
引擎次序（`sub_4B06D0`）：`0x4B32AF SetTargetTexture(36)` 画进去 → `0x4B34C0 SetTargetTexture(v384[4])` **切走** →
`0x4B3625 SetTexture("Tex0",36)` → `0x4B36A3 sub_4A71B0` 全屏四边形 ⇒ **采样时是已完成的快照**。
既有 **bit43**（sprite 反馈环）闸保留且对 effect 纹理也查，实测 0 次触发。

## 实现要点

- 源文本是 **SlideBlur 21 tap**（`i=-10..+10` 含）×0.0476；**ZoomBlur 10 tap** ×`ratio[10]` ⇒ "10-tap" 只对 ZoomBlur 成立。
- `kFxEntries` 扩成 `{entry, samplers}`；pipeline 键步长 `*4 → *8`；Metal 没有 `radians()`（`MTLLibraryErrorCode=3`）⇒ 两源内联 `Angle*π/180`。
- 翻面：203 两条 binding ⇒ `recorded`；202 仍 `kUnEffectProgramNotExecuted`（名字/语义不变，原因串带 id/technique/pass）。

## 读数（2026-11-17 / mac arm64 / `links/raw` / prefix27）

- `ctest`：`d3dx9-effect` **608 checks / 0 failures**；全量 **107 项**（103 passed + 4 平台 skip）。
- 像素（真 GPU + 纹理 RT + 读回）：SlideBlur `Angle=0 ⇒ B=210 G=0 R=45`、`Angle=90 ⇒ B=255 G=45 R=0`、
  ZoomBlur ⇒ `B=159 G=0 R=96`，与照源文本的 C++ 参考 **0 LSB**（容差 1）。
- E2E：`没有实现执行` **1091 → 0**；`pending=0`；`implemented=299 gaps=11` 与 before 逐字相同。
- **黑 → 有内容**：`SN0000 pos=0xD190` 转场段 before 44 采样 = **1 个 hash（纯黑）**、after 25 采样 = **25 个 hash**；
  抽帧 before `uniq=1 / nonblack=0%`，after `uniq=8.2万–28.9万`（亮度 244→102 的斜坡）。
- 变异三条实测红（tap 21→20 = 12 LSB；uv 步长 ×10 = 72 LSB；program 映射 = 45 LSB + 纯档 `mock.last.program` 红）。

## 未做

DXBC 侧只有静态保证（本机只编 MSL）；202（Snow）仍拒；caps 缺省值与 `LIFT_D3D9_PS_VERSION` 的移除属 P4。
