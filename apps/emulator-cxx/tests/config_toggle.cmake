# 设置界面的**开关按钮真的写配置**判据。
#
# 为什么需要它：右列 ON/OFF 曾经点了没反应（第一条缺 handler 的指令 `0x2cf` 直接把模拟器停掉）。
# 判据用「两次运行的配置快照」差分，而不是断言某个绝对值 —— 初始值取决于本机的玩家 `SYS4REG.INI`：
#   · 点 **ON**  ⇒ `display:ScreenMode` 变成 1；
#   · 点 **OFF** ⇒ 变成 0（这一半在任何初始值下都成立：目标 0 与初始值 1 不同 ⇒ 走"更换"路径写键）。
# 两条合起来：只要 ON/OFF 有一条没接上，差分对不上就红。
if(NOT DEFINED EXE OR NOT DEFINED INSTALL OR NOT DEFINED SRC OR NOT DEFINED OUTDIR)
  message(FATAL_ERROR "EXE / INSTALL / SRC / OUTDIR 必须由 CTest 传入")
endif()

function(run_toggle name script out_var)
  set(dump "${OUTDIR}/config-toggle-${name}.txt")
  file(REMOVE "${dump}")
  execute_process(
    COMMAND "${EXE}" --install "${INSTALL}" --skip-logo --play-headless
            --input-script "${SRC}/input/${script}"
            --anchor-script 21092 --require-script 20940
            --tick-ms 16 --max-steps 220000 --dump-config "${dump}"
    RESULT_VARIABLE code
    OUTPUT_VARIABLE out
    ERROR_VARIABLE err)
  if(NOT code EQUAL 0)
    message(FATAL_ERROR "${name}: emulator-cxx 退出码 ${code}\n${out}\n${err}")
  endif()
  if(out MATCHES "opcode-not-implemented")
    message(FATAL_ERROR "${name}: 出现未实现指令\n${out}")
  endif()
  if(NOT EXISTS "${dump}")
    message(FATAL_ERROR "${name}: 没有导出配置 ${dump}\n${out}")
  endif()
  file(STRINGS "${dump}" line REGEX "^display:ScreenMode=")
  if(NOT line)
    message(FATAL_ERROR "${name}: 配置导出里没有 display:ScreenMode")
  endif()
  string(REPLACE "display:ScreenMode=" "" value "${line}")
  string(STRIP "${value}" value)
  set(${out_var} "${value}" PARENT_SCOPE)
endfunction()

run_toggle("on" "config-toggle-on.txt" on_mode)
run_toggle("off" "config-toggle-off.txt" off_mode)

if(NOT off_mode EQUAL 0)
  message(FATAL_ERROR "点 OFF 之后 display:ScreenMode 应为 0，实际 ${off_mode}")
endif()
if(on_mode EQUAL off_mode)
  message(FATAL_ERROR
    "点 ON 与点 OFF 得到同一个 display:ScreenMode（${on_mode}）—— 右列开关没接上配置写")
endif()
message(STATUS "开关写配置：ON=${on_mode} / OFF=${off_mode}")
