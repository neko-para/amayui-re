# 🐞S2 win32 构建断：d3d9_gpu.cpp 的 std::max 被 <windows.h> 的 max 宏吃掉（HEAD 0077863 起）

- id: REQ-01M7QPHN0GWR5VE7STHBT28VMM
- type: bug
- status: done
- parent: REQ-01M6JFZFN15MBPG7Y89WNZACFW
- repro: apps/emulator-lift/src/runtime/d3d9_gpu.cpp#NOMINMAX
- severity: S2
- done_reason: 按本仓惯例在该 TU 首 include 前加 NOMINMAX guard（20 处同款）后修好：该 TU 2 errors → exit 0，全量 cmake --build out exit 0。属 HEAD 0077863（overlay 纹理 grow-only，提交说明自记 win32 未验证）的既有断裂，与本轮 quickjs 升级无关。
- tags: [emulator-lift, render, win32, build]

## 现象

win32 全量构建在 `src/runtime/d3d9_gpu.cpp:1941-1942` 断：

```
error: expected unqualified-id
   const int cap_w = std::max(w, 1024);
note: expanded from macro 'max'      (Windows Kits minwindef.h:193)
```

## 机制

该 TU 经 `runtime/present_sdl.h` / `<SDL3/SDL.h>` / `<d3dcompiler.h>` 间接拉进 `<windows.h>`，
`minwindef.h` 的 `min` / `max` 宏会把 `std::max` 的函数名撑坏。引入者是已提交的 `0077863`
（overlay 纹理 grow-only；提交说明自记"win32 未验证"）。mac 上不 include `<windows.h>` ⇒ 一直是绿的。

## 落地

按本仓既有惯例（`src/runtime/present_sdl.cpp:6` / `src/runtime/memory.cpp:19` 等 20 处同款）在
**第一条 include 之前**加 `#if defined(_WIN32) / #ifndef NOMINMAX / #define NOMINMAX`；
`std::max` 两行保持原样；同时更新 `:2040` 那条已变错的"⛔ 不要写 `std::min`"注释
（顶部有 `NOMINMAX` 后 `std::min` / `std::max` 可用；不许退回 `min(...)` / `max(...)` 宏形式）。

## 读数（2026-11-18 / win32 / clang-cl 22.1.2）

- 修前：该 TU **2 errors**，全量构建停在这里。
- 修后：该 TU exit 0；全量 `cmake --build out` **exit 0**。
- 复现：`cd apps/emulator-lift && cmake --build out --target emulator-lift-aot`（或任一带 `d3d9_gpu.cpp` 的 target）。
