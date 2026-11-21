# Present 每帧整幅回读 · Phase A（按需回读 + 诊断 opt-in + 惰性 `lastHash`）

**口径（⛔ 不是 DX9 语义）**："guest 后缓冲在帧边界权威"是**我们的模型**（Phase B 才动它）。
真 DX9 的后缓冲内容在 GPU，CPU 读回只有 `GetRenderTargetData` / lockable back buffer 的 `LockRect`
两条路；shim 已在这两条 + `UpdateSurface` / `LoadSurfaceFromSurface` / 纹理 LockRect /
`SetRenderTarget` 旧目标 / `Clear` 各有一道**显式**屏障。Present 里的整幅回读是"模型 + 诊断"顶出来的，
不是 D3D9 要求。

## 消费者（结论 + 符号锚；行号随改动漂移，按符号 `rg`）

- **要 guest 像素**（Present 当场回读）：
  - `SdlWindow::submitFrame` 的 renderer 回退路径（`SDL_UpdateTexture(arena.pixels)`，`present_sdl.cpp`）；
  - `HeadlessPresenter::present` 的 `--dump-frame` 分支（`present.cpp`）；
  - `--frame-log` / `--profile` / `FrameSampler`（`setFrameHash` / `sampler_`，`present.cpp` / `aot_run.cpp`）；
  - 控制层 freeze（`--control-root`；`freezePresentedFrame`，`present.cpp`）；
  - `--log-level info` 的 frameStats（`d3d9::present`，`d3d9.cpp`）。
- **不要**：GPU 直出（target 才是最新；`SdlWindow::submitDirect`）、overlay / UI / 插件 /
  `--present-screenshot`（读窗口 / target，不读 guest）、`present-tail` / `lastHash` 的缺省摘要（收口补算一次）。
- 7 个**显式**读点（`GetRenderTargetData` / `LockRect` / `UpdateSurface` / `LoadSurfaceFromSurface` /
  纹理 `LockRect` / `SetRenderTarget` 旧目标 / `Clear`）与 `Clear`→`rasterFlush` 的屏障**一字未改**（Phase B）。
- 叠层 / UI / 插件 / 截图**不读** guest 的硬证据：`src/ui/**` 对 `arena` 零引用；直出叠层只画 swapchain
  （`d3d9_gpu.cpp` 的 overlay pass）；开/关 `--overlay` 的 `--dump-frame` 逐字节相同（`tests/aot_overlay.cpp`）。

## Phase A 设计（零语义改动）

- `PresentSink::needsGuestPixels()`（非纯虚，默认 false）由 `SdlWindow` 实现为
  `!(impl_ && direct_present && direct != nullptr)`（三个条件：直出真的建起来了才不要 guest 字节）；
  `d3d9::present` 内联 needs 谓词：`sink 要 || dump || presenter.wantsGuestPixels() || frameStats`。
  无消费者 ⇒ 只 `rasterFlush`（不下载、不等 fence）⇒ `materialized=false`。
- **dump 拒写旧帧**：`materialized=false` 时 `present()` 不写盘、`dumpFailures++` + 一次 warn；
  退回路径的 `dumpLastFrame` 没有 "Present 时 recorded hash" 时 `hash_still_matches=false`（⛔ 不拿现算值冒充）。
- **惰性 `lastHash`**：`0 = 本帧没算`。`captureLastFrameHash()` 只在收口（1 次/run）与控制层
  `sample`/`screenshot` 时补算；冻结副本优先；无副本时只在 `gpu->rasterTargetBase() ==
  last_arena_->pixels(last_surface_)` 时 `rasterSyncToGuest` 补一次（**会写 guest 后缓冲**，已接受），
  不匹配/失败 ⇒ `hashFailures++` + 一次 warn + `lastHash()` 保持 0。
- `LIFT_PRESENT_EAGER=1` = 回退开关（强制每帧回读 + 每帧 hash）；缺省不设、不进 `--help`。
- 守卫：CTest `present-readback`（pure，Mock `GpuRaster` + 真 D3D9 设备夹具），判据见
  `apps/emulator-lift/tests/present_readback.cpp` 头注释；变异条目见 `tools/mutate-check.mjs`。

## A/B 读数（无仪器）

平台：macOS arm64 · RelWithDebInfo · `apps/emulator-lift/out`（单树）；日期 2026-11-20。
两臂 = 改动前构建（`git checkout 7f108ac` + 同一 preset 构建，复制成临时文件）/ 当前
`out/emulator-lift-aot`；交错 3 对，每跑先 `apps/emulator-lift/scripts/clean-appdata.sh <私有 appdata>`：

```bash
./out/emulator-lift-aot --present window --present-hidden \
  --game-dir ../../links/install --appdata <私有> --skip-logo --audio off --max-frames 600 \
  --input-plan scripts/recipes/save05-move.plan
```

| 臂 | wall（3 runs，s） | 中位 |
|---|---|---|
| base | 19.45 / 19.15 / 18.70 | 19.15 |
| new | 19.30 / 18.24 / 18.82 | 18.82 |

差 −1.7%（逐对 −0.15 / −0.91 / +0.12）⇒ **低于本机 5% 分辨力，不宣称收益**。两臂 `frames=600`、
`stop @ eip=00498AE4`、`region : arena in_use=… peak=… surfaces=…`、`table … misses=` 逐位相同
（`lookups` 每轮抖动、`lastHash` 每轮不同 ⇒ 都不是指纹，`AGENTS.md §12.6`）。

- base 臂按 `git checkout 7f108ac` + 同一 preset **重建**；⛔ 不必保留 19 MB 的 exe 副本（库外临时文件）。
  Phase B 的 A/B 若还要 base，先核对新构建的 `stop @ eip` / `frames` 与本表两臂一致。

## Phase B（`Clear` 走 GPU 侧 / guest 后缓冲从"权威"降为"派生"）

需求单：`REQ-01M7W2FR9EJEMG2QDMZNG6N7WV`。**口径**：「guest 后缓冲在**每个帧边界**权威」是**我们的
模型**（⛔ 不是 DX9 语义）；本阶段把它降成**派生** —— 全幅 `Clear` 不再"整幅下载 + 在 guest 里填色"，
而是把清值交给 GPU 侧的 `LOADOP_CLEAR`；guest 字节只在 7 个显式读点 + 诊断点**就地**材质化。
⛔ **不做 per-target 镜像**（要改第 6 个读点 `SetRenderTarget` 旧目标 + "RT 当纹理采样"的路由 ⇒ 另一张单）。

### 实现要点

1. 接口 `GpuRaster::rasterRequestColorClear(bgra8_pixel)`（`gpu_raster.h`）+ **转发壳** `SdlWindow`
   （`present_sdl.h` / `.cpp`）。漏转发 = 基类空实现 = 清色**静默**丢（同 `rasterRequestDepthClear`
   那次 Live2D 整层丢失的形状）。与深度版的**三处不同**：绑 target 身份 / 后者覆盖前者 /
   **arm 即「镜像 != guest」**。
2. **`rasterRequestColorClear` 里 `dirty_ = true` 是本阶段最承重的一行**：`rasterFlush` 的
   `if (had_draws) dirty_ = true` 对 **clear-only** 的 flush 不成立 ⇒ 没有这一行，清完之后
   `rasterDirty()` 回 false，而 target 已经是清色、guest 还是旧的：三条 `!rasterDirty()` 路径
   （`d3d9_pipeline.cpp` 的 guest 上传 / `d3dx9_sprite.cpp` 的同形判断 / `present_sdl.cpp` 的
   `submitDirect`）会把旧 guest 上传回去**盖掉清色**，所有诊断点也会读旧帧。
   ⛔ 别改成"`rasterDirty()` 里加 arm"：那只覆盖"arm 还没被消费"的那一半。
3. `rasterFlush`：空队列早退、target 空判、`seg_has_draws` 三处都放宽到含"待执行的 color clear"
   （清-only 也必须真的提交一次）；清值 = 调用方给的 **BGRA8 4 字节**，`SDL_FColor` 是 `r,g,b,a`
   ⇒ 通道**反着**取（换错只差"清成另一个颜色"，图形判据照样绿）。`rasterPrepare` /
   `rasterSyncFromGuest` / `rasterDiscardTarget` 三条路丢弃 arm —— **先消费再丢**。
4. `d3d9::clear`：改道判据 = **`rasterTargetBase() == base && Count == 0`**（`base` = 本次 `Clear` 的
   那个 surface 的 guest 像素），⛔ 不是"== 后缓冲"。`mirror != base` 那一支**逐字保留**（只 flush），
   且**CPU 填色照旧**（今天那次 sync 是条件性的、CPU 填色是无条件的 ⇒ 这一支 guest 仍被填对）。
   `cleared_pixels` 在 arm 分支补整幅计数（否则 `[Clear]` 日志写"清 0 个像素"）。

### 判据（三条可红；⛔ 需求单原来的「dump 首帧必须含非背景像素」作废）

首帧实测是**黑**（`tests/aot_overlay.cpp` 头注释）⇒ 旧判据会打在**正确**实现上。

- **B1（dump 档）**：`--max-frames 3 --dump-frame <dir>/f{frame}.bmp` + `LIFT_GPU_SUBMITS=1`
  ⇒ 每帧都有 BMP，且该帧 `readback>=1`（dump 是消费者；触发漏了当场红）。
- **B2（无 dump 档）**：逐帧 `rb_clear==0 && rb_present==0`，且凡
  `readback - rb_clear - rb_present > 0` 的帧必须能**归因**（见下一条）。
- **下载归因（⛔ 不以 `readback=0` 为目标）**：判据是"**非读点下载 = 0**"，不是"总下载 = 0"。按 DX 预期分两类：
  - **DX 语义本身就要求 CPU 读回**（真机也走这条 ⇒ 保留、⛔ 不优化掉）：`GetRenderTargetData` /
    可锁后缓冲 `LockRect` / `UpdateSurface` / `LoadSurfaceFromSurface` / 纹理 `LockRect`。
  - **不是 DX 语义的**：`Clear` 的整幅下载（`Clear` 只写 VRAM）——Phase B 已消掉；
    `SetRenderTarget` 旧目标那一项（真机像素留在 VRAM 纹理里，我们为了"guest 字节是权威副本"才下载）——
    单独记账，实测 600 帧只有 **4 次**换 target ⇒ 本基准里不是热点，**不做** per-target 镜像。
- **B3（像素）**：同一条命令 `--max-frames 1 --dump-frame` 跑两臂（缺省 / `LIFT_PRESENT_EAGER=1`
  = 已知新鲜的参照臂）⇒ 首帧**逐字节相同**。**只对帧 1**：同一二进制连跑两遍，帧 2/3 的 dump
  就不同（真实时间依赖）⇒ ⛔ 不拿跨 run 的帧 ≥2 比。
- `Clear` 子桶 ms/帧只作 **observation**（`hpa1.out:2095` 在本机不存在，需求单已删该判据）。

### 本次读数（win32 · RelWithDebInfo · `apps/emulator-lift/out` · 2026-11-20）

命令（**无诊断**档，⛔ 不带 `--frame-log` / `--profile` / `--log-level info`）：

```powershell
$env:LIFT_GPU_SUBMITS="1"
.\out\emulator-lift-aot.exe --present window --present-hidden `
  --game-dir ..\..\links\install --appdata ..\..\.tmp\phaseb\appdata `
  --skip-logo --audio off --max-frames 600 `
  --input-plan scripts\recipes\save05-move.plan
```

（`[gpu-submits]` 每帧一行；下表的数是 600 行的**求和**，`rb_clear` / `rb_present` / `targets` 是
本阶段新加的归因字段。）

| 档 | readback | rb_clear | rb_present | upload | targets |
|---|---|---|---|---|---|
| before（Phase A 之后 / Phase B 之前） | 601 | 599 | 0 | 602 | 4 |
| after（Phase B） | 3 | 0 | 0 | 4 | 4 |
| before + `--frame-log` | 602 | 0 | 600 | 602 | 4 |
| after + `--frame-log` | 603 | 0 | 600 | 4 | 4 |

- **归因闭合**：600 帧里 599 次整幅下载来自 `Clear`，另 2 次来自换 target（`targets=4`）⇒"只改
  `Clear`"这条路吃到绝大部分（这也是走 A 路而不是 per-target 镜像的读数依据）。
- `upload=` 602 → 4：`Clear` 不再写 guest ⇒ `dirty_` 跨帧为真 ⇒ `rasterSyncFromGuest` 不再每帧把
  旧 guest 传回去。
- `--frame-log` 档每帧仍然读一次（`rb_present=600`）：诊断档**照旧拿得到本帧像素**，只是归因从
  `Clear` 换成了 `Present`（Phase A 那段"成本从 Present 迁到 `Clear`"的反向）。
- **语义指纹**（600 帧同场景，两臂）：`exit=5`、`frames=600`、`stop @ eip=00498AE4`、
  `region : arena in_use=89340944 peak=119683880 surfaces=181 heap_blocks=7888`、`table misses=0`
  逐位相同。⚠ `insns` 两轮差 9–13%、`synthetic : 续延=` 30→29：`save05-move` 是 **real-time**
  场景（同一二进制连跑两遍，帧 2 起 dump 就不同）⇒ 这两项**不是**指纹，⛔ 不据此判回归。
- 墙钟：⛔ **不宣称收益**。同日做了一轮**代理 A/B**（交替 3 对）：同一二进制，缺省臂 vs
  `LIFT_PRESENT_EAGER=1`（后者仍每帧整幅回读 + 每帧 hash ⇒ 是"Phase B 之前成本形态"的**下界代理**，
  不是 pre-Phase-B 二进制——那个构建已被覆盖；且 E 臂的 `Clear` 仍走 Phase B）。命令 = 上面那条
  **去掉 `--max-frames` 与 `--frame-log`**：带 `--max-frames 600` 会在配方 `wait 20000` 处 exit 5
  （`pending=51`）且**没有** `[profile-window]` 行；带 `--frame-log` 会让缺省臂也每帧回读
  （实测两臂 `readback` 1055.8 vs 1045.8 ms / 161 帧）⇒ 那是空比较。
  - 窗口墙钟 7494 / 7515 / 7509 → 7487 / 7507 / 7495 ms（逐对 −0.09 / −0.11 / −0.19%，中位 **−0.11%**）
    ⇒ **只有本机分辨力的 1/50，不可分辨**。
  - `Present` 子桶 **8.28 → 4.88 ms/帧**（差 3.40 = 回读 ~2.4 + hash ~0.93，折窗宽 ~7.3%）⇒ 成本确实
    被拿掉了，但窗口是 **wait-closed**（7.5 s 由配方 `wait` 定死）⇒ 差额被**帧数**吸收
    （窗内 presents 161 → 168、全程 frames 884 → 968）。
  - ⇒ 要拿墙钟记 Phase B 的收益，窗口必须改成**定帧数**（另一个评测口径，本单不做）；
    指纹 6/6 逐位同（`exit 0` / `stop @ eip=00412383` / arena 三元组 / `misses=0` / `pending=0`）。

### 剩余边界

- **RT 帧仍会下载 1–2 次**（`SetRenderTarget` 旧目标的保全屏障）：这是**我们的模型**的代价，不是 DX 语义
  （真机像素留在 VRAM 纹理里）。本单**不**优化它：实测 SAVE05 窗口 600 帧只有 4 次换 target ⇒ 不是热点；
  真要做就 per-target 镜像 + 让"RT 当纹理采样"直接绑 GPU target（另开单，按需）。
- **rect `Clear`（`Count != 0`）保持材质化**：仍整幅下载 + CPU 填色。本引擎在 SAVE05 窗口里是否真用
  rect `Clear` 仍是 `<待核>`（`clear-shape` 统计**没做**，要开单才做；本单只把它列进需求单的 `## 未做`）。
- **诊断点**：`--dump-frame` / `freeze` / `frameStats` / `lastHash` 都经 `materialized` 闸门；
  `FrameSampler`（`present.cpp`）是唯一不看 `materialized` 的点，但它在 `wantsGuestPixels()` 里
  （`present.h`）⇒ needs 谓词必先回读，Phase B 之后仍拿到本帧字节。⇒ 本阶段**未改**诊断点：
  根因是 `dirty_`，已由"arm ⇒ dirty"修掉；若将来新增不看 `materialized` 的消费者，那条要重审。
- `downloadInto` 仍是**唯一**把 `dirty_` 置假的下载路径（`rasterPrepare` / `rasterSyncFromGuest` /
  `rasterDiscardTarget` 各自在别的语义下置假，语义未变）。
