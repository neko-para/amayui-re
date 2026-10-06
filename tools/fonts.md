# tools/fonts.mjs — cnjp 分发字体（**从基底 + 字典重建随包发的那份 TTF**）

> 一句话：引擎按 **cp932 码位**读文本，译文里写不进 cp932 的简体字会被字典换成一个"日文写法"存进 BIN；
> 于是字体必须把那个**日文码位**画成**简体字形** —— 这就是 cnjp。
> 现在它由 **一条命令 + 一份字典**从 Sarasa Gothic SC 基底重放出来，判据是
> **除 `head.modified`（时间戳）外与可信产物逐字节相同**。
> 字段 / 不变量 / 操作的真源是 `pnpm tools fonts describe`（本文件不重复 schema）。

## 1. 它取代了什么

旧仓的链是 `scripts/font_CN_JP.py`（来自上游 SExtractor 的 Font 工具，**Python + fontTools**）：
`SarasaGothicSC-Regular.ttf` →（反转 `res/subs_cn_jp.json` 后逐对写 cmap）→ `..._cnjp.ttf`，
再**另做一步**（改族名 + 对齐码页）得到 `Amayui-CN_cnjp.ttf`。那一步的脚本**没进仓**
（旧仓 `docs/font-build.md` §8 指向的文件已不存在），于是"怎么得到这份字体"只剩一句散文。

现在：**纯 Node**（`tools/lib/fonts.mjs`），基底与字典都是入库/可复现的输入，
产物与**随包发的可信产物**逐字节对齐 —— 那条散文变成了可执行的判据。

## 2. 链路（每一步都由实测反推，不是照抄）

| 步 | 做什么 | 实测依据（成品 vs 基底逐字节比） |
|---|---|---|
| ① | **只重写 `cmap`**：反转字典（`简→日写法` 变 `日写法→简`），对**除平台 1（Mac）外**的每个子表做 `cmap[日写法] = cmap[简体]`（**原地改** ⇒ 允许链式） | 其余 18 张表**逐字节相同**（`glyf`/`GPOS`/`hmtx` 都不动）；`cmap` 63,297 → 149,555 B |
| ② | **`name`**：id1/16 → 族名；id3/4 → `族名 + 子族名`（子族名是 `Regular` 时省略）；id6 → PS 名（同样按子族名决定后缀） | 中间产物 vs 成品：`cmap` 逐字节相同 ⇒ 这一步只动 `name` + `OS/2` |
| ③ | **`OS/2`**：`ulCodePageRange1/2` 按 **fontTools `recalcCodePageRanges`** 的算法重算（FontForge 直译） | 算出 `0x603e019f` / `0xdfd70000` —— 与成品**精确相同**；932 = range1 bit 17 |

★ **唯一非确定量是时间戳**：写盘会更新 `head.modified`，它连带改掉 `head` 的表校验和与文件级
`checkSumAdjustment`（实测共 11 字节，全在 `head` 及其目录项里）⇒ 判据把这三处归一化后再比。

## 3. 为什么不能随手换个字体库

`fonteditor-core` / `opentype.js` / `fontkit` / `font-flux-js` 都会把**认识的表解析成对象再序列化**：
未改动的表不保证逐字节原样（`opentype.js` 的 `toArrayBuffer` 会**重建 `post`**，官方 issue #529）。
我们的要求正相反：**18 张表一字不动**。所以走裸 sfnt 手术，并把 fontTools 的序列化口径逐条对齐 ——
不这么做，载荷全同、整文件照样逐字节不同（本工具就是这么一路调出来的）：

* `cmap` f4：`splitRange` 分段（**含"拆分值不值"的阈值 4/8**）＋ `idRangeOffset = 2*(len(endCode)+len(glyphIndexArray)-i)`；
* `cmap` 表：记录按平台/编码排序，**编译后字节相同的子表共享偏移**；f14 等未解析格式**按它自己声明的长度**带过
  （❌ 不能"从 offset 抄到表尾"：基底里 f14 排在最前，那样会把 f4/f12 又抄一遍）；
* `cmap` f12：`_IsInSameRun` 要求**码位与字形 id 都恰好 +1**（用"差值相等"会在空洞处并组，凭空造出中间码位）；
* `name`：记录按 `(platformID, platEncID, langID, nameID)` 排序，**相同字符串只存一份**；
* sfnt：**物理顺序**按 fontTools 的 `TTFTableOrder`（不在表里的按 tag 排序垫后、DSIG 最后），
  **目录记录**一律按 tag 排序 —— 两套顺序是两件事，都要照做。

## 4. 怎么跑

```bash
pnpm tools fonts describe        # 自描述：数据 / 三步变换 / 判据 / 序列化口径
pnpm tools fonts build           # 构建（缺省 dry-run：只报会写什么 + 统计）
pnpm tools fonts build --write   # 落到 dist/fonts/Amayui-CN_cnjp.ttf
pnpm tools fonts build --bold --write
pnpm tools fonts verify          # ★ 判据：与 corpus 里的可信产物比（归一化时间戳后逐字节）
pnpm tools fonts verify --bold --json
```

## 5. 边界（❌ 与 ★）

* ★ **前置**：基底 TTF 是 **7z 解压产物**（`corpus/assets/fonts/SarasaGothicSC/*.ttf`，gitignore，本地件）。
  仓库里没有 7z 依赖 ⇒ 这一步**记为前置**，工具只报"基底不在"；同一份 7z 也可从
  `corpus/assets/fonts/SarasaGothicSC-TTF-1.0.40.7z` 重新解出。
* ❌ **不去覆盖 `corpus/assets/fonts/Amayui-CN_cnjp*.ttf`**：那是校验基准（也是随包发的件）。
  字典改了要换基准，是一次**显式的再烘**（换完要同步 `corpus/assets.json` 里的指纹）——
  不是构建的副作用。工具因此只写 `dist/fonts/`。
* ❌ **不改字形**：`glyf`/`loca`/`GPOS`/`GSUB`/`hmtx`/… 全部逐字节搬运 ⇒ **基底换版本时产物必须重烘**，
  判据会立刻红（这正是它该有的行为）。
* 守卫：`tools/test/fonts.test.mjs`（含"改一个字节必须红"的自检；基底/基准不在场时按本仓惯例直接 return）。
