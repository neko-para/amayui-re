# Berkeley SoftFloat 3e（vendor，D3）—— 只编 x86 需要的 8086 特化
#
# ★ 为什么不进 vcpkg：我们依赖的是它的**逐位行为**，包升级换了算术实现会让位精确夹具
#   无声失效 ⇒ 必须钉死字节（vendor + README 里的 sha256）。见 `README.md`。
# ★ 为什么只编 8086 特化：上游的 `source/<spec>/` 是**互斥的**平台特化（8086 / 8086-SSE /
#   ARM-VFPv2 / RISCV）。x87 的 80 位扩展精度对应 8086 特化；其余三份已从 vendor 树里删掉
#   （同一份字节里删目录不改任何被编译文件的内容）。
file(GLOB SOFTFLOAT_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/source/*.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/source/8086/*.c")

# ★ 上游的两套实现是**互斥**的（它的 Makefile 按构建档只编其中一套）：
#   · FAST_INT64 档（64 位宿主，本线用的这一档）：by-value 的 `extF80_*` 是主体，
#     `*M.c` 结尾的内部文件（memory 形式）属于另一档，编进来会立刻报
#     "call to undeclared function 'softfloat_*M'"（那些声明在 `#else` 分支里）。
#   · `extF80M_*.c` / `f128M_to_extF80M.c` 这类**公开的 M 包装**不在过滤范围内
#     （文件名不是以 `M.c` 结尾），它们自己带 `#ifdef SOFTFLOAT_FAST_INT64` 守卫。
list(FILTER SOFTFLOAT_SOURCES EXCLUDE REGEX "M\\.c$")
# 8086 特化里还有几份"memory 形式"的内部实现（名字不是 M.c 结尾）与 bf16 支持：
#   · `*MToCommonNaN.c` —— memory 形式的 NaN 转换，属于另一档；
#   · `*bf16*` —— bfloat16 是较新的格式，8086 特化的 `specialize.h` 没有
#     `softfloat_bf16UIToCommonNaN` / `softfloat_commonNaNToBF16UI` 这些函数（上游按特化裁剪）。
# 两批都**不是**我们需要的路径（本线只用 extF80）；exclude 掉不动任何文件内容。
list(FILTER SOFTFLOAT_SOURCES EXCLUDE REGEX "MToCommonNaN\\.c$")
list(FILTER SOFTFLOAT_SOURCES EXCLUDE REGEX "bf16")

add_library(berkeley-softfloat STATIC ${SOFTFLOAT_SOURCES})
target_include_directories(berkeley-softfloat PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/source/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/source/8086"
    # 上游的 platform.h 是"按目标编辑"的模板（build/template-FAST_INT64/），
    # 我们自己在 vendor 根上提供一份（本仓唯一自己写的文件，见它的文件头）。
    "${CMAKE_CURRENT_SOURCE_DIR}")
# SOFTFLOAT_FAST_INT64：64 位宿主上走 64 位快路径（我们的三个 preset 全是 64 位）；
# PUBLIC 是必须的 —— 它参与 `extFloat80_t` 的可见布局。
target_compile_definitions(berkeley-softfloat PUBLIC SOFTFLOAT_FAST_INT64)
set_target_properties(berkeley-softfloat PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED ON)

# 第三方代码不接管本仓的警告口径（不改它一行，也不为它压警告改源码）
if (WIN32)
    target_compile_options(berkeley-softfloat PRIVATE /w)
endif()

# ★ 给**消费者**强制前置 include `platform.h`（`REQ-01M773EFSYYE97YKEA6TQEQ53J`）。
#   为什么需要：`source/include/softfloat.h:53-54`（与 `source/softfloat_state.c:43-44`）有
#   `#ifndef THREAD_LOCAL ⇒ #define THREAD_LOCAL`（**空**）的兜底 ⇒ 任何 TU 若**先** include
#   `softfloat.h`、没先 include `platform.h`，编译 + 链接都过，运行时却把 **TLS 模板**当变量访问
#   （库写的是每线程 TLS 块）：实测库置的位在消费方读回 `0`，复审在 macOS 上读到过 `248`。
#   **静默**错误 —— 今天没有这样的 TU，但下一处 include 就会中。前置 include 把这条路从结构上关掉。
#   ★ 为什么是 `INTERFACE`：本目录的 `.c` 自己都先 include `platform.h`（那也是 `#pragma once`
#     的同一个文件，重复 include 无害）；要救的是**消费者**，所以只能挂在 INTERFACE 上
#     （改成 `PRIVATE` ⇒ 消费者拿不到 ⇒ CTest `softfloat-tls` 红，见 `tools/mutate-check.mjs`）。
#   ★ 为什么用相对名 `platform.h` 而不是绝对路径：消费者在**别的目录**里编译，绝对路径会把
#     构建目录绑死在这台机器上。`platform.h` 所在的 vendor 根已在上面
#     `target_include_directories(... PUBLIC ...)` 里 ⇒ 两种驱动的 include 路径搜索都能找到它。
#   ★ `/FI` 已在 windows-x64（clang-cl）实测：加了它，消费方 TU 在**任何 include 之前**就看得见
#     `THREAD_LOCAL` / `LITTLEENDIAN`（两个宏只有 `platform.h` 定义）。
if (MSVC)
    # clang-cl 也走这一支（CMake 对 MSVC ABI 的 clang 同样置 MSVC=1）
    target_compile_options(berkeley-softfloat INTERFACE /FIplatform.h)
else()
    target_compile_options(berkeley-softfloat INTERFACE -include platform.h)
endif()
