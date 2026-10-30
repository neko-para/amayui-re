# 点击节奏判据（**程序侧基线，不是引擎要求**）：同一段时间线里，正文追加次数必须 ≥ 30。
# ★ 整窗挡住是引擎语义（spec CORE-OPS-WAIT：颜色窗没走完，等待位不放行）；但"能推进几次"
#   由这份脚本写的毫秒数（`0x238` 的时长 / `0x202` 的延迟与持续时间）决定 ⇒ 本判据只守
#   "点击被大量吞掉"这一件事，⛔ 不能读成"引擎要求每句多久"。
# ★ 实测（这份 install + 这条时间线）：80 击 / 24s ⇒ text-appends=37；阈值 30 是留余量的下限。
#   坏法（等待位不看颜色窗闩）只有 0–13 ⇒ 30 能把两种形态分开。
# ⚠ 脚本 / 配置一变就要重算这个基线；与 CMakeLists.txt 的同名注释保持**同一组数字**（37 / 30）。
if(NOT DEFINED EXE OR NOT DEFINED INSTALL OR NOT DEFINED SRC OR NOT DEFINED OUTDIR)
    message(FATAL_ERROR "adv_click_rate.cmake: 需要 -DEXE= -DINSTALL= -DSRC= -DOUTDIR=")
endif()
set(log "${OUTDIR}/adv-click-rate.log")
execute_process(
    COMMAND "${EXE}" --install "${INSTALL}" --skip-logo --play-headless
            --input-script "${SRC}/input/adv-click-rate.txt"
            --anchor-script 21092 --tick-ms 16 --max-steps 8000000
    OUTPUT_FILE "${log}"
    RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "adv-click-rate: 播放器退出码 ${rc}（日志 ${log}）")
endif()
file(READ "${log}" text)
if(NOT text MATCHES "text-appends=([0-9]+)")
    message(FATAL_ERROR "adv-click-rate: 日志里没有 text-appends=（见 ${log}）")
endif()
set(appends "${CMAKE_MATCH_1}")
if(appends LESS 30)
    message(FATAL_ERROR "adv-click-rate: 80 次点击只推进了 ${appends} 次正文（阈值 30，本机实测基线 37，程序侧基线）——点击被大量吞掉。日志 ${log}")
endif()
message(STATUS "adv-click-rate: 80 次点击 ⇒ 正文追加 ${appends} 次（阈值 30 / 实测基线 37）")
