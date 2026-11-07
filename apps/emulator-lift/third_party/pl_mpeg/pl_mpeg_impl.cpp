// pl_mpeg 的**唯一**实现实例（单文件库的惯例：一个 TU 定义实现宏，其余 TU 只看见声明）。
//
// ★ 为什么单独一个 TU：把 `PL_MPEG_IMPLEMENTATION` 圈在这里，`movie_decoder.cpp` 才能
//   `#include "pl_mpeg.h"` 而**不**把整个解码器实现再编一遍（否则重复符号）。
// ★ 为什么要在 include 之前给 `<cstdio>`：上游 pl_mpeg.h 的**公共声明段**就用了 `FILE`
//   （`plm_create_with_file` / `plm_buffer_create_with_file`），但 `<stdio.h>` 是到实现段
//   才 include 的 ⇒ 不先给就会 `unknown type name 'FILE'`（emulator-cxx 的 `movie.cpp`
//   也是同样先 include `<cstdio>`）。⛔ 不改 vendored 副本（三处逐字节守卫）。
// ★ 本文件除「标准头 + 实现宏 + include」外**不加任何东西**（升级时整份照抄）。
#include <cstdio>

#define PL_MPEG_IMPLEMENTATION
#include "pl_mpeg.h"
