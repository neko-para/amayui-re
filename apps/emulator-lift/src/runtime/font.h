// emulator-lift · 宿主 GDI 字体 / 文本层（P2）
//
// ★ 口径（用户 2026-11-04 D3 / 真源 `apps/emulator-cxx/SCOPE.md` §0 第 1 条 / §1.1 / §1.2）：
//   · **主字体总是 Amayui CN**（`corpus/assets/fonts/Amayui-CN_cnjp.ttf`，粗体同目录 `-Bold.ttf`）；
//     `CreateFontIndirectA` 里的字面名**不改变字形来源**（`--font-engine-name` 是显式 A/B 对照档，默认关）。
//   · `AddFontMemResourceEx` 送进来的内存**只当缺字后备**（`REQ-01M79N1ZV391BCM7MYSKK0GVV0`）：
//     主字体有该码位的字形时**永不**用到它 —— ⛔ 不按 `lfFaceName` 路由（引擎把 `AGE Extend`
//     硬编码给所有文本，按名字路由会把全部中文切回日文字形，那是倒退）。
//   · **基线补偿 +2px** 是**宿主视觉策略**（⛔ 不是引擎语义，登记在 `SCOPE.md`），0 = 关。
//   · GGO 的**格式**（1 位 / 17 级 / 65 级）与行对齐是**引擎合同**；1px 级栅格差按平台 discard。
// ★ 本文件 = "宿主字体 + GDI 对象状态"的叶子层；`src/shims/gdi32_*.cpp` 只做 Win32 形状与参数解包。
// ★ 自洽判据（plan §5 B3）的**唯一**取字/推进路径是 `gdiGlyph()`：
//     `GetGlyphOutlineA` 的 gmCellIncX / `GetTextExtentPoint32A` 的宽 / `TextOutA` 的推进
//   三者都从它派生，⛔ 不许各算一遍。
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "runtime/handles.h"
#include "runtime/memory.h"
#include "runtime/surface.h"

namespace lift {

// =======================================================================================
// 宿主字体配置（CLI `--font` / `--baseline-lift` / `--font-engine-name`；main.cpp 调一次）
// =======================================================================================
// ★ **基线补偿量**（宿主视觉策略，⛔ 不是引擎语义）：**唯一一处常量**，名字对齐 cxx 的对应量。
//   cxx 真源 = `apps/emulator-cxx/src/core/text_raster.cpp:647` 的 `lift = 2`
//   （env 覆盖 `EMULATOR_CXX_BASELINE_LIFT` 在 `:648-651`；**唯一应用点** `:675`
//   `baseline = y_effective + current_ascent - lift`；生效条件 `:643-646` = "宿主字体在用"）。
//   这里是**缺省值**；运行时以 `--baseline-lift`（`FontConfig::baseline_lift`）为准，`0` = 关。
constexpr int kBaselineLift = 2;

struct FontConfig {
    std::string regular_path;      // 宿主常规字体（CMake 注入 `LIFT_HOST_FONT_PATH`）
    std::string bold_path;         // 同上 `-Bold.ttf`（空 = 由 regular_path 派生）
    int baseline_lift = kBaselineLift;  // 宿主视觉策略：把墨迹整体抬 N px（0 = 关；见 `kBaselineLift`）
    bool use_engine_name = false;  // A/B 对照档：按引擎字面名取字（默认关）
};

FontConfig fontDefaultConfig();          // CMake 注入的那一份（不含 CLI 覆盖）
void fontSetConfig(const FontConfig&);   // CLI 覆盖（进程级；在任何 FontEngine 使用前调用）
const FontConfig& fontConfig();          // 当前生效的配置（基线补偿也从这里读）

// =======================================================================================
// GDI / GGO 常量（wingdi.h 的真值；⛔ 别猜）
// =======================================================================================
constexpr std::uint32_t kGdiError = 0xFFFFFFFFu;
constexpr std::uint32_t kGgoMetrics = 0;
constexpr std::uint32_t kGgoBitmap = 1;
constexpr std::uint32_t kGgoNative = 2;
constexpr std::uint32_t kGgoGray2 = 4;
constexpr std::uint32_t kGgoGray4 = 5;
constexpr std::uint32_t kGgoGray8 = 6;
constexpr std::uint32_t kGgoUnhinted = 0x0100;

// GRAY4 = 17 级（0..16）、GRAY8 = 65 级（0..64）—— 与 `apps/emulator-cxx/src/core/text_alpha.h` 同源。
constexpr std::uint32_t kGray4MaxLevel = 16;
constexpr std::uint32_t kGray8MaxLevel = 64;

// stock object 的确定性令牌（P0 的 gdi32_boot.cpp 就返回 0x00013000 + index；这里接管）。
constexpr std::uint32_t kStockObjectBase = 0x00013000u;
constexpr int kStockObjectMax = 32;

// DC 状态常量（Win32 真值）
constexpr std::int32_t kTransparent = 1;
constexpr std::int32_t kOpaque = 2;
constexpr std::uint32_t kObjPen = 1;
constexpr std::uint32_t kObjBrush = 2;
constexpr std::uint32_t kObjPal = 5;
constexpr std::uint32_t kObjFont = 6;
constexpr std::uint32_t kObjBitmap = 7;
constexpr std::uint32_t kDefaultGuiFont = 17;   // stock 索引：DEFAULT_GUI_FONT
constexpr std::uint32_t kStockFontWeight = 400;
constexpr std::uint32_t kStockFontHeight = 16;  // 像素（负高度 ⇒ 字符高度）

// =======================================================================================
// 字形（宿主视图）：连续覆盖率 0..255，与 GGO 的格式**无关**；格式在 `gdiPackGlyph` 里量化。
// =======================================================================================
struct GlyphMask {
    int black_box_x = 0;   // gmBlackBoxX
    int black_box_y = 0;   // gmBlackBoxY
    int origin_x = 0;      // gmptGlyphOrigin.x（相对基线；可为负）
    int origin_y = 0;      // gmptGlyphOrigin.y
    int advance = 0;       // gmCellIncX（像素；= floor(26.6 advance)）
    std::vector<std::uint8_t> coverage;  // black_box_x * black_box_y 行主序，0..255
};

struct HostTextMetrics {
    std::int32_t height = 0;
    std::int32_t ascent = 0;
    std::int32_t descent = 0;
    std::int32_t internal_leading = 0;
    std::int32_t external_leading = 0;
    std::int32_t ave_char_width = 0;
    std::int32_t max_char_width = 0;
    std::int32_t weight = 0;
    std::int32_t overhang = 0;
    std::uint8_t italic = 0;
    std::uint8_t underlined = 0;
    std::uint8_t struck_out = 0;
};

// =======================================================================================
// 字体引擎（一台 Machine 一份）：FreeType 桥 + 纯档的合成接缝 + 每字缓存
// =======================================================================================
class FontEngine {
public:
    struct Impl;  // 实现细节（FreeType 句柄）—— 只是为了不让 FT 头文件漏进本头

    FontEngine();
    ~FontEngine();
    FontEngine(const FontEngine&) = delete;
    FontEngine& operator=(const FontEngine&) = delete;

    // ★ 纯档接缝（不读字体文件）：确定性的黑框 / origin / advance / 覆盖率。
    //   用途 = 格式算术（DWORD 行对齐、17/65 级值域、负 origin）与"extent == Σ advance"自洽。
    void setSyntheticForTest(bool on);
    bool syntheticForTest() const { return synthetic_; }

    // ★ 纯档接缝（不读字体文件）：替换 `metrics()` 里**FT 派生段**的计算。
    //   只给 pure 档守卫用（缓存 / 失效逻辑照旧走 `metrics()` 的真实路径）；生产路径永不安装。
    //   `user` 原样回传（测试用它数"真算了几次"）。
    using MetricsSourceForTest = bool (*)(void* user, int pixel_size, bool bold, HostTextMetrics* out);
    void setMetricsSourceForTest(MetricsSourceForTest fn, void* user);

    // -----------------------------------------------------------------------------------
    // ★ 缺字形 fallback（`REQ-01M79N1ZV391BCM7MYSKK0GVV0`）
    //   主字体仍是宿主 Amayui CN（中文译文靠它）；`AddFontMemResourceEx` 送进来的游戏字体
    //   （`AGE-EXTEND.TTF` 的族名 = `AGE Extend`）**只当主字体缺该码位时的后备**，按登记顺序试。
    //   ⛔ 不按 `lfFaceName` 路由：引擎把 `AGE Extend` 硬编码给所有文本，按名字路由会把全部中文
    //   切回日文字形（那是倒退）。
    // -----------------------------------------------------------------------------------
    // 登记一份内存字体。字节**会被拷贝**（guest 内存随时释放 / 复用；FT 的 memory face 指向这份拷贝）。
    //   `family_out`（可空）= face 的族名（日志 / 诊断用）。返回稳定 id（> 0）；0 = 解析失败
    //   （已记一行日志 ⇒ 调用方按 Win32 失败返回，⛔ 不假装成功）。
    //   ★ 登记 / 移除都会清**字形**缓存：之前缓存的 `.notdef`（或后备字形）不能继续命中。
    std::uint32_t addMemoryFont(const void* data, std::size_t size, std::string* family_out = nullptr);
    bool removeMemoryFont(std::uint32_t id);
    std::size_t memoryFontCount() const;
    std::string memoryFontFamily(std::size_t index) const;  // 第 index 个后备的族名（空 = 没加载）

    // 缺字形记账（`--profile` 的摘要行 + 守卫）：**按解析计**（缓存未命中时一次），不是按调用计。
    //   `fallback_uses` = 该字形最终由某个后备字体提供；
    //   `notdefs` = 主 face 与**全部**后备都没有 ⇒ 只好画 `.notdef`（诚实：返回 true + 计数 + 首次 warn，
    //   ⛔ 不是"缺字当成功静默画出去"，也不是"失败"）。
    struct GlyphFallbackStats {
        std::uint64_t fallback_uses = 0;
        std::uint64_t notdefs = 0;
    };
    GlyphFallbackStats glyphFallbackStats() const;

    // ★ 纯档接缝（形状对照 `MetricsSourceForTest`）：替身 **FT 的取字原语**（不读字体文件）。
    //   `face_slot`：0 = 主 face，1..N = 后备（按登记顺序）。
    //   · `char_index` = `FT_Get_Char_Index`：返回 0 = **该 face 没有这个码位的字形**。
    //   · `load` = `FT_Load_Char` + 位图拷贝：★ 缺字时它也**返回 true 并给出 `.notdef` 位图**
    //     （FreeType 的真实语义 —— 正是它让"load 成功"不能当成"有字形"）。
    //   `face_count` = 1（只有主 face）+ 后备个数。安装 / 重装会清字形缓存。生产路径恒不安装。
    struct GlyphSourceForTest {
        int face_count = 0;
        std::uint32_t (*char_index)(void* user, int face_slot, char32_t cp) = nullptr;
        bool (*load)(void* user, int face_slot, char32_t cp, int pixel_size, bool bold,
                     GlyphMask* out) = nullptr;
        void* user = nullptr;
    };
    void setGlyphSourceForTest(const GlyphSourceForTest& src);

    void clearCache();

    // `metrics()` 缓存的可观测计数（`--profile` 打印；⛔ 不是墙钟，也不含 synthetic 分支）。
    std::uint64_t metricsHits() const;
    std::uint64_t metricsMisses() const;

    // 取一个字形（metrics + 覆盖率）。false = **没有字体后端**（宿主字体缺失 / FT 起不来）
    //   —— 调用方必须按失败返回（⛔ 不许静默画空白）。
    //   ★ `origin_y` 已含宿主基线补偿 `FontConfig::baseline_lift`（`kBaselineLift`）：**位移在 origin，
    //     不在度量** —— 这样 `gmptGlyphOrigin.y` 与 guest 自己算的基线一起把墨迹抬 N px，而
    //     `tmAscent` / 行高仍是 FT 真值（⛔ 不动 guest 的行距）。
    bool glyph(char32_t cp, int pixel_size, bool bold, GlyphMask* out);

    // 由 FT face 的 `size` 算 TEXTMETRICA 要的几格（tmHeight/tmAscent/tmDescent/...）。
    //   ★ 结果按 `(clamp 后像素字号, bold, 字体路径)` 缓存（上限 64 条，到顶整表 clear）。
    //   **只出 FT 派生段**：`weight` / `italic` / `underlined` / `struck_out` 不在这里，也不进键 ——
    //   它们由 `GdiFontDesc` 派生，只由 `gdiTextMetrics` 每次覆盖（唯一一处；⛔ 别在这里再写一遍：
    //   两处写同一字段时删掉任一处守卫都照样绿）。`setSyntheticForTest` / `fontSetConfig` 会清表（见 .cpp）。
    bool metrics(int pixel_size, bool bold, HostTextMetrics* out);

    // 诊断 / assets 测试：宿主字体文件在不在（不加载）
    bool fontFileAvailable(bool bold) const;

private:
    std::unique_ptr<Impl> impl_;
    bool synthetic_ = false;
};

// =======================================================================================
// ★ 临时诊断层（**只为**「读档标题乱码」这一单的落站取证；`LIFT_TEXT_TRACE=1` 打开）
//
//   口径：
//   · **默认零输出**（env 未设 ⇒ `textTrace()` 第一次就返回，不碰 stderr、不改行为）；
//   · 只打印，⛔ 不改任何返回值 / 推进 / 缓存；
//   · 落点只在取字链（`FontEngine::glyph` / `gdiDecodeCp932` 的逐字节走法）与
//     `shims/gdi32_font.cpp` 的三个 A 版文本 API。
//   ★ 本层是**诊断**，不是合同：取证结束后整段可删（见 `.tmp/save-title/*`）。
// =======================================================================================
bool textTraceEnabled();
void textTrace(const char* fmt, ...);
// 按 CP932 **双字节语义**逐个字符走一遍 `bytes`，打印每步的字节偏移与"前进几个字节"
//   （假设 ① 的判据：若某层按**字节**前进，这里会看到 consumed=1 且 cp 是半个字符）。
void traceCp932Walk(const char* who, std::string_view bytes);

// =======================================================================================
// GDI 对象（payload 落在现有 HandleTable 里；guest 看到的是不透明整数）
// =======================================================================================
struct GdiFontDesc {
    std::int32_t height = -static_cast<std::int32_t>(kStockFontHeight);
    std::int32_t width = 0;
    std::int32_t escapement = 0;
    std::int32_t orientation = 0;
    std::int32_t weight = 400;
    std::uint8_t italic = 0;
    std::uint8_t underline = 0;
    std::uint8_t strike_out = 0;
    std::uint8_t charset = 0;
    std::uint8_t out_precision = 0;
    std::uint8_t clip_precision = 0;
    std::uint8_t quality = 0;
    std::uint8_t pitch_and_family = 0;
    std::string face_name;  // 引擎字面名（默认忽略；A/B 档才用）
};

struct GdiDcState {
    std::uint32_t font_handle = 0;  // 当前选中的字体对象（stock 令牌或 HandleTable 句柄）
    GdiFontDesc font;               // 与 font_handle 同步的一份**值**
    std::uint32_t bitmap_handle = 0;
    std::uint32_t pen_handle = 0;
    std::uint32_t brush_handle = 0;
    std::uint32_t palette_handle = 0;
    std::uint32_t text_color = 0x00FFFFFFu;  // COLORREF 0x00BBGGRR
    std::uint32_t bk_color = 0x00000000u;
    std::int32_t bk_mode = kOpaque;
    bool display = false;   // CreateICA("DISPLAY") 的屏幕 DC
    std::string device;     // 设备名（诊断）
    // ★ P1 的接缝：DDraw 表面 DC 的目标（`fontAttachDcSurface` 登记；0 = 没有目标）
    std::uint32_t surface_ea = 0;
    SurfaceArena* surface_arena = nullptr;
    // 屏幕 DC 的**确定性产物**：不画真窗口，但记账（TextOutA 的调用/字形/像素数）
    std::uint32_t text_calls = 0;
    std::uint32_t glyphs_drawn = 0;
    std::uint32_t pixels_written = 0;
};

struct GdiDcObject {
    GdiDcState state;
};

struct GdiFontObject {
    GdiFontDesc desc;
};

// ---- 对象表 helper（shim 与 P1 接缝共用）----
std::uint32_t gdiStockObject(int index);
int gdiStockIndex(std::uint32_t handle);  // -1 = 不是 stock 令牌
bool gdiIsStockFont(int index);           // OEM/ANSI/SYSTEM/DEFAULT_GUI 字体

GdiDcObject* gdiAsDc(HandleTable& table, std::uint32_t hdc);       // nullptr = 不是 DC
GdiFontObject* gdiAsFont(HandleTable& table, std::uint32_t hfont); // nullptr = 不是字体

std::uint32_t gdiCreateDc(HandleTable& table, bool display, const std::string& device);
std::uint32_t gdiCreateFont(HandleTable& table, const GdiFontDesc& desc);
GdiFontDesc gdiDefaultFontDesc();
int gdiPixelSize(const GdiFontDesc& desc);  // |lfHeight|；0 ⇒ 缺省 16；夹到 1..512
bool gdiBold(const GdiFontDesc& desc);      // lfWeight >= 700 ⇒ Bold face

// DC 里当前字体的**有效描述**（没有选中 ⇒ 缺省）
GdiFontDesc gdiEffectiveFont(const GdiDcState& dc);

// =======================================================================================
// 取字 / 推进 / 绘制（三者共用 `gdiGlyph` 这一条路径）
// =======================================================================================
std::vector<char32_t> gdiDecodeCp932(std::string_view bytes);
char32_t gdiCp932Char(std::uint32_t uchar);  // A 版 API 的 uChar（CP932 单/双字节）→ Unicode

bool gdiGlyph(FontEngine& fonts, const GdiFontDesc& desc, char32_t cp, GlyphMask* out);
bool gdiTextMetrics(FontEngine& fonts, const GdiFontDesc& desc, HostTextMetrics* out);

// `GetTextExtentPoint32A` 的宽 = Σ gmCellIncX（同一路径求两次，测试用它交叉核对）
bool gdiTextExtent(FontEngine& fonts, const GdiFontDesc& desc, std::string_view cp932,
                   std::int32_t* width, std::int32_t* height);

// GGO 位图打包（**纯函数**：格式算术在这里，可纯测）：
//   GGO_BITMAP      : 1bpp，行 = ((w + 31) >> 5) << 2 字节（DWORD 对齐）
//   GGO_GRAY4/8     : 8bpp（每像素 1 字节），行 = (w + 3) & ~3（DWORD 对齐），值 0..16 / 0..64
// 返回 false = 不支持的格式 / mask 自相矛盾。`pitch` = 行字节数（未对齐的宽度见注释）。
bool gdiPackGlyph(const GlyphMask& mask, std::uint32_t format, std::vector<std::uint8_t>* out,
                  std::uint32_t* pitch);

struct TextDrawResult {
    bool ok = false;
    bool surface_missing = false;  // DC 登记了 surface_ea 但 arena 找不到它（P1 拼装错误 ⇒ loud）
    std::int32_t advance = 0;      // 实际推进的像素宽（== gdiTextExtent 的宽）
    std::uint32_t glyphs = 0;
    std::uint32_t pixels = 0;
};

// `TextOutA` 的宿主实现：写 DC 的目标（表面 DC）或只记账（屏幕 DC）。
TextDrawResult gdiDrawText(FontEngine& fonts, const GdiFontDesc& desc, const GdiDcState& dc,
                           int x, int y, std::string_view cp932);

// =======================================================================================
// ★ 与 P1（DDraw GetDC/ReleaseDC）的接缝
//
//   `bool fontAttachDcSurface(std::uint32_t hdc, std::uint32_t surface_ea, SurfaceArena* arena);`
//   P1 在 **DDraw `IDirectDrawSurface::GetDC`（vtable +68，十进制）返回 HDC 之前**调用它，
//   把自己的 `surface_ea` 与 `SurfaceArena*` 绑到这个 HDC；`ReleaseDC`（vtable +104）
//   里调 `fontDetachDc(hdc)`。找不到活动 Machine / hdc 不是 DC ⇒ false + 一行日志（⛔ 静默成功）。
// =======================================================================================
bool fontAttachDcSurface(std::uint32_t hdc, std::uint32_t surface_ea, SurfaceArena* arena);
bool fontDetachDc(std::uint32_t hdc);

}  // namespace lift
