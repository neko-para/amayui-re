# A4 的 trace 逐字节判据没有真的比对：boot-trace 只哈希寄存器 / 不跑 --trace-shims

- id: REQ-01M6JFZ7CCH5DVX510W92VVRHK
- type: bug
- status: done
- parent: REQ-01M6H6FW5JGQGHACXHPWZD4Y3E
- verify: apps/emulator-lift/tests/boot_trace.cpp#trace-bytes-identical
- repro: apps/emulator-lift/tests/boot_trace.cpp#fnv(h, cpu.r, sizeof(cpu.r))
- severity: S2
- tags: [emulator-lift, m1, a4]

## 一句话
A4 的"同一条命令跑两次 trace 逐字节相同"**没有被任何测试真正比对**：`boot_trace` 只哈希寄存器 / eip / eflags / x87 元信息 / 堆计数，不哈希 guest 内存、不跑 `--trace-shims`；`traceInputsHash` 也不含 `--game-dir`。

## 落地（2026-11-04）
- `tests/boot_trace.cpp` 新增 `trace-bytes-identical`：同一次初始化跑两遍，`--trace-shims` 各写一个临时文件，
  **逐字节比较**（并断言非空，不允许"两边都空"假绿）；任何字节差都打印第一个不同位置。
- `RunOptions` 新增 `trace_out`（缺省 stdout）；`Machine::run` 用它把 shim trace 写进指定文件。
- `TraceInputs` / `traceInputsHash` / `dumpTraceInputs` 补 **`--game-dir`**（`Vfs::gameDir()`）。
- `Vfs::setGameDir` 清掉上一轮的 `.lift-tmp` ⇒ `GetTempFileNameA(uUnique=0)` 的序号不跨运行累积
  （否则第二次运行会拿到 AGE0001 而不是 AGE0000）。
- 守卫：`file-path-ext#temp-dir-cleared`（预置 `.lift-tmp/AGE0000.TMP`，`setGameDir` 后必须消失）。

## 判据 / 变异
- `boot-trace#trace-bytes-identical`：两份 trace 字节相等且非空；`trace: run1=108914 bytes run2=108914 bytes`。
- 变异：`Machine::run` 忽略 `trace_out`（两份文件都空）⇒ `boot-trace` 红（CTest 退出码 8）。
- 变异：`Vfs::setGameDir` 不清理 `.lift-tmp` ⇒ `file-path-ext#temp-dir-cleared` 红（CTest 退出码 8）。

## 门禁（本机 macOS arm64）
- `ctest` 21/21（含 `conformance-safe`）；`boot-trace` 14 checks / 0 failures。
- `boot-trace` 主判据仍与改前一致：`insns=60652 hash=D06091AF5DD19647 inputs=35CB9281F6D0881B`。
- `pnpm test` 1159 pass / 0 fail；`requirements validate` 6/6；`lift check` 绿。

## 仍保留
- 没有在 Windows 上跑（`trace_out` 的 `_wfopen_s` 路径未实跑）；`.lift-tmp` 清理只发生在 `setGameDir`，
  若调用方在同一 `Machine` 里换 game-dir 会再清一次（有意）。
