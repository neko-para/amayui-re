# UI 图片烘焙链（ALF→AGF→改图→注回，配方化可复现）

- id: REQ-01M41554NN4ACPMY01BB2CHZM8
- type: req
- status: done
- parent: REQ-01M3SVH2V332E4CJYN0BHH0YK8
- verify: tools/test/ui-bake.test.mjs#① 配方守卫：每条不变量都能红

## 范围
把旧仓那批**手工改过的 UI 图**（10 张汉化图 + 7 张未汉化的 `-0`）从**原始游戏件**重建出来，
落成**数据驱动的配方 + 一条命令**，并把旧仓那一堆一次性脚本（`.tmp/*.py`、`.tmp/*.html`）里的
参数**全部搬进入库文本**（recipe JSON）。

链路（每一步都可复现）：

```text
<游戏安装>\SYS4INI.BIN（ALF 索引）→ DATA1.ALF → SO0xx.AGF → RGBA（= 旧仓 res/images/SO0xx-0.png，逐像素相同）
   ├─ clean 段（列/行填充 · 贴底图 · 模板覆盖 · 拉普拉斯插值 · 置透明 · 局部恢复）─▶ 干净底图
   ├─ text  段（recipe → HTML → headless Chrome）────────────────────────────▶ 文字层
   └─ 合成（`text.compose` 三选一）→ 生效版 PNG → 注回 AGF（保留原头/meta/调色板）
```

## 非目标
* **不搬**旧仓 `scripts/uimap/*.py` 与 `.tmp/*`（登记在 `corpus/assets.json` 的 `tooling/*`）；
  它们只是"参数从哪来"的旁证，不是运行时依赖。
* **不做 OCR / 不做 inpainting**（用户口径：文案手输很快；AI 修复在渐变与 8bpp 量化下出伪影）。
* **不追求 AGF/PNG 的字节级相同**（编码器不同 ⇒ 字节必不可能相同）：判据一律是**像素级相同**。

## 判据
* `pnpm tools ui-bake verify` ⇒ 每张有配方的图**逐像素相同**（`逐通道不等 0`）；
* `pnpm tools ui-bake agf` ⇒ 注回的 AGF 与旧仓 `res/images/*.AGF` **解码后像素相同**；
* `pnpm tools ui-bake lint` ⇒ 配方过 schema / 坐标 / 效果层次守卫（红 = 退出码 1）。

## 口径与坑（**真源不在这里**）
九条"别凭印象改"的实测口径（半开区间 / `--headless=old` / 三种 `compose` 口径 / 不用 Jimp 的
`composite` / `background-clip` 落在画字那层 / 字体钉死 / 裸件优先于 ALF / 旧文档与实测不符的三处）
一律住 **`tools/ui-bake.md` §5**；每张图的特有证据住在它 recipe 的 `_note` 里。本节点不复述（一屏预算）。

## 已知边界
`SO001` 还不归零（约 4.6% 像素有差）：那是**那一张图的生成史**（生效版是旧仓分四段整画布截图叠出来的，
中间段的清理底板与旧参数已不在手上）⇒ 按"多段型图接受较高偏差"处理；残差另由
`REQ-01M42S9QSMTPCYTHDEDCHWX3R5`（S3）track。

## 落地
* 工具：`tools/ui-bake.mjs`（域 `ui-bake`）；模型与算子在 `tools/lib/ui-bake/`；
* 配方：`tools/ui-bake/recipes/<块>.json`（一图一份，同名 `.md` 写口径）；效果字典 `tools/ui-bake/effects.json`；
* 基建守卫：`tools/test/ui-bake.test.mjs`；
* **消费规则**：改一张图的坐标 / 文案 / 样式 = 改它的 recipe（纯文本、可 diff、可审），
  然后 `pnpm tools ui-bake verify <块>` 必须回 0。
