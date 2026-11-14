// emulator-lift · 画面 ↔ 窗口客户区的 letterbox 映射（**唯一一份**公式）
//
// ★ 为什么必须有这一份：present 侧用 `SDL_SetRenderLogicalPresentation(…, LETTERBOX)` 把画面
//   letterbox 到窗口里（`present_sdl.cpp` 的 `submitFrame` / `submitDirect`），而**反向**的
//   `Impl::mapMouse` 原先自己又写了一遍算式，还硬编码 1280×720；后缓冲却可以是 15 种适配器模式
//   之一（`runtime/d3d9.cpp` 的模式表 / DDraw 的 4 种）⇒ 非 1280×720 时鼠标映射系统性偏移。
//   现在两条路径共用这一份：鼠标事件入（`windowPixelToImage`）与宿主光标 warp 出
//   （`imageToWindowPixel`），公式不可能再各写一遍而静默漂移。
//
// ★ 纯函数、**SDL-free**：pure CTest（`tests/cursor_warp.cpp` 的 `cursor-map/*`）直接 include。
// ★ `image_w/image_h` = **被呈现的那一帧**的尺寸（⛔ 不是常数 1280×720）。窗口 sink 取自己最后一帧
//   （`tex_w/tex_h`，或直出路径的 `direct_w/direct_h`；0 = 还没有帧 ⇒ 退回 `WindowManager::kScreenW/H`）。
#pragma once

namespace lift {

struct LetterboxMap {
    // 画面像素 / 图像像素。0 = 没有可映射的几何（窗口 / 图像尺寸非正）。
    double scale = 0.0;
    // 画面左上角在窗口**客户区**里的偏移（居中后的黑边宽度）。
    double off_x = 0.0;
    double off_y = 0.0;
    // 被映射的图像尺寸（`windowPixelToImage` 的 clamp 上界 = image-1；也用于诊断 / 一致性检查）。
    int image_w = 0;
    int image_h = 0;
};

// window = 宿主窗口**客户区**尺寸；image = 被呈现的那一帧的尺寸。居中 letterbox：
//   scale = min(window_w/image_w, window_h/image_h)；dw/dh = image*scale；off = (window-dw)/2。
LetterboxMap letterboxFor(int window_w, int window_h, int image_w, int image_h);

// 画面像素 → 窗口客户区像素（四舍五入）。**⛔ 不 clamp**：Win32 的屏幕坐标可为负 / 越界
// （多显示器、窗口化），warp 侧必须原样线性映射；越界由系统自己处理。
// 返回 false = 几何不可用（非正尺寸），此时不写输出。
bool imageToWindowPixel(const LetterboxMap& map, int image_x, int image_y, int* window_x, int* window_y);

// 窗口客户区像素 → 画面像素（取整后 **clamp 到 0..image-1**：`mapMouse` 的既有口径 —— 光标在
// 黑边里 / 窗口外时落到最近的画面边缘，⛔ 不给负坐标）。
// 返回 false = 几何不可用（非正尺寸），此时不写输出。
bool windowPixelToImage(const LetterboxMap& map, double window_x, double window_y, int* image_x, int* image_y);

// 窗口客户区像素 → 画面像素的**整条**换算（= `letterboxFor(window_w, window_h, image_w, image_h)`
// 再 `windowPixelToImage`）。`present_sdl.cpp` 的 `mapMouse` 只调这一个入口。
// ★ 为什么要有这个组合：`mapMouse` 的 image 尺寸必须是**最近一次呈现的帧尺寸**
//   （`frameImageW()/frameImageH()`）。把"构造 map"与"用它反算"分成两个调用点后，任何一处被写回
//   常数（1280×720）都是静默的 —— 复审实测：把 `letterboxFor(…, frameImageW(), frameImageH())`
//   变异回 `(…,1280,720)` 时 **84/84 全绿**。现在调用点只有一个，且
//   `tests/cursor_warp.cpp` 的 `cursor-map/window-pixel-to-image` 直接钉住"image 尺寸真的改变结果"。
//   ★ 前提（也是已知风险）：窗口像素 ~ 画面像素的对应成立只在 **guest 客户区尺寸 == 帧尺寸** 时；
//   两者不等时这里把窗口像素当帧坐标系（`docs/03-emulator/design-notes.md` 的 L3-cursor-warp-01）。
// 返回 false = 几何不可用（非正尺寸），此时不写输出。
bool mapWindowPixelToImage(int window_w, int window_h, int image_w, int image_h, double window_x,
                           double window_y, int* image_x, int* image_y);

}  // namespace lift
