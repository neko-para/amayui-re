#include "runtime/overlay.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace lift {
namespace {

// ---------------------------------------------------------------------------------------
// 5×7 位图字体
// ---------------------------------------------------------------------------------------
// ★ 每个字形 = 7 行 × 5 列（`#` = 亮点、` ` = 暗点），行序自上而下。用 ASCII 图形而不是
//   十六进制位表，是因为它可**肉眼复核**：改错一格 `只写矩形` / 字符栅格化的用例会当场红。
// ★ 覆盖范围 = ` `–`~` 里本仓会用到的那一批：`0-9 A-Z a-z . , : / - + % ( ) 空格`。
//   没有覆盖的字符由 `overlayGlyph` 返回 false，渲染时按空格处理（⛔ 不猜字形、⛔ 不画方块）。
struct Glyph {
    char ch;
    const char* art;  // 7*5 = 35 个字符
};

constexpr Glyph kGlyphs[] = {
    {' ', "     " "     " "     " "     " "     " "     " "     "},
    // ---- 数字 ----
    {'0', " ### " "#   #" "#  ##" "# # #" "##  #" "#   #" " ### "},
    {'1', "  #  " " ##  " "  #  " "  #  " "  #  " "  #  " " ### "},
    {'2', " ### " "#   #" "    #" "   # " "  #  " " #   " "#####"},
    {'3', "#####" "   # " "  #  " "   # " "    #" "#   #" " ### "},
    {'4', "   # " "  ## " " # # " "#  # " "#####" "   # " "   # "},
    {'5', "#####" "#    " "#### " "    #" "    #" "#   #" " ### "},
    {'6', "  ## " " #   " "#    " "#### " "#   #" "#   #" " ### "},
    {'7', "#####" "    #" "   # " "  #  " " #   " " #   " " #   "},
    {'8', " ### " "#   #" "#   #" " ### " "#   #" "#   #" " ### "},
    {'9', " ### " "#   #" "#   #" " ####" "    #" "   # " " ##  "},
    // ---- 大写 ----
    {'A', " ### " "#   #" "#   #" "#####" "#   #" "#   #" "#   #"},
    {'B', "#### " "#   #" "#   #" "#### " "#   #" "#   #" "#### "},
    {'C', " ### " "#   #" "#    " "#    " "#    " "#   #" " ### "},
    {'D', "#### " "#   #" "#   #" "#   #" "#   #" "#   #" "#### "},
    {'E', "#####" "#    " "#    " "#### " "#    " "#    " "#####"},
    {'F', "#####" "#    " "#    " "#### " "#    " "#    " "#    "},
    {'G', " ### " "#   #" "#    " "#  ##" "#   #" "#   #" " ### "},
    {'H', "#   #" "#   #" "#   #" "#####" "#   #" "#   #" "#   #"},
    {'I', " ### " "  #  " "  #  " "  #  " "  #  " "  #  " " ### "},
    {'J', "  ###" "   # " "   # " "   # " "   # " "#  # " " ##  "},
    {'K', "#   #" "#  # " "# #  " "##   " "# #  " "#  # " "#   #"},
    {'L', "#    " "#    " "#    " "#    " "#    " "#    " "#####"},
    {'M', "#   #" "## ##" "# # #" "# # #" "#   #" "#   #" "#   #"},
    {'N', "#   #" "##  #" "# # #" "#  ##" "#   #" "#   #" "#   #"},
    {'O', " ### " "#   #" "#   #" "#   #" "#   #" "#   #" " ### "},
    {'P', "#### " "#   #" "#   #" "#### " "#    " "#    " "#    "},
    {'Q', " ### " "#   #" "#   #" "#   #" "# # #" "#  # " " ## #"},
    {'R', "#### " "#   #" "#   #" "#### " "# #  " "#  # " "#   #"},
    {'S', " ### " "#   #" "#    " " ### " "    #" "#   #" " ### "},
    {'T', "#####" "  #  " "  #  " "  #  " "  #  " "  #  " "  #  "},
    {'U', "#   #" "#   #" "#   #" "#   #" "#   #" "#   #" " ### "},
    {'V', "#   #" "#   #" "#   #" "#   #" "#   #" " # # " "  #  "},
    {'W', "#   #" "#   #" "#   #" "# # #" "# # #" "## ##" "#   #"},
    {'X', "#   #" "#   #" " # # " "  #  " " # # " "#   #" "#   #"},
    {'Y', "#   #" "#   #" " # # " "  #  " "  #  " "  #  " "  #  "},
    {'Z', "#####" "    #" "   # " "  #  " " #   " "#    " "#####"},
    // ---- 小写 ----
    {'a', "     " "     " " ### " "    #" " ####" "#   #" " ####"},
    {'b', "#    " "#    " "#### " "#   #" "#   #" "#   #" "#### "},
    {'c', "     " "     " " ### " "#   #" "#    " "#   #" " ### "},
    {'d', "    #" "    #" " ####" "#   #" "#   #" "#   #" " ####"},
    {'e', "     " "     " " ### " "#   #" "#####" "#    " " ### "},
    {'f', "  ## " " #   " "#### " " #   " " #   " " #   " " #   "},
    {'g', "     " " ####" "#   #" "#   #" " ####" "    #" " ### "},
    {'h', "#    " "#    " "#### " "#   #" "#   #" "#   #" "#   #"},
    {'i', "  #  " "     " " ##  " "  #  " "  #  " "  #  " " ### "},
    {'j', "   # " "     " "  ## " "   # " "   # " "#  # " " ##  "},
    {'k', "#    " "#    " "#  # " "# #  " "##   " "# #  " "#  # "},
    {'l', " ##  " "  #  " "  #  " "  #  " "  #  " "  #  " " ### "},
    {'m', "     " "     " "## ##" "# # #" "# # #" "#   #" "#   #"},
    {'n', "     " "     " "#### " "#   #" "#   #" "#   #" "#   #"},
    {'o', "     " "     " " ### " "#   #" "#   #" "#   #" " ### "},
    {'p', "     " " ####" "#   #" "#   #" " ####" "#    " "#    "},
    {'q', "     " " ####" "#   #" "#   #" " ####" "    #" "    #"},
    {'r', "     " "     " "# ## " "##   " "#    " "#    " "#    "},
    {'s', "     " "     " " ####" "#    " " ### " "    #" "#### "},
    {'t', " #   " " #   " "#### " " #   " " #   " " #  #" "  ## "},
    {'u', "     " "     " "#   #" "#   #" "#   #" "#  ##" " ## #"},
    {'v', "     " "     " "#   #" "#   #" "#   #" " # # " "  #  "},
    {'w', "     " "     " "#   #" "#   #" "# # #" "# # #" " # # "},
    {'x', "     " "     " "#   #" " # # " "  #  " " # # " "#   #"},
    {'y', "     " "     " "#   #" "#   #" " ####" "    #" " ### "},
    {'z', "     " "     " "#####" "   # " "  #  " " #   " "#####"},
    // ---- 标点 / 符号 ----
    {'.', "     " "     " "     " "     " "     " " ##  " " ##  "},
    {',', "     " "     " "     " "     " " ##  " " ##  " " #   "},
    {':', "     " " ##  " " ##  " "     " " ##  " " ##  " "     "},
    {'/', "    #" "    #" "   # " "  #  " " #   " "#    " "#    "},
    {'-', "     " "     " "     " "#####" "     " "     " "     "},
    {'+', "     " "  #  " "  #  " "#####" "  #  " "  #  " "     "},
    {'%', "##  #" "##  #" "   # " "  #  " " #   " "#  ##" "#  ##"},
    {'(', "   # " "  #  " " #   " " #   " " #   " "  #  " "   # "},
    {')', " #   " "  #  " "   # " "   # " "   # " "  #  " " #   "},
};

constexpr int kArtLen = kOverlayGlyphW * kOverlayGlyphH;  // 35

std::string formatFps(double fps) {
    // 只显示一位小数；负数 / 非有限值不该走到这里（调用方置 `fps_available` 前就挡掉）。
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1f", fps);
    return std::string(buf);
}

// `worst` 行的一位小数（与 fps 同一份口径：`%.1f`；⛔ 不用 std::to_string 的 6 位）
std::string formatMs(double ms) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1f", ms);
    return std::string(buf);
}

}  // namespace

bool overlayGlyph(char ch, std::uint8_t rows[kOverlayGlyphH]) {
    for (int i = 0; i < kOverlayGlyphH; ++i) rows[i] = 0;
    for (const Glyph& g : kGlyphs) {
        if (g.ch != ch) continue;
        if (std::strlen(g.art) != static_cast<std::size_t>(kArtLen)) return false;  // 表写坏了 ⇒ 不画
        for (int y = 0; y < kOverlayGlyphH; ++y) {
            std::uint8_t bits = 0;
            for (int x = 0; x < kOverlayGlyphW; ++x) {
                if (g.art[y * kOverlayGlyphW + x] == '#') bits |= static_cast<std::uint8_t>(1u << (kOverlayGlyphW - 1 - x));
            }
            rows[y] = bits;
        }
        return true;
    }
    return false;
}

std::string overlayScriptPayload(const std::string& name, bool name_degraded, std::uint32_t id) {
    // ★ 降级形态**只用 id**（⛔ 不猜名字）：`MarkerSample.name` 在降级时也是 `id:0x…`，
    //   这里显式格式化一次，保证"名字表读不到"与"名字恰好为空"不会混为一谈。
    if (!name_degraded) return name;
    char buf[64];
    std::snprintf(buf, sizeof(buf), "id:0x%08X（降级）", static_cast<unsigned>(id));
    return std::string(buf);
}

void overlayWorstPush(std::deque<OverlayFrameSample>& samples, const OverlayFrameSample& sample) {
    // 时间戳必须单调（调用方只喂 `steady_clock`）：倒退的样本会让"窗口 = 最新样本往前 3 s"
    // 失去意义 ⇒ 直接丢（⛔ 不排序、不猜哪一条才是"现在"）。
    if (!samples.empty() && sample.at < samples.back().at) return;
    samples.push_back(sample);
    const auto cutoff = sample.at - kOverlayWorstWindow;
    while (!samples.empty() && samples.front().at < cutoff) samples.pop_front();
}

OverlayWorst overlayWorstMax(const std::deque<OverlayFrameSample>& samples) {
    OverlayWorst out;
    for (const OverlayFrameSample& s : samples) {
        out.has_worst = true;
        out.wall_max_ms = std::max(out.wall_max_ms, s.wall_ms);
        out.guest_max_ms = std::max(out.guest_max_ms, s.guest_ms);
        out.shim_max_ms = std::max(out.shim_max_ms, s.shim_ms);
    }
    return out;
}

std::vector<std::string> formatOverlayLines(const OverlayState& state, const OverlayStatus& status) {
    std::vector<std::string> lines;
    if (state.mouse_available) lines.push_back("mouse " + std::to_string(state.mouse_x) + "," + std::to_string(state.mouse_y));
    // ★ 身份行在 mouse 之后、fps 之前（固定顺序；拿到才写，⛔ 不占位）。
    if (status.has_script && !status.script.empty()) lines.push_back("script " + status.script);
    if (state.fps_available) lines.push_back("fps " + formatFps(state.fps));
    if (state.frame_available) lines.push_back("frame " + std::to_string(state.frame));
    if (state.raster != nullptr && state.raster[0] != '\0') lines.push_back(std::string("raster ") + state.raster);
    if (state.present != nullptr && state.present[0] != '\0') lines.push_back(std::string("present ") + state.present);
    // ★ `worst` 永远最后一行：3 s 窗口内每帧 wall / guest / shim 的最大值。
    //   `guest` 没开块计时就写 `--`（⛔ 不写 0.0：那会被读成"guest 不用时间"）。
    if (status.has_worst) {
        std::string line = "worst 3s  wall=" + formatMs(status.wall_max_ms) + "ms guest=";
        line += status.has_guest ? formatMs(status.guest_max_ms) + "ms" : std::string("--");
        line += " shim=" + formatMs(status.shim_max_ms) + "ms";
        lines.push_back(std::move(line));
    }
    return lines;
}

OverlaySize overlaySize(const std::vector<std::string>& lines) {
    if (lines.empty()) return OverlaySize{};
    std::size_t longest = 0;
    for (const std::string& line : lines) longest = line.size() > longest ? line.size() : longest;
    const int chars = static_cast<int>(longest);
    const int count = static_cast<int>(lines.size());
    OverlaySize out;
    out.w = 2 * kOverlayPad + chars * kOverlayAdvance - 1;  // 最后一列后面不留字距
    out.h = 2 * kOverlayPad + count * kOverlayLineH - 1;    // 最后一行下面不留行距
    return out;
}

void renderOverlay(std::uint8_t* rgba, int stride, int w, int h, const std::vector<std::string>& lines) {
    if (rgba == nullptr || w <= 0 || h <= 0) return;
    if (stride < w * 4) return;  // 行距不够 ⇒ 写不了这一块（⛔ 不越界写别人的行）
    // ① 不透明深底：这块矩形里**每个**像素都写（叠层不依赖底下的画面）。
    for (int y = 0; y < h; ++y) {
        std::uint8_t* row = rgba + static_cast<std::ptrdiff_t>(y) * stride;
        for (int x = 0; x < w; ++x) {
            std::uint8_t* p = row + static_cast<std::ptrdiff_t>(x) * 4;
            p[0] = kOverlayBg[0];
            p[1] = kOverlayBg[1];
            p[2] = kOverlayBg[2];
            p[3] = kOverlayBg[3];
        }
    }
    // ② 亮字：逐字符栅格化，越出矩形就裁掉。
    int line_top = kOverlayPad;
    for (const std::string& line : lines) {
        if (line_top >= h) break;
        int pen_x = kOverlayPad;
        for (const char ch : line) {
            std::uint8_t rows[kOverlayGlyphH];
            overlayGlyph(ch, rows);  // 未覆盖字符 ⇒ 全 0（= 空格）
            for (int gy = 0; gy < kOverlayGlyphH; ++gy) {
                const int py = line_top + gy;
                if (py < 0 || py >= h) continue;
                std::uint8_t bits = rows[gy];
                if (bits == 0) continue;
                std::uint8_t* dst_row = rgba + static_cast<std::ptrdiff_t>(py) * stride;
                for (int gx = 0; gx < kOverlayGlyphW; ++gx) {
                    if ((bits & (1u << (kOverlayGlyphW - 1 - gx))) == 0) continue;
                    const int px = pen_x + gx;
                    if (px < 0 || px >= w) continue;
                    std::uint8_t* p = dst_row + static_cast<std::ptrdiff_t>(px) * 4;
                    p[0] = kOverlayFg[0];
                    p[1] = kOverlayFg[1];
                    p[2] = kOverlayFg[2];
                    p[3] = kOverlayFg[3];
                }
            }
            pen_x += kOverlayAdvance;
            if (pen_x >= w) break;  // 这一行剩下的字都在矩形外
        }
        line_top += kOverlayLineH;
    }
}

std::vector<std::uint8_t> renderOverlayImage(const std::vector<std::string>& lines, int* w, int* h) {
    const OverlaySize size = overlaySize(lines);
    if (w != nullptr) *w = size.w;
    if (h != nullptr) *h = size.h;
    std::vector<std::uint8_t> image;
    if (size.w <= 0 || size.h <= 0) return image;
    image.assign(static_cast<std::size_t>(size.w) * static_cast<std::size_t>(size.h) * 4u, 0);
    renderOverlay(image.data(), size.w * 4, size.w, size.h, lines);
    return image;
}

}  // namespace lift
