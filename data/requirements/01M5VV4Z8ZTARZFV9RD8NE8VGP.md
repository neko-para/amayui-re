# 🐞TITLE 退出后进程不退出：0x23d/0x32b 缺 handler + 窗口宿主不关窗

- id: REQ-01M5VV4Z8ZTARZFV9RD8NE8VGP
- type: bug
- status: done
- parent: REQ-01M5BK8QMQYK7XQG707YVMQ202
- verify: apps/emulator-cxx/tests/core_selftest.cpp#check_exit_cleanup
- repro: apps/emulator-cxx/tests/core_selftest.cpp#check_exit_cleanup
- severity: S2
- tags: [exit, emulator-cxx]

## 现象

TITLE 的退出路径（`label_0000388c`：`call label_0000384c` → `poll-input` → `i19c` → `exit`）在 emulator-cxx 里走不完：`label_0000384c` 先执行 `0x1F6`、`release-texture 4/5`、`0x23D`、`0x32B`，其中 `0x23D` / `0x32B` 没有 handler ⇒ `step()` 以 `opcode-without-handler` 结束；窗口宿主又把 `machine.halted()` 当成“保留最后一帧等用户关窗” ⇒ 进程不退。

## 判据 / 复现

- `0x23D` / `0x32B` 必须有 handler：前者清槽 42..999 的影片对象，后者清网格槽表 0..999；脚本随后应执行到根帧 `exit`（`0x2`），stop reason = `Halted`。
- 窗口宿主在 `machine.halted()` 时必须关窗返回，不再等待用户关窗。
- 守卫：`apps/emulator-cxx/tests/core_selftest.cpp` 的 `check_exit_cleanup`（`0x1F6 → 0x23D → 0x32B → 0x2`，断言 `Halted`）；缺任一 handler 会以 `UnsupportedOpcode` 红。

## 证据锚

- `0x23D` handler `sub_41A300` @ `0x41A300`；`0x32B` handler `sub_41A4A0` @ `0x41A4A0`。
- TITLE 退出序列见本地 `TITLE.lst` 尾部（非锚）。

## 残留

`0x23D` 的场景侧槽→纹理解绑、`0x32B` 的 D3DX 对象内部释放未建模；spec 规则仍未成篇（另开单）。
