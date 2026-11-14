# GPU 准入拒绝 MAG!=MIN 与 MIPFILTER=NONE+缩小采样：两条都不是"表达不了"（SDL sampler 分开收 min/mag、max_lod 是 per-sampler），且翻面后 mip 选级无任何用例

- id: REQ-01M7AZ68J05PMVWVY7A589TQ6Y
- type: req
- status: done
- parent: REQ-01M7A74YG08R65NJBFXF7DEJDK
- verify: apps/emulator-lift/tests/d3d9_pipeline.cpp#d3d9p/min-filter
- done_reason: 已实现（两条"表达不了"都被真实现推翻）：① gpuEligible 删掉 MAG != MIN 的拒批 —— min_filter/mag_filter 在 GpuSampler 里本来就分开存、落到 SDL 也分开写（si.min_filter / si.mag_filter）；② MIPFILTER=NONE（D3DTEXF_NONE）不再拒批，改为 per-sampler 的 mipmap 位：p.texture.sampler.mipmap = ss.mip_filter != kTexfNone，该位进 samplerFor 的缓存键（后端据此建第二个 sampler："只采 level 0"与"按 LOD 选级"各一个）；③ 整段 gpu_mip_divergence（含 triangleMipLevel 的 GPU 分歧记账）删除。★ 技术偏离（实测推翻了单里给的值）：单里写 max_lod = 0，实测是错的 —— 采样先算 λ' = clamp(λ, min_lod, max_lod) 再按 λ' <= 0 选 MAG、否则选 MIN，max_lod = 0 会把缩小也钳成 λ'=0 ⇒ 走 MAG ⇒ MINFILTER 整个失效。实现改用 max_lod = 0.49（kNoMipMaxLod，注释写了原因）：λ<=0（放大 / 1:1）时 λ'=λ 仍选 MAG，λ>0（缩小）时 λ'∈(0,0.49] 选 MIN 且最近级 round(λ')=0。实测 MAG=POINT+MIN=LINEAR、4:1 缩小时 max_lod=0 与 MIN=POINT 像素相同（0x00140000），0.49 时分别为 0x00110000 / 0x00140000。★ 具名位：bit22 kUnGpuMipDivergence 删除（已真实现 ⇒ 永不触发；enum 注释写明 22 是空号、不许复用）；bit38 保留但改名为 kUnGpuFilterUnmapped（仍有一条可达路径：MIN/MAG = D3DTEXF_NONE(0) 映射不了）。★ 旧期望 0x00C72020 是伪造的合格证，⛔ 不要恢复：那组参数是 4:1 放大，D3D9 放大用 MAGFILTER ⇒ MAG=POINT 必须纯红 0x00FF0000；旧断言断的其实是"CPU 把 MIN 用在放大上"。现在放大臂断言 MAG 生效（MAG=POINT+MIN=LINEAR ⇒ 0x00FF0000；对照 MAG=LINEAR+MIN=POINT ⇒ 0x00C72020；两值必须不同），并新增真缩小臂（8×8 纹理 → 2×2 像素：MIN=POINT = 0x00140000 / MIN=LINEAR ≈ 0x00110000 两个手算值，两值必须不同）。判据：tests/d3d9_pipeline.cpp#d3d9p/min-filter 与 tests/d3d9_pipeline.cpp#d3d9p/mip-levels（⚠ 两条都在 --gpu 臂 = CTest d3d9-pipeline-gpu：无 GPU 设备时 SKIP 77，须真卡跑，不是假绿）；tests/d3d9_mip.cpp#d3d9-mip/lod-levels 只覆盖纯函数（派生级内容与取整），不能当唯一判据。
- tags: [emulator-lift, d3d9, gpu, sampler, mip]

## 判据（机械可检验）

⚠ **先纠正原用例的一个错误前提（别照抄）**：`d3d9p/min-filter` 原来那组（2×2 纹理铺到 **8×8 像素**，`drawQuad` 顶点 x/y ∈ `0..8`，`tests/d3d9_pipeline.cpp` HEAD 版 `:873-881`）是 **4:1 放大**，而 D3D9 放大采样用 **`MAGFILTER`** ⇒ `MAG=POINT` 时 `(2,2)` 必须是纯 texel `0x00FF0000`，**不是**双线性 `0x00C72020`。旧断言 `0x00C72020` 断言的其实是 CPU 的"任一 LINEAR ⇒ 双线性"（把 MIN 用在放大上）——**那条断言本身是错的**，⛔ 不要恢复它。所以本单判据是"把两档过滤真的分开"：

1. **放大臂**（2×2 纹理 → 8×8 像素）：`MAG=POINT + MIN=LINEAR` ⇒ `pixelAt(2,2) == 0x00FF0000`（**MAGFILTER 生效、MINFILTER 不参与放大**），Draw `D3D_OK` 且 `skipped_draws == 0`；对照臂 `MAG=LINEAR` 的同一像素 ⇒ `0x00C72020`。
2. **缩小臂（缺的那一半）**：**8×8 纹理 → 2×2 像素**（4:1 缩小）、`MIPFILTER=NONE` 之外的缺省态：`MIN=POINT` ⇒ 取最近 texel（例如 `(0,0)` 像素 = 纹理 `(2,2)` 的纯色）；`MIN=LINEAR` ⇒ 该像素是 4×4 texel 的平均值（手算写死）⇒ 两个值**必须不同**，且各自等于手算值。这一臂才是"MINFILTER 真的参与"的判据（旧用例缺的就是它）。
3. **准入**：上面两臂（都带 `MAG!=MIN`）的 Draw 都必须 `D3D_OK`、`gpuCoverage().queued >= 1`、`skipped_draws == 0` —— 直接否掉今天 `kUnGpuFilterSplit` 的整批拒绝。
4. `d3d9p/mip-levels` 恢复**选级**判据（这一组**没有**上面那个前提错误：4×4 纹理 → 2×2 / 1×1 像素是**真缩小**）：`MIPFILTER=POINT` 时 `2:1` ⇒ level1 的 box 值 `[3,16;114,0]`、`4:1` ⇒ level2 = `33`、`1:1` ⇒ 逐 texel 等于 level 0；`MIPFILTER=NONE` + `4:1` ⇒ **`D3D_OK` 且像素 = level 0 的 texel(2,2) = 0**（不是拒绝）。
5. **反向条款**（防"永远放行"）：第 1 臂的两个 `MAG` 值与第 2 臂的两个 `MIN` 值都必须**产生不同像素** —— 判别力在这里，不在"拒绝"上。

## 现状证据（"可表达"的三条）

1. **sampler 结构体本来就分开存 MAG / MIN**（`src/runtime/gpu_raster.h:163-168`）：`GpuSampler { address_u, address_v, mag_filter, min_filter }`。
2. **准入处也是分别填的**（`src/runtime/d3d9_pipeline.cpp:3920-3921`），**落到 SDL 时也分别写**（`src/runtime/d3d9_gpu.cpp:2150-2154`）：`si.min_filter = filt(s.min_filter)`、`si.mag_filter = filt(s.mag_filter)`、`si.mipmap_mode = NEAREST`，而 `si.max_lod = 1000.0f` 就在下面 `:2157`（per-sampler 常量）⇒ `MAG=POINT + MIN=LINEAR` 不需要任何新结构：**删掉闸即可**。
3. **`max_lod` / `mipmap_mode` 是 sampler 字段，sampler 是"按批建"的**：`samplerFor`（`d3d9_gpu.cpp:2131-2168`）每次按 `GpuSampler` 的键取/建 sampler ⇒ 给缓存键（`:2132-2135`）加一位 mip 模式，就能**精确表达**"只采 level 0"（`MIPFILTER=NONE` ⇔ `max_lod = 0`）。**"一张纹理一档"不成立。**
4. **而且链已经在**：上传时**已经**从完整 level 0 逐级 box filter 并逐级上传（`d3d9_gpu.cpp:2047-2099`，用 `d3d9::mipDownsampleBox` `:2068`）⇒ `MIPFILTER=NONE` **不需要**任何上传侧改动，只需那一批用 `max_lod = 0` 的 sampler（`MIPFILTER != NONE` 的批仍用 `max_lod = 1000`）。

## 那段理由为什么"随 CPU 删除已失效"

`src/runtime/d3d9_pipeline.cpp:3906-3919` 逐字：CPU 参考把两个过滤**合成一条**决策（`samplerUsesLinear`：任一 LINEAR ⇒ 双线性），而 SDL_GPU 的 sampler 把 MAG / MIN **分开存、分开用** ⇒ `MAG=POINT + MIN=LINEAR` 两边都"成功"、整批像素却不同，且 fallback 账看不见 ⇒ 按 `docs/00-origin/decisions.md` §8.1（CPU=oracle / GPU=product）"要么一致，要么不接受该批" ⇒ 这里只放行 `MAG == MIN`。
⇒ 这条闸的全部正当性来自"CPU 是 oracle，而 CPU 把两个过滤合成一条"：CPU 光栅族一删（本次改动），**oracle 不存在**，前提就没了 ⇒ 同一常量变成纯粹的能力拒绝。`MIPFILTER=NONE` 那条同形（`:4257-4261`）：只剩"我们没做 `max_lod = 0` 的 sampler"，而那是**可以做的**。
⚠ 覆盖证据链也断了：`README.md:738` 仍写"`max_lod` 是一张纹理一档，关不掉 mipmap"（假理由），而 `d3d9_pipeline.cpp:4279` 的注释还指向"见 README 的口径行"——**README 里没有那条**（引用悬空）。

## 为什么不是"设计"

1. **"表达不了"是事实错误，不是取舍**：`MAG`/`MIN` 在结构体、准入、SDL 三处都是分开的，`max_lod` 是 per-sampler 且 sampler 按批建 ⇒ 两句"表达不了"都能被 5~10 行代码推翻（判据：删 `d3d9_pipeline.cpp:3912-3919` 那 8 行、给 sampler 键加一位 mip 模式，两条用例就能恢复成像素判据）。**"做不到"与"没做"要分清。**
2. **拒绝的正当性前提已经消失**：`3906-3911` 的论证链是"CPU=oracle，而 CPU 合成一条决策"，本次改动把 oracle 删了 ⇒ 前提消失、结论不能留。
3. **它把"过滤参数真的参与"换成了"我们拒绝"**：`min-filter` 现在只剩"我们拒了它"（且原用例的放大臂断言本身就把 MIN 用在放大上）；`mip-levels` 原来的判据是"选级对"，现在也只剩"我们拒了它" ⇒ 能力没变好、断言反而变少；还顺带删掉了一个错误前提却没换成正确判据。
4. **拒绝的代价是画面**：`MAG!=MIN` 与 `MIPFILTER=NONE` 在真游戏里都是常见组合（`MIPFILTER` 的 D3D9 缺省值就是 `D3DTEXF_NONE`，见 [d3dsamplerstatetype](https://learn.microsoft.com/en-us/windows/win32/direct3d9/d3dsamplerstatetype)）⇒ 命中即整批不画。

## "缩小采样选级零覆盖"（内容 vs 选级，别一概而论）

- **内容（CPU 侧派生级算法）有覆盖**：`tests/d3d9_mip.cpp` 的纯函数用例（`d3d9-mip/box-2x2-to-1x1` / `box-odd` / `chain-geometry` / `lod-levels` / `triangle-lod-right-batch` / `perspective-derivatives` / `degenerate`）测 `mipLevelCount` / `mipLevelWidth/Height/Pitch/Offset` / `mipDownsampleBox` / `triangleMipLevel`；而且 GPU 路径用的就是同一批函数（`d3d9_gpu.cpp:2047-2099`）⇒ 派生级的"内容"这条路是活的。
- **真正零覆盖的是"选级后的像素"**：`tests/d3d9_pipeline.cpp:1016-1026` 翻面后只剩"拒绝 + 记账 + 像素不变"；原用例 ②（2:1 ⇒ level1 `[3,16;114,0]`、4:1 ⇒ level2 `33`）与 ③（`MIPFILTER=NONE` + 4:1 ⇒ level 0 的 texel）**都没有替代**。GPU 侧 LOD→级号由驱动按 UV 导数算 ⇒ 这恰恰是**必须靠像素读回**才能守的一段，纯函数守不到。判据 2 就是补这个洞；若短期不做，必须在本单备注里写明"GPU 侧缩小采样的**选级**不可判"。
- `ensureTextureMipChain` / `g_mip_chains` 那条环已死 —— 它是 **CPU 版本**的链缓存，与 GPU 的链生成是两套东西 ⇒ ⛔ 别把它的纯函数判据当成"GPU 选级有证据"。

## 复现

```powershell
cd apps/emulator-lift
cmake --preset windows-x64; cmake --build --preset windows-x64 --target lift-d3d9-pipeline
ctest --test-dir out -R d3d9-pipeline-gpu -V   # 今天：min-filter / mip-levels 期望 INVALIDCALL + bit38 / bit22。⚠ 需真 GPU，否则 SKIP 77
ctest --test-dir out -R d3d9-mip -V            # 纯函数仍绿（不覆盖渲染路径）—— 这条**不需要 GPU**
# 游戏侧读数：这两条闸在 boot→TITLE 里各命中几次（0 只说明"今天没踩"，不代表不该修）
./out/emulator-lift.exe --appdata ../../.tmp/appdata --game-dir ../../links/install --present headless --native-overrides all --skip-logo --max-frames 600 --log-level info --log-cat d3d9 | tee ../../.tmp/filter/replay.log
grep -c 'MAG==MIN' ../../.tmp/filter/replay.log                       # MAG!=MIN 的拒批数
grep -c 'MIPFILTER=NONE 且本批有缩小采样' ../../.tmp/filter/replay.log
```

## 收口锚点（`verify` 候选）

- `apps/emulator-lift/tests/d3d9_pipeline.cpp#d3d9p/min-filter`、`#d3d9p/mip-levels` —— CTest `d3d9-pipeline-gpu`（`CMakeLists.txt:1021-1023`）。
- `apps/emulator-lift/tests/d3d9_mip.cpp#d3d9-mip/lod-levels` —— CTest `d3d9-mip`（`CMakeLists.txt:1136-1138`，顶层无条件、**不依赖 GPU** ⇒ pure 档）。
- ⛔ 不要引 `gpu-batch/sampler-filter-eligibility` 作 `verify` —— 它今天的判据就是"必须拒绝"，与本单相反（本单要求它改成"允许 + 像素不同"）。

★ **`verify` 为什么留空**：`min-filter` / `mip-levels` 今天在**锁着旧行为**（期望 `INVALIDCALL` + bit38 / bit22），修完要随实现**一起翻面**；现在写进 `verify` 等于拿"缺陷被锁死"当收口凭据。翻面之后再把 `verify` 写成 `d3d9p/min-filter`（`d3d9-mip/lod-levels` 只覆盖纯函数，不能单独立据）。

## 未测

- 两条闸在 boot→TITLE 里的命中次数：**未测**（用上面 `grep -c`）。
- 正文里的 `文件:行` 取自 `.tmp/cpu-migration/tickets.md`（2026-11-14 当前工作树），并发改动会让行号漂。
