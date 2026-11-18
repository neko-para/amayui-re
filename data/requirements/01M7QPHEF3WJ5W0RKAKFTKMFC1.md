# 🐞S2 win32 构建断：vendored quickjs-ng 0.10.1 的 windows.h / winsock2.h 冲突（已升级到 0.18.0）

- id: REQ-01M7QPHEF3WJ5W0RKAKFTKMFC1
- type: bug
- status: done
- parent: REQ-01M7PE0D9MJA4REXQ1YJFVPHGF
- verify: apps/emulator-lift/tests/ui_script_quickjs.cpp#ui-script-quickjs
- repro: apps/emulator-lift/third_party/quickjs-ng/CMakeLists.txt#WIN32_LEAN_AND_MEAN
- severity: S2
- done_reason: 升级 vendored quickjs-ng 0.10.1 → v0.18.0（tag/commit 见正文）后 win32 编译恢复：修前 `cmake --build out --target lift-qjs` exit 1 / 20 errors，修后 exit 0；全量构建 exit 0。判据 = 该 CTest 所链的 lift-qjs 在 win32 上能编能链。
- tags: [emulator-lift, quickjs, win32, build]

## 现象

win32（clang-cl 22.1.2 / Ninja / RelWithDebInfo）上 `apps/emulator-lift` 全量构建在 `lift-qjs` 断：

```
third_party/quickjs-ng/cutils.c:42 → cutils.h:39 → winsock2.h
error: redefinition of 'fd_set' / 'sockaddr_in' / 'sockaddr'
error: expected identifier            (IPPROTO_RAW / IPPROTO_MAX)
20 errors generated.
```

机制：`cutils.c` 先 `#include <windows.h>`（`:35`）⇒ 顺带拉进 `winsock.h`；随后 `#include "cutils.h"`，而后者在
`_MSC_VER` 下 `#include <winsock2.h>`（`:39`）。两套 winsock 声明撞车。

## 为什么 vendor 本身没问题

- 18 个 vendored 文件与上游 v0.10.1 **逐字节相同**（`git ls-tree` + `sha256`，18/18；父级另抽检 2 个）。
- 上游自己的 `qjs` target 在 `if(WIN32)` 下追加 `WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0601`
  （v0.10.1 `CMakeLists.txt:241-244`）—— **我们 vendor 时只抄了 `_GNU_SOURCE`，漏了这两个 define**。
  ⇒ 是"构建形状漏抄"，不是"上游代码有 bug"，也不是"vendor 时把字节改坏了"。

## 裁决与落地（2026-11-18，用户口径）

- ⛔ 不给 0.10.1 打 patch；用户裁决**直接升级到 quickjs-ng v0.18.0**
  （tag `v0.18.0` / commit `d7215b0ad3fc2c562e8317fd9bca058fd4863804`）。
- 新树 19 个文件逐字节自上游（sha256 19/19、全 LF）：删 `cutils.c` / `xsum.c` / `xsum.h`（**上游也删了**），
  增 `dtoa.c` / `dtoa.h` / `builtin-iterator-zip{,-keyed}.h`；`cutils` 的实现进了 **`cutils.h`**（170 处 `static inline`）。
- 构建形状按上游 `qjs` target 对齐：4 源 / C11 / `_GNU_SOURCE` / `QUICKJS_NG_BUILD` / WIN32 两个 define /
  `m` + `CMAKE_DL_LIBS` + Threads / `if(NOT MSVC)` 的 `-funsigned-char`（clang-cl 上该 flag 是 no-op ⇒ 与上游 win32 分支等价）。
- 包装层只有两处硬断点：`JSInterruptHandler` 首参 `JSRuntime*` → `JSContext*`（上游 PR #1802）、版本 `static_assert` 0/18/0；
  内存 / 栈 / 中断预算的常量与判定一行未动。
- 沙箱口径随之改：**沙箱不是安全边界**（用户 2026-11-18 裁决：JS 无宿主权限，信任脚本）⇒ 0.18.0 的
  `JS_AddIntrinsicBaseObjects` 自带 `BigInt` 不追删；保留的删除只有功能目的（禁动态编译 / 去随机源）。

## 读数（2026-11-18 / win32 / clang-cl 22.1.2 / RelWithDebInfo）

- 修前：`cmake --build out --target lift-qjs` = **exit 1**，20 errors。
- 修后：同命令 **exit 0**（4 TU + `lift-qjs.lib`）；全量 `cmake --build out` **exit 0**。
- `ctest --test-dir out -R '^ui-script-quickjs$'` = **1/1 Passed**。
- 复现：`cd apps/emulator-lift && cmake --build out --target lift-qjs`。
