# UI 图片烘焙链（ALF→AGF→改图→注回，配方化可复现）

- id: REQ-01M41554NN4ACPMY01BB2CHZM8
- type: req
- status: doing
- parent: REQ-01M3SVH2V332E4CJYN0BHH0YK8

## 范围

把旧仓那批**手工改过的 UI 图**（10 张汉化图 + 7 张未汉化的 `-0`）从**原始游戏件**重建出来，
落成**数据驱动的配方 + 一条命令**，并把旧仓那一堆一次性脚本（`.tmp/*.py`、`.tmp/*.html`）里的
参数**全部搬进入库文本**（recipe JSON）。

链路（已实测可复现，见下）：

```text
<游戏安装>\SYS4INI.BIN（ALF 索引）→ DATA1.ALF → SO0xx.AGF → RGBA
   │                                    （= 旧仓 res/images/SO0xx-0.png，逐像素相同）
   ├─ clean 段（列/行填充 · 贴底图 · 模板覆盖 · 拉普拉斯插值 · 置透明 · 局部恢复）──▶ 干净底图
   ├─ text  段（recipe → HTML → headless Chrome）──▶ 文字层
   └─ 合成（`text.compose` 三选一，见下）        ──▶ 生效版 PNG
                                                  └─▶ 注回 AGF（保留原头/meta/调色板）
```

## 非目标

* **不搬**旧仓 `scripts/uimap/*.py` 与 `.tmp/*`（登记在 `corpus/assets.json` 的 `tooling/*`）；
  它们只是"参数从哪来"的旁证，不是运行时依赖。
* **不做 OCR / 不做 inpainting**（用户口径：文案手输很快；AI 修复在渐变与 8bpp 量化下出伪影）。
* **不追求 AGF/PNG 的字节级相同**：PNG 编码器不同（Pillow 的 zlib 与 Node 的 zlib 版本不同）
  ⇒ 字节必不可能相同；AGF 侧调色板若有重复色，同色索引可在两项间摆动（解码后像素相同）。
  判据一律是**像素级相同**。

## 判据

* `pnpm tools ui-bake verify` ⇒ 每张有配方的图**逐像素相同**（`逐通道不等 0`）；
* `pnpm tools ui-bake agf` ⇒ 注回的 AGF 与旧仓 `res/images/*.AGF` **解码后像素相同**；
* `pnpm tools ui-bake lint` ⇒ 配方过 schema / 坐标 / 效果层次守卫（红 = 退出码 1）。

## 已定的口径（实测，别凭印象改）

1. **原始件是只读来源**：`agf-source.mjs` 只 `readSync`；索引是 `SYS4INI.BIN`（**不是** `DATA1.AAI`，那个文件不存在）。
2. **文字层必须由 headless Chrome 渲**：`--headless=old`（`--headless`/`=new` 在 Windows 上会弹窗，
   三者产物逐字节相同）；重复渲染**确定性**。
3. **合成有三种口径，由 recipe 的 `text.compose` 显式声明**（**不许猜**）：
   `over-base`（缺省，只渲透明文字层再合成）/ `render-only`（成品即渲染结果）/
   `canvas`（底图作 `<img id="bg">` + **整画布截图**，旧仓 SO009A·SO020 就是这条）。
   `canvas` 会把 Chrome 的 **premultiply 往返**烙进底图，而那些像素**就在生效版里**：
   SO020 同一份 clean+文字层走 `over-base` 差 145985 通道、走 `canvas` 为 0；SO002 反过来
   （整画布会让它差 1544 px）。⇒ **一张图的历来口径必须由证据定，不能全局统一**。
4. **合成不用 Jimp 自带 `composite`/`blit`**：它是 `alpha = dstA + srcA` 口径，与 PIL 不同
   （SO002 一个块差 1109 px）⇒ `image.mjs::composeOver` 是 PIL `alpha_composite` 的等价实现。
5. **清理是半开区间**（与旧仓 `clean_fill.py` 的 `range()` 逐字对齐）：列填充填 `[x0+keepL, x1-keepR+1)`。
   改成闭区间在 SO002 上就是 2572 px 偏差。
6. **`background-clip:text` 的声明必须落在画字那一层**（渐变才有用）；写在外层 div 上，
   div 只含绝对定位子元素（高 0）⇒ 渐变轴长 0、上半段消失。
7. **字体必须钉住**：`@font-face` 指向 `corpus/assets/fonts/SarasaGothicSC/*.ttf`（由同名 7z 解出、
   与旧仓 `res/fonts/` 逐字节相同；缺失时**硬失败**并给出解压命令，不许静默退回系统字体）。
   ⚠ 旧 HTML 里写的字体名可能是**错的**（SO030 四份全写 WenQuanYi，生效版字形实测是 Sarasa）。
8. **裸件优先于 ALF 条目**：安装根 `<名>.AGF` 覆盖 `DATA1.ALF` 里同名的那一条。SO025 的生效版
   就是安装根那份裸件（同尺寸/同 bpp，但调色板 256 项只有 18 项相同）⇒ `source.kind = "loose"`。
   这**不是**"由 ALF 载荷派生"，任何 clean 算子都表达不了。
9. **旧文档有几处与实测不符**（一律以生效版像素为准）：SO030 的阶段角色写反、SO030 的字体名写错、
   SO039 导出的清理 JSON 漏了一块 —— 细节见 `tools/ui-bake.md` §5 与各 recipe 的 `_note`。

## 已知边界

`SO001` 还不归零（约 4.6% 的像素有差），**不是参数没调对**而是那**一张图的生产史**：
生效版是旧仓分四段整画布截图叠出来的，实测当多段链重放时 **stage1 就差 52713 通道**
⇒ 中间段的清理底板与旧参数**已不在手上**。⇒ 按**"多段型图"接受较高偏差**处理；
完整证据记在该 recipe 的 `note` 里（**别在本节点展开实现细节**）。

## 落地

* 工具：`tools/ui-bake.mjs`（域 `ui-bake`）；模型与算子在 `tools/lib/ui-bake/`；
* 配方：`tools/ui-bake/recipes/<块>.json`（一图一份，同名 `.md` 写口径）；效果字典 `tools/ui-bake/effects.json`；
* 口径与踩坑：`tools/ui-bake.md`；基建守卫：`tools/test/ui-bake.test.mjs`；
* **消费规则**：改一张图的坐标 / 文案 / 样式 = 改它的 recipe（纯文本、可 diff、可审），
  然后 `pnpm tools ui-bake verify <块>` 必须回 0。
