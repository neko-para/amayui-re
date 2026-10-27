# emulator-cxx 覆盖范围与显式限制

> 本文件只记录 emulator 已经**明确覆盖**的行为，以及**显式不做**的部分（宿主策略 / 平台边界 / 已知限制）。
> 引擎行为本身以 `data/spec/` 为准；本文件不写引擎语义，只写"这个模拟器如何落地、哪些明确不做"。
> 取证与逐条数值见 `docs/02-engine/ops-forensics-2026-10-title-option.md` §31–§38，
> 守卫见 `tests/core_selftest.cpp`，进度/待办见 `data/requirements/`（`pnpm tools requirements plan`）。

## 1. 字体

### 1.1 字体总是 Amayui CN（宿主替换）

- **总是使用 Amayui CN**：`corpus/assets/fonts/Amayui-CN_cnjp.ttf`（CMake 的 `EMULATOR_CXX_FONT_PATH`），
  粗体是同目录的 `-Bold.ttf`。
- 引擎字体对象里的字面名（`0x1a5` 写入的名称）**一律忽略**；引擎用来量 ascent 的常量参考字体
  "ＭＳ ゴシック"（`0x456D09`）**不加载**。
- 引擎在重建字体时用参考字体量 ascent；emulator **改用渲染字体（Amayui）量**
  （`cached_ascent = current_ascent`，`src/core/text_raster.cpp`）。这是宿主策略，不是引擎语义；
  引擎的参考行为仍登记在 spec 的 `RES-RENDER-TEXT-ALIGN-REFERENCE`。
- `EMULATOR_CXX_FONT` 环境变量只作调试覆盖；默认/产品行为就是 Amayui。

### 1.2 字形格式与合成

- 格式由引擎的 AA 配置决定：`set:EnableAntiFont` + `message:UseAntiFont`（+ `message:AntiFontLevel`）
  ⇒ 1-bit / GRAY4 / GRAY8。
- 两趟按引擎：模式 1 = 副色 (x+1, y+1) → 主色；模式 2 = 主色两遍；模式 3 = 径向副色。
- 合成用引擎规则：满覆盖覆写；否则 `outA = max(dstA, srcA)`、
  `outC = (srcC*srcA + dstC*(255-srcA))/255`（不是 alpha-over）。
- 每个码点只加载/栅格化一次，两趟共用同一 bitmap；advance / bearing 取原 glyph，偏移不改 advance。

### 1.3 字形来源策略

- **有离线 bake 数据**（`EMULATOR_CXX_GLYPH_ATLAS` 指向的文件存在）⇒ **完全按引擎**：格式看 AA 配置、
  两趟照引擎。当前 loader 仍是 **stub**（会打警告，仍用 FT 字形）；真实 GDI 字形（Windows 提供者 /
  离线 atlas）接进同一入口后绘制逻辑不变。
- **无 bake** ⇒ 两个宿主方案（为了更好的观感，不是引擎语义）：
  - **同色**（fake bold）⇒ **Bold 字体 + 默认灰度 AA + 单趟**（默认）；`EMULATOR_CXX_SAME_COLOR_MONO=1` 回到
    normal + 1-bit mono + 单趟；
  - **异色**（真 shadow）⇒ 默认灰度 AA + **两趟**。
- 无 bake 的颜色选择是宿主视觉策略；bake 的格式是引擎语义。
- FT 与 GDI 的 1-bit 差约 1px，属平台栅格化（spec 的 `RES-RENDER-TEXT-GLYPH-RASTER` 明确 discard）；
  不追求逐像素对齐。

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

## 3. 显式不做 / 已知限制

- **全屏**（§2.2）。
- **任意字体**：只支持 Amayui CN（+Bold）；引擎字体对象的字面名、参考字体不生效。
- **逐像素 GDI 字形**：FT 与 GDI 的 1px 差异按平台 discard；无 bake 时是近似。
- **离线 bake 的真实字形**：loader 仍是 stub（`EMULATOR_CXX_GLYPH_ATLAS` 只做存在性探测）。

## 4. 相关文档

- `README.md`（构建 / 运行 / 宿主策略）
- `../../docs/02-engine/ops-forensics-2026-10-title-option.md`（§31–§38 取证）
- `../../docs/02-engine/spec-audit-2026-10.md`（审计）
- `../../data/spec/`（引擎规范）
- `../../data/requirements/`（进度 / 待办）
