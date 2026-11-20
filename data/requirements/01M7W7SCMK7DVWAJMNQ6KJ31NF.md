# P2 · 其余 7 处 spawn 迁到 runChild

- id: REQ-01M7W7SCMK7DVWAJMNQ6KJ31NF
- type: req
- status: open
- parent: REQ-01M7W7RSC34YJTNMK3ESYVXZY2
- tags: [emulator-lift, tests, subprocess]

## 做什么

把剩余 **7 处**手写 spawn 迁到 `tests/support/subprocess::runChild`：

- `aot_overlay`（原 donor，改成调 support）；
- `gpu_batch`（顺带消灭固定临时文件名）；
- `fault_report`（保留 POSIX `fork+fn` 后端，只统一 `ChildRun` 字段 / CRLF）；
- `aot_quit_exit`；
- `aot_token_summary`；
- `lift_gen_ascii_output`；
- `lift_ir_proto`。

一次只迁一个文件；一 commit 一 revert（`git revert` 该 commit）。

## 判绿

- `rg 'CreateProcessW|std::system\(' apps/emulator-lift/tests` 只在 `tests/support/` 命中。
- `rg 'lift-guard-child' apps/emulator-lift/tests` 为 0。
- `ctest -R 'aot-overlay|gpu-batch-order|fault-report|aot-quit|aot-token-summary|gen-ascii-output|lift-ir-proto'`
  与迁移前**同结论**（贴 N/N 与 skip 名单）。

## 边界

- ⛔ 不改任何断言 / 阈值；⛔ 不动 `fault_report` 的 POSIX 后端语义（它测的正是 `fork+fn`）。
- ⛔ 不顺手迁 P3/P4 的内容。
