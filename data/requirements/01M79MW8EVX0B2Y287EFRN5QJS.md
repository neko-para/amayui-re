# 音频：解码起播重建的叶子（sub_4E0030 mdct_init 41.8 ms / sub_4DFDA0 32.4 ms）override（原「整函数 sub_4B6570」不可准入）

- id: REQ-01M79MW8EVX0B2Y287EFRN5QJS
- type: req
- status: open
- parent: REQ-01M6H6G6DSRG1ZRD810Z0HTH8V
- tags: [emulator-lift, audio, native-override]

## 订正（2026-11-13 · 音频阶段设计，`.tmp/audio-phase/design.md` §4）

★ **原单的"整函数 override `sub_4B6570`"不可准入**：它是**解码器工厂 + 容器分派**（`docs/audio-feasibility.md:496-497`），
会建 COM/vtable、调 guest Stop ⇒ 违反"纯核 + 不碰 guest 容器"。而且 218.9 ms 里 **203.6 ms 在 `4D8180→4D7BA0`**，
其中 `42×4D9AC0 = 126.5 ms` **已被否**（`.tmp/audio-token/throughput-analysis.md:152-166`）。
⇒ 改为**叶子函数**：

## 现状
- 未做，且此前无票：`rg 4B6570 apps/emulator-lift/src` 只命中 `dsound.cpp` 的注释与 `native_mdct.h` 的说明。
- 已 done 的同族 override：Huffman 子树（`0x4E1570` / `0x4E1950`）、`__ftol2_sse`、MDCT、PCM 段（`0x48D767`）。
- 触发面：`[this+0x414]` 那条分支，**每条语音一次**（不是每刀）。

## 要做（已收窄为叶子）
1. `sub_4E0030`（mdct_init，**41.8 ms/次**）与 `sub_4DFDA0`（**32.4 ms**，先定性）两个叶子，做成函数级 override（`native_overrides.cpp` 的既有形状），进 `all` 组。
2. 守卫：照同族 `mdct/*` 的**数值容差**形状（⛔ 不逐字节）：解释器臂做 oracle，`max|Δ| <= 1e-6 * peak`；另加 `hits==1 && fallbacks==0` 的机械判据（override 真被走到）。
3. 停止线：叶子内若出现 guest 容器/虚表访问 ⇒ 停下报告，**不**把它塞进 override。

## 判据
- 新 CTest `native-override/vorbis-startup-rebuild`：override 生效（`ov` 计数 > 0）+ 数值容差通过。
- 读数（进文档，不进 test）：同一输入下 `fill` 的 `guest` 均值下降，且 `--sound` 实时臂 `underruns` 不增加。

## 非目标
- 不做 `sub_4D9AC0`（vorbis_book_init_decode）：它要碰 guest 容器，已否。
- 不追"逐位一致"：实时音频只要求不爆音 / 不重播。
