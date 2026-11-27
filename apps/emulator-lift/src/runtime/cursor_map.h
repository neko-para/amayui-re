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

// `--input-plan` 的坐标参考系：人手在**标准画面**上量出来的 (x,y) 都属于它。
// ⛔ 这两个常数只在本文件（= 唯一公式来源）里被除；注入 / 控制层里不许出现 1280/720 的除法。
inline constexpr int kReferenceFrameW = 1280;
inline constexpr int kReferenceFrameH = 720;

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

// ★ S5：**overlay 的指针位置**（= HUD 命中 / hover 的唯一来源）整条映射。
//   guest **客户区** → `letterboxFor` + `imageToWindowPixel` → 宿主**窗口点** → 按
//   `pixel/logical` 换算到**窗口像素**（命中表 = resolved rect 的空间）→ `ClickRouter::hoverKey`。
//   ⛔ 不读 SDL 的 `GetMouseState`：输入是**设备**里的 guest 客户区坐标（`WindowManager::cursorClientPos`
//   同一个数），输出是窗口像素 —— 这样 `--present window --input-plan` 的 HUD hover 会跟随注入。
//   ★ 纯函数、SDL-free：pure CTest（`tests/overlay.cpp` 的 `overlay/pointer-map-*`）直接喂数，
//     **不依赖 `SDL_GetMouseFocus`**（宿主焦点判据留在调用方，是产品语义）。
//   ★ `logical_w/h` = `SDL_GetWindowSize`（窗口点）· `pixel_w/h` = `SDL_GetWindowSizeInPixels`
//     （窗口像素）· `image_w/h` = 最近一次呈现的**帧尺寸**（⛔ 不是常数 1280×720）。
//   命中判据与 `Impl::pointerWindowPixels` **同一个** DPR 口径（四舍五入）。
//   返回 false = 几何不可用（任一尺寸非正 / 输出指针为空），此时**不写**输出（⛔ 不猜）。
bool overlayPointerWindowPixels(int client_x, int client_y, int logical_w, int logical_h, int pixel_w,
                                int pixel_h, int image_w, int image_h, int* window_px, int* window_py);

// 1280×720 **参考系** → 目标**客户区**像素（各轴独立均匀缩放 + 四舍五入；⛔ 不 clamp）。
//   目标尺寸 = 主窗口的 `HostWindow::client_w/h`（headless 也有；guest 的 `CreateWindowExA` 填）。
//   0 / 非正 ⇒ 退回 1280×720（恒等映射 = "还没有窗口"时的确定性缺省）。
// ★ 方向与 `windowPixelToImage` 相反：那个是"窗口像素 → 画面像素"的**反**函数（带 clamp）；
//   这个是"参考系 → 目标空间"的**正**映射，用于把配方里的人手坐标投给 `input::*`。
// ★ 非 16:9 时按**各轴独立**缩放（拉伸参考系，不 letterbox、不 clamp 到边缘）——
//   ⚠ 这一条**未取证**（没有"非 1280×720 时点击命中"的观测）；已观测的 run 里目标是 1280×720
//   ⇒ 恒等。改这一条之前先补观测（`docs/03-emulator/input-plan.md` §2.8）。
// 返回 false = 输出指针为空。
bool referenceToClientPixel(int client_w, int client_h, int ref_x, int ref_y, int* x, int* y);

}  // namespace lift
