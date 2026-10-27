# 设置界面**每一页**的判据：点该 tab ⇒ 停在期望的脚本、且该页真的建出了期望数量的命中项；
# 再滚 6 格 ⇒ 一次都不许出现"未实现 / 非法 opcode"的停机。
#
# 为什么两头都要：
#  · 只查"还在 20940 里"会**恒真**（点错 tab 也仍在同一个脚本里）——旧脚本的坐标就是这样一路绿的；
#  · 只查"不崩"也不够：滚动触发的是**整页重建**，重建会读一批"按档号读配置"的指令，
#    少实现一条就停 ⇒ 所以滚动那半正是"缺指令"的探针。
#
# 每条 tab 的期望值来自实测（命中项数量 = 该页控件数）；脚本 id 来自 CONFIG.BIN 的分派
# （声音 / 角色两页换成 CONFIG2.BIN 20939）。
if(NOT DEFINED EXE OR NOT DEFINED INSTALL OR NOT DEFINED SRC)
  message(FATAL_ERROR "EXE / INSTALL / SRC 必须由 CTest 传入")
endif()

# tab : 收口脚本（0 = 不检查，那一页会自己重进 CONFIG，收口脚本不唯一）: 页内命中项数量（实测）
set(CASES
    "system:20940:34"
    "game:20940:38"
    "adv:20940:40"
    "sound:0:47"
    "chara:0:56"
    "ops:20940:32")

foreach(case IN LISTS CASES)
  string(REPLACE ":" ";" parts "${case}")
  list(GET parts 0 tab)
  list(GET parts 1 script)
  list(GET parts 2 count)

  # ① 点 tab：停在期望脚本 + 命中项数量对得上
  execute_process(
    COMMAND "${EXE}" --install "${INSTALL}" --skip-logo --play-headless
            --input-script "${SRC}/input/title-option-tab-${tab}.txt"
            --anchor-script 21092 --require-script ${script}
            --tick-ms 16 --max-steps 220000 --watch-locals 0
    RESULT_VARIABLE code OUTPUT_VARIABLE out ERROR_VARIABLE err)
  if(NOT code EQUAL 0)
    message(FATAL_ERROR "tab-${tab}: 退出码 ${code}（期望停在 ${script}）\n${out}\n${err}")
  endif()
  string(REGEX MATCHALL "loc\\[0\\]=([0-9]+)" hits "${out}")
  if(NOT hits)
    message(FATAL_ERROR "tab-${tab}: 输出里没有 loc[0]（--watch-locals 没生效？）\n${out}")
  endif()
  list(GET hits -1 last)
  string(REGEX REPLACE "loc\\[0\\]=" "" got "${last}")
  if(NOT got EQUAL count)
    message(FATAL_ERROR
      "tab-${tab}: 该页命中项数量 ${got}，期望 ${count} —— 点到的是别的 tab，或整页重建没跑起来")
  endif()

  # ② 同一页滚 6 格：不许停机
  execute_process(
    COMMAND "${EXE}" --install "${INSTALL}" --skip-logo --play-headless
            --input-script "${SRC}/input/config-scroll-${tab}.txt"
            --anchor-script 21092 --require-script ${script}
            --tick-ms 16 --max-steps 260000
    RESULT_VARIABLE code2 OUTPUT_VARIABLE out2 ERROR_VARIABLE err2)
  if(NOT code2 EQUAL 0)
    message(FATAL_ERROR "scroll-${tab}: 滚动后退出码 ${code2}（停机 = 缺指令）\n${out2}\n${err2}")
  endif()
  if(out2 MATCHES "opcode-not-implemented")
    message(FATAL_ERROR "scroll-${tab}: 出现未实现指令\n${out2}")
  endif()
  message(STATUS "${tab}: 页内命中项 ${got}，滚动 6 格无停机")
endforeach()

