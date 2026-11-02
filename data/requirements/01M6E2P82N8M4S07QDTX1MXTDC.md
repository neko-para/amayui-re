# emulator-cxx · ADV 文本层逐行对齐 / 平移 / 显示适配比

- id: REQ-01M6E2P82N8M4S07QDTX1MXTDC
- type: req
- status: done
- parent: REQ-01M5SZ3M7GCR0M23RSYBD7GEA5
- verify: apps/emulator-cxx/CMakeLists.txt#spec_adv_text_draw_scale
- tags: [emulator-cxx, spec-followup]

本单是 `REQ-01M6E0K08SMX4ZV85ZMVESC1G7`（spec 收口）明确留给模拟器侧的跟进项。spec 的合同已写在
`ADV-TEXT-DRAW-POSITION`（`62-adv-text`）；⛔ 本单不回头改 spec 去迎合实现。

## 本轮已完成（2026-11-02）

- `align_text_layer()` 改成**逐行 Δx 平移**：按 `layer.cells` 的 y 归组（振り仮名 `span_exclude` 不参与）；
  种子 min = 层宽拷贝、max = 文本原点；模式 1 Δ = 基准 − trunc(跨度/2)、模式 2 Δ = 基准 − 右界 + 左界、
  模式 0 无 Δ、其它 Δ = 0；只改绘制项 `aux[0]`，**不写回** `f0c/f10`；`0x303` / `0x70` / `0x198`
  写入后重算现有条目（`0x303` 后置也生效）。
- CTest `emulator-core-text-layer-align` 重写：单行行心 = 层左 + 原点 + 基准（120，不是 100）、
  两行宽度不同各自 120、模式 2 行右界 120、模式 0 无平移、`0x303` 后置生效；
  变异「把行按 y 归组改成整块一组」实测红（两行行心偏）。
- `op_72`：跳过位已置时整段 return；未置时清输入字 / 两个事件锁存 / 推进锁存，再置第 31/30 位；
  `adv-wait` ⑥（整字清 0）/ ⑦（skip 支，`dispatch_for_test`）已绿；变异「把清字移到 bit27 判断之前」红。
- **显示适配比**（本轮补齐，`0x45d6da/ec` + `0x45bf01` + `0x45c1ed`）：纹理模式下 `0x70` 把
  `f7c/f80` 覆写成 `trunc(宽高 × 字体适配比 + 0.5)`，画布按这两格建；逐字条目源矩形 = 布局格 × 比
  （x/x1 横比、y/y1 纵比）；对齐 Δ 乘横比；位置「层左/上 + 格」**不**乘字体比（按场景 `s`，本模型 s = 1）。
  CTest `spec_adv_text_draw_scale`：比 1.0 / 1.28×0.72 两趟逐项比对画布、源矩形、Δ、位置；
  变异「源矩形不乘比 / 画布用 f14/f18 / Δ 不乘比 / 位置乘比」四处都实测红。
  字形 / 文本游标是否同乘一个比仍是残差 `RES-ADV-TEXT-CANVAS-SCALE`（本单不发明）。
- `0x300` 重放：`reveal_text_layer_cell` 重建条目后调 `align_text_layer`（重放那条也吃当前对齐）；
  CTest `emulator-core-text-gate` 新增「两格揭示后行心 = 5 + 5 + 100 = 110」，删掉那次 re-align ⇒ 实测红（67 vs 110）。

## 仍待（已移到 `REQ-01M6E5WK3GDA57AR2DSVXXJQE9`）

- 纵向读者 `sub_4573A0` 只被 `sub_45B2B0`（层记录 `+0x70 == 1` 的另一条绘制入口）调用；
  ADV 可见的逐字路径 `sub_45BE20` 只调横向 `sub_4572A0`（锚 `0x45c1e1`）⇒ 本模型 x-only 是机器事实。
- 120B 行记录日志序列（`sub_4574A0` / `sub_4575B0`）本模型未建模。
- `0x213` 改基后的旧条目 / `item_key` 撞键边缘。

## 判据

1. 两行宽度不同、模式 1：两行各自的 x 中心都 = 层左 + 该行起点 + 基准；只整块中心满足 ⇒ 红。
2. 模式 2：层左 0、原点 20、基准 100 ⇒ 行右界 = 120（不是 100）。
3. 模式 0：无平移；其它取值：Δ = 0（参与）。
4. `0x303` 写在追加之后、下一次绘制必须生效；`0x198` 移动层后不被 `0x71` 覆盖。
5. 适配比 1.28 / 0.72（config WinX/WinY=1000、viewport 1280×720、窗口态 + VF type 2）：
   画布 `f7c/f80` = `trunc(宽高 × 比 + 0.5)`；源矩形 / Δ 各按方向缩放；位置 = 层左/上 + 格 + 缩放后的 Δ
   （自身位置按场景 `s`，本模型 s = 1）。
6. `op_72`：跳过位已置时 `input_word()` 保留、第 31 位不置；未置时整字 / 锁存清 0。

## 边界

- 机器事实锚在 `ADV-TEXT-DRAW-POSITION` 的 `bindings.sites`；本单只做实现与守卫。
- `apps/emulator` / `apps/emulator-new` 是冻结件，⛔ 不改。
- 守卫落 `apps/emulator-cxx/tests/core_selftest.cpp`（顶层 CTest），并做一次「改坏 ⇒ 红 ⇒ 还原」。
