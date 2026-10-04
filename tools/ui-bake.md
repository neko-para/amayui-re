# tools/ui-bake.mjs — UI 图片烘焙链（**配方驱动的可复现改图**）

> 一句话：旧仓那批手工改过的 UI 图（10 张汉化图 + 7 张未汉化图），现在由**一条命令 + 一份配方**
> 从**原始游戏件**重放出来，判据是**与生效版逐像素相同**。
> 字段 / 不变量 / 操作的真源是 `pnpm tools ui-bake describe`（本文件不重复 schema）。

## 1. 它取代了什么

旧仓改一张 UI 图的做法是：`scripts/uimap/scan_blocks.py` 找块 → 清理工作台导出
`.tmp/<名>_clean.json` + `.sh` → 手写 `.tmp/ux-redraw/<块>/render*.html` → headless Chrome 截图 →
PIL 合成 → `node scripts/agf/cli.js inject` → 手工复制到 `res/` 与 `patch/`。
参数散在命令行、JSON、HTML、Python 四个地方，**不可审计、不可回归、改一次就废一批脚本**
（旧仓 `.tmp/` 里 700+ `.mjs` + 400+ `.py` 就是这么来的）。

现在：**一个块一份 JSON**（`tools/ui-bake/recipes/<块>.json`），坐标 / 保留带 / 填充列 / 文案 / CSS 全是数据。

## 2. 链路（每一步都可复现）

```text
<游戏安装>\SYS4INI.BIN（ALF 索引）→ DATA1.ALF → SO0xx.AGF → RGBA
   │                                                    （= 旧仓 res/images/SO0xx-0.png，逐像素相同）
   ├─ clean 段：fill（列/行填充）· paste（跨图/块贴底图）· template（块内空白钮覆盖）
   │            · laplace（拉普拉斯模板插值）· transparent（置透明）· restore（局部还原）
   ├─ text 段：recipe → HTML（@font-face 引本地 Sarasa SC）→ headless Chrome
   └─ 合成：`text.compose` 三选一（见 §3）
        └─ 注回 AGF（保留原件的头 / meta / 调色板 / ACIF 前缀 / trailer）
```

## 3. 三种合成口径（**必须显式声明，不许猜**）

这批生效版当年不是同一条链做出来的，所以 `text.compose` 是 recipe 的必答项之一：

| 值 | 做法 | 谁用 | 为什么 |
|---|---|---|---|
| `over-base`（缺省） | 只渲**透明文字层**，再 `src over` 干净底图（PIL `Image.alpha_composite` 同口径） | SO002 | 底图不被 Chrome 重编码 ⇒ 块外逐像素保留 |
| `render-only` | 成品**就是**渲染结果 | SO009B | 旧仓产物与渲染 PNG 逐字节相同 |
| `canvas` | 干净底图作 `<img id="bg">` 贴进页面 + **整画布截图**，截图即成品 | SO009A / SO020 | 旧仓那两张就是这么做的；生效版里带着 Chrome 的 premultiply 往返（`a=0` 的 RGB 归零、`0<a<255` 取整）。同一份 clean+文字走 `over-base` 会差十万级通道数 |

★ 判据只认**像素**：PNG 字节不可能相同（成品由 Jimp 重新编码，参考图是 Chrome 自己的编码器产物；
Pillow 与 Node 的 zlib 版本也不同）。AGF 侧同样只认"解码后像素相同"——调色板若有重复色，
同色索引可在两项间摆动（SO009B 实测 73/182272 字节）而**解码结果完全一样**。

## 4. 怎么查 / 怎么改

```bash
pnpm tools ui-bake                      # 域详情（数据 / 不变量 / 操作）
pnpm tools ui-bake describe             # ★ 字段 / 不变量 / 命令（schema 的唯一真源）
pnpm tools ui-bake list                 # 配方一览：块 / 生效版 / 是否汉化 / 有没有配方
pnpm tools ui-bake lint                 # ★ 配方守卫（红 = 退出码 1；跑 `pnpm test` 也会跑）
pnpm tools ui-bake plan   <块>          # 执行计划（clean 操作 + 文字层），不渲染
pnpm tools ui-bake run    <块>          # 重放到 .tmp/ui-bake/（干净底图 / 文字层 / 成品 / AGF / 生成的 HTML）
pnpm tools ui-bake verify [<块>…]       # ★ 判据：重放 + 与 corpus/assets/ui-images/ 生效版逐像素对照
pnpm tools ui-bake agf    [<旧仓路径>]  # AGF 层判据：注回产物 vs 旧仓 res/images/*.AGF 解码后比像素
pnpm tools ui-bake build  [--out <目录>] # ★ 把**全部**配方从原始件完整构建 → dist/ui-bake/（PNG + AGF + 报告）
pnpm tools ui-bake apply  <块> [--write]# 写回 corpus/assets/ui-images/<块>-<版>.png（缺省 dry-run）
```

### 为什么要有 `build`（与 `apply` 的分工）

`corpus/assets/ui-images/` 里那批 PNG 是**旧仓搬进来的**（`apply` 至今没执行过，我逐个 sha256 核过
它们与旧仓 `res/images/` 逐字节相同）。所以"当前资源"**并不能证明本仓能构建它们**。
`build` 就是那条证明：它把 10 张图**从只读原始件**完整走一遍
（clean → headless 渲染 → 合成 → 注回 AGF），产物落在 **`dist/ui-bake/`**（生成物区，`git add` 不到），
并当场报三列：

| 列 | 是什么 | 期望 |
|---|---|---|
| ① PNG 逐像素 vs corpus 生效版 | 构建出的图对不对 | 全 0 |
| ② AGF 解码后 vs 旧仓 `res/images/<块>.AGF` | 注回件对不对（同一份 8bpp 领域的比对） | 全 0 |
| ③ AGF 解码 vs 我们自己的成品 PNG | 参考量：**8bpp 调色板量化损失** | 必然 >0（量级 22–54），旧仓那批同样非零 |

★ `loose` 来源（SO025）在 ② 上**不能**与旧仓 `res/images/` 比：它的生效版来自安装根裸件，
而旧仓那份是另一条链的产物（256 项调色板只有 18 项相同）⇒ 这一类只断言量化损失量级（maxΔ ≤ 48）。

`build` **不写** `corpus/`：要在"构建物"与"入库件"之间做取舍，那是 `apply --write` 的事
（而它会把文件换成 Jimp 编码、丢掉"与旧仓逐字节相同"这条性质，所以默认不做）。

* **改一张图** = 改它的 recipe（坐标 / 文案 / 样式），然后 `verify <块>` 必须回 **0**。
  若改的是"日文原图的清理方式"，那属于"重建原始件"的范畴，**不要在 recipe 里将就**。
* `--game <安装目录>` 指定原始件位置（缺省 `E:\Games\Eushully\天結いキャッスルマイスター`）；
  `--chrome <路径>` 或环境变量 `UI_BAKE_CHROME` 指定浏览器（缺省自动探测 Chrome/Edge）。
* 中间产物一律落 `.tmp/ui-bake/`（派生区，不入库）；**原始游戏件只读**。

## 5. 复现口径（踩过的坑，别改回去）

1. **headless 必须 `--headless=old`**：`--headless` / `=new` 在 Windows 上会**真开一个窗口**；
   三者产物逐字节相同，但只有 `=old` 不弹窗。★ 同一 HTML 重复渲染**确定性**。
   ★★ **受限沙箱下这一步会失败，且失败原因与配方无关**：Chrome 的 mojo IPC 要开**命名管道**，
   沙箱禁止 ⇒ 报 `FATAL:mojo…platform_channel.cc: Check failed: . : 拒绝访问。(0x5)`，
   随后 `crashpad` 自杀、`--screenshot` 拿不到文件。**判据**：先在 `read-only`/`workspace-write` 下
   直接跑一次 `chrome --headless=old --screenshot=… data:text/html,<h1>x</h1>`；
   如果它也失败，就别去动配方 —— 那是**这一步需要提权**（与 `pnpm install` 同一类）。
   本仓 `AGENTS.md` §5 记的"不要捕获子进程输出（要开命名管道 ⇒ EPERM）"是同一堵墙的表现。
2. **不整画布截图**（除非该块声明 `canvas`）：把底图贴进页面让 Chrome 重画，会引入
   premultiply 往返散色（SO002 实测 1544 px 偏差）。
3. **合成用 PIL 口径**，不要用 Jimp 自带的 `composite`/`blit`：后者是 `alpha = dstA + srcA`
   与 `(a*(s-d)-d+255)>>8+d`，边缘反锯齿像素差 1（SO002 一个块 1109 px）。
4. **清理是半开区间**（与旧仓 `clean_fill.py` 的 `range()` 逐字对齐）：列填充填 `[x0+keepL, x1-keepR+1)`。
   改成闭区间在 SO002 上就是 2572 px 偏差。
5. **`background-clip:text` 的声明必须落在画字的那一层**：渐变写在最内层 span 上才对。
   写在外层 div 上时，div 只含绝对定位子元素（高 0）⇒ **渐变轴长 0、上半白段消失**
   （SO001 中簇：字心变成描边色；SO039 列3：两段式渐变丢上半段）。
   ⇒ `buildTextHtml` 会把 `background` / `background-image` / `-webkit-background-clip` /
   `background-clip` / `-webkit-text-fill-color` 这五条**自动落到 span**，其余落外层 div。
6. **同块多层的绘制顺序**照旧仓：`shadow → outline → fill` 自下而上（`z-index` 显式给）。
7. **`line-height` 是定位的一部分，而且有两种口径**：
   * 旧仓用 `display:grid; place-items:center` 的块（`.tb` 那类），`line-height` = **字号**，
     字形靠 grid 居中；
   * 旧仓用 **flex 居中** 的块（`.btn.row/.btn.col` 那类，SO025/SO021 的按钮）实测比
     "`line-height` = 块高"的盒内居中**低 1px** ⇒ `pos.y` 直接用 `y0`，不要 `y0-1`。
   把 grid 那类写成 `line-height:<块高>` 会让基线落到行盒底部（SO001 实测整体偏低 **21px**）。
8. **纵向渐变画在行内盒上会按字体度量拉伸**：span 若是 `position:static`（行内盒），
   渐变盒高 = 字体 ascent+descent ≈ **26.6px** 而不是行高 22px ⇒ 渐变被纵向拉伸
   （SO017 实测蓝通道 ±1，14,078 通道的残差）。要落成 `display:inline-block` 让盒子
   = 文字宽 × 行高。
9. **`transform:scaleX()` 要写在"片段 span"上而不是层 div 上**：旧仓把它加在 flex item
   （宽度 fit-content、按自身中心缩放）上；写在层 div 上缩放中心/取整不同
   （SO025 col1 实测每块差 ~1400 px）。
10. **字体必须钉住**：`@font-face` 指向 `corpus/assets/fonts/SarasaGothicSC/*.ttf`
    （由 `corpus/assets/fonts/SarasaGothicSC-TTF-1.0.40.7z` 解出、与旧仓 `res/fonts/` 逐字节相同）。
    这条不钉住时，headless 会**静默**去用系统装的同名字体 —— 换机器就换字形。
    ⚠ 别被"旧 HTML 里写 WenQuanYi"骗了：SO030 的四份旧 HTML 全写文泉驿，但生效版字形实测是 Sarasa。
11. **裸件优先于 ALF 条目**：安装根 `<名>.AGF` 覆盖 `DATA1.ALF` 里的同名条目。SO025 的生效版
    就是安装根那份裸件（同尺寸/同 bpp，但调色板 256 项只有 18 项相同）⇒ recipe 用
    `source.kind = "loose"` 声明。
12. **比"文字层"前先看 `compose`**：`canvas` 口径下 `.tmp/ui-bake/<块>-textlayer.png`
    **是整画布截图、含底图**（不是透明文字层）。拿它去和"透明层"逐像素比会得出完全错误的结论
    —— 要比重放效果就比**成品**（`<块>-<版>.png`），要"文字层对文字层"就得把两边的 `compose`
    与底图都对齐。SO001 排查时踩过这条。
13. **`layer.css` 里 `background*` 那五条会自动落到 span**（见第 5 条），而且 span 缺省是
    `position:absolute; left:0; right:0`。纵向渐变画在**行内盒**上才是"文字宽 × 行高"，
    所以渐变类效果通常需要 `spanBox:"inline"`（配 `align:"center"` 用 grid 居中，
    等价于旧仓 `.tb{display:grid;place-items:center}` + `.ly{grid-area:1/1}`）。

## 6. 目录

```text
tools/ui-bake.mjs                # CLI（参数 → 模型 → 输出）
tools/lib/ui-bake/
  recipe.mjs                     # 领域模型：配方 schema / 校验 / HTML+CSS 生成
  bake.mjs                       # 领域模型：按配方重放（clean → 渲染 → 合成 → 注回）
  image.mjs                      # 纯工具：像素算子（填充 / 贴块 / 恢复 / 合成 / 拉普拉斯）
  render.mjs                     # 纯工具：headless Chrome 渲染 + @font-face
  agf-source.mjs                 # 纯工具：ALF → AGF → RGBA（**只读**原始件）
  agf-write.mjs                  # 纯工具：把改好的 RGBA 注回 AGF
  age-format.mjs                 # 纯工具：`packages/age-format` 的入口收敛（CLI 不直接伸手到 packages/）
tools/ui-bake/effects.json       # 效果字典：E1…E10 共享 CSS + 层次的全局定义
tools/ui-bake/recipes/<块>.json  # ★ 一个块一份配方（入库真源）
```

## 7. 判据的现状（**不写进本文件**）

哪几张已经归零、残差多少、为什么 —— 那是**状态**：它由 `pnpm tools ui-bake verify` 现场回答，
不手写在这里（`AGENTS.md` §10）。进度与"还要做什么"看 `pnpm tools requirements plan`。
