/*
 * Berkeley SoftFloat 3e 的 `platform.h` —— **本仓唯一自己写的文件**（不是上游字节）。
 *
 * 为什么必须自己写：上游把 `platform.h` 当作**按目标平台编辑**的模板放在
 * `build/template-FAST_INT64/platform.h`，文件里带 `==>` 标记（"Edit lines marked with `==>`"），
 * 由它的 Makefile 复制进构建目录后再编译。所以 vendor 树里 `source/8086/` **没有** platform.h，
 * 缺了它就编不过（`fatal error: 'platform.h' file not found`）。
 *
 * 我们的三个 preset 都是 64 位宿主 ⇒ 用 FAST_INT64 那一档；`SOFTFLOAT_FAST_INT64` 由
 * CMakeLists 以 PUBLIC 定义（它参与 `extFloat80_t` 的可见布局，所以必须两侧一致）。
 *
 * `THREAD_LOCAL` 必须是**真线程局部**：本线的 guest **不是**单线程 —— 音频 feeder 是第二条 guest
 * 执行流（`src/runtime/audio.cpp` 起自己的 `X86`，在自己的线程里走同一份 SoftFloat）。而
 * `softfloat_exceptionFlags` / `softfloat_roundingMode` / `softfloat_detectTininess` /
 * `extF80_roundingPrecision` 都是**模块级**变量（`source/softfloat_state.c`），
 * `x87TakeExceptionFlags()` 还是"读 + 清"的读改写（`src/runtime/fpu_bridge.cpp`）
 * ⇒ 进程级共享会让两条流互相偷 / 污染 x87 异常位（PE 等），PCM 段的"逐位一致"就不成立。
 * **每线程一份 softfloat 状态是正确性要求**，不是优化（`REQ-01M76ZHN9V2WNCYT1ACJDJ94CW`；
 * 守卫 = CTest `fpu-thread-flags`，把下面那段改回空 ⇒ 当场红）。
 *
 * 为什么不用 C11 的 `_Thread_local`：SoftFloat 以 **C99** 编译（本目录 CMakeLists 的
 * `C_STANDARD 99`），而 Windows 侧的 C 前端是 MSVC 口径（`cl` / `clang-cl`，没有 C11 的
 * `_Thread_local`）⇒ 用编译器扩展（`__declspec(thread)` / `__thread`），
 * `_Thread_local` 只作未知编译器的保底。
 * ⚠ 静态库（本工程）进 EXE 两种扩展都成立；只有"静态库进**运行时加载的 DLL**"才有
 *   `__declspec(thread)` 的旧限制（VS2015 之前的 loader）—— 本线的落点不是它。
 */
#pragma once

#define LITTLEENDIAN 1

#ifdef __cplusplus
#define INLINE inline
#else
#if defined(_MSC_VER) && !defined(__clang__)
#define INLINE __forceinline
#else
#define INLINE inline
#endif
#endif

// ★ 判据 = **真的线程局部**（理由见文件头）。顺序重要：clang-cl **同时**定义 `_MSC_VER` 与
//   `__clang__`，而 MSVC 的 C 前端不认 GNU 的 `__thread` ⇒ 先按 `_MSC_VER` 分档。
//   `#define THREAD_LOCAL`（空）是变异点：`tools/mutate-check.mjs` 的 `ctest:fpu-thread-flags`
//   把它改回空，要求 CTest 当场红。
#ifndef THREAD_LOCAL
#if defined(_MSC_VER)
#define THREAD_LOCAL __declspec(thread)
#elif defined(__GNUC__) || defined(__clang__)
#define THREAD_LOCAL __thread
#else
#define THREAD_LOCAL _Thread_local
#endif
#endif
