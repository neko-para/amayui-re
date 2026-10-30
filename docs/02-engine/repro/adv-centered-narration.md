# 复现：序章「夕阳 ↔ 光柱塔」之间的居中旁白不显示

对应缺陷：`REQ-01M65A72Y5ZHJPXMGPMHB0KH92`。

## 观测点

真机在**夕阳天空**与**光柱塔**两幕之间、以及各自幕内，有**多句旁白画在屏幕中央**（无消息窗框、按点击逐句推进）。
模拟器里这些旁白一句都不出现，且点击直接跳到下一幕。

## 复现命令（需要游戏安装）

```bash
apps/emulator-cxx/out/emulator-cxx --install "$AMAYUI_INSTALL" --skip-logo --play-headless \
  --input-script .tmp/adv/walk-adv2.txt --anchor-script 21092 --tick-ms 16 \
  --bmp .tmp/adv/bmp5 --bmp-stride 100 --bmp-limit 14
```

- `.tmp/adv/walk-adv2.txt` 是宿主输入时间线（本地临时文件，不入库）；等价的可入库时间线见
  `apps/emulator-cxx/tests/input/route-newgame-chapter0.txt`。
- 观测：`.tmp/adv/bmp5/frame-0700`（夕阳）与 `frame-0900`（光柱塔）都是纯背景、无字。
- 有消息窗的场景能出字（`.tmp/adv/bmp4` 末帧）⇒ 不是文本整条链没通，而是这两幕用了另一种呈现。

## 为什么 `repro` 指本文件

BMP 落在 `.gitignore` 的 `.tmp/` 下，**换机器/干净 clone 不存在**，不能当 `repro`（否则
`requirements validate` 必红）。本文件是可入库的“存在证明 + 重开条件”：命令、观测点、期望都写死；
BMP 只作插图。

## 下一步（判据方向）

1. 这两幕的文本是同一个文本层（`0x6e` / `0x196` + `0x70` 的矩形）还是另一条“居中字幕”路径；
2. 若是同一文本层：为什么没有绘制条目（键基数 / 层矩形 / 揭示游标）；
3. 点击直接跳下一幕是否因为空文本也被当成一句（`0x305` 的武装门）。
