// emulator-lift · 宿主基线补偿（`--baseline-lift` / `kBaselineLift`）落在 `gmptGlyphOrigin.y`（assets）
//
// ★ 口径（父级 2026-11-08 裁决）：**位移在 origin、不在度量**。
//   · cxx 的对应量 = `apps/emulator-cxx/src/core/text_raster.cpp:647` 的 `lift = 2`（唯一应用点 `:675`
//     `baseline = y_effective + current_ascent - lift`）⇒ 它只把**墨迹**上移 2px，行高不变。
//   · 引擎正文走 `GetGlyphOutlineA` + guest 自己合成（`0x46F893`：字形顶 = 基线 − `gmptGlyphOrigin.y`），
//     `TextOutA` 的基线补偿对正文**不生效** ⇒ 补偿必须烘进 `gmptGlyphOrigin.y`。
//
// ★ 判据（每条都会红）：
//   ① `lift/default`：缺省 `baseline_lift` 就是 **2**（cxx 的值；⛔ 不是"某个好看的像素数"）。
//   ② `lift/origin-delta`：`--baseline-lift 0` 与缺省值下，guest 读到的 `gmptGlyphOrigin.y` 差**正好 2**，
//      且方向正确（lift 越大 origin.y 越大 ⇒ 墨迹越**高**）。
//   ③ `lift/metrics-unchanged`：两次的 56 B `TEXTMETRICA` **逐字节相同**（`tmAscent` 仍是 FT 真值 ——
//      `SCOPE.md` 那条"补偿不写进度量"没有被破坏），`GetTextExtentPoint32A` 的宽 / 高也不变。
//   ④ `lift/knob`：把开关设成 5 ⇒ origin 差正好 5 ⇒ 位移量**不是**硬编码在绘制点的常量。
//   ⑤ `lift/pixels`：同一段文本在表面 DC 上画两次（lift=0 / lift=2）⇒ **墨迹像素总数不变**
//      （容差 0：整块字形被整体上移，没有被字形格 / 表面裁掉）+ **垂直质心移动正好 2px**。
//   ⑥ `lift/textout-shim`：`gdi32!TextOutA`（**经 shim 跑 guest 代码**）的墨迹也仍然只随开关移动 2px，
//      且 DC 的 `text_calls` / `pixels_written` 真的涨了（⛔ 不是"shim 静默 no-op 也算绿"）。
//
// ★ assets 档：缺宿主字体 ⇒ `LIFT_SKIP`（77）。注册位置见 CMakeLists（`if(EXISTS LIFT_HOST_FONT_PATH)`）。
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "check.h"
#include "runtime/font.h"
#include "runtime/machine.h"
#include "shims/register.h"

namespace {

constexpr std::uint32_t kCode = 0x00401000u;
constexpr std::uint32_t kCode2 = 0x00401400u;  // TextOutA 那条路径（经 shim）
constexpr std::uint32_t kLogFont = 0x00402000u;
constexpr std::uint32_t kMat = 0x00402100u;
constexpr std::uint32_t kText = 0x00402120u;
constexpr std::uint32_t kDisplay = 0x00402140u;
constexpr std::uint32_t kTm = 0x00402200u;
constexpr std::uint32_t kTm2 = 0x00402280u;  // lift=0 那次写**另一块**缓冲（否则失败也会"绿"）
constexpr std::uint32_t kGm = 0x00402300u;
constexpr std::uint32_t kSize = 0x00402400u;
constexpr std::uint32_t kTmPtr = 0x00402480u;  // 本次 GTM 写哪块缓冲（换一块 ⇒ 失败不会"空过"）
constexpr std::uint32_t kHdc = 0x00403000u;
constexpr std::uint32_t kFont = 0x00403004u;
constexpr std::uint32_t kStubBase = 0x7FF80000u;
constexpr std::uint32_t kSentinel = 0xFFFFFFFFu;
constexpr int kPixelSize = 20;
constexpr std::uint32_t kTmSize = 56;
// ★ 规格值：cxx `text_raster.cpp:647` 的 `lift = 2`。断言里写字面量（⛔ 不写 kBaselineLift：
//   拿实现常量当期望值时，把常量改坏也照样绿）。
constexpr int kSpecLift = 2;

struct Asm {
    explicit Asm(std::uint32_t code_base) : base(code_base) {}
    std::vector<std::uint8_t> b;
    std::uint32_t base = kCode;
    std::uint32_t ea() const { return base + static_cast<std::uint32_t>(b.size()); }
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

void put32(std::vector<std::uint8_t>& buf, std::size_t off, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) buf[off + i] = static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF);
}

std::uint32_t stubOf(lift::Machine& m, const char* name) {
    const lift::ShimDef* d = m.shims().find("gdi32", name);
    if (!d || !d->stub_ea) {
        std::fprintf(stderr, "stub missing: %s\n", name);
        return 0;
    }
    return d->stub_ea;
}

void reprimeStack(lift::Machine& m) {
    m.cpu().esp() = m.layout().stack_top - 4u;
    m.mem().wr32(m.cpu().esp(), 0xFFFFFFFFu);
    m.cpu().running = true;
}

bool runCode(lift::Machine& m) {
    m.cpu().eip = kCode;
    lift::RunOptions o;
    o.until_ea = kSentinel;
    o.max_insns = 200000;
    return m.run(o).reason == lift::StopReason::UntilEa;
}

std::int32_t i32At(lift::Machine& m, std::uint32_t ea) {
    return static_cast<std::int32_t>(m.mem().rd32(ea));
}

void setLift(int value) {
    lift::FontConfig cfg = lift::fontConfig();
    cfg.baseline_lift = value;
    lift::fontSetConfig(cfg);
}

// 表面 DC 上的墨迹统计（三个通道任一位非 0 = 墨迹；白字画在清零的表面上）
struct InkStats {
    std::int64_t count = 0;
    double centroid_y = 0.0;
};

InkStats measureInk(const lift::GuestSurface& surface, const std::uint8_t* pixels) {
    InkStats stats;
    double sum_y = 0.0;
    for (int y = 0; y < surface.height; ++y) {
        for (int x = 0; x < surface.width; ++x) {
            const std::size_t off = static_cast<std::size_t>(y) * surface.pitch +
                                    static_cast<std::size_t>(x) * 4u;
            if (pixels[off] != 0 || pixels[off + 1] != 0 || pixels[off + 2] != 0) {
                ++stats.count;
                sum_y += static_cast<double>(y);
            }
        }
    }
    if (stats.count > 0) stats.centroid_y = sum_y / static_cast<double>(stats.count);
    return stats;
}

}  // namespace

int main() {
    using namespace lift_test;

    lift::Machine m;
    std::string err;
    g_current_case = "setup";
    LIFT_CHECK(m.initBare(&err));
    LIFT_CHECK(m.prepareStack(&err));
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
    const char text[] = {'\x83', '\x65', '\x83', '\x58', '\x83', '\x67', 0};  // テスト
    LIFT_CHECK(m.placeData(kText, text, sizeof(text), &err));
    const char display[] = "DISPLAY";
    LIFT_CHECK(m.placeData(kDisplay, display, sizeof(display), &err));
    const std::vector<std::uint8_t> zeros(0x400, 0);
    LIFT_CHECK(m.placeData(kTm, zeros.data(), kTmSize, &err));
    LIFT_CHECK(m.placeData(kTm2, zeros.data(), kTmSize, &err));
    LIFT_CHECK(m.placeData(kGm, zeros.data(), 24, &err));
    LIFT_CHECK(m.placeData(kSize, zeros.data(), 8, &err));
    for (const std::uint32_t slot : {kHdc, kFont, kTmPtr}) {
        LIFT_CHECK(m.placeData(slot, zeros.data(), 4, &err));
    }

    // ---- guest 程序：建 DC / 建 -20 字体 / 选入 / GTM / GGO_METRICS('M') / extent ----
    Asm a(kCode);
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
    // GTM 的输出缓冲由数据槽 kTmPtr 决定 ⇒ 两次运行写**两块不同的**缓冲：
    // 第二次若静默失败（返回 0、不写 buffer），kTm2 仍是预清零值 ⇒ ③ 会红（而不是拿第一次的字节空过）。
    a.pushMem(kTmPtr);
    a.pushMem(kHdc);
    a.callRel(stubOf(m, "GetTextMetricsA"));
    // GetGlyphOutlineA(hdc, 'M', GGO_METRICS=0, &gm, 0, NULL, &mat)
    a.pushImm(kMat);
    a.pushImm(0);
    a.pushImm(0);
    a.pushImm(kGm);
    a.pushImm(0);
    a.pushImm(0x4D);
    a.pushMem(kHdc);
    a.callRel(stubOf(m, "GetGlyphOutlineA"));
    a.pushImm(kSize);
    a.pushImm(6);
    a.pushImm(kText);
    a.pushMem(kHdc);
    a.callRel(stubOf(m, "GetTextExtentPoint32A"));
    a.ret();
    LIFT_CHECK(m.placeCode(kCode, a.b.data(), a.b.size(), &err));

    // ---- ① 缺省值 = cxx 的 2 ----
    g_current_case = "lift/default";
    LIFT_CHECK_EQ_I64(lift::fontConfig().baseline_lift, kSpecLift);
    LIFT_CHECK_EQ_I64(lift::kBaselineLift, kSpecLift);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(lift::FontConfig().baseline_lift), kSpecLift);

    // ---- 缺省（lift=2）跑一遍 ----
    g_current_case = "run-lift-default";
    m.mem().wr32(kTmPtr, kTm);  // 这次 GTM 写 kTm
    LIFT_CHECK(runCode(m));
    const std::int32_t origin_y_lift = i32At(m, kGm + 12);  // gmptGlyphOrigin.y
    std::vector<std::uint8_t> tm_lift(kTmSize);
    for (std::uint32_t i = 0; i < kTmSize; ++i) tm_lift[i] = m.mem().rd8(kTm + i);
    const std::int32_t ascent_lift = i32At(m, kTm + 4);
    const std::int32_t height_lift = i32At(m, kTm + 0);
    const std::int32_t extent_w_lift = i32At(m, kSize + 0);
    const std::int32_t extent_h_lift = i32At(m, kSize + 4);
    std::printf("lift=2 : gmptGlyphOrigin.y=%d tmAscent=%d tmHeight=%d extent=%dx%d\n",
                origin_y_lift, ascent_lift, height_lift, extent_w_lift, extent_h_lift);

    // ---- ② 关掉补偿（lift=0）再跑一遍：origin 差正好 2，方向正确 ----
    g_current_case = "lift/origin-delta";
    setLift(0);
    reprimeStack(m);
    m.mem().wr32(kTmPtr, kTm2);  // 这次写**另一块**（kTm2 预清零）
    LIFT_CHECK(runCode(m));
    const std::int32_t origin_y_off = i32At(m, kGm + 12);
    std::printf("lift=0 : gmptGlyphOrigin.y=%d\n", origin_y_off);
    LIFT_CHECK_EQ_I64(origin_y_lift - origin_y_off, kSpecLift);
    LIFT_CHECK(origin_y_lift > origin_y_off);  // lift 越大 origin.y 越大 ⇒ 墨迹越**高**（屏幕 y 越小）

    // ---- ③ 度量不受补偿影响：两块缓冲的 56 B 逐字节相同；extent 的宽 / 高也不变 ----
    g_current_case = "lift/metrics-unchanged";
    {
        bool same = true;
        for (std::uint32_t i = 0; i < kTmSize; ++i) {
            if (tm_lift[i] != m.mem().rd8(kTm2 + i)) same = false;
        }
        LIFT_CHECK(same);  // tmAscent / tmHeight / tmDescent ... 全部不变（补偿⛔不写进度量）
    }
    LIFT_CHECK_EQ_I64(i32At(m, kTm2 + 4), ascent_lift);
    LIFT_CHECK_EQ_I64(i32At(m, kSize + 0), extent_w_lift);
    LIFT_CHECK_EQ_I64(i32At(m, kSize + 4), extent_h_lift);
    LIFT_CHECK(ascent_lift > 0);

    // ---- ④ 开关是"值"不是"硬编码 2"：设成 5 ⇒ origin 差正好 5 ----
    g_current_case = "lift/knob";
    setLift(5);
    reprimeStack(m);
    LIFT_CHECK(runCode(m));
    const std::int32_t origin_y_5 = i32At(m, kGm + 12);
    LIFT_CHECK_EQ_I64(origin_y_5 - origin_y_off, 5);
    std::printf("lift=5 : gmptGlyphOrigin.y=%d\n", origin_y_5);

    // ---- ⑤ 像素证据：墨迹总数不变 + 垂直质心正好移动 2px ----
    g_current_case = "lift/pixels";
    {
        lift::SurfaceFormat fmt;
        fmt.bpp = 32;
        fmt.rmask = 0x00FF0000u;
        fmt.gmask = 0x0000FF00u;
        fmt.bmask = 0x000000FFu;
        fmt.amask = 0xFF000000u;
        const std::uint32_t surface_ea = m.surfaces().create(192, 64, fmt);
        LIFT_CHECK_NE_U32(surface_ea, 0u);
        lift::GuestSurface* surface = m.surfaces().find(surface_ea);
        LIFT_CHECK(surface != nullptr);
        const std::uint32_t hdc = m.mem().rd32(kHdc);
        LIFT_CHECK(lift::fontAttachDcSurface(hdc, surface_ea, &m.surfaces()));
        lift::GdiDcObject* dc = lift::gdiAsDc(m.handles(), hdc);
        LIFT_CHECK(dc != nullptr);
        if (!surface || !dc) return finish("text-baseline-lift");
        std::uint8_t* pixels = m.surfaces().pixels(*surface);
        LIFT_CHECK(pixels != nullptr);
        if (!pixels) return finish("text-baseline-lift");
        dc->state.text_color = 0x00FFFFFFu;  // 白
        dc->state.bk_mode = lift::kTransparent;
        const std::size_t bytes =
            static_cast<std::size_t>(surface->pitch) * static_cast<std::size_t>(surface->height);
        const lift::GdiFontDesc desc = lift::gdiEffectiveFont(dc->state);
        std::printf("desc: lfHeight=%d weight=%d\n", desc.height, desc.weight);

        InkStats at[2];
        const int lifts[2] = {0, kSpecLift};
        for (int i = 0; i < 2; ++i) {
            setLift(lifts[i]);
            std::memset(pixels, 0, bytes);
            const lift::TextDrawResult r =
                lift::gdiDrawText(m.fonts(), desc, dc->state, 0, 30, std::string_view(text, 6));
            LIFT_CHECK(r.ok);
            LIFT_CHECK(r.pixels > 0u);
            at[i] = measureInk(*surface, pixels);
            LIFT_CHECK(at[i].count > 0);
            std::printf("draw(lift=%d) : ink=%lld centroid_y=%.4f\n", lifts[i],
                        (long long)at[i].count, at[i].centroid_y);
        }
        // ★ 墨迹像素总数**不变**（容差 0）：整块字形被整体上移，没有被字形格 / 表面裁掉。
        LIFT_CHECK_EQ_I64(at[1].count, at[0].count);
        // ★ 垂直质心正好上移 2px（屏幕 y 变小）。
        const double moved = at[0].centroid_y - at[1].centroid_y;
        LIFT_CHECK(moved > kSpecLift - 1e-6 && moved < kSpecLift + 1e-6);
        LIFT_CHECK(lift::fontDetachDc(hdc));
    }

    // ---- ⑥ `gdi32!TextOutA` 那条路径（**经 shim 跑 guest 代码**）：仍只随开关移动 2px ----
    //   ★ 与 ⑤ 的区别：⑤ 直接调宿主 helper（`gdiDrawText`，GGO 之外的旁路），⑥ 走 guest → shim → 同一条
    //     绘制路径，并把 `dc.state` 的记账（text_calls / pixels_written）也对上 —— 否则"补偿搬了位置、
    //     TextOutA 没坏"这件事没有任何一条守卫经过 Win32 形状层。
    g_current_case = "lift/textout-shim";
    {
        lift::SurfaceFormat fmt;
        fmt.bpp = 32;
        fmt.rmask = 0x00FF0000u;
        fmt.gmask = 0x0000FF00u;
        fmt.bmask = 0x000000FFu;
        fmt.amask = 0xFF000000u;
        const std::uint32_t surface_ea = m.surfaces().create(192, 64, fmt);
        LIFT_CHECK_NE_U32(surface_ea, 0u);
        lift::GuestSurface* surface = m.surfaces().find(surface_ea);
        LIFT_CHECK(surface != nullptr);
        const std::uint32_t hdc = m.mem().rd32(kHdc);
        LIFT_CHECK(lift::fontAttachDcSurface(hdc, surface_ea, &m.surfaces()));
        lift::GdiDcObject* dc = lift::gdiAsDc(m.handles(), hdc);
        LIFT_CHECK(dc != nullptr);
        if (!surface || !dc) return finish("text-baseline-lift");
        std::uint8_t* pixels = m.surfaces().pixels(*surface);
        LIFT_CHECK(pixels != nullptr);
        if (!pixels) return finish("text-baseline-lift");
        dc->state.text_color = 0x00FFFFFFu;
        dc->state.bk_mode = lift::kTransparent;
        const std::size_t bytes =
            static_cast<std::size_t>(surface->pitch) * static_cast<std::size_t>(surface->height);

        // guest 程序：TextOutA(hdc, 0, 30, text, 6)
        {
            Asm b(kCode2);
            b.pushImm(6);
            b.pushImm(kText);
            b.pushImm(30);
            b.pushImm(0);
            b.pushMem(kHdc);
            b.callRel(stubOf(m, "TextOutA"));
            b.ret();
            LIFT_CHECK(m.placeCode(kCode2, b.b.data(), b.b.size(), &err));
        }
        InkStats at[2];
        const int lifts[2] = {0, kSpecLift};
        std::uint32_t pixels_before = dc->state.pixels_written;
        for (int i = 0; i < 2; ++i) {
            setLift(lifts[i]);
            std::memset(pixels, 0, bytes);
            reprimeStack(m);
            m.cpu().eip = kCode2;
            lift::RunOptions o;
            o.until_ea = kSentinel;
            o.max_insns = 200000;
            LIFT_CHECK(m.run(o).reason == lift::StopReason::UntilEa);
            at[i] = measureInk(*surface, pixels);
            LIFT_CHECK(at[i].count > 0);
            std::printf("TextOutA(lift=%d) : ink=%lld centroid_y=%.4f\n", lifts[i],
                        (long long)at[i].count, at[i].centroid_y);
        }
        // 记账这条形状层路径确实跑到了（⛔ 不是"shim 静默 no-op 也算绿"）
        LIFT_CHECK(dc->state.text_calls >= 2u);
        LIFT_CHECK(dc->state.pixels_written > pixels_before);
        LIFT_CHECK_EQ_I64(at[1].count, at[0].count);
        const double moved = at[0].centroid_y - at[1].centroid_y;
        LIFT_CHECK(moved > kSpecLift - 1e-6 && moved < kSpecLift + 1e-6);
        LIFT_CHECK(lift::fontDetachDc(hdc));
    }

    return finish("text-baseline-lift");
}
