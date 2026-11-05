# emulator-lift · Windows 编码：控制台输出乱码 + 非 ASCII argv 崩溃

- id: REQ-01M6P46K3Z49ED2N0KPN1JVBJG
- type: bug
- status: done
- parent: REQ-01M6H6G6DSRG1ZRD810Z0HTH8V
- verify: apps/emulator-lift/tests/host_encoding.cpp#host-encoding
- repro: apps/emulator-lift/tests/host_encoding.cpp#host-encoding
- severity: S1
- done_reason: 已修并加回归守卫 host-encoding：UTF-8 argv（GetCommandLineW）/ 控制台码页 65001（重定向不动）/ 目录级 /utf-8；实测日文 --game-dir 从 exit 3 变为 exit 0，ctest 43/43 绿。
- tags: [emulator-lift windows encoding]

## 现象

- 在中文 Windows 控制台（`chcp` = 936）运行 `emulator-lift`：所有中文 / 日文日志显示为乱码。
  实测 exe 内的窄字面量是 **UTF-8** 字节（`--help` 里 `emulator-lift —— …` 的字节是
  `E2 80 94`），而控制台按 **936** 解释 ⇒ 乱码。根因不是"源文件被读坏"（clang-cl 默认 UTF-8），
  而是 **输出字节集（UTF-8）≠ 控制台代码页（936）**。
- 同源的第二个问题：`--game-dir` 带日文（例如 `E:\Games\Eushully\天結いキャッスルマイスター`）
  时进程 **exit 3 / abort**，只打出 font 一行。根因：Windows `main(char**)` 的 argv 由 CRT 按
  **ANSI 936** 转出，`vfs` 再按 UTF-8 解析 ⇒ `std::filesystem` 抛异常 ⇒ `std::terminate`。

## 根因

1. `main.cpp` 用窄 `printf` 输出 UTF-8 字面量；`SetConsoleOutputCP` 从未调用 ⇒ 控制台按 936 解码。
2. `argv` 直接使用 CRT 的窄 argv（ANSI 码页）⇒ 非 ASCII 路径的字节不是 UTF-8。
3. CMake 的 `/utf-8` 只加在 `emulator-lift-runtime` 上；`emulator-lift` 可执行文件与测试在 MSVC 下
   没有它 ⇒ 换 MSVC 构建时窄字面量会按本地码页生成（双重乱码）。

## 修法

- `src/runtime/host_encoding.{h,cpp}`：
  - `argvUtf8()`：Windows 从 `GetCommandLineW` 经 `CommandLineToArgvW` 重建 UTF-8；POSIX 直通。
  - `initConsoleUtf8()`：仅当进程挂着控制台（`GetConsoleOutputCP() != 0`）时把控制台**输出**码页
    切到 65001；正常退出 / 已知 fatal 路径恢复。重定向时输出字节不转码（码页仍会切，退出恢复）。
- `main.cpp`：第一行调用 `initConsoleUtf8()`；参数解析改用 `argvUtf8()`。
- `CMakeLists.txt`：MSVC 分支加**目录级** `/utf-8`（覆盖 exe 与全部测试）。

## 验证

- `apps/emulator-lift/out/lift-host-encoding.exe <exe>`：
  - `--help` 字节含 `emulator-lift —— 在 guest-state`（UTF-8）；
  - 日文 `--game-dir` + `--max-insns 1` ⇒ exit 0，输出含 UTF-8 路径；
  - `AllocConsole` 后 `initConsoleUtf8()` ⇒ `GetConsoleOutputCP() == 65001`。
- 人工控制台：`chcp 936` 下运行 `emulator-lift --help`，中文不再乱码（程序退出后码页恢复）。
- 回归：`ctest --test-dir apps/emulator-lift/out` 43/43 绿（2026-11-05）。

## 边界

- 只保证本仓自己的 stdout / stderr；不改 guest 的 CP932 语义。
- 重定向时输出字节保持 UTF-8（⛔ 不按控制台码页转码）。
