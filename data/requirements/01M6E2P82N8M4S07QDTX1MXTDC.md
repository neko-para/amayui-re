# emulator-cxx · ADV 文本层逐行对齐 / 平移 / 显示适配比

- id: REQ-01M6E2P82N8M4S07QDTX1MXTDC
- type: req
- status: doing
- parent: REQ-01M5SZ3M7GCR0M23RSYBD7GEA5
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

## 仍待

- **y 读者**（`sub_4575B0` / `sub_4573A0`）与 120B 行记录日志序列：本模型只做 ADV 可见文本的 x；
  `0x300` 重放后的重对齐未做。
- **`0x70` 画布**仍用未缩放 `f14/f18`，未把缩放值写回 `f7c/f80`；绘制项位置 / 源矩形未乘
  `Font+0x355E0/355E4`；纹理消费的 `s` 与画布比的关系见 `RES-ADV-TEXT-CANVAS-SCALE`。
- `0x213` 改基后的旧条目 / 场景绘制模式 ≠ 1 时 `0x71` 不清条目：这些条目不会被对齐（旧问题）；
  `0x300` 部分重放后再追加可能撞 `item_key`（实现面复审指出的边缘）。
- 对齐对的 `+0x124` 无构造初值（spec 已写）；`0x303` 对不存在组崩溃按 spec 保留。

## 判据

1. 两行宽度不同、模式 1：两行各自的 x 中心都 = 层左 + 该行起点 + 基准；只整块中心满足 ⇒ 红。
2. 模式 2：层左 0、原点 20、基准 100 ⇒ 行右界 = 120（不是 100）。
3. 模式 0：无平移；其它取值：Δ = 0（参与）。
4. `0x303` 写在追加之后、下一次绘制必须生效；`0x198` 移动层后不被 `0x71` 覆盖。
5. 适配比 1.28 / 0.72（config WinX/WinY=1000、viewport 1280×720、窗口态 + VF type 2）：
   画布 `f7c/f80`、绘制项位置 / 源矩形、Δ 各按方向缩放。
6. `op_72`：跳过位已置时 `input_word()` 保留、第 31 位不置；未置时整字 / 锁存清 0。

## 边界

- 机器事实锚在 `ADV-TEXT-DRAW-POSITION` 的 `bindings.sites`；本单只做实现与守卫。
- `apps/emulator` / `apps/emulator-new` 是冻结件，⛔ 不改。
- 守卫落 `apps/emulator-cxx/tests/core_selftest.cpp`（顶层 CTest），并做一次「改坏 ⇒ 红 ⇒ 还原」。
