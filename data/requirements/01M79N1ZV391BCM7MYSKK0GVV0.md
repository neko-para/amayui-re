# 🐞S2 emulator-lift · CVINIT 角色名里的 U+E010 渲染失败（真机是空心心形 ♡）：字体 cmap / 码位路径

- id: REQ-01M79N1ZV391BCM7MYSKK0GVV0
- type: bug
- status: done
- parent: REQ-01M6H6G6DSRG1ZRD810Z0HTH8V
- verify: apps/emulator-lift/tests/gdi_font_fallback.cpp#shim/fallback-glyph
- repro: apps/emulator-lift/src/runtime/cp932_table.h#0xf050e010
- severity: S2
- tags: [emulator-lift, font, gdi, glyph]

## 现象

`CVINIT` 的一条角色名在模拟器里缺字：`dist/views/data/CVINIT.BIN.txt:251`
= `set-string (global-string 111b7) "ダイナマイト<U+E010>亜美"`（U+E010 是私用区码位）。
**真机**在同一位置渲染一个**空心心形（♡）**；用户确认字体本身没问题（真机用 AmayuiCN 能渲染）⇒ 疑点在我们的 shim 文本路径。

## 已核事实（2026-11-13）

- 该字符码位是 **U+E010**（读 UTF-8 视图逐码位确认，不是显示丢失）。
- `repo:apps/emulator-lift/src/runtime/cp932_table.h` 有 `0xf050e010` ⇒ 我们的 CP932 表把字节对 **0xF050 ↔ U+E010** 双向映射。
- 解析 `repo:corpus/assets/fonts/Amayui-CN_cnjp.ttf` 的 cmap：**U+E010 / U+F050 都是 glyph 0**，**U+2661（♡）是 glyph 1815**。
- shim 的字形入口 `FontEngine::glyph(cp, …)`（`repo:apps/emulator-lift/src/runtime/font.cpp`）直接 `FT_Load_Char(face, cp, …)`。
  ★ **实测更正**：`FT_Load_Char` 在 cmap 缺该码位时**不失败** —— `FT_Get_Char_Index` 返回 0、`FT_Load_Char` 仍返回 0
  并加载 glyph 0（`.notdef`，探针读数 bitmap=7×22、advance 832/26.6）。⇒ 我们今天会把 `.notdef` 当成功缓存并画出去。

## <待核>

1. 真机为什么能画：字体走 symbol/legacy cmap？Windows CP932 把 0xF050 映到 U+2661？引擎用 A 版 API 传字节对？还是字体是"映射字体"要靠自己的映射表？
2. 我们错在哪一步：码位错 / cmap 子表选错 / fallback 缺失。
3. 需要一条能打印"码位 → cmap 子表 + glyph id"的 trace（`font.cpp` 似乎已有 `cmap[%d] platformID=… glyphID=…`）。

## 判据（修好后）

- 该码位在宿主字体上能取到**非空字形**（black_box > 0 或 advance > 0），且 `FT_Load_Char` 不再失败；用 pure CTest 钉住（不依赖游戏资产）。
- 真机观感：该位置出现空心心形（人眼 + 截图 artifact）。
- 守卫必须能红：修前该用例红（glyph 0 / mask 空），修后绿。

## 复现（★ `dist/` 是 `.gitignore` 的生成视图，不能当锚）

视图只是缓存；真源 = 基线（游戏安装）+ patch。要重看这一行：
```bash
pnpm tools patch view --kind data --name '$1$CVINIT.BIN'   # 落 dist/views/data/$1$CVINIT.BIN.txt
sed -n '/global-string 111b7/p' 'dist/views/data/$1$CVINIT.BIN.txt'
```
`global-string 111b7` 的取值里第 6 个字符是 U+E010。本单的 `repro` 锚因此挂在**代码**上
（`cp932_table.h` 的 `0xf050e010`），不挂 `dist/`。

## 顺带要修的（与本单同根，可同批做）

- `glyph()` 的成功判据从"`FT_Load_Char` 返回值"改成"`FT_Get_Char_Index(cp) != 0`"；缺字形时**响亮记账**
  （首次 warn + 计数进摘要），⛔ 不把 `.notdef` 当成功。
- 守卫（pure，不需要游戏资产）：对 Amayui CN 的 CTest 断言"已知存在的码位 glyph≠0"与"U+E010 glyph=0 ⇒ 走缺字形分支而不是画 .notdef"。
  这条守卫与"真机为什么能画 ♡"是**两件事**，不要用它替代后者。
