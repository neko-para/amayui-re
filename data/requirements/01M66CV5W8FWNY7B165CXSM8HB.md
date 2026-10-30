# 🐞 从 OPTION / Game Start 退回 TITLE：按钮提前渲染（未随整屏渐变）

- id: REQ-01M66CV5W8FWNY7B165CXSM8HB
- type: bug
- status: doing
- parent: REQ-01M5BK8QMQYK7XQG707YVMQ202
- verify: apps/emulator-cxx/CMakeLists.txt#emulator-core-wait-colour-window
- repro: docs/02-engine/repro/title-partial-redraw.md#观测点
- severity: S2
- tags: [emulator-cxx, title, render]

## 现象（用户实测，窗口模式）

从 TITLE 进 **OPTION** 再退回 TITLE、以及进 **Game Start** 再退回 TITLE：按钮（Option / Game Start…）
**提前一整块出现**，不跟界面其余内容一起渐变。期望退回 TITLE 时整屏一起渐变。

## 根因（2026-10-31，已实跑）

`0x21c` 的自然放行**丢了引擎的“待处理”闩** `+B5B4`：`Machine::message_subsystem_busy()`
只建模 `0x238` 计时器 + 待画格/打字态，不看“还有颜色窗在跑”。

- `0x202` 排窗写 `+B5B4=1`（`0x4AD155`）；`0x323` 排窗/窗推进同样写（`0x4AE545` 等）；
  渲染步入 `0x4B40B0` 清 0；`sub_407E20` 在 `+B5BC`（时长）=0 时返回 `+B5B4`（`0x407E97`）。
- TITLE：`0x320` 建 mesh 200000 → `0x322` → `0x323 delay=0 dur=300` → `0x21c` → `0x1f7 200000 1`。
  HEAD trace：`0x21C@368ms` → 同一 clock `0x1F7@368ms` ⇒ mesh 在任何一帧渲染前被拆，
  按钮的 `0x203` 在淡入窗还在延迟段时把起色改成不透明。

## 处置

- 实现新增 `scene_pending_`（= `+B5B4`）：排窗置 1、窗推进每帧再置、渲染步入清 0；
  `message_subsystem_busy()` 检查它，并把“已排窗未盖章（`window_start==0`）”也算忙（只认时长 > 0）。
- 修后同一 trace：`0x323@368ms` → `0x21C@368ms` → `0x1F7` 实际派发 @ **1184ms**（0x323 之后 816ms）。
- TS 参考实现同步 `SceneState.pending` / `serviceFlags()` 谓词 A。
- 守卫：`emulator-core-wait-colour-window`（0x202 dur=200 + 0x21c，start+199/200 仍置起、+201 清；
  `dur=0` 不许把门永关）；`emulator-core-adv-click-rate` 按“整窗挡住是引擎语义”重定基线
  （8M 步、期望 37、阈值 30）。
- 旧结论“第二/三次一次 0x202 都没有”**作废**：返回 TITLE 会重跑建场（49 条 0x202）；
  `0x1F7→0x1FB→0x203` 是逐帧显隐驱动。

## 剩余（需要真机）

`RES-WAIT-COLOUR-PACING`：整窗挡住后 ADV 80 击/24s 只推进 ~37 句（约 0.65s/句）；
仍需一次真机“字出现 → 可点击”的间隔测量，确认每句 1.8s 淡入是否真的挡脚本。
