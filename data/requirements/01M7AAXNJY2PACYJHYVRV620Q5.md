# emulator-lift · d3dx9 sprite 的 CPU 合成器是另一条 CPU 像素路径（删 CPU 光栅不会带走它）

- id: REQ-01M7AAXNJY2PACYJHYVRV620Q5
- type: req
- status: done
- parent: REQ-01M6H6G6DSRG1ZRD810Z0HTH8V
- verify: apps/emulator-lift/tests/gpu_batch.cpp#d3d9p/sprite-no-cpu-composite
- done_reason: 删掉 src/runtime/d3dx9_sprite.cpp 的 compositeBatch 里 !gpu_done { … } 整块（GPU 回退前的屏障 + 双层逐像素循环：中心采样 / MODULATION / SRCALPHA 混合 / storePixel），以及只为它存在的 mul255() / sprite_fallback_reason / written_before / 旧 u0/u1/v0/v1 浮点裁剪。回退分支改成「跳过这一条 + skip_reason」（不再写像素）；账本调用改成 gpu_done ? gpuNoteSpriteDrawn() : gpuNoteSpriteNotDrawn(skip_reason)，不再 if (sprite_gpu_active) 门控 ⇒ 没后端也记账。d3d9_pipeline.h 删 sprite_fallback_draws / sprite_fallback_pixels 两个字段；gpuNoteSpriteResult(bool,pixels,reason) 拆成 gpuNoteSpriteDrawn() + gpuNoteSpriteNotDrawn(reason)（后者原因进 skipped_reasons）。machine.cpp 摘要从 sprite: draws= gpu= fallback= fallback_pixels= skipped= 改成 sprite: draws= gpu= skipped=；frame-trace 的 sprite_fallback= 改 sprite_skipped=。保留 fallback_* / gpuNoteFallback / fallback_reasons / fallback top:：draw 侧沿用「保留接口、无写入方」的既有裁决，现在 draw 与 sprite 都没有写入方（恒 0，fallback top: 不出现）。测试：tests/d3dx9.cpp 拆成 pure 臂（协议 / 生命周期）+ --gpu 臂（三个像素用例，期望值一个没改，比较改「每通道 ≤1 LSB」）；CMakeLists.txt 新增顶层 d3dx9-gpu（SKIP_RETURN_CODE 77 + SDL_VIDEODRIVER=dummy），基线 83 → 84 条。两处真行为变化：① 16bpp 目标的 sprite 原来由 CPU 按 R5G6B5 写出 0xF81F，现在「非 32bpp ⇒ 跳过」，断言改成「像素仍是清屏色」；② 源矩形部分越界原来由 CPU 裁一截画，现在「GPU 不裁剪 ⇒ 记账 + 跳过」，断言改成四个采样点保持清屏色。实测真实场景 130 / 200 / 600 帧命中均为 0，但未在更靠后的存档 / 战斗复测（600 帧只到 ADV 那一段）—— 不是「已验证不影响」。反向条款：把 sprite 的 GPU 批提交改坏（ok = gpu->rasterQueue(...) → ok = true，假装成功）⇒ lift-d3dx9 --gpu 190/0 → 190/33 failures、lift-gpu-batch --gpu 375/0 → 375/3；在 !gpu_done 分支注入一条 CPU 像素写 ⇒ lift-gpu-batch（pure）587/0 → 587/2；两处变异都已还原复测。注：假提交时账本 sprite_gpu_draws 仍是 1 ⇒ 只有像素断言抓得住，这正是像素断言该留的理由。最硬的一条证据：逐帧后缓冲 hash 与改前逐帧相同 —— 130 帧 130/130、600 帧 600/600（两份 --log-level info 日志的 Present: 第 N 帧 hash=0x… 行逐帧比对）。第二条守卫（--gpu 臂，无卡 SKIP 77）不进 verify 字段（该字段只收一条）⇒ 记在这里：apps/emulator-lift/tests/d3dx9.cpp 的三个像素用例 d3dx9/draw+end-pixels、d3dx9/texture+draw、d3dx9/sprite-donotaddref-stale-texture。
- tags: [emulator-lift, sprite, cpu-retire]

## 背景

`d3dx9_sprite.cpp` 的 `compositeBatch` 曾在 GPU 不可用 / 被拒时**自己逐像素**把 sprite 混进颜色目标（后缓冲 arena，或绑定的纹理 RT）。
它不是 `rasterTriangle`，所以 `REQ-01M7A74YG08R65NJBFXF7DEJDK` 删 CPU 光栅时**不会**顺带删掉它；`tests/d3dx9.cpp` 的像素断言就挂在这条路径上。

## 结论（2026-11-14 收口）

- `!gpu_done { … }` 整块已删（GPU 回退前的屏障 + 双层逐像素循环：中心采样 / MODULATION / SRCALPHA 混合 / `storePixel`），以及只为它存在的 `mul255()` / `sprite_fallback_reason` / `written_before` / 旧 `u0/u1/v0/v1` 浮点裁剪。
- 回退分支改成「跳过这一条 + `skip_reason`」（⛔ 不写像素）；账本走 `gpu_done ? gpuNoteSpriteDrawn() : gpuNoteSpriteNotDrawn(skip_reason)`，**不再** `if (sprite_gpu_active)` 门控 ⇒ 没后端也记账。

## 判据落点

- 本单 `verify`（pure，任何机器可判）：`apps/emulator-lift/tests/gpu_batch.cpp#d3d9p/sprite-no-cpu-composite` —— 不装 `GpuRaster` ⇒ 像素一个不写 ∧ `sprite_draws==1 ∧ sprite_gpu_draws==0 ∧ sprite_skipped==1` ∧ 原因以 `sprite:` 开头。
- 像素臂：`apps/emulator-lift/tests/d3dx9.cpp` 的 `--gpu` 臂（CTest `d3dx9-gpu`：`CMakeLists.txt` 顶层无条件注册 + `SKIP_RETURN_CODE 77` + `SDL_VIDEODRIVER=dummy`；无卡 ⇒ SKIP 77，⛔ 不假绿）—— `d3dx9/draw+end-pixels` / `d3dx9/texture+draw` / `d3dx9/sprite-donotaddref-stale-texture` 三个像素用例，期望值一个没改，比较改成「每通道 ≤1 LSB」。同文件的 pure 臂保留协议 / 生命周期（不判像素）。
- 账本与其摘要：`d3d9_pipeline.h` 删 `sprite_fallback_draws` / `sprite_fallback_pixels`，`gpuNoteSpriteResult(bool,pixels,reason)` 拆成 `gpuNoteSpriteDrawn()` + `gpuNoteSpriteNotDrawn(reason)`（原因进 `skipped_reasons`）；`machine.cpp` 摘要 `sprite: draws= gpu= skipped=`、frame-trace 的 `sprite_fallback=` → `sprite_skipped=`。
- **保留** `fallback_*` / `gpuNoteFallback` / `fallback_reasons` / `fallback top:`（draw 侧「保留接口、无写入方」的既有裁决）。现在 draw 与 sprite 都没有写入方 ⇒ 恒 0，`fallback top:` 不出现。

## 两处行为变化（真行为改变，不是措辞）

1. 16bpp 目标的 sprite：原来由 CPU 按 R5G6B5 写出 `0xF81F`，现在「非 32bpp ⇒ 跳过」；断言改成「像素仍是清屏色」。
2. 源矩形**部分**越界：原来由 CPU 裁一截画，现在「GPU 不裁剪 ⇒ 记账 + 跳过」；断言改成四个采样点保持清屏色。

实测真实场景 130 / 200 / 600 帧命中均为 0，但**未在更靠后的存档 / 战斗复测**（600 帧只到 ADV 那一段）。

## 读数

两组读数**尚未**登记进 `docs/03-emulator/observations.md`（**待登记**，登记不在本单 —— ⛔ 不是「已登记」）：130/200/600 帧的 `sprite:` 账；逐帧后缓冲 hash 与改前逐帧相同（130/130、600/600）。

## 非目标

不改 sprite 合批 / 顺序语义；不扩带纹理点 / 线。

## 关系

父节点是 M2（`REQ-01M6H6G6DSRG1ZRD810Z0HTH8V`）。与 `REQ-01M7A74YG08R65NJBFXF7DEJDK` 是同一轮迁移的**兄弟**（沿用它的账本 / 分档口径、具名位与摘要字段），**不是**父子 ⇒ 本单收口不会把它的聚合从 `[3/4]` 抬到 `[4/4]`（它剩下的一格是 `REQ-01M7AZ65EAE7Q6STW2S0ZNXGM4`）。
