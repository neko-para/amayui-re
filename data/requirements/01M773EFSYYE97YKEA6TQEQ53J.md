# emulator-lift · softfloat.h 的空 THREAD_LOCAL 兜底是静默危险：漏 include platform.h 会编译链接都过、运行时读 248

- id: REQ-01M773EFSYYE97YKEA6TQEQ53J
- type: req
- status: open
- parent: REQ-01M6H6G6DSRG1ZRD810Z0HTH8V
- tags: [emulator-lift, softfloat, build]

## 一句话
`softfloat.h:53-54`（与 `softfloat_state.c:43-44`）有 `#ifndef THREAD_LOCAL → #define THREAD_LOCAL`（空）的兜底：
任何 TU 若**先** include `softfloat.h`、没先 include `platform.h`，编译 + 链接都过，运行时却把 TLS 描述符当变量读
（复审实测 `flags=248` / `rounding=248`）⇒ **静默**错误。今天没有这样的 TU，但下一处 include 就会中。

## 已核事实（2026-11-12，`.tmp/audio-token/softfloat-change-review.md`）
- `THREAD_LOCAL` 已在 `third_party/berkeley-softfloat/platform.h:46-54` 定义为真 TLS（三档）。
- 全部 286 个 vendored `.c` + 两个 C++ TU（`fpu_bridge.cpp`、`tests/fpu_thread.cpp`）都先 include `platform.h`。
- 漏 include 时：`extern uint_fast8_t softfloat_exceptionFlags;` 被当成非 TLS 符号；在 macOS 上实测链接成功、运行时读 248。
- 本单**不**改 vendored 头（`source/include/softfloat.h` / `softfloat_state.c`）的字节 —— 这是仓库纪律。

## 要做（二选一，父级倾向 A）
**A（不动 vendored 字节，推荐）**：`third_party/berkeley-softfloat/CMakeLists.txt` 给消费者强制前置 include：
```cmake
if(MSVC)
  target_compile_options(berkeley-softfloat INTERFACE /FIplatform.h)
else()
  target_compile_options(berkeley-softfloat INTERFACE -include platform.h)
endif()
```
（include dir 已 PUBLIC。效果 = 所有链接 `berkeley-softfloat` 的 TU 自动先看见 `platform.h`。）
**B**：在 `softfloat.h` 的空兜底处改成 `#error "include platform.h before softfloat.h"`（要动 vendored 字节，需显式裁决）。

## 判据
- 一个"先 include `softfloat.h`、不 include `platform.h`"的临时 TU 经消费者编译链后**不再**能读成非 TLS（A：预处理器/`nm` 证明它拿到 `platform.h`；B：编译期 `#error`）。
- 现有 `fpu-thread-flags` / `math` / `conformance` / `dispatch` 全绿；`pnpm test` / `pnpm verify` / `ctest` 不变。
- ⛔ 不改 SoftFloat 算法；⛔ 不把 `THREAD_LOCAL` 退回空。

## 未做 / 边界
- A 的 `-include` 是编译器选项，Windows 侧（clang-cl）用 `/FIplatform.h` 需实测一次；本机 macOS 只能验非 MSVC 臂。
- 不加"编译失败"型自动守卫（要 spawn 编译器；本单的护栏本身就是编译期选项）。
