# 点击节奏判据：同一段时间线里，正文追加次数必须 ≥ 阈值。
# ★ 整窗挡住是引擎语义（spec CORE-OPS-WAIT：颜色窗没走完，等待位不放行），所以本判据守的是
#   "点击被大量吞掉"，**不是**"每句都必须秒过"。
#   80 击 / 24s 的期望约 37（整窗挡住下的实测）⇒ 阈值取 30：仍远高于坏法 0–13
#   （等待位不看颜色窗闩时，2M 步只覆盖 15.5s ⇒ text-appends=0；修好前后都在 0–13 一档）。
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
    message(FATAL_ERROR "adv-click-rate: 80 次点击只推进了 ${appends} 次正文（阈值 30，期望约 37）——点击被大量吞掉。日志 ${log}")
endif()
message(STATUS "adv-click-rate: 80 次点击 ⇒ 正文追加 ${appends} 次")
