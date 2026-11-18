// emulator-lift · UI 脚本 → 窗口像素的**统一合成入口**（M0 / B）
//
// ★ 本头只有一件东西：`presentUi`。它把"这一帧的 display list"从 **脚本**（`ui/script.h`，
//   生产实现 = QuickJS）拿过来，交给宿主字体栅格化（`ui/raster.cpp`），再交给 sink 的
//   `presentOverlay`。⛔ 这里**不构造**任何 HUD 内容 —— 默认 HUD 是 `scripts/ui/hud.js`，
//   C++ 侧只负责把脚本**读进来**并交给 runtime（`present_sdl.cpp` 的 `loadUiScript`；
//   脚本是运行时读的外置文件，⛔ 不编进 exe）。
// ★ 这里**不产出行 / 面板尺寸**：那是脚本自己的事（`ui.measure` + `ui.rect`），C++ 侧只把
//   脚本给的 list 变成像素。
//   几何 / 画布原点 / 栅格化 / 合成四步逐字不变（`overlay.md` D12：不存在第二条绘制路径）。
// ★ 纯布局 + 纯栅格化：SDL-free / GPU-free（唯一外部依赖 = `FontEngine`），可被 pure CTest 直接 include。
#pragma once

#include <string>

#include "ui/script.h"

namespace lift {
class PresentSink;  // `runtime/present.h`（前向声明 ⇒ 本头不进 SDL / sink 实现）
}  // namespace lift

namespace lift::ui {

// 参考字号（1280×720 参考像素）：默认 HUD 用的字号，也是**宿主字体可用性检查**的锚点
// （`present_sdl.cpp` 的 `init` / `setOverlay` 用 `font.metrics(kUiFontRefPx, false, …)` 判字体在不在）。
// ★ 值必须与 `scripts/ui/hud.js` 的 `SIZE` 一致；脚本自己也要用 `ui.measure(…, SIZE)` 排版，
//   所以这条常量只是"启动期字体闸门用哪一档"的**一份**声明，⛔ 不是版式真源。
inline constexpr int kUiFontRefPx = 16;

// 每帧：问 sink 要几何（`presentGeometry`）→ 算画布原点（`uiCanvasOrigin`）→ 让脚本产出
// display list（`script.render`）→ 栅格化 → `sink->presentOverlay(rgba,w,h,x,y)`。
// 两条呈现路径由 sink 自己选（renderer / GPU 直出）。
// ★ 返回值：`false` = **脚本失败 / 栅格化失败 / 合成失败**（`err` 必须填）。
//   "这一帧没有几何 / 脚本产出空 list" 是**正常**情况 ⇒ `true`（⛔ 不当错误）。
bool presentUi(PresentSink* sink, FontEngine& font, UiScript& script, const UiFrameReadings& readings,
               std::string* err);

}  // namespace lift::ui
