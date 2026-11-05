# emulator-lift · CPU/渲染基础设施重构：GLM · pixel_format · Unicorn 双后端

- id: REQ-01M6MZRHEKGRHVDHX4K4CZ1SD8
- type: req
- status: doing
- parent: REQ-01M6H6G6DSRG1ZRD810Z0HTH8V
- tags: [emulator-lift, infra, cpu]

## 一句话
把 `apps/emulator-lift` 的**通用数学 / 像素格式 / CPU 执行**从手写设施中拆出来：数学迁到 GLM、像素格式收敛到一个模块，并评估/接入 Unicorn 作为第二 CPU 后端；先保留现有解释器做对照，不破坏现有 CTest。

## 背景（2026-11-05 现查）
- M2 已实现 D3D9/D3DX + 软件 fixed-function 光栅，但通用数学与像素 mask 逻辑在多处重复。
- 当前 boot 到 `kernel32!CreateThread`：`insns=109,536,868`、Debug 约 17.4 s；伪线程探针显示下一站是 `0x0040C131` 的空字符串/空 vector（字体枚举后），仍无 `BeginScene`/`DrawPrimitive`/`Present`。
- 当前 `out` 是 **Debug**（`-g`，无优化）；性能判断前必须先做 Release 测量。
- 实际 SDL 窗口仍是 P3 未做；`Present` 只走 `HeadlessPresenter` 的哈希。

## 已发现的具体问题（本单要跟踪/消除）
1. **数学重复**：`shims/d3dx9.cpp`、`runtime/d3d9_pipeline.cpp`、`runtime/d3dx9_sprite.cpp` 各自手写 4×4 / vec3 / vec4 / 单位阵；D3DX 是行主序 + 行向量，GLM 是列主序 + 列向量，必须显式适配。
2. **像素工具重复**：`font.cpp`、`d3d9_pipeline.cpp`、`d3dx9_sprite.cpp` 各有一份 `maskShift`/`maskBits`/`expandChannel`；`ddraw_surface.cpp` 另有 `loadPixel`/`storePixel`；`d3d9.cpp` 另有 `pixelFromColor`。
3. **纹理过滤缺陷**：`sampleTexture` 只用 `mag_filter` 决定 point/linear，`min_filter` 不参与（与注释“MIN/MAG 都支持”不一致）。
4. **每像素读 guest 状态**：`rasterTriangle` 在像素循环里反复 `texStageOf`/`samplerOf`，应在每次 Draw 前快照。
5. **裁剪不全**：只做近平面（`z>=0`），没有远平面 / x / y 平面裁剪，目前靠 scissor 兜底。
6. **软件光栅性能**：1280×720 = 921,600 像素/帧；Debug 下明显慢；需 Release `ms/frame` 与 emulator-cxx 的软实现经验对照。
7. **CPU 预算**：当前解释器约 11 MIPS（`lift-bench`），60fps 只有约 21 万 guest 指令/帧；109M 启动指令说明 CPU 是长期瓶颈。
8. **线程硬卡点**：`kernel32!CreateThread`（DirectSound 混音线程）按 SCOPE 是硬错误；伪线程探针已证明线程之后还有字体枚举/资源初始化。
9. **窗口/呈现缺口**：guest 窗口已 headless 创建，真实 SDL 窗口/P3 呈现未做。
10. **文档漂移**：`SCOPE.md` 的 3D 行仍写“D3DX 仍 stub / D3D9 状态机不做”，与 `f28dc1d` 的实现不一致。
11. **测试捕获假红**：`tests/user32_window.cpp` / `tests/dsound.cpp` 用 `dup2(...) != 0` 判成功；POSIX `dup2` 成功返回目标 fd(2) ⇒ 捕获未启用、5/17 个假失败。已修为 `< 0`（本单已落）。

## 交付物
### A. GLM 数学迁移
- 新增 `runtime/math.{h,cpp}`（或等价落点），统一 D3DX 行主序 ↔ GLM 的适配；D3DXMatrix*/Vec* 与内部 T&L 都走它。
- 保留现有 `tests/d3dx9.cpp`（`repo:apps/emulator-lift/tests/d3dx9.cpp`） 的手算断言作为护栏；新增至少一条会红的约定测试（行/列主序、LH、零向量归一化）。
- `vcpkg.json` 增加 `glm`；CMake 链接。

### B. pixel_format 提取
- 新增 `runtime/pixel_format.{h,cpp}`；集中 `SurfaceFormat` 的 mask/shift/expand/pack、`loadPixel`/`storePixel`、`pixelFromColor`。
- 替换 `font.cpp` / `d3d9_pipeline.cpp` / `d3dx9_sprite.cpp` / `ddraw_surface.cpp` / `d3d9.cpp` 的重复实现；热路径保持 inline 或 constexpr 快路径。
- 修 `sampleTexture` 的 `min_filter`：按 min/mag 各自选择，或显式记录“只按 mag”并改注释/日志；不得静默。
- （可选，不在本单必须）批量转换可用 SDL3；逐像素路径不强制。

### C. Unicorn 双 CPU 后端 POC
- 先做 `CpuBackend` 接缝，保留现有解释器为 reference；Unicorn 后端至少能：
  - 映射 guest 内存 / 寄存器 / 栈；
  - 命中现有 IAT shim 页并调用 `ShimRegistry`；
  - 支持 `--max-insns` / `--until ea` / 基本 fault 诊断；
  - 跑通一个 pure 夹具与一次 boot 到当前停点（或更远）。
- **不破坏性接入**：默认仍用现有解释器；Unicorn 用 CLI/编译开关选择；现有 CTest 全绿。
- 记录 Unicorn 的 GPLv2 依赖、嵌套 guest 回调可行性、稀疏 guest 内存/fault 语义、x87/SSE 确定性等风险。

## 判据
1. `pnpm tools requirements validate` / `pnpm tools corpus validate` / `pnpm test` 绿。
2. `cmake --build --preset macos-arm64` + `ctest --test-dir out --output-on-failure` 绿（至少现有图形/数学/窗口测试）。
3. A/B 后 `git grep` 不再有重复的 `maskShift`/`maskBits`/`expandChannel` 定义（只允许 pixel_format 一份）。
4. A/B 代码已 `git add`（**不 commit**）；C 的代码保持 unstaged。
5. C 至少能构建并跑一个双后端冒烟；若 Unicorn 集成被嵌套回调/内存模型卡住，写清具体 EA/API/失败信息与下一步，不许伪装成“已完成”。

## 边界 / 不做
- 不做 GPU 后端（SDL_GPU）；本单只做 CPU/数学/像素底座与 Unicorn POC。
- 不改 `corpus/`、`data/spec/`；不写引擎语义结论。
- 不 commit；任务 C 不 staged。
- 不把“Unicorn 能跑”当成“与真机等价”；只报可复现的构建/测试/性能事实。

## 进展（2026-11-05）
- **A/B 已完成并 staged（未 commit）**：GLM 1.0.3 + `runtime/math.*`；`runtime/pixel_format.*` 消除 5 处重复，min_filter 不再被忽略；GDI 16bpp 保留旧口径；`ctest` 41/41、`pnpm test` 1159/1159、`lift check` 绿。
- **C 已完成 POC（unstaged）**：`--cpu interp|unicorn`；Unicorn 后端可跑真 AGE.EXE 到当前 `CreateThread` 停点（109,536,877 insns；`--cpu interp` 为 109,536,868）；默认构建仍 41/41，Unicorn 构建 42/42。
- **性能实测（Release，同一二进制）**：到 `CreateThread` 的整段 boot：interp **2.13–2.23 s**，Unicorn **10.02–11.09 s**（Unicorn 更慢）；紧循环 15M 指令：interp ~90 MIPS，Unicorn ~170 MIPS（约 1.85×）。结论：当前“切片 + 每次 shim 往返”的结构在 shim 密集的 boot 上是负收益，不能据此固定 CPU 架构。
- **两个决定性坑**：Unicorn 静态库 `crc32` 与 zlib 同名不同算法（用动态 triplet 隔离）；Unicorn 32 位模式不能写 FS_BASE/GS_BASE（用 GDT/选择子 + flat SS）。
- **未完成**：CreateThread 之后、GPU/窗口、Unicorn 切片开销优化、MMX/MXCSR/SEH/SMC；需用户决定是否投入优化或保留解释器。
## 2026-11-05 追加：单 out 构建 + hook/stable-stub 性能
- 已按用户要求统一到**唯一 `out`**：RelWithDebInfo + Unicorn 后端 ON；`ctest` 42/42（41 + `unicorn` 85 checks）。不再有 `out-unicorn` / `macos-arm64-unicorn`。
- boot 到 `CreateThread`（同一 `out` 二进制，`--cpu` 切换）：interp **3.27 s**；unicorn 基线 **18.05 s**；关全局计数 hook **5.90 s**；inline shim（保留计数）**15.73 s**；inline + 关计数 **5.63 s**。
- `LIFT_UC_PROFILE=1`：基线 slices=2359 / emu=18064 ms / invoke=51 ms；关计数 slices=2264 / emu=5834 ms / invoke=38 ms ⇒ **主要成本是逐指令计数 hook + TCG/内存，不是 stop/restart**；stable stub/direct-invoke 单独只省约 5%（关计数时）。
- 结论：stable stub 必须是**每 API 唯一地址**（现有 `ShimRegistry` 已如此，`tests/mechanism` 的 `addRuntime-unique-stub` 守着）；Unicorn 没有“把多个 API 映射成同一个函数指针”的魔法。当前 POC 即使关计数仍比 interp 慢约 1.8×，需要继续 profile TCG/内存，不能据此固定架构。
## 需要用户后续裁决
- 线程：伪线程（音频 silent）还是真线程。
- CPU 后端最终固定到哪一条（现有解释器 / Unicorn / 其他），需在双后端性能对比后决定。
- 是否接受 Unicorn 的 GPLv2 依赖与 x87/SSE 1 ulp 级差异。
