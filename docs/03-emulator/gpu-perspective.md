# GPU 批的透视校正插值（逐顶点 `1/w`）：口径 / 双源改动 / 判据 / 读数

> 需求单 `REQ-01M7NC21S484PFYDAE7YTNX02J`（B 单）。前置 = A 单（202 的 shim 状态 / CPU `vs_1_0` /
> `ps_1_0` / binding，`d3dx-effect.md` §12）与 P4（caps 固定 2.0/1.0 + 实现集硬等式，`caps.md`）。
> 本文件只写**口径 / 落点 / 判据 / 读数 / 残余风险**；一次性读数随下面"复现命令"一次可重跑，
> 数字不进 test（`docs/03-emulator/README.md` §0.1）。

## 1. 一句话与为什么

`submitPrimitives` 以前有一条**整批拒绝**：任何一批只要某个顶点的 `invw != 1` ⇒ `recordUnsupported`
（bit21，旧名 `kUnGpuPerspective`）+ `pipeWarn` + `D3DERR_INVALIDCALL`。202（Snow）的 `Proj` 是
`D3DXMatrixPerspectiveFovLH` 的输出、顶点是视图空间 ⇒ `clip.w = z_view ≠ 1` ⇒ **雪批在真 run 里被这条闸
整批拒**（`d3dx-effect.md` §12.3 第 1 条）。

现在改成**真实现**：GPU 顶点带**逐顶点 `1/w`**，片元里把顶点上已乘过 `1/w` 的 `uv` / `color` **除回来**。
⇒ D3D9 的透视校正与 GPU 的屏幕空间线性插值等价，透视批（含 202）真入队、真画出来。
⛔ 不是"告诉引擎不支持"、⛔ 不是给 202 开特判、⛔ 也不是"整批一个 `invw`"的近似。

## 2. 口径（唯一一份数学）

D3D9 的片元属性按**透视校正**插值。GPU（MSL / DXBC）的片元属性插值是**屏幕空间线性**的：
给定顶点的 `a_i`，片元拿到 `Σ λ_i·a_i`（λ = 屏幕空间重心权重）。

⇒ 在 **VS 里输出** `a_i / w_i` 与 `1/w_i`，在 **PS 里**算

```
a = (Σ λ_i · a_i/w_i) / (Σ λ_i · 1/w_i)
```

就与 D3D9 完全同一条式子。本实现的顶点数据流本来就是"乘过 `1/w`"的形状（`toScreen` 把
`SVertex.u/r/...` 都乘了 `invw`）⇒ 这次只多带**已经算好的那个 `1/w`**，片元里除回来：

| 层 | 字段 | 语义 |
|---|---|---|
| `CVertex` | `x/y/z/w` | 裁剪空间（`w` 来自世界×视×投影；XYZRHW 时 `w = 1/rhw`） |
| `SVertex` | `invw`、`u/v = uv·invw`、`r/g/b/a = color·invw` | `toScreen` 之后；`invw = 1/w`（固定功能 `w == 1` ⇒ `invw == 1`） |
| `GpuRasterVertex` | `invw`、`u/v = uv/w`、`r/g/b/a = color/w` | 交给后端的屏幕空间顶点（逐顶点，⛔ 不整批近似） |
| 片元 | `uv = u/v ÷ invw`、`color = (r,g,b,a) ÷ invw` | 后端 PS（MSL 与 DXBC 逐句一致） |

★ **`invw == 1` 的批（固定功能无投影 / XYZRHW `rhw=1` / sprite）除法是恒等** ⇒ 逐像素与旧实现相同。
这是"固定功能路径不变"的判据，靠既有像素用例守（§5.2）。
★ **深度 `z`** 不受影响：`SVertex.z = z/w` 已是屏幕空间线性量，仍走 `SV_Position.z`（`w = 1`），
D3D 的 z 插值本来就是屏幕空间线性（§5.2 的 `d3d9p/z-buffer` 等用例照旧）。

## 3. 落点（逐项）

| # | 落点 | 改了什么 |
|---|---|---|
| ① | `src/runtime/gpu_raster.h` | `GpuRasterVertex` 加 `float invw = 1.0f`（逐顶点；注释写清 `u/v` / 颜色是 `/w` 的值）；新纯函数 `gpuVertexUv(v,&u,&v)` = `(u/w)÷(1/w)` |
| ② | 同文件 `gpuDrawSampleRect` | 采样/脏区 bbox 改用 `gpuVertexUv`：`u/w` 的 bbox 与**真实**采样范围可差一个数量级（近处 `uv/w` 小、远处大）⇒ 直接拿 `u/w` 会**少传 texel**；`invw` 不可用/非有限 ⇒ 退回"整幅"（保守） |
| ③ | `src/runtime/d3d9_pipeline.cpp` `push_tri` | `g.invw = (float)v.invw`（逐顶点上传）；把旧的 `invw != 1 ⇒ gpu_invw_ok=false` 换成**只挑不可表示**：`!(finite && > 0)` ⇒ `gpu_w_bad` |
| ④ | 同文件拒批块 | 位 21 **语义收窄、号不变**：`kUnGpuPerspective` → `kUnGpuVertexW`，文案 = "顶点 w 不可表示（w<=0 / 非有限 ⇒ 1/w 无意义）"；仍然 `pipeWarn` + `noteGpuSkip`（一个像素都不写 ⇒ 必须有账） |
| ⑤ | `src/runtime/d3d9_gpu.cpp` **DXBC** 源 | `VSIn` 加 `float invw : TEXCOORD3`（effect 批 `TEXCOORD4`）、`VSOut`/`FxVSOut` 原样带出；6 档固定功能 PS 与 5 个 effect PS 里 `uv → uv/invw`、`color → color/invw` |
| ⑥ | 同文件 **MSL** 源 | 与 ⑤ **逐句一致**：`[[attribute(3)]]` / `[[attribute(4)]]`、`[[user(locn2)]]` / `[[user(locn3)]]`、PS 里同一个除法 |
| ⑦ | 同文件顶点布局 | `Vertex` 36 → 40 B（+`invw`）、`FxVertex` 44 → 48 B（+`invw`）；pipeline 的属性表 3 → 4 / 4 → 5 条（`SDL_GPU_VERTEXELEMENTFORMAT_FLOAT`）；上传时逐顶点写 `invw` |
| ⑧ | 同文件纹理探针 | `LIFT_GPU_FF_TEX` 选 texel 用 `gpuVertexUv` 的**真实 uv**（诊断读数不再骗人） |
| ⑨ | `src/runtime/d3d9_pipeline.h` | `clipVs10` 的注释更新（"`w != 1` 的批会被整批拒"已不成立） |
| ⑩ | `apps/emulator-lift/README.md` | 旁路表 / `skipped` 口径里 `invw != 1` 的条目改成"顶点 `w` 不可表示"，并指向本文件 |

★ **⛔ 没有动的**：202 的 shim / binding / CPU `vs_1_0`（A 单面）、caps 的声明与 `required == implemented`
等式（P4 面）、`gpuEligible` 的全部准入闸、`d3dx9_sprite` 的顶点（`invw` 默认 1 ⇒ 行为不变）。

## 4. 具名拒绝（真正不可表示的那一类）

`w <= 0` / 非有限 ⇒ `1/w` 没有意义（D3D9 的合法 clip 空间要求 `w > 0`）。这类批仍然：
`D3DERR_INVALIDCALL` + `recordUnsupported(bit21 kUnGpuVertexW)` + 默认级别一行 warn + `skipped_draws +1`。
⛔ 不再有"凡 `invw != 1` 就拒"的笼统形态；bit 号 21 保留（历史读数的位含义作废、号不复用给别的语义）。

## 5. 判据（红了意味着什么）

### 5.1 新像素 oracle：`d3d9p/perspective-interpolation`（`tests/d3d9_pipeline.cpp`，`d3d9-pipeline-gpu`）

几何：视图空间四边形，下边 `z = 1`（近）/ 上边 `z = 4`（远），`PROJECTION = perspectiveFovLH`、
WORLD/VIEW = 单位阵 ⇒ `clip.w = z_view` 跨 1..4、`1/w` 跨 1..0.25；两个深度不同的三角形；
屏幕落点用 `clipVs10`（与实现同一份 `mul(vec4(pos,1), Proj)`）算 ⇒ 屏幕上是**梯形**。

四批 draw（同一四边形、同一套顶点 uv / 颜色）：

1. `XYZ|TEX1`（透视）⇒ 逐像素 == 参考 `(Σλ·uv/w)/(Σλ·1/w)` 的那个 texel（POINT，≤1 LSB）；
2. **①的仿射版**（`XYZRHW`，`rhw = 1`，屏幕几何与①逐 float 相同）⇒ 逐像素 == 仿射参考；
3. `XYZ|DIFFUSE`（透视）⇒ 逐像素 == 透视校正后的颜色（≤1 LSB）；
4. **③的仿射版**（`XYZRHW`）。

**控制行**（证明不是恒真）：②与"①的参考"在同一批像素上必须差 > 32 LSB 的至少 20 个；④与"③的参考"同理。
②/④ 就是"故意不做透视校正"的那一档。

采样只取**三角形内部**（重心 ≥ 0.03）且**远离 texel 边界**（0.06 texel）的像素：前者避开填充规则边界，
后者让 POINT 采样的参考是**精确的某个 texel**（与 float 抖动无关）。纹理是 16×16、`texel(x,y)` 的
`R = x*16 / G = y*16 / B = 255-x*16` ⇒ **错一个 texel 就红**。

### 5.2 固定功能不回归

`invw == 1` ⇒ PS 里除 1 是恒等 ⇒ 既有像素用例全部照旧：`d3d9-pipeline-gpu`（纯色三角形 / 带纹理四边形 /
MAG-MIN 过滤 / mip 级 / z 缓冲 / alpha 混合 / 裁剪 …）、`d3dx9-gpu`（sprite）、`d3dx9-effect` 的
`fade-pixels` / `blur-pixels`、`d3d9-mip` 的 `screenUvDerivatives`。它们绿 = "固定功能逐像素没变"。

### 5.3 202 的透视档

- `d3dx9-effect/snow-vertices`（pure，Mock 记录顶点）：`persp` 档改成 **`z` 跨 1..4**（真 `w != 1`），
  ① 固定功能那条路与 CPU `vs_1_0` 那条路**逐 float 相同（含 `invw`，共 10 个 float）**；
  ② 这批顶点里**确实**有 `invw != 1`（⛔ 不是退到 `w == 1` 的档）；
  ③ `skipped_draws` 不涨、位 21 不置位 ⇒ 那条闸真的没了。
- `d3dx9-effect/snow-pixels`（真 GPU + 资源 202 + RT 读回）：新增**真透视 `Proj`** 档 ——
  视图空间四边形 `z` 跨 1..4、uv **恒定**在 texel(12,12) 的中心。透视校正下 `(u0/w)÷(1/w) == u0`
  （任意内部像素）⇒ 必须采到 texel(12,12)；判据：Draw 回 S_OK、`skipped_draws` 不涨、位 21 不置位、
  采样像素 == `texel * Color`。**控制行**：同一像素"不校正"的 uv 离 texel(12,12) 至少 2 个 texel
  （实测 8.47）⇒ 这个采样点对"漏除 `invw`"有分辨力。

### 5.4 具名拒绝的那一类：`d3d9p/vertex-w-refused`（`d3d9-pipeline-gpu`）

`XYZRHW` 的 `rhw = -1` ⇒ `w = -1`（`1/w` 无意义）⇒ Draw 回 `D3DERR_INVALIDCALL`、位 21
`kUnGpuVertexW` 置位、`skipped_draws == 1`、`gpu_draws == 0`；同一三角形的 `rhw = +1` 是**正控**
（S_OK、`gpu_draws == 1`、`skipped == 0`、位 21 不置位），拒绝之后同一份合法几何照旧能画。
★ 覆盖账在**每次 `Machine::run` 开头**重算 ⇒ 断言读在"Draw 是这一跑最后一次 guest 调用"处。

### 5.5 变异（`tools/mutate-check.mjs`，各 `--match` 单跑）

| 变异 | 落点 | 期望 |
|---|---|---|
| 片元**漏除 `invw`**（`Sample(samp, uv/invw)` → `Sample(samp, uv)`） | `d3d9_gpu.cpp` 的 **DXBC** `PSMainTex` | `d3d9-pipeline-gpu` 的 `perspective-interpolation` 红（`w==1` 的批不受影响 ⇒ 只跑仿射用例会假绿） |
| 顶点**不带逐顶点 `1/w`**（`g.invw = v.invw` → `1.0f`） | `d3d9_pipeline.cpp` 的 `push_tri` | 同上红（画出来仍是仿射）；`d3dx9-effect` 的 `snow-vertices`（pure）也在 `saw_nonunit_invw` 上红 |

## 6. 读数（2026-11-18 / win32 / RelWithDebInfo / `windows-x64`）

构建：`cmake --build --preset windows-x64 --parallel 4` ⇒ exit 0。

### 6.1 目标档 ctest

```
ctest --test-dir apps/emulator-lift/out -R 'd3d9-pipeline|d3dx9-effect'
⇒ 3/3 通过（d3d9-pipeline · d3d9-pipeline-gpu · d3dx9-effect）
# 更宽一圈（含 GPU 后端 / 合批 / mip）：
-R 'd3d9-pipeline|d3dx9-effect|gpu-batch|d3dx9-gpu|d3d9-gpu|d3d9-mip|d3d9$' ⇒ 9/9
```

新 oracle 自己的读数（`d3d9-pipeline-gpu` 的 stdout）：

```
perspective/uv：采样 607 像素（154 个 texel）；透视批最大误差 0 LSB；仿射批 0 LSB；控制行 368 像素差 >32（最大 96）
perspective/color：采样 948 像素；透视批最大误差 1 LSB；仿射批 1 LSB；控制行 750 像素差 >24（最大 85）
d3d9-pipeline-gpu: 1049 checks, 0 failures
```

202 的透视档（`d3dx9-effect` 的 stdout）：

```
snow-vertices：单位 Proj 与固定功能逐 float 相同（6 顶点）；真透视 Proj（w 跨 1..4）亦相同（含 invw；
               gpu_draws=1、位 21 未置位）；透视下 clip.w 原样保留；光照下 VS 顶点色仍是白
202 透视批（Proj 透视，w 跨 1..4）：像素 (33,28) B=255 G=0 R=0 A=255（误差 0；不校正则 uv 偏 8.47 texel）
202 像素 oracle：四档最大误差 = 0 LSB/通道
d3dx9-effect: 983 checks, 0 failures
```

### 6.2 全量 ctest

```
ctest --test-dir apps/emulator-lift/out --output-on-failure
⇒ 109/109 通过，0 failed；Total Test time (real) = 147.15 sec
（同一形状的前一次：125.62 s；两次都是 109/109）
```

### 6.3 变异实测（两条都红）

```
node tools/mutate-check.mjs --match '固定功能片元漏除'
⇒ ✅ 守卫红了（CTest 退出码 8）  … 1/1 条按预期变红。已全量重建回基线
node tools/mutate-check.mjs --match 'GPU 顶点不带逐顶点'
⇒ ✅ 守卫红了（CTest 退出码 8）  … 1/1 条按预期变红。已全量重建回基线
```

### 6.4 202 的真 run（★ 可达性仍未观测）

- 跑法（AOT 臂，真有 D3D12 设备；本仓库的配方只到 TITLE→GAMESTART→ADV→FELLOW→SAVE）：

  ```bash
  cd apps/emulator-lift
  ./out/emulator-lift-aot --exe ../../corpus/assets/age/AGE.EXE__dumped.sectfix.EXE \
    --game-dir ../../links/install --appdata <本树 .tmp 下的专用副本> \
    --skip-logo --audio off --present window --present-hidden \
    --input-plan scripts/recipes/sc0000-wipe.plan \
    --max-frames 8000 --time-limit 400
  ```
  ★ `--until` 只能用**派发块入口**（`out/aot/lift_table.inc` 里有的 EA）；`0x00418340` 不是块入口（BUG-31）。
  202 的可达配方归 N1（`REQ-01M7NNNMPFB1GDB52J662Y1CMZ`），本文件不把它写成"已到达"。
- 读数（2026-11-18；两次：`sn0000.plan` 4000 帧 + `sn0000-save-profile.plan` **8000 帧 / 配方走完**）：
  `stop: max-frames`、`input-plan: steps=21 fired=6 pending=0`、`raster: sdl-gpu/direct3d12/DXBC`。
  `--until 0x00418340`（`sub_418340` = `Set3DEffectSnow` 的入口）**一次都没命中** ⇒ 这条路径里
  **202 没有被创建/执行**（与 `caps.md` §7 第 5 条"run 级可达性未观测"同一个结论）。
  整份日志里 `[warn]` 共 **0 条**（缺省日志级 = `warn`）：既没有 `…顶点 w 不可表示…`，也没有旧的
  `顶点 invw!=1 … 拒绝该批` ⇒ **位 21 在两次真 run 里 0 次**。
  ⚠ **AOT 臂的运行摘要不打印 d3d9 的 `unsupported :` 位图**（它没有 `Machine` 实例），所以"位 21 = 0"
  的**机械判据**落在夹具级（§5.3：`PipelineInfo.unsupported` / `gpuCoverage`），真 run 这一侧只有
  "可见的消息面里 0 条 w 拒绝"这一条弱证据。
- ★ **"雪真的出现"没有取得真 run 证据**：不是本实现的问题，是**可达性**问题（缺一条能走到雪景的
  输入配方 / 存档）。夹具级证据（§6.1 的 `snow-vertices` / `snow-pixels`）覆盖"202 的批被接受、
  像素按透视校正画对"；真 run 证据要等一条能被 `--until 0x00418340` 命中的配方。
  ⛔ 不要把这条读数写成"雪已出现在真 run"。

## 7. 残余风险 / 边界

1. **`rhw != 1` 的 XYZRHW 批以前被整批拒，现在会被"透视校正"**：本实现按 D3D9 文档把 `rhw` 当
   `1/w`（`SVertex.invw = rhw`）⇒ 这类批变成透视校正插值（以前是"删除"）。⚠ 仓库里**没有** `rhw != 1`
   的用例，引擎是否用这种批**未观察**；若真机语义确实是"预制顶点线性插值"，那要把 `toScreen` 的
   `invw` 在 `pretransformed` 分支固定成 1（一行），并新增一档判据。今天没有任何证据要求它。
2. **`1/w` 是 float**：`w` 跨度极大（如 1..1000）时 `1/w` 的相对精度仍够（float 24 bit），但
   `uv/w ÷ 1/w` 的**逐像素**误差随 |w 跨比| 增大；实测 1..4 时 ≤1 LSB。极端深度跨度未读数。
3. **`gpuDrawSampleRect` 的保守性依赖 `invw > 0`**：`w` 不可表示的批在 `submitPrimitives` 就被拒，
   到不了后端；sprite / 仿射批 `invw == 1` ⇒ 与旧行为逐字节相同（`gpu-batch` 的脏区/切段用例绿）。
4. **DXBC 侧没有"在 mac 上编译"的保证**：MSL 与 DXBC 是**两份源**、靠逐句一致的人工约束
   （`kShaderSource` 的两支）。本轮的像素读数全部来自 **win32 的 DXBC**（SDL_GPU 的 D3D12 后端在
   `_WIN32` 下只编 DXBC）⇒ MSL 那一支**没有运行时读数**（mac 上跑 ctest 才能覆盖）。
   ⛔ 不把"两份源长得一样"当成"两边都验过"。
5. **`w == 0` 的三角形仍走 `toScreen` 的"丢弃"路径**（不是具名拒批）：`toScreen` 对非有限 / `|w|<1e-12`
   返回 false，`emit` 直接丢这个三角形（不加 `skipped`）。这是既有行为，本单没有改；真正的"整批
   `w` 坏"由 `push_tri` 的 `kUnGpuVertexW` 具名拒绝覆盖（有测试：`XYZRHW` 的 `rhw < 0`）。
6. **202 的真 run 证据缺一条可达配方**（§6.4）：这是**输入面**的缺口，不是插值实现的缺口。

## 8. 复现命令

```bash
cd apps/emulator-lift
cmake --build --preset windows-x64 --parallel 4
ctest --test-dir out -R 'd3d9-pipeline-gpu|d3dx9-effect' --output-on-failure   # 新 oracle + 202 透视档
ctest --test-dir out --output-on-failure                                      # 109/109
cd ../.. && node tools/mutate-check.mjs --match '固定功能片元漏除'
node tools/mutate-check.mjs --match 'GPU 顶点不带逐顶点'
node tools/mutate-check.mjs --list                                            # 静态检查：146/146
```

## 9. 202 的可达性（N1，2026-11-18）

> 需求单 `REQ-01M7NNNMPFB1GDB52J662Y1CMZ`。结论：**这份 appdata 的存档全在序章 / 地牢 2D ADV，没有一张在雪地图上**
> ⇒ 202 在真 run 到不了，不是配方写法问题。

- **输入面（实测，headless + 控制层 sample/screenshot）**：
  - SAVE00（`click 300 91`）→ `SN0000.BIN:0x46E8` @ frame 1624（序章旁白）；
  - SAVE02（`click 300 220`）→ `SC0000.BIN:0x99C0` @ frame 1443（地牢 2D ADV）；
  - SAVE03（`click 300 269`）→ `SC0000.BIN:0xCB9C` @ frame 678（地牢 2D ADV）；
  - ★ 「加载第 4 槽 + 1 击」的 wipe 之后**不是 3D 雪景**，而是地牢 2D ADV（再点 10 下仍在地牢，`SC0000:0xF7C8` @ frame 3317）。
- **触发链（静态，锚 EA）**：202 的唯一创建点 `0x418340`（`0x4183AE push 0CAh` → `D3DXCreateEffectFromResourceA`，
  错误串 `Set3DEffectSnow` @ `0x418363`）；唯一脚本上游 = **opcode `0x326`**（handler `0x426E10`）。
  `i326` 在 941 份脚本清单里**只有 1 次**（`SETWEATHER.BIN:39`），且被 `global-int 4fdb == 2` 门控；
  而 `4fdb` 的 189 处脚本写点里**没有一次写 2**（0×184 / 1×3 / 3×2）⇒ 天气型来自**数据记录**
  （`SETL2DMOC.BIN` 的 `i341`/`i345`：记录 id + 天气列），**雪是地图数据驱动，不是剧情脚本驱动**。
- **运行**：两条链共 20,000 帧，挂 `--until 0x418340`（`lift_table.inc` 的合法派发块入口）⇒ **0 命中**、`[warn]` 0 条。
  ⚠ 位 21 / `skipped_draws` 在真 run 仍无机械读数（AOT 摘要不打印 d3d9 `unsupported`，见 §6.4）。
- **下一步**：要一份**雪地图存档**（真机存一槽）或先解析 `SETL2DMOC` 的 238 条 `i341` 记录（天气列 = 2 的地图 id）；
  ★ 用户口径（2026-11-18）：**暂时也不知道具体雪景存档** ⇒ N1 保持**待验证**记录，不阻塞其他工作。
  ⛔ 不要靠继续堆点击（80 击只推进 2 句对白）。存在性快判据：`LIFT_AOT_SHIM_TRACE=1` 里 `D3DXCreateEffectFromResourceA` 的资源号 `202`。
  有雪档后可直接用两个已查表停点：`0x004B56D0`（`Snow::Render` / `SetMatrix("Proj")`）、`0x00426E10`（`0x326` handler）。
