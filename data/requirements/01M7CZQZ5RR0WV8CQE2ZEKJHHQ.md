# A · 整 .text 静态 lift（构建期 AOT：guest 代码 → IR → 与 shim 联编成单一二进制）

- id: REQ-01M7CZQZ5RR0WV8CQE2ZEKJHHQ
- type: req
- status: open
- parent: REQ-01M7JHSEKXW8KAKB6K2RS645M7
- tags: [lift, aot]

## 范围

构建期把 guest `.text`（从 PE `entry` 起）lift 成 IR，与 runtime + Win32/D3D shim 编成**一个二进制**；
四条硬口径 + 三条落地附加口径见 `REQ-01M7CHE8HBPVEYTNSD05SJSW47`。

★ **差分 oracle 已不是"解释器进程"**（2026-11-16 起）：解释器 target 已删除（`AGENTS.md` §12 开头），
只剩 `emulator-lift-runtime` 的 `Machine` / `execInsn` 被 `repo:apps/emulator-lift/tests` 当夹具用。
⇒ 凡"与解释器臂比"的判据，对照面都是**夹具里的 `Machine`**，⛔ 不是 `out/emulator-lift.exe`（那是退役前的旧产物）。

## 判据的方向（细则在子单）

- **"一次都不交回解释器"要用发货档读得到的量**：启动 readout 的 `table : … hits=… misses=…` 里 **`misses=0`**
  （六项语义指纹之一，§12.6）。
  ⚠ `handoffs` **只存在于两个测试夹具**（`repo:apps/emulator-lift/tests/lift_ir_shim.cpp`、
  `repo:apps/emulator-lift/tests/lift_ir_proto.cpp`）的统计结构里 —— 发货二进制**不产出它**
  （`rg handoffs apps/emulator-lift/src` 零命中）⇒ 它只作**夹具级**证据，⛔ 不作发货档判据（2026-11-17 核）。
- 与 `Machine` 夹具在同一 `--clock=virtual`、同一 present 计数下**逐位一致**（对照面同上，不再有"解释器臂"）；
- 一个字：**可 lift 性不是瓶颈**（96.1% / 93.4% 加权覆盖，`OBS-20261114-11`），瓶颈在发射器与钩子。

## 为什么它是独立一支

它换的是**执行后端**，不属任何一条内容阶段（M2 画面 / M3 3D / M4 音频）；挂在阶段下会被那些子单埋掉。
