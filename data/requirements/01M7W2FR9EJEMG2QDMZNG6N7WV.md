# Present 回读 Phase B：Clear 走 GPU 侧 + guest 后缓冲降为派生（放弃帧边界权威）

- id: REQ-01M7W2FR9EJEMG2QDMZNG6N7WV
- type: req
- status: done
- parent: REQ-01M7JHSEZVMYB4XHEN32V7V6HJ
- verify: apps/emulator-lift/tests/present_readback.cpp#present-readback/clear-arms-not-downloads
- tags: [emulator-lift, perf, gpu, present, semantics]

## 一句话

放弃「guest 后缓冲在**每个帧边界**权威」这条**我们的模型**（⛔ 不是 DX9 语义），把每帧整幅 GPU→guest 回读从默认路径拿掉：
全幅 `Clear` 改成 GPU 侧的 color-clear 请求，guest 像素只在 7 个**显式读点** + 诊断点**就地**材质化。

## 为什么不是 DX9 语义

- 真 DX9 后缓冲在 GPU，CPU 不可见；CPU 读回只有 `GetRenderTargetData` 与 lockable back buffer 的 `LockRect` 两条路。
- shim 已在这 7 处各有一道**显式**屏障：`GetRenderTargetData` / `LockRect` / `UpdateSurface` /
  `LoadSurfaceFromSurface` / 纹理 `LockRect` / `SetRenderTarget` 旧目标 / `Clear`。
- 每帧回读的真实消费者只有：renderer 回退路径的 sink 与诊断（hash / dump / freeze / sampler）。
- overlay / UI / 插件 / screenshot **不读** guest 像素（`src/ui/**` 对 arena 零引用；直出叠层只画 swapchain；
  开/关 overlay 的 `--dump-frame` 逐字节相同，`tests/aot_overlay.cpp`）。⇒ ⛔ 不为它们加默认回读。

## 已落地（2026-11-20）

- 新接口 `GpuRaster::rasterRequestColorClear(bgra8)` + `SdlWindow` 转发壳（漏转发 = 静默丢清色；
  守卫 `gpu-batch/color-clear-forwarded`）。与深度版的三处不同：**绑 target 身份 / 后者覆盖前者 / arm 即「镜像 != guest」**。
- `arm ⇒ dirty_` 是最承重的一行：`rasterFlush` 的 `if (had_draws) dirty_` 对 clear-only 不成立；缺它就会有
  旧 guest 被上传回去盖掉清色（三条 `!rasterDirty()` 路径 + 所有诊断点）。
- `rasterFlush` 空队列早退 / target 空判 / `seg_has_draws` 放宽到含待执行的 color clear（清-only 也真的提交一次）。
- `d3d9::clear` 改道判据 = `rasterTargetBase() == base && Count == 0`；`mirror != base` 一支（只 flush + CPU 填色）**逐字保留**。
- 走 **A 路**（不做 per-target 镜像）：读数显示 600 帧里 599 次整幅下载来自 `Clear`，换 target 只有 4 次。

## 判据（⛔ 原「首帧必须含非背景像素」作废：首帧实测是黑，会打在正确实现上）

- **B1（dump 档）**：`--dump-frame` 时**每帧**都有 BMP，且该帧 `readback>=1`（dump 是消费者；触发漏了当场红）。
- **B2（无 dump 档）**：逐帧 `rb_clear==0 && rb_present==0`；凡 `readback - rb_clear - rb_present > 0` 的帧必须能**归因**（见下一条）。
- **下载归因（⛔ 不以 `readback=0` 为目标）**：每一次下载都要能归因到一条**显式读点**的 guest 调用。
  · **DX 语义本身要求的 CPU 读回**（真机也走这条）⇒ 保留、⛔ 不优化掉：`GetRenderTargetData` /
    可锁后缓冲 `LockRect` / `UpdateSurface` / `LoadSurfaceFromSurface` / 纹理 `LockRect`。
  · **不是 DX 语义的两处**：`Clear` 的整幅下载（`Clear` 只写 VRAM；Phase B 已消掉）与
    `SetRenderTarget` 旧目标那一项（真机像素留在 VRAM 纹理里，我们为了"guest 字节是权威副本"才下载）。
    后者单独记 `rb_target`；实测 SAVE05 窗口 600 帧只有 **4 次**换 target（2 帧）⇒ 本基准里不是热点，
    **不做** per-target 镜像；等某个场景真把它变热再开单（按需、不预先做）。
  · 判据的形状因此是"**非读点下载 = 0**"，而不是"总下载 = 0"。
  `targets>=1`（RT 帧的那 1–2 次下载被**点名**，不是被忽略）。
- **B3（像素）**：同一命令 `--max-frames 1 --dump-frame`，缺省臂 vs `LIFT_PRESENT_EAGER=1` 参照臂 ⇒ 首帧**逐字节相同**
  （跨 run 的帧 ≥2 不稳定，`tests/aot_overlay.cpp` 已证否 ⇒ ⛔ 不拿跨 run 的帧 ≥2 比）。
- 语义指纹（固定 `--max-frames`）：`exit` + `frames` + `stop @ EA` + arena 三元组 + `table misses`。
- `Clear` 子桶 ms/帧只作 **observation**（`hpa1.out:2095` 在本机不存在）。
- 读数（win32 · 600 帧 · 无诊断档）：`readback` 601 → 3（`rb_clear` 599 → 0）、`upload` 602 → 4；
  `--frame-log` 档 `rb_present=600`（诊断照旧拿得到本帧）。墙钟代理 A/B 中位 −0.11%（低于分辨力）⇒ **不宣称收益**。
  全文见 `docs/03-emulator/present-readback.md` 的 Phase B 段。

## 边界 / 未做

- ⛔ 不改 DX 语义：7 个显式读点的行为逐位不变。
- **RT 帧仍会下载 1–2 次**（`SetRenderTarget` 旧目标的保全屏障）：这是**我们的模型**的代价，不是 DX 语义；
  实测 600 帧只有 4 次换 target ⇒ 不在本单优化（per-target 镜像 = 让"RT 当纹理采样"直接绑 GPU target，另开单）。
- rect `Clear`（`Count != 0`）保持材质化；本引擎是否真用 rect `Clear` 仍是 `<待核>`（`clear-shape` 统计**没做**，要开单才做）。
- 诊断点未改：根因是 `dirty_`，已由"arm ⇒ dirty"修掉；新增不看 `materialized` 的消费者要重审。
- ⛔ 脏矩形回读：已裁不立项（本场景全屏重绘，近零收益）。
- 墙钟收益要精确记，就让 **JS 侧在载入完成后用 `ctx.profile.begin/end` 夹住那段操作**（窗口由状态门定界、不是墙钟，
  `PluginHost::profileEnd` 复用 `AotHost::endWindowText()`，报告走 stdout，与配方窗同一份文本）—— 本单没做。
