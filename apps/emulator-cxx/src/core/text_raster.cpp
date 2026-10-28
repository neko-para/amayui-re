#include "core/text_raster.h"

#include <cstdio>
#include <cstdlib>

#include "core/color.h"
#include "core/encoding.h"
#include "core/text_gdi.h"

#include <algorithm>
#include <cmath>
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

// ★ 宿主回退字形来源：FreeType 的**默认 render**（灰度 AA）。
//   只在"没有真机字形来源"时用它；两趟/一趟由调用方按来源决定（见 §37/§38）。
bool load_glyph(FT_Face face, std::uint32_t codepoint, bool mono) {
    const FT_Int32 flags = FT_LOAD_RENDER | (mono ? FT_LOAD_TARGET_MONO : FT_LOAD_TARGET_NORMAL);
    return FT_Load_Char(face, codepoint, flags) == 0;
}

// ★ 一幅字形位图的**平台无关视图**：来源可以是 FreeType（宿主回退）或 GDI（真机路径）。
//   下游的写入规则（引擎 `sub_46D9F0` 的满覆盖覆写 / max 合成）对两者是同一套。
enum class MaskKind {
    Mono,    // 1-bit 硬掩码：bit 置位 = 满覆盖
    FtGray,  // FreeType 的 0..255 灰度（宿主回退）
    Gray4,   // GDI `GGO_GRAY4_BITMAP` 的原始值 0..16
    Gray8,   // GDI `GGO_GRAY8_BITMAP` 的原始值 0..64
};

struct MaskView {
    const std::uint8_t* pixels = nullptr;
    int width = 0;
    int height = 0;
    int pitch = 0;
    MaskKind kind = MaskKind::Mono;
    bool quantize = false;  // 只对 FtGray 有意义（bake 路径按 17 / 65 级量化）
    bool gray8 = false;
};

int mask_alpha(const MaskView& mask, int row, int column) {
    const std::size_t offset =
        static_cast<std::size_t>(row) * static_cast<std::size_t>(mask.pitch) +
        static_cast<std::size_t>(column);
    switch (mask.kind) {
        case MaskKind::Mono: {
            const std::uint8_t byte = mask.pixels[static_cast<std::size_t>(row) *
                                                       static_cast<std::size_t>(mask.pitch) +
                                                   static_cast<std::size_t>(column / 8)];
            return ((byte >> (7 - (column & 7))) & 1) != 0 ? 255 : 0;
        }
        case MaskKind::Gray4: {
            // 引擎 `0x46DAAF`：alpha = value * 255 / 17（GRAY4 = 17 级；实测 GDI 的原始值 0..16
            // ⇒ alpha ∈ {15,30,…,240}，是 15 的倍数）。
            return static_cast<int>(mask.pixels[offset]) * 255 / 17;
        }
        case MaskKind::Gray8: {
            // 引擎 `0x46DA9C`：alpha = value * 255 / 65（GRAY8 = 65 级）。
            return std::min(255, static_cast<int>(mask.pixels[offset]) * 255 / 65);
        }
        case MaskKind::FtGray:
        default: {
            int value = mask.pixels[offset];
            if (mask.quantize) {
                const int levels = mask.gray8 ? 64 : 16;
                const int level = (value * levels + 127) / 255;
                value = (level * 255 + levels / 2) / levels;
            }
            return value;
        }
    }
}

void blit_mask(TextureRecord& texture, const MaskView& mask, int origin_x, int origin_y,
               const Rgb& color) {
    if (mask.pixels == nullptr || mask.pitch <= 0) return;
    for (int row = 0; row < mask.height; ++row) {
        for (int column = 0; column < mask.width; ++column) {
            const int value = mask_alpha(mask, row, column);
            if (value <= 0) continue;
            compose_texel(texture, origin_x + column, origin_y + row, color, value);
        }
    }
}

// ★ 引擎的投影偏移（`Font+0x568/0x56C`）：构造常量 (1,1)，全二进制没有第三条写入路径
//   （取证见 ops-forensics §29）。
constexpr int kProjectionX = 1;
constexpr int kProjectionY = 1;

// 引擎字体重建入口用的常量参考字面名（`0x456D09`；UTF-8 字面量，只在"比较 GDI 用的是哪份字体"时用）。
constexpr const char* kReferenceFaceUtf8 = "ＭＳ ゴシック";

// ★ 绘制模式 3 的**径向副色趟**：逐字按引擎画一圈副色后再画主色。
//
// 锚（`sub_471180` 的模式 3 支 + `sub_46ECC0`）：
//   · `0x471A53..0x471A8F`：`half = sqrt(off_y + off_x) * 8.0`（`dbl_526A98 = 8.0`）、
//     `step = 360.0 / half`（`dbl_526A90 = 360.0`）；`acc` 从 0 起；
//   · `0x471A95..0x471AE2`：每趟由角度取一点 —— `sub_46ECC0`（`0x46ECC6..0x46ED4E`）里
//     `rad = 角度 * 180 / π`（`dbl_5263F0 = 180.0`、`dbl_526A80 = π`），
//     `dx = round(sin(rad) * off_x)`、`dy = round(cos(rad) * off_y)`（先 ±0.5 再 (int) 截断）；
//     用副色把同一份字形位图画在 `(x + dx, y + dy)`；画完 `acc += step`；
//   · `0x471AFD`：`while (acc < 360.0)`（`flt_526A8C = 360.0`）。
// ⇒ 偏移恒为 (1,1) 时：`half = sqrt(2)*8 ≈ 11.31`、`step ≈ 31.82°` ⇒ **12 趟**；每趟的 (dx,dy)
//   落在 3×3 邻域内 ⇒ 副色只**外扩 1px**（一圈），不是 ±1..±4 的斜向光晕。
// ★ `rad = 角度 * 180 / π` 是引擎**逐字**的写法（不是 `* π / 180`）。照抄，不做"修正"：
//   本版的合同是"副色按多趟径向画"，半径/趟数/步长原先登记为未定（`RES-RENDER-TEXT-PROJECTION`），
//   这里把它改成**按站点取数**。
void draw_radial_outline(TextureRecord& texture, const MaskView& mask, int origin_x, int origin_y,
                         const Rgb& color, int off_x, int off_y) {
    const auto engine_round = [](double value) {
        // `sub_46ECC0` 的 (int) 之前那两步：>0 加 0.5、<0 减 0.5（⇒ 四舍五入，不是截断）。
        if (value > 0.0) return static_cast<int>(value + 0.5);
        if (value < 0.0) return static_cast<int>(value - 0.5);
        return static_cast<int>(value);
    };
    const double half =
        std::sqrt(static_cast<double>(off_y) + static_cast<double>(off_x)) * 8.0;
    // 偏移恒为 (1,1) ⇒ half = 11.31；只有两个偏移都为 0 才会到这里（那时引擎会画一趟 (0,0) 的
    // 副色 —— 恒等于主色的位置，不可观测）。这里直接返回：不可达 + 不伪造一趟。
    if (!(half > 0.0)) return;
    const double step = 360.0 / half;
    double angle = 0.0;
    do {
        // 引擎是 `角度 * 180 / π`：照抄（见上面的说明）。
        const double radians = angle * 180.0 / 3.14159265358979323846;
        const int dx = engine_round(std::sin(radians) * static_cast<double>(off_x));
        const int dy = engine_round(std::cos(radians) * static_cast<double>(off_y));
        blit_mask(texture, mask, origin_x + dx, origin_y + dy, color);
        angle += step;
    } while (angle < 360.0);
}

// 离线 bake 数据的**存在性探测**（loader 尚未实现；见 §37）。有数据时保留引擎的两趟语义。
bool baked_glyphs_available() {
    const char* path = std::getenv("EMULATOR_CXX_GLYPH_ATLAS");
    if (path == nullptr || path[0] == '\0') return false;
    std::error_code ec;
    return std::filesystem::exists(std::filesystem::path(path), ec);
}

// ★ 真机字形来源（Windows GDI）：`EMULATOR_CXX_GLYPH_GDI` 非空且不是 "0" ⇒ 开（`=false` 也算开：
//   这个开关的口径是"设了就开"，与 `EMULATOR_CXX_SAME_COLOR_MONO` 那类"存在即开"一致）。
//   语义：字型 / hinting / 栅格化 / 灰度级数都来自平台（引擎就是用 `GetGlyphOutline` 要字形），
//   ⇒ 这一档**完全按引擎**（格式由 AA 配置决定、两趟照引擎），不是观感选择。
bool gdi_glyphs_enabled() {
    const char* env = std::getenv("EMULATOR_CXX_GLYPH_GDI");
    if (env == nullptr || env[0] == '\0' || env[0] == '0') return false;
    return gdi_glyph_available();
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

namespace {
TextGlyphSource g_last_glyph_source = TextGlyphSource::None;
}  // namespace

TextGlyphSource last_glyph_source() { return g_last_glyph_source; }

bool draw_text_into_texture(Machine& machine, std::uint32_t texture_id, int x, int y,
                            const std::string& sjis, std::string* error) {
#ifdef EMULATOR_CXX_HAVE_TEXT
    TextureRecord* texture = machine.texture_mut(texture_id);
    if (texture == nullptr || texture->rgba.empty() || sjis.empty()) return false;

    const int size = machine.font_size() > 0 ? machine.font_size() : 24;
    // ★ 诊断（`EMULATOR_CXX_TRACE_TEXT=1`）：把**实际用来出字形的字号**打出来。
    //   判据用：配置界面里 0x204 必须按 A 支字号画（`0x75` 写的那支），⛔ 不是 `0x197` 的 B 支。
    // ★ 诊断（`EMULATOR_CXX_TRACE_TEXT=1`）：本函数在 `vm_ops_text.cpp` 的匿名命名空间里没有那个 helper，
    //   这里就地读环境变量（同一个开关、同一个语义）。
    {
        static const bool trace = std::getenv("EMULATOR_CXX_TRACE_TEXT") != nullptr;
        if (trace) std::fprintf(stderr, "[text] draw size=%d chars=%zu\n", size, sjis.size());
    }
    // 引擎的 AA 开关（决定字形格式：1-bit / GRAY4 / GRAY8；见 §32/§33）。只在"真机字形来源"下用。
    const bool anti_alias = machine.config().get_int("set:EnableAntiFont") != 0 &&
                            machine.config().get_int("message:UseAntiFont") != 0;
    const bool gray8 = machine.config().get_int("message:AntiFontLevel") == 1;
    const int draw_mode = machine.font_draw_mode();
    const Rgb primary = decode_color(machine.font_primary_color());
    const Rgb secondary = decode_color(machine.font_secondary_color());
    // ★ 字形来源三档（§37/§38 + 本轮的 GDI 档）：
    //   ① offline bake（`EMULATOR_CXX_GLYPH_ATLAS` 指向的文件存在）—— loader 仍是 stub；
    //   ② **Windows GDI**（`EMULATOR_CXX_GLYPH_GDI=1`）—— **真机路径**：字型/hinting/栅格化/灰度级数
    //      都来自平台，引擎本来就是向 `GetGlyphOutline` 要字形；
    //   ③ 宿主回退（默认）—— FreeType 字形 + 观感方案。
    //   ①②都算"引擎字形"⇒ 格式看 AA 配置、两趟照引擎；③才是宿主观感选择。
    const bool gdi = gdi_glyphs_enabled();
    const bool baked = !gdi && baked_glyphs_available();
    const bool engine_glyphs = gdi || baked;
    g_last_glyph_source = gdi ? TextGlyphSource::Gdi : TextGlyphSource::FreeType;
    const bool same_color = primary.r == secondary.r && primary.g == secondary.g &&
                            primary.b == secondary.b;
    // ★ §38：引擎字形 ⇒ 完全按引擎（格式看 AA 配置、两趟照引擎）；宿主回退 ⇒ 同色默认 **Bold 字体 +
    //   默认灰度 AA + 单趟**（`EMULATOR_CXX_SAME_COLOR_MONO` 回到 normal + mono），异色 灰度+两趟。
    const bool mono_same_switch = std::getenv("EMULATOR_CXX_SAME_COLOR_MONO") != nullptr;
    const bool use_bold = engine_glyphs ? machine.font_metric_a() >= 700
                                        : (machine.font_metric_a() >= 700 ||
                                           (!mono_same_switch && same_color));

    int current_ascent = 0;
    FT_Face face = nullptr;
    std::string path;
    GdiFormat gdi_format = GdiFormat::Mono;
    std::wstring gdi_face;
    if (gdi) {
        // ★ **字面名的宿主策略在 GDI 档同样生效**（SCOPE §1.1：引擎写的 `0x1a5` 一律忽略、一律用
        //   宿主字体 Amayui CN —— 译文是中文，引擎那份 MS Gothic 覆盖不了简体字形，也带日文字形）。
        //   `EMULATOR_CXX_GLYPH_GDI_FACE=engine` 是**显式对照档**：按引擎的字面名取字（纯真机字型，
        //   用来 A/B 对比）；给别的值则表示"用这个族名"。
        gdi_face = gdi_face_from_utf8("Amayui CN");
        if (const char* env = std::getenv("EMULATOR_CXX_GLYPH_GDI_FACE");
            env != nullptr && env[0] != '\0') {
            gdi_face = std::string(env) == "engine" ? gdi_face_from_sjis(machine.font_string())
                                                    : gdi_face_from_utf8(env);
        }
        // 真机路径：量度也走 GDI（引擎的"本次量度 ascent"就是 `GetTextMetricsA` 的 tmAscent）。
        if (!gdi_font_ascent(gdi_face, size, machine.font_metric_a(), &current_ascent)) {
            if (error != nullptr) *error = "gdi-font-metrics-failed";
            return false;
        }
        if (anti_alias) gdi_format = gray8 ? GdiFormat::Gray8 : GdiFormat::Gray4;
    } else {
        path = default_font_path();
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
        face = font_cache().face(path);
        if (face == nullptr) {
            if (error != nullptr) *error = "font-load-failed:" + path;
            return false;
        }
        if (FT_Set_Pixel_Sizes(face, 0, static_cast<FT_UInt>(std::clamp(size, 1, 512))) != 0) {
            if (error != nullptr) *error = "font-size-failed";
            return false;
        }
        current_ascent = static_cast<int>(face->size->metrics.ascender >> 6);
    }

    // ★ 度量：引擎的「缓存 ascent」来自重建入口用**常量字面名** "ＭＳ ゴシック" 建的参考字体，
    //   渲染字体则是对象当前字面名（本仓 Amayui CN）。引擎在赌两者 ascent 相近；本 emulator
    //   **固定用渲染字体（Amayui CN）做这次量度**（宿主策略，见 SCOPE §1.1 / ops-forensics §73）。
    //   ⇒ 模式 1 与模式 0 的基线相同，量度只依赖"我们真正在用的那份字体"。
    const int cached_ascent = current_ascent;

    // ★ 宿主对齐补偿（2026-10-28 用户口径）：替换字体之后**墨迹整体偏低**，因为 Amayui 的 ascent
    //   比引擎的参考字体大（GDI 实测：20px 19 vs 17、28px 27 vs 24），而引擎的"字形格顶"是按参考
    //   字体量出来的 ⇒ 多出来的那 2~3px 全落在下方。这里把基线再抬 **2px** 补回来（宿主视觉策略，
    //   ⛔ 不是引擎语义：引擎没有这条，它的对位合同仍是"cached / current 两档"）。
    //   `EMULATOR_CXX_BASELINE_LIFT=0` 可关掉（对比用），其它整数值可微调。
    //   ★ 只在"字体被宿主替换"时补：GDI 档若显式用引擎的字面名（`GLYPH_GDI_FACE=engine`，
    //     即真机那份 MS Gothic），量度与字形同源、位置本来就是对的 ⇒ 补偿 = 0。
    const bool host_face_in_use =
        !gdi || gdi_face != gdi_face_from_utf8(kReferenceFaceUtf8);
    int lift = 0;
    if (host_face_in_use) {
        lift = 2;
        if (const char* env = std::getenv("EMULATOR_CXX_BASELINE_LIFT");
            env != nullptr && env[0] != '\0') {
            lift = std::atoi(env);
        }
    }

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
    // ★ 基线 = 引擎算出来的那个再减掉宿主对齐补偿（见上：替换字体后墨迹偏低 2px）。
    const int baseline = y_effective + current_ascent - lift;
    bool mono = false;
    bool quantize = false;
    if (engine_glyphs) {
        // 引擎字形 ⇒ 完全按引擎：格式看 AA 配置（1-bit / GRAY4 / GRAY8）。
        mono = !anti_alias;
        quantize = anti_alias;
    } else {
        // 宿主回退 + 同色：默认 Bold + 默认灰度 AA；`EMULATOR_CXX_SAME_COLOR_MONO=1` 回到 normal + mono。
        mono = same_color && mono_same_switch;
        quantize = false;
    }
    const bool one_pass = !engine_glyphs && same_color;
    // 诊断口（宿主，⛔ 不是产品路径）：`EMULATOR_CXX_TEXT_TRACE=1` 逐次打印 0x204 的参数形状。
    // 用来回答"这一段文字是几趟、什么颜色、什么绘制模式、字形从哪来"，不改变任何绘制行为。
    if (std::getenv("EMULATOR_CXX_TEXT_TRACE") != nullptr) {
        std::fprintf(stderr,
                     "[text] tex=%u x=%d y=%d size=%d mode=%d align=%d primary=%02X%02X%02X "
                     "secondary=%02X%02X%02X src=%s same=%d one_pass=%d ascent=%d cached=%d "
                     "baseline=%d lift=%d face=\"%s\" utf8=\"%s\"\n",
                     texture_id, x, y, size, draw_mode, machine.font_align_mode(), primary.r, primary.g,
                     primary.b, secondary.r, secondary.g, secondary.b,
                     gdi ? "gdi" : (baked ? "bake" : "ft"), same_color ? 1 : 0, one_pass ? 1 : 0,
                     current_ascent, cached_ascent, baseline, lift,
                     cp932_to_utf8(machine.font_string()).c_str(), cp932_to_utf8(sjis).c_str());
    }
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
        MaskView mask;
        GdiGlyph gdi_glyph;  // 生命周期 = 本次迭代（`mask.pixels` 指向它）
        int advance = 0;
        int origin_x = 0;
        int origin_y = 0;
        if (gdi) {
            if (!gdi_load_glyph(gdi_face, size, machine.font_metric_a(), gdi_format, codepoint,
                                &gdi_glyph)) {
                pen_x += size / 2;
                continue;
            }
            advance = gdi_glyph.advance > 0 ? gdi_glyph.advance : size / 2;
            origin_x = pen_x + gdi_glyph.left;
            origin_y = baseline - gdi_glyph.top;
            // 诊断（`EMULATOR_CXX_TEXT_TRACE=1`）：GDI 的灰度档到底给 0..16 / 0..64 还是 0..255。
            // 只报一次最大值 —— 引擎按 /17、/65 换算（`0x46DAAF` / `0x46DA9C`），值域不对会**饱和**。
            if (std::getenv("EMULATOR_CXX_TEXT_TRACE") != nullptr &&
                gdi_format != GdiFormat::Mono) {
                static int reported = -1;
                if (gdi_glyph.max_value > reported) {
                    reported = gdi_glyph.max_value;
                    std::fprintf(stderr, "[text] gdi gray max raw value = %d (format=%d)\n",
                                 reported, static_cast<int>(gdi_format));
                }
            }
            mask.pixels = gdi_glyph.pixels.empty() ? nullptr : gdi_glyph.pixels.data();
            mask.width = gdi_glyph.width;
            mask.height = gdi_glyph.height;
            mask.pitch = gdi_glyph.pitch;
            mask.kind = gdi_format == GdiFormat::Mono
                            ? MaskKind::Mono
                            : (gdi_format == GdiFormat::Gray8 ? MaskKind::Gray8 : MaskKind::Gray4);
        } else {
            if (!load_glyph(face, codepoint, mono)) {
                pen_x += size / 2;
                continue;
            }
            const FT_GlyphSlot slot = face->glyph;
            advance = static_cast<int>(slot->advance.x >> 6) > 0
                          ? static_cast<int>(slot->advance.x >> 6)
                          : size / 2;
            origin_x = pen_x + slot->bitmap_left;
            origin_y = baseline - slot->bitmap_top;
            mask.pixels = slot->bitmap.buffer;
            mask.width = static_cast<int>(slot->bitmap.width);
            mask.height = static_cast<int>(slot->bitmap.rows);
            mask.pitch = static_cast<int>(slot->bitmap.pitch);
            mask.kind = slot->bitmap.pixel_mode == FT_PIXEL_MODE_MONO ? MaskKind::Mono
                                                                     : MaskKind::FtGray;
            mask.quantize = quantize;
            mask.gray8 = gray8;
        }
        if (one_pass) {
            // 宿主回退 + 同色：单趟（无阴影）。
            blit_mask(*texture, mask, origin_x, origin_y, primary);
        } else {
            // 引擎字形（bake / GDI）或宿主回退 + 异色（阴影）：两趟 / 模式 2、3 照引擎。
            if (draw_mode == 1) {
                blit_mask(*texture, mask, origin_x + kProjectionX, origin_y + kProjectionY,
                          secondary);
            } else if (draw_mode == 3) {
                draw_radial_outline(*texture, mask, origin_x, origin_y, secondary, kProjectionX,
                                    kProjectionY);
            }
            blit_mask(*texture, mask, origin_x, origin_y, primary);
            if (draw_mode == 2) {
                blit_mask(*texture, mask, origin_x, origin_y, primary);
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
