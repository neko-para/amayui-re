#include "runtime/message_box.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "runtime/audio.h"
#include "runtime/clock.h"
#include "runtime/font.h"
#include "runtime/guest_token.h"
#include "runtime/host_window.h"
#include "runtime/input.h"
#include "runtime/log.h"
#include "runtime/machine.h"
#include "runtime/present.h"
#include "runtime/x86.h"

namespace lift {
namespace {

// ---- Win32 返回值（MessageBox 的 uType → 按钮集）----
constexpr std::uint32_t kIdOk = 1;
constexpr std::uint32_t kIdCancel = 2;
constexpr std::uint32_t kIdAbort = 3;
constexpr std::uint32_t kIdRetry = 4;
constexpr std::uint32_t kIdIgnore = 5;
constexpr std::uint32_t kIdYes = 6;
constexpr std::uint32_t kIdNo = 7;       // ★ MB_YESNO 的默认 / Esc / X（硬要求；`0x4BA82B cmp eax,6`）
constexpr std::uint32_t kIdTryAgain = 10;
constexpr std::uint32_t kIdContinue = 11;

// ---- 合成颜色（RGBA8；只用于宿主送显的那一份像素）----
constexpr std::uint8_t kBootBase = 32;   // 没有任何末帧时的纯色底 #202020
constexpr int kDimAlpha = 160;           // 末帧变暗的 α（0..255）；余量 = 255-160 = 95
inline constexpr std::uint8_t kPanelBg[3] = {226, 226, 230};
inline constexpr std::uint8_t kPanelBorder[3] = {72, 76, 88};
inline constexpr std::uint8_t kTitleFg[3] = {28, 30, 38};
inline constexpr std::uint8_t kTextFg[3] = {36, 38, 46};
inline constexpr std::uint8_t kButtonBg[3] = {242, 242, 246};
inline constexpr std::uint8_t kButtonHoverBg[3] = {252, 252, 255};
inline constexpr std::uint8_t kButtonDownBg[3] = {212, 214, 222};
inline constexpr std::uint8_t kButtonBorder[3] = {96, 100, 112};
inline constexpr std::uint8_t kAccent[3] = {40, 110, 220};

// 调用账（见 message_box.h 的那一段；只由 shim 写、只由运行摘要读）。
// ★ 用原子：feeder 线程也会调 shim（虽已先判线程、不进模态），但**计数**仍可能与主线程并发写；
//   ⛔ 不为这几条计数让一条防御路径变成数据竞争（UB）。
std::atomic<std::uint64_t> g_messagebox_calls{0};
std::atomic<std::uint32_t> g_messagebox_last_type{0};
std::atomic<std::uint32_t> g_messagebox_last_result{0};
std::atomic<bool> g_modal_in_flight{false};

struct InFlightGuard {
    InFlightGuard() { g_modal_in_flight.store(true, std::memory_order_relaxed); }
    ~InFlightGuard() { g_modal_in_flight.store(false, std::memory_order_relaxed); }
    InFlightGuard(const InFlightGuard&) = delete;
    InFlightGuard& operator=(const InFlightGuard&) = delete;
};

// =======================================================================================
// UTF-8 ⇄ 码点（只给正文折行 / 字形合成用；与本仓的 `wideToUtf8` 不冲突：那是宿主编码层）
// =======================================================================================
std::vector<char32_t> decodeUtf8(const std::string& s) {
    std::vector<char32_t> out;
    out.reserve(s.size());
    std::size_t i = 0;
    while (i < s.size()) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        char32_t cp = 0;
        int n = 0;
        if (c < 0x80u) {
            cp = c;
            n = 1;
        } else if ((c & 0xE0u) == 0xC0u) {
            cp = c & 0x1Fu;
            n = 2;
        } else if ((c & 0xF0u) == 0xE0u) {
            cp = c & 0x0Fu;
            n = 3;
        } else if ((c & 0xF8u) == 0xF0u) {
            cp = c & 0x07u;
            n = 4;
        } else {
            cp = 0xFFFDu;
            n = 1;
        }
        if (i + static_cast<std::size_t>(n) > s.size()) {
            out.push_back(0xFFFDu);
            break;
        }
        bool ok = true;
        for (int k = 1; k < n; ++k) {
            const unsigned char cc = static_cast<unsigned char>(s[i + static_cast<std::size_t>(k)]);
            if ((cc & 0xC0u) != 0x80u) {
                ok = false;
                break;
            }
            cp = (cp << 6) | (cc & 0x3Fu);
        }
        if (!ok) {
            out.push_back(0xFFFDu);
            ++i;
        } else {
            out.push_back(cp);
            i += static_cast<std::size_t>(n);
        }
    }
    return out;
}

void appendUtf8(std::string* out, char32_t cp) {
    if (cp < 0x80u) {
        out->push_back(static_cast<char>(cp));
    } else if (cp < 0x800u) {
        out->push_back(static_cast<char>(0xC0u | (cp >> 6)));
        out->push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    } else if (cp < 0x10000u) {
        out->push_back(static_cast<char>(0xE0u | (cp >> 12)));
        out->push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
        out->push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    } else {
        out->push_back(static_cast<char>(0xF0u | (cp >> 18)));
        out->push_back(static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu)));
        out->push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
        out->push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    }
}

int cellWidth(char32_t cp) { return cp < 0x80u ? 1 : 2; }

// 把正文按 **逻辑行**（\r\n / \n / \r）切开，再按字符格折行。⛔ 不改变内容顺序。
std::vector<std::string> splitAndWrap(const std::string& text) {
    std::vector<std::string> lines;
    std::vector<char32_t> line;
    bool prev_cr = false;
    auto flushLine = [&]() {
        if (line.empty()) {
            lines.emplace_back();
            return;
        }
        std::string cur;
        int cells = 0;
        for (const char32_t cp : line) {
            const int cw = cellWidth(cp);
            if (cells + cw > kModalTextCells && !cur.empty()) {
                lines.push_back(cur);
                cur.clear();
                cells = 0;
            }
            appendUtf8(&cur, cp);
            cells += cw;
        }
        lines.push_back(cur);
    };
    for (const char32_t cp : decodeUtf8(text)) {
        if (cp == U'\r') {
            flushLine();
            line.clear();
            // `\r\n` 是**一个**换行（否则每个 Windows 换行都会多出一行空行）。
            prev_cr = true;
        } else if (cp == U'\n') {
            if (!prev_cr) {
                flushLine();
                line.clear();
            }
            prev_cr = false;
        } else {
            prev_cr = false;
            line.push_back(cp);
        }
    }
    flushLine();
    // 去掉尾部空行（引擎正文常以 \r\n 收尾；留一行空行只会把面板撑高）
    while (!lines.empty() && lines.back().empty()) lines.pop_back();
    return lines;
}

// =======================================================================================
// 几何
// =======================================================================================
ModalSpec buildSpec(std::uint32_t u_type, bool* unknown, bool* defbutton_overridden) {
    ModalSpec spec;
    const std::uint32_t low = u_type & 0x0Fu;
    auto add = [&spec](std::uint32_t id, const char* label) {
        ModalButton b;
        b.id = id;
        b.label = label;
        spec.buttons.push_back(b);
    };
    spec.unknown_type = false;
    switch (low) {
        case 0x0:  // MB_OK
            add(kIdOk, "OK");
            spec.esc_result = kIdOk;
            spec.close_result = kIdOk;
            break;
        case 0x1:  // MB_OKCANCEL
            add(kIdOk, "OK");
            add(kIdCancel, "Cancel");
            spec.esc_result = kIdCancel;
            spec.close_result = kIdCancel;
            break;
        case 0x2:  // MB_ABORTRETRYIGNORE
            add(kIdAbort, "Abort");
            add(kIdRetry, "Retry");
            add(kIdIgnore, "Ignore");
            // ⚠ 偏差：Win32 没有 Cancel ⇒ Esc/X 无效；我们没有 X 就出不去 ⇒ 取默认键（Abort）。
            spec.esc_result = kIdAbort;
            spec.close_result = kIdAbort;
            break;
        case 0x3:  // MB_YESNOCANCEL
            add(kIdYes, "Yes");
            add(kIdNo, "No");
            add(kIdCancel, "Cancel");
            spec.esc_result = kIdCancel;
            spec.close_result = kIdCancel;
            break;
        case 0x4:  // MB_YESNO（★ 默认键强制 No，见文件头的硬要求）
            add(kIdYes, "Yes");
            add(kIdNo, "No");
            spec.esc_result = kIdNo;
            spec.close_result = kIdNo;
            break;
        case 0x5:  // MB_RETRYCANCEL
            add(kIdRetry, "Retry");
            add(kIdCancel, "Cancel");
            spec.esc_result = kIdCancel;
            spec.close_result = kIdCancel;
            break;
        case 0x6:  // MB_CANCELTRYCONTINUE
            add(kIdCancel, "Cancel");
            add(kIdTryAgain, "Try Again");
            add(kIdContinue, "Continue");
            spec.esc_result = kIdCancel;
            spec.close_result = kIdCancel;
            break;
        default:
            // ⛔ 不静默：按 MB_OK 处理，调用方据 `unknown_type` 打一行 warn。
            add(kIdOk, "OK");
            spec.esc_result = kIdOk;
            spec.close_result = kIdOk;
            spec.unknown_type = true;
            break;
    }
    // MB_DEFBUTTONn：0x100 = DEFBUTTON2（下标 1）… 0x300 = DEFBUTTON4（下标 3）。
    // ⛔ MB_DEFBUTTON1 的值 = 0，与"没写"不可区分 ⇒ 只有非 0 才算"显式给了"。
    const unsigned defbutton = (u_type >> 8) & 0x0Fu;
    std::size_t index = 0;
    if (defbutton != 0 && !spec.buttons.empty()) {
        index = defbutton < spec.buttons.size() ? defbutton : spec.buttons.size() - 1;
    }
    if (spec.buttons.empty()) {
        spec.default_result = kIdOk;
        *unknown = spec.unknown_type;
        *defbutton_overridden = false;
        return spec;
    }
    if (low == 0x4) {
        // MB_YESNO：**永远 IDNO**。显式给了 DEFBUTTON2/3/4 也忽略（它们指向 No 或越界，
        // 唯一的"指向 Yes"是 DEFBUTTON1，而它的值与"没写"不可区分）⇒ 记一行 Debug 说明。
        index = spec.buttons.size() > 1 ? 1 : 0;
        if (defbutton != 0) spec.defbutton_overridden = true;
    }
    for (std::size_t i = 0; i < spec.buttons.size(); ++i) spec.buttons[i].is_default = (i == index);
    spec.default_result = spec.buttons[index].id;
    *unknown = spec.unknown_type;
    *defbutton_overridden = spec.defbutton_overridden;
    return spec;
}

// =======================================================================================
// 合成（CPU，一整幅不透明 RGBA8）
// =======================================================================================
struct Rgb {
    std::uint8_t r = 0;
    std::uint8_t g = 0;
    std::uint8_t b = 0;
};

Rgb rgb(const std::uint8_t (&c)[3]) { return Rgb{c[0], c[1], c[2]}; }

void setPixel(std::vector<std::uint8_t>& img, int w, int h, int x, int y, Rgb c) {
    if (x < 0 || y < 0 || x >= w || y >= h) return;
    std::uint8_t* p = img.data() + (static_cast<std::size_t>(y) * static_cast<std::size_t>(w) +
                                    static_cast<std::size_t>(x)) * 4u;
    p[0] = c.r;
    p[1] = c.g;
    p[2] = c.b;
    p[3] = 255;
}

void fillRect(std::vector<std::uint8_t>& img, int w, int h, const ModalRect& r, Rgb c) {
    if (r.empty()) return;
    for (int y = r.y; y < r.y + r.h; ++y)
        for (int x = r.x; x < r.x + r.w; ++x) setPixel(img, w, h, x, y, c);
}

void strokeRect(std::vector<std::uint8_t>& img, int w, int h, const ModalRect& r, int thickness, Rgb c) {
    if (r.empty()) return;
    for (int t = 0; t < thickness; ++t) {
        const ModalRect edge{r.x + t, r.y + t, r.w - 2 * t, r.h - 2 * t};
        if (edge.w <= 0 || edge.h <= 0) break;
        for (int x = edge.x; x < edge.x + edge.w; ++x) {
            setPixel(img, w, h, x, edge.y, c);
            setPixel(img, w, h, x, edge.y + edge.h - 1, c);
        }
        for (int y = edge.y; y < edge.y + edge.h; ++y) {
            setPixel(img, w, h, edge.x, y, c);
            setPixel(img, w, h, edge.x + edge.w - 1, y, c);
        }
    }
}

void blendPixel(std::vector<std::uint8_t>& img, int w, int h, int x, int y, Rgb c, int coverage) {
    if (coverage <= 0) return;
    if (coverage > 255) coverage = 255;
    if (x < 0 || y < 0 || x >= w || y >= h) return;
    std::uint8_t* p = img.data() + (static_cast<std::size_t>(y) * static_cast<std::size_t>(w) +
                                    static_cast<std::size_t>(x)) * 4u;
    const auto mix = [coverage](std::uint8_t dst, std::uint8_t src) {
        return static_cast<std::uint8_t>((static_cast<int>(src) * coverage +
                                          static_cast<int>(dst) * (255 - coverage)) /
                                         255);
    };
    p[0] = mix(p[0], c.r);
    p[1] = mix(p[1], c.g);
    p[2] = mix(p[2], c.b);
    p[3] = 255;
}

// 画一行 UTF-8 文本（基线 = top_y + ascent，与 `gdiDrawText` 同一合成约定）。
// 返回 false = **没有字体后端**（`glyph()` 失败）——面板/按钮已画，调用方负责 warn。
bool drawText(std::vector<std::uint8_t>& img, int w, int h, FontEngine* fonts, const std::string& utf8,
              int pixel_size, int x, int top_y, Rgb color) {
    if (fonts == nullptr) return false;
    HostTextMetrics tm;
    if (!fonts->metrics(pixel_size, false, &tm)) return false;
    const int baseline = top_y + tm.ascent;
    int pen = x;
    for (const char32_t cp : decodeUtf8(utf8)) {
        GlyphMask mask;
        if (!fonts->glyph(cp, pixel_size, false, &mask)) return false;
        for (int row = 0; row < mask.black_box_y; ++row) {
            for (int col = 0; col < mask.black_box_x; ++col) {
                const int coverage =
                    mask.coverage[static_cast<std::size_t>(row) * mask.black_box_x + col];
                if (coverage <= 0) continue;
                blendPixel(img, w, h, pen + mask.origin_x + col, baseline - mask.origin_y + row, color,
                           coverage);
            }
        }
        pen += mask.advance;
    }
    return true;
}

}  // namespace

// =======================================================================================
// 纯函数
// =======================================================================================
ModalSpec modalSpecFromUType(std::uint32_t u_type) {
    bool unknown = false;
    bool overridden = false;
    ModalSpec spec = buildSpec(u_type, &unknown, &overridden);
    spec.unknown_type = unknown;
    spec.defbutton_overridden = overridden;
    return spec;
}

ModalSpec makeMessageBoxSpec(std::uint32_t u_type, const std::string& caption_utf8,
                             const std::string& text_utf8) {
    ModalSpec spec = modalSpecFromUType(u_type);
    spec.caption = caption_utf8;
    spec.lines = splitAndWrap(text_utf8);
    return spec;
}

std::uint32_t modalDefaultResult(const ModalSpec& spec) {
    if (spec.buttons.empty()) return spec.default_result;
    for (const ModalButton& b : spec.buttons)
        if (b.is_default) return b.id;
    return spec.default_result;
}

ModalLayout modalLayout(const ModalSpec& spec, int screen_w, int screen_h) {
    ModalLayout layout;
    layout.screen_w = screen_w > 0 ? screen_w : 1280;
    layout.screen_h = screen_h > 0 ? screen_h : 720;
    const int pad = kModalPad;
    const int panel_h_min = 2 * pad + kModalButtonH;
    int panel_w = kModalPanelW;
    if (panel_w > layout.screen_w - 2 * 8) panel_w = std::max(80, layout.screen_w - 2 * 8);
    const int title_h = spec.caption.empty() ? 0 : kModalTitleH;
    const int text_h = static_cast<int>(spec.lines.size()) * kModalLineH;
    const int btn_row_h = spec.buttons.empty() ? 0 : kModalButtonH;
    int panel_h = pad + title_h + text_h + (btn_row_h != 0 ? kModalTextGap + btn_row_h : 0) + pad;
    if (panel_h < panel_h_min) panel_h = panel_h_min;
    int panel_x = (layout.screen_w - panel_w) / 2;
    int panel_y = (layout.screen_h - panel_h) / 2;
    if (panel_x < 0) panel_x = 0;
    if (panel_y < 0) panel_y = 0;
    layout.panel = ModalRect{panel_x, panel_y, panel_w, panel_h};
    const int content_w = panel_w - 2 * pad;
    layout.title = title_h > 0 ? ModalRect{panel_x + pad, panel_y + pad, content_w, title_h}
                               : ModalRect{};
    layout.text = text_h > 0
                      ? ModalRect{panel_x + pad, panel_y + pad + title_h, content_w, text_h}
                      : ModalRect{};
    if (!spec.buttons.empty()) {
        const int count = static_cast<int>(spec.buttons.size());
        const int total_w = count * kModalButtonW + (count - 1) * kModalButtonGap;
        int bx = panel_x + (panel_w - total_w) / 2;
        if (bx < panel_x + pad) bx = panel_x + pad;
        const int by = panel_y + panel_h - pad - kModalButtonH;
        layout.button_rects.reserve(spec.buttons.size());
        for (int i = 0; i < count; ++i) {
            layout.button_rects.push_back(
                ModalRect{bx + i * (kModalButtonW + kModalButtonGap), by, kModalButtonW, kModalButtonH});
        }
    }
    layout.focus = 0;
    for (std::size_t i = 0; i < spec.buttons.size(); ++i) {
        if (spec.buttons[i].is_default) {
            layout.focus = static_cast<int>(i);
            break;
        }
    }
    return layout;
}

std::uint32_t modalHitTest(const ModalSpec& spec, const ModalLayout& layout, int x, int y) {
    const std::size_t n = std::min(spec.buttons.size(), layout.button_rects.size());
    for (std::size_t i = 0; i < n; ++i) {
        if (layout.button_rects[i].contains(x, y)) return spec.buttons[i].id;
    }
    return 0;
}

// =======================================================================================
// 状态机
// =======================================================================================
ModalState::ModalState(ModalSpec spec, int screen_w, int screen_h)
    : spec_(std::move(spec)), layout_(modalLayout(spec_, screen_w, screen_h)) {
    focus_ = layout_.focus;
    layout_.focus = focus_;
}

std::optional<std::uint32_t> ModalState::step(const ModalInput& in) {
    // ★ quit 收口：外部要"结束这次 run"（脚本 quit / marker 超时）⇒ 用**默认值**退出模态，
    //   ⛔ 不吞掉 WM_QUIT（调用方看到 quitPosted 后走正常停机链）。
    if (in.quit_requested) {
        quit_requested_ = true;
        return modalDefaultResult(spec_);
    }
    // X（关窗）与 Esc：按 §4 的表；MB_YESNO 两者都是 IDNO。
    if (in.close_requested) return spec_.close_result;
    if (in.escape) return spec_.esc_result;
    if (spec_.buttons.empty()) return spec_.default_result;

    const int count = static_cast<int>(spec_.buttons.size());
    // ---- 键盘焦点 ----
    const bool tab_edge = in.tab && !prev_tab_;
    prev_tab_ = in.tab;
    const bool left_edge = in.left && !prev_left_;
    prev_left_ = in.left;
    const bool right_edge = in.right && !prev_right_;
    prev_right_ = in.right;
    if (tab_edge) {
        focus_ = (focus_ + (in.shift ? count - 1 : 1)) % count;
        dirty_ = true;
    }
    if (left_edge) {
        focus_ = (focus_ + count - 1) % count;
        dirty_ = true;
    }
    if (right_edge) {
        focus_ = (focus_ + 1) % count;
        dirty_ = true;
    }
    layout_.focus = focus_;
    if (in.enter) return spec_.buttons[static_cast<std::size_t>(focus_)].id;

    // ---- 鼠标：**按下沿即提交**（见设计 §3.3；注入的抬起可能永远不来）----
    const bool press_edge = in.lbutton && !prev_lbutton_;
    prev_lbutton_ = in.lbutton;
    int hover = -1;
    for (std::size_t i = 0; i < layout_.button_rects.size() && i < spec_.buttons.size(); ++i) {
        if (layout_.button_rects[i].contains(in.mouse_x, in.mouse_y)) {
            hover = static_cast<int>(i);
            break;
        }
    }
    if (hover != hover_) {
        hover_ = hover;
        dirty_ = true;
    }
    if (in.lbutton != pressed_) {
        pressed_ = in.lbutton;
        dirty_ = true;
    }
    if (press_edge) {
        const std::uint32_t hit = modalHitTest(spec_, layout_, in.mouse_x, in.mouse_y);
        if (hit != 0) return hit;
    }
    return std::nullopt;
}

// =======================================================================================
// 合成
// =======================================================================================
bool composeModalFrame(const ModalSpec& spec, const ModalLayout& layout, const ModalInput& in,
                       FontEngine* fonts, const std::uint8_t* backdrop, int backdrop_w, int backdrop_h,
                       int backdrop_pitch, std::vector<std::uint8_t>* out, int* out_w, int* out_h,
                       bool* font_missing) {
    if (font_missing != nullptr) *font_missing = false;
    const int w = layout.screen_w > 0 ? layout.screen_w : 1280;
    const int h = layout.screen_h > 0 ? layout.screen_h : 720;
    if (out == nullptr) return false;
    out->assign(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4u, 0);
    if (out_w != nullptr) *out_w = w;
    if (out_h != nullptr) *out_h = h;

    // ① 底：末帧变暗（α=160 ⇒ ×95/255）；没有末帧（boot）⇒ 纯色 #202020。
    const bool have_backdrop = backdrop != nullptr && backdrop_w > 0 && backdrop_h > 0 &&
                               backdrop_pitch >= backdrop_w * 4;
    for (int y = 0; y < h; ++y) {
        std::uint8_t* row = out->data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(w) * 4u;
        const int sy = have_backdrop ? std::min(backdrop_h - 1, y * backdrop_h / h) : 0;
        for (int x = 0; x < w; ++x) {
            std::uint8_t rgb_out[3] = {kBootBase, kBootBase, kBootBase};
            if (have_backdrop) {
                const int sx = std::min(backdrop_w - 1, x * backdrop_w / w);
                const std::uint8_t* src = backdrop + static_cast<std::size_t>(sy) *
                                                         static_cast<std::size_t>(backdrop_pitch) +
                                          static_cast<std::size_t>(sx) * 4u;
                for (int c = 0; c < 3; ++c) {
                    rgb_out[c] = static_cast<std::uint8_t>((static_cast<int>(src[c]) *
                                                           (255 - kDimAlpha)) /
                                                          255);
                }
            }
            row[static_cast<std::size_t>(x) * 4u + 0] = rgb_out[0];
            row[static_cast<std::size_t>(x) * 4u + 1] = rgb_out[1];
            row[static_cast<std::size_t>(x) * 4u + 2] = rgb_out[2];
            row[static_cast<std::size_t>(x) * 4u + 3] = 255;
        }
    }

    // ② 面板 + 1px 边框
    fillRect(*out, w, h, layout.panel, rgb(kPanelBg));
    strokeRect(*out, w, h, layout.panel, 1, rgb(kPanelBorder));

    // ③ 标题 / 正文（FontEngine；缺后端 ⇒ 面板/按钮照画 + 报告）
    bool font_ok = true;
    if (!layout.title.empty() && !spec.caption.empty()) {
        if (!drawText(*out, w, h, fonts, spec.caption, kModalTitlePx, layout.title.x + 4,
                      layout.title.y + 3, rgb(kTitleFg))) {
            font_ok = false;
        }
    }
    if (!spec.lines.empty()) {
        for (std::size_t i = 0; i < spec.lines.size(); ++i) {
            const int line_y = layout.text.y + static_cast<int>(i) * kModalLineH;
            if (!drawText(*out, w, h, fonts, spec.lines[i], kModalTextPx, layout.text.x + 4, line_y + 3,
                          rgb(kTextFg))) {
                font_ok = false;
                break;
            }
        }
    }
    if (font_missing != nullptr) *font_missing = !font_ok;

    // ④ 按钮（默认键 / 焦点 = 蓝框；悬停 = 更亮底；按下 = 更暗底）
    const std::uint32_t hit = modalHitTest(spec, layout, in.mouse_x, in.mouse_y);
    for (std::size_t i = 0; i < layout.button_rects.size() && i < spec.buttons.size(); ++i) {
        const ModalRect& r = layout.button_rects[i];
        Rgb bg = rgb(kButtonBg);
        if (spec.buttons[i].id == hit) bg = in.lbutton ? rgb(kButtonDownBg) : rgb(kButtonHoverBg);
        fillRect(*out, w, h, r, bg);
        const bool highlight = spec.buttons[i].is_default || static_cast<int>(i) == layout.focus;
        strokeRect(*out, w, h, r, highlight ? 2 : 1, highlight ? rgb(kAccent) : rgb(kButtonBorder));
        // 标签：在按钮里居中（宽度用字体量；量不到就左对齐、仍画）
        int text_w = 0;
        if (fonts != nullptr) {
            for (const char32_t cp : decodeUtf8(spec.buttons[i].label)) {
                GlyphMask mask;
                if (!fonts->glyph(cp, kModalTextPx, false, &mask)) {
                    font_ok = false;
                    break;
                }
                text_w += mask.advance;
            }
        }
        const int tx = r.x + std::max(4, (r.w - text_w) / 2);
        const int ty = r.y + (r.h - kModalLineH) / 2 + 3;
        if (!drawText(*out, w, h, fonts, spec.buttons[i].label, kModalTextPx, tx, ty, rgb(kTextFg))) {
            font_ok = false;
        }
    }
    if (font_missing != nullptr) *font_missing = !font_ok;
    return true;
}

// =======================================================================================
// 驱动器（唯一一处模态循环）
// =======================================================================================
bool modalDialogInFlight() { return g_modal_in_flight.load(std::memory_order_relaxed); }

namespace {

// 一次输入快照的"变了没有"：变了才 `noteInputSample`（对话框就是那个读点；见设计 §1.4 ①）。
struct InputSnapshot {
    int x = 0;
    int y = 0;
    bool lbutton = false;
    bool enter = false;
    bool escape = false;
    bool tab = false;
    bool quit = false;
    bool same(const InputSnapshot& o) const {
        return x == o.x && y == o.y && lbutton == o.lbutton && enter == o.enter && escape == o.escape &&
               tab == o.tab && quit == o.quit;
    }
};

}  // namespace

std::uint32_t runModalDialog(Machine& machine, X86& cpu, const ModalSpec& spec) {
    const std::uint32_t fallback = modalDefaultResult(spec);
    // feeder 线程：⛔ 不碰 wm / presenter（见设计 §7.1；`WindowManager` 有主线程亲和性账）。
    //   ★ 这条必须在**重入检查之前**：feeder 不该为了判"有没有模态在跑"去读主线程的状态。
    if (cpu.audio != nullptr && cpu.audio->onFeederThread()) {
        logAt(LogLevel::Warn, LogShim, "[modal] ",
              "在音频 feeder 线程上收到 MessageBox ⇒ headless 语义：立即返回默认值 %u（⛔ 不碰窗口）",
              fallback);
        return fallback;
    }
    // 重入：对话框里再弹对话框 ⇒ 默认值 + 一次 warn（⛔ 嵌套模态循环）。
    if (g_modal_in_flight.load(std::memory_order_relaxed)) {
        logAt(LogLevel::Warn, LogShim, "[modal] ",
              "对话框未退出又弹了一个（⛔ 不嵌套）⇒ 立即返回默认值 %u", fallback);
        return fallback;
    }
    PresentSink* sink = machine.presenter().sink();
    ModalPresenter* presenter = sink != nullptr ? sink->modalPresenter() : nullptr;
    if (presenter == nullptr) {
        logAt(LogLevel::Warn, LogShim, "[modal] ",
              "没有模态呈现后端（headless / 测试替身）⇒ 立即返回默认值 %u（⛔ 不阻塞）", fallback);
        return fallback;
    }

    // ★ 末帧快照必须在**第一次 Yield 之前**拷完（宿主内存，跨 Yield 安全；见设计 §1.2/§2.3）。
    std::vector<std::uint8_t> backdrop;
    int backdrop_w = 0;
    int backdrop_h = 0;
    std::uint32_t backdrop_pitch = 0;
    const bool have_backdrop =
        machine.presenter().snapshotLastFrame(&backdrop, &backdrop_w, &backdrop_h, &backdrop_pitch);
    const int screen_w = have_backdrop && backdrop_w > 0 ? backdrop_w : 1280;
    const int screen_h = have_backdrop && backdrop_h > 0 ? backdrop_h : 720;

    InFlightGuard in_flight;
    ModalState state(spec, screen_w, screen_h);
    WindowManager* wm = cpu.windows;
    InputSnapshot last_input;
    bool have_last_input = false;
    bool presented_once = false;
    bool font_warned = false;
    std::uint32_t result = fallback;
    std::vector<std::uint8_t> frame;
    int frame_w = 0;
    int frame_h = 0;

    while (true) {
        // ① 等宿主事件：**真实等待必须在 YieldGuard 里**（持令牌 sleep = 死锁）。
        const auto wait_begin = std::chrono::steady_clock::now();
        {
            GuestToken::YieldGuard yield(guestToken());
            presenter->waitHostEvents(5);
        }
        const auto wait_end = std::chrono::steady_clock::now();
        // ② 按**真实睡掉**的时长推进虚拟时钟（否则注入 `click` 的抬起判据永远不成立；
        //    设计 §1.4 ②。真实档已由 `--present window` 置 real-time，不新增确定性损失）。
        const auto slept_ns = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(wait_end - wait_begin).count());
        if (slept_ns > 0) machine.clock().advance100ns(slept_ns / 100u);
        // ③ 推进注入通道（与 `GetMessageA` 的宿主事件泵同一口径：模态期间没有块边界，
        //    这是唯一能让脚本动作落地的点；frame / insn / guest_ms 在模态期间冻住）。
        machine.tickInputScript(cpu);
        machine.tickInteraction(cpu);

        // ④ 读唯一输入状态源（WindowManager；注入与真实事件写的是同一份）
        ModalInput in;
        InputSnapshot snap;
        if (wm != nullptr) {
            const std::uint32_t hwnd = wm->primaryWindow();
            int mx = 0;
            int my = 0;
            wm->cursorClientPos(hwnd, &mx, &my);
            in.mouse_x = mx;
            in.mouse_y = my;
            in.lbutton = (wm->mouseButtons() & input::kMkLButton) != 0;
            in.enter = wm->keyDown(0x0Du);
            in.escape = wm->keyDown(0x1Bu);
            in.tab = wm->keyDown(0x09u);
            in.shift = wm->keyDown(0x10u);
            in.left = wm->keyDown(0x25u);
            in.right = wm->keyDown(0x27u);
            in.quit_requested = wm->quitPosted();
            snap = InputSnapshot{mx, my, in.lbutton, in.enter, in.escape, in.tab, in.quit_requested};
        }
        in.close_requested = presenter->closeRequested();
        // 外部要收口（SDL quit / 窗口关掉 / WM_QUIT）：用默认值退出模态，⛔ 不让它挂死。
        if (sink != nullptr && sink->stopRequested()) result = fallback;
        if (in.quit_requested || (sink != nullptr && sink->stopRequested())) {
            if (frame.empty()) {
                // 还没画过就收口：不留下"半开"的模态状态。
                presenter->endModal();
                return result;
            }
            break;
        }
        if (wm != nullptr && (!have_last_input || !snap.same(last_input))) {
            wm->noteInputSample(machine.clock().nowMs());
            last_input = snap;
            have_last_input = true;
        }

        // ⑤ 状态机
        const std::optional<std::uint32_t> settled = state.step(in);
        if (settled.has_value()) {
            result = *settled;
            break;
        }

        // ⑥ 重绘（脏才合成 / 贴；整幅不透明 RGBA8 一次性交给 sink）
        if (state.dirty() || !presented_once) {
            bool font_missing = false;
            if (!composeModalFrame(state.spec(), state.layout(), in, &machine.fonts(),
                                   have_backdrop ? backdrop.data() : nullptr, backdrop_w, backdrop_h,
                                   static_cast<int>(backdrop_pitch), &frame, &frame_w, &frame_h,
                                   &font_missing)) {
                logAt(LogLevel::Warn, LogShim, "[modal] ", "合成模态帧失败（尺寸非法）⇒ 用默认值 %u 收口",
                      fallback);
                result = fallback;
                break;
            }
            if (font_missing && !font_warned) {
                font_warned = true;
                logAt(LogLevel::Warn, LogShim, "[modal] ",
                      "没有可用的字体后端（FontEngine::glyph 失败）⇒ 面板/按钮照画，正文按不可见处理"
                      "（⛔ 不静默：这一行就是证据）");
            }
            std::string present_err;
            bool present_ok = false;
            {
                // ★ 呈现（SDL_RenderPresent / GPU submit）也是宿主等待 ⇒ 同样在 Yield 里。
                GuestToken::YieldGuard yield(guestToken());
                present_ok = presenter->presentModal(frame.data(), frame_w, frame_h, &present_err);
            }
            if (!present_ok) {
                logAt(LogLevel::Warn, LogShim, "[modal] ",
                      "模态帧贴不上去：%s ⇒ 用默认值 %u 收口（⛔ 不装成画过了）",
                      present_err.empty() ? "(未给原因)" : present_err.c_str(), fallback);
                result = fallback;
                break;
            }
            state.markPresented();
            presented_once = true;
        }
    }
    // ⑦ 收口：释放模态纹理 / 恢复被挂起的关窗宽限。
    presenter->endModal();
    return result;
}

// =======================================================================================
// 调用账
// =======================================================================================
void noteMessageBoxCall(std::uint32_t u_type, std::uint32_t result) {
    g_messagebox_calls.fetch_add(1, std::memory_order_relaxed);
    g_messagebox_last_type.store(u_type, std::memory_order_relaxed);
    g_messagebox_last_result.store(result, std::memory_order_relaxed);
}

std::uint64_t messageBoxCalls() { return g_messagebox_calls.load(std::memory_order_relaxed); }
std::uint32_t messageBoxLastType() { return g_messagebox_last_type.load(std::memory_order_relaxed); }
std::uint32_t messageBoxLastResult() { return g_messagebox_last_result.load(std::memory_order_relaxed); }

}  // namespace lift
