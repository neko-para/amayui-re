# 点击节奏判据：同一段时间线里，正文追加次数必须 ≥ 阈值。
# ★ 守的是"点击大量被吞"（等待位被颜色窗卡住时只推进十几次；修好后 >100）。
#   阈值取 60：给"换机器 / 改排版"留余量，但仍远高于坏法的 ~13。
if(NOT DEFINED EXE OR NOT DEFINED INSTALL OR NOT DEFINED SRC OR NOT DEFINED OUTDIR)
    message(FATAL_ERROR "adv_click_rate.cmake: 需要 -DEXE= -DINSTALL= -DSRC= -DOUTDIR=")
endif()
set(log "${OUTDIR}/adv-click-rate.log")
execute_process(
    COMMAND "${EXE}" --install "${INSTALL}" --skip-logo --play-headless
            --input-script "${SRC}/input/adv-click-rate.txt"
            --anchor-script 21092 --tick-ms 16 --max-steps 2000000
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
if(appends LESS 60)
    message(FATAL_ERROR "adv-click-rate: 80 次点击只推进了 ${appends} 次正文（阈值 60）——点击被大量吞掉。日志 ${log}")
endif()
message(STATUS "adv-click-rate: 80 次点击 ⇒ 正文追加 ${appends} 次")
