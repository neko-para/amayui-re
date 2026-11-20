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

## Phase B

回读归零（`Clear` 屏障改 `rasterRequestColorClear`、guest 后缓冲从权威降为派生）见需求单
`REQ-01M7W2FR9EJEMG2QDMZNG6N7WV`；Phase A 单独**不降** Present 的 readback ms/帧（成本迁到 `Clear`）。
