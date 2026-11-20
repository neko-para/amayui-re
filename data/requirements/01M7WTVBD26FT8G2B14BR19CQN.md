# emulator-lift · SAVE07 进 live2d 战斗前 D3DX 资源耗尽：SurfaceArena 128 MiB 顶穿（S1）

- id: REQ-01M7WTVBD26FT8G2B14BR19CQN
- type: bug
- status: done
- parent: REQ-01M7JHSD5MWTB9N8WJC2Y2T69M
- repro: apps/emulator-lift/scripts/recipes/save07-battle.plan#click 640 517
- severity: S1
- tags: [emulator-lift, d3d9, arena, save07]

## 一句话

`SAVE07` →「结束回合」→「決定」进 live2d 战斗前 `stop : resource-exhausted @ eip=0047839D`：
一张 2048×2048 页把 **128 MiB 的 SurfaceArena** 顶穿（**6/6 稳定**）。

## 观测（复现读数）

- 停机：`resource-exhausted @ eip=0047839D（非正常结局） — D3DX 资源耗尽（shim d3dx9_43!D3DXCreateTextureFromFileInMemory）`；
  触发点 `src/shims/d3dx9.cpp` 的 `d3dxResourceStop`（`D3DXCreateTextureFromFileInMemory` 分支）。
- `region : arena in_use=119,343,064 peak=119,343,064 surfaces=340`；
  请求 2048² A8R8G8B8 = 16,777,216 B（含块头 16,777,224）、**最大空闲块 14,873,992**、缺口 1,902,560 B。
- 总空闲 14,874,664 与最大空闲只差 672 B ⇒ **不是碎片化**。
- 释放链**已证伪泄漏**：换屏时那 3 张 2048² 被释放、同跑 `in_use` 119,032,104 → 18,432,040
  ⇒ 是"工作集确实超过 128 MiB"，不是"没回收"。

## 根因

128 MiB 是**自定模型**（真机上限由驱动 / 显存决定、不可知：`docs/03-emulator/external-docs.md` 偏离 #10；
`design-notes.md` 的 L3-arena-01 早已登记"只够一份屏工作集"的上限风险）。本场景下限 136,120,288 B
（129.81 MiB）⇒ 差 1.9 MB。

## 修法

`SurfaceArena` 128 MiB → **256 MiB**；arena 新上界 `0x38000000` 会吃掉 AGERC 的 fallback base
`0x30000000` ⇒ fallback 搬到 `0x40000000`；`GuestLayout` 成为唯一真源（`machine.cpp` / `aot_host.cpp`
都引用它，⛔ 不再各写字面量）。

## 判据（重开条件）

`repro` 指向这条配方：**日志里再出现 `resource-exhausted`** 即重开；正常形态是
`stop : stopped @ eip=00412383（正常结局）— input-plan: quit` 且第 ~250 帧是战斗界面。
- 修后读数（4 跑逐位同）：`region : arena in_use=92,662,952 peak=141,169,728 surfaces=110`
  ⇒ peak 134.63 MiB < 256 MiB，余量 121.37 MiB。
- 关系不变量由 CTest `region-layout` 钉住（区间两两不相交 + fallback base 不在 arena 内）；
  "批量 create→release 回基线 + 合并后仍能拿满一块"由 `arena-recycle` 钉住（对冲"容量变大让
  漏回收变钝"）。

## 未做 / 剩余风险

- **不是保证**：更大的工作集（更多常驻图集 / 更长的场景链）仍可能耗尽。
- 128 MiB 时代 arena 同时是"漏回收"的**意外探测器**（BUG-01 就是这么抓到的）⇒ 抬容量把那个
  探测器变钝了；本单只加了"分配/释放平衡"的守卫，**换屏 `in_use` 不增长**要多屏真游戏跑
  （`LIFT_STRESS` 档），本单没做。
- rect `Clear` / 战斗内其它大页（若出现）不在本单取证范围。
