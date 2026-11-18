# quickjs-ng v0.18.0（vendor，A 的静态库）
#
# ★ 上游构建形状（它自己的 CMakeLists.txt）**没有** vendored：我们只需要一个静态库，
#   不要 CLI / qjsc / test262 / examples。4 个 C 源与 include 目录 = 上游 `qjs` target 的子集；
#   文件字节与上游 v0.18.0 相同（清单 / commit / 升级口径见 `README.md`）。
# ★ 为什么是 STATIC：语言绑定要静态链进 exe（发行树不多一个 DLL）；`BUILD_SHARED_LIBS` 不参与。
# ★ 为什么 PUBLIC include：消费者（`src/ui/quickjs_value.h`）写 `#include "quickjs.h"`。
#
# ★ 下面每一段都标了上游 `CMakeLists.txt`（v0.18.0 tag）的**行号**；与上游不同的地方
#   都写了"为什么此时不适用"。
add_library(lift-qjs STATIC
    # 上游 `set(qjs_sources …)`（CMakeLists.txt:273-278）的完整列表，一项不少。
    dtoa.c
    libregexp.c
    libunicode.c
    quickjs.c)

target_include_directories(lift-qjs PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")

# C11 + 扩展开：上游 `set(CMAKE_C_STANDARD 11)` / `C_STANDARD_REQUIRED ON` /
# `C_EXTENSIONS ON`（CMakeLists.txt:9-11）。`quickjs.c` 走 `quickjs-c-atomics.h`（C11 atomics）。
set_target_properties(lift-qjs PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS ON)

# `-funsigned-char`：上游用 `xcheck_add_c_compiler_flag(-funsigned-char)` **探测后全局加**
# （CMakeLists.txt:108-110，`if(NOT SUNOS)` 内）。本仓的判据是 `if(NOT MSVC)`：clang-cl 走
# MSVC 分支 ⇒ **不加**。这不叫"少加了"：`-funsigned-char` 在 clang-cl 上是 **no-op**
# （`clang-cl: warning: unknown argument ignored ...`，2026-11-18 / clang 22.1.2 实测）
# ⇒ 上游 win32（MSVC / clang-cl）最终也是 signed char，与本仓等价。
# quickjs 的字符串 / 字节码按无符号 char 处理；非 MSVC 的平台（mac / linux 的 clang / gcc）需要这个 flag。
if(NOT MSVC)
    target_compile_options(lift-qjs PRIVATE -funsigned-char)
endif()

# `qjs_defines`：上游 CMakeLists.txt:285-292。
#   · `_GNU_SOURCE`：无条件追加（:285）；影响 `strdup` / `clock_gettime` 等 POSIX 可见性，mac / linux 都要。
#   · `if(WIN32)` 的 `WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0601`（:286-288）：与上游 `qjs_defines`
#     **全集对齐**，是**防御性选择**、⛔ **不是当前的承重项**。理由：
#     ① 这两个 define 是 `PRIVATE`；宿主只 include `quickjs.h`（不 include `cutils.h`）⇒ 拿不到；
#     ② 0.18.0 的 `cutils.h` 自己先 `winsock2.h`（`:60`）后 `windows.h`（`:61`），`cutils.c` 已不存在
#        ⇒ 0.10.1 那个撞法在 0.18.0 里本身已经消失；
#     ③ 2026-11-18 实测：当前 4 个 TU **去掉这两个 define 也能编**（`quickjs.c` / `libunicode.c`
#        单独编 exit 0）。
#     保留它们是为了将来「给 `lift-qjs` 加源」或「上游回退 include 顺序」时仍安全：任何"先 include
#     `<windows.h>`"的 TU 随后再进 `cutils.h` 会撞 `winsock.h` vs `winsock2.h`
#     （`fd_set` / `sockaddr` / `IPPROTO_RAW` 重定义），而 `WIN32_LEAN_AND_MEAN` 让 `<windows.h>` 不拉
#     `winsock.h`（探针 `.tmp/qjs-018/probe5.c`：`<windows.h>` 在前 + `cutils.h`、不加 defines ⇒ error，
#     加上 ⇒ 编译过）。
#     `_WIN32_WINNT=0x0601` 只是照上游 define 全集对齐，**未找到本库代码级依赖**。
#   · `else()` 的 `m`（:289-292）：UNIX-y 平台才有 libm 这个独立库；win32 上它在 CRT 里。
#   · `QUICKJS_NG_BUILD`：上游在 project 级 `add_compile_definitions`（:46）。它只决定
#     `JS_LIBC_EXTERN` 的展开（`quickjs.h:82-95`），而 `quickjs-libc.c` 我们不 vendor
#     ⇒ **对本库无行为影响**；加它是为了与上游 project 级定义的形状对齐。
set(lift_qjs_defines _GNU_SOURCE QUICKJS_NG_BUILD)
set(lift_qjs_libs)
if(WIN32)
    list(APPEND lift_qjs_defines WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0601)
else()
    list(APPEND lift_qjs_libs m)
endif()
# 上游 CMakeLists.txt:299-303：`${CMAKE_DL_LIBS}`（win32 上为空）+ `${CMAKE_THREAD_LIBS_INIT}`。
# ⛔ 不是"我们额外要的库"：`qjs` target 就是 PUBLIC 链这些，消费者（exe）需要同一份。
list(APPEND lift_qjs_libs ${CMAKE_DL_LIBS})
find_package(Threads)
if(NOT CMAKE_SYSTEM_NAME STREQUAL "WASI")
    list(APPEND lift_qjs_libs ${CMAKE_THREAD_LIBS_INIT})
endif()

target_compile_definitions(lift-qjs PRIVATE ${lift_qjs_defines})
target_link_libraries(lift-qjs PUBLIC ${lift_qjs_libs})

# =======================================================================================
# 以下上游段**故意不适用**（逐条给上游行号）：
#
# · CMakeLists.txt:8 `set(CMAKE_C_VISIBILITY_PRESET hidden)`：只对 SHARED 库的符号可见性有意义，
#   我们是 STATIC（构建形状由本仓决定），没有导出面。
# · CMakeLists.txt:13-43 的 `xoption(...)`：CLI / 安装 / sanitizer / GC stress 这些开关都针对
#   上游自己的可执行文件与测试；我们一个都不要（`BUILD_SHARED_LIBS` 也由宿主决定）。
# · CMakeLists.txt:92-107 的告警 flag（`-Wall` / `-Wextra` / `-Wno-*`）与 :120-147 的 MSVC `/wd*`：
#   上游给"它自己的 target"清警告噪声。我们的门禁不是 vendor 代码的告警面，且没有 `-Werror`。
#   （`:112-117` 的 `_CRT_*` 是"clang 在 Windows 但非 MSVC 命令行"的宏定义、不是告警 flag；
#    clang-cl 走 MSVC 分支 ⇒ 那条 `if` 不成立，也不需要。）
# · CMakeLists.txt:149-162 的 `/STACK:8388608`：改的是 `CMAKE_EXE_LINKER_FLAGS`，作用于上游 CLI 的
#   **可执行文件**（它的 JS 栈 = 1 MB，默认 1 MB 线程栈会溢出）。我们是静态库，宿主 exe 的栈
#   是宿主的事（A 的宿主栈与预算见 `src/ui/script_quickjs.cpp`）。
# · CMakeLists.txt:258-263 / 339-343 的 `qjs-libc`（`QJS_BUILD_LIBC` 缺省 OFF 时仍会建
#   `quickjs-libc` 静态库并链进去）：那是 `std` / `os` 模块与 CLI 的依赖。脚本侧只拿宿主注入的
#   `ui` / `game`（见 vendor `README.md`），⛔ 不要 `quickjs-libc.c`。
# · CMakeLists.txt:313-320 的 `BUILDING_QJS_SHARED` / `USING_QJS_SHARED`：只在 SHARED 时定义；
#   我们是 STATIC，`JS_EXTERN` 展开成空（`quickjs.h:62-76`）。
# · CMakeLists.txt:323-337 的 `qjs_module_lib`、345 起的 `qjs_wasm` / `qjsc` / `qjs_exe` /
#   test262 / examples / api-test：全部是上游自己的产物，不在"静态库最小集"里。
