#include "core/text_raster.h"

#include <cstdio>
#include <cstdlib>

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

// ★ 引擎的「缓存 ascent」来自字体重建入口用**常量字面名** "ＭＳ ゴシック" 建的参考字体
//   （`0x456D09`），不是渲染字体（`0x1a5` 设的 Amayui CN）。这是引擎的**偷懒假设**（两份字体 ascent
//   相近）；本 emulator **明确用渲染字体（Amayui CN）做这次量度**（宿主策略，不是引擎语义，见
//   `apps/emulator-cxx/README.md` 与 ops-forensics §28）：宿主替换了字面名，再按 MS Gothic 量会把
//   模式 1 的基线压低约 2px。引擎侧的参考字体行为仍记录在 spec 与取证里。

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

// ★ 字形像素的写入规则 = 引擎 `sub_46D9F0` 的合成（不是 alpha-over！）：
//   · 满覆盖（srcA = 255 / 1-bit 掩码的置位）⇒ **直接覆写**（同像素重复画幂等）；
//   · 否则 outA = max(dstA, srcA)、outC = (srcC*srcA + dstC*(MAX-srcA))/MAX。
//   引擎默认用 `GetGlyphOutlineA` 的 **GGO_BITMAP（1-bit 硬掩码）**（`Font+0x548 = 0`；只有
//   `set:EnableAntiFont` + `message:UseAntiFont` 才切 GRAY4/GRAY8）。1-bit 掩码的两趟只是
//   同一张掩码平移 (1,1) ⇒ **几何膨胀（右下加粗）**，不会把半透明覆盖率相加进笔画缝。
//   锚：0x46DA87-0x46DC36（16bpp）/ 0x46DCD0-0x46DE89（32bpp）；取证见 ops-forensics §32。
void compose_texel(TextureRecord& texture, int x, int y, const Rgb& color, int alpha) {
    if (x < 0 || x >= static_cast<int>(texture.width) || y < 0 ||
        y >= static_cast<int>(texture.height)) {
        return;
    }
    const std::size_t d = (static_cast<std::size_t>(y) * texture.width + x) * 4;
    std::uint8_t* texel = &texture.rgba[d];
    if (alpha >= 255) {  // 满覆盖 ⇒ 覆写
        texel[0] = static_cast<std::uint8_t>(color.r);
        texel[1] = static_cast<std::uint8_t>(color.g);
        texel[2] = static_cast<std::uint8_t>(color.b);
        texel[3] = 255;
        return;
    }
    const int dst_alpha = texel[3];
    const int out_alpha = std::max(dst_alpha, alpha);
    const int keep = 255 - alpha;
    texel[0] = static_cast<std::uint8_t>((color.r * alpha + texel[0] * keep) / 255);
    texel[1] = static_cast<std::uint8_t>((color.g * alpha + texel[1] * keep) / 255);
    texel[2] = static_cast<std::uint8_t>((color.b * alpha + texel[2] * keep) / 255);
    texel[3] = static_cast<std::uint8_t>(out_alpha);
}

// ★ 无离线 bake 数据时的宿主回退：FreeType 的**默认 render**（灰度 AA），并且**只画一趟**
//   （不画副色/径向/主色二遍 = 无阴影）。这是用户选定的回退形态，见 ops-forensics §37。
bool load_glyph(FT_Face face, std::uint32_t codepoint, bool mono) {
    const FT_Int32 flags = FT_LOAD_RENDER | (mono ? FT_LOAD_TARGET_MONO : FT_LOAD_TARGET_NORMAL);
    return FT_Load_Char(face, codepoint, flags) == 0;
}

void blit_glyph(TextureRecord& texture, const FT_Bitmap& bitmap, int origin_x, int origin_y,
                const Rgb& color, bool quantize, bool gray8) {
    for (unsigned row = 0; row < bitmap.rows; ++row) {
        for (unsigned column = 0; column < bitmap.width; ++column) {
            int value = 0;
            if (bitmap.pixel_mode == FT_PIXEL_MODE_MONO) {
                const unsigned char byte =
                    bitmap.buffer[static_cast<std::size_t>(row) * bitmap.pitch + column / 8];
                value = ((byte >> (7 - (column & 7))) & 1) != 0 ? 255 : 0;
            } else {
                value = bitmap.buffer[static_cast<std::size_t>(row) * bitmap.pitch + column];
            }
            if (value == 0) continue;
            if (quantize && bitmap.pixel_mode != FT_PIXEL_MODE_MONO) {
                const int levels = gray8 ? 64 : 16;
                const int level = (value * levels + 127) / 255;
                value = (level * 255 + levels / 2) / levels;
                if (value == 0) continue;
            }
            compose_texel(texture, origin_x + static_cast<int>(column),
                          origin_y + static_cast<int>(row), color, value);
        }
    }
}

// 离线 bake 数据的**存在性探测**（loader 尚未实现；见 §37）。有数据时保留引擎的两趟语义。
bool baked_glyphs_available() {
    const char* path = std::getenv("EMULATOR_CXX_GLYPH_ATLAS");
    if (path == nullptr || path[0] == '\0') return false;
    std::error_code ec;
    return std::filesystem::exists(std::filesystem::path(path), ec);
}

#endif  // EMULATOR_CXX_HAVE_TEXT

}  // namespace

void compose_glyph_texel(std::uint8_t* texel, int r, int g, int b, int alpha) {
    if (texel == nullptr) return;
    if (alpha >= 255) {  // 满覆盖 ⇒ 覆写
        texel[0] = static_cast<std::uint8_t>(r);
        texel[1] = static_cast<std::uint8_t>(g);
        texel[2] = static_cast<std::uint8_t>(b);
        texel[3] = 255;
        return;
    }
    const int dst_alpha = texel[3];
    const int out_alpha = std::max(dst_alpha, alpha);
    const int keep = 255 - alpha;
    texel[0] = static_cast<std::uint8_t>((r * alpha + texel[0] * keep) / 255);
    texel[1] = static_cast<std::uint8_t>((g * alpha + texel[1] * keep) / 255);
    texel[2] = static_cast<std::uint8_t>((b * alpha + texel[2] * keep) / 255);
    texel[3] = static_cast<std::uint8_t>(out_alpha);
}

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
    // 引擎的 AA 开关（bake 路径按它选格式：1-bit / GRAY4 / GRAY8）。无 bake 时不用它。
    const bool anti_alias = machine.config().get_int("set:EnableAntiFont") != 0 &&
                            machine.config().get_int("message:UseAntiFont") != 0;
    const bool gray8 = machine.config().get_int("message:AntiFontLevel") == 1;
    const int draw_mode = machine.font_draw_mode();
    const Rgb primary = decode_color(machine.font_primary_color());
    const Rgb secondary = decode_color(machine.font_secondary_color());
    const bool baked = baked_glyphs_available();
    const bool same_color = primary.r == secondary.r && primary.g == secondary.g &&
                            primary.b == secondary.b;
    // ★ §38：有 bake ⇒ 完全按引擎（格式看 AA 配置、两趟照引擎）；无 bake ⇒ 同色默认 **Bold 字体 +
    //   默认灰度 AA + 单趟**（`EMULATOR_CXX_SAME_COLOR_MONO` 回到 normal + mono），异色 灰度+两趟。
    const bool mono_same_switch = std::getenv("EMULATOR_CXX_SAME_COLOR_MONO") != nullptr;
    const bool use_bold = machine.font_metric_a() >= 700 ||
                          (!mono_same_switch && !baked && same_color);
    std::string path = default_font_path();
    if (path.empty()) {
        if (error != nullptr) *error = "font-path-missing";
        return false;
    }
    if (use_bold) {
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

    // ★ 度量：引擎的「缓存 ascent」来自重建入口用**常量字面名** "ＭＳ ゴシック" 建的参考字体，
    //   渲染字体则是对象当前字面名（本仓 Amayui CN）。引擎在赌两者 ascent 相近；本 emulator
    //   **明确用 Amayui CN（渲染字体）做这次量度**（宿主策略，不是引擎语义）：宿主替换了字面名，
    //   再按 MS Gothic 量会把模式 1 的基线压低约 2px。引擎侧的参考字体行为仍记录在 spec/取证。
    const int current_ascent = static_cast<int>(face->size->metrics.ascender >> 6);
    const int cached_ascent = current_ascent;  // emulator：用渲染字体（Amayui CN）量度

    int pen_x = x;
    // ★ `0x204` 的 y 语义 = **字形格顶**（不是基线）。引擎把 ascent 加两次、减一次
    //   （`sub_456710`（0x204 请求）→ 绘制入口 `sub_46F2D0` / `sub_471180`）：
    //   ① 对位模式 = 1 时 `y ← y + 缓存ascent − 本次ascent`（`0x4567AF`/`0x4567B5`/`0x4567B7`）；
    //   ② 绘制入口 `y ← y + 本次ascent`（`0x46F37C add [arg_8], eax` / `0x471253 add [arg_8], edx`，
    //      两者都是 tmAscent）⇒ 落字用的是**基线**；
    //   ③ 字形顶 = 基线 − `gmptGlyphOrigin.y`（`0x46F893`，≈ FreeType 的 `bitmap_top`）。
    //   ⇒ 对位模式 = 0 的基线 = `y + 本次ascent`；对位模式 = 1 的基线 = `y + 缓存ascent`（本次被消掉）。
    //   ★ 缓存 ascent 的写者是 **`GetTextMetricsA` 的指针出参**，不是 `mov [..+313F0h],..` ——
    //     字体重建入口 `sub_459F40` 把 `&Font[0x313EC]` 交给 `sub_456C90`（`0x459F8F`），后者在里面
    //     调 `GetTextMetricsA`（`0x456DA1`）⇒ `+0x313EC` = tmHeight、**`+0x313F0` = tmAscent**。
    //     所以“全语料 0 处写 = 恒 0”是假否定（按偏移 grep 看不见平台 API 的出参）。
    //   ★ 引擎量的是常量参考字体 "ＭＳ ゴシック"（`0x456D09`），不是渲染字体；本 emulator 选择
    //     用渲染字体重测（见上），所以 `cached_ascent == current_ascent`、模式 1 与模式 0 的基线相同。
    //     这条差异是**宿主策略**：引擎的参考字体数值仍登记在 RES-RENDER-TEXT-ALIGN-REFERENCE。
    //   ⛔ 把缓存当 0 会把整行抬高一个 ascent：2026-10-27 实测 CONFIG1.BIN 的纹理 196（30px 行带）
    //     文字被切成两半、首行还被纹理上缘裁掉。
    //   锚：`CORE-OPS-RENDER-TEXT-204`「两条绘制入口 / 纵向对位」；取证记录
    //   docs/02-engine/ops-forensics-2026-10-title-option.md §26 / §28。
    const int y_effective = machine.font_align_mode() == 1 ? y + cached_ascent - current_ascent : y;
    const int baseline = y_effective + current_ascent;
    // 投影偏移：引擎的偏移量由 `Font+0x568/0x56C` 给出（构造初值 (1,1)）；写入处
    // `0x4601E8`/`0x461803` 把**字体对象资源记录**（`Font+0x414` 数组里的对象）的
    // `[+0xC0]/[+0xC4]` 抄进这两个格。那是引擎自己的资源记录，不是 TTF 表，也不是配置键
    // （配置词表里没有 shadow/offset 键）；字段语义尚未逐字核。本模型没有该资源记录，用构造初值 (1,1)；副色与主色
    // 同色时（CONFIG1.BIN 的标签把两者都设成白）它就是 1px 膨胀 —— 这是脚本 + 引擎默认的直接结果，
    // ⛔ 不要在这里加"同色就跳过"的引擎没有的分支。真机/资源侧的偏移值见 ops-forensics §29。
    bool mono = false;
    bool quantize = false;
    if (baked) {
        // 有 bake ⇒ 完全按引擎：格式看 AA 配置。
        mono = !anti_alias;
        quantize = anti_alias;
    } else {
        // 无 bake + 同色：默认 Bold + 默认灰度 AA；`EMULATOR_CXX_SAME_COLOR_MONO=1` 回到 normal + 1-bit mono。
        mono = same_color && mono_same_switch;
        quantize = false;
    }
    const bool one_pass = !baked && same_color;
    if (baked) {
        static bool warned = false;
        if (!warned) {
            std::fprintf(stderr,
                         "[text] baked atlas present (%s) but loader is a stub; using FT glyphs + 2-pass\n",
                         std::getenv("EMULATOR_CXX_GLYPH_ATLAS"));
            warned = true;
        }
    }
    for (const std::uint32_t codepoint : decode_sjis(sjis)) {
        if (!load_glyph(face, codepoint, mono)) {
            pen_x += size / 2;
            continue;
        }
        const FT_GlyphSlot slot = face->glyph;
        const int advance = static_cast<int>(slot->advance.x >> 6) > 0
                                ? static_cast<int>(slot->advance.x >> 6)
                                : size / 2;
        const int origin_x = pen_x + slot->bitmap_left;
        const int origin_y = baseline - slot->bitmap_top;
        if (one_pass) {
            // 无 bake + 同色：mono + 单趟（无阴影）。
            blit_glyph(*texture, slot->bitmap, origin_x, origin_y, primary, quantize, gray8);
        } else {
            // 有 bake（引擎真实）或无 bake + 异色（阴影）：两趟/模式 2、3 照引擎。
            if (draw_mode == 1) {
                blit_glyph(*texture, slot->bitmap, origin_x + 1, origin_y + 1, secondary, quantize, gray8);
            } else if (draw_mode == 3) {
                for (int step = 1; step <= 4; ++step) {
                    blit_glyph(*texture, slot->bitmap, origin_x + step, origin_y + step, secondary, quantize, gray8);
                    blit_glyph(*texture, slot->bitmap, origin_x - step, origin_y - step, secondary, quantize, gray8);
                }
            }
            blit_glyph(*texture, slot->bitmap, origin_x, origin_y, primary, quantize, gray8);
            if (draw_mode == 2) {
                blit_glyph(*texture, slot->bitmap, origin_x, origin_y, primary, quantize, gray8);
            }
        }
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
