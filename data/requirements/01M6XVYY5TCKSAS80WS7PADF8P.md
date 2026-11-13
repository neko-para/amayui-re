# emulator-lift · 识别驱动交互状态机：任务 = 节点图（when → action → next），三种失败全部响亮；两种识别源（脚本身份 / 画面特征）

- id: REQ-01M6XVYY5TCKSAS80WS7PADF8P
- type: req
- status: done
- parent: REQ-01M6XVYA68DQWWMQPY795WPN0Q
- order: 1
- verify: apps/emulator-lift/tests/interaction_graph.cpp#script-cond-forms
- tags: [emulator-lift, interaction, state-machine]

## 现在到哪了（2026-11-10：进 / 出 EU-ROOM 都跑通；只剩"脚本身份"识别源）

- **节点图 runner**：`repo:apps/emulator-lift/src/runtime/interaction_graph.h` + `repo:apps/emulator-lift/src/runtime/interaction_graph.cpp`
  （行写法解析 / 状态机 / 三种失败 / trace / `failureReport()` / `defaultFailDumpPath`）。
- **画面采样**：`repo:apps/emulator-lift/src/runtime/screen_sample.h` + `.cpp`，采样点 = Present（唯一呈现汇点）；
  整帧 hash / 矩形均值 / 矩形 hash / "与上一次采样不同"的**单调计数**。
- **CLI**：`--interaction-graph` / `--interaction-trace` / `--interaction-fail-dump`；三种失败 **exit 5**；
  "任务没走完"（guest 侧硬停）也 **exit 5** —— 见 `repo:apps/emulator-lift/README.md` §4.2d。
- **动作仍走时间线那套注入**：图把动作**追加**进 `InputScript`，自动 hold / 延迟抬起 / profiler 窗口 / `quit` 只有一份实现
  （⛔ 不另写一套按下抬起）。可选按钮词（`click … [left|right]`，缺省 left）也走同一条路。
- **守卫**：`repo:apps/emulator-lift/tests/interaction_graph.cpp`（19 条 pure 用例）+ 变异条目 `ctest:interaction-graph`。
- **脚本**：`repo:apps/emulator-lift/scripts/interaction/` 三份（进房间 / 出房间·左键 / 出房间·右键），全部**零帧号**。

## 判据对照

| 判据 | 现状 |
|---|---|
| 进 Eushully-chan Room，且**不依赖帧号** | ✅ 真机实测（`--interaction-graph room-enter.graph` ⇒ exit 0） |
| 三种失败全部响亮 | ✅ 各一条 pure 用例（`timeout-loud` / `no-sample-blind` / `expect-change-loud`，另有 3 条回归）+ stderr 打"最后状态与全部候选"；⚠ **exit 5 只有真机实测**（退出码路径在 `main`，pure CTest 看不见） |
| `next` 是单个 id；写不存在的 id ⇒ 解析期失败（带行号） | ✅ `parse-errors`（13 条错误各带 `origin:行:`） |
| 节点图可在 pure CTest 里用合成输入跑 | ✅ 19 条（含 `graph-scripts-parse` 把三份真图纳入"必须能解析"） |
| trace 可 diff | ✅ `trace-diff-stable`（逐字节相同）；`--present window` 首行 `# NON-DIFFABLE` |
| ⛔ 不以"帧号到 X"当判据 | ✅ 三份脚本里 0 个帧号 |
| **出** Eushully-chan Room（同一终态） | ✅ 真机 exit 0 + `task done` + TITLE hash `0x94CB492BF7F1AEEB`；左键 / 右键各一份图（`OBS-20261110-01/04`） |
| 两种识别源（画面特征 / 脚本身份） | ✅ 画面特征 ✅；`when script` **已实现**（电平条件：解析 `interaction_graph.cpp:433-479`、判定 `:258`；用例 `script-cond-forms` / `script-cond-identity`；2026-11-11 `382e3c4`） |

## 卡住的（已清空，留记录）

1. ~~`REQ-01M6ZDSGQY1RXX3N3WJKA8D26H`~~ —— 后缓冲 `LockRect`/`UnlockRect` **已实现并收口**（2026-11-09）。
2. ~~`REQ-01M708DF24KY0QZNEHY66S0HYW`~~ —— SEH 展开 **已收口**（机制级；它修的是**退出游戏**，不是出房间，
   见 spike `REQ-01M71P87BMTZK4MHT8468BMC3G`）。
3. ~~`REQ-01M6ZDSPVYS3R78TK8TFKVBC8N`~~ —— 「戻る」点击**不是缺陷**（2026-11-10 判 dropped：11-09 的
   "点不动"是量测窗 artifact），图已换回 `action click center`。

## 实测记录

- **进房间（2026-11-09）**：`room-enter.graph` **两次 exit 0**，两份 trace **逐字节相同**
  （`task done nodes=2 transitions=6 actions=2`，insns ≈ 1.30e9）。
- **出房间（2026-11-10，macOS arm64 / RelWithDebInfo，`--present headless --raster cpu`）**：
  `room-exit.graph` 与 `room-exit-right.graph` 各 **exit 0 + `task done`**，
  `transitions=11 actions=3 samples=249 changes=44`，终态 TITLE hash `0x94CB492BF7F1AEEB`
  （三次读数逐位相同，`insns=2,120,728,564`）。
  ★ 观测窗必须盖住一次 TITLE 重载帧（今天单帧 **7.3e8 条** ≈73 s guest）——
  11-09 的误判就是这么来的（`OBS-20261110-02`）。

## 收口（2026-11-13）

- 判据全落地；最后一条（`when script` 电平条件）2026-11-11 `382e3c4` 已实现，纯档用例 `script-cond-forms` / `script-cond-identity`。
- 正文里"`when script` 仍解析期拒绝"的说法是 2026-11-10 的旧读数，已按代码与用例改正。
- 收口凭据：`apps/emulator-lift/tests/interaction_graph.cpp#script-cond-forms`（ctest `interaction-graph`）。
