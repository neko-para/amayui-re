# S7 间接 `call` 放宽（D4）的**生成期判据**捕获。
#
# 只做一件事：按 `lift-s7-indirect` 的固定输入跑一次 `lift-gen`，把它的 stdout+stderr 原样落到
# `LOG`，让 CTest 能读回 `[S7 criteria]` / `[S7 criteria/site accounts]` / `[S7 downgrade]` 行。
#
# ★ 为什么由构建期跑、而不是测试自己 spawn 生成器：
#   ① 测试是 pure 档，判据必须读**生成那份 `.ll` 的同一趟**的读数（同一次执行 ⇒ 不会"读到的
#      判据"与"链接进去的产物"来自两次不同输入）；
#   ② `lift-gen` 的 stderr 是这条判据的唯一可复核面（`.ll` 头部只有聚合计数、没有逐根结论）。
# ★ 生成的 `.ll` / `.pin` / `.table` 与既有 s7 夹具同形；`--no-close-*` 三个开关与
#   `lift-s7-structured` 的生成命令保持一致（否则 D15/D17/D26 的候选面扫描会慢到分钟级）。
if(NOT DEFINED GEN OR NOT DEFINED EXE OR NOT DEFINED ROOTS OR NOT DEFINED OUT OR NOT DEFINED PIN
   OR NOT DEFINED TABLE OR NOT DEFINED LOG)
  message(FATAL_ERROR "lift_gen_capture.cmake: GEN / EXE / ROOTS / OUT / PIN / TABLE / LOG are required")
endif()

# ★ 可选 `EXTRA`：追加给 `lift-gen` 的额外开关（`;` 分隔）。缺省**空** ⇒ 与改动前逐字相同
#   （`lift-s7-indirect` 就是这个路径）。
#   为什么需要它：`lift-s7-iat` 的根集要的是**受控的 4 个单元**，而静态 call 闭包（D14）默认开
#   ⇒ 它会把 132 个软根拉进来（179 个单元），判据读数就不再是"这四个根"的性质。
#   ⇒ 那一条传 `--no-close-calls`（**只**影响那一条，⛔ 不动 `lift-s7-indirect` 的输入）。
set(_args "${GEN}" --exe "${EXE}" --roots "${ROOTS}" --out "${OUT}" --pin "${PIN}" --table "${TABLE}"
          --no-close-stores --no-close-data --no-close-jmptab)
if(DEFINED EXTRA AND NOT "${EXTRA}" STREQUAL "")
  list(APPEND _args ${EXTRA})
endif()

execute_process(
  COMMAND ${_args}
  RESULT_VARIABLE rc
  OUTPUT_VARIABLE out
  ERROR_VARIABLE err)

file(WRITE "${LOG}" "${out}${err}")
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "lift-s7-indirect: lift-gen failed (exit ${rc}) — full output in ${LOG}")
endif()
