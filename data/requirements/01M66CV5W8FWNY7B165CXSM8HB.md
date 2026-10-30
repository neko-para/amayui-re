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
  `message_subsystem_busy()` 检查它。（原先把“已排窗未盖章 `window_start==0`”也算忙的那条兜底
  已在 2026-10-31 删除：引擎在第一次推进时无条件盖章，那个状态在引擎里不存在。）
- 修后同一 trace：`0x323@368ms` → `0x21C@368ms` → `0x1F7` 实际派发 @ **1184ms**（0x323 之后 816ms）。
- TS 参考实现同步 `SceneState.pending` / `serviceFlags()` 谓词 A。
- 守卫：`emulator-core-wait-colour-window`（0x202 dur=200 + 0x21c：起点+199 仍置起、**起点+200（相等）即已到期并放行**；
  `dur=0` 不许把门永关，`dur=0 + delay=200` 证明延迟段算窗内）；`emulator-core-adv-click-rate` 按
  “整窗挡住是引擎语义”重定基线（8M 步、**实测 37（程序侧基线）**、阈值 30）。
- 旧结论“第二/三次一次 0x202 都没有”**作废**：返回 TITLE 会重跑建场（49 条 0x202）；
  `0x1F7→0x1FB→0x203` 是逐帧显隐驱动。

## 已裁决（2026-10-31，不再是“需要真机”）

`RES-WAIT-COLOUR-PACING` 已**裁决并删除**（`data/spec/audit.json` 不再有这条残差）：

- “颜色窗没走完 ⇒ `0x21c` 不放行”是**引擎规则**，已完整静态取证（排窗置闩 / 渲染步入清 / 推进时再置 / 谓词 A 返回闩），
  已写进 `CORE-OPS-WAIT` 正文与可证伪条件；
- “每句 1.8 s”是**脚本数据**（`0x202` 的持续时间操作数 / `0x238` 的时长 / `0x1b5` 的每字间隔），不是引擎常量
  ⇒ ⛔ 不进 spec 残差、也不加替代残差。整窗挡住后同一条时间线实测推进 37 句（程序侧基线，随脚本/配置变化要重算）。
