# 🐞设置界面示例文本偶发一个字变细（字重 0x2bd 也是画的时候现读）

- id: REQ-01M644B2T9G7FK1KB0CGZF4V1W
- type: bug
- status: done
- parent: REQ-01M5BK8QMQYK7XQG707YVMQ202
- verify: apps/emulator-cxx/tests/core_selftest.cpp#check_text_pending_style_snapshot
- repro: apps/emulator-cxx/tests/core_selftest.cpp#check_text_pending_style_snapshot

## 现象

设置界面「ADV 设定」页底部示例文本（文本层 9 的画布 = 纹理 29）里，**偶发某一个字的笔画粗细与其它字不同**
（同一个字号、同一个字，有时粗体有时常规体）。用户口径："ADV的文本有概率粗细不对，也是只有一个字"。

## 根因（量化过）

`draw_text_into_texture()` 在**画的时候**现读 `machine.font_metric_a()` 挑 Bold / Regular 字体文件
（`repo:apps/emulator-cxx/src/core/text_raster.cpp`），而跨帧队列的快照里只有 `size/draw_mode/primary/secondary`
—— **字重没进快照**。示例文本由脚本 52（CONFIG.BIN）在 clock=2720 追加（当时字重 700），
脚本 20940 的行块在 clock=2832 写 `0x2bd 0`、同一帧稍后 50404 写 `0x2bd 1`；恰在这一帧**帧首**被推进出来的
那一格就按 Regular 画了出来（tick=16 命中第 5 格「神」x=168）。

- trace：12 次绘制里**恰好 1 次** `metric=0 bold=0 font="Amayui-CN_cnjp.ttf"`，其余 11 次 `metric=700`；
  size/mode/align/两色/src/fmt 全同。
- 量化（RGBA alpha）：该格 α 总和 −17.9%～−22.2%，**墨迹高度完全不变**（30/30、25/25、13/13）
  ⇒ 与已收口的「字号变小」缺陷正交，确实是笔画粗细。
- **决定性实验**：只把 `message:MessageSpeed` 改成 0（追加即抽干）⇒ 10 个 tick × 12 次绘制全部 `metric=700`，
  异常 0 次 ⇒ 只在跨帧队列存在时出现，且追加那一刻的值就是 700。

## 修法

- `PendingGlyph` 加 `weight`，主文本与振り仮名两条追加路径都写 `font_metric_a()`；
- `TextStyleSnapshot` 加 `weight`；`draw_text_into_texture` 用它（`use_bold` / GDI 的 `gdi_font_ascent` /
  `gdi_load_glyph` / 诊断行都改成快照值，⛔ 不再现读）。
- spec：`repo:data/spec/chapters/62-adv-text.md` 的「逐字符显示」条把样式清单补成
  「字号 / **粗细** / 绘制模式 / 主色 / 副色」，判据里加 `0x2bd`。

## 守卫（`apps/emulator-cxx/tests/core_selftest.cpp`，CTest `emulator-core-adv-pacing`）

`check_text_pending_style_snapshot` ②段：三条机对照（异色白/黑，让字重门生效）——
① 基准（追加时 700）；② 追加**之后**写 `0x2bd 0` ⇒ 光栅结果必须与①**完全相同**；
③ 追加**之前**就是 0（对照）⇒ 必须与①**不同**（防"Bold 字面缺失时全等"的假绿）。
**变异实测**：① 把 `draw_text_into_texture` 的字重改回现读 ⇒ 红（`墨迹 281 vs 基准 434`）；
② 删掉 `pending.weight = font_metric_a()` ⇒ 红（三条机全变常规体）。

## 欠账

- 振り仮名支没有单独的守卫（主/假名共用同一条绘制路径，删主文本那行已能红）；
- `font_align_mode()` / AA 配置仍是"画的时候现读"（本屏 12 次绘制里恒为 `align=1 / fmt=1`，未成缺陷）。
