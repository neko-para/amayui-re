# emulator-cxx 覆盖范围与显式限制

> 本文件只记录 emulator 已经**明确覆盖**的行为，以及**显式不做**的部分（宿主策略 / 平台边界 / 已知限制）。
> 引擎行为本身以 `data/spec/` 为准；本文件不写引擎语义，只写"这个模拟器如何落地、哪些明确不做"。
> 取证与逐条数值见 `docs/02-engine/ops-forensics-2026-10-title-option.md` §31–§38，
> 守卫见 `tests/core_selftest.cpp`，进度/待办见 `data/requirements/`（`pnpm tools requirements plan`）。

## 1. 字体

### 1.1 字体总是 Amayui CN（宿主替换）

- **总是使用 Amayui CN**（宿主替换）：`corpus/assets/fonts/Amayui-CN_cnjp.ttf`（CMake 的
  `EMULATOR_CXX_FONT_PATH`），粗体是同目录的 `-Bold.ttf`。★ **这条在 GDI 档同样生效**（见 §1.3）：
  GDI 档默认就按宿主族名 `Amayui CN` 取字、不读引擎的字面名。
- 引擎字体对象里的字面名（`0x1a5` 写入的名称）**一律忽略**。★ 实测本机数据的 `0x1a5` 写的是
  **「ＭＳ ゴシック」/「ＭＳ 明朝」**（不是 Amayui CN —— 译文脚本没有改这一格，所以宿主替换是必需的，
  否则简体字形要靠系统字体链接兜底）。`EMULATOR_CXX_GLYPH_GDI_FACE=engine` 是**显式对照档**：
  按引擎的字面名取字（纯真机字型，用来 A/B 对比），不是产品行为。
- 引擎用来量 ascent 的常量参考字体 "ＭＳ ゴシック"（`0x456D09`）**不加载**；它的量度只在
  `EMULATOR_CXX_ALIGN_REFERENCE=1` 时用来当「缓存 ascent」（见下条）。
- 引擎在重建字体时用参考字体量 ascent；emulator **固定改用渲染字体（Amayui）量**
  （`cached_ascent = current_ascent`，`src/core/text_raster.cpp`）。这是宿主策略，不是引擎语义；
  引擎的参考行为仍登记在 spec 的 `RES-RENDER-TEXT-ALIGN-REFERENCE`。
- **宿主基线补偿 +2px**：替换字体后墨迹整体偏低 —— Amayui 的 ascent 比引擎的参考字体大
  （FT 量到 20px 20 / 28px 28，参考字体 17 / 24），而引擎的"字形格顶"是按参考字体量的 ⇒ 多出来的
  那几像素全落在下方。所以画字前把基线再**抬 2px**（`EMULATOR_CXX_BASELINE_LIFT`，`0` = 关、可微调）。
  这是**宿主视觉策略**（⛔ 不是引擎语义；引擎没有这条）。补偿后与真机（= GDI 用引擎字面名那一档）比：
  **20px 完全一致，28px 低 1px**。数值与取证见 ops-forensics §42。
  ★ GDI 档若显式用引擎的字面名（`GLYPH_GDI_FACE=engine`，量度与字形同源）⇒ 补偿自动为 0。
- `EMULATOR_CXX_FONT` 环境变量只作调试覆盖；默认/产品行为就是 Amayui。

### 1.2 字形格式与合成

- 格式由引擎的 AA 配置决定：`set:EnableAntiFont` + `message:UseAntiFont`（+ `message:AntiFontLevel`）
  ⇒ 1-bit / GRAY4 / GRAY8。**这条对所有字形来源一致**（含下面的默认 FreeType 回退）—— 格式不是观感开关。
- 灰度档的级换算按引擎：GRAY4 是 **17 级**（`值 × 255 / 17` ⇒ 0,15,…,**240**）、GRAY8 是 **65 级**
  （`值 × 255 / 65` ⇒ 0…**251**）。⛔ 不是 2^n（16 / 64 ⇒ 满覆盖 255）。宿主的连续覆盖率先量化到
  17 / 65 级再换算，见 `src/core/text_alpha.h`。
- 两趟按引擎：模式 1 = 副色 (x+1, y+1) → 主色；模式 2 = 主色两遍；模式 3 = 径向副色。
- 合成用引擎规则：满覆盖覆写；否则 `outA = max(dstA, srcA)`、
  `outC = (srcC*srcA + dstC*(255-srcA))/255`（不是 alpha-over）。
- 每个码点只加载/栅格化一次，两趟共用同一 bitmap；advance / bearing 取原 glyph，偏移不改 advance。

### 1.3 字形来源策略

- **Windows + `EMULATOR_CXX_GLYPH_GDI=1`** ⇒ **真机路径**：字型 / hinting / 栅格化 / 灰度级数全部来自
  GDI 的 `GetGlyphOutline`（引擎本来就是这么要字形的），格式按 AA 配置、两趟照引擎。
  字面名**仍走 §1.1 的宿主替换**（默认族名 `Amayui CN`，私有挂载仓库里的 TTF）；
  `EMULATOR_CXX_GLYPH_GDI_FACE=engine` 才按引擎 `0x1a5` 那格取字（对照用），
  `= <族名>` 则用指定族名。这不是"另一种观感方案"，而是把引擎脚下的平台接成同一台机器上的同一个 GDI。
  实现在 `src/core/text_gdi.cpp`（只在 `_WIN32` 有实体）；默认关闭。
- **有离线 bake 数据**（`EMULATOR_CXX_GLYPH_ATLAS` 指向的文件存在）⇒ **完全按引擎**：格式看 AA 配置、
  两趟照引擎。当前 loader 仍是 **stub**（会打警告，仍用 FT 字形）；真实 GDI 字形（Windows 提供者 /
  离线 atlas）接进同一入口后绘制逻辑不变。
- **其它（默认）** ⇒ 只换"字形从哪来"（FreeType），**格式判据仍是上面那条引擎口径**：
  - **同色**（fake bold）⇒ **Bold 字体 + 单趟**（默认）；`EMULATOR_CXX_SAME_COLOR_MONO=1` 是对照开关
    （同色时强制 1 位 + 单趟）；
  - **异色**（真 shadow）⇒ **两趟**。
- 字形来源与粗体选择是宿主视觉策略；**格式（1 位 / GRAY4 / GRAY8 与 17 / 65 级换算）是引擎语义**，
  三档共用同一条判据 —— 否则"配置关掉抗锯齿"时我们会比真机亮一档。
- **绘制模式 3 的副色趟按引擎是"径向一圈"**（12 趟、位移落 3×3 邻域、只外扩 **1px**）——三档都一样，
  这不随字形来源变；见 ops-forensics §40。
- FT 与 GDI 的 1-bit 差约 1px，属平台栅格化（spec 的 `RES-RENDER-TEXT-GLYPH-RASTER` 明确 discard）；
  不追求逐像素对齐。GDI 档没有这条差（同一个平台栅格器）。

### 1.4 已覆盖的文本行为

- `0x204` 文本：对位模式 0/1、多行、颜色、绘制模式 1/2/3、按纹理矩形裁剪、缓存刷新；
- 标题 / 菜单 / CONFIG1（选项标签、滚动、配置界面文本）；
- 场景 / 背景 / UI 纹理与滚动（`0x1f7` / `0x1f8` / `0x1f9` 等路径按 spec）。

## 2. 显示

### 2.1 窗口

- 逻辑分辨率 **1280×720**（引擎设计分辨率）；窗口 `SDL_WINDOW_RESIZABLE`，场景会铺满交换链。
- 真机截图 1602×958 是引擎的虚拟全屏/显示缩放（客户区约 1602×903 ≈ 1.25×）；把 emulator 窗口拖到
  同尺寸即可对照，相对布局不变。

### 2.2 全屏：不支持

- **模拟器不做全屏**：没有 `SDL_WINDOW_FULLSCREEN`，也没有 `SDL_SetWindowFullscreen` / 全屏切换。
- 理由：窗口 + 交换链缩放已经覆盖"大画面"的需求；全屏会引入显示器模式/缩放策略，与"窗口可缩放"重复。
- 想接近真机观感时，把可缩放窗口拖到 1602×958（或显示器尺寸）即可。

## 2A. 玩家配置：根与保存闸

- **根 = 本地应用数据目录**（`%LOCALAPPDATA%`），⛔ 不是漫游（`%APPDATA%`）：与 spec「窗口与呈现」的
  配置根一条同源（`set:UseAppDataFolder` **恰为 1** 时才换根）。实现是纯函数 `core::player_config_root`
  （本地 → XDG 数据目录 → `~/.local/share` → 安装目录；**漫游候选只出现在诊断打印里**）。
- **保存闸**：spec 的 `CORE-CONFIG-EXIT-SAVE` 说"窗口关闭收尾会整体重写玩家文件"，但那**目标路径**在本仓
  是**外部只读数据**（真机那份 `SYS4REG.INI`，以及只读的游戏安装目录）⇒ 宿主侧加一道闸：
  **默认不写**；要写得显式指路（`--config <副本>`）或 `--config-save`，`--no-config-save` 强制不写。
  跳过时会打印 `config save skipped (host safety gate)`。
  ⛔ 这不是引擎语义：引擎确实会写；这是我们**不去改外部数据**的取舍（`AGENTS.md` §8）。

## 3. 显式不做 / 已知限制

- **全屏**（§2.2）。
- **任意字体**：只支持 Amayui CN（+Bold）；引擎字体对象的字面名、参考字体不生效。
- **逐像素 GDI 字形（默认档）**：FT 与 GDI 的 1px 差异按平台 discard；无 bake 时是近似。
  （想要真机字形就开 `EMULATOR_CXX_GLYPH_GDI=1`，见 §1.3。）
- **离线 bake 的真实字形**：loader 仍是 stub（`EMULATOR_CXX_GLYPH_ATLAS` 只做存在性探测）。
- **非 Windows 上的 GDI 档**：没有实体（`text_gdi.cpp` 在那里只剩返回 false 的桩），自动回退到 FT。
- **`0x205` 的格宽来源**：只实现"无缩放、`set:BlankExtentMode` 缺省"那一支（格宽 = A 支字号）；
  `set:BlankExtentMode = 1` 的"空白字符量度宽"与带字体缩放的档**未实现**（spec 已写，见
  `RES-RENDER-NUMTEXT-BLANK` / `RES-RENDER-NUMTEXT-METRIC`）。
- **`0x20e` 的门控支**：虚拟全屏类型 = 1 时"切到纹理槽 38 → 清 → 切回后缓冲 → 再清"**未建模**
  （模型没有显示记录的虚拟全屏类型格）。已实现的是非门控支：目标是纹理槽 ⇒ 清该表面并换内容版本；
  目标是后缓冲 ⇒ 没有可写表面（宿主每帧重建交换链）。
- **`0x205` 的运行期观察**：设置界面 game 页会执行它，但现有 CTest 只判"停在 20940 + 命中项数量"，
  **不判文本 / 落点**；它的行为证据目前只有 `core_selftest` 的像素级夹具（pure 档）。
- **打字机的"跳过"不置第 27 位**：引擎在抽干时同时置第 27 位（`0x409542`），本实现只清第 29 位 +
  抽干 —— 第 27 位在这个模型里没有服务者（置了会让主循环卡在门后），见 `REQ-01M60CFPK2X36ZCQD21M3E5G7E`。
- **打字机的"前台支"未建模**：追加调用在门判之前还有一条前置支（子系统返回非 0 ⇒ 置第 27 位 + 一个标志格），
  本实现不模拟那条支（同上：第 27 位没有服务者）。

## 4. 相关文档

- `README.md`（构建 / 运行 / 宿主策略）
- `../../docs/02-engine/ops-forensics-2026-10-title-option.md`（§31–§38 取证）
- `../../docs/02-engine/spec-audit-2026-10.md`（审计）
- `../../data/spec/`（引擎规范）
- `../../data/requirements/`（进度 / 待办）
