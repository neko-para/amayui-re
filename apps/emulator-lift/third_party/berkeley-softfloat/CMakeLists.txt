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
