# 复现：文本层「没写键基数 ⇒ 这一层一个字都不显示」

对应缺陷：`REQ-01M664WNNJQA81AXYVPFN350KC`（`ab57b47` 引入，已修；与"点击丢失"是两件事）。

## 观测点

某个文本层**没有调过 `0x213`** 时，它的逐字符绘制条目一条都不建 ⇒ 这一层一个字都不显示
（窗框 / 立绘 / 背景照旧）。

★ 实测纠正：本作 ADV 消息层**确实调了 `0x213`**，所以这**不是**"ADV 点击丢失"的根因
（那条另见 `docs/02-engine/repro/adv-click-loss.md`）。本条只覆盖"没写基数的层"这一类——
它之所以重要，是因为症状同样是"点一下画面不变"，会把人引到输入链上去。

## 复现（不需要游戏安装的最小装置）

```bash
cd apps/emulator-cxx/out
./emulator-core-selftest --only-text-layer-fallback
```

该用例造一个**只调 `0x70`、不调 `0x213`** 的文本层，再用 `0x6e` 追加正文：

- 修好后：兜底键 `kTextLayerFallbackBase + 组号×1000` 上必须出现绘制条目；
- 坏法（`ab57b47`）：`create_text_layer_item` 只看 `key_base_set`（= 脚本调过 `0x213`）
  ⇒ 没写基数的层一条条目都不建 ⇒ 无字。

带安装时的等价人工复现：

```bash
apps/emulator-cxx/out/emulator-cxx --install "$AMAYUI_INSTALL" --skip-logo --play-headless \
  --input-script apps/emulator-cxx/tests/input/route-newgame-chapter0.txt \
  --anchor-script 21092 --tick-ms 16 --bmp .tmp/bug/text --bmp-stride 100 --bmp-limit 20
```

观测：进入 `SC0000.BIN` 后，有消息窗的帧里窗框在、正文**为零像素**。

## 为什么 `repro` 指本文件

`--only-text-layer-fallback` 是**可入库、无安装**的存在证明；BMP 只作插图（`.tmp/` 不入库）。
本条缺陷的"可观测分歧"= 消息层有没有绘制条目，装置里直接断言它。
