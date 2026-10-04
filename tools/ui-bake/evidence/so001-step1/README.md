# SO001 第一步的证据（为什么这些文件在仓库里）

这一目录不是生成物，也**不是**复现链的一部分。它是**证据包**：用来证明
`corpus/assets/ui-images/SO001-4.png` 那张图的**第一步**用什么参数做的，以及**为什么它是这样**。

> 唯一的消费者是 `tools/ui-bake/recipes/SO001.md` 的「已知残差」与
> `REQ-01M42S9QSMTPCYTHDEDCHWX3R5`（SO001 重放残差，缺陷单）。
> **为什么**整条链只有这样一张图有残差、以及复现的边界在哪，见 `docs/00-origin/decisions.md`。

## 来源

用户提供的**当年做第一步的历史会话档案**（`dsh-session-session-fe0d9b57-…zip`，在仓库外，不迁入）。
档案里的 `gen15.py` 与会话内产物是**这些文件唯一的来源**；仓库里没有第二份。

## 这些文件各自证明什么

| 文件 | 证明什么 |
|---|---|
| `gen15.py` | 第一步**真值生成器**：`left = x0-8`、`top = y0+24`、`width:93`、`height:26`、`line-height:22`、`scaleX(0.6/0.8)`、warm 三层 `shd→out→gin`、三条描边 **2px**。它生成下面那个 HTML。 |
| `so001_render15.html` | **第一步的最终渲染页**（`gen15.py` 的产物）。 |
| `so001_clean_full.png` | 它需要的**清理底图**（`<img id="bg">`；日文已被抹掉、中文还没画）。仓库其它地方只有清理后的**成品**，没有这张中间底图。 |
| `so001_render15.png` | 用上面两个文件经 headless Chrome 渲出的图 —— **与当时的 `SO001-1.png` 逐像素相同**。`corpus/` 里**只有** `SO001-4.png`，没有 `-1`，所以这是那份中间版唯一的视觉留档。 |
| `textonly*.html` / `.png` | 字体对照组的实验记录：缺字体 / 换雅黑 / 无阴影 三种情况下**字形与墨迹范围**的差别。用来钉住"字体必须真加载、字形不能靠系统字体兜底"。 |

## 怎么用

* **复现第一步**（判据是"与 `so001_render15.png` 逐像素相同"）：把这四个文件放同一目录，
  让 `../res/fonts/SarasaGothicSC/{Regular,Bold}.ttf` 可解析（旧仓 `res/` 的布局），
  再跑 `tools/lib/ui-bake/render.mjs` 的同一套 Chrome flag（见 `tools/ui-bake.md` §5.1）；
* **别把它们当成 recipe 的输入**：配方是从**原始 ALF/AGF** 重建的，不读这一目录。
  这里只回答"当年那一步的参数是什么"。

★ 本目录的 PNG 走 LFS（`.gitattributes` 的 `*.png`）；`.py` / `.html` 是纯文本、正常入库。
