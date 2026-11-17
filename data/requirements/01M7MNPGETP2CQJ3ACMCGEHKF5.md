# GPU 渲染路线：从 fixed-function 到 shader / D3DX FX（S0 取证 · S1 容器与 COM · S2 shader 面 · S3 画面验收）

- id: REQ-01M7MNPGETP2CQJ3ACMCGEHKF5
- type: req
- status: open
- parent: REQ-01M7JHSD5MWTB9N8WJC2Y2T69M
- tags: [emulator-lift, d3d9, gpu, dx9]

## 一句话

从"固定功能 + 软件模糊"走到"**真正的 GPU 渲染**"（shader / D3DX FX 路径）。**形态 = 构建期预转写**
（`REQ-01M7MNYPDXM2NEQR77DXYZM3CJ`）：引擎会提交的 shader / effect **有限且固定** ⇒ 构建期转成 MSL/DXBC +
生成"资源号/哈希 → 管线"映射表，运行时只查表绑定；⛔ 不做运行时通用翻译器，⛔ 未收录字节码必须响亮停。

## 有限集合（S0 已点清，2026-11-17）

容器 = `AGE.EXE__dumped.sectfix.EXE` 的 PE `.rsrc` → **RT_RCDATA**（恰好 4 个：200/201/202/203，lang 0x411），
**都是 ASCII HLSL 源码**（CRLF、CP932 注释、以 `//====` 开头；FX magic `0xFEFF0901` / `DXBC` / `CTAB` / 版本 token **0/4 命中**）。
⚠ 只能从**拆壳件**取字节：原版 `links/raw/AGE.EXE` 的 `.rsrc` raw 只有 0xE00，数据 RVA 0x1AD4A0 超出。
⛔ 不要再写"202 是编译过的 FX 二进制"（`platform-effort.md` 已改）。

| 资源 | 大小 | 内容 | 目标 |
|---|---|---|---|
| 200 | 892 B | `FadePS` + `technique Fade`（两级纹理按 `Alpha` 交叉淡化） | `compile ps_1_0` |
| 201 | 1,077 B | 同 200（引擎效果**等级 2** 用的就是它） | `compile ps_2_0` |
| 202 | 899 B | `SnowPS` + `technique Snow` | `vs_1_0 + ps_1_0` |
| 203 | 1,776 B | `SlideBlurPS` / `ZoomBlurPS`（**横向滑窗模糊**，10-tap） | `compile ps_2_0` |

## `ID3DXEffect` 的实际调用面（S0 钉死；槽号与 `d3dx9effect.h` 布局逐位吻合）

`+0x08 Release` · `+0x78 SetFloat` · `+0xD0 SetTexture` · `+0xE8 SetTechnique` · `+0xFC Begin` · `+0x100 BeginPass` ·
`+0x104 CommitChanges` · `+0x108 EndPass` · `+0x10C End`。
未读到（⛔ 别按"应该"实现）：`GetDevice +0x110` / `OnLostDevice +0x114` / `OnResetDevice +0x118`。

## 现有后端的能力边界（S0）

顶点全在 CPU（`d3d9_pipeline.cpp` 的 `transformVertex` / `applyLighting` / 裁剪 / `toScreen`）；GPU 只做屏幕空间三角形 +
**6 个固定片元置换**（`d3d9_gpu.cpp` 的 DXBC / MSL 源）。明确不支持 ⇒ **整批拒绝**（0 像素 + 具名位）：
alpha test(bit30) · FLAT(31) · **stage≥1(33)** · 多 bpp(36) · 寻址(37) · 过滤(38)。设备 vtable 里**没有**
`Create/SetVertexShader`(+0x16C/+0x170) 与 `Create/SetPixelShader`(+0x194/+0x198) ⇒ 走到就是 `comUnsupported` 响停。
★ 注：`--raster gpu` 开关**已删**（GPU 是唯一光栅，没有开关）。
★ 注：effect 指针存的是 `this(0x100504F0)+0xB590`，**不是** marker 的 `Engine`。

## 阶段（每段都要有可再校验判据；⛔ 不许"看起来像"）

- ✅ **P0**（已收口，子单 `REQ-01M7MSGRJCEPAE8NS323WEG0SG`）：`PeResources` 读 `RT_RCDATA` + pure 用例钉 4 份字节与 sha256；
  `d3dx9-effect/resources-pinned`。
- ✅ **P1**（已收口，同子单）：导出 9 参 + 9 槽 `ID3DXEffect` 只记状态；E2E `exit 0 / pending=0 / unimplemented=0`；
  ★ 实测**推翻**了"effect draw 会被 stage≥1 拒绝"的预期（203 只绑 Tex0 ⇒ bit33 不触发）⇒ 新增临时具名位 **44**
  `effect shader 尚未实现执行`，每批一条 warn（E2E 1195 批）。P2 要把它翻面。
- **P2**：Fade(201) 的 MSL + DXBC(SM5.1) 双源 + 后端"每 draw 一个 shader + 常量 + 2 纹理" + 只对 effect draw 开口子。
  判据：两张已知纹理 + `Alpha∈{0,0.5,1}` 的像素 oracle；账本 `recorded` 非 `skipped`。
- **P3**：203 两个 technique（10-tap）。★ 价值重述（2026-11-17 探针）：**画面正确性**（effect 层今天整层被丢弃），
  ⛔ 不再是"换 insns 收益"—— 该窗口的 2e10/帧是 **caps=0（等级 0）** 走软件模糊的产物，等级 ≥2 时软件模糊 0 次调用。
  判据：该 technique 的 draw 被 `recorded` 且像素与参考一致（不再挂"单帧 insns 降到 1e6"）。
- **P4**：caps 报 `PS=0xFFFF0200` / `VS=0` + "声明⇔行为"守卫。**必须 P3 之后**（提前报 2.0 会同时点亮 203）。

## 风险 / 待核（⛔ 别当已核）

- **NaN**：`ColRate` 的 `0/0`（两图全透明）在 ps_2_0 与 Metal clamp 下不保证一致 ⇒ P2 必须有该用例把行为钉住。
- **203 的 `Tex0` 来源未读**（`[this+0xA668h]` 包装 +0x20）：若是"后缓冲快照"就撞反馈环（bit28/43）—— P3 最大未知，先读。
- **level 还有第 2 入口**：`0x4A6FA2` 的 `arg_4 ∈ [-1..2]` 且 ≠ -1 时直接覆盖 `[this+0xB64C]` ⇒ "caps=2.0 ⇒ 等级一定 2"有例外。
- P1/P2/P3 的 E2E 验证都需要"临时让引擎走 FX 路"（caps 默认为 0 ⇒ 走不到）。需要一个**显式 opt-in 的开发开关**（例如
  `LIFT_D3D9_PS_VERSION`），缺省关闭、启动读数一行、且默认输出逐字节不变；⛔ 不许把 caps 缺省改高（那会让未实现路径静默点亮）。

## 判据（父单收口时）

- caps 报的值与实现能力一致（守卫：报 PS≥2.0 ⇒ `D3DXCreateEffectFromResourceA` 已实现且不响停）。
- 同一段转场：单帧 guest 指令从 2e10 回到 1e6 量级，画面与 CPU oracle 差异在阈值内。
- 映射表覆盖 4 个资源；漏一个 ⇒ 响亮停并点名 id/哈希。
