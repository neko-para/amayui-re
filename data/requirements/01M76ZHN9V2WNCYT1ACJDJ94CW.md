# emulator-lift · softfloat 的 THREAD_LOCAL 为空 ⇒ feeder 与主线程的 PE 互相污染（PCM 段逐位一致的前置）

- id: REQ-01M76ZHN9V2WNCYT1ACJDJ94CW
- type: req
- status: done
- parent: REQ-01M6H6G6DSRG1ZRD810Z0HTH8V
- verify: apps/emulator-lift/tests/fpu_thread.cpp#fpu-thread-flags
- tags: [emulator-lift, audio, softfloat, concurrency]

## 一句话
`third_party/berkeley-softfloat/platform.h` 把 `THREAD_LOCAL` 定义成空串（注释还写着"guest 单线程"），而
`softfloat_exceptionFlags` / `x87TakeExceptionFlags()` 是**进程全局 + 读改写** ⇒ 音频 feeder（第二条 guest 执行流）
与主线程会互相偷/污染 x87 状态字（PE 等）。这是 PCM 段在 feeder 里"逐位一致"的**前置条件**。

## 已核事实（2026-11-12，`.tmp/audio-token/feeder-seg-analysis.md` §6）
- `third_party/berkeley-softfloat/platform.h:30-31`：`#define THREAD_LOCAL`（空）+ 注释"guest 单线程"——已过期：
  feeder 是第二条 guest 执行流（`audio.cpp` 自己的 `X86` + 私有 softfloat 调用）。
- `softfloat_exceptionFlags` 是模块级全局（SoftFloat 3e 的 `s_roundingMode` / `s_exceptionFlags` 同族）；
  `x87TakeExceptionFlags()` 是"读 + 清"的读改写 ⇒ 两个线程同时执行时会丢标志或串标志。
- 只在 `--audio sdl`（真 feeder）下可达；CTest 默认 Virtual/pump 档**不会**红 ⇒ 静默。
- 相关：`REQ-01M75W6BB3PY3MF42ZTS23KTBS`（PCM 段 0x48D767 落地）把"逐位一致"当判据，必须先修这条。

## 要做
1. `platform.h`：`THREAD_LOCAL` 改成真 `thread_local`（C++11；MSVC / clang / gcc 都支持），并更新那段过期注释
   （"guest 可以有第二条执行流；softfloat 状态必须每线程一份"）。⛔ 不动 SoftFloat 的算法。
2. 逐处确认没有"跨线程共享 softfloat 状态"的依赖：`rg 'softfloat_exceptionFlags|x87TakeExceptionFlags|s_exceptionFlags'`
   给调用点清单；若某处靠"全局共享"传状态，改成显式参数或每线程状态。
3. 守卫（pure 档，新 `tests/fpu_thread.cpp` 或并入 `tests/math.cpp`）：两个 `std::thread` 各自
   `x87FromDouble` + 触发不同 PE（如 inexact / invalid），各取各的 `x87TakeExceptionFlags()`：
   断言 **A 的 PE 只出现在 A**、B 的只出现在 B（N 次循环，容忍调度）；变异：把 `THREAD_LOCAL` 改回空 ⇒ 必须红。
4. 门禁：`ctest -R fpu` / 全量 ctest；`pnpm test`；`--audio sdl` 一次真跑（`LIFT_AUDIO_TRACE=1`）无异常。

## 判据
- `platform.h` 的 `THREAD_LOCAL` 是 `thread_local`；
- 新用例能红（改回空后红）且在默认 pure 档跑；
- 主线程 / feeder 的 PE 不再互相出现（用例逐轮断言，不锁具体数值）。

## 未做 / 边界
- 不重写 SoftFloat；不改 `x87TakeExceptionFlags` 的签名（只改存储期）。
- 若实测发现"主线程从不读 PE / feeder 从不写 PE"（即今天没有可观察后果），仍要修：这是**前置条件**，
  不能靠"今天恰好不撞"。

## 收口（2026-11-12）
- `platform.h`：`THREAD_LOCAL` 三档（`_MSC_VER`→`__declspec(thread)`；`__GNUC__||__clang__`→`__thread`；否则 `_Thread_local`）；
  文件头改成"feeder 是第二条 guest 执行流 ⇒ 每线程一份状态是正确性要求"。TLS 生效有 `nm -m` 证据（四变量在 `__thread_vars`）。
- 新 pure 用例 `fpu-thread-flags`（`tests/fpu_thread.cpp`）：两线程各 20000 轮 + **`std::barrier` 强制交错**，
  断言只含自己的位；TLS 态 3/3 绿；变异（`THREAD_LOCAL`→空）**同一二进制连跑 10 次 10 红**。
  `tools/mutate-check.mjs --match fpu-thread-flags` ⇒ 红（退出码 8），还原后 sha256 `ea31f241…` 一致。
- 单线程等价：TLS 版与"改前语义"版的 `lift-math` / `lift-native-override` 输出逐字节相同（复审实测）。
- 读数：`ctest -E overlay-window-smoke` **84/84**（注册 85、5 个 by-design SKIP）；`pnpm test` 546；`pnpm verify` 460；
  `--audio sdl` 40 帧 exit 0（`device=ok` / `underruns=0` / `violations=0`）。
- 文档：`third_party/berkeley-softfloat/README.md` 的"只有 fpu_bridge.cpp include softfloat.h"与 `exec_fpu.cpp` 笔误已订正；
  允许点 = `fpu_bridge.cpp` + `tests/fpu_thread.cpp`，都必须先 include `platform.h`。
- 未做：Windows preset 未跑（`__declspec(thread)` 未实测）；`softfloat.h` 的空兜底仍是静默危险 ⇒ 见后续单。
