# CTX — 一次性交接文档（读完可删）

> 生成时间：2026-11-11 ~11:00（Asia/Shanghai）。仓库：`E:\Projects\amayui-re`。
> **这份文档不依赖任何需求单**：只靠它 + 工作树就能接手。需求单号只作索引出现，内容都抄在这里。
> 交接原因：设备要换。**工作树有大量未提交改动（无任何 commit）**——必须连工作树一起搬走。

---

## 0. 一句话现状

`apps/emulator-lift`（C++/CMake，win32 x64，preset `windows-x64`，build dir `apps/emulator-lift/out`）本轮做了 **14 项修复**，
其中最有价值的是查清并修好一条 **S1**：GPU 臂整层丢失 Live2D 角色（`SdlWindow` 漏转发 `rasterRequestDepthClear`）。
**默认档全绿**：`ctest` **81/81** · `pnpm test` **546/0** · `pnpm tools requirements validate` **6/6** · `git diff --check` 干净。

---

## 1. 环境与门禁（先跑这三条）

```bash
cd apps/emulator-lift && cmake --build --preset windows-x64 && ctest --test-dir out     # 期望 81/81，约 36 s
cd ../.. && pnpm test                                                                   # 期望 546 pass / 0 fail
pnpm tools requirements validate                                                        # 期望 6/6
```

**已知的红（pre-existing，不是本轮引起，别顺手修）**：`pnpm tools lift check` **4 条**
（`kernel32!RaiseException` / `kernel32!RtlUnwind` 在 C++ 里是 `add`、JSON 里写成 stub；另有两条 `FAMILY_STUB_APPROVED` 白名单漂移）。

---

## 2. 本轮已收口（14 项，每项都有能红的守卫）

| # | 改了什么 | 守卫（CTest / 用例） | 关键读数 |
|---|---|---|---|
| 1 | `SetRenderTarget` 绑定后**回写 viewport**（D3D9 文档要求按新目标全幅）；顺带在 `rasterTriangle` 加了**深度矩形边界**保护（RT > 后缓冲时 z 只在深度矩形内读写） | `d3d9p/set-render-target-viewport`（CTest `d3d9-pipeline`） | 变异红 14 条断言 |
| 2 | **状态块真快照**：`CreateStateBlock` 建块即捕获、`Capture` 重捕、`Apply` 写回（含绑定 AddRef 重建、掩码按 `D3DSBT_*` 分侧） | `d3d9/stateblock-capture-apply` | 修前 31 红；画面：矩形块 `x=780 run=254 → 42` |
| 3 | **PNG 解码 + 上传**：`D3DXCreateTextureFromFileInMemory` 从"只读文件头"改成真解码（libpng 简化 API），像素走既有 `lockRect/unlockRect`；`find_package(PNG)` + `vcpkg.json` 加 `libpng` | `d3dx9/texture-from-memory-png` · `d3dx9/png-decode` · `d3dx9/texture-upload-rollback` | `texa` 0/171 → 627/627 行 mean>0；Live2D 角色出现 |
| 4 | **LZSS Encode 整函数 native override**（`sub_4367B0`）：纯核原地改 guest `0xD430` + 薄 ABI 包装；`kMax` 13→14 | `native-override-e2e` · pure `lzss/*` 6 例 · assets 9 档 | 尾段指令 131,104,827 → **8,495,927**；墙钟 7681→**650 ms**（11.8×） |
| 5 | **GPU 准入保守化**：`mag_filter != min_filter` ⇒ 不接受（回退 + 记账） | `gpu-batch/sampler-filter-eligibility` | 删准入 ⇒ 6 条红 |
| 6 | **mip 链 + LOD**：宿主旁路链（键 `serial`、`version` 失效、box filter）、透视校正导数选级、GPU 多级上传、`MIPFILTER=NONE` 显式回退 | `d3d9-mip/lod-levels` · `d3d9p/mip-levels` | 62 批里 53 批缩小 >1.5×；10 批真取到 `lvl≥1` |
| 7 | **参数边界**：`SetRenderState` 的 `state` 上界 / 状态块 vtable `+0x18` 哨兵 / VB·IB `Lock` 越界 | `d3d9p/render-state-bound` · `d3d9/stateblock-vtable-sentinel` · `d3d9/lock-overrun-invalidcall` | 删哨兵 ⇒ 停因 `DecodeError`（野跳），不是 `Trap` |
| 8 | **点/线图元真光栅**：1px diamond-exit、共用 `writeShadedPixel`（深度→shade→alpha test→混合→写掩码→z 写，**唯一一份**）；带纹理/需采样 ⇒ `INVALIDCALL` 零像素 | `d3d9/linelist-writes-pixels` · `linestrip-16` · `line-axes-and-direction` · `line-textured-loud-reject` | 短路回 `break` ⇒ 19 红 |
| 9 | **GPU 账本可观测性**：拆 `queued / recorded / empty`，`recorded` 判据 = **命令缓冲提交成功后**回报批号；summary 增字段 | `gpu-batch/ledger-queued-vs-recorded` | 修前 `--gpu-strict` 对"整层丢失"恒绿 |
| 10 | **S1**：`SdlWindow`（present_sdl）漏转发 `rasterRequestDepthClear` ⇒ `Clear(ZBUFFER)` 从未到 GPU ⇒ 每帧末尾 `z=0` 全屏 quad 把深度写成 0 ⇒ 下帧 `z=0.5` 的 FF 批全灭。补转发 + 12 处转发收到唯一取用点 `impl_->raster()` | `gpu-batch/depth-clear-forwarded`（**穿过壳**：注入 Mock） | 角色区 `TH6` 0 → 3.954%；`[gpu-depth] arm=clear` 0 行 → 每帧 1 行 |
| 11 | **GPU 黄金图回归集**：比较器（16×16 块 `max|Δ|`≤1.5 · 全图 `mean|Δch|`≤1.5 · 单像素>16 ≤0.25%）+ 首条 fixture（TITLE f2）+ LFS 黄金图 | `golden/compare-self-passes` 等 4 例（pure）· CTest `golden-image`（assets，可 SKIP 且有原因） | 实测噪声 0（两次连跑 BMP 逐字节同） |
| 12 | **`CheckDeviceFormat` 诚实**：支持集 = 纹理创建侧同一份；`TEXTURE` 只收 `Usage∈{0,RT}`（RT 限 32bpp）、`SURFACE` 仅 `DEPTHSTENCIL+D24S8`、未知一律 `NOTAVAILABLE` | `d3d9/checkdeviceformat-truthful` | 修前 13 failures |
| 13 | **`--dump-frame` 目录不存在时空转**（实测 546 s）：预建目录 + 失败 `exit 2` + 写失败计数 `dumpFailures=` | `cli-dump-frame` · `frame-dump-failure` | 546 s → **6.96 s**；根因是 BMP 写失败让 Present 提前 `kFail`，帧数上限永远触发不到 |
| 14 | **L1 契约补 5 行**（`docs/03-emulator/contracts-win32.md`）：`L1-D3D9-06..10` = 状态块 / mip+LOD / 线图元 / `CheckDeviceFormat` / `Clear(ZBUFFER)` 穿壳 | 每行都写了 `文件#用例名` | — |

另有一条**按"不是缺陷"收口**：`ID3DXSprite::Begin` 的 `ALPHABLEND` 假设。
实测 **1013/1013 次 Begin 全传 `0x10`**（`D3DXSPRITE_ALPHABLEND`；`0x1` 是 `DONOTSAVESTATE`）⇒ 无触发面，不改、不加守卫。

---

## 3. 在飞 / 半成品（**接手第一优先**）

### 3.1 `quit-tail` 夹具锚不确定（改动在工作树，未提交）

**症状**：夹具脚本锚是 `at frame 130`；boot 出现帧停滞时该锚被折算成**墙钟**触发 ⇒ 点不到 TITLE 的 Quit ⇒ 两臂都停在 `--time-limit 600` ⇒ 判据数由墙钟决定。
**这就是"一个 test 要 20 分钟"的全部原因**：`2 臂 × 600 s = 1200 s`。

**已改**（`apps/emulator-lift/tests/quit_tail.cpp`，+188/−15；**同时含上一单的路径编码修复**）：
1. 锚 → `at guest_ms 120000`（依据：Quit 点实测 `guest_ms=87991`，+32 s）；
2. 判据加两条：锚必须命中（`waited_samples>0`）、停因必须 `stopped` + `detail` 含 `kernel32!ExitProcess`（⛔ 不许被 `--time-limit` 兜底成通过）；
3. **顺带修掉第二个不确定源**：`--appdata` 跨调用复用 + 退出写 `SAVE.DAT` ⇒ 两次 `tail` 差 13M（131,104,827 vs 143,751,706）⇒ 改成每次 `remove_all`；
4. `tools/mutate-check.mjs` +1 条变异（锚退回 `at frame 130` ⇒ 红）。

**读数**：修前 `24 checks / 1 failure`（1203 s，两轮 time-limit）⇒ **修后 `43 checks / 0 failures`（219.9 s），再跑一次 244.98 s 也 0/0**；
两次 `tail_insns=131104827`、`waited_samples=3`、`click frame=1472/1473`、`stop=stopped`。

**还没验证完（就是交接要补的）**：
- 最终变异的**完整一轮**（变异体红 + 还原后基线复验）：变异体红**观察到过**（20 checks/5 failures、601 s、"锚未命中 n=0"），但那是在 appdata 修复**之前**；修复后**没重跑**。
  ⇒ **这是唯一真正欠的一项**。跑法：`$env:LIFT_ASSETS='1'` 重新 configure + build → `node tools/mutate-check.mjs --match 01M745ZGKZAHTCEQ9KQ7CY3JJ0` → 恢复默认 configure（`ctest -N`=81）。
- 默认档 `ctest` 81/81 **未实跑**（默认 configure 已恢复，`ctest -N` = 81；我刚才接手时跑过 **81/81 绿**，见 §1）；
- `pnpm test` **546/0 已跑过**（同上）；
- 独立复审 subagent：**已完成**，结论见下（5 项通过、1 项必修 = 变异重跑、2 处非阻塞笔误）；
- `report.md` 只写到"读数 + 变异前半段"（§5/§6 仍是"待回填"）。

**只读复审结论（2026-11-11，未改文件/未构建/未跑 CTest）**：

| 复核项 | 判定 | 依据 |
|---|---|---|
| 锚 = guest 时间 | ✅ | `quit_tail.cpp:114-115` 只有一处 `kGuestAnchorMs="120000"`；依据在 `:44-58`（Quit 点 frame130 = `guest_ms 87991`；120000 = +32 s ≈ 4.3× 实测 boot 差） |
| 锚未命中响亮失败 | ✅ | `:512-526` 五条 `LIFT_CHECK` + stderr「锚未命中（n=0）」；`time-limit` 属 `ok()` ⇒ exit 0，只有 ⑦⑧ 拦得住（10:37 mutant 日志实测 5 红） |
| 停因判据 | ✅ | `:531-532` 要求 `stop` 以 `stopped` 开头且 `detail` 含 `kernel32!ExitProcess`。nit：`startsWith` 等价于 `==`，写成 `==` 更硬 |
| `--appdata` 第二不确定源 | ✅ | `:422-425` 每 run `remove_all` + 重建、清不掉即红；子进程 `CREATE_ALWAYS`（`:137`）不会串读旧日志 |
| 变异会红 | ⚠️ **推理成立、证据过期** | boot 前 30 s 无 Present ⇒ `input_script.cpp:1199-1214` 把 frame 折成 wall（`frame=0/waited=0/bounded=1`）；即使不 fold，frame130 也早于 TITLE（Quit 在 frame≈1472）⇒ time-limit ⇒ ⑧ 红。**但唯一红读数跑的是 appdata 修复前的文件** ⇒ 必须重跑 |
| 脆断言 | ✅ 没有 | `:546-547` 是"两次相同"；`131104827` 只在注释且有来源 |

另两处非阻塞笔误：`tools/mutate-check.mjs:2726` 的「② 之前」、`:2747` 的 what 写"两轮停在 time-limit"（实际第一份 run 就 break）。
`--time-limit 600→180` 的建议**留给下一单**，不在本次收尾里做。

**⛔ 两个文件不要整文件 revert**：
- `tests/quit_tail.cpp`：里面同时有上一单的**路径编码修复**（`wstring` + `CreateProcessW`，实测 `12/7 → 24/1`）；
- `tools/mutate-check.mjs`：含上一单新增的 `optInEnv` 第三态（未设 env ⇒ `⏭ 本机未启用`，不是 `⛔`）。

**跑它的方法**（opt-in，约 4 分钟/次；跑完必须恢复默认 configure）：
```bash
cd apps/emulator-lift
cmake --preset windows-x64 -DLIFT_ASSETS=1          # 或按 CMakeLists 里的闸位
cmake --build --preset windows-x64 && ctest -R '^quit-tail$' --output-on-failure
cmake --preset windows-x64                          # 恢复，之后 ctest -N 应回到 81
```

**建议**：把 `--time-limit 600`（现在只是兜底上限）降到 **180 s**，让未来任何锚失效 3 分钟就红而不是 10 分钟/臂。

### 3.2 `quit-tail` 路径编码（同一文件；功能已好，单还挂着）

`fs::path::string()`（Windows = ANSI 936）→ `utf8ToWide()` 严格解码失败 → `--game-dir` 丢成空串。
已改成 **Windows 子进程命令行只用原始 UTF-16**（`CreateProcessW` + `path::wstring()`）→ 子进程 `argvUtf8()` → `main` → `vfs::pathFromUtf8`；
POSIX 保持 `.string()`（原生 UTF-8）。与 `title_entry_paths.cpp` 一致。
**同族巡检**（只报告、未改）：`host_window.cpp` / `agerc_module.cpp` / `interaction_graph.cpp` 有同类 `.string()` 风险（当前 ASCII 路径不出事）。

---

## 4. 还没做的工作（按优先级）

| # | 缺口 | 位置 | 判据建议 | 触发面 |
|---|---|---|---|---|
| 1 | `D3DRS_COLORVERTEX(141)` + 三个 `*MATERIALSOURCE` 只存不读 ⇒ 顶点色该不该参与光照错 | `runtime/d3d9_pipeline.cpp` 的 `transformVertex` | 开光照 + 顶点色红 + 材质蓝：`COLORVERTEX=0` ⇒ 走材质路径 | 引擎是否写这 6 个状态 `<待核>`（语义正确性单，不是"已观测到偏色"） |
| 2 | `Clear(ZBUFFER)` **先 arm 后 flush** ⇒ CLEAR 落在含更早 draw 的 pass 开头（窄窗不对称） | `runtime/d3d9.cpp` 的 `clear` | 队列非空时 `Clear(Z)` 必须先 flush/新开段 | 实测命中过（`arm=clear seq=0 queue=1`），但那条是关深度的 sprite ⇒ 无后果 |
| 3 | sprite 合成目标恒为后缓冲（不看 `kPlColorSurface`/viewport/scissor） | `runtime/d3dx9_sprite.cpp` 的 `compositeBatch` | RT 存在时 sprite 必须写 RT | 今天引擎无 RTT（`SetRenderTarget` 恒 NULL）⇒ 潜在 |
| 4 | `GetDeviceCaps` 与实现脱钩（`LineCaps` 等；`Caps/Caps2` 恒 0） | `runtime/d3d9.cpp` 的 `fillCaps` | caps 位由"同一份实现集"生成 | 引擎只读 `MaxTextureWidth/Height` |
| 5 | `ValidateDevice` 恒 `S_OK,1` | `runtime/d3d9_pipeline.cpp` 的 `validateDevice` | 复用 `drawPrecheck` 判据 | 无 `.lst` 站点 |
| 6 | `GetBackBuffer` 每次新建对象 | `shims/d3d9.cpp` 的 `shimGetBackBuffer` | 连续两次同 EA + refcount=2 | 影响"同一目标"判据 ⇒ 每次 `SetTarget` 走屏障 |
| 7 | `MinIndex` / 负 `BaseVertexIndex` 被丢 | `d3d9_pipeline.cpp` 的 `drawIndexedPrimitive` | 区间 `[min, min+n)`；`base` 按 int32 | 两站点都是 `0/0` |
| 8 | `SetRenderState` 的 `default:` 静默接受一切 | 同上 `setRenderState` | 未实现状态必须 `recordUnsupported` | sRGB 写等 |
| 9 | GPU 侧 FF 覆盖率可疑（账本里 `fragments` 实为**顶点数**） | `runtime/d3d9_gpu.cpp` | 逐批"应有像素 vs 实得像素"对账 | 92 万像素 vs 124 批 |

**需要人（agent 不能替）**：
- **右侧 `(1060,40)200×640` 的 1px 暗短划**：已排除采样实现/精度/UV 越界/mip（A/B 逐像素差 **0/128000**）/纹理内容（level 0 哈希 == guest）/状态泄漏/视口/深度。
  现在事实是"这层 1:1 细点网格是页内容/alpha/混合的产物"——**要对真机一帧才能判它对不对**。⛔ 不要用"强行取更高 mip"压 `row_hf` 掩盖。
- **GPU 侧 FF 覆盖率**要不要当产品缺陷（取舍）。

---

## 5. 已否证（**别重查**）

1. 纹理副本陈旧（`[gpu-submits]` 的 `uploads/full` 是**累计值**；第二帧 0 次上传；`texa=` 静态）
2. guest 覆盖镜像擦掉已提交的批（`rasterSyncFromGuest` 显式拒绝非空队列）
3. 深度**清值**（日志 137 次 `Clear` 的 guest z 全 `1.000000`）
4. 视口二次映射（整跑**零次** `SetViewport`；`vp==tgt` 124/124）
5. scissor 裁屏（`sc` 全幅 124/124）
6. 没进 render pass（`rec=1` 122/124）
7. mip 回归（`bin3` 无 mip 代码但其 GPU 帧与 `bin6` **逐字节相同**）
8. `LIFT_NO_MIP` 对 GPU 无效（该开关只被 **CPU** 光栅器消费）
9. FF 管线本身（无纹理 FF 批在 GPU 臂与 CPU **逐点吻合**）
10. 顶点链（布局 `FLOAT3@0/FLOAT4@12/FLOAT2@28` 与 shader 对齐；无共用 staging）
11. 三张 2048² 图集内容（level 0 **整幅哈希 == guest**，单 texel 四通道也相同）
12. mip 选级公式（对该几何实测 `lod=1.506 ⇒ level 2`，正确）；右侧真实三角形全是 **1:1**（0.89–1.0 texel/px）⇒ level 0 是 D3D9 语义要求
13. `ID3DXSprite::Begin` 无 ALPHABLEND（1013/1013 带 `0x10`）

---

## 6. 口径（本轮改过，别再按旧的理解）

- **CPU 光栅 = oracle / 调试**，**GPU 光栅 = product**；真源 = **D3D9 语义 + 引擎行为**（两条后端都是投影）。
  已写进 `docs/00-origin/decisions.md` §8.1。
- 一致性三层：**能力层**（`gpuEligible` 保守 + 回退记账）· **夹具层**（GPU 黄金图，自己与自己比）· **调试层**（`LIFT_GPU_VERIFY` 按需，不作门禁）。
  ⛔ 取消"每帧逐像素等于 CPU"的全局契约。
- **`--gpu-strict` 的真实语义**：运行结束后判 `fallback_draws + sprite_fallback_draws > 0` ⇒ 打印原因清单 + `exit 3`（**不是**"碰到回退就中止"）。
  但它的记账曾是"入队即覆盖"（已修成 `recorded/empty` 可观测；**通过条件未动**，用户裁决"这里不重要，有记录就行"）。
- `native-overrides` / `real-time` 属**时钟轴**，与 raster 选型无关；确定性按 **flags 集合**定义。
- 黄金图容差：16×16 块 `max|Δ|` ≤1.5 · 全图 `mean|Δch|` ≤1.5 · 单像素 >16 比例 ≤0.25%（**不用整帧哈希**；实测两次跑亮度档数会漂 243 vs 248）。

---

## 7. 代码地图（本轮新增/改动，按文件）

- `src/runtime/png_decode.{h,cpp}`（新）：libpng 简化 API → RGBA8。
- `src/runtime/golden_image.{h,cpp}`（新）：分块比较器 + PNG 编码（复用 `png_decode`）；`tests/golden_image.cpp` + `tests/golden/title-f2-gpu.png`（**LFS**）。
- `src/runtime/native_overrides.{h,cpp}`：`kMax` 13→14；`lzssEncodeObject` + `nativeLzss4367B0`；`tests/native_override_e2e.cpp`（新）。
- `src/runtime/d3d9_pipeline.cpp`：`writeViewportFullTarget` · `writeShadedPixel`（三角形/点/线共用）· `setRs` 上界 · Lock 越界 · `triangleMipLevel`/链 · `gpuEligible` 的 `mag==min` · 账本 `recorded/empty` · `[draw]` 的 `rec/vp/tgt/sc/z` 字段。
- `src/runtime/gpu_raster.h`：批号通道 `gpuRasterSetBatchTag` / `gpuLedgerNoteRecordedDraw`；mip 纯函数判据。
- `src/runtime/d3d9_gpu.cpp`：mip 多级上传（`num_levels`/`max_lod`/`cycle`）· 诊断门控 `LIFT_GPU_FF_TEX` / `LIFT_GPU_ZTEST` / `LIFT_GPU_DEPTH_CLEAR` / `LIFT_GPU_DEPTH_TRACE` / `LIFT_MIPDBG`（**默认全关**）。
- `src/runtime/present_sdl.{h,cpp}`：`rasterRequestDepthClear()` 转发 · 12 处转发收到唯一取用点 `impl_->raster()` · 夹具缝 `setGpuRasterForTest`（运行时检查，`/DNDEBUG` 下 assert 会被编掉）· CRLF→LF（`i/lf w/lf`）。
- `src/shims/d3d9.cpp` / `src/runtime/d3d9.cpp`：状态块快照 · `+0x18` 哨兵 · `checkDeviceFormat` · `createTextureState` 的 mip 链。
- `src/shims/d3dx9.cpp`：`uploadRgbaToTexture`（只经 `lockRect/unlockRect`，解锁前重取 `TextureInfo`）。
- `src/main.cpp`：`--dump-frame` 预建目录 + 失败 `exit 2` + summary 打 `dumpFailures=`。
- 新测试：`d3d9_mip.cpp` · `mip_e2e.cpp` · `native_override_e2e.cpp` · `golden_image.cpp` · `cli_dump_frame.cpp` · `frame_dump_failure.cpp`。

---

## 8. 踩过的坑（别再踩）

- `--dump-frame <dir>` 的目录**曾经**必须预建，否则空转很久（**已修**：现在会自动建；但 `--dump-frame` 目录若非法会 `exit 2`）。
- **assets 档**要先 `LIFT_ASSETS=1` 重新 configure；跑完必须恢复，否则默认门禁的条数会变。
- `quit-tail` 现在是 **~4 分钟/次**（以前 20 分钟），因为它会跑两臂；⛔ 不要因为它慢就把它删了。
- `mutate-check.mjs --list` = 112 条；新增条目要带 `optInEnv`（opt-in 的 CTest）。
- 变异工具遇到"基线本身红"会报 **exit 4 / 不可判定**（那不是变异没生效，是基线坏了）。
- PowerShell 看 junction 要用 `lstatSync`（本仓 Node 侧口径），不要用 `Get-ChildItem -Directory`。
- 单文件行数预算：需求单正文 ≤ 80 行（超了 `validate` 会红，要压）。

---

## 9. 推荐的第一步（按顺序）

1. `cmake --build --preset windows-x64 && ctest --test-dir out` ⇒ 期望 **81/81**；若不绿，先看是不是 `tests/quit_tail.cpp` 的半成品（§3.1），**只修锚那部分**。
2. 跑一次 assets 档 `quit-tail`（~4 min）确认 `43 checks / 0 failures`；然后**补那一轮变异**（这是唯一欠项）：
   `$env:LIFT_ASSETS='1'` 重新 configure + build → `node tools/mutate-check.mjs --match 01M745ZGKZAHTCEQ9KQ7CY3JJ0`（红 + 还原基线绿）→ 恢复默认 configure（`ctest -N`=81）。
3. 复审已完成（§3.1 的表，5 项通过）⇒ 回填 `report.md` §5/§6 + 顺手改两处笔误，即可收口那张单。
4. 从 §4 表里按优先级挑：**`D3DRS_COLORVERTEX`（唯一会在 3D 场景现形的）** → `clear` 的 arm 顺序 → 其余潜在项。
5. §4 末尾两条"需要人"的，登记 + 绕开，⛔ 不要猜。
