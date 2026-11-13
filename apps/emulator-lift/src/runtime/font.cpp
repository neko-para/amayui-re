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
#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <tuple>
#include <utility>

#include <ft2build.h>
#include FT_FREETYPE_H

#include "runtime/cp932.h"
#include "runtime/machine.h"
#include "runtime/pixel_format.h"
#include "runtime/vfs.h"  // openHostFileUtf8（Windows 的 UTF-8 路径）

namespace lift {

// =======================================================================================
// ★ 临时诊断层（`LIFT_TEXT_TRACE=1`；默认零输出）—— 只为「读档标题乱码」这一单取证。
//   ⛔ 不改行为；取证结束后可整段删（调用点各 1 个 `if (textTraceEnabled())`）。
// =======================================================================================
bool textTraceEnabled() {
    static const bool on = std::getenv("LIFT_TEXT_TRACE") != nullptr;
    return on;
}

void textTrace(const char* fmt, ...) {
    if (!textTraceEnabled()) return;
    std::fputs("[text-trace] ", stderr);
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(stderr, fmt, ap);
    va_end(ap);
    std::fputc('\n', stderr);
    std::fflush(stderr);
}

void traceCp932Walk(const char* who, std::string_view bytes) {
    if (!textTraceEnabled()) return;
    const std::uint8_t* p = reinterpret_cast<const std::uint8_t*>(bytes.data());
    std::size_t off = 0;
    int index = 0;
    textTrace("%s: walk len=%zu", who, bytes.size());
    while (off < bytes.size() && index < 96) {
        char16_t wide = 0;
        const int consumed = cp932Decode(p + off, bytes.size() - off, &wide);
        textTrace("%s: cp[%d] off=+%zu consumed=%d cp=U+%04X", who, index, off, consumed,
                  static_cast<unsigned>(wide));
        if (consumed <= 0) break;  // 非法序列：真路径也是"按 Windows 口径跳过 1 字节"
        off += static_cast<std::size_t>(consumed);
        ++index;
    }
}

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

// ★ 配置代际：`fontSetConfig` 每次 +1。各 FontEngine 在自己的取字 / 度量入口比对它 ——
//   不一致就清掉两级缓存（字形 + 度量）。这是 `fontSetConfig` **唯一的**失效钩子：
//   它没有引擎清单可遍历（配置是进程级、引擎是每 Machine 一台），所以用代际而不是回调。
std::uint64_t g_config_epoch = 1;

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

// 通道换算走 runtime/pixel_format（唯一一份，⛔ 这里不再保留 maskShift/maskBits 的副本）

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
    const int dr = pixel_format::expandChannelGdi(before, s.format.rmask);
    const int dg = pixel_format::expandChannelGdi(before, s.format.gmask);
    const int db = pixel_format::expandChannelGdi(before, s.format.bmask);
    const int keep = 255 - coverage;
    const int r = (color.r * coverage + dr * keep) / 255;
    const int g = (color.g * coverage + dg * keep) / 255;
    const int b = (color.b * coverage + db * keep) / 255;
    std::uint32_t out = before & ~(s.format.rmask | s.format.gmask | s.format.bmask);
    out |= pixel_format::packChannelGdi(r, s.format.rmask);
    out |= pixel_format::packChannelGdi(g, s.format.gmask);
    out |= pixel_format::packChannelGdi(b, s.format.bmask);
    storeSurfacePixel(s, base, x, y, out);
    if (written) ++*written;
}

void fillSurfaceRect(GuestSurface& s, std::uint8_t* base, int x0, int y0, int w, int h,
                     const Rgb& color) {
    const std::uint32_t solid = pixel_format::packChannelGdi(color.r, s.format.rmask) |
                                pixel_format::packChannelGdi(color.g, s.format.gmask) |
                                pixel_format::packChannelGdi(color.b, s.format.bmask);
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
    ++g_config_epoch;  // ★ 换配置 ⇒ 所有 FontEngine 的缓存作废（在各自入口比对，见 `syncConfigEpoch`）
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
    // ★ `metrics()` 的缓存：键 = `metrics` 的**输入集** `(clamp 后像素字号, bold, 字体路径)`。
    //   ⛔ 不用 HDC 做键 —— 同一个屏幕 DC 上会被换字体量不同字号（`gdi32_object.cpp` 的 SelectObject
    //   路径），键里少一维就把小字号的 ascent 复用给大字号（差 8px，见 `.tmp/task3-report.md` §2）。
    //   值只放 FT 派生段（weight / italic / underlined / struck_out 每次仍从 desc 覆盖）。
    std::map<std::tuple<int, int, std::string>, HostTextMetrics> metrics_cache;
    std::uint64_t metrics_hits = 0;
    std::uint64_t metrics_misses = 0;
    std::uint64_t config_epoch = 0;  // 与 `g_config_epoch` 不一致 ⇒ 清表（见 `syncConfigEpoch`）
    FontEngine::MetricsSourceForTest metrics_source = nullptr;  // 纯档接缝（生产恒 nullptr）
    void* metrics_source_user = nullptr;
    bool warned_missing = false;
    bool warned_bold = false;
};

// 度量缓存上限：条目数天然是个位数（字号 × regular/bold），到顶**整表 clear**（不做 LRU、不引依赖）。
constexpr std::size_t kMetricsCacheMax = 64;

namespace {

// 代际变了（= `fontSetConfig` 跑过）⇒ 两级缓存全清。`baseline_lift` 也在这里面（它被烘进字形的
// `origin_y`），所以改 lift 之后必须清字形缓存，否则 A/B 对照档会拿到旧 origin。
void syncConfigEpoch(FontEngine::Impl& impl) {
    if (impl.config_epoch == g_config_epoch) return;
    impl.config_epoch = g_config_epoch;
    impl.cache.clear();
    impl.metrics_cache.clear();
}

FT_Library ensureLibrary(FontEngine::Impl& impl) {
    if (!impl.lib_tried) {
        impl.lib_tried = true;
        if (FT_Init_FreeType(&impl.lib) != 0) impl.lib = nullptr;
    }
    return impl.lib;
}

// ★ 临时诊断用：`face->charmap` 在 `face->charmaps[0..num_charmaps)` 里的下标（-1 = 没有）。
int traceCharmapIndex(FT_Face face) {
    if (face == nullptr || face->charmap == nullptr) return -1;
    for (int i = 0; i < face->num_charmaps; ++i) {
        if (face->charmaps[i] == face->charmap) return i;
    }
    return -1;
}

FT_Face faceFromPath(FontEngine::Impl& impl, const std::string& path) {    if (path.empty()) return nullptr;
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
    if (textTraceEnabled()) {
        // 假设 ② 的判据：这个 face 里到底有几张 cmap 子表、各自 platform/encoding 是什么，
        // FreeType 默认**选中**了哪一张（`face->charmap`）。平台 1 = Mac（cnjp 重写时被跳过的那张）。
        textTrace("face %s: num_charmaps=%d selected=%d", path.c_str(), face->num_charmaps,
                  traceCharmapIndex(face));
        for (int i = 0; i < face->num_charmaps; ++i) {
            const FT_CharMap c = face->charmaps[i];
            textTrace("  charmap[%d] platformID=%u encodingID=%u ft_encoding=0x%08X", i,
                      static_cast<unsigned>(c->platform_id), static_cast<unsigned>(c->encoding_id),
                      static_cast<unsigned>(c->encoding));
        }
    }
    return face;
}

// `metrics` 缓存键里的"字体路径" = 这次**请求**要用的那份 face 的路径（与 `faceForSize` 的取法一致：
// bold 且 bold 为空 ⇒ 常规；bold 文件**存在但加载失败**时 `faceForSize` 会回退常规字面，而这里仍是
// bold_path ⇒ 那种情况下会多出一条**重复条目**（值由同一个 face 算出，数值不会串）。
// ⛔ 但**不能**把这一维去掉改成裸字号当键：bold/常规是两份 face（ascent 与 'M'/漢/あ 的 advance 都不同）。
std::string facePathFor(bool bold) {
    const FontConfig& cfg = fontConfig();
    std::string path = bold ? cfg.bold_path : cfg.regular_path;
    if (path.empty()) path = cfg.regular_path;
    return path;
}

// 取（并切换）一个字号的 face。bold 缺字体文件时回退常规字面（与 emulator-cxx 同口径）。
FT_Face faceForSize(FontEngine::Impl& impl, bool bold, int pixel_size) {
    const FontConfig& cfg = fontConfig();
    std::string path = facePathFor(bold);
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
    if (!impl_) return;
    // ★ 合成档与真字体档的**签名不同**（合成：height=size+2 / ascent=size / ave=6）⇒ 切档必须把
    //   **两级**缓存都清掉，否则合成的度量会被真字体调用复用（反之亦然）。
    impl_->cache.clear();
    impl_->metrics_cache.clear();
}

void FontEngine::clearCache() {
    if (!impl_) return;
    impl_->cache.clear();
    impl_->metrics_cache.clear();
}

void FontEngine::setMetricsSourceForTest(MetricsSourceForTest fn, void* user) {
    if (!impl_) return;
    impl_->metrics_source = fn;
    impl_->metrics_source_user = user;
    impl_->metrics_cache.clear();  // 换源 ⇒ 旧结果作废（否则守卫会先命中旧条目）
}

std::uint64_t FontEngine::metricsHits() const { return impl_ ? impl_->metrics_hits : 0; }
std::uint64_t FontEngine::metricsMisses() const { return impl_ ? impl_->metrics_misses : 0; }

bool FontEngine::fontFileAvailable(bool bold) const {
    const FontConfig& cfg = fontConfig();
    const std::string& path = (bold && !cfg.bold_path.empty()) ? cfg.bold_path : cfg.regular_path;
    return hostFileExists(path);
}

bool FontEngine::glyph(char32_t cp, int pixel_size, bool bold, GlyphMask* out) {
    if (out == nullptr) return false;
    const int size = std::clamp(pixel_size, 1, 512);
    if (synthetic_) {
        // ★ 合成档**不**吃基线补偿：它的 `origin_y = black_box_y` 是格式算术的契约
        //   （`tests/gdi.cpp` 逐值钉住），不是 FT 的 `bitmap_top`，也没有"Amayui 与参考字体的
        //   ascent 差"这回事（补偿的理由见 `kBaselineLift`）。真字体档才位移。
        *out = syntheticMask(cp, size);
        return true;
    }
    if (!impl_) return false;
    syncConfigEpoch(*impl_);
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
    if (textTraceEnabled()) {
        // 假设 ② 的判据：**选中的那张 cmap 子表**（platformID / encodingID）+ 该码位查到的 glyph id。
        const FT_CharMap cm = face->charmap;
        textTrace("glyph cp=U+%04X size=%d bold=%d charmap[%d] platformID=%u encodingID=%u glyphID=%u",
                  static_cast<unsigned>(cp), size, bold ? 1 : 0,
                  cm ? traceCharmapIndex(face) : -1,
                  cm ? static_cast<unsigned>(cm->platform_id) : 999u,
                  cm ? static_cast<unsigned>(cm->encoding_id) : 999u,
                  static_cast<unsigned>(FT_Get_Char_Index(face, static_cast<FT_ULong>(cp))));
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
    // ★ 宿主基线补偿的**唯一位移点**（`kBaselineLift` / `--baseline-lift`）：把 `gmptGlyphOrigin.y`
    //   抬 N px ⇒ guest 用它算的"字形顶 = 基线 − origin.y"整体上移 N px。等价于 cxx 在
    //   `text_raster.cpp:675` 的 `baseline = ... - lift`（那边减基线、这边加 origin，屏幕位置同一），
    //   但**不改度量**：`tmAscent` / `tmHeight` 保持 FT 真值 ⇒ guest 的行距 / 排版不变。
    m.origin_y = slot->bitmap_top + fontConfig().baseline_lift;
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

namespace {

// FT 派生段（**缓存的就是这一段**）：TEXTMETRICA 里随字体 / 字号变化的那些格子。
// 返回 false = 没有可用的 face（调用方按失败返回，⛔ 不画空白冒充成功）。
bool computeFaceMetrics(FontEngine::Impl& impl, bool bold, int size, HostTextMetrics* out) {
    if (impl.metrics_source != nullptr) {
        // 纯档接缝：只替换"FT 怎么算"，缓存 / 键 / 失效全部照旧走 `metrics()`。
        return impl.metrics_source(impl.metrics_source_user, size, bold, out);
    }
    FT_Face face = faceForSize(impl, bold, size);
    if (face == nullptr) {
        if (!impl.warned_missing) {
            impl.warned_missing = true;
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
    out->overhang = 0;
    return true;
}

}  // namespace

bool FontEngine::metrics(int pixel_size, bool bold, HostTextMetrics* out) {
    if (out == nullptr) return false;
    const int size = std::clamp(pixel_size, 1, 512);
    if (synthetic_) {
        // ★ synthetic **不进缓存**（它不读字体文件，也没有 3 次 `FT_Load_Char` 的成本）。
        out->height = size + 2;
        out->ascent = size;
        out->descent = 2;
        out->internal_leading = 0;
        out->external_leading = 0;
        out->ave_char_width = 6;
        out->max_char_width = 6;
        out->overhang = 0;
        return true;
    }
    if (!impl_) return false;
    syncConfigEpoch(*impl_);
    // ★ 键 = `metrics` 的输入集：clamp 后的像素字号 / bold / 字体路径（⛔ 不是 HDC：同一个屏幕 DC
    //   上会被 SelectObject 换字体、量不同字号）。**desc 派生的四格（weight / italic / underlined /
    //   struck_out）不在这里** —— 它们不是 FT 派生物，只由 `gdiTextMetrics` 每次从 desc 覆盖。
    //   ★ 只留**一处**写这四个字段：两处写同一个字段时，删掉任一处守卫都照样绿（实测见交付说明 M4）。
    const std::tuple<int, int, std::string> key{size, bold ? 1 : 0, facePathFor(bold)};
    const auto cached = impl_->metrics_cache.find(key);
    if (cached != impl_->metrics_cache.end()) {
        ++impl_->metrics_hits;
        *out = cached->second;
        return true;
    }
    ++impl_->metrics_misses;
    if (!computeFaceMetrics(*impl_, bold, size, out)) return false;
    // ★ **入表的值只放 FT 派生段**：`weight/italic/underlined/struck_out` 是 desc 派生物、不归这一层
    //   （否则"表里那格"= 首个请求者的 desc 值，命中时又盖回调用方 —— 那正是"两处写同一字段"的隐式版：
    //   今天靠 `gdiTextMetrics` 无条件重写才不出错）。显式清零 = 缓存里根本没这四格。
    out->weight = 0;
    out->italic = 0;
    out->underlined = 0;
    out->struck_out = 0;
    if (impl_->metrics_cache.size() >= kMetricsCacheMax) {
        // 到顶**整表 clear**（不做 LRU：条目天然个位数，引一个淘汰策略只会多一条要测的路径）。
        impl_->metrics_cache.clear();
    }
    impl_->metrics_cache.emplace(key, *out);
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
    if (!fonts.metrics(gdiPixelSize(desc), gdiBold(desc), out)) return false;
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

    // ★ 基线补偿**不在这里**：它已经烘进 `mask.origin_y`（见 `FontEngine::glyph` 里
    //   `slot->bitmap_top + baseline_lift`）。这里再减一次就是抬 2N px —— 而引擎正文走
    //   GGO + guest 自己合成，压根不经这条路径，所以补偿必须落在**两者共用**的 origin 上。
    const int baseline = y + tm.ascent;
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
