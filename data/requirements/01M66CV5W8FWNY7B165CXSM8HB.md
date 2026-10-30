# 🐞 从 OPTION / Game Start 退回 TITLE：按钮提前渲染（未随整屏渐变）

- id: REQ-01M66CV5W8FWNY7B165CXSM8HB
- type: bug
- status: done
- parent: REQ-01M5BK8QMQYK7XQG707YVMQ202
- verify: apps/emulator-cxx/CMakeLists.txt#emulator-core-wait-colour-window
- repro: docs/02-engine/repro/title-partial-redraw.md#观测点
- severity: S2
- tags: [emulator-cxx, title, render]

## 处置（2026-10-31，用户确认修复）

- 根因：`0x21c` 的自然放行没看「待处理」闩 `+B5B4`（`0x202`/`0x323` 排窗置、渲染步入清、`sub_407E20` 返回它）。
- 修复：`Machine::message_subsystem_busy()` 接上该闩；CTest `emulator-core-wait-colour-window`（`verify` 指向它）。
- 用户 2026-10-31 实测确认：**从 OPTION / Game Start 退回 TITLE 时按钮不再提前整块出现**，随整屏一起渐变。
- 判据保留：`verify` 的那条 CTest 是「窗没走完不放行」的守卫；本条的现象级复现步骤见 `repro`。

## 备注（与残差的关系）

本条属于「**A 类弃权**」的反例：`RES-WAIT-CLOCK` / `RES-VM-SCENE-SWITCHES` 覆盖的是「时钟/推进步的跳过条件」，
而不是「门读不读待处理闩」—— 后者本来就有合同。所以这类缺陷**不该**被残差挡住。
