// emulator-lift · 宿主 GDI 字体 / 文本层（P2）——实现
//
// ★ 见 `font.h` 的口径。这里只有四件事：
//   ① FreeType 桥（face / size / metrics / 覆盖率）＋ 纯档的合成接缝；
//   ② GDI 对象表（DC / 字体）落在现有 `HandleTable` 上的 helper；
//   ③ **唯一**的取字/推进路径 `gdiGlyph`（GGO / extent / TextOut 都从它派生）；
//   ④ GGO 的格式算术（`gdiPackGlyph`，纯函数）与写表面（`gdiDrawText`）。
// ★ ⛔ 这里不出现 Win32 形状（参数解包 / 返回值约定 / 句柄校验的响亮失败）——那些在 shims/。
#include "runtime/font.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <map>
#include <tuple>
#include <utility>

#include <ft2build.h>
#include FT_FREETYPE_H

#include "runtime/cp932.h"
#include "runtime/machine.h"
#include "runtime/vfs.h"  // openHostFileUtf8（Windows 的 UTF-8 路径）

namespace lift {
namespace {

// ---------------------------------------------------------------------------------------
// 配置（进程级；CLI 在 Machine 之前调 `fontSetConfig`）
// ---------------------------------------------------------------------------------------
std::string boldPathFor(const std::string& regular) {
    if (regular.empty()) return {};
    const std::size_t dot = regular.rfind(".ttf");
    if (dot == std::string::npos) return regular;
    return regular.substr(0, dot) + "-Bold.ttf";
}

FontConfig makeDefaultConfig() {
    FontConfig cfg;
#ifdef LIFT_HOST_FONT_PATH
    cfg.regular_path = LIFT_HOST_FONT_PATH;
#endif
    cfg.bold_path = boldPathFor(cfg.regular_path);
    return cfg;
}

FontConfig g_config = makeDefaultConfig();

bool hostFileExists(const std::string& path) {
    if (path.empty()) return false;
    std::FILE* f = openHostFileUtf8(path, "rb");
    if (!f) return false;
    std::fclose(f);
    return true;
}

// note: 诊断日志只在**第一次**失败时打，避免每个字都刷屏；返回值仍是 false（调用方必须失败返回）。

struct Rgb {
    int r = 255;
    int g = 255;
    int b = 255;
};

Rgb decodeColor(std::uint32_t colorref) {
    Rgb c;
    c.r = static_cast<int>(colorref & 0xFFu);
    c.g = static_cast<int>((colorref >> 8) & 0xFFu);
    c.b = static_cast<int>((colorref >> 16) & 0xFFu);
    return c;
}

int maskShift(std::uint32_t mask) {
    for (int i = 0; i < 32; ++i) {
        if (((mask >> i) & 1u) != 0) return i;
    }
    return 0;
}

int maskBits(std::uint32_t mask) {
    int n = 0;
    for (int i = 0; i < 32; ++i) {
        if (((mask >> i) & 1u) != 0) ++n;
    }
    return n;
}

std::uint64_t maskMax(std::uint32_t mask) {
    const int bits = maskBits(mask);
    if (bits <= 0) return 1;
    if (bits >= 32) return 0xFFFFFFFFull;
    return (1ull << bits) - 1ull;
}

int extractChannel(std::uint32_t pixel, std::uint32_t mask) {
    if (mask == 0) return 0;
    const std::uint64_t v = (static_cast<std::uint64_t>(pixel & mask)) >> maskShift(mask);
    const std::uint64_t max = maskMax(mask);
    return static_cast<int>(v * 255ull / max);
}

std::uint32_t composeChannel(int value, std::uint32_t mask) {
    if (mask == 0) return 0;
    if (value < 0) value = 0;
    if (value > 255) value = 255;
    const std::uint64_t max = maskMax(mask);
    const std::uint64_t v = (static_cast<std::uint64_t>(value) * max + 127ull) / 255ull;
    return (static_cast<std::uint32_t>(v) << maskShift(mask)) & mask;
}

std::uint32_t readSurfacePixel(const GuestSurface& s, const std::uint8_t* base, int x, int y) {
    const std::size_t off = static_cast<std::size_t>(y) * s.pitch +
                            static_cast<std::size_t>(x) * s.format.bytesPerPixel();
    std::uint32_t value = 0;
    std::memcpy(&value, base + off, s.format.bytesPerPixel());
    return value;
}

void storeSurfacePixel(const GuestSurface& s, std::uint8_t* base, int x, int y, std::uint32_t value) {
    const std::size_t off = static_cast<std::size_t>(y) * s.pitch +
                            static_cast<std::size_t>(x) * s.format.bytesPerPixel();
    std::memcpy(base + off, &value, s.format.bytesPerPixel());
}

// 表面像素 = 通道掩码解码后的 RGB；混合后按同一组掩码编回去（16/24/32bpp 同一份算式）。
void blendSurfacePixel(GuestSurface& s, std::uint8_t* base, int x, int y, const Rgb& color,
                       int coverage, std::uint32_t* written) {
    if (x < 0 || y < 0 || x >= s.width || y >= s.height) return;
    if (coverage <= 0) return;
    if (coverage > 255) coverage = 255;
    const std::uint32_t before = readSurfacePixel(s, base, x, y);
    const int dr = extractChannel(before, s.format.rmask);
    const int dg = extractChannel(before, s.format.gmask);
    const int db = extractChannel(before, s.format.bmask);
    const int keep = 255 - coverage;
    const int r = (color.r * coverage + dr * keep) / 255;
    const int g = (color.g * coverage + dg * keep) / 255;
    const int b = (color.b * coverage + db * keep) / 255;
    std::uint32_t out = before & ~(s.format.rmask | s.format.gmask | s.format.bmask);
    out |= composeChannel(r, s.format.rmask);
    out |= composeChannel(g, s.format.gmask);
    out |= composeChannel(b, s.format.bmask);
    storeSurfacePixel(s, base, x, y, out);
    if (written) ++*written;
}

void fillSurfaceRect(GuestSurface& s, std::uint8_t* base, int x0, int y0, int w, int h,
                     const Rgb& color) {
    const std::uint32_t solid = composeChannel(color.r, s.format.rmask) |
                                composeChannel(color.g, s.format.gmask) |
                                composeChannel(color.b, s.format.bmask);
    for (int row = 0; row < h; ++row) {
        const int y = y0 + row;
        if (y < 0 || y >= s.height) continue;
        for (int col = 0; col < w; ++col) {
            const int x = x0 + col;
            if (x < 0 || x >= s.width) continue;
            storeSurfacePixel(s, base, x, y, solid);
        }
    }
}

}  // namespace

// =======================================================================================
// 配置
// =======================================================================================
FontConfig fontDefaultConfig() { return makeDefaultConfig(); }

void fontSetConfig(const FontConfig& cfg) {
    g_config = cfg;
    if (g_config.bold_path.empty()) g_config.bold_path = boldPathFor(g_config.regular_path);
}

const FontConfig& fontConfig() { return g_config; }

// =======================================================================================
// FontEngine
// =======================================================================================
struct FontEngine::Impl {
    FT_Library lib = nullptr;
    bool lib_tried = false;
    std::map<std::string, FT_Face> faces;   // host path → face（生命周期 = 本对象）
    std::map<std::string, int> face_size;   // host path → 当前 pixel size
    std::map<std::tuple<std::uint32_t, int, int>, GlyphMask> cache;  // (cp, size, bold) → mask
    bool warned_missing = false;
    bool warned_bold = false;
};

namespace {

FT_Library ensureLibrary(FontEngine::Impl& impl) {
    if (!impl.lib_tried) {
        impl.lib_tried = true;
        if (FT_Init_FreeType(&impl.lib) != 0) impl.lib = nullptr;
    }
    return impl.lib;
}

FT_Face faceFromPath(FontEngine::Impl& impl, const std::string& path) {
    if (path.empty()) return nullptr;
    const auto it = impl.faces.find(path);
    if (it != impl.faces.end()) return it->second;
    if (ensureLibrary(impl) == nullptr) return nullptr;
    FT_Face face = nullptr;
    if (FT_New_Face(impl.lib, path.c_str(), 0, &face) != 0 || face == nullptr) {
        std::fprintf(stderr, "[font] 无法加载宿主字体：%s（GDI 文本路径会**明确失败**，不画空白）\n",
                     path.c_str());
        return nullptr;
    }
    impl.faces.emplace(path, face);
    return face;
}

// 取（并切换）一个字号的 face。bold 缺字体文件时回退常规字面（与 emulator-cxx 同口径）。
FT_Face faceForSize(FontEngine::Impl& impl, bool bold, int pixel_size) {
    const FontConfig& cfg = fontConfig();
    std::string path = bold ? cfg.bold_path : cfg.regular_path;
    if (path.empty()) path = cfg.regular_path;
    FT_Face face = faceFromPath(impl, path);
    if (face == nullptr && bold) {
        if (!impl.warned_bold) {
            impl.warned_bold = true;
            std::fprintf(stderr, "[font] 粗体文件不可用（%s）⇒ 回退常规字面\n", path.c_str());
        }
        path = cfg.regular_path;
        face = faceFromPath(impl, path);
    }
    if (face == nullptr) return nullptr;
    const auto it = impl.face_size.find(path);
    if (it == impl.face_size.end() || it->second != pixel_size) {
        if (FT_Set_Pixel_Sizes(face, 0, static_cast<FT_UInt>(pixel_size)) != 0) return nullptr;
        impl.face_size[path] = pixel_size;
    }
    return face;
}

std::int32_t advanceOf(FT_Face face, std::uint32_t cp) {
    if (FT_Load_Char(face, static_cast<FT_ULong>(cp), FT_LOAD_DEFAULT) != 0) return 0;
    return static_cast<std::int32_t>(face->glyph->advance.x >> 6);
}

// ★ 推进 = **floor**(26.6)：`>> 6`（不是四舍五入）。extent / GGO / TextOut 共用它（`gdiGlyph` 是唯一路径）。
//   合成字形也走这里（26.6 的 6*64+32 ⇒ 6）⇒ 变异"floor→round"在 pure 档就能红。
int advancePixels(std::int64_t advance_266) { return static_cast<int>(advance_266 >> 6); }

// 纯档的合成字形：确定性、与字体文件无关。
//   advance = 6（= floor(6.5px)；26.6 的 6*64+32 ⇒ `>>6` 得 6，**四舍五入**会变 7 ⇒ 变异条目）
//   origin.x 依赖 cp 的奇偶（奇数 = -1，覆盖"负 origin 也要如实写出"这条）
//   覆盖率里 (0,0) 恒为 255 ⇒ GRAY4 的最大级必须是 16、GRAY8 必须是 64（值域用例靠它）
GlyphMask syntheticMask(char32_t cp, int pixel_size) {
    const std::uint32_t v = static_cast<std::uint32_t>(cp);
    GlyphMask m;
    m.black_box_x = 5 + static_cast<int>(v % 3u);
    m.black_box_y = std::max(1, pixel_size);
    m.origin_x = (v & 1u) ? 1 : -1;
    m.origin_y = m.black_box_y;
    m.advance = advancePixels(6 * 64 + 32);  // 26.6 的 6.5px ⇒ floor = 6
    m.coverage.assign(static_cast<std::size_t>(m.black_box_x) * static_cast<std::size_t>(m.black_box_y),
                      0);
    for (int y = 0; y < m.black_box_y; ++y) {
        for (int x = 0; x < m.black_box_x; ++x) {
            m.coverage[static_cast<std::size_t>(y) * m.black_box_x + x] =
                static_cast<std::uint8_t>((x * 37 + y * 53 + static_cast<int>(v) * 11) & 0xFF);
        }
    }
    m.coverage[0] = 255;
    return m;
}

}  // namespace

FontEngine::FontEngine() : impl_(new Impl()) {}
FontEngine::~FontEngine() {
    if (!impl_) return;
    for (auto& entry : impl_->faces) {
        if (entry.second != nullptr) FT_Done_Face(entry.second);
    }
    impl_->faces.clear();
    if (impl_->lib != nullptr) FT_Done_FreeType(impl_->lib);
}

void FontEngine::setSyntheticForTest(bool on) {
    synthetic_ = on;
    if (impl_) impl_->cache.clear();
}

void FontEngine::clearCache() {
    if (impl_) impl_->cache.clear();
}

bool FontEngine::fontFileAvailable(bool bold) const {
    const FontConfig& cfg = fontConfig();
    const std::string& path = (bold && !cfg.bold_path.empty()) ? cfg.bold_path : cfg.regular_path;
    return hostFileExists(path);
}

bool FontEngine::glyph(char32_t cp, int pixel_size, bool bold, GlyphMask* out) {
    if (out == nullptr) return false;
    const int size = std::clamp(pixel_size, 1, 512);
    if (synthetic_) {
        *out = syntheticMask(cp, size);
        return true;
    }
    if (!impl_) return false;
    const std::tuple<std::uint32_t, int, int> key{static_cast<std::uint32_t>(cp), size, bold ? 1 : 0};
    const auto cached = impl_->cache.find(key);
    if (cached != impl_->cache.end()) {
        *out = cached->second;
        return true;
    }
    FT_Face face = faceForSize(*impl_, bold, size);
    if (face == nullptr) {
        if (!impl_->warned_missing) {
            impl_->warned_missing = true;
            std::fprintf(stderr,
                         "[font] 没有可用的宿主字体（LIFT_HOST_FONT_PATH / --font）⇒ "
                         "GetGlyphOutlineA / GetTextMetricsA / TextOutA 会返回失败\n");
        }
        return false;
    }
    if (FT_Load_Char(face, static_cast<FT_ULong>(cp), FT_LOAD_RENDER | FT_LOAD_TARGET_NORMAL) != 0) {
        return false;
    }
    const FT_GlyphSlot slot = face->glyph;
    const FT_Bitmap& bitmap = slot->bitmap;
    GlyphMask m;
    m.black_box_x = static_cast<int>(bitmap.width);
    m.black_box_y = static_cast<int>(bitmap.rows);
    m.origin_x = slot->bitmap_left;
    m.origin_y = slot->bitmap_top;
    // ★ 推进 = **floor**(26.6)：`>> 6`（不是四舍五入）。extent / GGO / TextOut 共用它。
    m.advance = advancePixels(slot->advance.x);
    const std::size_t count =
        static_cast<std::size_t>(m.black_box_x) * static_cast<std::size_t>(m.black_box_y);
    m.coverage.assign(count, 0);
    if (count != 0 && bitmap.buffer != nullptr) {
        for (int row = 0; row < m.black_box_y; ++row) {
            const std::uint8_t* src = bitmap.buffer + static_cast<std::ptrdiff_t>(row) * bitmap.pitch;
            for (int col = 0; col < m.black_box_x; ++col) {
                std::uint8_t value = 0;
                if (bitmap.pixel_mode == FT_PIXEL_MODE_MONO) {
                    value = ((src[col >> 3] >> (7 - (col & 7))) & 1u) != 0 ? 255 : 0;
                } else if (bitmap.pixel_mode == FT_PIXEL_MODE_BGRA) {
                    value = src[static_cast<std::size_t>(col) * 4 + 3];
                } else {
                    value = src[col];
                }
                m.coverage[static_cast<std::size_t>(row) * m.black_box_x + col] = value;
            }
        }
    }
    impl_->cache.emplace(key, m);
    *out = std::move(m);
    return true;
}

bool FontEngine::metrics(int pixel_size, bool bold, std::int32_t weight, HostTextMetrics* out) {
    if (out == nullptr) return false;
    const int size = std::clamp(pixel_size, 1, 512);
    if (synthetic_) {
        out->height = size + 2;
        out->ascent = size;
        out->descent = 2;
        out->internal_leading = 0;
        out->external_leading = 0;
        out->ave_char_width = 6;
        out->max_char_width = 6;
        out->weight = weight;
        out->overhang = 0;
        return true;
    }
    if (!impl_) return false;
    FT_Face face = faceForSize(*impl_, bold, size);
    if (face == nullptr) {
        if (!impl_->warned_missing) {
            impl_->warned_missing = true;
            std::fprintf(stderr,
                         "[font] 没有可用的宿主字体（LIFT_HOST_FONT_PATH / --font）⇒ "
                         "GetGlyphOutlineA / GetTextMetricsA / TextOutA 会返回失败\n");
        }
        return false;
    }
    const FT_Size_Metrics& sm = face->size->metrics;
    out->ascent = std::max<std::int32_t>(0, static_cast<std::int32_t>(sm.ascender >> 6));
    out->descent = std::max<std::int32_t>(0, static_cast<std::int32_t>((-sm.descender) >> 6));
    out->height = out->ascent + out->descent;
    out->external_leading =
        std::max<std::int32_t>(0, static_cast<std::int32_t>(sm.height >> 6) - out->height);
    out->internal_leading = std::max<std::int32_t>(0, out->height - size);
    out->ave_char_width = advanceOf(face, 'M');
    out->max_char_width = std::max({out->ave_char_width, advanceOf(face, 0x6F22u),  // 漢
                                    advanceOf(face, 0x3042u)});                       // あ
    out->weight = weight;
    out->overhang = 0;
    return true;
}

// =======================================================================================
// GDI 对象表 helper
// =======================================================================================
std::uint32_t gdiStockObject(int index) {
    if (index < 0 || index >= kStockObjectMax) return 0;
    return kStockObjectBase + static_cast<std::uint32_t>(index);
}

int gdiStockIndex(std::uint32_t handle) {
    if (handle < kStockObjectBase || handle >= kStockObjectBase + static_cast<std::uint32_t>(kStockObjectMax)) {
        return -1;
    }
    return static_cast<int>(handle - kStockObjectBase);
}

bool gdiIsStockFont(int index) {
    switch (index) {
        case 10:  // OEM_FIXED_FONT
        case 11:  // ANSI_FIXED_FONT
        case 12:  // ANSI_VAR_FONT
        case 13:  // SYSTEM_FONT
        case 14:  // DEVICE_DEFAULT_FONT
        case 16:  // SYSTEM_FIXED_FONT
        case 17:  // DEFAULT_GUI_FONT
            return true;
        default:
            return false;
    }
}

namespace {

void destroyDc(void* payload) { delete static_cast<GdiDcObject*>(payload); }
void destroyFont(void* payload) { delete static_cast<GdiFontObject*>(payload); }

}  // namespace

GdiDcObject* gdiAsDc(HandleTable& table, std::uint32_t hdc) {
    HandleEntry* entry = table.get(hdc);
    if (entry == nullptr || entry->kind != HandleKind::GdiDc || entry->payload == nullptr) return nullptr;
    return static_cast<GdiDcObject*>(entry->payload);
}

GdiFontObject* gdiAsFont(HandleTable& table, std::uint32_t hfont) {
    HandleEntry* entry = table.get(hfont);
    if (entry == nullptr || entry->kind != HandleKind::GdiFont || entry->payload == nullptr) return nullptr;
    return static_cast<GdiFontObject*>(entry->payload);
}

GdiFontDesc gdiDefaultFontDesc() {
    GdiFontDesc desc;
    desc.height = -static_cast<std::int32_t>(kStockFontHeight);
    desc.weight = static_cast<std::int32_t>(kStockFontWeight);
    desc.face_name = fontConfig().use_engine_name ? std::string() : std::string("Amayui CN");
    return desc;
}

int gdiPixelSize(const GdiFontDesc& desc) {
    int size = desc.height < 0 ? -desc.height : desc.height;
    if (size == 0) size = static_cast<int>(kStockFontHeight);
    return std::clamp(size, 1, 512);
}

bool gdiBold(const GdiFontDesc& desc) { return desc.weight >= 700; }

GdiFontDesc gdiEffectiveFont(const GdiDcState& dc) { return dc.font; }

std::uint32_t gdiCreateDc(HandleTable& table, bool display, const std::string& device) {
    auto* object = new GdiDcObject();
    object->state.font = gdiDefaultFontDesc();
    object->state.font_handle = gdiStockObject(static_cast<int>(kDefaultGuiFont));
    object->state.display = display;
    object->state.device = device;
    const std::uint32_t handle =
        table.add(HandleKind::GdiDc, display ? "display-dc" : "dc", object, 0);
    if (HandleEntry* entry = table.get(handle)) entry->destroy = &destroyDc;
    return handle;
}

std::uint32_t gdiCreateFont(HandleTable& table, const GdiFontDesc& desc) {
    auto* object = new GdiFontObject();
    object->desc = desc;
    const std::uint32_t handle = table.add(HandleKind::GdiFont, "font", object, 0);
    if (HandleEntry* entry = table.get(handle)) entry->destroy = &destroyFont;
    return handle;
}

// =======================================================================================
// 取字 / 度量 / 推进（三个 GDI API 的唯一来源）
// =======================================================================================
std::vector<char32_t> gdiDecodeCp932(std::string_view bytes) {
    std::vector<char32_t> out;
    if (bytes.empty()) return out;
    const std::u16string wide = cp932ToWide(bytes);
    for (std::size_t i = 0; i < wide.size(); ++i) {
        const char16_t unit = wide[i];
        if (unit >= 0xD800 && unit <= 0xDBFF && i + 1 < wide.size() && wide[i + 1] >= 0xDC00 &&
            wide[i + 1] <= 0xDFFF) {
            const char32_t cp = 0x10000u + ((static_cast<char32_t>(unit) - 0xD800u) << 10) +
                                (static_cast<char32_t>(wide[i + 1]) - 0xDC00u);
            out.push_back(cp);
            ++i;
        } else {
            out.push_back(static_cast<char32_t>(unit));
        }
    }
    return out;
}

char32_t gdiCp932Char(std::uint32_t uchar) {
    // A 版 API：`uChar` 是**代码页的字节序列**。CP932 双字节按"前导字节在高位"打包
    // （与 emulator-cxx 的 `gdi_font_metrics` 同一约定：`bytes = {值>>8, 值&0xFF}`）。
    std::uint8_t bytes[2] = {0, 0};
    int length = 0;
    if (uchar <= 0xFFu) {
        bytes[0] = static_cast<std::uint8_t>(uchar & 0xFFu);
        length = 1;
    } else {
        bytes[0] = static_cast<std::uint8_t>((uchar >> 8) & 0xFFu);
        bytes[1] = static_cast<std::uint8_t>(uchar & 0xFFu);
        length = 2;
    }
    char16_t wide = 0;
    if (cp932Decode(bytes, static_cast<std::size_t>(length), &wide) > 0) {
        return static_cast<char32_t>(wide);
    }
    // 不是合法的 CP932 序列：如实按 Unicode 码点兜底（调用方会进 notdef；⛔ 不崩、不假装成功）
    return static_cast<char32_t>(uchar);
}

bool gdiGlyph(FontEngine& fonts, const GdiFontDesc& desc, char32_t cp, GlyphMask* out) {
    return fonts.glyph(cp, gdiPixelSize(desc), gdiBold(desc), out);
}

bool gdiTextMetrics(FontEngine& fonts, const GdiFontDesc& desc, HostTextMetrics* out) {
    if (!fonts.metrics(gdiPixelSize(desc), gdiBold(desc), desc.weight, out)) return false;
    out->weight = desc.weight;
    out->italic = desc.italic;
    out->underlined = desc.underline;
    out->struck_out = desc.strike_out;
    return true;
}

bool gdiTextExtent(FontEngine& fonts, const GdiFontDesc& desc, std::string_view cp932,
                   std::int32_t* width, std::int32_t* height) {
    HostTextMetrics tm;
    if (!gdiTextMetrics(fonts, desc, &tm)) return false;
    std::int32_t pen = 0;
    for (const char32_t cp : gdiDecodeCp932(cp932)) {
        GlyphMask mask;
        if (!gdiGlyph(fonts, desc, cp, &mask)) return false;
        pen += mask.advance;  // ★ 与 GGO 的 gmCellIncX / TextOut 的推进同一个字段
    }
    if (width) *width = pen;
    if (height) *height = tm.height;
    return true;
}

// =======================================================================================
// GGO 位图打包（纯函数）
// =======================================================================================
bool gdiPackGlyph(const GlyphMask& mask, std::uint32_t format, std::vector<std::uint8_t>* out,
                  std::uint32_t* pitch_out) {
    if (mask.black_box_x < 0 || mask.black_box_y < 0) return false;
    const int width = mask.black_box_x;
    const int height = mask.black_box_y;
    std::uint32_t pitch = 0;
    if (format == kGgoBitmap) {
        // 1bpp：每 32 像素一个 DWORD（行按 DWORD 对齐）
        pitch = static_cast<std::uint32_t>(((width + 31) >> 5) << 2);
    } else if (format == kGgoGray4 || format == kGgoGray8) {
        // 8bpp（每像素 1 字节；值域 0..16 / 0..64）；行按 DWORD 对齐
        pitch = static_cast<std::uint32_t>((width + 3) & ~3);
    } else {
        return false;  // GGO_NATIVE / GRAY2 / LCD 等：⛔ 不猜布局
    }
    const std::uint32_t total = pitch * static_cast<std::uint32_t>(height);
    if (pitch_out) *pitch_out = pitch;
    if (out == nullptr) return true;
    out->assign(total, 0);
    if (width == 0 || height == 0) return true;
    if (mask.coverage.size() < static_cast<std::size_t>(width) * static_cast<std::size_t>(height)) {
        return false;  // mask 自相矛盾（黑框与覆盖率长度对不上）
    }
    for (int row = 0; row < height; ++row) {
        std::uint8_t* dst = out->data() + static_cast<std::size_t>(row) * pitch;
        for (int col = 0; col < width; ++col) {
            const std::uint8_t coverage =
                mask.coverage[static_cast<std::size_t>(row) * width + col];
            if (format == kGgoBitmap) {
                if (coverage >= 128) {
                    dst[col >> 3] |= static_cast<std::uint8_t>(0x80u >> (col & 7));
                }
            } else {
                const std::uint32_t max_level = (format == kGgoGray4) ? kGray4MaxLevel : kGray8MaxLevel;
                // 连续覆盖率 → 量到 (max_level+1) 级：满覆盖 = max_level（16 / 64），⛔ 不是 2^n。
                const std::uint32_t level =
                    (static_cast<std::uint32_t>(coverage) * max_level + 127u) / 255u;
                dst[col] = static_cast<std::uint8_t>(std::min(level, max_level));
            }
        }
    }
    return true;
}

// =======================================================================================
// 绘制（表面 DC 真的写像素；屏幕 DC 只记账）
// =======================================================================================
TextDrawResult gdiDrawText(FontEngine& fonts, const GdiFontDesc& desc, const GdiDcState& dc,
                           int x, int y, std::string_view cp932) {
    TextDrawResult result;
    HostTextMetrics tm;
    if (!gdiTextMetrics(fonts, desc, &tm)) return result;

    SurfaceArena* arena = dc.surface_arena;
    GuestSurface* surface = nullptr;
    if (dc.surface_ea != 0) {
        if (arena == nullptr) {
            result.surface_missing = true;
            return result;
        }
        surface = arena->find(dc.surface_ea);
        if (surface == nullptr) {
            result.surface_missing = true;
            return result;
        }
    }
    std::uint8_t* pixels = nullptr;
    if (surface != nullptr) {
        pixels = arena->pixels(*surface);
        if (pixels == nullptr) {
            result.surface_missing = true;
            return result;
        }
    }

    const int lift = fontConfig().baseline_lift;  // ★ 宿主视觉策略（SCOPE 登记；0 = 关）
    const int baseline = y + tm.ascent - lift;
    const Rgb fg = decodeColor(dc.text_color);
    const Rgb bg = decodeColor(dc.bk_color);
    int pen = x;
    for (const char32_t cp : gdiDecodeCp932(cp932)) {
        GlyphMask mask;
        if (!gdiGlyph(fonts, desc, cp, &mask)) return result;  // 没有字体后端 ⇒ 整段失败
        if (surface != nullptr && dc.bk_mode == kOpaque) {
            // OPAQUE：先铺字形格的背景（GDI 的合同；引擎的 0x204 路径不走 TextOutA）
            fillSurfaceRect(*surface, pixels, pen, y, mask.advance, tm.height, bg);
        }
        std::uint32_t drawn = 0;
        for (int row = 0; row < mask.black_box_y; ++row) {
            for (int col = 0; col < mask.black_box_x; ++col) {
                const int coverage =
                    mask.coverage[static_cast<std::size_t>(row) * mask.black_box_x + col];
                if (coverage <= 0) continue;
                if (surface != nullptr) {
                    blendSurfacePixel(*surface, pixels, pen + mask.origin_x + col,
                                      baseline - mask.origin_y + row, fg, coverage, &drawn);
                } else {
                    ++drawn;  // 屏幕 DC：同一份覆盖率的**确定性记账**（不写任何宿主窗口）
                }
            }
        }
        result.pixels += drawn;
        ++result.glyphs;
        pen += mask.advance;
    }
    result.advance = pen - x;
    result.ok = true;
    return result;
}

// =======================================================================================
// ★ 与 P1 的接缝
// =======================================================================================
bool fontAttachDcSurface(std::uint32_t hdc, std::uint32_t surface_ea, SurfaceArena* arena) {
    Machine* machine = activeMachine();
    if (machine == nullptr) {
        std::fprintf(stderr, "[font] fontAttachDcSurface(0x%08X)：没有活动 Machine\n", hdc);
        return false;
    }
    GdiDcObject* dc = gdiAsDc(machine->handles(), hdc);
    if (dc == nullptr) {
        std::fprintf(stderr, "[font] fontAttachDcSurface(0x%08X)：不是 GDI DC 句柄\n", hdc);
        return false;
    }
    if (surface_ea != 0 && arena == nullptr) {
        std::fprintf(stderr, "[font] fontAttachDcSurface(0x%08X)：arena 为空（surface=%08X）\n", hdc,
                     surface_ea);
        return false;
    }
    dc->state.surface_ea = surface_ea;
    dc->state.surface_arena = arena;
    return true;
}

bool fontDetachDc(std::uint32_t hdc) {
    Machine* machine = activeMachine();
    if (machine == nullptr) {
        std::fprintf(stderr, "[font] fontDetachDc(0x%08X)：没有活动 Machine\n", hdc);
        return false;
    }
    GdiDcObject* dc = gdiAsDc(machine->handles(), hdc);
    if (dc == nullptr) {
        std::fprintf(stderr, "[font] fontDetachDc(0x%08X)：不是 GDI DC 句柄\n", hdc);
        return false;
    }
    dc->state.surface_ea = 0;
    dc->state.surface_arena = nullptr;
    return true;
}

}  // namespace lift
