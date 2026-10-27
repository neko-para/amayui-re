// Windows GDI 字形提供者（引擎的真机字形来源；见 `core/text_gdi.h` 的说明）。
//
// ★ 这里刻意**不**做任何"观感调整"：不问 AA 配置以外的任何东西、不改字形、不合成粗体。
//   `GetGlyphOutline` 给出什么就写什么 —— 格式与级数由调用方按引擎的 AA 开关选定。
// ★ 宿主侧唯一的自由：字面名。引擎把 `0x1a5` 写的那串 CP932 名字塞进 LOGFONT；本机若没装那个字
//   （译文的 Amayui CN 只在本仓 `corpus/assets/fonts/` 下），先把该 TTF 按 `FR_PRIVATE` 挂进来，
//   于是 GDI 仍按**引擎给的族名**找字 —— 名字对不上时 GDI 会自己替换（那也正是真机的行为）。
#include "core/text_gdi.h"

#include "core/encoding.h"

#include <filesystem>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>
#endif
namespace core {
namespace {

#ifdef _WIN32

std::wstring sjis_to_wide(const std::string& bytes) {
    if (bytes.empty()) return {};
    int wide_length = MultiByteToWideChar(932, 0, bytes.data(), static_cast<int>(bytes.size()),
                                          nullptr, 0);
    if (wide_length <= 0) return {};
    std::wstring wide(static_cast<std::size_t>(wide_length), L'\0');
    MultiByteToWideChar(932, 0, bytes.data(), static_cast<int>(bytes.size()), wide.data(),
                        wide_length);
    return wide;
}

std::string host_font_path() {
    if (const char* env = std::getenv("EMULATOR_CXX_FONT"); env != nullptr && env[0] != '\0') {
        return env;
    }
#ifdef EMULATOR_CXX_FONT_PATH
    return EMULATOR_CXX_FONT_PATH;
#else
    return {};
#endif
}

// 引擎没给字面名时用的兜底族名 = 本仓宿主字体的族名（`Amayui-CN_cnjp.ttf` 的 name 表 ID 1）。
constexpr wchar_t kHostFontFamily[] = L"Amayui CN";

void load_private_font() {
    static bool attempted = false;
    if (attempted) return;
    attempted = true;
    const std::string path = host_font_path();
    if (path.empty()) return;
    std::error_code ec;
    const std::filesystem::path native = path_from_utf8(path);
    if (!std::filesystem::exists(native, ec)) return;
    AddFontResourceExW(native.c_str(), FR_PRIVATE, nullptr);
}

// 一份"当前字体"的缓存：引擎的字体对象是唯一实例，所以一张缓存就够（换字面名 / 字号 / 粗细才重建）。
struct GdiState {
    HDC dc = nullptr;
    HFONT font = nullptr;
    std::wstring face;
    int size = -1;
    int weight = -1;
    bool broken = false;
};

GdiState& state() {
    static GdiState value;
    return value;
}

bool ensure_font(const std::wstring& face_in, int size, int weight) {
    GdiState& s = state();
    if (s.broken) return false;
    if (s.dc == nullptr) {
        load_private_font();
        s.dc = CreateCompatibleDC(nullptr);
        if (s.dc == nullptr) {
            s.broken = true;
            return false;
        }
        // 引擎只按标识矩阵取字形（`GetGlyphOutline` 的 MAT2 恒为单位阵）；
        // MSDN 要求先切成 GM_ADVANCED 才是"按世界变换"的语义。
        SetGraphicsMode(s.dc, GM_ADVANCED);
    }
    std::wstring face = face_in;
    if (face.empty()) face = kHostFontFamily;
    const int weight_effective = weight >= 700 ? 700 : (weight > 0 ? weight : 400);
    if (s.font != nullptr && s.face == face && s.size == size && s.weight == weight_effective) {
        return true;
    }
    if (s.font != nullptr) {
        DeleteObject(s.font);
        s.font = nullptr;
    }
    LOGFONTW lf{};
    lf.lfHeight = -size;  // 引擎：lfHeight = −字号（负 = 按字符高度）
    lf.lfWidth = 0;       // ★ 引擎的 LOGFONT 带 lfWidth = 高度/2；可缩放字面下 GDI 不据此拉伸
                          //   （Wine 的等价路径是 `FT_Set_Pixel_Sizes(face, 0, height)`）⇒ 传 0。
    lf.lfWeight = weight_effective;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfQuality = DEFAULT_QUALITY;  // 引擎 lfQuality 恒 0（唯一写点 0x4653E4）
    lf.lfPitchAndFamily = DEFAULT_PITCH | FF_DONTCARE;
    if (face.size() >= LF_FACESIZE) face.resize(LF_FACESIZE - 1);
    std::memcpy(lf.lfFaceName, face.c_str(), (face.size() + 1) * sizeof(wchar_t));
    s.font = CreateFontIndirectW(&lf);
    if (s.font == nullptr) {
        s.broken = true;
        return false;
    }
    SelectObject(s.dc, s.font);
    s.face = face;
    s.size = size;
    s.weight = weight_effective;
    return true;
}

UINT ggo_format(GdiFormat format) {
    switch (format) {
        case GdiFormat::Gray4: return GGO_GRAY4_BITMAP;
        case GdiFormat::Gray8: return GGO_GRAY8_BITMAP;
        case GdiFormat::Mono:
        default: return GGO_BITMAP;
    }
}

#endif  // _WIN32

}  // namespace

bool gdi_glyph_available() {
#ifdef _WIN32
    return !state().broken;
#else
    return false;
#endif
}

std::wstring gdi_face_from_sjis(const std::string& cp932) {
#ifdef _WIN32
    return sjis_to_wide(cp932);
#else
    (void)cp932;
    return {};
#endif
}

std::wstring gdi_face_from_utf8(const std::string& utf8) {
#ifdef _WIN32
    if (utf8.empty()) return {};
    int wide_length = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()),
                                          nullptr, 0);
    if (wide_length <= 0) return {};
    std::wstring wide(static_cast<std::size_t>(wide_length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(),
                        wide_length);
    return wide;
#else
    (void)utf8;
    return {};
#endif
}

bool gdi_font_ascent(const std::wstring& face, int size, int weight, int* ascent) {
#ifdef _WIN32
    if (ascent == nullptr) return false;
    if (!ensure_font(face, size, weight)) return false;
    TEXTMETRICW metrics{};
    if (GetTextMetricsW(state().dc, &metrics) == 0) return false;
    *ascent = static_cast<int>(metrics.tmAscent);
    return true;
#else
    (void)face;
    (void)size;
    (void)weight;
    (void)ascent;
    return false;
#endif
}

bool gdi_load_glyph(const std::wstring& face, int size, int weight, GdiFormat format,
                    std::uint32_t codepoint, GdiGlyph* out) {
#ifdef _WIN32
    if (out == nullptr) return false;
    if (size <= 0) return false;
    if (!ensure_font(face, size, weight)) return false;
    HDC dc = state().dc;
    const UINT fu_format = ggo_format(format);

    // 标识矩阵：`FIXED` 是 16.16（value 整数部分、fract 小数部分）。
    MAT2 transform{};
    transform.eM11.value = 1;
    transform.eM22.value = 1;

    GLYPHMETRICS metrics{};
    const DWORD needed =
        GetGlyphOutlineW(dc, static_cast<UINT>(codepoint), fu_format, &metrics, 0, nullptr, &transform);
    if (needed == GDI_ERROR) return false;

    const int width = static_cast<int>(metrics.gmBlackBoxX);
    const int height = static_cast<int>(metrics.gmBlackBoxY);
    // 返回缓冲的行距：单色按 DWORD 对齐，灰度按 4 字节对齐（与 Wine 的 get_*_glyph_bitmap 同形）。
    const int pitch =
        format == GdiFormat::Mono ? ((width + 31) / 32) * 4 : ((width + 3) / 4) * 4;
    out->width = width;
    out->height = height;
    out->pitch = pitch;
    out->left = static_cast<int>(metrics.gmptGlyphOrigin.x);
    out->top = static_cast<int>(metrics.gmptGlyphOrigin.y);
    out->advance = static_cast<int>(metrics.gmCellIncX);
    out->max_value = 0;
    out->pixels.clear();
    if (needed > 0) {
        // 行距假设的自检：GDI 的返回长度必须正好等于"对齐后的行距 × 行数"。
        // 对不上 ⇒ 我们对布局的假设是错的（读下去会斜切 / 越界），宁可不画也不要读错。
        if (needed != static_cast<DWORD>(pitch) * static_cast<DWORD>(std::max(height, 0))) {
            out->pixels.clear();
            return false;
        }
        out->pixels.resize(needed);
        const DWORD got = GetGlyphOutlineW(dc, static_cast<UINT>(codepoint), fu_format, &metrics,
                                           needed, out->pixels.data(), &transform);
        if (got == GDI_ERROR) {
            out->pixels.clear();
            return false;
        }
        out->max_value = 0;
        for (const std::uint8_t value : out->pixels) {
            if (value > out->max_value) out->max_value = value;
        }
    }
    return true;
#else
    (void)face;
    (void)size;
    (void)weight;
    (void)format;
    (void)codepoint;
    (void)out;
    return false;
#endif
}

}  // namespace core
