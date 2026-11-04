// emulator-lift · GDI 对象表 / DC 状态 / GGO 格式算术 / 自洽（P2；pure）
//
// 判据（plan §5 B3 / §6 P2）：
//   ① 对象与状态：DC / 字体走 `HandleTable`；`SelectObject` 返回**旧对象**且再选回去能恢复；
//      `DeleteObject` 对"仍被选中"的对象返回 FALSE（不释放）；`GetCurrentObject` 拿得到当前字体。
//   ② 屏幕 DC（`CreateICA("DISPLAY")`）不崩、有**确定性产物**（DC 的 text_calls / glyphs / pixels 记账）。
//   ③ GGO 的**格式算术**（用可注入的合成字形源，不需要字体文件）：
//      - `GGO_BITMAP` 行 = ((w+31)>>5)<<2（DWORD 对齐）；`GGO_GRAY4/8` 行 = (w+3)&~3；
//      - GRAY4 值域 0..16（满覆盖 = 16）、GRAY8 值域 0..64（满覆盖 = 64）；
//      - `cbBuffer == 0` 返回所需字节数；写出的位图与 `gmBlackBoxX/Y` / `gmptGlyphOrigin` / `gmCellIncX` 自洽；
//      - 负 origin 如实写出（不夹到 0）。
//   ④ **自洽**：`GetTextExtentPoint32A(s)` 的宽 == Σ `GetGlyphOutlineA` 的 gmCellIncX == `TextOutA`
//      实际推进；三条走同一条取字/推进路径（`gdiGlyph`）。
//   ⑤ 表面 DC：`gdiDrawText` 真的写进 guest surface 的像素（裁剪 / 文字色 / bk mode），两次渲染字节相同。
//
// ★ 为什么用合成字形源：pure 档不能依赖 corpus 字体（那属于 assets 档）；这里要测的是**格式算术**，
//   合成源给出确定的黑框 / origin / advance（advance = 6 = floor(6.5px)，变异"floor→round"会变 7）。
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "check.h"
#include "runtime/font.h"
#include "runtime/machine.h"
#include "shims/register.h"

namespace {

constexpr std::uint32_t kCode = 0x00401000u;
constexpr std::uint32_t kCode2 = 0x00401400u;
constexpr std::uint32_t kProc = 0x00401800u;  // EnumFontFamiliesExA 的 guest 回调（⛔ 与主程序不重叠）
constexpr std::uint32_t kLogFont = 0x00402000u;
constexpr std::uint32_t kGm = 0x00402100u;
constexpr std::uint32_t kGm2 = 0x00402140u;
constexpr std::uint32_t kGm1 = 0x00402180u;
constexpr std::uint32_t kBuf = 0x00402200u;
constexpr std::uint32_t kMat = 0x00402600u;
constexpr std::uint32_t kText = 0x00402620u;
constexpr std::uint32_t kDisplay = 0x00402640u;
constexpr std::uint32_t kSize = 0x00402660u;
constexpr std::uint32_t kTm = 0x00402680u;
constexpr std::uint32_t kEnumResult = 0x00402700u;
constexpr std::uint32_t kEnumSeen = 0x00402704u;
constexpr std::uint32_t kEnumFaceFirst = 0x00402708u;
constexpr std::uint32_t kEnumLparam = 0x0040270Cu;
constexpr std::uint32_t kHdc = 0x00402710u;
constexpr std::uint32_t kFont = 0x00402714u;
constexpr std::uint32_t kCurrentFont = 0x00402718u;
constexpr std::uint32_t kOldObject = 0x0040271Cu;
constexpr std::uint32_t kOldObject2 = 0x00402720u;
constexpr std::uint32_t kNeeded = 0x00402724u;
constexpr std::uint32_t kGot = 0x00402728u;
constexpr std::uint32_t kDelSelected = 0x0040272Cu;
constexpr std::uint32_t kRestored = 0x00402730u;
constexpr std::uint32_t kDelFree = 0x00402734u;
constexpr std::uint32_t kDelAgain = 0x00402738u;
constexpr std::uint32_t kDelDc = 0x0040273Cu;
constexpr std::uint32_t kDelDcObject = 0x00402740u;
constexpr std::uint32_t kBkOld = 0x00402744u;
constexpr std::uint32_t kTextColorOld = 0x00402748u;
constexpr std::uint32_t kStockBrush = 0x0040274Cu;
constexpr std::uint32_t kNeed1 = 0x00402750u;
constexpr std::uint32_t kStubBase = 0x7FF80000u;
constexpr std::uint32_t kSentinel = 0xFFFFFFFFu;
constexpr std::uint32_t kFixtureHeap = 0x00100000u;
constexpr std::uint32_t kSurfaceW = 64;
constexpr std::uint32_t kSurfaceH = 32;

struct Asm {
    explicit Asm(std::uint32_t code_base = kCode) : base(code_base) {}
    std::vector<std::uint8_t> b;
    std::uint32_t base;
    std::uint32_t ea() const { return base + static_cast<std::uint32_t>(b.size()); }
    void u8(std::uint8_t v) { b.push_back(v); }
    void u32(std::uint32_t v) {
        for (int i = 0; i < 4; ++i) b.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF));
    }
    void pushImm(std::uint32_t v) {
        u8(0x68);
        u32(v);
    }
    void pushMem(std::uint32_t ea_value) {
        u8(0xFF);
        u8(0x35);
        u32(ea_value);
    }
    void callRel(std::uint32_t target) {
        const std::uint32_t at = ea();
        u8(0xE8);
        u32(static_cast<std::uint32_t>(static_cast<std::int32_t>(target - (at + 5))));
    }
    void storeEax(std::uint32_t slot) {
        u8(0xA3);
        u32(slot);
    }
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

void reprimeStack(lift::Machine& m) {
    m.cpu().esp() = m.layout().stack_top - 4u;
    m.mem().wr32(m.cpu().esp(), 0xFFFFFFFFu);
    m.cpu().running = true;
}

lift::RunResult run(lift::Machine& m, std::uint32_t code_ea) {
    m.cpu().eip = code_ea;
    lift::RunOptions o;
    o.until_ea = kSentinel;
    o.max_insns = 200000;
    return m.run(o);
}

// 直接从**覆盖率**构造一个 mask（格式算术的单测用；与字体文件无关）
lift::GlyphMask makeMask(int w, int h, std::uint8_t value) {
    lift::GlyphMask m;
    m.black_box_x = w;
    m.black_box_y = h;
    m.origin_x = -2;
    m.origin_y = h;
    m.advance = 6;
    m.coverage.assign(static_cast<std::size_t>(w) * static_cast<std::size_t>(h), value);
    return m;
}

// 测试自己算 GGO 行字节数（⛔ 不抄实现里的函数）
std::uint32_t rowBytesFor(std::uint32_t format, int width) {
    if (format == lift::kGgoBitmap) return static_cast<std::uint32_t>(((width + 31) >> 5) << 2);
    return static_cast<std::uint32_t>((width + 3) & ~3);
}

// 独立实现的覆盖率 → 级数（GRAY4 17 级 / GRAY8 65 级），用来核对写出的字节
std::uint8_t grayLevel(std::uint32_t max_level, std::uint32_t coverage) {
    return static_cast<std::uint8_t>((coverage * max_level + 127u) / 255u);
}

}  // namespace

int main() {
    using namespace lift_test;

    // =====================================================================================
    // ⓪ 格式算术（纯函数 `gdiPackGlyph`；不需要 Machine）
    // =====================================================================================
    {
        g_current_case = "pack-mono-row-align";
        const lift::GlyphMask m = makeMask(7, 3, 255);
        std::vector<std::uint8_t> out;
        std::uint32_t pitch = 0;
        LIFT_CHECK(lift::gdiPackGlyph(m, lift::kGgoBitmap, &out, &pitch));
        LIFT_CHECK_EQ_U32(pitch, rowBytesFor(lift::kGgoBitmap, 7));  // 4
        LIFT_CHECK_EQ_U32(pitch, 4u);
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(out.size()), 12u);  // 4 * 3
        LIFT_CHECK_EQ_U32(out[0], 0xFEu);        // 7 位满覆盖（最高位起）
        LIFT_CHECK_EQ_U32(out[4], 0xFEu);        // 第二行同样从行首开始（DWORD 行对齐）
    }
    {
        g_current_case = "pack-gray4-range";
        const lift::GlyphMask m = makeMask(7, 3, 255);
        std::vector<std::uint8_t> out;
        std::uint32_t pitch = 0;
        LIFT_CHECK(lift::gdiPackGlyph(m, lift::kGgoGray4, &out, &pitch));
        LIFT_CHECK_EQ_U32(pitch, rowBytesFor(lift::kGgoGray4, 7));  // (7+3)&~3 = 8
        LIFT_CHECK_EQ_U32(pitch, 8u);
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(out.size()), 24u);
        std::uint8_t max_value = 0;
        for (const std::uint8_t v : out) {
            LIFT_CHECK(v <= lift::kGray4MaxLevel);  // 值域 0..16（⛔ 不是 0..255）
            if (v > max_value) max_value = v;
        }
        LIFT_CHECK_EQ_U32(max_value, 16u);  // 满覆盖 = 16（17 级）；按 2^n 会得 15
        LIFT_CHECK_EQ_U32(out[0], grayLevel(16, 255));  // 独立算式交叉核对
    }
    {
        g_current_case = "pack-gray8-range";
        const lift::GlyphMask m = makeMask(5, 2, 255);
        std::vector<std::uint8_t> out;
        std::uint32_t pitch = 0;
        LIFT_CHECK(lift::gdiPackGlyph(m, lift::kGgoGray8, &out, &pitch));
        LIFT_CHECK_EQ_U32(pitch, rowBytesFor(lift::kGgoGray8, 5));  // (5+3)&~3 = 8
        LIFT_CHECK_EQ_U32(pitch, 8u);
        std::uint8_t max_value = 0;
        for (const std::uint8_t v : out) {
            LIFT_CHECK(v <= lift::kGray8MaxLevel);
            if (v > max_value) max_value = v;
        }
        LIFT_CHECK_EQ_U32(max_value, 64u);  // 65 级
    }
    {
        g_current_case = "pack-quantize-midtone";
        // 覆盖率 128：GRAY4 → round(128*16/255) = 8；GRAY8 → round(128*64/255) = 32
        const lift::GlyphMask m = makeMask(2, 2, 128);
        std::vector<std::uint8_t> g4;
        std::vector<std::uint8_t> g8;
        LIFT_CHECK(lift::gdiPackGlyph(m, lift::kGgoGray4, &g4, nullptr));
        LIFT_CHECK(lift::gdiPackGlyph(m, lift::kGgoGray8, &g8, nullptr));
        LIFT_CHECK_EQ_U32(g4[0], 8u);
        LIFT_CHECK_EQ_U32(g8[0], 32u);
    }
    {
        g_current_case = "pack-empty-and-unsupported";
        lift::GlyphMask empty;
        std::vector<std::uint8_t> out;
        std::uint32_t pitch = 123;
        LIFT_CHECK(lift::gdiPackGlyph(empty, lift::kGgoBitmap, &out, &pitch));
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(out.size()), 0u);
        LIFT_CHECK_EQ_U32(pitch, 0u);
        // 未实现的格式：如实拒绝（⛔ 不猜布局）
        LIFT_CHECK(!lift::gdiPackGlyph(empty, lift::kGgoNative, &out, &pitch));
        LIFT_CHECK(!lift::gdiPackGlyph(empty, lift::kGgoGray2, &out, &pitch));
    }
    {
        g_current_case = "cp932-uchar";
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(lift::gdiCp932Char(0x41u)), 0x41u);
        // 0x82A0（CP932 前导字节在高位）⇒ あ U+3042
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(lift::gdiCp932Char(0x82A0u)), 0x3042u);
    }

    // =====================================================================================
    // ① 对象表 / DC / SelectObject / DeleteObject / GGO / 自洽（guest 真的跑一遍 shim）
    // =====================================================================================
    lift::Machine m;
    std::string err;
    g_current_case = "setup";
    LIFT_CHECK(m.initBare(&err));
    LIFT_CHECK(m.prepareStack(&err));
    LIFT_CHECK(m.heap().init(&m.mem(), m.layout().heap_base, kFixtureHeap));
    m.fonts().setSyntheticForTest(true);  // ★ 可注入的合成字形源（pure 档不读字体文件）
    lift::registerGdiFontShims(m.shims());
    {
        // bare 机器没有 bind：给每条**已实现**的 def 一个确定性桩地址（与 mechanism.cpp 同法）
        const std::size_t count = m.shims().all().size();
        for (std::uint32_t id = 0; id < count; ++id) {
            lift::ShimDef* d = const_cast<lift::ShimDef*>(m.shims().byId(id));
            if (!d || !d->fn) continue;
            d->stub_ea = kStubBase + id * 4u;
            m.cache().installShim(d->stub_ea, d->id);
        }
    }

    // LOGFONTA：lfHeight = -20（20px），weight = 400，lfFaceName = 「ＭＳ ゴシック」（会被忽略）
    std::vector<std::uint8_t> logfont(60, 0);
    put32(logfont, 0, static_cast<std::uint32_t>(-20));
    put32(logfont, 16, 400);
    const char engine_face[] = "\x82\x6C\x82\x72\x20\x83\x53\x83\x56\x83\x62\x83\x4E";
    std::memcpy(&logfont[28], engine_face, sizeof(engine_face) - 1);
    LIFT_CHECK(m.placeData(kLogFont, logfont.data(), logfont.size(), &err));
    std::vector<std::uint8_t> mat(16, 0);
    put32(mat, 0, 0x00010000u);
    put32(mat, 12, 0x00010000u);
    LIFT_CHECK(m.placeData(kMat, mat.data(), mat.size(), &err));
    const char text[] = {'A', static_cast<char>(0x82), static_cast<char>(0xA0), 'B', 0};
    LIFT_CHECK(m.placeData(kText, text, sizeof(text), &err));
    const char display[] = "DISPLAY";
    LIFT_CHECK(m.placeData(kDisplay, display, sizeof(display), &err));
    const std::vector<std::uint8_t> zeros(0x400, 0);
    LIFT_CHECK(m.placeData(kBuf, zeros.data(), zeros.size(), &err));
    LIFT_CHECK(m.placeData(kTm, zeros.data(), 56, &err));
    LIFT_CHECK(m.placeData(kGm, zeros.data(), 20, &err));
    LIFT_CHECK(m.placeData(kGm2, zeros.data(), 20, &err));
    LIFT_CHECK(m.placeData(kGm1, zeros.data(), 20, &err));
    LIFT_CHECK(m.placeData(kSize, zeros.data(), 8, &err));
    for (const std::uint32_t slot : {kHdc, kFont, kCurrentFont, kOldObject, kOldObject2, kNeeded, kGot,
                                     kDelSelected, kRestored, kDelFree, kDelAgain, kDelDc, kDelDcObject,
                                     kBkOld, kTextColorOld, kStockBrush, kEnumResult, kEnumSeen,
                                     kEnumFaceFirst, kEnumLparam, kNeed1}) {
        LIFT_CHECK(m.placeData(slot, zeros.data(), 4, &err));
    }

    // EnumFontFamiliesExA 的 guest 回调：记录 lfFaceName[0] 与 lParam，返回 0（继续枚举）
    const std::vector<std::uint8_t> proc = {
        0x8B, 0x44, 0x24, 0x04,              // mov eax,[esp+4]      ; LOGFONTA*
        0x0F, 0xB6, 0x48, 0x1C,              // movzx ecx, byte [eax+28]
        0x89, 0x0D, 0x08, 0x27, 0x40, 0x00,  // mov [kEnumFaceFirst], ecx
        0x8B, 0x54, 0x24, 0x10,              // mov edx,[esp+16]     ; lParam
        0x89, 0x15, 0x0C, 0x27, 0x40, 0x00,  // mov [kEnumLparam], edx
        0xC7, 0x05, 0x04, 0x27, 0x40, 0x00, 0x01, 0x00, 0x00, 0x00,  // mov dword [kEnumSeen],1
        0x31, 0xC0,                          // xor eax,eax
        0xC2, 0x10, 0x00};                   // ret 16
    LIFT_CHECK(m.placeCode(kProc, proc.data(), proc.size(), &err));

    // ---- 程序 1：建 DC / 字体 / 选入 / 当前对象 / 度量 / 枚举 / GGO / extent / 绘制 / 删已选中 ----
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
    // SelectObject(hdc, font) —— 返回缺省 stock 字体
    a.pushMem(kFont);
    a.pushMem(kHdc);
    a.callRel(stubOf(m, "SelectObject"));
    a.storeEax(kOldObject);
    // SelectObject(hdc, font) 再来一次 —— 返回旧对象 = font 自己
    a.pushMem(kFont);
    a.pushMem(kHdc);
    a.callRel(stubOf(m, "SelectObject"));
    a.storeEax(kOldObject2);
    // GetCurrentObject(hdc, OBJ_FONT=6) ⇒ font
    a.pushImm(6);
    a.pushMem(kHdc);
    a.callRel(stubOf(m, "GetCurrentObject"));
    a.storeEax(kCurrentFont);
    // GetTextMetricsA(hdc, &tm)
    a.pushImm(kTm);
    a.pushMem(kHdc);
    a.callRel(stubOf(m, "GetTextMetricsA"));
    // EnumFontFamiliesExA(hdc, NULL, proc, 0xABCD, 0)
    a.pushImm(0);
    a.pushImm(0xABCDu);
    a.pushImm(kProc);
    a.pushImm(0);
    a.pushMem(kHdc);
    a.callRel(stubOf(m, "EnumFontFamiliesExA"));
    a.storeEax(kEnumResult);
    // GetGlyphOutlineA(hdc, 'A', GGO_GRAY4=5, &gm, 0, NULL, &mat) ⇒ 所需字节数
    a.pushImm(kMat);
    a.pushImm(0);
    a.pushImm(0);
    a.pushImm(kGm);
    a.pushImm(5);
    a.pushImm(0x41);
    a.pushMem(kHdc);
    a.callRel(stubOf(m, "GetGlyphOutlineA"));
    a.storeEax(kNeeded);
    // GetGlyphOutlineA(hdc, 'A', 5, &gm2, needed, buf, &mat) ⇒ 实际写入
    a.pushImm(kMat);
    a.pushImm(kBuf);
    a.pushMem(kNeeded);
    a.pushImm(kGm2);
    a.pushImm(5);
    a.pushImm(0x41);
    a.pushMem(kHdc);
    a.callRel(stubOf(m, "GetGlyphOutlineA"));
    a.storeEax(kGot);
    // GetGlyphOutlineA(hdc, 'B'=0x42, GGO_GRAY4, &gm1, 0, NULL, &mat) ⇒ 负 origin 的用例
    //   合成源：'B' = 0x42（偶数）⇒ gmptGlyphOrigin.x = -1；black_box_x = 5 + (0x42 % 3) = 5
    a.pushImm(kMat);
    a.pushImm(0);
    a.pushImm(0);
    a.pushImm(kGm1);
    a.pushImm(5);
    a.pushImm(0x42);
    a.pushMem(kHdc);
    a.callRel(stubOf(m, "GetGlyphOutlineA"));
    a.storeEax(kNeed1);
    // GetTextExtentPoint32A(hdc, text, 4, &size)
    a.pushImm(kSize);
    a.pushImm(4);
    a.pushImm(kText);
    a.pushMem(kHdc);
    a.callRel(stubOf(m, "GetTextExtentPoint32A"));
    // SetBkMode(hdc, TRANSPARENT=1) ⇒ 返回旧值
    a.pushImm(1);
    a.pushMem(kHdc);
    a.callRel(stubOf(m, "SetBkMode"));
    a.storeEax(kBkOld);
    // SetTextColor(hdc, 0x000000FF) ⇒ 返回旧值
    a.pushImm(0x000000FFu);
    a.pushMem(kHdc);
    a.callRel(stubOf(m, "SetTextColor"));
    a.storeEax(kTextColorOld);
    // TextOutA(hdc, 10, 20, text, 4) —— 屏幕 DC：只记账，不崩
    a.pushImm(4);
    a.pushImm(kText);
    a.pushImm(20);
    a.pushImm(10);
    a.pushMem(kHdc);
    a.callRel(stubOf(m, "TextOutA"));
    // DeleteObject(font)：仍被选中 ⇒ FALSE
    a.pushMem(kFont);
    a.callRel(stubOf(m, "DeleteObject"));
    a.storeEax(kDelSelected);
    // SelectObject(hdc, oldObject) ⇒ 恢复；返回 font
    a.pushMem(kOldObject);
    a.pushMem(kHdc);
    a.callRel(stubOf(m, "SelectObject"));
    a.storeEax(kRestored);
    a.ret();
    LIFT_CHECK(m.placeCode(kCode, a.b.data(), a.b.size(), &err));

    g_current_case = "run-1";
    LIFT_CHECK(run(m, kCode).reason == lift::StopReason::UntilEa);

    const std::uint32_t hdc = m.mem().rd32(kHdc);
    const std::uint32_t hfont = m.mem().rd32(kFont);
    g_current_case = "objects";
    LIFT_CHECK_NE_U32(hdc, 0u);
    LIFT_CHECK_NE_U32(hfont, 0u);
    LIFT_CHECK_EQ_U32(m.mem().rd32(kOldObject), lift::gdiStockObject(17));  // 缺省 stock 字体
    LIFT_CHECK_EQ_U32(m.mem().rd32(kOldObject2), hfont);  // 第二次选入返回**旧对象 = font**
    LIFT_CHECK_EQ_U32(m.mem().rd32(kCurrentFont), hfont); // GetCurrentObject(OBJ_FONT)
    LIFT_CHECK_EQ_U32(m.mem().rd32(kRestored), hfont);    // 选回旧对象，返回 font
    LIFT_CHECK_EQ_U32(m.mem().rd32(kDelSelected), 0u);    // 仍被选中 ⇒ FALSE
    LIFT_CHECK(lift::gdiAsFont(m.handles(), hfont) != nullptr);  // 没被释放
    {
        lift::GdiDcObject* dc = lift::gdiAsDc(m.handles(), hdc);
        LIFT_CHECK(dc != nullptr);
        if (dc) {
            LIFT_CHECK_EQ_U32(dc->state.font_handle, lift::gdiStockObject(17));  // 已恢复
            LIFT_CHECK_EQ_U32(dc->state.text_calls, 1u);
            LIFT_CHECK_EQ_U32(dc->state.glyphs_drawn, 3u);  // 'A' + あ + 'B'（CP932 双字节算一个字）
            LIFT_CHECK(dc->state.pixels_written > 0u);      // 屏幕 DC 的确定性记账
            LIFT_CHECK_EQ_U32(dc->state.bk_mode, static_cast<std::uint32_t>(lift::kTransparent));
            LIFT_CHECK_EQ_U32(dc->state.text_color, 0x000000FFu);
            LIFT_CHECK_EQ_U32(m.mem().rd32(kBkOld), 2u);             // 旧 bk mode = OPAQUE
            LIFT_CHECK_EQ_U32(m.mem().rd32(kTextColorOld), 0x00FFFFFFu);
        }
    }

    g_current_case = "text-metrics";
    LIFT_CHECK_EQ_I64(m.mem().rd32(kTm + 0), 22);    // synthetic：height = size + 2
    LIFT_CHECK_EQ_I64(m.mem().rd32(kTm + 4), 20);    // tmAscent
    LIFT_CHECK_EQ_I64(m.mem().rd32(kTm + 8), 2);     // tmDescent
    LIFT_CHECK_EQ_I64(m.mem().rd32(kTm + 28), 400);  // tmWeight

    g_current_case = "enum-font-families";
    LIFT_CHECK_EQ_U32(m.mem().rd32(kEnumSeen), 1u);          // guest 回调真的被调到（恰好一次）
    LIFT_CHECK_EQ_U32(m.mem().rd32(kEnumFaceFirst), 0x41u);  // lfFaceName[0] = 'A'（Amayui CN）
    LIFT_CHECK_EQ_U32(m.mem().rd32(kEnumLparam), 0xABCDu);   // lParam 原样送到
    LIFT_CHECK_EQ_U32(m.mem().rd32(kEnumResult), 0u);        // 返回值 = 回调的返回值

    g_current_case = "ggo-needed-and-metrics";
    const std::uint32_t needed = m.mem().rd32(kNeeded);
    // 'A' = 0x41：合成字形 black_box_x = 5 + (0x41 % 3) = 7、black_box_y = 20
    // GRAY4 pitch = (7+3)&~3 = 8 ⇒ needed = 8*20 = 160
    LIFT_CHECK_EQ_U32(needed, 160u);
    LIFT_CHECK_EQ_U32(m.mem().rd32(kGot), needed);
    LIFT_CHECK_EQ_U32(m.mem().rd32(kGm + 0), 7u);   // gmBlackBoxX
    LIFT_CHECK_EQ_U32(m.mem().rd32(kGm + 4), 20u);  // gmBlackBoxY
    LIFT_CHECK_EQ_I64(static_cast<std::int32_t>(m.mem().rd32(kGm + 8)), 1);  // gmptGlyphOrigin.x（'A'）
    LIFT_CHECK_EQ_I64(m.mem().rd32(kGm + 12), 20);  // gmptGlyphOrigin.y
    LIFT_CHECK_EQ_I64(m.mem().rd16(kGm + 16), 6);   // gmCellIncX（floor(6.5)）
    LIFT_CHECK_EQ_I64(m.mem().rd16(kGm + 18), 0);   // gmCellIncY
    // 第二份 metrics 与第一份一致（同一个字形、同一条路径）
    LIFT_CHECK_EQ_U32(m.mem().rd32(kGm2 + 0), m.mem().rd32(kGm + 0));
    LIFT_CHECK_EQ_U32(m.mem().rd16(kGm2 + 16), m.mem().rd16(kGm + 16));
    // 负 origin 如实写出（'B' 的合成 origin.x = -1）
    LIFT_CHECK_EQ_U32(m.mem().rd32(kGm1 + 0), 5u);   // gmBlackBoxX
    LIFT_CHECK_EQ_I64(static_cast<std::int32_t>(m.mem().rd32(kGm1 + 8)), -1);  // 负 origin 如实写出
    LIFT_CHECK_EQ_I64(m.mem().rd16(kGm1 + 16), 6);   // gmCellIncX

    g_current_case = "ggo-bitmap-range-and-alignment";
    {
        std::uint8_t max_value = 0;
        for (std::uint32_t i = 0; i < needed; ++i) {
            const std::uint8_t v = m.mem().rd8(kBuf + i);
            LIFT_CHECK(v <= 16);
            if (v > max_value) max_value = v;
        }
        LIFT_CHECK_EQ_U32(max_value, 16u);         // 满覆盖像素 (0,0) ⇒ 16
        LIFT_CHECK_EQ_U32(m.mem().rd8(kBuf), 16u); // 第一行第 0 像素
        // 行对齐：pitch = 8 ⇒ 最后一行（第 19 行）从 buf[8*19] 开始；该像素覆盖率 186 ⇒ 级 12。
        // 若实现按 width 紧凑排（pitch=7），这里会读到别的行/列 ⇒ 红。
        LIFT_CHECK_EQ_U32(m.mem().rd8(kBuf + 8u * 19u), grayLevel(16, 186));
        LIFT_CHECK_EQ_U32(m.mem().rd8(kBuf + 8u * 19u + 7u), 0u);  // 行尾 padding 是 0
    }

    g_current_case = "extent-advance-self-consistency";
    {
        // 文本 = 'A' + あ(0x82A0) + 'B' ⇒ 3 个字，每个 advance = 6 ⇒ 宽 18；高 = tmHeight = 22
        LIFT_CHECK_EQ_I64(m.mem().rd32(kSize + 0), 18);
        LIFT_CHECK_EQ_I64(m.mem().rd32(kSize + 4), 22);
        lift::GdiDcObject* dc = lift::gdiAsDc(m.handles(), hdc);
        LIFT_CHECK(dc != nullptr);
        if (dc) {
            // 逐字走**同一条**取字路径求 advance 之和，再与 extent 的宽对比
            std::int32_t sum = 0;
            for (const char32_t cp : lift::gdiDecodeCp932(std::string_view(text, 4))) {
                lift::GlyphMask mask;
                LIFT_CHECK(lift::gdiGlyph(m.fonts(), lift::gdiEffectiveFont(dc->state), cp, &mask));
                sum += mask.advance;
            }
            LIFT_CHECK_EQ_I64(sum, 18);
            // ★ 用**字体对象**的描述（DC 已恢复成 stock 字体，那个缺省字号不是 -20）
            const lift::GdiFontObject* font = lift::gdiAsFont(m.handles(), hfont);
            LIFT_CHECK(font != nullptr);
            if (!font) return finish("gdi");
            std::int32_t width = 0;
            std::int32_t height = 0;
            LIFT_CHECK(lift::gdiTextExtent(m.fonts(), font->desc, std::string_view(text, 4), &width,
                                           &height));
            LIFT_CHECK_EQ_I64(width, sum);  // ③ extent == Σ gmCellIncX
            LIFT_CHECK_EQ_I64(height, 22);
        }
    }

    // ---- 程序 2：DeleteObject 释放 / 重复删除 / DeleteDC / DeleteObject(DC) / GetStockObject ----
    {
        Asm b(kCode2);
        b.pushMem(kFont);
        b.callRel(stubOf(m, "DeleteObject"));
        b.storeEax(kDelFree);
        b.pushMem(kFont);
        b.callRel(stubOf(m, "DeleteObject"));
        b.storeEax(kDelAgain);
        b.pushMem(kHdc);
        b.callRel(stubOf(m, "DeleteDC"));
        b.storeEax(kDelDc);
        b.pushMem(kHdc);
        b.callRel(stubOf(m, "DeleteObject"));
        b.storeEax(kDelDcObject);
        b.pushImm(4);  // BLACK_BRUSH
        b.callRel(stubOf(m, "GetStockObject"));
        b.storeEax(kStockBrush);
        b.ret();
        LIFT_CHECK(m.placeCode(kCode2, b.b.data(), b.b.size(), &err));
        reprimeStack(m);
        g_current_case = "run-2";
        LIFT_CHECK(run(m, kCode2).reason == lift::StopReason::UntilEa);
        g_current_case = "delete-object-dc";
        LIFT_CHECK_EQ_U32(m.mem().rd32(kDelFree), 1u);  // 选回旧对象后可以删
        LIFT_CHECK(lift::gdiAsFont(m.handles(), hfont) == nullptr);
        LIFT_CHECK_EQ_U32(m.mem().rd32(kDelAgain), 0u);        // 重复删除 ⇒ FALSE
        LIFT_CHECK_EQ_U32(m.mem().rd32(kDelDc), 1u);           // DeleteDC 成功
        LIFT_CHECK(lift::gdiAsDc(m.handles(), hdc) == nullptr);
        LIFT_CHECK_EQ_U32(m.mem().rd32(kDelDcObject), 0u);     // DeleteObject(DC) ⇒ FALSE
        LIFT_CHECK_EQ_U32(m.mem().rd32(kStockBrush), 0x00013004u);  // P0 的令牌值不变
    }

    // =====================================================================================
    // ② 表面 DC：真的写进 guest surface 的像素（裁剪 / 文字色 / bk mode / 确定性）
    // =====================================================================================
    {
        g_current_case = "surface-dc-attach";
        lift::SurfaceFormat fmt;
        fmt.bpp = 32;
        fmt.rmask = 0x00FF0000u;
        fmt.gmask = 0x0000FF00u;
        fmt.bmask = 0x000000FFu;
        fmt.amask = 0xFF000000u;
        const std::uint32_t surface_ea = m.surfaces().create(static_cast<std::int32_t>(kSurfaceW),
                                                             static_cast<std::int32_t>(kSurfaceH), fmt);
        LIFT_CHECK_NE_U32(surface_ea, 0u);
        lift::GuestSurface* surface = m.surfaces().find(surface_ea);
        LIFT_CHECK(surface != nullptr);
        const std::uint32_t hdc2 = lift::gdiCreateDc(m.handles(), /*display=*/false, "SURFACE");
        LIFT_CHECK_NE_U32(hdc2, 0u);
        LIFT_CHECK(lift::fontAttachDcSurface(hdc2, surface_ea, &m.surfaces()));
        lift::GdiDcObject* dc = lift::gdiAsDc(m.handles(), hdc2);
        LIFT_CHECK(dc != nullptr);
        if (!dc || !surface) return finish("gdi");

        lift::GdiFontDesc desc;
        desc.height = -20;
        desc.weight = 400;
        dc->state.font = desc;
        dc->state.text_color = 0x000000FFu;  // COLORREF：红
        dc->state.bk_mode = lift::kTransparent;

        const std::string_view sv(text, 4);
        const lift::TextDrawResult r1 = lift::gdiDrawText(m.fonts(), desc, dc->state, 0, 20, sv);
        LIFT_CHECK(r1.ok);
        LIFT_CHECK_EQ_I64(r1.advance, 18);  // 与 extent 同一个推进路径
        LIFT_CHECK_EQ_U32(r1.glyphs, 3u);
        LIFT_CHECK(r1.pixels > 0u);

        std::uint8_t* pixels = m.surfaces().pixels(*surface);
        LIFT_CHECK(pixels != nullptr);
        const std::size_t bytes =
            static_cast<std::size_t>(surface->pitch) * static_cast<std::size_t>(surface->height);
        std::vector<std::uint8_t> first(pixels, pixels + bytes);
        bool saw_red = false;
        for (std::size_t i = 0; i + 3 < bytes; i += 4) {
            if (pixels[i + 2] != 0) {  // 红通道（rmask 0x00FF0000）
                saw_red = true;
                break;
            }
        }
        LIFT_CHECK(saw_red);  // 文字色真的写进去了
        // 透明 bk mode 不清背景：(60,0) 在最后一段文本之外，仍是 0
        LIFT_CHECK_EQ_U32(m.mem().rd8(surface_ea + 0u * surface->pitch + 60u * 4u + 2u), 0u);

        // 确定性：清空后同一段文本再画一次 ⇒ 逐字节相同
        std::memset(pixels, 0, bytes);
        const lift::TextDrawResult r2 = lift::gdiDrawText(m.fonts(), desc, dc->state, 0, 20, sv);
        LIFT_CHECK(r2.ok);
        const std::vector<std::uint8_t> second(pixels, pixels + bytes);
        LIFT_CHECK(first == second);

        // 裁剪：在右下角画 ⇒ 不越界；完全在视口外 ⇒ 一个像素都不写
        std::memset(pixels, 0, bytes);
        const lift::TextDrawResult r3 = lift::gdiDrawText(
            m.fonts(), desc, dc->state, static_cast<int>(kSurfaceW) - 2,
            static_cast<int>(kSurfaceH) - 1, sv);
        LIFT_CHECK(r3.ok);
        LIFT_CHECK(r3.pixels > 0u);
        const lift::TextDrawResult r4 = lift::gdiDrawText(m.fonts(), desc, dc->state, -100, -100, sv);
        LIFT_CHECK(r4.ok);
        LIFT_CHECK_EQ_U32(r4.pixels, 0u);

        // OPAQUE：先铺背景色（bk_color = 蓝），字形格内、字形外的像素应是纯背景
        std::memset(pixels, 0, bytes);
        dc->state.bk_mode = lift::kOpaque;
        dc->state.bk_color = 0x00FF0000u;  // COLORREF：蓝
        const lift::TextDrawResult r5 = lift::gdiDrawText(m.fonts(), desc, dc->state, 0, 20, sv);
        LIFT_CHECK(r5.ok);
        // 'A' 的格 = x 0..5、y 20..41（被表面裁到 31）；origin.x = 1 ⇒ x=0 在字形黑框外
        // ⇒ 屏幕坐标 (0, 30) 只有背景色。
        LIFT_CHECK_EQ_U32(m.mem().rd8(surface_ea + 30u * surface->pitch + 0u * 4u + 0u), 0xFFu);
        LIFT_CHECK_EQ_U32(m.mem().rd8(surface_ea + 30u * surface->pitch + 0u * 4u + 2u), 0u);

        LIFT_CHECK(lift::fontDetachDc(hdc2));
        LIFT_CHECK(!lift::fontDetachDc(0x00DEAD00u));  // 不是 DC：false + 一行日志（不崩）
    }

    return finish("gdi");
}
