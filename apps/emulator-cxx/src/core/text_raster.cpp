#include "core/text_raster.h"

#include "core/color.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#ifdef EMULATOR_CXX_HAVE_TEXT
#include <ft2build.h>
#include FT_FREETYPE_H
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#elif defined(EMULATOR_CXX_HAVE_ICONV)
#include <iconv.h>
#endif
#endif

namespace core {
namespace {

void blend_texel(TextureRecord& texture, int x, int y, int r, int g, int b, int alpha) {
    if (x < 0 || x >= static_cast<int>(texture.width) || y < 0 || y >= static_cast<int>(texture.height)) {
        return;
    }
    const std::size_t d = (static_cast<std::size_t>(y) * texture.width + x) * 4;
    blend_argb_over(&texture.rgba[d], r, g, b, alpha);
}

#ifdef EMULATOR_CXX_HAVE_TEXT

std::string default_font_path() {
    if (const char* env = std::getenv("EMULATOR_CXX_FONT"); env != nullptr && env[0] != '\0') {
        return env;
    }
#ifdef EMULATOR_CXX_FONT_PATH
    return EMULATOR_CXX_FONT_PATH;
#else
    return {};
#endif
}

std::string bold_font_path(const std::string& regular) {
    const auto dot = regular.rfind(".ttf");
    if (dot == std::string::npos) return regular;
    return regular.substr(0, dot) + "-Bold.ttf";
}

// Shift-JIS（CP932）字节串 → Unicode 码点。用平台自带的代码页转换，不手写映射表：
//   Windows = `MultiByteToWideChar(932, ...)`；其它平台 = 系统 iconv（`SHIFT_JIS` / `CP932`）。
// 未定义序列按 1 字节跳过（引擎的逐字推进不受影响）。
std::vector<std::uint32_t> decode_sjis(const std::string& bytes) {
    std::vector<std::uint32_t> codepoints;
    if (bytes.empty()) return codepoints;
#ifdef _WIN32
    int wide_length = MultiByteToWideChar(932, MB_ERR_INVALID_CHARS, bytes.data(),
                                          static_cast<int>(bytes.size()), nullptr, 0);
    if (wide_length <= 0) {
        wide_length = MultiByteToWideChar(932, 0, bytes.data(), static_cast<int>(bytes.size()),
                                          nullptr, 0);
    }
    if (wide_length <= 0) return codepoints;
    std::wstring wide(static_cast<std::size_t>(wide_length), L'\0');
    MultiByteToWideChar(932, 0, bytes.data(), static_cast<int>(bytes.size()), wide.data(),
                        wide_length);
    for (const wchar_t value : wide) codepoints.push_back(static_cast<std::uint32_t>(value));
    return codepoints;
#elif defined(EMULATOR_CXX_HAVE_ICONV)
    const char* names[] = {"CP932", "SHIFT_JIS", "SJIS"};
    for (const char* name : names) {
        iconv_t cd = iconv_open("UTF-8", name);
        if (cd == reinterpret_cast<iconv_t>(-1)) continue;
        std::vector<char> output(bytes.size() * 4 + 16, 0);
        char* in_ptr = const_cast<char*>(bytes.data());
        std::size_t in_left = bytes.size();
        char* out_ptr = output.data();
        std::size_t out_left = output.size();
        const std::size_t result = iconv(cd, &in_ptr, &in_left, &out_ptr, &out_left);
        iconv_close(cd);
        if (result == static_cast<std::size_t>(-1)) continue;
        const std::size_t produced = output.size() - out_left;
        std::size_t index = 0;
        while (index < produced) {
            const auto lead = static_cast<unsigned char>(output[index]);
            std::uint32_t cp = 0;
            std::size_t length = 0;
            if ((lead & 0x80u) == 0) {
                cp = lead;
                length = 1;
            } else if ((lead & 0xE0u) == 0xC0u && index + 1 < produced) {
                cp = ((lead & 0x1Fu) << 6) | (static_cast<unsigned char>(output[index + 1]) & 0x3Fu);
                length = 2;
            } else if ((lead & 0xF0u) == 0xE0u && index + 2 < produced) {
                cp = ((lead & 0x0Fu) << 12) |
                     ((static_cast<unsigned char>(output[index + 1]) & 0x3Fu) << 6) |
                     (static_cast<unsigned char>(output[index + 2]) & 0x3Fu);
                length = 3;
            } else if ((lead & 0xF8u) == 0xF0u && index + 3 < produced) {
                cp = ((lead & 0x07u) << 18) |
                     ((static_cast<unsigned char>(output[index + 1]) & 0x3Fu) << 12) |
                     ((static_cast<unsigned char>(output[index + 2]) & 0x3Fu) << 6) |
                     (static_cast<unsigned char>(output[index + 3]) & 0x3Fu);
                length = 4;
            } else {
                index += 1;
                continue;
            }
            codepoints.push_back(cp);
            index += length;
        }
        if (!codepoints.empty()) return codepoints;
    }
    return codepoints;
#else
    for (const unsigned char byte : bytes) codepoints.push_back(byte);
    return codepoints;
#endif
}

struct FontCache {
    FT_Library library = nullptr;
    bool initialized = false;
    std::map<std::string, FT_Face> faces;

    FT_Face face(const std::string& path) {
        if (!initialized) {
            initialized = true;
            if (FT_Init_FreeType(&library) != 0) library = nullptr;
        }
        if (library == nullptr || path.empty()) return nullptr;
        const auto it = faces.find(path);
        if (it != faces.end()) return it->second;
        FT_Face loaded = nullptr;
        if (FT_New_Face(library, path.c_str(), 0, &loaded) != 0) return nullptr;
        faces[path] = loaded;
        return loaded;
    }
};

FontCache& font_cache() {
    static FontCache cache;
    return cache;
}

// 字体对象的主色 / 副色是「平台字节序」存的值（低字节 = 蓝、最高字节不用）。
struct Rgb {
    int r = 255;
    int g = 255;
    int b = 255;
};

Rgb decode_color(int stored) {
    Rgb rgb;
    rgb.r = stored & 0xFF;
    rgb.g = (stored >> 8) & 0xFF;
    rgb.b = (stored >> 16) & 0xFF;
    return rgb;
}

void draw_glyph(TextureRecord& texture, FT_Face face, std::uint32_t codepoint, int pen_x, int pen_y,
                const Rgb& color) {
    if (FT_Load_Char(face, codepoint, FT_LOAD_RENDER) != 0) return;
    const FT_GlyphSlot slot = face->glyph;
    const FT_Bitmap& bitmap = slot->bitmap;
    const int origin_x = pen_x + slot->bitmap_left;
    const int origin_y = pen_y - slot->bitmap_top;
    for (unsigned row = 0; row < bitmap.rows; ++row) {
        for (unsigned column = 0; column < bitmap.width; ++column) {
            const unsigned char value =
                bitmap.buffer[static_cast<std::size_t>(row) * bitmap.pitch + column];
            if (value == 0) continue;
            blend_texel(texture, origin_x + static_cast<int>(column),
                        origin_y + static_cast<int>(row), color.r, color.g, color.b, value);
        }
    }
}

#endif  // EMULATOR_CXX_HAVE_TEXT

}  // namespace

bool text_backend_available() {
#ifdef EMULATOR_CXX_HAVE_TEXT
    return true;
#else
    return false;
#endif
}

bool draw_text_into_texture(Machine& machine, std::uint32_t texture_id, int x, int y,
                            const std::string& sjis, std::string* error) {
#ifdef EMULATOR_CXX_HAVE_TEXT
    TextureRecord* texture = machine.texture_mut(texture_id);
    if (texture == nullptr || texture->rgba.empty() || sjis.empty()) return false;

    const int size = machine.font_size() > 0 ? machine.font_size() : 24;
    const bool bold = machine.font_metric_a() >= 700;
    std::string path = default_font_path();
    if (path.empty()) {
        if (error != nullptr) *error = "font-path-missing";
        return false;
    }
    if (bold) {
        // 粗体文件不存在时退回常规字面；字重是字体对象的一格，不改变字符编码。
        const std::string candidate = bold_font_path(path);
        std::error_code ignored;
        if (std::filesystem::exists(candidate, ignored)) path = candidate;
    }
    FT_Face face = font_cache().face(path);
    if (face == nullptr) {
        if (error != nullptr) *error = "font-load-failed:" + path;
        return false;
    }
    if (FT_Set_Pixel_Sizes(face, 0, static_cast<FT_UInt>(std::clamp(size, 1, 512))) != 0) {
        if (error != nullptr) *error = "font-size-failed";
        return false;
    }

    const int ascent = static_cast<int>(face->size->metrics.ascender >> 6);
    const int draw_mode = machine.font_draw_mode();
    const Rgb primary = decode_color(machine.font_primary_color());
    const Rgb secondary = decode_color(machine.font_secondary_color());

    int pen_x = x;
    const int baseline = y + ascent;
    for (const std::uint32_t codepoint : decode_sjis(sjis)) {
        // 投影偏移的起点：模式 0 只画主色；1 = 副色 (+1,+1) 再主色；2 = 主色两遍；3 = 多趟径向副色。
        if (draw_mode == 1) {
            draw_glyph(*texture, face, codepoint, pen_x + 1, baseline + 1, secondary);
        } else if (draw_mode == 3) {
            for (int step = 1; step <= 4; ++step) {
                draw_glyph(*texture, face, codepoint, pen_x + step, baseline + step, secondary);
                draw_glyph(*texture, face, codepoint, pen_x - step, baseline - step, secondary);
            }
        }
        draw_glyph(*texture, face, codepoint, pen_x, baseline, primary);
        if (draw_mode == 2) {
            draw_glyph(*texture, face, codepoint, pen_x, baseline, primary);
        }
        if (FT_Load_Char(face, codepoint, FT_LOAD_DEFAULT) != 0) {
            pen_x += size / 2;
            continue;
        }
        int advance = static_cast<int>(face->glyph->advance.x >> 6);
        if (advance <= 0) advance = size / 2;
        pen_x += advance;
    }
    texture->non_file = true;
    return true;
#else
    (void)machine;
    (void)texture_id;
    (void)x;
    (void)y;
    (void)sjis;
    if (error != nullptr) *error = "text-backend-missing";
    return false;
#endif
}

}  // namespace core
