# 设置界面底部示例文本：**文本层画布真的被画上了字**。
#
# 判据分两半，缺一不可：
#  · 结构：图层 9 的画布 = 纹理 29、尺寸 = `0x70` 写的 824×120
#    （尺寸不对 ⇒ 层矩形 / 纹理编号 / 画布创建这条链断了）；
#  · 内容：alpha=0 的像素数 < 总像素数（否则就是一张全透明画布 —— 文本没画进去）。
#
# 为什么不比像素 / 不比截图：宿主字体（Amayui CN）与真机字型不同，逐像素没有 oracle；
# 但"画布上有没有墨"是机械可判的，而且文本层整条链（0x70/0x71/0x79/0x213 + 0x6e/0x196）
# 断任何一环都会让它变红。
if(NOT DEFINED EXE OR NOT DEFINED INSTALL OR NOT DEFINED SRC OR NOT DEFINED OUTDIR)
  message(FATAL_ERROR "EXE / INSTALL / SRC / OUTDIR 必须由 CTest 传入")
endif()

execute_process(
  COMMAND "${EXE}" --install "${INSTALL}" --skip-logo --play-headless
          --input-script "${SRC}/input/title-option-tab-adv.txt"
          --anchor-script 21092 --require-script 20940
          --tick-ms 16 --max-steps 220000
          --dump-texture 29 --dump-texture-out "${OUTDIR}/config-sample-text.bmp"
  RESULT_VARIABLE code OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT code EQUAL 0)
  message(FATAL_ERROR "config-sample-text: 退出码 ${code}\n${out}\n${err}")
endif()

if(NOT out MATCHES "texture 29 \\(824x120\\)")
  message(FATAL_ERROR "config-sample-text: 没有导出 824×120 的纹理 29（文本层画布没建出来）\n${out}\n${err}")
endif()

string(REGEX MATCH "alpha0=([0-9]+)" _ "${out}")
if(NOT CMAKE_MATCH_1)
  message(FATAL_ERROR "config-sample-text: 输出里没有 alpha0=...（--dump-texture 没生效？）\n${out}")
endif()
set(zero "${CMAKE_MATCH_1}")
math(EXPR pixels "824 * 120")
if(zero GREATER_EQUAL pixels)
  message(FATAL_ERROR
    "config-sample-text: 纹理 29 全透明（alpha0=${zero} / ${pixels}）——示例文本没有画进文本层")
endif()

message(STATUS "config-sample-text: 纹理 29 有墨（alpha0=${zero} / ${pixels}）")

# 第二半：**循环演示闸真的在重放**。示例文本不是一次性打字机 —— 用户口径是"应当不断重复刷新展示
# （让用户看到推进速度）"。旧实现把它写成一次性（纹理 19 秒与 169 秒逐字节相同）⇒ 这里为 0。
if(NOT out MATCHES "text-gate slot=9 cells=[0-9]+ cycles=([0-9]+)")
  message(FATAL_ERROR "config-sample-text: 输出里没有 text-gate 诊断行（演示闸没被服务）\n${out}")
endif()
if(NOT CMAKE_MATCH_1 GREATER 0)
  message(FATAL_ERROR "config-sample-text: 演示闸 cycles=0 —— 示例文本只展示了一次（没有循环重放）")
endif()
message(STATUS "config-sample-text: 演示闸重放 ${CMAKE_MATCH_1} 轮")
