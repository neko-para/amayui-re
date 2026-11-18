// emulator-lift · UI display list 的**最终模型子集**（M0）
//
// ★ N1 裁决：M0 的 HUD 不是"一次性的 vector<rect/text>"，而是 `overlay.md` §3.1 最终模型的**子集**：
//   同一份 POD（`Element`）+ 同一个构造入口（`UiBuilder`）+ 同一个"顺序数组 → 窗口像素"的 resolve
//   （`ui/raster.cpp`）。M1 只**加能力**（layout / when / hit / clip），⛔ 不改这个模型 ——
//   否则 M0 的像素判据会在 M1 被推翻重写。
// ★ M0 只填子集：`Rect` / `Text` + 绝对 `at` + 显式 `size`（Text 的尺寸由宿主字体度量）。
//   ⛔ M0 不认 anchor / 百分比 / when / 嵌套 / 交互（那些字段还没进 POD）。
// ★ 纯数据 + 纯构造：**SDL-free / GPU-free**，可被 pure CTest 直接 include。
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace lift::ui {

// RGBA8（不透明底 + 亮字是 M0 HUD 的两种颜色；`ui/raster.cpp` 只写这两种值，便于像素判据）。
struct Color {
    std::uint8_t r = 0;
    std::uint8_t g = 0;
    std::uint8_t b = 0;
    std::uint8_t a = 255;
};

enum class ElementKind : std::uint8_t {
    Rect = 0,
    Text = 1,
};

// 一个元素 = 一条绘制指令（立即模式；没有跨帧控件树，`overlay.md` D4）。
struct Element {
    ElementKind kind = ElementKind::Rect;
    int id = 0;              // 文档序内唯一（M0 = 构造序；M1 起仍由宿主发号）
    int x = 0;               // 参考像素（1280×720 参考系的绝对 at）
    int y = 0;
    int w = 0;               // Rect 的参考像素宽（Text 忽略：宽度由字体度量）
    int h = 0;               // Rect 的参考像素高（Text 忽略）
    std::string text;        // Text 的内容（UTF-8；宿主字体逐码位取字）
    // ★ 命中键（`REQ-01M7QF01A00F9946DX0GPH23JQ` 切片 2 后半）：**非空** ⇒ 这条元素（只有 Rect）
    //   是一个命中区，命中区 = 它的 **resolved rect**（缩放 / 居中 / DPR 之后的窗口像素）。
    //   空 = 不可命中（`ui.rect` 不传 `key` 时的缺省）。⛔ 这不是 C++ 的按钮：外观 / 状态 / 主题
    //   在 `scripts/ui/kit.js`（需求单「已裁决」第 2 条）。
    std::string key;
    int font_size = 16;      // Text 的参考像素字号（D5：实际 = round(size*s)，按最终像素重绘）
    bool bold = false;       // Text 是否用粗体 face
    Color fill{0, 0, 0, 255};   // Rect 填充（Text 的底色由下层 Rect 提供）
    Color color{255, 255, 255, 255};  // Text 前景
    int z = 0;               // 绘制序：z 升序、同 z 按 seq 升序（稳定；M0 全 0）
    int seq = 0;             // 构造序（宿主发号；⛔ 不由调用方填）
};

// 唯一构造入口（M0 只有两个）：`rect` / `text`。`seq` 由 builder 单调发号。
class UiBuilder {
public:
    void clear() {
        elements_.clear();
        next_seq_ = 0;
    }

    Element& rect(int x, int y, int w, int h, const Color& fill, int z = 0) {
        Element e;
        e.kind = ElementKind::Rect;
        e.x = x;
        e.y = y;
        e.w = w;
        e.h = h;
        e.fill = fill;
        e.z = z;
        return push(std::move(e));
    }

    Element& text(int x, int y, std::string text, int font_size, const Color& color, bool bold = false, int z = 0) {
        Element e;
        e.kind = ElementKind::Text;
        e.x = x;
        e.y = y;
        e.text = std::move(text);
        e.font_size = font_size;
        e.color = color;
        e.bold = bold;
        e.z = z;
        return push(std::move(e));
    }

    const std::vector<Element>& elements() const { return elements_; }

private:
    Element& push(Element&& e) {
        e.seq = next_seq_++;
        e.id = e.seq;
        elements_.push_back(std::move(e));
        return elements_.back();
    }

    std::vector<Element> elements_;
    int next_seq_ = 0;
};

}  // namespace lift::ui
