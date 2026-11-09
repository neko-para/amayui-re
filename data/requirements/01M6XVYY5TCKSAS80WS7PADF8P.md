# emulator-lift · 识别驱动交互状态机：任务 = 节点图（when → action → next），三种失败全部响亮；两种识别源（脚本身份 / 画面特征）

- id: REQ-01M6XVYY5TCKSAS80WS7PADF8P
- type: req
- status: blocked
- parent: REQ-01M6XVYA68DQWWMQPY795WPN0Q
- order: 1
- blocked_by: [01M6ZDSPVYS3R78TK8TFKVBC8N, 01M708DF24KY0QZNEHY66S0HYW]
- tags: [emulator-lift, interaction, state-machine]

# emulator-lift · 识别驱动交互状态机：任务 = 节点图（when → action → next），三种失败全部响亮；两种识别源（脚本身份 / 画面特征）

- id: REQ-01M6XVYY5TCKSAS80WS7PADF8P
- type: req
- status: doing
- parent: REQ-01M6XVYA68DQWWMQPY795WPN0Q
- order: 1
- tags: [emulator-lift, interaction, state-machine]

## 现在到哪了（2026-11-09：第一期落地，出房间卡在另一条单）

- **节点图 runner**：`repo:apps/emulator-lift/src/runtime/interaction_graph.h` + `repo:apps/emulator-lift/src/runtime/interaction_graph.cpp`（行写法解析 / 状态机 / 三种失败 /
  trace / `failureReport()` / `defaultFailDumpPath`）。
- **画面采样**：`repo:apps/emulator-lift/src/runtime/screen_sample.h` + `repo:apps/emulator-lift/src/runtime/screen_sample.cpp`，采样点 = Present（唯一呈现汇点）；
  整帧 hash / 矩形均值 / 矩形 hash / "与上一次采样不同"的**单调计数**。
- **CLI**：`--interaction-graph` / `--interaction-trace` / `--interaction-fail-dump`；三种失败 **exit 5**；
  "任务没走完"（guest 侧硬停）也 **exit 5** —— 见 `repo:apps/emulator-lift/README.md` §4.2d。
- **动作仍走时间线那套注入**：图把动作**追加**进 `InputScript`（新 `appendText`），自动 hold / 延迟抬起 /
  profiler 窗口 / `quit` 只有一份实现（⛔ 不另写一套按下抬起）。
- **守卫**：`repo:apps/emulator-lift/tests/interaction_graph.cpp`（14 条 pure 用例）+ 变异条目 `ctest:interaction-graph`
  （`--match interaction_graph.cpp` 实测红后还原）。
- **脚本**：`repo:apps/emulator-lift/scripts/interaction/room-enter.graph` 与 `room-exit.graph`；
  后者用 `screen rect … avg` + 框中心（ESC）而不是写死坐标，两条都**零帧号**。

## 判据对照

| 判据 | 现状 |
|---|---|
| 进 Eushully-chan Room，且**不依赖帧号** | ✅ 真机实测（`--interaction-graph room-enter.graph` ⇒ exit 0；trace：title 命中 → click → `expect screen changed` → room 命中 → `task done`） |
| 三种失败全部响亮 | ✅ 各一条 pure 用例（`timeout-loud` / `no-sample-blind` / `expect-change-loud`，另有 `rect-blind-is-no-sample` / `acting-expires` / `expect-needs-new-sample` 三条回归）+ stderr 打"最后状态与全部候选"；⚠ **exit 5 只有真机实测**（`room-exit` 跑出 exit 5），退出码路径在 `main` 里，pure CTest 看不见 |
| `next` 是单个 id；写不存在的 id ⇒ 解析期失败（带行号） | ✅ `parse-errors`（13 条错误各带 `origin:行:`） |
| 节点图可在 pure CTest 里用合成输入跑 | ✅ 14 条：`parse-ok` / `parse-errors` / `only-next-target` / `timeout-loud` / `no-sample-blind` / `expect-change-loud` / `changed-counts-window` / `stall-fallback-expires` / `quit-marks-task-done` / `acting-expires` / `expect-needs-new-sample` / `rect-blind-is-no-sample` / `trace-diff-stable` / `graph-scripts-parse` |
| trace 可 diff | ✅ `trace-diff-stable`（逐字节相同）；`--present window` 首行 `# NON-DIFFABLE` |
| ⛔ 不以"帧号到 X"当判据 | ✅ 两条脚本里 0 个帧号（房间空闲时 Present 稀疏到 ~0.5 s/帧） |
| **出** Eushully-chan Room（同一终态） | ⛔ **卡住**（见下） |

## 卡住的（都不是本单能修的，各有一条单）

1. ~~`REQ-01M6ZDSGQY1RXX3N3WJKA8D26H`~~ —— 后缓冲 `LockRect`/`UnlockRect` **已实现并收口**
   （2026-11-09；真机 `LockRect(backbuffer)` 调用成功、旧 trap 消失）。⇒ 但出房间仍 **exit 5**：停在新 gap
   `kernel32!RaiseException`（`Command_Exit_Exception` 的 C++ EH 展开）⇒ 终态判据移到
   `REQ-01M708DF24KY0QZNEHY66S0HYW`（S2，open）。
2. `REQ-01M6ZDSPVYS3R78TK8TFKVBC8N` —— 房间里「戻る」按钮对**注入点击**无反应（hover 有高亮、点 CG 鑑賞 有效、
   ESC 有效）。⇒ 脚本暂时用 `key 0x1B` 走同一条退出路径；那条单修好后换回 `click center`。

## 实测记录（2026-11-09，macOS arm64 / RelWithDebInfo，`--skip-logo`，`--audio off`）

- `room-enter.graph`：**两次运行各 exit 0**，且两份 trace **逐字节相同**（`diff` 无输出）。
  trace 全文：`task start nodes=2 entry=title` → `node title enter frame=0` → `when screen hash 0x94CB492BF7F1AEEB samples=31 hit`
  → `action click 944 562 applied` → `expect screen changed samples=33 hit` → `node title done` → `node room enter frame=33`
  → `when screen hash 0x96E51755F00B2E9E samples=44 hit` → `action quit applied` → `node room done`
  → `task done nodes=2 transitions=6 actions=2`（insns ≈ 1.30e9）。
  ⇒ "不依赖帧号 / 重复到稳定 / trace 可 diff"三条都在真机上成立。
- `room-exit.graph`：**仍 exit 5**，但停在**另一个 gap**（2026-11-09 复测）：`LockRect(backbuffer)` 已成功 2 次
  （Pitch=5120 / pBits=0x28000008 / 0,0..1280,720）+ `UnlockRect` 2 次，随后 `eip=0x4123C4` 的
  `__CxxThrowException(Command_Exit_Exception)` 撞上未实现的 `kernel32!RaiseException` ⇒ 见 `REQ-01M708DF24KY0QZNEHY66S0HYW`。
  旧读数（停 LockRect）保留在 git 历史里。
  ★ 注意这条的"停"在运行摘要里是 `Stopped`（`ok()` 为真、退出码 0）—— 那是 `REQ-01M6WW9R6SJ7P8XA4522XCR48A`
  记的口径问题；本单在 `main` 里按"任务没到 `task done` 就是失败"把它拦成 exit 5。

## 不做

- ⛔ 不在此单重新设计 profiler / 吞吐（各有单）。
- ⛔ 不把画面识别做成通用 CV 框架：只做本组需要的几种特征；模板匹配不做。
- ⛔ 不用 OCR / 泛化模型：不可复现、不可 diff。
- ⛔ 不把时间线并进节点图（真源 §7 第 3 期）。
