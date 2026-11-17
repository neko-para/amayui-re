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
