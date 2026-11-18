# quickjs-ng（第三方 JS 引擎）

本目录是 **quickjs-ng v0.10.1** 的**原样副本（未改一行）**，只 vendored 我们编译所需的
5 个 C 源 + 它们 include 的头文件 + 上游 `LICENSE`。⛔ 没有上游的 `CMakeLists.txt` / `Makefile` /
CLI / 测试 / 示例：构建形状由本仓 `CMakeLists.txt` 的 `lift-qjs` target 决定（见下）。

- 上游：<https://github.com/quickjs-ng/quickjs>
- tag：`v0.10.1`
- commit：`3c9afc9943323ee9c7dbd123c0cd991448f4b6c2`（`Set version to 0.10.1`）
- 许可证：MIT（见 `LICENSE`；文件头也带 MIT 声明）
- 版本宏真源：`quickjs.h` 的 `QJS_VERSION_MAJOR/MINOR/PATCH = 0/10/1`；
  `src/ui/script_quickjs.cpp` 用 `static_assert` 钉住这三个值（升级漏改那边 ⇒ 编译期红）。

## 文件清单（18 个，全部原样）

| 文件 | 角色 |
|---|---|
| `quickjs.c` | 解释器 / 字节码 / 值 / 内建对象（主体） |
| `cutils.c` | 容器 / 字符串 / 动态缓冲 |
| `libregexp.c` | 正则引擎 |
| `libunicode.c` | Unicode 表 / 大小写 / 标识符 |
| `xsum.c` | 快速字符串哈希（atom 表用） |
| `quickjs.h` | 公共 C API（唯一被宿主 include 的头） |
| `quickjs-atom.h` | 预定义 atom 名 |
| `quickjs-opcode.h` | 字节码 opcode 表 |
| `quickjs-c-atomics.h` | C11 / MSVC 原子适配（`quickjs.c` 内部用） |
| `cutils.h` | `cutils.c` 的接口 |
| `libregexp.h` | 正则接口 |
| `libregexp-opcode.h` | 正则字节码 |
| `libunicode.h` | Unicode 接口 |
| `libunicode-table.h` | Unicode 表（生成物，上游入库） |
| `list.h` | 侵入式链表 |
| `xsum.h` | 哈希接口 |
| `builtin-array-fromasync.h` | 内建 `Array.fromAsync` 源码字面量（C 数组） |
| `LICENSE` | 上游 MIT 文本 |

## 为什么 vendor 它

UI 脚本层（`src/ui/script.h`）需要一个**嵌入式、无系统依赖、可设内存/栈/CPU 预算**的 JS 引擎。
quickjs-ng 是 MIT、单层 C、可静态链进 exe（发行树不多一个 DLL），且 v0.10.1 提供
`JS_SetMemoryLimit` / `JS_SetMaxStackSize` / `JS_SetInterruptHandler` 三个预算口
（`quickjs.h` 的 `:440` / `:446` / `:1031`）。⛔ 不用 vcpkg：我们要钉死字节
（语言行为 / 沙箱形状随版本变），且本仓已有的第三方惯例就是"原样副本 + 本文件"。

## 怎么用

- 构建：`apps/emulator-lift/CMakeLists.txt` 末尾的 A 块 `add_subdirectory(third_party/quickjs-ng)`；
  本目录的 `CMakeLists.txt` 建静态库 `lift-qjs`（5 个 C 源、C11、`_GNU_SOURCE`、
  非 MSVC 加 `-funsigned-char`；include 目录是 **PUBLIC**，消费者写 `#include "quickjs.h"`）。
- 唯一 include `quickjs.h` 的 TU = `src/ui/script_quickjs.cpp`（`src/ui/script.h` 与
  `src/ui/script_quickjs.h` **都不许** include 它）。
- ⛔ 没有 `quickjs-libc.c` / `qjs` CLI / `std` / `os` 模块：脚本侧只拿宿主注入的 `ui` / `game`。

## 怎么改

- **升级**：把上表 18 个文件整体换成新 tag 的逐字节副本（`git -C <上游 checkout> show <tag>:<file>`），
  同步改本文件的 tag / commit / 版本宏说明，再跑
  `cmake --build apps/emulator-lift/out --target lift-ui-script-quickjs` +
  `ctest --test-dir apps/emulator-lift/out -R '^ui-script-quickjs$' --output-on-failure`。
  新 tag 若改了 `JS_AddIntrinsicEval` / `JS_SetInterruptHandler` / 错误 `stack` 的形状，
  沙箱与预算用例会当场红（那正是升级需要人看的地方）。
- **本地补丁**：⛔ 不要直接改这份副本（那就不是"上游原样"，也说不清来源）。要改行为就改
  `src/ui/script_quickjs.cpp` 的包装层。若某平台确实编不过、必须打补丁：只打最小补丁，
  并在本文件加一节写明**补丁内容 + 理由 + 何时能删**（当前没有补丁）。
- 不要手工重打本文件（换行 / 编码 / 尾随空格会让"逐字节副本"这个前提失效）。
