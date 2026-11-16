# =====================================================================================
# AOT 形状守卫 ①：**分片清单自洽** —— ⚠ 2026-11-16 起**不再是 CTest 的判据入口**。
#
# ★ 权威判据已搬进被测二进制：CTest `lift-aot-split-manifest` 现在跑
#   `emulator-lift-gen --verify-split <out/aot> --split <N>`（六项断言在 `src/aot/lift_gen.cpp`
#   的 `verifyAotSplit` 里，与本脚本逐条对等）。搬迁理由：旧注册面是 `COMMAND ${CMAKE_COMMAND} -P …`
#   ⇒ 没有 target、没有 `main(`，`tools/test/cmake-test-entry.test.mjs` 的"注册名 ↔ 源文件"判据判不了它。
# ★ 这份脚本保留为**手工诊断**（`cmake -DLIFT_AOT_DIR=… -DLIFT_AOT_SPLIT=… -P lift_aot_split.cmake`），
#   ⛔ 不要把它重新接回 `add_test`；改判据请改 `verifyAotSplit`，并让两处保持一致（或删掉本文件）。
# =====================================================================================
#
# 设计：`.tmp/aot/irsplit/design.md` §4.1-2。红线是"拆分的每一份都复用**全局**算出来的
# `entry_owner` / `block_flags` / `structure.structured_roots`"——这条红线坏掉时**产物仍然能链、
# 能跑**（跨份静态 `call` 退化成 `ret i32`，语义等价、形状变了），所以只能靠机械不变量抓。
#
# 不变量（⛔ 不断言任何"当前数字"；全部由构建产物自己算）：
#   1. `Σ functions == LIFT_AOT_PIN_FUNCTIONS`（pin 是启动期核对的同一份计数）
#   2. 各份 `define i32 @lift_cc_*` 的符号集合**两两不交**，并集 == `lift_table.inc` 的函数名集合
#      ⇒ 一份重复定义另一份的函数 = 链接期 duplicate symbol（这里提前红）；
#      一份漏了某个分量 = 并集缺项（这里提前红）
#   3. 每份 `.ll` ≤ 20 MB（`design.md` §3.1 的上界：`max(20 MB, 最大分量)`）
#   4. 份数 == `LIFT_AOT_SPLIT`；清单里每份的 sha256 / 字节数与文件实际一致
#      （防"清单说 8 份、树上是 7 份"或清单陈旧）
#
# 用法：`cmake -DLIFT_AOT_DIR=<out/aot> -DLIFT_AOT_SPLIT=<N> -P lift_aot_split.cmake`
# =====================================================================================
if(NOT DEFINED LIFT_AOT_DIR OR NOT DEFINED LIFT_AOT_SPLIT)
    message(FATAL_ERROR "lift_aot_split: 需要 -DLIFT_AOT_DIR 与 -DLIFT_AOT_SPLIT")
endif()
if(LIFT_AOT_SPLIT LESS 1)
    message(FATAL_ERROR "lift_aot_split: LIFT_AOT_SPLIT=${LIFT_AOT_SPLIT}（必须 ≥ 1）")
endif()

set(_pin "${LIFT_AOT_DIR}/lift_pin.h")
set(_table "${LIFT_AOT_DIR}/lift_table.inc")
set(_manifest "${LIFT_AOT_DIR}/lift_aot.split.txt")
if(NOT EXISTS "${_pin}" OR NOT EXISTS "${_table}")
    # 没有生成物（缺 clang / 执行源 / closure.txt）⇒ 本次构建没有 AOT 形状可查。
    # ★ 不 `FAIL`：AOT 侧的"没有生成物"由 `emulator-lift-aot` 启动时响亮报告并 exit 1 覆盖。
    message(STATUS "lift_aot_split: skip（没有 lift_pin.h / lift_table.inc：本次构建没有 AOT 生成物）")
    return()
endif()

# ---- pin 里的函数数（启动期核对用的同一份宏）----
file(STRINGS "${_pin}" _pin_fn REGEX "^#define LIFT_AOT_PIN_FUNCTIONS ")
if(NOT _pin_fn)
    message(FATAL_ERROR "lift_aot_split: lift_pin.h 里没有 LIFT_AOT_PIN_FUNCTIONS")
endif()
string(REGEX REPLACE "^#define LIFT_AOT_PIN_FUNCTIONS +([0-9]+)u.*$" "\\1" PIN_FUNCTIONS "${_pin_fn}")
if(NOT PIN_FUNCTIONS MATCHES "^[0-9]+$")
    message(FATAL_ERROR "lift_aot_split: 解析不了 LIFT_AOT_PIN_FUNCTIONS：`${_pin_fn}`")
endif()

# ---- 派发表里的函数名集合（`LIFT_AOT_ENTRY(0x<EA>u, lift_cc_<root>)`）----
file(STRINGS "${_table}" _table_lines REGEX "^LIFT_AOT_ENTRY\\(0x[0-9A-Fa-f]+u, lift_cc_[0-9A-Fa-f]+\\)$")
if(NOT _table_lines)
    message(FATAL_ERROR "lift_aot_split: lift_table.inc 里一行 LIFT_AOT_ENTRY 都没有")
endif()
set(TABLE_FNS "")
foreach(_line IN LISTS _table_lines)
    string(REGEX REPLACE "^LIFT_AOT_ENTRY\\(0x[0-9A-Fa-f]+u, (lift_cc_[0-9A-Fa-f]+)\\)$" "\\1" _fn "${_line}")
    list(APPEND TABLE_FNS "${_fn}")
endforeach()
list(REMOVE_DUPLICATES TABLE_FNS)
list(LENGTH TABLE_FNS TABLE_FN_COUNT)

# ---- 一份 `.ll` 里的 `define i32 @lift_cc_*` 符号集合 ----
function(_defines_of path out_var)
    file(STRINGS "${path}" _defs REGEX "^define i32 @lift_cc_[0-9A-Fa-f]+\\(")
    set(_names "")
    foreach(_d IN LISTS _defs)
        string(REGEX REPLACE "^define i32 @(lift_cc_[0-9A-Fa-f]+)\\(.*$" "\\1" _n "${_d}")
        list(APPEND _names "${_n}")
    endforeach()
    set(${out_var} "${_names}" PARENT_SCOPE)
endfunction()

set(_all_fns "")
if(LIFT_AOT_SPLIT EQUAL 1)
    # ---- 单份路径：今天的形状。份数必然 1，清单不应存在 ----
    set(_ll "${LIFT_AOT_DIR}/lift_aot.ll")
    if(NOT EXISTS "${_ll}")
        message(FATAL_ERROR "lift_aot_split: split=1 但没有 `${_ll}`")
    endif()
    if(EXISTS "${_manifest}")
        message(FATAL_ERROR "lift_aot_split: split=1 却存在 `${_manifest}`（清单陈旧 / 两种形状混在一棵树里）")
    endif()
    _defines_of("${_ll}" _all_fns)
    list(LENGTH _all_fns _n)
    if(NOT _n EQUAL PIN_FUNCTIONS)
        message(FATAL_ERROR "lift_aot_split: split=1 的定义数=${_n} != PIN_FUNCTIONS=${PIN_FUNCTIONS}")
    endif()
    list(REMOVE_DUPLICATES _all_fns)
    list(LENGTH _all_fns _nu)
    if(NOT _nu EQUAL _n)
        message(FATAL_ERROR "lift_aot_split: split=1 的 .ll 里有重复 `define`（${_n} -> ${_nu}）")
    endif()
    message(STATUS "lift_aot_split: OK（single-O2；defines=${_n}）")
else()
    if(NOT EXISTS "${_manifest}")
        message(FATAL_ERROR "lift_aot_split: LIFT_AOT_SPLIT=${LIFT_AOT_SPLIT} 但没有 `${_manifest}`")
    endif()
    file(STRINGS "${_manifest}" _parts REGEX "^part ")
    list(LENGTH _parts _nparts)
    if(NOT _nparts EQUAL LIFT_AOT_SPLIT)
        message(FATAL_ERROR
            "lift_aot_split: 清单份数=${_nparts} != LIFT_AOT_SPLIT=${LIFT_AOT_SPLIT}"
            "（典型原因：改了 LIFT_AOT_SPLIT 却没重新生成清单）")
    endif()
    set(_sum_functions 0)
    set(_max_part_bytes 0)
    set(_seen "")
    foreach(_p IN LISTS _parts)
        separate_arguments(_f UNIX_COMMAND "${_p}")
        # `part <index> <file> <sha256> <bytes> <functions> <first-root> <last-root>`
        list(LENGTH _f _n)
        if(NOT _n EQUAL 8)
            message(FATAL_ERROR "lift_aot_split: 清单行字段数=${_n}（应为 8）：`${_p}`")
        endif()
        list(GET _f 1 _idx)
        list(GET _f 2 _file)
        list(GET _f 3 _sha)
        list(GET _f 4 _bytes)
        list(GET _f 5 _fns)
        set(_path "${LIFT_AOT_DIR}/${_file}")
        if(NOT EXISTS "${_path}")
            message(FATAL_ERROR "lift_aot_split: 清单第 ${_idx} 份的文件不存在：`${_path}`")
        endif()
        # sha256 / 字节数（清单 vs 实际）
        file(SHA256 "${_path}" _real_sha)
        if(NOT _real_sha STREQUAL _sha)
            message(FATAL_ERROR "lift_aot_split: 第 ${_idx} 份 sha256 不一致（清单=${_sha} 实际=${_real_sha}）")
        endif()
        file(SIZE "${_path}" _real_bytes)
        if(NOT _real_bytes EQUAL _bytes)
            message(FATAL_ERROR "lift_aot_split: 第 ${_idx} 份字节数不一致（清单=${_bytes} 实际=${_real_bytes}）")
        endif()
        # 上界：20 MB（design.md §3.1）
        if(_real_bytes GREATER 20971520)
            message(FATAL_ERROR "lift_aot_split: 第 ${_idx} 份 ${_real_bytes} B 超过 20 MB 上界")
        endif()
        if(_real_bytes GREATER _max_part_bytes)
            set(_max_part_bytes ${_real_bytes})
        endif()
        # 符号集合：两两不交
        _defines_of("${_path}" _defs)
        list(LENGTH _defs _nd)
        if(NOT _nd EQUAL _fns)
            message(FATAL_ERROR "lift_aot_split: 第 ${_idx} 份 define 数=${_nd} != 清单 functions=${_fns}")
        endif()
        math(EXPR _sum_functions "${_sum_functions} + ${_nd}")
        foreach(_fn IN LISTS _defs)
            list(FIND _seen "${_fn}" _dup)
            if(NOT _dup EQUAL -1)
                message(FATAL_ERROR "lift_aot_split: 符号 `${_fn}` 出现在两份分片里（⛔ 两两不交破了）")
            endif()
            list(APPEND _seen "${_fn}")
            list(FIND TABLE_FNS "${_fn}" _in_table)
            if(_in_table EQUAL -1)
                message(FATAL_ERROR "lift_aot_split: 第 ${_idx} 份定义了 `${_fn}`，但派发表里没有它")
            endif()
        endforeach()
    endforeach()
    # Σ functions == PIN_FUNCTIONS
    if(NOT _sum_functions EQUAL PIN_FUNCTIONS)
        message(FATAL_ERROR
            "lift_aot_split: Σ functions=${_sum_functions} != LIFT_AOT_PIN_FUNCTIONS=${PIN_FUNCTIONS}")
    endif()
    # 并集 == 派发表函数集
    list(LENGTH _seen _nseen)
    if(NOT _nseen EQUAL TABLE_FN_COUNT)
        message(FATAL_ERROR "lift_aot_split: 分片并集=${_nseen} != 派发表函数集=${TABLE_FN_COUNT}")
    endif()
    foreach(_fn IN LISTS TABLE_FNS)
        list(FIND _seen "${_fn}" _found)
        if(_found EQUAL -1)
            message(FATAL_ERROR "lift_aot_split: 派发表里的 `${_fn}` 不在任何分片里")
        endif()
    endforeach()
    message(STATUS
        "lift_aot_split: OK（LIFT_AOT_SPLIT=${LIFT_AOT_SPLIT}；Σfunctions=${_sum_functions}；"
        "最大份=${_max_part_bytes} B）")
endif()
