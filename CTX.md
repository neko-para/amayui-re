# CTX · emulator-lift 交接（2026-11-13）

> **一次性 handoff**：记"现在到哪、还剩什么、别踩什么"。不是长期文档（长期口径在 `AGENTS.md` / `apps/emulator-lift/README.md` / `SCOPE.md`）。
> 进度真源仍是 `pnpm tools requirements plan`；本文只是给下一个人（或下一轮 agent）的入口。

## 0. 一句话状态

当前实现线 `apps/emulator-lift`（C++/CMake）。口径已定：**GPU 是唯一光栅 + 实时（real-time）**，不做像素一致/可重现。
本轮改动**未提交**（约 90 个文件 + 27 个新需求单），门禁全绿（见 §4）。剩余工作见 §3。

## 1. 已拍板的口径（不要重新讨论）

| 主题 | 裁决 |
|---|---|
| 光栅 | **GPU 唯一**；无 GPU ⇒ **拒绝启动**（exit 2 + 指路）；`--raster` / `--raster-sprites` / `--gpu-strict` 已删；`--present headless` 走**隐藏窗口**的 GPU |
| 像素 | 不追求逐位/可重现；不做黄金图；**允许功能回退**，但拒绝必须**响亮 + 记账**（`unsupported` 具名位 / `skipped`） |
| 音频 | 实时；underrun（环陈旧）⇒ **交付 0 + 首次 warn + 计数**（写见证 `[stream+0x2468]`，⛔ 不做内容比对）；允许宿主插静音，不重播 |
| D3D9 | 除**明确拒绝**外都补；`CreateProcessA` **永久 trap**；`ImmAssociateContext` **条件 trap**（`hIMC==0` 返回 NULL；`hIMC!=0` 响亮 trap） |
| MF / MCI+mixer / 摇杆 | SCOPE §1 已有行覆盖 ⇒ 保持 no-op，台账挂 `scope_ref`，不实现 |
| 3D / Live2D | 3D 网格明确拒绝（SCOPE §1）；Live2D 已交付，**M3 已关** |
| Windows 专有 | 延后到 Windows 机器（慢机超时 / POSIX 搬守卫 / 退出期 SIGSEGV） |

## 2. 本轮已落地（主题 + 关键守卫）

- **GPU 纹理作 RT + 读回**（`REQ-01M79MSMWGFEKF8J7C7VYWS4YP`）：准入放行 + 反馈环闸 + depth 复用；`d3d9p/render-to-texture(-feedback)`、`gpu-batch/depth-retained-across-target-switch`。
- **sprite→RT**（`REQ-01M79V2TVJH98BKH0C0WQV2FET`）：`colorTargetOfState()` 唯一判据；`d3d9p/sprite-render-target`（含 GPU 账本断言）。
- **缩略图全黑**（`REQ-01M780VKKRFHJEX6VM74DRCYBM`）：`SAVE00.STH` payload 0 → **172800**，断言 + 变异（`save-graph`）。
- **D3D9 四条**：caps 清假声明（`d3d9/caps-claims`，双向蕴含）、`ValidateDevice` 显式 1 pass（`d3d9p/validate-device-one-pass`，防漂移）、`Base/Min` int32 区间（`d3d9p/indexed-range`，修前可复现 SIGSEGV）、`SetRenderState` A/B/C 分档（`d3d9p/render-state-policy`）。
- **CPU 光栅退休前半**（`REQ-01M78BF1…`）：删三个 CLI 开关、无 GPU 拒绝启动（`lift/no-gpu-refuses-start`）、`--raster` 未知参数（`lift/raster-flag-gone`）、headless 隐藏窗口（`gpu-batch/headless-hidden-window`）、17 个起子进程用例的"无 GPU ⇒ SKIP 77"共用判据。
- **点/线 GPU 化**：展开成三角形（`gpu-batch/lines-points`）；三条旁路（invw / mip 缩小 / 后端失败）改显式拒绝 + bit21/22/23。
- **拒批账本**（`REQ-01M7AAXVGY9SG75XCREHPX641Z`）：12 个具名位 29..40 + `skipped` / `skipped top`（`gpu-batch/skip-ledger`）。
- **音频 underrun 插静音**（`REQ-01M75PTAC1…`）：写见证 + 4 条守卫 + `fill-witness-wiring`（真 fill → witness，删 `++witness` 会红）。
- **字体**：`docs/03-emulator/fonts.md`（AmayuiCN = CP932 码位映射字体）；游戏自带 `AGE Extend` 内存字体作**缺字后备** → 修好 U+E010（空心心形 ♡），守卫 `gdi-font-fallback`（pure + assets + 经 shim）。
- **overrides 台账清理 A**（`REQ-01M7A5TZ…`）：74 → 62（noop 47 / trap 15）；kind 三值化；删 12 条不可达；AGERC 单列 `bareUnexecuted`；`check` 逐字节一致。
- **工具**：`disasm-fn` 折叠块可见（1805 块 / 175141 B）；mutate 超时改结构判据；`[input-script]` 收口日志与事实一致。

## 3. 剩余工作（按建议顺序）

### P0 · 把 CPU 光栅真正退休
1. `REQ-01M7A74Y…` **M2/M3**：3 个用例（`d3d9` / `d3dx9` / `d3d9-pipeline`）的 **300 条像素断言**分档迁移
   （结构 57 / GPU 臂 85 / 已覆盖 16 / 删 119 / 不经光栅 23，见 `.tmp/cpu-migration/design.md`），
   然后 `gpuEligible` 拒批改"记账 + **跳过**"、删 `LIFT_GPU_VERIFY`、删 `rasterTriangle` 一族。
   档位：4 个 pure 臂保留 + 新增 `d3d9-gpu` / `d3dx9-gpu` / `d3d9-pipeline-gpu`（顶层注册 + SKIP 77）。
2. `REQ-01M7AAXNJY2PACYJHYVRV620Q5`：`d3dx9_sprite.cpp` 的 **sprite CPU 合成器**（另一条 CPU 像素路径）改"记账 + 跳过"。
3. `REQ-01M78BF1…` 在 1/2 落地后收口。

### P1 · 台账语义机制
4. `REQ-01M7A5TZP0P9AFNGQ79N321J75`（overrides 清理 B）：`scope_ref` 必填（指向 SCOPE §1 具体行）+ reason 从该行派生 + 37 条过期文本重写 + `MODULE_REASONS` 退役。
   - 已裁决：MF/MCI/摇杆 挂现有行；`CreateProcessA` / `IME` 需在 SCOPE §1 各加一行（用户 2026-11-13 已授权）。
5. `REQ-01M79P033W150Z107FZZJ5NX1Q`：`platform-effort.md` 的 Live2D 足迹读数（"title 0 次"）按可复跑命令回写。

### P2 · 音频
6. **实时臂守卫**（设计 `.tmp/audio-phase/design.md` §2）：`fill_slow==0` / `fill_max<=500ms` / `underruns==0` / `underrun_silence_events==0`；`feeder-rt-e2e`（assets，SKIP 77）。
7. `REQ-01M711JFV…`：令牌等待归到窗口（窗口级等待账）再排序三类阻塞。
8. `REQ-01M75VZXV4…`（尖峰）/ `REQ-01M75PTEGCR1…`（吞吐）：按实时口径收口；`fread` 进 `YieldGuard` 是尖峰剩余项。
9. `REQ-01M79MW8…`：起播重建的**叶子** override（`sub_4E0030` / `sub_4DFDA0`），守卫照 `mdct/*` 数值容差。

### P3 · 画面/性能读数
10. `REQ-01M6RQ2…`：TITLE 墙钟**重测**（判据已改：wall ≤33ms 或写读数收口；合批已在 2026-11-08 落地）。
11. `REQ-01M73JV31…`：TITLE 右侧条纹**复验**（旧证据早于 PNG 解码落地）。
12. `REQ-01M6RH9CD…`：两个子单收口后关父单。

### P4 · 守卫/小缺口（可一批做完）
13. 声明式 Draw 画像素守卫（`REQ-01M79P02…`）、离屏 plain surface RT 拒绝守卫（`REQ-01M79WE8…`）、
    sprite 自采样反馈环闸（`REQ-01M79ZRT…`）、`Clear/unlockRect` 收口到 `colorTargetOfState`（`REQ-01M79ZRTX…`）、
    `SetRenderState` 合法值域（`REQ-01M7A3M73…`）、声明路 indexed 区间 + `start_index`（`REQ-01M7A3M7E…`）、
    `GetBackBuffer` 同一对象（`REQ-01M7A2F1…`）。
14. `REQ-01M6RMVJ…`（Clear z 传值）+ `REQ-01M73XEB…`（先 flush 再 arm）**同一处，一起改**。
15. `REQ-01M7460…`（COLORVERTEX/MATERIALSOURCE）。
16. `REQ-01M7A5VF…`：ADV/存档/OPTION 场景的 invw/mip 命中普查（现在会被拒绝，痕迹 bit21/22）。

### P5 · 稳定性/理论债
17. `REQ-01M72GAA7CV`（SEH 静默丢异常，S3，机制仍在）、`REQ-01M773EF…`（softfloat `THREAD_LOCAL`）、
    `REQ-01M78A0V0…` / `REQ-01M79NYY…`（两条偶发红）、`REQ-01M72GW902…`（退出期 SIGSEGV，**等 Windows 机器**）。
18. ~~日志降噪~~（`REQ-01M7AG06MXS6PK2B53B29QM76Z`，✅ 2026-11-13）：默认 **121 → 25/26 行**，三段（结论 / 异常 / 一行摘要）；
    缺口明细进 `--report`、native-ovr per-entry 进 `--profile`（异常项默认点名一行）；守卫 `stdout-quiet`。
    follow-up（2026-11-13 已做）：`hits==0` 不再算异常、A4 警告整行删除（`boot-trace` 保留）、`bit11=MIPFILTER` 列已知良性（不进异常段，`read :` 带 `mipfilter≈nearest`）、AGERC 重定位信息降到 Info。
    **未做**（有意留）：异常段 stderr 分流、打印点接 `LogCat::LogReport`、其它良性位登记、`--report` 纯清单模式。

### P6 · 延后（Windows 机器）
19. `REQ-01M786CQ…`（慢机超时）、`REQ-01M7704NBZ…`（5 条 Windows 专有守卫搬 POSIX）。

## 4. 门禁（当前读数，2026-11-13）

```bash
cd apps/emulator-lift && cmake --build --preset macos-arm64
ctest --test-dir out --output-on-failure          # 96 注册 / 92 通过 / 4 SKIP（约 380 s）
cd <repo>
pnpm test                                         # 合同档 546/546
pnpm verify                                       # 工具链档 494/494
pnpm typecheck                                    # 六个工程 exit 0
pnpm tools requirements validate                  # 6/6
pnpm tools corpus validate                        # 8 通过 / 0 失败 / 1 警告（允许的缺席）
pnpm tools lift check                             # 绿
node tools/mutate-check.mjs --list                # 108 条静态全过
git diff --check
```
4 个 SKIP：`cli-time-limit` / `cli-dump-frame` / `frame-dump-failure`（Windows 专有）、`gpu-raster`（dummy 视频建不出 GPU device）。
★ 读数会随改动变；**不要**把本文的数字写成断言。

## 5. 环境/坑（都踩过）

- **macOS 本机可做 GPU**：`--present window` / `--present hidden` 都能建 Metal device；但 `SDL_VIDEODRIVER=dummy` ⇒ 无 GPU ⇒ 拒绝启动（测试会 `LIFT_SKIP(77)`）。
- **headless = 隐藏窗口**：`--present headless` 也建 `SdlWindow{hidden}`；日志 `present : … window=hidden`。
- **`dist/` 是生成物且 gitignore**：视图/字体产物都在里面，⛔ 不要把 `dist/` 路径当锚点或证据；真源 = 基线 + patch。
- **需求树只能经 CLI/工作台写**：`pnpm tools requirements set … --write`；⛔ 手改 `data/requirements/*.md`。
- **overrides 只能经 `lift sync --write` 写**；`lift check` 现在有逐字节断言（手改一个字符就红）。
- **SCOPE.md 不许在用户没明确要求时编辑**（2026-11-13 已就 IME / CreateProcessA 两行授权）。
- **不要 `git add` / `commit`**：提交时机由用户定。
- **Windows 读数不能在本机复现**：尖峰 / fill_max / 慢机超时都只能引用已入库的 Windows 读数。
- **临时文件**：`.tmp/`（gitignored）；复审/设计稿都在 `.tmp/*/`。

## 6. 本轮未提交改动的提交建议

约 90 个跟踪文件（+6047 / −1135）+ 27 个新文件（26 张需求单 + `tests/gdi_font_fallback.cpp` / `tests/subprocess_skip.h` / `docs/03-emulator/fonts.md`）。
改动在**同一批文件里高度交叉**（`d3d9_pipeline.cpp` 同时承载 RTT / D3D9 / 拒批账本 / CPU 退休），按 hunk 拆提交成本高。建议：

```
feat(emulator-lift): GPU-only raster + RTT/sprite RT + D3D9 gaps + realtime audio underrun silence
```
正文：CPU 光栅退休（CLI 删除 / 无 GPU 拒绝 / headless 隐藏窗口）、RTT + sprite→RT、D3D9 四条、
音频插静音、字体 fallback、overrides 台账清理、需求树 + 文档。
（若要拆：`apps/` + `tools/` 一提交；`data/requirements/` + `docs/` 另一提交。）

## 7. 交接后的第一步

1. 提交（或先看 `git diff` 的三处关键：`audio.cpp` 的 `generateTo` 判据、`d3d9_pipeline.cpp` 的 `gpuEligible`/`noteGpuSkip`、`main.cpp` 的无 GPU 拒绝）。
2. 从 §3 P0 开始（先 M2 的"结构断言 9 条先行"，风险最低）。
3. 任何"判据过期"的怀疑：先跑 `pnpm tools requirements show <id>` 看正文，再动代码。
