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
 * `THREAD_LOCAL` 定为空：本线的 guest 是**单线程**（README §2.5），而且 `softfloat_roundingMode`
 * 在每次 x87 操作前都会从 guest 控制字重新设一次（见 `src/runtime/fpu_bridge.cpp`），
 * 不存在跨线程共享状态的问题。真要并行跑两台 Machine 时改回 `_Thread_local` 即可。
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

#ifndef THREAD_LOCAL
#define THREAD_LOCAL
#endif
