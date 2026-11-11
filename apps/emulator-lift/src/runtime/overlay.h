// emulator-lift · 窗口调试叠层（`--overlay`）
//
// ★ 纯逻辑、**零 SDL 类型**：字体 + 行格式化 + 一块 RGBA8 矩形的绘制。
//   窗口 sink（`present_sdl.cpp`）只负责把这里算出来的图像搬到窗口像素上；
//   两条呈现路径（GPU 直出 / SDL_Renderer）共用**同一份**行与图像（⛔ 不要各写一份格式化）。
// ★ 纪律：叠层只存在于**宿主送显的那一份像素**里 —— ⛔ 不写 guest 内存。
//   ✅ 只在**窗口**与 `--present-screenshot`（窗口像素）里可见；
//   ⛔ `--interaction-fail-dump` / `--dump-frame` 是 **guest 帧**（`arena.dumpBmp`），
//   不含叠层 —— 这与"guest 像素 / 整帧 hash 零改动"是同一条口径（⛔ 不要为了快照可见去写 guest）。
// ★ 默认关：只有 `--overlay`（且 `--present window`）才会被调用。
//
// 布局（固定，不提供配置）：
//   1 px 内边距 + 5×7 字形 + 1 px 字距 / 行距；宽度 = 最长行，高度 = 行数 × 行高。
//   底 = 不透明深色（`kOverlayBg`），字 = 亮色（`kOverlayFg`）—— 只覆盖这一块矩形。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace lift {

// 5×7 位图字体：每字符 5 列 × 7 行，1 bit/像素。
inline constexpr int kOverlayGlyphW = 5;
inline constexpr int kOverlayGlyphH = 7;
inline constexpr int kOverlayPad = 1;                            // 四边内边距（px）
inline constexpr int kOverlayAdvance = kOverlayGlyphW + 1;       // 字距 1 px
inline constexpr int kOverlayLineH = kOverlayGlyphH + 1;         // 行距 1 px

// 颜色：深底 + 亮字（RGBA8，不透明）。★ 底色不是纯黑：让"叠层真的画上去了"可以在
// 窗口像素里用**精确颜色**判出来（`--present-screenshot` / 捕获测试），不靠肉眼看。
inline constexpr std::uint8_t kOverlayBg[4] = {16, 24, 32, 255};      // #101820
inline constexpr std::uint8_t kOverlayFg[4] = {235, 255, 235, 255};   // #EBFFEB

// 叠层要显示的内容。每个字段都带"拿得到吗"的开关：拿不到的**整行省略**（⛔ 不编数字）。
struct OverlayState {
    bool mouse_available = false;  // WindowManager 有主窗口时才拿得到（guest 客户区坐标）
    int mouse_x = 0;
    int mouse_y = 0;
    bool fps_available = false;    // 至少 Present 过一次才有
    double fps = 0.0;              // 最近 1 s 的 Present 次数（滚动窗口）
    bool frame_available = false;  // Present 计数
    std::uint64_t frame = 0;
    const char* raster = nullptr;   // "cpu" / "gpu"；nullptr ⇒ 省略
    const char* present = nullptr;  // "direct" / "renderer"；nullptr ⇒ 省略
};

// 纯函数：给定 state ⇒ 确定的行（顺序固定：mouse / fps / frame / raster / present）。
// 行内只有 ASCII；未覆盖的字符在渲染时按空格处理（`overlayGlyph` 返回 false）。
std::vector<std::string> formatOverlayLines(const OverlayState& state);

// 该字符的 7 行位图（`rows[i]` 的低 5 位有效，bit4 = 最左列）。
// 返回 false = 字体没覆盖这个字符（调用方按空格画，⛔ 不猜字形）。
bool overlayGlyph(char ch, std::uint8_t rows[kOverlayGlyphH]);

struct OverlaySize {
    int w = 0;
    int h = 0;
};
// 这些行需要多大的矩形（空行集 ⇒ {0,0}）。
OverlaySize overlaySize(const std::vector<std::string>& lines);

// 把叠层画进一块 RGBA8 缓冲：`rgba` 指向矩形左上角，`stride` = 一行字节数。
// ★ 只写 (w, h) 这块矩形：每个像素都写成不透明的底或字（矩形外**一个字节都不动**）；
//   内容超出矩形时按矩形裁剪（⛔ 不越界）。`w <= 0 || h <= 0 || rgba == nullptr` ⇒ 什么都不做。
void renderOverlay(std::uint8_t* rgba, int stride, int w, int h, const std::vector<std::string>& lines);

// 便捷：按 `overlaySize()` 分配一块 RGBA8 图像并画好（窗口路径上传 GPU / 纹理用）。
std::vector<std::uint8_t> renderOverlayImage(const std::vector<std::string>& lines, int* w, int* h);

}  // namespace lift
