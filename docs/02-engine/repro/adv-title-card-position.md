# 复现：章节标题卡位置歪 + 人物轮廓整体渲染错

对应缺陷：`REQ-01M65A7BCESQG4C2B59V1HT3SB`。

## 观测点

真机的章节标题卡在**左侧中心**；模拟器画在**下方中央**，且其后人物轮廓整体渲染错。

## 复现命令（需要游戏安装）

```bash
apps/emulator-cxx/out/emulator-cxx --install "$AMAYUI_INSTALL" --skip-logo --play-headless \
  --input-script apps/emulator-cxx/tests/input/route-newgame-chapter0.txt \
  --anchor-script 21092 --tick-ms 16 --bmp .tmp/adv/bmp6 --bmp-stride 100 --bmp-limit 20
```

- 观测：`.tmp/adv/bmp6/frame-1500`（章节标题卡）与后续帧（人物轮廓）。
- BMP 在 `.tmp/`（gitignore）⇒ 不能当 `repro`；本文件是入库的“存在证明 + 重开条件”。

## 为什么 `repro` 指本文件

同 `adv-centered-narration.md`：BMP 不入库；命令、判据、期望都写在这里。

## 下一步（判据方向）

1. 标题卡的位置来自哪条绘制请求（键盘 / 层矩形 / 锚点）；
2. 人物轮廓的错是绘制模式、缩放还是字形来源；
3. 与 `ADV-TEXT-LAYER-DRAW-KEY` 的宿主兜底是否同源。
