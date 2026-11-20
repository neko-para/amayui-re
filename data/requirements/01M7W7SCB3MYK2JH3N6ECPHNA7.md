# P1 · 跨平台 runChild + 自检 + 3 处迁移（已落地）

- id: REQ-01M7W7SCB3MYK2JH3N6ECPHNA7
- type: req
- status: done
- parent: REQ-01M7W7RSC34YJTNMK3ESYVXZY2
- verify: apps/emulator-lift/tests/support/probe_child.cpp#support-child-proc
- done_reason: commit 4062f05：runChild（Win32 CreateProcessW / POSIX fork+execv、文件捕获、超时 991、argv UTF-8）+ support-child-proc（37 checks）+ 迁移 plugin_e2e / cli_aot_flags / aot_audio_wiring（删平台 skip，mac 真跑 329 / 13 checks）。
- tags: [emulator-lift, tests, subprocess]

## 已落地（2026-11-20，commit `4062f05`）

- `tests/support/subprocess.{h,cpp}`：唯一 `runChild(exe, argv, {cwd,timeout})`；Win32 `CreateProcessW` +
  `joinCommandLine` + UTF-8；POSIX `fork/chdir/open/dup2/execv/waitpid`；输出走**两个临时文件**（⛔ 管道，§5.1）；
  超时 `SIGKILL` + 991；`started=false` ⇒ −1；`text()` 只 Windows CRLF 归一；RAII 清理。
- `tests/support/probe_child.cpp` + 顶层 CTest `support-child-proc`（37 checks：argv 空格 / `天結` / 尾 `\` /
  内嵌 `"` / 空串、stdout/stderr 分文件、exit 7、SIGTERM 143、200 ms 预算超时 991、exe 缺失、cwd）。
- 迁移 `plugin_e2e` / `cli_aot_flags` / `aot_audio_wiring`：删平台 skip 与本地 spawn；超时 30/60/120 s 沿用。

## 判据

- `support-child-proc` 绿（37 checks）；mac 上 `cli-aot-flags` **329 checks**、`aot-audio-wiring` **13 checks** 真跑。
- `ctest -N` = 133；`pnpm verify` 绿（`cmake-test-entry` 通过）。
