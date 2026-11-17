# D3DX9 effect 路径（P0/P1）：资源、`ID3DXEffect` 面、caps 分路与"effect draw 具名拒绝"

> 路线在需求单 `REQ-01M7MNPGETP2CQJ3ACMCGEHKF5`；形态裁决见 ADR `REQ-01M7MNYPDXM2NEQR77DXYZM3CJ`
> （**构建期预转写，有限固定集合**）与 `REQ-01M7MNP9K8T81SP2HNXQ9TTXPG`（目标是真正的 GPU 渲染）。
> 本文件只写**读数与口径**；缺陷史在 `defects.md`。

## 1. 资源：4 份 **ASCII HLSL 源码**（不是编译后的 FX 二进制）

容器 = `AGE.EXE__dumped.sectfix.EXE` 的 PE `.rsrc` → **RT_RCDATA**，恰好 4 个 id：

| id | 大小 | sha256 | 内容 | 目标 |
|---|---|---|---|---|
| 200 | 892 B | `678dd662f4d7bc98eb4042261dd20571f11ea70045e66f38c8548d17197f8518` | `FadePS` + `technique Fade` | `ps_1_0` |
| 201 | 1,077 B | `fb76f089c287e69a0928ef640ed15e1044f87ff19e3e739be2192ee2a46d419b` | 同 200（等级 2 用） | `ps_2_0` |
| 202 | 899 B | `a948d29bc5aab3e0fa62ec551903a0f102ad56e3cd5e5943d0a8c0c4bd0473ac` | `SnowPS` + `technique Snow` | `vs_1_0 + ps_1_0` |
| 203 | 1,776 B | `0ec494f035f7a6678b0b8aea1ec25a3ed8b4967b6084b8e4469a3e94ec5847bd` | `SlideBlurPS` / `ZoomBlurPS`（10-tap 模糊） | `ps_2_0` |

- 4/4 **无** FX magic（`0xFEFF0901`）/ `DXBC` / `CTAB` / shader 版本 token ⇒ **没有内嵌字节码**，也没有"编译后的 FX"。
- ⚠ 只能从**拆壳件**取：原版 `links/raw/AGE.EXE` 的 `.rsrc` raw 只有 0xE00、数据 RVA 0x1AD4A0 越界 ⇒ 读不到字节。
- 判据（`ctest -R '^d3dx9-effect$'` 的 `d3dx9-effect/resources-pinned`）：大小 + 首两字节 `//` + 完整 sha256 + `RT_RCDATA` 恰好 4 项。

## 2. 引擎侧怎么用它们（台账锚：`D3D9-effect-level` / `D3D9-effect-object`）

- 效果**等级** = `PixelShaderVersion` 对 `0xFFFF0200` / `0xFFFF0100` 归约成 2 / 1 / 0，写 `this+0xB64C`；
  等级决定建哪些 FX：`1 ⇒ 200`、`≥2 ⇒ 201`、`>1 ⇒ 203`、`0 ⇒ 整条 FX 路跳过`。
- `this+0xB590` = **`ID3DXEffect*`**（不是 caps 的 `Caps3`）：`0x4B0735 push &[ebx+0B590h]` 是 `ppEffect` 出参。
- `ID3DXEffect` 实际只被调 **9 个槽**：`+0x08 Release` · `+0x78 SetFloat` · `+0xD0 SetTexture` ·
  `+0xE8 SetTechnique`（`push "Fade"`）· `+0xFC Begin` · `+0x100 BeginPass` · `+0x104 CommitChanges` · `+0x108 EndPass` · `+0x10C End`。

## 3. 我们的实现（P0/P1）

- `src/shims/d3dx9_effect.{h,cpp}`：`readEffectSource()`（`X86::image` + 既有 `PeResources`；按内容 sha256 查 4 条入表，
  不在表里 ⇒ `Unrecorded` ⇒ 响亮停，打 id + sha256 + 前 32 字节）；`D3DXCreateEffectFromResourceA`（9 参）+
  9 槽 COM（**只记状态**，不执行 shader、不提交 draw）。未取证的 `GetDevice/OnLostDevice/OnResetDevice` ⇒ `comUnsupported`。
- `Begin` 的 `*pPasses` 报 **1**：4 份文本每个 technique 恰好 1 个 `pass P0`，这是**结构性下限**，⛔ 不是解析结果（P2 才解析）。
- **caps 开发开关** `LIFT_D3D9_PS_VERSION`（缺省不存在 ⇒ 什么都不变）：只改 `+0xCC`，值域 `0/0x100/0x200/0x300`，
  非法 ⇒ 点名合法值 + `exit 2`；开档打一行 readout（等级 ⇒ 会建哪些 FX）并在**每次 effect 创建**打一条 warn（≤4 条）。
  ★ 引擎只读 5 个 caps 字段（4 站点），`PixelShaderVersion` 是**唯一**有整条分路后果的项（台账锚）。

## 4. 读数（2026-11-17 / mac arm64 / RelWithDebInfo / `links/raw` / prefix27）

- E2E（`LIFT_D3D9_PS_VERSION=0x200`）：`exit 0` · `input-plan pending=0` · `unimplemented=0` · `comUnsupported=0` ·
  **不再停 `eip=7FFE0060`**；effect 资源 **201 / 203 在 frame 949 创建成功**；方法面计数
  `SetTechnique 718 · SetFloat 2684 · SetTexture 718 · Begin 718 · BeginPass 718 · CommitChanges 718 · EndPass 718 · End 718`。
- 缺省档（不给 env）与改动前的差异**只有一行** `shim: implemented 298→299 / gaps 12→11`（新实现了一个导出）+ 抖动行。

## 5. ★ effect draw 的具名拒绝（P1 **临时**）

修前：引擎对 203 只绑 `Tex0` ⇒ 既有的 `stage≥1`（bit33）不触发 ⇒ effect 内提交的 draw 被**当作固定功能接受**
（**错误像素、账上无具名位** —— 本仓最贵的一类静默错）。

修后：具名位 **44 `effect shader 尚未实现执行（P1 临时）`**，`gpuEligible` 的第一条判据整批拒绝，**每批一条** warn：
`effect 的 shader 尚未实现执行（资源 id=203 technique="SlideBlur" pass=0；P1 只记状态 ⇒ 整批拒绝，⛔ 不当作固定功能画）`。
读数：同一 E2E 拒绝 **1195** 批（`SlideBlur 885 + ZoomBlur 310`）。
守卫：`d3dx9-effect/effect-draw-refused`（HRESULT + 原因串 + bit44 + "End 之后不再记"）。
★ **P2 翻面清单**：删判据 + 删 bit44 + 删 `effectDrawWarn` + 把该用例改成 `recorded`。⛔ 不要把它留成永久契约。

## 6. 未做（照 S0/P1 的诚实项）

- `Release` 到 0 的收口只有 pure 用例覆盖（E2E 在 time-limit 前没走到收尾）。
- `*pPasses = 1` 是结构性下限（P2 才解析 technique/pass）。
- `IID_ID3DXEffect` 没有可校验来源 ⇒ 只答 `IUnknown`。
- 等级第 2 入口（`0x4A6FA2` 的 `arg_4`）本次没观测到覆盖（读数与"caps 2.0 ⇒ 等级 2"一致，但不硬解释）。
- 203 的 `Tex0` 来源（`[this+0xA668h]` 包装 +0x20）未读 —— 若是后缓冲快照就撞反馈环，P3 前必须先读。

## 7. caps 分路的验证读数（2026-11-17；把"caps ⇒ FX 取代软件模糊"从假设变成已证）

`--until <EA>`（AOT 的"跑到该 EA 的块边界就停"，先用 `--until 004127D2` / `--until 004A71B0` 验过它认得出真执行过的入口）：

| 探针 EA | 是什么 | `LIFT_D3D9_PS_VERSION=0x200` 下的结果 |
|---|---|---|
| `0x004A62A0` | `BlurTexture`（软件模糊入口） | **0 命中** |
| `0x004B3792` | 软件模糊的唯一动静调用点（`loc_4B36EF` 里 `call sub_4A62A0`） | 0 命中 |
| `0x004B36EF` | 软件模糊那一支的入口 | 0 命中 |
| `0x004B3518` | FX 支里 `SetTechnique("ZoomBlur")` | 0 命中（本窗口 `var_98 != 1` ⇒ 只走 SlideBlur） |
| `0x004A71B0` | effect draw 帮助函数（`CreateVertexBuffer` → FVF `0x244` → Draw） | **命中（frame 1384）** |

⇒ 等级 2 ⇒ `[this+0xB59C]`（203 的 effect 指针）非 0 且等级=2 ⇒ `0x4B34F9` 的 `jl loc_4B36EF` 不成立 ⇒ 走 FX 支；
**软件模糊在本窗口根本没进**。⇒ "caps ⇒ FX 取代软件模糊"**在本窗口成立**（不再是假设）。

★ 同时**撤回**两处早先的表述：
1. "那段重活 = 软件模糊" —— 只在 **caps=0（等级 0）** 的配置下成立；等级 ≥2 时它 0 次调用。
2. 判据"整跑 `insns` 从 108e9 下降" —— 两次 run 的**帧数不同**（8,729 vs 10,150），整跑总量不可横比。
   正确口径是**同一帧窗口**的 per-frame insns（例如转场 ≈1500–2000 帧）或同一帧数下的比较。
3. 剩下的 ~1.1e11 是 **frames ≈8500–9800 的持续段**（每帧 ≈1.3e8，profile 分散：top 仅 4.5%，
   含 `sub_4904D0` 文本查找）⇒ 更像**转场之后的正文内容**，与转场/模糊无关，是另一条性能问题（不在本路线内）。
   ⚠ 归因工具缺口：`--profile` 只有**整跑聚合**、没有按帧区间归因 ⇒ 那段"到底谁贵"今天给不出读数。

## 8. P2 · Fade 真画（2026-11-17）

- **A · pass 解析**：`parseEffectSource` → `FxModule`（常量 / `sampler_state` / shader 函数 / technique / pass）。
  4 份文本的 pass 块里**只有** `PixelShader` / `VertexShader` 两条赋值 ⇒ **没有 D3D9 渲染状态**要应用
  （出现未建模的名字 ⇒ `UnsupportedState` 并点名，⛔ 不静默忽略）。`Begin` 的 `*pPasses` 从 P1 的结构性常数 1 改成解析结果。
- **B/C · 后端**：新 `src/runtime/d3d9_effect_gpu.h`（可选接口 `GpuEffectRaster` + `GpuEffectDraw`）；
  `d3d9_gpu.cpp` 增 **MSL + DXBC 双源**（`FxVSMain` 带**第二套 UV 属性**；`PSMainFade` ps_2_0 / `PSMainFadeLinear` ps_1_0；
  常量表 + 2 sampler + 独立 pipeline 缓存；flush 里按字节偏移绑顶点 + 2 纹理 + 逐 draw 推 fragment uniform）。
  顶点仍是 CPU 变换后的屏幕空间三角形（`L3-tl-01`）。
- **D · 像素 oracle**（真 D3D9 + 纹理 RT + `GetRenderTargetData`）：

| `Alpha` | 读回 | 与 C++ 参考 |
|---|---|---|
| 0 | `(R255, A128)` | 一致 |
| 0.5 | `(R255, G170, A191)` | 一致（最大误差 **1 LSB/通道**） |
| 1 | `(R255, G255, A255)` | 一致 |

- **NaN 档**（两图全透明 + `Alpha=0.5` ⇒ `ColRate = 0/0`）：输出缓冲与未画前**逐字节相同**；先跑**控制行**
  （同 blend/路径、纹理不透明 ⇒ 目标**必须**变，实测变了）证明不是恒真。⇒ 结论是「**不可观测**」
  （SRCALPHA/INVSRCALPHA 下 `ret.a=0` ⇒ 源因子 0），⛔ 不是"我们定义了 NaN 语义"：shader 里没有特判。
- **E · 翻面（按 technique 分流）**：删 P1 的 `kUnEffectShaderNotExecuted` / 每批判据 / `effectDrawWarn`；
  同号新语义 **`kUnEffectProgramNotExecuted`**（"effect 的 pass 程序不在实现集里"）。
  Fade(201) ⇒ `recorded`（后端收到一条队列条目：program=`FadePs20`、Alpha、2 纹理、uv1）；
  203 SlideBlur / 202 ⇒ 仍 `INVALIDCALL` + 具名位 + 原因串（id/technique/pass）。E2E 里 `尚未实现执行` **0 条**（改前 1232）。

### 8.1 转场窗口读数（机械规则，不拿"帧号"当真理）

规则 = 一段连续帧：① 每帧 hash 都与前一帧不同 ② 段长 ≥10 ③ 每帧 insns 中位数 >5e7；取 boot 之后第一段。
⚠ 这条只能筛出"逐帧推进且很贵"，**不能**证明段里就是"渐变到黑"（hash 读不出像素）。

| | caps=0 | caps=0x200 |
|---|---|---|
| 窗口 | 278–307（30 帧） | 299–327（29 帧） |
| per-frame 中位 / 总和 | 129.5M / 3.255e9 | 131.3M / 3.385e9 |
| hash 形状 | **8 帧周期循环**（×3 轮） | **8 帧周期循环**（×3 轮） |

⇒ 该窗口两跑几乎相同，且**都不是单调推进**（是 8 帧周期）。★ 真正的分路证据在别处：caps=0 有 **帧 3198–3209**
（12 帧、中位 **1.03e9**/帧、合计 **29.9e9**）的软件模糊窗；caps=0x200 **同时间带没有任何 >5e7 的帧**。

### 8.2 两跑都有的长尾（与 caps / effect 无关）

caps=0x200：帧 8980–9800 / 821 帧 / 109.3e9 / 中位 135M；caps=0：帧 7981–8651 / 671 帧 / 89.4e9 / 中位 134M
⇒ 每帧速率一样 ⇒ 是**转场之后的正文内容**，不是转场；归因需要 `REQ-01M7MW746DSB2G7NCBSJC6XV92`（profile 按帧区间）。

### 8.3 顺手修掉一个既有越界读

`effectResourcePins()` 的 `kPins[]` 缺终止项（头文件写了契约、表里没有）⇒ `pins[4].id == 0` **一直越界读**；
P2 新增 constexpr 表后当场红 ⇒ 补终止项 + `effectResourcePinBySha` 在终止项 `break`。

### 8.4 两个坑（给后来者）

1. `Machine::gpuRaster()` 生产路径给的是 **`SdlWindow` 的转发包装** ⇒ 对包装 `dynamic_cast<GpuEffectRaster*>` **永远失败**
   （第一版把真 GPU 的 effect draw 全拒了）。⛔ 不许改 `present_sdl` / `gpu_raster.h` ⇒ 用
   `d3d9_effect_gpu.h` 的两个交付入口 + "活着的那个后端"回落。
2. 直接写 guest 纹理像素**不推进内容版本** ⇒ GPU 上传缓存看不到（oracle 第一版踩过）。

## 9. 用户验收读数（2026-11-17 · macOS · 相对点击的偏移）

缺省档（caps=0）与 `LIFT_D3D9_PS_VERSION=0x200`（等级 2）两跑的用户观感对照：

| | 缺省（caps=0，软件路径） | `PS=0x200`（FX 路径） |
|---|---|---|
| 转场 | 点击后 **+4 s 变纯黑**，黑屏极长（+22 s 才出章节动画，+46 s 才进 SC0000 文本） | **与真机差不多了**：渐变大部分正确 |
| 剩下的不符 | —— | **effect 层仍是黑的**（那正是 203 的 `SlideBlur`/`ZoomBlur` 未实现、被具名拒绝的部分） |

⇒ 结论（用户口径）：**FX 路径就是这条转场的答案**；剩下的黑 = 203 未实现（P3）。音频未验。

### 9.1 这次确认推翻了什么、确立了什么

- ❌ 推翻"整体依然没变化 / 像卡在 CPU 渲染侧"的初判 —— 那是**缺省档（caps=0）**的现象：不设 `LIFT_D3D9_PS_VERSION` 时
  引擎根本不会建 effect，P1/P2 的成果**不会被触发**。
- ✅ 确立：`caps ⇒ FX 分路 ⇒ 转场与真机一致（除 203 的 effect 层）`，这条现在是**用户实测**而不是推断。
- ⇒ 之后要做的是：**P3（203 的两个 10-tap technique）**，然后 **P4（把 caps 缺省翻到 `PS=0x200` / `VS=0xFFFE0100` +
  `required == implemented` 硬等式守卫）**；202（Snow，`vs_1_0`）**必须先实现**（A 单
  `REQ-01M7NAW2QT3TG9RCFMKR052C83`）—— ⛔ "先允许它缺失并登记" 这条已被用户否决（2026-11-18）。

## 10. P3 · 203 的两个 technique（SlideBlur / ZoomBlur）

- **前置（`Tex0` 来源）**：`[this+0xA668]` = 纹理表 **slot 36**（基址 `0xA5D8 + 36*4`）= 引擎自建的**全屏 `D3DUSAGE_RENDERTARGET` 纹理**
  （`sub_4A6D00 → sub_4A2C10(...,36,w,h,mode) → sub_48AB20/48AC40 → D3DXCreateTexture(usage=1, fmt 21/22)`）。
  引擎次序：`0x4B32AF SetTargetTexture(36)` 画进去 → `0x4B34C0 SetTargetTexture(v384[4])` **切走** → `0x4B3625 SetTexture("Tex0",36)` →
  `0x4B36A3` 全屏四边形 ⇒ **采样的是已完成的快照**，不撞反馈环；bit43 闸保留且对 effect 纹理也查（实测 0 次触发）。
- **tap 数按源文本**：`SlideBlur` 是 **21 tap**（`i=-10..+10` 含）×0.0476，`ZoomBlur` 是 **10 tap** ×`ratio[10]`
  ⇒ "10-tap" 只对 ZoomBlur 成立。
- **实现**：MSL + DXBC 双源各加 `PSMainSlideBlur` / `PSMainZoomBlur`；`kFxEntries` 扩成 `{entry, samplers}`（203 = 1 sampler）；
  pipeline 键步长 `*4 → *8`。★ Metal 没有 `radians()`（`MTLLibraryErrorCode=3`）⇒ 两源内联 `Angle*π/180`。
- **翻面**：203 两条 binding ⇒ `recorded`；202（Snow，`vs_1_0`）仍 `kUnEffectProgramNotExecuted`（原因串带 id/technique/pass）。

### 10.1 读数

| 项 | 读数 |
|---|---|
| `d3dx9-effect` | **608 checks / 0 failures**（全量 107 项：103 passed + 4 平台 skip） |
| 像素（真 GPU + 纹理 RT + 读回） | SlideBlur `Angle=0 ⇒ B=210 G=0 R=45`、`Angle=90 ⇒ B=255 G=45 R=0`、ZoomBlur ⇒ `B=159 G=0 R=96`，与参考 **0 LSB**（容差 1） |
| E2E（caps=0x200） | `没有实现执行` **1091 → 0**、`pending=0`、`implemented=299 gaps=11` 与 before 逐字相同 |
| 黑 → 有内容 | `SN0000 pos=0xD190` 段：before 44 采样 = **1 个 hash**（纯黑 `0xD11B…`）；after 25 采样 = **25 个 hash**；抽帧 before `uniq=1 / nonblack=0%`、after `uniq=8.2万–28.9万`（亮度 244→102 的斜坡） |
| 变异 | tap 21→20 = 12 LSB · uv 步长 ×10 = 72 LSB · program 映射 = 45 LSB + 纯档 `mock.last.program` 红 |

⇒ 至此 **caps=0x200 下转场与真机一致**（用户实测 + 本地读数）。剩下的是：**202（Snow）**、P4（caps 固定 2.0）。

## 11. 202 的 `Proj` 语义（N3，2026-11-18 / win32 / 只读）

> 目的：202（`SnowPS` / `technique Snow`，`vs_1_0 + ps_1_0`）落地前，先定 `SetMatrix("Proj")` 喂的是哪组矩阵、
> 行/列序、以及顶点进来时在哪个空间 —— 它决定 CPU `vs_1_0` 的形状。
> 语料 `corpus/disasm/files/AGE.EXE__dumped.sectfix.EXE.lst`（sha256 `7a7ba237ac22a9eb…`）；锚点全部是**二进制 EA**。

### 11.1 结论

1. `[renderer+418h]` = **纯投影矩阵**（`D3DXMatrixPerspectiveFovLH` 的输出），**不含 view / world**；
   同组 `[+458h]` = view（`LookAtLH` / `D3DTS_VIEW=2`）、`[+498h]` = world（`D3DTS_WORLD=256`）。
2. 202 的 `Proj` = `[renderer+418h]` 的**逐字节副本**（`Snow+1Ch`），中间没有再乘任何矩阵。
3. 行/列序 = **D3DX 原生**：行主序（`m[r][c]@16r+4c`）+ 行向量 `v' = v*M`。
4. `SetMatrix("Proj")` 在 `Begin` **之前**、每次 `Snow::Render` 一次 ⇒ **per-effect**（跨多轮 Begin/End 保持）；
   `Color` 是每粒子在 `Begin` 前设的 uniform float4。
5. **顶点进来时已经是视图空间**（引擎在 CPU 上乘过 `world×view`）⇒ CPU `vs_1_0` 只乘 `Proj`：
   `clip = mul(vec4(pos,1), Proj)`，`w` 原样输出、**不做除**。⛔ 不许再用设备的 WORLD/VIEW 状态（会二次变换）。

### 11.2 证据（节选）

| EA | 观察 | 说明 |
|---|---|---|
| `0x4531BE / 0x4531CE / 0x4531DE` | `lea edi,[ebx+418h]/[+458h]/[+498h]` + `rep movsd`(0x10) | 这三个字段的写点（`sub_4531B0`，`retn 0C0h` = 3×4×4） |
| `0x499DD7 / 0x49A28D / 0x4A1DA7` | 三个调用点传参顺序全是 **(proj, view, world)** | 分派成立：`+418h←arg_0`、`+458h←arg_40`、`+498h←arg_80` |
| `0x49A21A` · `0x4A1D89` | `D3DXMatrixPerspectiveFovLH` · `SetTransform(3)` | `[app+0A388h]` = 投影（`D3DTS_PROJECTION`） |
| `0x499D77` · `0x499D94` | `D3DXMatrixLookAtLH` · `SetTransform(2)` | `[app+0A3C8h]` = view（`D3DTS_VIEW`） |
| `0x4A1DB4` | `SetTransform(0x100)` | `[app+0A408h]` = world（`D3DTS_WORLD`） |
| `0x4B5716 / 0x4B5726` | `lea edx,[esi+1Ch]` + `push "Proj"` + `call [ecx+98h]` | `SetMatrix("Proj", &[Snow+1Ch])`，无二次乘 |
| `0x4B5726` vs `0x4B55C4` | `Snow::Render` 的 SetMatrix vs `Snow::DrawOne` 的 `[+0FCh] Begin` | `Proj` 在 Begin 之前 ⇒ per-effect；Begin/End 每粒子一对 |
| `0x4B5767` · `0x4B543A` | `D3DXMatrixMultiply(world, view)` · `D3DXVec4Transform((x,y,z,1))` | 顶点先变成视图空间；`0x4B55F5 DrawPrimitiveUP(TRIANGLESTRIP,2,particle+2Ch,20)` 交给 VS |
| `0x4B5706` | `push 102h` | FVF = `XYZ|TEX1`（无 normal / diffuse） |
| `0x4A70E2 / 0x4A70DC / 0x4A70D6 / 0x4A70D0` | `1.0 → [+0A408h]/[+0A41Ch]/[+0A430h]/[+0A444h]` | 对角在 `+0/+14h/+28h/+3Ch` ⇒ **行主序** |
| `0x4B550B` | `fld [ecx+34h]; fsub [eax+34h]` | 排序键 = 变换后的 z（降序）⇒ 佐证视图空间深度 |

### 11.3 对实现的含义

- C++：`clip = transformVec4D3D(fromD3DLayout(proj16), Vec4(x,y,z,1))` —— 与 `transformVertex`
  （`d3d9_pipeline.cpp:3866` 的 `multiplyD3D(multiplyD3D(world,view),proj)`）**同一套算子、只换矩阵**。
- ⛔ 不要用设备的 WORLD/VIEW 状态；⛔ 不做 `w` 除法（裁剪在 `clipHomogeneous`、`1/w` 在 `toScreen`）。
- shim 侧：`SetMatrix` 在 Begin 之前，而 `publishDrawProgram`（`d3dx9_effect.cpp:1410`）在 scope 外是 no-op
  ⇒ 矩阵必须进**状态块**、在 `Begin`/`BeginPass` 发布时读；⛔ 不能存成"最后一次 SetMatrix"的全局。
- `transformVertexOnce`（`d3d9_pipeline.cpp:3919`）的每 Draw 去重前提是"顶点号唯一决定结果"；VS 路多一个输入（`Proj`）
  ⇒ 缓存键要并入矩阵，或这条路禁用缓存。

### 11.4 复现（只读）

```bash
pnpm tools disasm-fn show sub_4B56D0 --lst --lines 130   # 202 的 SetMatrix / SetTexture / SetTechnique / world×view
pnpm tools disasm-fn show sub_4531B0 --lst               # +418h/+458h/+498h 的写点
pnpm tools disasm-fn callers sub_4531B0                  # 三个调用点的传参顺序
pnpm tools disasm-at at --ea 0x4B5700 --lines 40          # SetMatrix("Proj") 现场
pnpm tools disasm-at at --ea 0x4531B0 --lines 26          # 写点现场
```

### 11.5 待核

1. `D3DXMatrixMultiply` 的参数顺序（按 D3DX 公开语义 `out = M1*M2`；若相反则 world/view 语义互换，但**不影响本结论**）。
2. `[app+0A408h]`（world）除单位阵初始化外没有别的立即数写点；**指针间接写**未穷尽 ⇒ 本语料里 world 恒为单位阵。
3. `Proj` 是 `Snow` **构造时的快照**（之后相机移动会不会用旧投影 = 需 N1 的 run 才能判）。
4. 粒子的 uv 写点未核（`sub_4B5410` 只写 xyz）。
