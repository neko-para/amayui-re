# quickjs-ng（第三方 JS 引擎）

本目录是 **quickjs-ng v0.18.0** 的**原样副本（未改一行）**，只 vendored 我们编译所需的
4 个 C 源 + 它们 include 的头文件 + 上游 `LICENSE`。⛔ 没有上游的 `CMakeLists.txt` / `Makefile` /
CLI / 测试 / 示例：构建形状由本仓 `CMakeLists.txt` 的 `lift-qjs` target 决定（见下）。

- 上游：<https://github.com/quickjs-ng/quickjs>
- tag：`v0.18.0`
- commit：`d7215b0ad3fc2c562e8317fd9bca058fd4863804`（`Set version to 0.18.0`）
- 许可证：MIT（见 `LICENSE`；文件头也带 MIT 声明）
- 版本宏真源：`quickjs.h` 的 `QJS_VERSION_MAJOR/MINOR/PATCH = 0/18/0`；
  `src/ui/quickjs_value.h` 用 `static_assert` 钉住这三个值（升级漏改那边 ⇒ 编译期红）。

## 文件清单（19 个，全部原样）

| 文件 | 角色 |
|---|---|
| `quickjs.c` | 解释器 / 字节码 / 值 / 内建对象（主体） |
| `dtoa.c` | double ↔ 字符串（`js_dtoa` / `js_atof`；0.18.0 拆成独立 TU） |
| `libregexp.c` | 正则引擎 |
| `libunicode.c` | Unicode 表 / 大小写 / 标识符 |
| `quickjs.h` | 公共 C API（唯一被宿主 include 的头） |
| `quickjs-atom.h` | 预定义 atom 名 |
| `quickjs-opcode.h` | 字节码 opcode 表 |
| `quickjs-c-atomics.h` | C11 / MSVC 原子适配（`quickjs.c` 内部用） |
| `cutils.h` | 容器 / 字符串 / 动态缓冲（0.18.0 起**没有** `cutils.c`：实现以 `static inline` 落在本头里） |
| `dtoa.h` | `dtoa.c` 的接口 |
| `libregexp.h` | 正则接口 |
| `libregexp-opcode.h` | 正则字节码 |
| `libunicode.h` | Unicode 接口 |
| `libunicode-table.h` | Unicode 表（生成物，上游入库） |
| `list.h` | 侵入式链表 |
| `builtin-array-fromasync.h` | 内建 `Array.fromAsync` 源码字面量（C 数组） |
| `builtin-iterator-zip.h` | 内建 `Iterator.zip` 源码字面量（C 数组） |
| `builtin-iterator-zip-keyed.h` | 内建 `Iterator.zipKeyed` 源码字面量（C 数组） |
| `LICENSE` | 上游 MIT 文本 |

★ 上游 `qjs_sources`（它的 `CMakeLists.txt` 的 `set(qjs_sources …)`）就是上表前四个 `.c`，
一项不多一项不少；其余 14 个文件是它们 `#include "…"` 的本地头。
★ 上游 v0.18.0 **删掉了** `xsum.c` / `xsum.h`（tag 树里没有这两个文件；`raw.githubusercontent.com/.../v0.18.0/xsum.c`
也是 HTTP 404）⇒ 我们跟着删。`cutils.c` 则是实现以 `static inline` 进 `cutils.h`（所以 `cutils.h` 还在、
`cutils.c` 没有）。"最小集"总是 = 能编出静态库的那几个 TU 及其 `#include` 的头。

## 为什么 vendor 它

UI 脚本层（`src/ui/script.h`）需要一个**嵌入式、无系统依赖、可设内存/栈/CPU 预算**的 JS 引擎。
quickjs-ng 是 MIT、单层 C、可静态链进 exe（发行树不多一个 DLL），且 v0.18.0 提供
`JS_SetMemoryLimit` / `JS_SetMaxStackSize` / `JS_SetInterruptHandler` 三个预算口
（`quickjs.h` 的 `:515` / `:521` / `:1193`）。⛔ 不用 vcpkg：我们要钉死字节
（语言行为 / 沙箱形状随版本变），且本仓已有的第三方惯例就是"原样副本 + 本文件"。

## 怎么用

- 构建：`apps/emulator-lift/CMakeLists.txt` 末尾的 A 块 `add_subdirectory(third_party/quickjs-ng)`；
  本目录的 `CMakeLists.txt` 建静态库 `lift-qjs`（**4 个 C 源**、C11、`_GNU_SOURCE` +
  `QUICKJS_NG_BUILD`、`if(WIN32)` 的 `WIN32_LEAN_AND_MEAN` / `_WIN32_WINNT=0x0601`、
  非 WIN32 的 `m` 与 `CMAKE_DL_LIBS` / `Threads`、`if(NOT MSVC)` 的 `-funsigned-char`；
  include 目录是 **PUBLIC**，消费者写 `#include "quickjs.h"`）。
  ★ `WIN32_LEAN_AND_MEAN` / `_WIN32_WINNT=0x0601` 与上游 `qjs_defines` **全集对齐**，是**防御性选择**，
  **不是当前的承重项**：这两个 define 是 `PRIVATE`（宿主只 include `quickjs.h`，拿不到）；0.18.0 的
  `cutils.h` 自己先 `winsock2.h`（`:60`）后 `windows.h`（`:61`）、`cutils.c` 已不存在
  ⇒ **当前 4 个 TU 去掉这两个 define 也能编**（2026-11-18 实测：`quickjs.c` / `libunicode.c` 单独编 exit 0）。
  保留它们是为了将来「给 `lift-qjs` 加源」或「上游回退 include 顺序」时仍安全：
  任何"先 include `<windows.h>`"的 TU 再进 `cutils.h` 会撞 `winsock.h` vs `winsock2.h`
  （`fd_set` / `sockaddr` / `IPPROTO_RAW` 重定义），而 `WIN32_LEAN_AND_MEAN` 让 `<windows.h>` 不拉
  `winsock.h`（验证探针 `.tmp/qjs-018/probe5.c`：`<windows.h>` 在前 + `cutils.h`、不加 defines ⇒ error，
  加上 ⇒ 编译过）。`_WIN32_WINNT=0x0601` 只是照上游 define 全集对齐，**未找到本库代码级依赖**。
  ★ `-funsigned-char` 用 `if(NOT MSVC)`：clang-cl 走 MSVC 分支 ⇒ 不加；该 flag 在 clang-cl 上
  本来就是 no-op（`unknown argument ignored`）⇒ 与上游 win32 分支的最终效果等价（signed char）。
- 唯一 include `quickjs.h` 的 TU = `src/ui/script_quickjs.cpp`（经私有头 `src/ui/quickjs_value.h`；
  `src/ui/script.h` 与 `src/ui/script_quickjs.h` **都不许** include 它）。
- ⛔ 没有 `quickjs-libc.c` / `qjs` CLI / `std` / `os` 模块：脚本侧只拿宿主注入的 `ui` / `game`。

## 怎么改

- **升级**：把上表 19 个文件整体换成新 tag 的逐字节副本（`git -C <上游 checkout> show <tag>:<file>`，
  或 `https://github.com/quickjs-ng/quickjs/archive/refs/tags/<tag>.tar.gz`），删掉新 tag 已不存在的旧文件，
  同步改本文件的 tag / commit / 版本宏说明；上游 `qjs_sources` / `qjs_defines` / 系统库若变了，
  再同步改本目录 `CMakeLists.txt`（每处与上游不同都要写"为什么此时不适用"）。然后跑
  `cmake --build apps/emulator-lift/out --target lift-qjs` +
  `cmake --build apps/emulator-lift/out --target lift-ui-script-quickjs` +
  `ctest --test-dir apps/emulator-lift/out -R '^ui-script-quickjs$' --output-on-failure`。
  新 tag 若改了 `JS_AddIntrinsicEval` / `JS_SetInterruptHandler` / `JSInterruptHandler` 的签名 /
  错误 `stack` 的形状 / `JS_AddIntrinsicBaseObjects` 装哪些内建，沙箱与预算用例会当场红
  （那正是升级需要人看的地方）。
- **本地补丁**：⛔ 不要直接改这份副本（那就不是"上游原样"，也说不清来源）。要改行为就改
  `src/ui/script_quickjs.cpp` 的包装层。若某平台确实编不过、必须打补丁：只打最小补丁，
  并在本文件加一节写明**补丁内容 + 理由 + 何时能删**（当前没有补丁）。
- 不要手工重打本文件（换行 / 编码 / 尾随空格会让"逐字节副本"这个前提失效）。
