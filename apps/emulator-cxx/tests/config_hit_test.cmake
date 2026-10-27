# 设置界面的**命中查找**判据（`CORE-INPUT-HIT-TEST-12E` 在真实界面上的用法）。
#
# 为什么需要它：右列按钮曾经**永远命中不到**（命中查找里"数组基址"错误地用了"取槽地址"而不是
# 取址原语 ⇒ 指针族的落点被写进指针槽）。症状 = 悬停/点击全部无反应。这条测试把两半都钉住：
#   ① 光标停在 ON 按钮上 ⇒ 脚本自己的命中下标必须 **≥ 0**；
#   ② 光标停在列表正文里 ⇒ 必须是 **−1**。
# 只有 ① 会"恒为 −1"地绿（旧行为），只有 ② 会"什么都算命中"地绿 —— 两条一起才有意义。
if(NOT DEFINED EXE OR NOT DEFINED INSTALL OR NOT DEFINED SRC)
  message(FATAL_ERROR "EXE / INSTALL / SRC 必须由 CTest 传入")
endif()

function(run_probe name script out_var)
  execute_process(
    COMMAND "${EXE}" --install "${INSTALL}" --skip-logo --play-headless
            --input-script "${SRC}/input/${script}"
            --anchor-script 21092 --require-script 20940
            --tick-ms 16 --max-steps 200000 --watch-locals 2014
    RESULT_VARIABLE code
    OUTPUT_VARIABLE out
    ERROR_VARIABLE err)
  if(NOT code EQUAL 0)
    message(FATAL_ERROR "${name}: emulator-cxx 退出码 ${code}\n${out}\n${err}")
  endif()
  if(out MATCHES "opcode-not-implemented")
    message(FATAL_ERROR "${name}: 出现未实现指令\n${out}")
  endif()
  # 收口那行形如 `[watch] final clock=... script=20940 loc[2014]=12`；★ 取**最后**一次
  # （前面的 `change` 行是过程值，第一条是 boot 期脚本留下的 0）。
  string(REGEX MATCHALL "loc\\[2014\\]=(-?[0-9]+)" hits "${out}")
  if(NOT hits)
    message(FATAL_ERROR "${name}: 输出里没有 loc[2014]（--watch-locals 没生效？）\n${out}")
  endif()
  list(GET hits -1 last)
  string(REGEX REPLACE "loc\\[2014\\]=" "" value "${last}")
  set(${out_var} "${value}" PARENT_SCOPE)
endfunction()

run_probe("hover-on" "config-hover-on.txt" on_value)
run_probe("hover-body" "config-hover-body.txt" body_value)

if(body_value GREATER -1)
  message(FATAL_ERROR "列表正文不该命中任何项，实际 loc[2014]=${body_value}")
endif()
if(on_value LESS 0)
  message(FATAL_ERROR
    "右侧 ON 按钮命中不到（loc[2014]=${on_value}）—— 命中查找把「数组基址」当成了槽地址？")
endif()
message(STATUS "命中查找：ON=${on_value}，正文=${body_value}（期望 前者≥0、后者 −1）")
