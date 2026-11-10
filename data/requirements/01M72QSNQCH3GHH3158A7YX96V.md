# 🐞S3 输入脚本的条件永不满足时静默丢弃（run 结束无任何告警）

- id: REQ-01M72QSNQCH3GHH3158A7YX96V
- type: bug
- status: done
- parent: REQ-01M6XVYA68DQWWMQPY795WPN0Q
- verify: apps/emulator-lift/tests/input_unfinished.cpp#unfinished-never-due
- repro: apps/emulator-lift/src/runtime/input_script.cpp#finished
- tags: [emulator-lift, input-script, diagnostics]

## 一句话
输入脚本里 `at frame N` 的条件**永不满足**时（例如 N 大于本次 run 的末帧），该动作在 run 结束时被**静默丢弃**：stdout 没有 `[input-script] finished`、stderr **也没有任何告警** ⇒ 使用者以为脚本跑完了。

## 复现（2026-11-10，macOS arm64 / RelWithDebInfo）
脚本 `at frame 130 / click 1190 630 / at frame 151 / profile_begin tail`，命令
`./out/emulator-lift --game-dir ../../links/install --appdata <私有 tmp> --skip-logo --audio off --present headless --raster gpu --input-script <脚本> --time-limit 300`。
本二进制末帧 = **150** ⇒ `at frame 151` 那一行永不触发；run 正常 `ExitProcess`，但：
- stdout **没有** `=== profile window: tail ===`、**没有** `[profile-window] begin "tail"`；
- stderr **没有**任何 `[input-script]` / `[profile-window]` 告警；
- 唯一的痕迹是**缺少** `[input-script] finished` 那一行（`finished()` = `applied_ == actions_.size()`，未完成就什么都不打）。

## 为什么是缺陷
`finished()` 是**存在性**观测（"打了这行 ⇒ 全应用了"），但缺这行同时意味着三件事：①脚本没跑完 ②条件写错 ③run 提前停了 —— 当前**无法区分**。这正是这次把读数算错一帧的原因（第一跑拿到的是"窗口没开、表全 0"的假象，而不是报错）。

## 判据（已交付，逐条成立）
1. run 结束时 `active() && !finished()` ⇒ 摘要里**必须**有一行可解析的 `input-script: unfinished pending=<n> releases=<m> first="<原动作行>"`。
2. 该行在"脚本全部应用"时**不出现**；**无脚本**的 run 里也不出现（连 `inputstat :` 都没有）。
3. `quit` 提前结束时**不出现**（`quit` 之后的动作是脚本**有意**跳过的，报它会让正常脚本天天告警）。
4. `first` = **文件序第一条未应用动作**的原文（⛔ 不是 `actions_[applied_]` —— 动作按**到期/边沿**应用，顺序 ≠ 文件序）。
5. 不替代既有的 `[input-script] finished` 日志；⛔ 不加 CLI 参数、⛔ 不改退出码。

## 修法（已落地）
- `src/runtime/input_script.h/.cpp`：三个只读访问器 `unfinishedActionCount()`（= `size − applied_`，下溢按 0）/ `firstUnfinishedActionText()`（**扫 `applied` 标志**）/ `firstPendingReleaseText()`。
- `src/runtime/machine.cpp` 的 `dumpRunSummary`：`input_script_.active()` 段内、缩进续行 `release :` / `marker :` **之后**按判据 2/3 打那一行；`first` 内的 `"` 换成 `'`（保一行 + 可解析）。

## 读数（真实 run）
- 未触发：`input-script: unfinished pending=1 releases=0 first="profile_begin tail"`，且无 `[input-script] finished`、无 `[profile-window]`（exit 0）。
- 全部应用：无 `unfinished`，有 `[input-script] finished：1 条动作全部应用…`（exit 0）。

## 守卫（`apps/emulator-lift/tests/input_unfinished.cpp`，pure / 顶层 add_test，7 例 35 checks）
`unfinished-never-due` · `unfinished-all-applied` · `unfinished-quit-negative-control` · `unfinished-pending-release` · `unfinished-out-of-order-applied` · `unfinished-quote-escaped` · `unfinished-no-script`。
五处变异实测必红后逐字节还原：去掉 `!quitRequested()`（quit 负控红）· `pending` 少算 1（never-due 红）· 扫描改回 `actions_[applied_]`（乱序例红，`first="mouse_move 20 20"`）· 去掉转义（quote 例红）· 整段挪出 `active()`（no-script 例红）。

## 未做
- ⛔ 没有 `--strict-script`（不把脚本写错变成退出码，按判据 5）。
- `at frame 151` 本身是**脚本写法**问题（末帧号随档位变），本条只保证"写错了要能看见"。
- 相邻但独立：`[input-script] finished` 在 `quit` 路径也打且写总动作数 ⇒ `REQ-01M72WHFMCWQ0KEJDQG0XH21AH`。
