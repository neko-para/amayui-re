// emulator-lift · 真 Amayui CN 的 GDI 字体 / 文本（P2；assets）
//
// 判据（plan §5 B3 / §6 P2；assets 档，缺字体文件 ⇒ `LIFT_SKIP` → 77，⛔ 不许假绿）：
//   ① `GetTextMetricsA` 的 ascent / height 合理，且**两次调用逐字节一致**；
//   ② `GetGlyphOutlineA` 三种格式的行对齐（BITMAP = ((w+31)>>5)<<2、GRAY4/8 = (w+3)&~3）与值域
//      （GRAY4 0..16、GRAY8 0..64）；`cbBuffer == 0` 返回的字节数 == 实际写入的字节数；
//   ③ `GetTextExtentPoint32A` 的宽 == Σ `GetGlyphOutlineA` 的 gmCellIncX（同一段 CP932 文本）；
//   ④ 同一段文本两次渲染的像素**逐字节相同**（确定性），且文本像素带非空；
//   ⑤ 表面 DC 的 `gdiDrawText` 真的写进 guest surface（P1 表面 DC 的接缝）。
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "check.h"
#include "runtime/font.h"
#include "runtime/machine.h"
#include "shims/register.h"

namespace {

constexpr std::uint32_t kCode = 0x00401000u;
constexpr std::uint32_t kLogFont = 0x00402000u;
constexpr std::uint32_t kMat = 0x00402100u;
constexpr std::uint32_t kText = 0x00402120u;
constexpr std::uint32_t kDisplay = 0x00402140u;
constexpr std::uint32_t kTm1 = 0x00402200u;
constexpr std::uint32_t kTm2 = 0x00402280u;
constexpr std::uint32_t kGm4 = 0x00402300u;
constexpr std::uint32_t kGm4b = 0x00402340u;
constexpr std::uint32_t kGm1 = 0x00402380u;
constexpr std::uint32_t kGm6 = 0x004023C0u;
constexpr std::uint32_t kSize = 0x00402400u;
constexpr std::uint32_t kBuf = 0x00402500u;
constexpr std::uint32_t kHdc = 0x00403000u;
constexpr std::uint32_t kFont = 0x00403004u;
constexpr std::uint32_t kOldObject = 0x00403008u;
constexpr std::uint32_t kNeed4 = 0x0040300Cu;
constexpr std::uint32_t kNeed1 = 0x00403010u;
constexpr std::uint32_t kNeed6 = 0x00403014u;
constexpr std::uint32_t kGot4 = 0x00403018u;
constexpr std::uint32_t kStubBase = 0x7FF80000u;
constexpr std::uint32_t kSentinel = 0xFFFFFFFFu;
constexpr std::uint32_t kFixtureHeap = 0x00100000u;
constexpr int kPixelSize = 20;

struct Asm {
    std::vector<std::uint8_t> b;
    std::uint32_t ea() const { return kCode + static_cast<std::uint32_t>(b.size()); }
    void u8(std::uint8_t v) { b.push_back(v); }
    void u32(std::uint32_t v) {
        for (int i = 0; i < 4; ++i) b.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF));
    }
    void pushImm(std::uint32_t v) { u8(0x68); u32(v); }
    void pushMem(std::uint32_t slot) { u8(0xFF); u8(0x35); u32(slot); }
    void callRel(std::uint32_t target) {
        const std::uint32_t at = ea();
        u8(0xE8);
        u32(static_cast<std::uint32_t>(static_cast<std::int32_t>(target - (at + 5))));
    }
    void storeEax(std::uint32_t slot) { u8(0xA3); u32(slot); }
    void ret() { u8(0xC3); }
};

std::uint32_t stubOf(lift::Machine& m, const char* name) {
    const lift::ShimDef* d = m.shims().find("gdi32", name);
    if (!d || !d->stub_ea) {
        std::fprintf(stderr, "stub missing: %s\n", name);
        return 0;
    }
    return d->stub_ea;
}

void put32(std::vector<std::uint8_t>& buf, std::size_t off, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) buf[off + i] = static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF);
}

std::uint32_t pitchFor(std::uint32_t format, int width) {
    if (format == lift::kGgoBitmap) return static_cast<std::uint32_t>(((width + 31) >> 5) << 2);
    return static_cast<std::uint32_t>((width + 3) & ~3);
}

}  // namespace

int main() {
    using namespace lift_test;

    lift::Machine m;
    std::string err;
    g_current_case = "setup";
    LIFT_CHECK(m.initBare(&err));
    LIFT_CHECK(m.prepareStack(&err));
    LIFT_CHECK(m.heap().init(&m.mem(), m.layout().heap_base, kFixtureHeap));
    if (!m.fonts().fontFileAvailable(false)) {
        LIFT_SKIP("缺宿主字体 Amayui-CN_cnjp.ttf（LIFT_HOST_FONT_PATH / corpus/assets/fonts）");
    }
    lift::registerGdiFontShims(m.shims());
    {
        const std::size_t count = m.shims().all().size();
        for (std::uint32_t id = 0; id < count; ++id) {
            lift::ShimDef* d = const_cast<lift::ShimDef*>(m.shims().byId(id));
            if (!d || !d->fn) continue;
            d->stub_ea = kStubBase + id * 4u;
            m.cache().installShim(d->stub_ea, d->id);
        }
    }

    std::vector<std::uint8_t> logfont(60, 0);
    put32(logfont, 0, static_cast<std::uint32_t>(-kPixelSize));
    put32(logfont, 16, 400);
    const char engine_face[] = "\x82\x6C\x82\x72\x20\x83\x53\x83\x56\x83\x62\x83\x4E";  // ＭＳ ゴシック
    std::memcpy(&logfont[28], engine_face, sizeof(engine_face) - 1);
    LIFT_CHECK(m.placeData(kLogFont, logfont.data(), logfont.size(), &err));
    std::vector<std::uint8_t> mat(16, 0);
    put32(mat, 0, 0x00010000u);
    put32(mat, 12, 0x00010000u);
    LIFT_CHECK(m.placeData(kMat, mat.data(), mat.size(), &err));
    // CP932 的「テスト」（テ=0x8365 / ス=0x8358 / ト=0x8367）：A 版字符串，双字节字符
    const char text[] = {'\x83', '\x65', '\x83', '\x58', '\x83', '\x67', 0};
    LIFT_CHECK(m.placeData(kText, text, sizeof(text), &err));
    const char display[] = "DISPLAY";
    LIFT_CHECK(m.placeData(kDisplay, display, sizeof(display), &err));
    const std::vector<std::uint8_t> zeros(0x1000, 0);
    LIFT_CHECK(m.placeData(kBuf, zeros.data(), zeros.size(), &err));
    LIFT_CHECK(m.placeData(kTm1, zeros.data(), 56, &err));
    LIFT_CHECK(m.placeData(kTm2, zeros.data(), 56, &err));
    for (const std::uint32_t slot : {kGm4, kGm4b, kGm1, kGm6}) {
        LIFT_CHECK(m.placeData(slot, zeros.data(), 24, &err));
    }
    LIFT_CHECK(m.placeData(kSize, zeros.data(), 8, &err));
    for (const std::uint32_t slot : {kHdc, kFont, kOldObject, kNeed4, kNeed1, kNeed6, kGot4}) {
        LIFT_CHECK(m.placeData(slot, zeros.data(), 4, &err));
    }

    Asm a;
    a.pushImm(0);
    a.pushImm(0);
    a.pushImm(0);
    a.pushImm(kDisplay);
    a.callRel(stubOf(m, "CreateICA"));
    a.storeEax(kHdc);
    a.pushImm(kLogFont);
    a.callRel(stubOf(m, "CreateFontIndirectA"));
    a.storeEax(kFont);
    a.pushMem(kFont);
    a.pushMem(kHdc);
    a.callRel(stubOf(m, "SelectObject"));
    a.storeEax(kOldObject);
    // 两次 GetTextMetricsA（必须逐字节一致）
    a.pushImm(kTm1);
    a.pushMem(kHdc);
    a.callRel(stubOf(m, "GetTextMetricsA"));
    a.pushImm(kTm2);
    a.pushMem(kHdc);
    a.callRel(stubOf(m, "GetTextMetricsA"));
    // GGO 'A' 的三档：先问大小（cbBuffer = 0）
    const auto emit_needed = [&a, &m](std::uint32_t format, std::uint32_t slot) {
        a.pushImm(kMat);
        a.pushImm(0);
        a.pushImm(0);
        a.pushImm(kGm4);
        a.pushImm(format);
        a.pushImm(0x41);
        a.pushMem(kHdc);
        a.callRel(stubOf(m, "GetGlyphOutlineA"));
        a.storeEax(slot);
    };
    emit_needed(5, kNeed4);
    emit_needed(1, kNeed1);
    emit_needed(6, kNeed6);
    // GGO 'A' GRAY4 真正写入（cb = needed）
    a.pushImm(kMat);
    a.pushImm(kBuf);
    a.pushMem(kNeed4);
    a.pushImm(kGm4b);
    a.pushImm(5);
    a.pushImm(0x41);
    a.pushMem(kHdc);
    a.callRel(stubOf(m, "GetGlyphOutlineA"));
    a.storeEax(kGot4);
    // GetTextExtentPoint32A(hdc, "テスト", 6, &size)
    a.pushImm(kSize);
    a.pushImm(6);
    a.pushImm(kText);
    a.pushMem(kHdc);
    a.callRel(stubOf(m, "GetTextExtentPoint32A"));
    // TextOutA(hdc, 0, 24, "テスト", 6)
    a.pushImm(6);
    a.pushImm(kText);
    a.pushImm(24);
    a.pushImm(0);
    a.pushMem(kHdc);
    a.callRel(stubOf(m, "TextOutA"));
    a.ret();
    LIFT_CHECK(m.placeCode(kCode, a.b.data(), a.b.size(), &err));

    g_current_case = "run";
    lift::RunOptions opt;
    opt.until_ea = kSentinel;
    opt.max_insns = 200000;
    m.cpu().eip = kCode;
    LIFT_CHECK(m.run(opt).reason == lift::StopReason::UntilEa);

    const std::uint32_t hdc = m.mem().rd32(kHdc);
    const std::uint32_t hfont = m.mem().rd32(kFont);
    g_current_case = "objects";
    LIFT_CHECK_NE_U32(hdc, 0u);
    LIFT_CHECK_NE_U32(hfont, 0u);
    LIFT_CHECK_EQ_U32(m.mem().rd32(kOldObject), lift::gdiStockObject(17));
    lift::GdiDcObject* dc = lift::gdiAsDc(m.handles(), hdc);
    LIFT_CHECK(dc != nullptr);
    if (!dc) return finish("gdi-font");

    // ---- ① 度量：两次一致 + 合理 ----
    g_current_case = "metrics-stable";
    bool tm_equal = true;
    for (std::uint32_t i = 0; i < 56; ++i) {
        if (m.mem().rd8(kTm1 + i) != m.mem().rd8(kTm2 + i)) tm_equal = false;
    }
    LIFT_CHECK(tm_equal);
    const std::int32_t tm_height = static_cast<std::int32_t>(m.mem().rd32(kTm1 + 0));
    const std::int32_t tm_ascent = static_cast<std::int32_t>(m.mem().rd32(kTm1 + 4));
    const std::int32_t tm_descent = static_cast<std::int32_t>(m.mem().rd32(kTm1 + 8));
    LIFT_CHECK(tm_ascent > 0);
    LIFT_CHECK(tm_height == tm_ascent + tm_descent);
    LIFT_CHECK(tm_ascent >= kPixelSize - 4 && tm_ascent <= kPixelSize + 8);  // 20px 的合理区间
    LIFT_CHECK(tm_height >= kPixelSize);
    LIFT_CHECK(m.mem().rd32(kTm1 + 28) == 400u);  // tmWeight

    // ---- ② GGO 三档：行对齐 / 值域 / cbBuffer==0 的返回 == 实际写入 ----
    g_current_case = "ggo-formats";
    const lift::GdiFontDesc desc = lift::gdiEffectiveFont(dc->state);
    lift::GlyphMask mask;
    LIFT_CHECK(lift::gdiGlyph(m.fonts(), desc, U'A', &mask));
    LIFT_CHECK(mask.black_box_x > 0);
    LIFT_CHECK(mask.black_box_y > 0);
    LIFT_CHECK(mask.advance > 0);
    for (const std::uint32_t format : {lift::kGgoBitmap, lift::kGgoGray4, lift::kGgoGray8}) {
        std::vector<std::uint8_t> out;
        std::uint32_t pitch = 0;
        LIFT_CHECK(lift::gdiPackGlyph(mask, format, &out, &pitch));
        LIFT_CHECK_EQ_U32(pitch, pitchFor(format, mask.black_box_x));
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(out.size()),
                          pitch * static_cast<std::uint32_t>(mask.black_box_y));
        if (format == lift::kGgoGray4 || format == lift::kGgoGray8) {
            const std::uint32_t max_level =
                format == lift::kGgoGray4 ? lift::kGray4MaxLevel : lift::kGray8MaxLevel;
            std::uint32_t seen_max = 0;
            for (const std::uint8_t v : out) {
                LIFT_CHECK(v <= max_level);
                if (v > seen_max) seen_max = v;
            }
            LIFT_CHECK(seen_max > 0);      // 真的有点亮像素
            LIFT_CHECK(seen_max <= max_level);
        }
    }
    // guest 侧问到的 needed 与宿主侧打包的长度一致
    const std::uint32_t need4 = m.mem().rd32(kNeed4);
    const std::uint32_t need1 = m.mem().rd32(kNeed1);
    const std::uint32_t need6 = m.mem().rd32(kNeed6);
    {
        std::vector<std::uint8_t> out4;
        std::vector<std::uint8_t> out1;
        std::vector<std::uint8_t> out6;
        LIFT_CHECK(lift::gdiPackGlyph(mask, lift::kGgoGray4, &out4, nullptr));
        LIFT_CHECK(lift::gdiPackGlyph(mask, lift::kGgoBitmap, &out1, nullptr));
        LIFT_CHECK(lift::gdiPackGlyph(mask, lift::kGgoGray8, &out6, nullptr));
        LIFT_CHECK_EQ_U32(need4, static_cast<std::uint32_t>(out4.size()));
        LIFT_CHECK_EQ_U32(need1, static_cast<std::uint32_t>(out1.size()));
        LIFT_CHECK_EQ_U32(need6, static_cast<std::uint32_t>(out6.size()));
    }
    LIFT_CHECK_EQ_U32(m.mem().rd32(kGot4), need4);  // 实际写入 == 之前问到的长度
    // guest 写出的位图与第一份 metrics 自洽
    LIFT_CHECK_EQ_U32(m.mem().rd32(kGm4 + 0), static_cast<std::uint32_t>(mask.black_box_x));
    LIFT_CHECK_EQ_U32(m.mem().rd32(kGm4 + 4), static_cast<std::uint32_t>(mask.black_box_y));
    LIFT_CHECK_EQ_I64(m.mem().rd32(kGm4 + 8), mask.origin_x);
    LIFT_CHECK_EQ_I64(m.mem().rd16(kGm4 + 16), mask.advance);

    // ---- ③ extent == Σ gmCellIncX（同一段 CP932 文本）----
    g_current_case = "extent-advance";
    {
        std::int32_t sum = 0;
        std::uint32_t chars = 0;
        for (const char32_t cp : lift::gdiDecodeCp932(std::string_view(text, 6))) {
            lift::GlyphMask one;
            LIFT_CHECK(lift::gdiGlyph(m.fonts(), desc, cp, &one));
            sum += one.advance;
            ++chars;
        }
        LIFT_CHECK_EQ_U32(chars, 3u);  // 「テスト」= 3 个双字节字符
        const std::int32_t extent_width = static_cast<std::int32_t>(m.mem().rd32(kSize + 0));
        const std::int32_t extent_height = static_cast<std::int32_t>(m.mem().rd32(kSize + 4));
        LIFT_CHECK_EQ_I64(extent_width, sum);
        LIFT_CHECK_EQ_I64(extent_height, tm_height);
        LIFT_CHECK(extent_width > 0);
    }

    // ---- ④ 表面 DC：确定性 + 文本像素带非空 ----
    g_current_case = "surface-text-deterministic";
    {
        lift::SurfaceFormat fmt;
        fmt.bpp = 32;
        fmt.rmask = 0x00FF0000u;
        fmt.gmask = 0x0000FF00u;
        fmt.bmask = 0x000000FFu;
        fmt.amask = 0xFF000000u;
        const std::uint32_t surface_ea = m.surfaces().create(256, 48, fmt);
        LIFT_CHECK_NE_U32(surface_ea, 0u);
        lift::GuestSurface* surface = m.surfaces().find(surface_ea);
        LIFT_CHECK(surface != nullptr);
        LIFT_CHECK(lift::fontAttachDcSurface(hdc, surface_ea, &m.surfaces()));
        if (!surface) return finish("gdi-font");
        std::uint8_t* pixels = m.surfaces().pixels(*surface);
        LIFT_CHECK(pixels != nullptr);
        const std::size_t bytes =
            static_cast<std::size_t>(surface->pitch) * static_cast<std::size_t>(surface->height);

        dc->state.text_color = 0x00FFFFFFu;  // 白
        dc->state.bk_mode = lift::kTransparent;
        const lift::TextDrawResult r1 =
            lift::gdiDrawText(m.fonts(), desc, dc->state, 0, 24, std::string_view(text, 6));
        LIFT_CHECK(r1.ok);
        LIFT_CHECK_EQ_U32(r1.glyphs, 3u);
        LIFT_CHECK(r1.pixels > 0u);
        const std::vector<std::uint8_t> first(pixels, pixels + bytes);
        // 文本像素带：有非零像素，且墨迹右界不超过 advance
        int max_x = -1;
        for (int y = 0; y < surface->height; ++y) {
            for (int x = 0; x < surface->width; ++x) {
                if (pixels[static_cast<std::size_t>(y) * surface->pitch +
                           static_cast<std::size_t>(x) * 4 + 2] != 0) {
                    if (x > max_x) max_x = x;
                }
            }
        }
        LIFT_CHECK(max_x >= 0);
        LIFT_CHECK(max_x < r1.advance);

        std::memset(pixels, 0, bytes);
        const lift::TextDrawResult r2 =
            lift::gdiDrawText(m.fonts(), desc, dc->state, 0, 24, std::string_view(text, 6));
        LIFT_CHECK(r2.ok);
        const std::vector<std::uint8_t> second(pixels, pixels + bytes);
        LIFT_CHECK(first == second);  // ★ 同一段文本两次渲染逐字节相同（确定性）
        LIFT_CHECK_EQ_I64(r2.advance, r1.advance);
    }

    return finish("gdi-font");
}
