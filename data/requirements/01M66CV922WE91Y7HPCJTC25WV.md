# 🐞 ADV 长按鼠标持续推进（引擎几乎无长按 repeat 逻辑）

- id: REQ-01M66CV922WE91Y7HPCJTC25WV
- type: bug
- status: done
- parent: REQ-01M5BK8QMQYK7XQG707YVMQ202
- verify: apps/emulator-cxx/tests/core_selftest.cpp#按住不放只放行一次
- repro: docs/02-engine/repro/adv-hold-repeat.md#观测点
- severity: S2
- tags: [emulator-cxx, adv, render]

## 根因（已取证）

输入是**事件锁存**，不是电平：窗口过程把"按下"或进输入字（`0x4B8DE6`）、消费者读一次就清
（`0x4772A9/B7`、`0x41330B`）。实现却每帧读 `mouse_button_source_ & 1` ⇒ 按住不放时，
脚本重新武装 `0x72` 后当拍就被放行 ⇒ 一句接一句推进。

## 修法

`service_advance_wait` / `service_choice_click` 都改成**上升沿锁存 + 消费一次**：
上一拍没按、这一拍按 ⇒ 置锁存；放行 / 选中时清掉。选项那边的边沿状态**每拍都更新**
（窗口没开也更新），否则"打开窗口的那一下点击"会被当成新按下、立刻选中。

## 守卫

- `emulator-core-adv-wait` 第 ④ 段：两条 `0x72` 连着来，**按住不放只许放行第一条**；
  松开再按才放行第二条。
- 变异实测：把 `if (down && !advance_input_prev_)` 改回 `if (down)`（电平）⇒
  报"按住不放把第二条也放行了（opt1=1）"，本条红。
- 选项侧：`emulator-core-choice-consumer` 改成"开窗之后才按下"，开窗那一下不再被当选中。

## 残留（已登记）

`RES-MSG-WAIT-INPUT-ATARM`：引擎在起播时还会**采一次输入并把输入字清 0**，本模型没有"输入字"这一层、
未做这次清字；两者对"按下早于置位"的处理可能不同（下一轮用真实时序比一次）。
