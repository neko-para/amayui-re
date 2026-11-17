# caps（`D3DCAPS9`）：声明什么、凭什么、缺一个会怎样

> 口径真源：需求单 `REQ-01M7MXHYR1M72JW77J136GT3ZG`（P4 · caps 固定报 2.0/1.0 + `required == implemented` 硬等式守卫）·
> ADR `REQ-01M7MNP9K8T81SP2HNXQ9TTXPG`（目标是真正的 GPU 渲染；caps 报的值 = 实现能力）。
> 本文件只写**口径 / 裁决 / 证据**；caps 与 effect 的读数、复现命令在 `d3dx-effect.md`
> （§9 用户验收 · §11 202 的 `Proj` 语义 · §12 A 单落地 · §13 P4 收口）。§1 带日期：它描述 P4 **实施后**的形状。

## 1. 现状（2026-11-18 · P4 已实施）

- **`LIFT_D3D9_PS_VERSION` 已整条删除**（env 解析 / 值域校验 / 开档 readout 都没有了）：
  `runtime/d3d9.cpp:fillCaps()` 固定写 `PixelShaderVersion = kCapsPixelShaderVersionClaim`
  （`0xFFFF0200`）/ `VertexShaderVersion = kCapsVertexShaderVersionClaim`（`0xFFFE0100`）/
  `MaxVertexShaderConst = 96` / `MaxPixelShaderValue = 8.0f`。声明值只在 `runtime/d3d9.h` 一份
  （实现、守卫、用例共用；⛔ 测试不另抄数字）。
- **启动守卫 = 硬等式 `required == implemented`**：`shims/d3dx9_effect.cpp:effectCapsGuard()`，
  调用点是 `shims/d3d9.cpp:shimGetDeviceCaps()`（`fillCaps` 返回之后 —— 那里拿得到**真的**写进
  guest 的 caps）。缺一个 ⇒ `HostStopKind::Trap`，`stop_why = "caps 实现集守卫"`，读数列出
  `missing` / `extra`。⛔ 没有 gap / `declared_gap` / 白名单；readout 用 `std::printf`
  （默认可见、进程内一次）。
- **运行期闸 = `pipelineTrap()`**（`runtime/d3d9_pipeline.cpp`）：`submitPrimitives` 在
  `gpuEligible` **之前**判 `ctx.effect_draw && !ctx.effect.program.implemented` ⇒ 打印
  资源 / technique / pass / entry / profile + 寄存器 + 栈，`stop_why = "unimplemented effect
  program"`、`HostStopKind::Trap`。⛔ 不再是 P1 的 `pipeWarn` + 0 像素 + 位 44；位 44 只剩
  "程序已实现、但这一批画不了"的其余拒批（后端 / 纹理数 / 纹理 bpp / UV 布局）。
- **会建的程序集已在实现集里**（200/201/202/203 的 5 条 PS binding + 202 的 `SnowVS`）：
  `shims/d3dx9_effect.cpp:effectProgramBindings()` / VS 表。启动 readout 与守卫读数见 §8。
- **`+0xC8` / `+0xD0` 仍是自洽值**（`96` / `8.0f`）：引擎对它们零读点（§7 第 1/4 条），P4 照写。
- **设备 `Create/SetVertexShader` / `Create/SetPixelShader` 仍未实现**：被调用即
  `comUnsupported`（§2.1 的**已声明能力边界**；⛔ 不进等式）。


## 2. 目标值（用户裁决 2026-11-18）

| 字段 | caps 偏移 | 目标值 | 兑现者 / 说明 |
|---|---|---|---|
| `PixelShaderVersion` | `+0xCC` | **`0xFFFF0200`**（`D3DPS_VERSION(2,0)`） | 后端 effect 管线的 PS 程序（4 份资源 / 5 条 binding，§4） |
| `VertexShaderVersion` | `+0xC4` | **`0xFFFE0100`**（`D3DVS_VERSION(1,0)`） | CPU 顶点程序 `transformVertexVs10`（§2.1） |
| `MaxPixelShaderValue` | `+0xD0` | **`8.0f`** | 自洽值（见 §7 第 4 条） |
| `MaxVertexShaderConst` | `+0xC8` | **`96`** | 自洽值（vs_1_0 的 `c0..c95`；见 §7 第 1 条） |

★ 目标值 = **真实预期**，不是"够用就报低"：做不到的能力要 `unsupported` 响亮停，⛔ 没有"报低一档 / 登记已知偏离 /
`declared_gap` / 白名单"这一档（P4 需求单 §一句话 与 §要做 第 3–4 条）。

### 2.1 为什么 VS 必须报 1.0（⛔ 不能报 0）

- 202 的 FX 是 **`vs_1_0 + ps_1_0`**（同一 pass：`SnowVS` / `SnowPS`，见 `d3dx-effect.md` §1）。报 `VS = 0` 会让引擎
  对"带 VS 的 pass"走另一条分支 —— 比"缺能力"更糟（用户口径 2026-11-18：「按 vs=1 汇报，防止错误」）。
- **兑现者 = CPU 的 `vs_1_0`**：`runtime/d3d9_pipeline.cpp:transformVertexVs10()` 逐句照 `SnowVS` 的源文本
  （`Pos = mul(pos, Proj); UV = uv;`），由 `shims/d3dx9_effect.cpp:effectVertexProgramSupported("SnowVS","vs_1_0")`
  按 **(entry, profile)** 查表放行 —— ⛔ 不是"凡 `vs_1_0` 都放行"。
- **设备 vtable 的 `Create/SetVertexShader` 仍未实现** ⇒ 真被调用照旧 `comUnsupported` 响停（`shims/d3d9.cpp:device9Methods()`
  里没有这两个槽 + 哨兵 `kLastDeviceSlotOffset = 0x1D8`）。这是**已声明的能力边界**（"声明目标 + 做不到就 abort"），
  ⛔ 不是偏离、⛔ 不是白名单、⛔ 不进等式（§3）。
- 与 ADR 一致：caps 报的值 = 实现能力；VS=1.0 的"能力"是"引擎可以提交 `vs_1_0` 程序"（它确实会），那条**兑现得了**。

## 3. 守卫口径：硬等式 `required == implemented`

```
required    = { (resource_id, technique, pass.entry, pass.profile) }  // 对 4 条 pin 的**字节**跑 parseEffectSource
              ∪ { (vertex_entry, vertex_profile) }                   // 202 的 SnowVS
implemented = effectProgramBindings()（运行时同一张 kBindings）∪ kVertexBindings
assert  required == implemented                                       // ★ 集合相等（双向）
missing     = required \ implemented                                  // 只用于**点名**
extra       = implemented \ required                                  // 声明侧不再覆盖的实现集条目
```

- **没有"缺"这一档**：⛔ 没有 gap / `declared_gap` / 已知偏离 / 白名单。缺一个 ⇒ **启动即 `Trap`**。
  合法表达"缺什么"的唯一方式 = 启动 readout 打出 `missing={…}`，**紧接着 abort**。
- **两侧都不是手抄**：`required` 由 pin 表（`shims/d3dx9_effect.cpp` 的 `effectResourcePins()`）+ `parseEffectSource`
  从资源字节里算出来；`implemented` 就是运行时用的那两张表（改一处两边一起变 —— 守卫断言的是**关系**，不是某个具体值）。
- **等式是双向的**（`effectCapsGuardImpl`）：`missing` 与 `extra` 任一非空都 Trap。P4 之后 caps 是固定值
  （没有 env 可调）；若有人把 `fillCaps` 改低，`required` 会缩小而实现集不动 ⇒ `extra` 非空 ⇒ 同样红
  （"只改声明"没有合法路径；要降 caps 必须同时删实现集）。
- **前提由 `fillCaps` 实际写进 guest 的 `+0xCC` 提供**（不是另一个常量）⇒ caps 报高 ⇒ `required` 自动变大（**会红**）。
  ★ 这就是"先 A、后 P4"的机制原因：202 没进实现集之前，caps 一旦报 2.0 就是启动 Trap。
- **落点 = shim 层**：校验函数是 `shims/d3dx9_effect.cpp:effectCapsGuard()`，调用点在
  `shims/d3d9.cpp:shimGetDeviceCaps()`（`fillCaps` 返回之后 —— 那里同时拿得到**真的**写进 guest 的值）。
  ⛔ 不让 `runtime/` 反过来 include `shims/`（那是反向依赖）。boot 早期就有一个 `GetDeviceCaps` 调用
  （`0x499123`：`mov eax,[ecx+1Ch]` / `call eax`，`ecx+1Ch` 就是设备 vtable 的 `#7`）⇒ "启动即红"成立，
  不会拖到某一帧。
- ★ **纯夹具路径（`Machine::initBare`，没有装配 PE）**：`required` 不可计算 ⇒ 守卫只打一行 readout
  跳过等式。这不是 gap：生产 AOT 启动前核对 pin 的 PE sha256（`AGENTS.md` §12.5）⇒ `cpu.image` 恒在；
  纯夹具里没有游戏镜像，`d3d9.cpp` 的那些 caps 合同仍然照测（它们测的是 `fillCaps` 的字段）。
- ★ **等式里只有 effect program 集合，没有 VS caps 这一项**。⛔ 不许加"caps 的 VS 版本必须 ⇔ 设备 vtable 有 VS 槽"这类判据：
  那会把声明能力降回 0，正是本政策否决的方向（§2.1）。
- **运行期同一条政策**（已实施）：effect draw 命中"未实现的 program" ⇒ **`pipelineTrap()` 响亮停**
  （`runtime/d3d9_pipeline.cpp` 的 `submitPrimitives`，在 `gpuEligible` **之前**；打印资源 / technique /
  pass / entry / profile + 寄存器 + 栈），⛔ 不许 `pipeWarn` + 0 像素 + 位 44 继续跑。
  已实现程序**不许** Trap（负控：`effect-draw-recorded` / `effect-draw-blur-recorded` /
  `effect-draw-snow-recorded` 三条都要求 S_OK 且 `skipped_draws` 不涨）。
- **readout 要默认可见** ⇒ 用 `std::printf`（或 `Warn` 级 log），⛔ 不用 `d3d9Log`（Info 级，默认看不见）。
  它是**一次性**输出（`AGENTS.md` §12.8：一次性输出不许收）。

## 4. 会建的程序集（caps 报 2.0/1.0 下的 `required`）

| 资源 | 创建点（EA） | 创建条件 | program / profile | 兑现落点 |
|---|---|---|---|---|
| 200 | `0x4B0743 push 0C8h` | 等级 **== 1** | `FadePS` / `ps_1_0` | `EffectProgram::FadePs10` |
| 201 | `0x4B0790 push 0C9h` | 等级 **≥ 2** | `FadePS` / `ps_2_0` | `EffectProgram::FadePs20` |
| 202 | `0x4183AE push 0CAh` → `0x4183B6 call j_D3DXCreateEffectFromResourceA`（`sub_418340` = `Set3DEffectSnow`） | 等级 ≥ 1（`0x418346 cmp [esi+0B64Ch],1` / `jl`）**且** 脚本 `opcode 0x326`（handler `sub_426E10` · `0x426E5C call sub_418340`）**且** 该纹理槽非空（`0x418357 cmp [esi+ebx*4+0A5D8h],0`） | `SnowPS` / `ps_1_0` | `EffectProgram::SnowPs10` |
| 202 | 同上（同一 pass） | 同上 | `SnowVS` / `vs_1_0` | CPU `transformVertexVs10`（§2.1） |
| 203 | `0x4B07FC push 0CBh` | 等级 **> 1** | `SlideBlurPS` / `ZoomBlurPS`，均 `ps_2_0` | `EffectProgram::SlideBlurPs20` / `ZoomBlurPs20` |

★ **source** 那一列没写：每一行的源都是该 `resource_id` 的 **ASCII HLSL 文本**（PE `.rsrc` → `RT_RCDATA`，
4 条按**内容 sha256** pin；字节 / technique / shader 全表见 `d3dx-effect.md` §1）。`program` 列只是"哪份文本里的
哪一条 shader 由哪个后端程序兑现" —— ⛔ 不是另抄一份期望表（§3 的 `required` 是从这些字节解析出来的）。

- **等级怎么来**：`PixelShaderVersion` 对 `0xFFFF0200` / `0xFFFF0100` 归约成 2 / 1 / 0 写 `[this+0xB64C]`
  （`0x4A6FB7 cmp eax,0FFFF0200h` · `0x4A6FBE mov [esi+0B64Ch],2`；台账 `KN-01M7MPADXQ7N2H3T7Y73341V28`）。
- **为什么 `required` 取全部 4 个（而不是只取等级 2 的集合）**：还有第二个等级入口
  `0x4A6FA2`（`arg_4 ∈ [-1..2]`，≠ -1 就覆盖等级），`arg_4` 来自配置键 `display:PixelShader`（`0x406B22`）⇒
  在 PS=2.0 下等级 **0/1/2 都可达** ⇒ required = 200 + 201 + 202 + 203（+ `SnowVS`）。这样取不依赖配置默认值（§7 第 3 条）。
- **前置 = P3（203）+ A 单（202 的 `vs_1_0` / `ps_1_0`）**。⛔ "把 202 登记为已知缺失"**不再**是 P4 的合法前置替代。

## 5. 证据

| 证据 | 出处 |
|---|---|
| **用户实测**：`PS=0x200` 下转场与真机一致（除 203 的 effect 层未实现）；缺省档（caps=0）会走软件模糊、转场 40+ s 黑屏 | `d3dx-effect.md` §9（2026-11-17 macOS · 两跑对照）；P4 需求单 §为什么 |
| `caps ⇒ FX 分路 ⇒ 取代软件模糊`：`PS=0x200` 下软件模糊入口 0 命中、FX 帮助函数命中 | `d3dx-effect.md` §7（`--until <EA>` 探针读数） |
| **202 的 `Proj` 语义**（N3）：纯投影 / 行主序 + 行向量 / 顶点进来已是视图空间 | `d3dx-effect.md` §11（EA 表：`0x4531BE`…、`0x4B5716`、`0x4B5767`、`0x4B5706`） |
| **A 落地读数**：`effect-draw-snow-recorded` / `snow-vertices` / `snow-pixels`，`d3dx9-effect` 全绿 | `d3dx-effect.md` §12.2–§12.4（含复现命令） |
| 引擎只读 **5 个 caps 字段**：`+0x58`/`+0x5C` · `+0xCC` · `+0xAC` · `+0x0C`（Caps2，死读） | 台账 `KN-01M7MPA94T7E7R0B0D2758465S`（accepted）+ `runtime/d3d9.h` 的注释块（逐字段读点 EA）。⇒ `+0xC4`/`+0xC8`/`+0xD0` **引擎零读点** |

★ EA 锚自查（只读、一次可重跑）：`node tools/disasm.mjs --at --ea 0x4B5716 --lines 6`
（`lea edx,[esi+1Ch]` + `push "Proj"` + `mov eax,[ecx+98h]`）· 同形查 `0x4B559A`（`mov edx,[eax+88h]` = `+0x88 SetVector`）。

## 6. 边界

- **P4 已实施**（§1）：env 已删、caps 固定 2.0/1.0、启动硬等式 + 运行期 `pipelineTrap()` 都在。
  本单**不实现任何 shader 程序**（202 在 A 单、203 在 P3）。
- **B（GPU 透视校正插值，`REQ-01M7NC21S484PFYDAE7YTNX02J`）独立**：P4 的守卫与验收**不依赖** B 的视觉验收
  （守卫只看"声明的 caps ⇒ 实现集完备"）。反之 **202 的真 run 归 B**：`submitPrimitives` 的 `gpu_invw_ok` 对
  `invw != 1` 整批拒（`kUnGpuPerspective` = 位 21），而 202 的 `Proj` 是透视矩阵 ⇒ `clip.w = z_view ≠ 1`
  ⇒ run 级必撞（`d3dx-effect.md` §12.3 第 1 条）。A 的验收口径 = **夹具级**，⛔ 不写成"雪已支持"。
- 设备 `Create/SetVertexShader` / `Create/SetPixelShader` 未实现 ⇒ 被调用即 `comUnsupported` 响停（§2.1）：
  **已声明的能力边界**，⛔ 不是偏离、⛔ 不进等式。

## 7. 待核

1. **`MaxVertexShaderConst = 96` 的来源**：值本身是 vs_1_0 的常量寄存器 `c0..c95`（公开规范口径），
   **本仓无 SDK 头可对、无二进制对拍** ⇒ 未核。影响面小（`+0xC8` 引擎零读点，见 §5），但它是"自洽值"不是取证值。
2. **设备 vtable 位号**：`Create/SetVertexShader = +0x16C/+0x170`、`Create/SetPixelShader = +0x1A8/+0x1AC`
   是按本仓已证槽序（`#100 SetStreamSource = 0x190`、`#104 SetIndices = 0x1A0`）**推**出来的 SDK 槽序，
   没有 SDK 头或二进制 vtable 反查作证 ⇒ 位号待核。结论"不在方法表 ⇒ 被调用即 `comUnsupported`"**不依赖**位号。
3. **配置键 `display:PixelShader`（`0x406B22`）的缺省值**：决定等级 0/1/2 哪一档在缺省配置下真可达。
   `required` 取全 4 个正是为了**不依赖**它；若将来观测到缺省返回会压等级，再改这条口径。
4. **`MaxPixelShaderValue = 8.0f`**：D3D 对 ps_2_0 的常见值，引擎零读点 ⇒ 只为自洽，不是取证值。
5. **202 在正常 play 里是否可达**（雪景 / `opcode 0x326` 是否被执行）：既有 E2E 读数只记到 201/203 创建，
   **没有 202 创建记录** ⇒ run 级可达性未观测。一次可跑的判据：`--until 0x00418340`（`Set3DEffectSnow` 的函数入口，
   `--until` 只在块边界停）；出现 `stop: until-ea @ eip=00418340` ⇒ 可达。

## 8. 读数（2026-11-18 / win32 / RelWithDebInfo / `windows-x64`）

- **目标档 ctest**：`cd apps/emulator-lift && ctest --test-dir out -R 'caps|d3dx9-effect|d3d9'`
  ⇒ **6/6 通过**（`d3d9` · `d3d9-gpu` · `d3d9-pipeline` · `d3d9-pipeline-gpu` · `d3d9-mip` ·
  `d3dx9-effect`）。全量 `ctest` ⇒ **107/107 通过**（同一读数口径见 `.tmp/caps-p4/impl-report.md`）。
- **启动 readout（真镜像）**：`LIFT_AGE_EXE=corpus/assets/age/AGE.EXE__dumped.sectfix.EXE` 跑
  `out/tests/lift-d3dx9-effect.exe`，stdout 原文（一次）：
  `caps      : PixelShaderVersion=0xFFFF0200 VertexShaderVersion=0xFFFE0100 ⇒ 等级 2 ⇒ 会建 200/201/202/203；`
  `required = 5 pixel + 1 vertex（{id=200 technique="Fade" ps_1_0 FadePS；id=201 … ps_2_0 FadePS；`
  `id=202 … ps_1_0 SnowPS；id=203 … SlideBlurPS；id=203 … ZoomBlurPS}），implemented = 5 + 1 ⇒ 硬等式成立`
  ⇒ 200 / 201 / 202（PS+VS）/ 203（两个 technique）全在实现集里。
- **注入读数**（`d3dx9-effect/caps-programs-guard` ②③，测试接缝 `effectCapsGuardForTest`）：
  从实现集拿掉 202 的 PS binding ⇒
  `missing（required \ implemented）= {id=202 technique="Snow" ps_1_0 SnowPS}`；
  拿掉 VS 表 ⇒ `{vertex vs_1_0 SnowVS}`。两次都 `cpu.running == false` +
  `HostStopKind::Trap` + `stop_why = "caps 实现集守卫"`。
- **运行期读数**（`d3dx9-effect/effect-draw-unimplemented-traps`）：自造 `ps_2_0 NopePS` 的 pass ⇒
  draw 回 `D3DERR_INVALIDCALL`、stderr 有 `=== D3D9 pipeline trap ===` 与
  `资源 id=901 technique="T" pass=0 … entry=NopePS profile=ps_2_0`、`stop_why = "unimplemented effect program"`；
  出 pass 后同一份几何回 S_OK（负控）。
- **复现命令**：

```bash
cd apps/emulator-lift
cmake --build --preset windows-x64 --parallel 4
ctest --test-dir out --output-on-failure -R 'caps|d3dx9-effect|d3d9'
ctest --test-dir out --output-on-failure                 # 全量
cd ../.. && node tools/mutate-check.mjs --match '硬等式降级'
node tools/mutate-check.mjs --match 'SnowPS/ps_1_0'      # 删 202 binding ⇒ 启动 Trap
```
