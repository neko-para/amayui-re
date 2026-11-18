# quickjs-ng v0.10.1（vendor，A 的静态库）
#
# ★ 上游构建形状（它自己的 CMakeLists.txt）**没有** vendored：我们只需要一个静态库，
#   不要 CLI / qjsc / test262 / examples。5 个 C 源与 include 目录 = 上游 `qjs` target 的子集；
#   文件字节与上游 v0.10.1 相同（清单 / commit / 升级口径见 `README.md`）。
# ★ 为什么是 STATIC：语言绑定要静态链进 exe（发行树不多一个 DLL）；`BUILD_SHARED_LIBS` 不参与。
# ★ 为什么 PUBLIC include：消费者（`src/ui/script_quickjs.cpp`）写 `#include "quickjs.h"`。
add_library(lift-qjs STATIC
    cutils.c
    libregexp.c
    libunicode.c
    quickjs.c
    xsum.c)

target_include_directories(lift-qjs PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")

# C11：`quickjs.c` 走 `quickjs-c-atomics.h`（C11 atomics）；C_EXTENSIONS 与上游一致开。
set_target_properties(lift-qjs PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS ON)

# `_GNU_SOURCE`：上游对 `qjs` 无条件追加（`list(APPEND qjs_defines _GNU_SOURCE)`）。
# 它影响 `strdup` / `clock_gettime` 等 POSIX 可见性；mac / linux 都要。
target_compile_definitions(lift-qjs PRIVATE _GNU_SOURCE)

# `-funsigned-char`：上游对非 MSVC 编译器探测后加（`xcheck_add_c_compiler_flag(-funsigned-char)`）。
# quickjs 的字符串 / 字节码按无符号 char 处理；不加会在 `char` 有符号的平台上改变行为。
# 上游是"探测到支持才加"；我们把它写成明确的构建形状（三个 preset 的 clang/gcc 都支持）。
if(NOT MSVC)
    target_compile_options(lift-qjs PRIVATE -funsigned-char)
endif()
