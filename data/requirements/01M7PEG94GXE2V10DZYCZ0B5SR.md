# M0 · 接线与统一合成（AOT --overlay + presentOverlay + 两条路径窗口像素）

- id: REQ-01M7PEG94GXE2V10DZYCZ0B5SR
- type: req
- status: doing
- parent: REQ-01M7PE0D9MJA4REXQ1YJFVPHGF
- tags: [emulator-lift, ui, overlay, m0]

## 一句话

按 `docs/03-emulator/overlay.md` §4 M0：在 AOT 臂接上 `--overlay`，把两条呈现路径统一到
`PresentSink::presentOverlay(rgba,w,h,x,y)` + `presentGeometry`；HUD 内容先用**最终模型子集**的
C++ display list（无脚本、无输入）；两条路径都按窗口像素合成，`--dump-frame` / `arena.hash` 不变。

## 已裁决（2026-11-18，父级；依据 `.tmp/overlay-review/verdict.md`）

| # | 裁决 |
|---|---|
| B1 | SDL_Renderer 路径：叠层那一次 draw 前后临时 `SDL_SetRenderLogicalPresentation(renderer,0,0,DISABLED)`，画完恢复 `LETTERBOX`；⛔ 不做逆缩放。若实测有副作用，M0 单里**显式**降级判据（原点 ±1 px + 采样中心色）并记录，⛔ 不静默降级。 |
| B2 | M0 HUD 走 D5 的 1280×720 参考系；判据 2 改为 **bbox 原点 == 画布原点**（= 画面矩形原点 + `((pic_w−1280s)/2,(pic_h−720s)/2)`），不是画面矩形原点。16:9 用例（如 640×360）+ 一条非 16:9 居中用例。 |
| N1 | display list 必须是最终模型子集：`src/ui/element.h` POD + `UiBuilder` + 只做绝对 `at`/`size` 的最小 resolve；⛔ 一次性 `vector<rect/text>`。M1 只加能力，不改模型。 |
| U1 | **必须支持中文**：生产文本走宿主字体（`FontEngine` / FreeType，`LIFT_HOST_FONT_PATH`），按最终像素重绘；⛔ 用旧 5×7 点阵当生产字形。5×7 只能作 `--ui-glyphs bitmap` 显式测试档（M2 接参数；M0 不做生产字形）；字体缺失 ⇒ 启动硬错（R5），⛔ 静默退回点阵。 |

## 判据（真源 overlay.md §4 M0；以下为可挂锚的形式）

1. `--present window --present-hidden --overlay --max-frames 2` ⇒ exit 0（AOT 臂第一次有叠层）。
2. 截图：叠层 bbox 原点 == **画布原点**；颜色精确值命中；16:9 与非 16:9 各一条（非 16:9 断言居中偏移）。
2b. 文本：宿主字体栅格（⛔ 点阵生产路径）；中文串（如 `天結`）经宿主字体得到非空覆盖率；字体缺失 ⇒ 启动硬错。
3. 关掉 `--overlay` ⇒ 窗口像素逐位等于“没有 UI”。
4. `--dump-frame` 逐字节不变、`arena.hash` 不变。
5. 默认档整轮 stderr 无 `[ui]` / `[overlay]` 周期行。
6. 两条呈现路径都过；直出无 GPU ⇒ SKIP 77。

## 落点

- `src/runtime/present.h`：`PresentSink::presentOverlay` / `presentGeometry`（默认 no-op）。
- `src/runtime/present_sdl.{h,cpp}`：renderer 路径的 1:1 合成 + `--overlay` 接线；窗口像素 / 画布原点。
- `src/runtime/d3d9_gpu.cpp`：直出 + 回读合成统一到同一个 `presentOverlay` 入口。
- `src/runtime/present_flags.h`：`kWindowFlagsAot` 加 `--overlay`。
- `src/aot/aot_main.cpp`：AOT `--overlay` 解析 / 接线。
- `src/ui/`：`element.h` + `UiBuilder` + 最小 resolve（M1 的模型子集）+ 宿主字体文本栅格（`FontEngine` / FreeType；⛔ 5×7 生产路径）。
- `tests/`：`ui_canvas`（纯函数 `uiCanvasOrigin`）、`overlay` / `present_window` 的 1:1 原点 / 颜色、AOT 子进程截图（assets 档）。
- `tools/mutate-check.mjs`：一条代表变异（origin 归零 / 画布偏移丢掉），实测红。

## 边界

- ⛔ 不开 `--ui` 文件入口、不接 QuickJS、不接输入 router（M2）。
- ⛔ 不写 guest；`--present-screenshot` 含 UI，`--dump-frame` / `arena.hash` / 控制层 `screenshot` 不含。
- ⛔ 不保留第二套渲染器；旧的 `formatOverlayLines` 等 M2 再删，M0 只走统一合成路径。
- ⛔ 不把 5×7 点阵当生产字体；`--ui-glyphs bitmap` 是 M2 的显式测试档。

## 未做

- M1 的布局 / when / 命中 / clip；M2 的 QuickJS 适配器与点击 router；M3 的 `script/pos` 数据面；M4 图片与热重载。
- 复审列出的非阻塞项（N2–N11）按各自里程碑收，不塞进 M0。
