# M2a · QuickJS 宿主 + HUD 迁脚本（读数注入 game；删 raster/present 行）

- id: REQ-01M7Q0JHP0NZCS9V1AJG0G5659
- type: req
- status: done
- parent: REQ-01M7PE0D9MJA4REXQ1YJFVPHGF
- verify: apps/emulator-lift/tests/ui_hud_script.cpp#ui-hud-script/script-line-top-frame
- tags: [emulator-lift, ui, overlay, quickjs]

# M2a · QuickJS 宿主 + HUD 迁脚本（外置 JS / 读数注入 / 帧列表 / 80% 底）

- 设计真源：`docs/03-emulator/overlay.md` §3–§4、`docs/03-emulator/overlay-quickjs.md`。
- 本切片 = M2 的 QuickJS 宿主 + HUD 迁脚本；M1 的 layout / when / hit / clip 不依赖、不在本单内。

## 一句话

`--overlay` 的 HUD 改由**外置** `scripts/ui/hud.js` 经 QuickJS 同步产出 display list；读数
（frame / mouse / fps / script / worst）作为只读 getter 注入 `game`，另加按需方法
`game.local_frames()`；删掉 `OverlayState` / `OverlayStatus` / `formatOverlayLines` / 5×7 点阵 /
`buildHud`；**去掉 raster·present 两行**、底 **80% 半透明**、`script` 行显示**栈顶**脚本名
（用户 2026-11-18 裁决）。

## 已核（2026-11-18 / main / macOS arm64）

- `JS_Eval` 要求 `ctx->eval_internal`（只由 `JS_AddIntrinsicEval` 设）⇒ 必须加 Eval；
  砂箱的门是**加完再删** `eval` / `Function` / `Function.prototype.constructor`（只删全局不够）。
- 语法错 / 运行期错的 `message` + `stack` 都带 `<origin>:<line>:<col>`。
- 帧列表：40 槽 × `0x78`；活动 = `record+0x00 != 0` 且已提交；`parent=+0x38`；`top=slot==cur`；
  名字复用 `MarkerSampler`（`docs/03-emulator/ui-frames.md`，14 证 / 8 未证）。

## 范围

- `third_party/quickjs-ng`（**v0.18.0** / `d7215b0a…` / MIT / 原样 vendor + README + 本仓 CMake；0.10.1 的 win32 构建断与升级口径见 REQ-01M7QPHEF3WJ5W0RKAKFTKMFC1）。
- `src/ui/script.h` / `script_quickjs.{h,cpp}` / `quickjs_value.h`（RAII 值包装；公开头零 qjs 类型）。
- `src/ui/present_ui.{h,cpp}`；删 `hud.{h,cpp}` / `overlay.{h,cpp}`；`src/runtime/ui_readings.h`。
- `scripts/ui/{hud.js,ui.d.ts,README.md}`：**外置**默认 HUD（⛔ 不编进 exe）；解析
  `<exe_dir>/scripts/ui/hud.js` → `LIFT_UI_SCRIPT_DIR/hud.js` → 硬错。
- `src/runtime/engine_frames.{h,cpp}` + `game.local_frames()` + HUD 栈顶行；`present.h` /
  `present_sdl.{h,cpp}` / `machine.{h,cpp}` / `aot_host.{h,cpp}` / `aot_main.cpp` 接线。
- `src/runtime/d3d9_gpu.cpp`：GPU 直出混合绘制管线（`SDL_DrawGPUPrimitives` + SRC_ALPHA /
  ONE_MINUS_SRC_ALPHA）+ `directReadback` 同公式。
- 守卫：`ui-script-quickjs` / `ui-hud-script` / `ui-frames` / `overlay` / `overlay-gpu` /
  `aot-overlay`；变异 3 条（砂箱、HUD 行、`top`）。

## 判据

1. `ui-script-quickjs`：砂箱（eval/Function/`.constructor`/Math.random/Date/performance/import）、
   预算（死循环不可 catch、OOM 不崩）、`ui.*`、`game` getter、错误带 `origin:line:col`、缺入口失败。
2. `ui-hud-script`：外置 `hud.js` 的行序 = mouse / script / fps / frame / worst（**无 raster/present**）、
   底 alpha 204 / 字 255；`local_frames()` 里 `top=true` 的项压过 `game.script`（无 top 回退）。
3. `ui-frames`：活动槽 / `cur` 第一 / `top` / 空槽跳过 / `parent` 原样 / 6 种不可用 ⇒ err + 空表 /
   id→名字（含降级）/ `game.local_frames()` 的字段与 `undefined` 口径。
4. `overlay` / `overlay-gpu`：画布原点像素 = `blend(#101820, guest, 204)`（两路径同值，±1）、
   关掉逐像素恢复、非 16:9 偏移不变；`aot-overlay` 直出 (0,0) = `(13,19,26)`，`--dump-frame` 不变，
   stderr 无周期行。
5. ~~`--ui <语法错脚本>` ⇒ exit 2 + path:line:col~~（2026-11-25 S3 删除 `--ui`；脚本语法错现由 `--plugin` 的 `ui-script-quickjs/error-syntax` 钉住）。

## 读数（macOS arm64 / RelWithDebInfo）

- 全量 ctest **116/116**（4 条既有 SKIP）；`ui-script-quickjs` 91/0、`ui-hud-script` 59/0、
  `ui-frames` 357/0、`overlay` 3090/0、`overlay-gpu` 1106/0、`aot-overlay` 36/0。
- 真 boot：`local_frames()` len=8，`[0] slot=1 TITLE.BIN(id=21092,parent=0,top=true)`，无 why。
- 直出 `(0,0)=#262040` = `blend(#101820,#8040C0,204)`；BMP `(13,19,26)`；win32 未验证。

## 未做

- M1 布局 / when / hit / 校验表；M2 点击路由 / `--ui-input` / `--ui-strict` / 热重载；
  M3 的 `host.log` / `probe` / `global`·`local` 数据面。
- `local_frames()` 只给最小版（无 parent 链 / depth）；`+0x5C` / `+0x64..6C` / 三组表未取证；
  控制层 `sample.frames` 另单。
- 发行打包 `scripts/ui/`；`.d.ts` 漂移守卫；体积 / 首帧开销读数；win32 未跑
  （合入前补 `ctest -R '^(ui-script-quickjs|ui-frames|overlay|aot-overlay)$'`）。

## 补充（2026-11-18 晚）

- **高帧率 overlay 部分丢失**：纹理在尺寸变化时被立即 `Destroy/Release`（上一帧 draw 可能仍在飞）
  ⇒ grow-only 容量 + 退役列表（只在 shutdown 释放）+ 子矩形上传；`overlay/size-churn` 200 帧守卫。
- **脚本编辑器**：`scripts/ui/tsconfig.json`（`allowJs/checkJs/strict`）；`tsc -p` 绿，不进 typecheck。
