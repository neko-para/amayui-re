# 🐞设置界面示例文本偶发一个字变小（跨帧队列按画的时候读字号）

- id: REQ-01M64274TVKACYGAGKVXSE2JYD
- type: bug
- status: done
- parent: REQ-01M5BK8QMQYK7XQG707YVMQ202
- verify: tools/test/spec-gaps.test.mjs#★ 精确划分
- repro: apps/emulator-cxx/tests/core_selftest.cpp

## 现象

设置界面「ADV」页底部的示例文本（`天結 / 天结神縁SAMPLE`），**偶发**其中一个字按更小的字号画出来 ——
字宽仍是 30 号字的推进量，所以看起来像"一个字莫名变小"，并且**位置会偏**。

## 根因（已定位，实测）

示例文本走文本层的**跨帧队列**（`0x6e` 追加、主循环的文本推进器每 D 毫秒画一格）。旧实现把
**字号 / 绘制模式 / 主色 / 副色留到"画的时候"**再读字体对象；而设置界面这一页在示例追加之后、那些格
还没画完之前，会为右列每一行写 `0x75`（A 支字号 20）并画 `0x204`。于是恰好跨帧夹住的那一格就按 20 号
画出来（字宽仍是 30）。

引擎的 `sub_46BE30` 在**追加时**就 SelectObject + 逐字写出（量度与光栅化都在那一刻），之后的推进只是
把排好版的格呈现出来 ⇒ 字号等样式必须**随格快照**。

## 修法

- `PendingGlyph` 增加 `draw_mode / primary / secondary`，`size` 改成"追加当时的字号"（不再是 0=现读）；
- 追加时（主文本与振り仮名两条路径）写入快照；`draw_front_pending_glyph` 用快照画
  （`TextStyleSnapshot` 经 `draw_text_into_texture(..., style)` 传下去）。
- 守卫：`core_selftest` 的 `check_text_pending_style_snapshot`（CTest `emulator-core-adv-pacing`）——
  30 号追加 "M" → 改 `0x75=10` → 跨过 100ms 间隔才放出来；断言入队字号 = 30，且画出来的墨迹高 ≥ 15
  （按 10 号画约 7~9）。**变异实测**：把绘制侧改回"现读"（`size=0, style=nullptr`）⇒ 当场红（墨迹高 9）。

## 复现（无人值守）

`--play-headless` + `apps/emulator-cxx/tests/input/title-option-tab-adv.txt`，只改 `--tick-ms`：
修复前 tick 16 得到 `h=20` 的单字（其余 28）、tick 30 得到 `h=15` 的单字；修复后 11 个 tick 值的
分段高度全部一致（44/28/23/23/23/23/23/22）。判据 = 对 `--dump-texture 29` 的 BMP 做列分段求墨迹高。

## spec 侧同步

这不是纯实现问题：spec 的「逐字符显示」条只写了推进节奏，**没写每一格的样式取哪一刻的值** —— 独立实现者可以合理地读成"推进时现读字体对象"。已在该条补一句（`repo:data/spec/chapters/62-adv-text.md`）：每格的字号 / 绘制模式 / 两色取**追加那一刻**的值，并给出可判定的负例（按新字号画、字宽却仍是追加时量度 ⇒ 本条错）。
