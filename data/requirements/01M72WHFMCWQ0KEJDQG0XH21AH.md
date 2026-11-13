# 🐞S3 [input-script] finished 在 quit 路径也打，且写「全部应用」⇒ 读数与事实相反

- id: REQ-01M72WHFMCWQ0KEJDQG0XH21AH
- type: bug
- status: done
- parent: REQ-01M6XVYA68DQWWMQPY795WPN0Q
- verify: apps/emulator-lift/tests/input_unfinished.cpp#unfinished-quit-negative-control
- repro: apps/emulator-lift/src/runtime/input_script.cpp#finish_pending
- tags: [emulator-lift, input-script, diagnostics]

## 一句话
`[input-script] finished：<N> 条动作全部应用（…）` 这条日志的条件是 `finished() || quit_requested_`，但文案写的是**总动作数**、说的是"全部应用" —— 脚本用 `quit` **提前**结束时恰恰有未应用动作，于是日志与事实相反。

## 复现（2026-11-10，macOS arm64 / RelWithDebInfo，`apps/emulator-lift`）
脚本 `at insn 10 / quit / at insn 20 / wait 1 insn`（任意档位，headless 即可）⇒ stdout：
`[input-script] finished：2 条动作全部应用（脚本按下的键 / 按钮已抬起；键 / 按钮 / 光标所有权交还 OS）`
而同一次运行的摘要（BUG-20 的 `input-script: unfinished` 行）写 `pending=1`（另一条动作从未应用）⇒ **两句互相矛盾**，且没有任何一行说"实际只应用了 1 条"。

## 为什么是缺陷
它是**读数**问题而不是措辞问题：`finished` 是"脚本跑完"的唯一正面证据（README §4.2c 与多处守卫、文档都引用它）。今天它与 `quit` 路径的正确行为**冲突**，读者只能靠 `pending=` 那行反推。
判据（修好后必须成立）：
1. `quit` 路径的日志**不得**声称"全部应用"（例如 `quit：已应用 <applied>/<total> 条，其余跳过`）。
2. `finished()` 的**语义不变**（`applied_ == size` 且 `releases_` 空），既有判据（`--expect-change-*` / `finished` 的消费者）不红。
3. 守卫：`quit` 提前结束的脚本 ⇒ 日志里不得出现"全部应用"；全部应用的脚本 ⇒ 仍打"全部应用"。

## 未做
- 未改文案（与 BUG-20 相邻但独立：那条只负责"把没跑的量报出来"）。
