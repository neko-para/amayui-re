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
★ A 单新增确认：202 的 `Color` / `Proj` 走 **`+0x88 SetVector` / `+0x98 SetMatrix`**。

## 现有后端的能力边界（S0）

顶点全在 CPU（`d3d9_pipeline.cpp` 的 `transformVertex` / `applyLighting` / 裁剪 / `toScreen`）；GPU 只做屏幕空间三角形 +
**6 个固定片元置换**（`d3d9_gpu.cpp` 的 DXBC / MSL 源）。明确不支持 ⇒ **整批拒绝**（0 像素 + 具名位）：
alpha test(bit30) · FLAT(31) · **stage≥1(33)** · 多 bpp(36) · 寻址(37) · 过滤(38)。设备 vtable 里**没有**
`Create/SetVertexShader`(+0x16C/+0x170) 与 `Create/SetPixelShader`(+0x194/+0x198) ⇒ 走到就是 `comUnsupported` 响停。
★ 注：`--raster gpu` 开关**已删**（GPU 是唯一光栅，没有开关）。
★ 注：effect 指针存的是 `this(0x100504F0)+0xB590`，**不是** marker 的 `Engine`。
★ 202 的 `vs_1_0` 由 **CPU 程序**兑现（A 单 `REQ-01M7NAW2QT3TG9RCFMKR052C83`）；设备 VS 槽仍未实现 ⇒ 被调用照旧 `comUnsupported`。

## 阶段（每段都要有可再校验判据；⛔ 不许"看起来像"）

- ✅ **P0**（已收口，子单 `REQ-01M7MSGRJCEPAE8NS323WEG0SG`）：`PeResources` 读 `RT_RCDATA` + pure 用例钉 4 份字节与 sha256；
  `d3dx9-effect/resources-pinned`。
- ✅ **P1**（已收口，同子单）：导出 9 参 + 9 槽 `ID3DXEffect` 只记状态；E2E `exit 0 / pending=0 / unimplemented=0`；
  ★ 实测**推翻**了"effect draw 会被 stage≥1 拒绝"的预期（203 只绑 Tex0 ⇒ bit33 不触发）⇒ 新增临时具名位 **44**
  `effect shader 尚未实现执行`，每批一条 warn（E2E 1195 批）。P2 要把它翻面。
- ✅ **P2**（已收口，子单 `REQ-01M7MSGRRHCA23RAX1QGDAYJ3X`）：Fade(201) 的 MSL + DXBC(SM5.1) 双源 + 每 draw 一个 shader/常量/2 纹理；
  像素 oracle 1 LSB；NaN 档「不可观测」；翻面**按 technique 分流**（Fade recorded / 203·202 仍具名拒绝）。
  判据：两张已知纹理 + `Alpha∈{0,0.5,1}` 的像素 oracle；账本 `recorded` 非 `skipped`。
- ✅ **P3**（已收口，子单 `REQ-01M7N01HHDJ5EC6HC4SE6J99TS`）：203 的 `SlideBlur`（**21-tap**，源文本如此）/`ZoomBlur`（10-tap）双源真画；
  转场从纯黑变成"被横向模糊的塔"（`pos=0xD190` 段 before 1 个 hash / after 25 个）；像素与参考 **0 LSB**；202 仍具名拒绝。
  ★ 判据随探针重述：该窗口的 `insns` 收益**不是** P3 的价值（软件模糊在 caps=0 才跑），价值是**画面正确性**。
- ⬜ **A**（子单 `REQ-01M7NAW2QT3TG9RCFMKR052C83`）：202 的 `SetMatrix/SetVector` + CPU `vs_1_0` + `ps_1_0 SnowPS` + binding 202。
  **A 是 P4 的前置**；⛔ 不接受"把 202 登记成已知缺失"来绕过。
- ⬜ **B**（子单 `REQ-01M7NC21S484PFYDAE7YTNX02J`）：GPU 批的**透视校正插值**（逐顶点 `1/w` + `uv/w` / `color/w`）——
  202 的 `clip.w ≠ 1` 批不再被 `kUnGpuPerspective` 整批拒，雪才真画出来。**B 在 A 之后**（同文件面，串行）。
- **P4**：caps 报 `PS=0xFFFF0200` / `VS=0xFFFE0100` + **硬等式 `required == implemented`** 守卫（缺一个 ⇒ **启动 Trap**，
  ⛔ 没有 gap / 已知偏离这一项）。**必须 P3 + A 之后**（提前报 2.0 会同时点亮 203/202）。

## 风险 / 待核（⛔ 别当已核）

- **NaN**：`ColRate` 的 `0/0`（两图全透明）在 ps_2_0 与 Metal clamp 下不保证一致 ⇒ P2 必须有该用例把行为钉住。
- **203 的 `Tex0` 来源未读**（`[this+0xA668h]` 包装 +0x20）：若是"后缓冲快照"就撞反馈环（bit28/43）—— P3 最大未知，先读。
- **level 还有第 2 入口**：`0x4A6FA2` 的 `arg_4 ∈ [-1..2]` 且 ≠ -1 时直接覆盖 `[this+0xB64C]` ⇒ "caps=2.0 ⇒ 等级一定 2"有例外。
- P1–P3 期间用临时 opt-in（`LIFT_D3D9_PS_VERSION`）让引擎走 FX 路；**P4 已裁决删除该开关**（caps 固定报真实能力）。
  ⛔ 不要把 opt-in 当产品形态，也不要在 P4 之后重新引入。

## 判据（父单收口时）

- caps 报的值与实现能力一致：报 PS=2.0 / VS=1.0 ⇒ 4 个 effect 程序 + 202 的 `SnowVS` 全在实现集；漏一个启动 Trap。
- 同一段转场：单帧 guest 指令从 2e10 回到 1e6 量级，画面与 CPU oracle 差异在阈值内。
- 映射表覆盖 4 个资源；漏一个 ⇒ 响亮停并点名 id/哈希。
