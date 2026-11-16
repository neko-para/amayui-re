# 🐞S2 AOT 臂丢掉 guest 退出码：ExitProcess(3) ⇒ 解释器 exit 2、AOT exit 0（src/aot 不引用 guestExitCode）

- id: REQ-01M7HP2YRZG222E0NE2H6MW6NK
- type: bug
- status: done
- parent: REQ-01M6H6G6DSRG1ZRD810Z0HTH8V
- verify: apps/emulator-lift/tests/guest_exit.cpp#lift-guest-exit
- repro: apps/emulator-lift/src/runtime/guest_exit.h#exitCodeForGuestExit
- severity: S2
- tags: [emulator-lift, aot, exit-code]

## 一句话

**两臂对同一个 guest 退出码给出不同结果**：guest `ExitProcess(3)` ⇒ 解释器 **exit 2**、AOT 臂 **exit 0**。

## 证据（读代码确认，未实跑）

| 臂 | 落点 | 判据 |
|---|---|---|
| 解释器 | `src/main.cpp:1392` | 用 `exitCodeForGuestExit`（guest 0 ⇒ 0 / 非 0 ⇒ 2） |
| AOT | `src/aot/aot_main.cpp:628` | `r.ok() ? 0 : 2` |

★ `rg -rn guestExitCode apps/emulator-lift/src/aot/` = **0 命中** ⇒ AOT 臂**根本没有这个映射**，guest 的退出码被丢掉。

## 影响

- 现有 CTest `exit-code` 的判据 ② 就是这条 ⇒ 该用例**在 AOT 臂必然红**（这也说明它是真差异，不是测试问题）。
- 任何"用退出码判成败"的自动化（含将来的控制层 CLI / 配方判据）在 AOT 臂上会**把失败读成成功**。

## 判据（收口时）

- 同一个 guest `ExitProcess(3)`：两臂退出码**相同**（先定成什么：2 还是 3，见下）。
- 一条能红的用例（今天 `exit-code` 已经覆盖解释器侧，AOT 侧要补）。

## 待裁决

退出码语义二选一：**① 保持"非 0 ⇒ 2"的既有口径**（两臂都 2），还是 **② 原样透传 guest 的码**（3）。
倾向 ①（既有口径、且 `exit-code` 的判据已经按它写）；② 会把"emulator 自己的失败退出码"与"guest 的码"混在一起。
