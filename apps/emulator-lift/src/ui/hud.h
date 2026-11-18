// emulator-lift · M0 内置 HUD（display list + 统一合成）
//
// ★ HUD 是 `overlay.md` D1 的**第一个 UI 插件**：它只产出 display list（`ui/element.h`），
//   宿主负责解析 / 栅格化 / 合成 —— ⛔ 不另开一条"HUD 专用"渲染路径。
// ★ 文本字形 = 宿主字体（`FontEngine` / FreeType）：支持中文 / 日文；⛔ 不用旧 5×7 点阵当生产字形
//   （旧点阵只留给 M2 的 `--ui-glyphs bitmap` 测试档）。
// ★ M0 的 HUD 内容 = 现有 7 行文案（`runtime/overlay.cpp:formatOverlayLines`）：mouse / script /
//   fps / frame / raster / present / worst。**行数 / 取不到的字段整行省略**仍由那份格式化负责；
//   这里只负责"每一行变成一个 Text 元素 + 一块不透明底"。
// ★ 纯布局 + 纯栅格化：SDL-free / GPU-free（唯一外部依赖 = FontEngine），可被 pure CTest 直接 include。
#pragma once

#include <string>
#include <vector>

#include "runtime/font.h"
#include "ui/element.h"

namespace lift {
class PresentSink;  // `runtime/present.h`（前向声明 ⇒ 本头不进 SDL / sink 实现）
}  // namespace lift

namespace lift::ui {

// M0 HUD 的参考系参数（1280×720 参考像素；实际像素 = round(... × s)）。
inline constexpr int kHudFontRefPx = 16;
inline constexpr int kHudPadRef = 8;      // 四边内边距（参考像素）
inline constexpr int kHudLineGapRef = 4;  // 行距（参考像素）

// 把行文本变成 display list：一块不透明底 + 每行一个 Text 元素。
// 宽度 / 行高用宿主字体在**参考字号**下度量（⛔ 不按字符数猜）。
// false = 宿主字体不可用（err 必须填；调用方按启动 / 运行期硬错处理，⛔ 不静默退回点阵）。
bool buildHud(UiBuilder& builder, FontEngine& font, const std::vector<std::string>& lines, std::string* err);

// 每帧：问 sink 要几何（`presentGeometry`）→ 算画布原点（`uiCanvasOrigin`）→ 解析 / 栅格化
// → `sink->presentOverlay(rgba,w,h,x,y)`。两条呈现路径由 sink 自己选（renderer / GPU 直出）。
// false = 字体不可用或 sink 合成失败（err 必须填）。
bool presentHud(PresentSink* sink, FontEngine& font, const std::vector<std::string>& lines, std::string* err);

}  // namespace lift::ui
