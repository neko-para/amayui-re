# emulator-lift · GetTextMetricsA 度量缓存 + 基线补偿搬到 gmptGlyphOrigin.y（与 cxx 同基线）

- id: REQ-01M6WXEDGGJY1Q1YDEM07S1YR3
- type: req
- status: done
- parent: REQ-01M6H6G6DSRG1ZRD810Z0HTH8V
- verify: apps/emulator-lift/tests/gdi_font_metrics_cache.cpp#cache/size-change
- tags: [emulator-lift m2 font perf]

## 一句话

两件事，同一个落点（`apps/emulator-lift` 宿主字体层）：**①** `GetTextMetricsA` 不再每次真算（度量缓存 +
命中率可观测），**②** 宿主基线补偿 `+2px` 从"只对 `TextOutA` 生效"改成"对引擎正文也生效"——位移落在
`gmptGlyphOrigin.y`、**不改度量**，从而与 `apps/emulator-cxx` 的 `lift = 2` 在同一文本上基线一致。

## ① 度量缓存（`FontEngine::metrics`）

- 键 = `metrics` 的输入集 `(clamp 后像素字号, bold, 字体路径)`；⛔ **不用 HDC**（同一屏幕 DC 会被
  `SelectObject` 换字体量不同字号 ⇒ 小字号 ascent 复用给大字号，差 8px）。
- 只缓存 **FT 派生段**；`weight / italic / underlined / struck_out` 不进键，只由 `gdiTextMetrics`
  从 desc 覆盖（**唯一一处**：两处写同一字段时删任一处守卫都照样绿）。
- synthetic 不进缓存；上限 64 条、到顶整表 clear（不做 LRU、不引依赖）。
- 失效：`setSyntheticForTest`（两级都清）+ **`fontSetConfig`**（新增配置代际钩子；顺带修掉"换 `--font` 后字形缓存仍报旧字面"的潜伏问题）。
- 可观测：`--profile` 的 shim 表下面多一行 `font!TEXTMETRIC-cache hits=… misses=… hit=…%`
  （不是墙钟、不与 shim 表相加），见 `repo:apps/emulator-lift/README.md` §4.3.1。

## ② 基线补偿搬到 `gmptGlyphOrigin.y`

- 常量一处：`repo:apps/emulator-lift/src/runtime/font.h` 的 `kBaselineLift`（注释指向 cxx 的
  `text_raster.cpp:647`，唯一应用点 `:675`）；运行时仍由 `--baseline-lift` 决定（缺省 2、`0` = 关）。
- 位移点：`repo:apps/emulator-lift/src/runtime/font.cpp` 的 `FontEngine::glyph` →
  `origin_y = bitmap_top + baseline_lift`；同时删掉 `gdiDrawText` 的 `- lift`（否则 `TextOutA` 抬 2N px）。
  两处合成结果与原行为逐像素相同。
- 为什么必须这样：引擎正文走 `GetGlyphOutlineA` + guest 自己合成（`0x46F893`：字形顶 = 基线 − origin.y），
  原来的 `- lift` 只在 `TextOutA` 基线上、对正文**完全没生效** ⇒ guest 可见 ascent 仍是 FT 20、cxx 有效值
  18 ⇒ 字比 cxx 低 2px。等价性：cxx 的 `baseline − top = y+20−2−top`，lift 的 `(y+20) − (top+2)` 同一式。
- 度量不动（`tmAscent` 仍是 FT 真值、行距 / extent 不变），修订说明见 `repo:apps/emulator-lift/SCOPE.md` §5.1。

## 判据（守卫，都会红；文件 = `tests/gdi_font_metrics_cache.cpp` / `tests/text_baseline_lift.cpp`）

| 断言 | 用例 |
|---|---|
| 同字体同字号连测两次 ⇒ 第二次命中（FT 段只算一次） | `cache/hit` |
| `-20`→`-28`：28 的 ascent/height **大于** 20、`misses == 2`（没串键） | `cache/size-change` |
| 20→28→20：回选小字号命中且值不变 | `cache/size-back` |
| `weight` 不进键、但 `tmWeight` 永远是本次 desc 的 | `cache/weight-not-in-key` |
| 同字号换 bold（weight 700）必须是另一条（不许复用常规 face 的度量） | `cache/bold-not-aliased` |
| `setSyntheticForTest` 之后必须 miss | `cache/synthetic-off-miss` |
| `fontSetConfig`（同路径只改 lift）之后必须 miss | `cache/config-miss` |
| 命中率那行真的打出来且数字与存取器一致 | `cache/profile-line` |
| `--baseline-lift 0` 与缺省：`gmptGlyphOrigin.y` 差正好 2、方向正确 | `lift/origin-delta` |
| 两次 `TEXTMETRICA` 56B 逐字节相同、extent 宽高不变 | `lift/metrics-unchanged` |
| 开关设 5 ⇒ origin 差正好 5（不是硬编码） | `lift/knob` |
| 墨迹像素总数不变（容差 0）+ 垂直质心正好移 2px | `lift/pixels` |

变异自检（手动施加 → 确认红 → 撤销，6/6 红；每条破坏的对应项见交付说明）。

## 实测（RelWithDebInfo · `windows-x64`）

- **A**：OPTION 装载帧（`--input-script .tmp/input/observe-enter-option.txt`，逐帧栈 `[frame] n=127`）：
  `gdi32!GetTextMetricsA` 改前 64.7 ms × 561（≈115 µs/次）→ 改后同脚本同帧号累计差
  3.5 ms(f127) − 2.4 ms(f126) ≈ **1.1 ms × 561**（≈2 µs/次）；整轮 83.2 ms/677 → 3.0 ms/677，
  命中率 **98.6%**（685 命中 / 10 未命中）。
- **B**：**TITLE 帧 120 三张两两逐字节相同**（改前 / 改后缺省 / 改后 `--baseline-lift 0`）——该画面
  **没有**经 GGO + `gmptGlyphOrigin.y` 合成的可见文本（标题 / 按钮都是素材 sprite）⇒ TITLE 不能当对照面。
  可见引擎文本在 **OPTION**（帧 155，561 次 GTM / 596 次 GGO）：`--present-screenshot`(24bpp) 与
  `--dump-frame`(32bpp) **给出同一组数** —— 变化像素 17764、其余 483/720 行逐字节相同（布局没动）；
  在变化像素集上做垂直平移匹配：`k=+2` 均差 **3.57/255**（86.4% 像素差 ≤8）、`k=0` 均差 **108.16**
  ⇒ 墨迹整体上移 **2px**（残差 3.57 = 字形半透明混在**静止**纹理背景上的自然起伏；裁剪会整行丢失）。
  口径 = **整机截图像素 + cxx 单元级量**（不是 cxx 整屏截图：两个实现渲染器不同，跨实现像素比对不构成
  oracle；cxx 侧仍用 `text_raster.cpp:647` 的 `lift = 2` / 有效 ascent 18）。

## 边界 / 残差

- 引擎若把 `gmptGlyphOrigin.y` 当"度量"而不是"落墨偏移"用，位移就会改布局 —— 实测 OPTION 整屏只有文本行内
  的墨迹变化、其余 483/720 行逐字节相同；但这是**整屏采样**，不排除某条未走到的文本路径另有用法，发现即开单。
- TITLE 没有 GGO 可见文本 ⇒ 想给"标题界面文本基线"做整机对照得先找到那类画面，不能拿 TITLE 充当。
- `lfWidth` / `lfEscapement` / `lfOrientation` 当前仍被忽略（既有平台偏差）；将来实现时必须进缓存键。
- synthetic 档不吃基线补偿（它的 `origin_y = black_box_y` 是格式算术契约，不是 FT 的 `bitmap_top`）。
