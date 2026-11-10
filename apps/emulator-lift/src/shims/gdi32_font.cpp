// emulator-lift · GDI32 字体 / 度量 / 字形 / 文本（P2）
//
// ★ 口径（plan §3 F4 / §5 B3 / §6 P2；`apps/emulator-cxx/SCOPE.md` §0 第 1 条）：
//   · 字形**一律**来自宿主 Amayui CN（`CreateFontIndirectA` 的字面名 / `AddFontMemResourceEx`
//     送进来的内存都不改变来源）；`--font-engine-name` 是显式 A/B 档（本平台只影响上报的族名）。
//   · `GetGlyphOutlineA` 支持 `GGO_METRICS(0)` / `GGO_BITMAP(1)` / `GGO_GRAY4(5)` / `GGO_GRAY8(6)`；
//     其它格式**如实拒绝**（`GDI_ERROR` + 一行日志），⛔ 不猜布局。
//   · `lpmat2` 只接受单位矩阵（16.16 的 eM11 = eM22 = 1、其余 0）；其它如实拒绝。
//   · `cbBuffer == 0` ⇒ 返回**所需字节数**（行按 DWORD 对齐），同时照常写 GLYPHMETRICS。
//   · **自洽**：`GetTextExtentPoint32A` 的宽 == Σ `GetGlyphOutlineA` 的 gmCellIncX == `TextOutA`
//     的推进 —— 三者都走 `font.h` 的 `gdiGlyph()` 这一条路径。
//   · `EnumFontFamiliesExA` 把**引擎可能查到的字面名**逐条报出（第一条是宿主族名）：
//     引擎 boot 时用它建**字体名表**，运行期按自己配置里的字面名（`ＭＳ ゴシック` 等）查表，
//     查不到就 `std::out_of_range` ⇒ 崩。名字只是**标签**，字形仍一律来自 Amayui CN。
//   · 字体文件缺失 / FreeType 起不来 ⇒ 这些 API **返回失败值 + 一行日志**（⛔ 不画空白冒充成功）。
#include <cstdint>
#include <string>
#include <vector>

#include "runtime/cp932.h"
#include "runtime/font.h"
#include "runtime/guest_call.h"
#include "runtime/x86.h"
#include "shims/register.h"

namespace lift {
namespace {

// LOGFONTA / TEXTMETRICA 的 x86 布局（wingdi.h 真值；A 版与 W 版结构相同，只有 lfFaceName 的元素宽度不同）
constexpr std::uint32_t kLogFontSize = 60;
constexpr std::uint32_t kTextMetricSize = 56;
constexpr int kLogFaceNameOff = 28;
constexpr int kLogFaceNameLen = 32;

GdiDcObject* requireDc(X86& cpu, std::uint32_t hdc, const char* api) {
    if (cpu.handles == nullptr) shimAbort("gdi32", api, "cpu.handles 为空（Machine 没装配）");
    GdiDcObject* dc = gdiAsDc(*cpu.handles, hdc);
    if (dc != nullptr) return dc;
    if (cpu.handles->get(hdc) == nullptr) cpu.handles->invalidHandle(hdc, api);
    shimAbort("gdi32", api, "句柄 0x%08X 不是 GDI DC（对象类型不匹配）", hdc);
}

FontEngine& requireFonts(X86& cpu, const char* api) {
    if (cpu.fonts == nullptr) shimAbort("gdi32", api, "cpu.fonts 为空（Machine 没装配字体层）");
    return *cpu.fonts;
}

// `lpString` + `cch` 的 A 版读法：cch < 0 ⇒ 读到 NUL；cch == 0 / NULL ⇒ 空串。
std::string readAnsiText(X86& cpu, ShimCall& call, int ptr_index, int cch_index) {
    const std::uint32_t ptr = call.u32(ptr_index);
    const std::int32_t cch = call.i32(cch_index);
    if (ptr == 0) return {};
    if (cch < 0) return cpu.mem->readCString(ptr, 4096);
    if (cch == 0) return {};
    const std::vector<std::uint8_t> bytes = call.bytes(ptr_index, static_cast<std::size_t>(cch));
    return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

void writeTextMetricA(GuestMemory& mem, std::uint32_t ea, const HostTextMetrics& tm) {
    mem.wr32(ea + 0, static_cast<std::uint32_t>(tm.height));
    mem.wr32(ea + 4, static_cast<std::uint32_t>(tm.ascent));
    mem.wr32(ea + 8, static_cast<std::uint32_t>(tm.descent));
    mem.wr32(ea + 12, static_cast<std::uint32_t>(tm.internal_leading));
    mem.wr32(ea + 16, static_cast<std::uint32_t>(tm.external_leading));
    mem.wr32(ea + 20, static_cast<std::uint32_t>(tm.ave_char_width));
    mem.wr32(ea + 24, static_cast<std::uint32_t>(tm.max_char_width));
    mem.wr32(ea + 28, static_cast<std::uint32_t>(tm.weight));
    mem.wr32(ea + 32, static_cast<std::uint32_t>(tm.overhang));
    mem.wr32(ea + 36, 96u);  // tmDigitizedAspectX
    mem.wr32(ea + 40, 96u);  // tmDigitizedAspectY
    mem.wr8(ea + 44, 0x20);  // tmFirstChar
    mem.wr8(ea + 45, 0xFF);  // tmLastChar
    mem.wr8(ea + 46, '?');   // tmDefaultChar
    mem.wr8(ea + 47, ' ');   // tmBreakChar
    mem.wr8(ea + 48, tm.italic);
    mem.wr8(ea + 49, tm.underlined);
    mem.wr8(ea + 50, tm.struck_out);
    // tmPitchAndFamily：低 4 位是 family（FF_DONTCARE=0）；TMPF_TRUETYPE(0x40) + 变宽字面(0x01)。
    mem.wr8(ea + 51, 0x41);
    mem.wr8(ea + 52, 0);  // tmCharSet = DEFAULT_CHARSET
    mem.wr8(ea + 53, 0);
    mem.wr8(ea + 54, 0);
    mem.wr8(ea + 55, 0);
}

void writeGlyphMetrics(GuestMemory& mem, std::uint32_t ea, const GlyphMask& mask) {
    mem.wr32(ea + 0, static_cast<std::uint32_t>(mask.black_box_x));  // gmBlackBoxX
    mem.wr32(ea + 4, static_cast<std::uint32_t>(mask.black_box_y));  // gmBlackBoxY
    mem.wr32(ea + 8, static_cast<std::uint32_t>(mask.origin_x));     // gmptGlyphOrigin.x（有符号）
    mem.wr32(ea + 12, static_cast<std::uint32_t>(mask.origin_y));    // gmptGlyphOrigin.y
    mem.wr16(ea + 16, static_cast<std::uint16_t>(static_cast<std::int16_t>(mask.advance)));  // gmCellIncX
    mem.wr16(ea + 18, 0);                                                                    // gmCellIncY
}

// 上报的族名：默认宿主族名；`--font-engine-name` 的 A/B 档才用引擎字面名（本平台只影响上报）。
std::string reportedFamilyName(const GdiFontDesc& desc) {
    if (fontConfig().use_engine_name && !desc.face_name.empty()) return desc.face_name;
    return "Amayui CN";
}

// =====================================================================================
// CreateFontIndirectA
// =====================================================================================
void shimCreateFontIndirectA(X86& cpu, ShimCall& call) {
    const std::uint32_t lf = call.u32(0);
    if (lf == 0 || !cpu.mem->committed(lf, kLogFontSize)) {
        shimLog("gdi32", "CreateFontIndirectA", "LOGFONTA 指针无效（0x%08X）⇒ 0", lf);
        call.result = 0;
        return;
    }
    GdiFontDesc desc;
    desc.height = static_cast<std::int32_t>(cpu.mem->rd32(lf + 0));
    desc.width = static_cast<std::int32_t>(cpu.mem->rd32(lf + 4));
    desc.escapement = static_cast<std::int32_t>(cpu.mem->rd32(lf + 8));
    desc.orientation = static_cast<std::int32_t>(cpu.mem->rd32(lf + 12));
    desc.weight = static_cast<std::int32_t>(cpu.mem->rd32(lf + 16));
    desc.italic = cpu.mem->rd8(lf + 20);
    desc.underline = cpu.mem->rd8(lf + 21);
    desc.strike_out = cpu.mem->rd8(lf + 22);
    desc.charset = cpu.mem->rd8(lf + 23);
    desc.out_precision = cpu.mem->rd8(lf + 24);
    desc.clip_precision = cpu.mem->rd8(lf + 25);
    desc.quality = cpu.mem->rd8(lf + 26);
    desc.pitch_and_family = cpu.mem->rd8(lf + 27);
    std::uint8_t face[kLogFaceNameLen] = {};
    cpu.mem->readBytes(lf + kLogFaceNameOff, face, sizeof(face));
    std::size_t face_len = 0;
    while (face_len < sizeof(face) && face[face_len] != 0) ++face_len;
    desc.face_name.assign(reinterpret_cast<const char*>(face), face_len);

    if (cpu.handles == nullptr) shimAbort("gdi32", "CreateFontIndirectA", "cpu.handles 为空");
    const std::uint32_t handle = gdiCreateFont(*cpu.handles, desc);
    if (handle == 0) shimAbort("gdi32", "CreateFontIndirectA", "句柄表分配失败");
    // ★ 宿主替换（cxx §1.1）：字面名**不参与**字形来源。A/B 档只上报引擎名，仍用宿主字体。
    if (fontConfig().use_engine_name && !desc.face_name.empty()) {
        shimLog("gdi32", "CreateFontIndirectA",
                "A/B 对照档：上报字面名 \"%s\"（字形仍来自宿主 Amayui CN —— 本平台没有按名查系统字）",
                desc.face_name.c_str());
    }
    shimLog("gdi32", "CreateFontIndirectA",
            "lfHeight=%d weight=%d face=\"%s\" ⇒ 字体 0x%08X（宿主替换：Amayui CN）", desc.height,
            desc.weight, desc.face_name.c_str(), handle);
    call.result = handle;
}

// =====================================================================================
// GetTextMetricsA
// =====================================================================================
void shimGetTextMetricsA(X86& cpu, ShimCall& call) {
    const std::uint32_t hdc = call.u32(0);
    const std::uint32_t out = call.u32(1);
    GdiDcObject* dc = requireDc(cpu, hdc, "GetTextMetricsA");
    FontEngine& fonts = requireFonts(cpu, "GetTextMetricsA");
    if (out == 0 || !cpu.mem->committed(out, kTextMetricSize)) {
        shimLog("gdi32", "GetTextMetricsA", "LPTEXTMETRICA 指针无效（0x%08X）⇒ 0", out);
        call.result = 0;
        return;
    }
    HostTextMetrics tm;
    if (!gdiTextMetrics(fonts, gdiEffectiveFont(dc->state), &tm)) {
        shimLog("gdi32", "GetTextMetricsA", "没有可用的宿主字体 ⇒ 0");
        call.result = 0;
        return;
    }
    writeTextMetricA(*cpu.mem, out, tm);
    call.result = 1;
}

// =====================================================================================
// GetGlyphOutlineA
// =====================================================================================
bool identityMat2(X86& cpu, std::uint32_t mat, const char* api) {
    if (mat == 0) return true;  // 文档要求非空；NULL 按单位矩阵处理（引擎只传单位矩阵）
    if (!cpu.mem->committed(mat, 16)) {
        shimLog("gdi32", api, "MAT2 指针无效（0x%08X）⇒ 拒绝", mat);
        return false;
    }
    const std::uint32_t m11 = cpu.mem->rd32(mat + 0);
    const std::uint32_t m12 = cpu.mem->rd32(mat + 4);
    const std::uint32_t m21 = cpu.mem->rd32(mat + 8);
    const std::uint32_t m22 = cpu.mem->rd32(mat + 12);
    const bool identity = m11 == 0x00010000u && m12 == 0 && m21 == 0 && m22 == 0x00010000u;
    if (!identity) {
        shimLog("gdi32", api,
                "非单位 MAT2（%08X %08X %08X %08X）⇒ 拒绝（本模拟器不做字形变换）", m11, m12, m21, m22);
    }
    return identity;
}

void shimGetGlyphOutlineA(X86& cpu, ShimCall& call) {
    const std::uint32_t hdc = call.u32(0);
    const std::uint32_t uchar = call.u32(1);
    std::uint32_t format = call.u32(2);
    const std::uint32_t gm_ea = call.u32(3);
    const std::uint32_t cb_buffer = call.u32(4);
    const std::uint32_t buffer = call.u32(5);
    const std::uint32_t mat = call.u32(6);

    GdiDcObject* dc = requireDc(cpu, hdc, "GetGlyphOutlineA");
    FontEngine& fonts = requireFonts(cpu, "GetGlyphOutlineA");
    if (gm_ea == 0 || !cpu.mem->committed(gm_ea, 20)) {
        shimLog("gdi32", "GetGlyphOutlineA", "LPGLYPHMETRICS 指针无效（0x%08X）⇒ GDI_ERROR", gm_ea);
        call.result = kGdiError;
        return;
    }
    if (!identityMat2(cpu, mat, "GetGlyphOutlineA")) {
        call.result = kGdiError;
        return;
    }
    // GGO_UNHINTED 只是 hinting 提示（1px 级栅格差属平台 discard）⇒ 剥掉；其余位不认。
    if ((format & kGgoUnhinted) != 0) format &= ~kGgoUnhinted;
    if (format != kGgoMetrics && format != kGgoBitmap && format != kGgoGray4 && format != kGgoGray8) {
        shimLog("gdi32", "GetGlyphOutlineA",
                "fuFormat=%u 未实现（只有 GGO_METRICS=0 / BITMAP=1 / GRAY4=5 / GRAY8=6）⇒ GDI_ERROR",
                format);
        call.result = kGdiError;
        return;
    }

    const char32_t cp = gdiCp932Char(uchar);
    GlyphMask mask;
    if (!gdiGlyph(fonts, gdiEffectiveFont(dc->state), cp, &mask)) {
        shimLog("gdi32", "GetGlyphOutlineA", "没有可用的宿主字体 ⇒ GDI_ERROR");
        call.result = kGdiError;
        return;
    }
    writeGlyphMetrics(*cpu.mem, gm_ea, mask);
    if (format == kGgoMetrics) {
        call.result = 1;  // 文档：GGO_METRICS 成功时返回非 0（Wine 同值）
        return;
    }

    std::vector<std::uint8_t> bytes;
    std::uint32_t pitch = 0;
    if (!gdiPackGlyph(mask, format, &bytes, &pitch)) {
        shimLog("gdi32", "GetGlyphOutlineA", "字形位图打包失败（黑框 %dx%d）⇒ GDI_ERROR",
                mask.black_box_x, mask.black_box_y);
        call.result = kGdiError;
        return;
    }
    (void)pitch;
    const std::uint32_t needed = static_cast<std::uint32_t>(bytes.size());
    if (cb_buffer == 0) {
        // ★ 判据：cbBuffer == 0 ⇒ 返回**所需字节数**（含 DWORD 对齐的行宽），不写 buffer。
        call.result = needed;
        return;
    }
    if (needed == 0) {
        call.result = 0;
        return;
    }
    if (cb_buffer < needed) {
        shimLog("gdi32", "GetGlyphOutlineA", "缓冲区太小（cb=%u < 需要 %u）⇒ GDI_ERROR", cb_buffer,
                needed);
        call.result = kGdiError;
        return;
    }
    if (buffer == 0 || !cpu.mem->committed(buffer, needed)) {
        shimLog("gdi32", "GetGlyphOutlineA", "lpBuffer 无效（0x%08X，需要 %u 字节）⇒ GDI_ERROR", buffer,
                needed);
        call.result = kGdiError;
        return;
    }
    cpu.mem->writeBytes(buffer, bytes.data(), needed);
    call.result = needed;
}

// =====================================================================================
// GetTextExtentPoint32A / TextOutA
// =====================================================================================
void shimGetTextExtentPoint32A(X86& cpu, ShimCall& call) {
    const std::uint32_t hdc = call.u32(0);
    const std::uint32_t size_ea = call.u32(3);
    GdiDcObject* dc = requireDc(cpu, hdc, "GetTextExtentPoint32A");
    FontEngine& fonts = requireFonts(cpu, "GetTextExtentPoint32A");
    if (size_ea == 0 || !cpu.mem->committed(size_ea, 8)) {
        shimLog("gdi32", "GetTextExtentPoint32A", "LPSIZE 指针无效（0x%08X）⇒ 0", size_ea);
        call.result = 0;
        return;
    }
    const std::string text = readAnsiText(cpu, call, 1, 2);
    std::int32_t width = 0;
    std::int32_t height = 0;
    if (!gdiTextExtent(fonts, gdiEffectiveFont(dc->state), text, &width, &height)) {
        shimLog("gdi32", "GetTextExtentPoint32A", "没有可用的宿主字体 ⇒ 0");
        call.result = 0;
        return;
    }
    cpu.mem->wr32(size_ea + 0, static_cast<std::uint32_t>(width));
    cpu.mem->wr32(size_ea + 4, static_cast<std::uint32_t>(height));
    call.result = 1;
}

void shimTextOutA(X86& cpu, ShimCall& call) {
    const std::uint32_t hdc = call.u32(0);
    const int x = call.i32(1);
    const int y = call.i32(2);
    GdiDcObject* dc = requireDc(cpu, hdc, "TextOutA");
    FontEngine& fonts = requireFonts(cpu, "TextOutA");
    const std::string text = readAnsiText(cpu, call, 3, 4);
    const TextDrawResult result =
        gdiDrawText(fonts, gdiEffectiveFont(dc->state), dc->state, x, y, text);
    if (result.surface_missing) {
        // 表面 DC 登记了 surface_ea，但 arena 里没有这块表面 ⇒ P1 的拼装错误（⛔ 不静默跳过）
        shimAbort("gdi32", "TextOutA",
                  "DC 0x%08X 绑定的表面 %08X 在 arena 里不存在（P1 的 GetDC/ReleaseDC 拼装问题）", hdc,
                  dc->state.surface_ea);
    }
    dc->state.text_calls += 1;
    dc->state.glyphs_drawn += result.glyphs;
    dc->state.pixels_written += result.pixels;
    if (!result.ok) {
        shimLog("gdi32", "TextOutA", "没有可用的宿主字体 ⇒ 0（字符数 %zu）", text.size());
        call.result = 0;
        return;
    }
    call.result = 1;
}

// =====================================================================================
// EnumFontFamiliesExA（按族名列表**逐条**回调；回调返回 0 = 停止、非 0 = 继续）
// =====================================================================================
// ★ 为什么是**多条**（2026-11-10 实测 + 反汇编）：这个 API 是引擎**字体名表**
//   （`this+0x313C0..0x313C4`，元素 32 字节，`sub_428990` @ 0x428990 线性按名查）的唯一填充点。
//   boot 时引擎用 `EnumFontFamiliesExA(hdc, {lfCharSet=0x80}, Proc, this+0x313C0, 0)` 让回调
//   `Proc`（EA 0x45EAC0）把每条族名 append 进表；运行期 `sub_4581F0`（EA 0x4581F0）按**引擎自己
//   配置里的字面名**查表（`sub_428990` 找不到返回 **-1**），再拿返回值当下标 ⇒
//   `0x4586A6 cmp edx,eax` + `0x4586A8 ja` 不跳 ⇒ `0x4586AA push aInvalidVectorT` +
//   `0x4586AF call ?_Xout_of_range@std` ⇒ `std::out_of_range` ⇒ 无人接 ⇒ CRT 的 `int3`。
//   ⇒ 只报一条宿主族名时，引擎查 `ＭＳ ゴシック`（EA 0x52686C）必然 -1：
//     TITLE→OPTION→左侧「ADV设定」（0x4586AA）与 TITLE→GameStart 两条路径都停在同一处。
// ★ 因此这里把**引擎可能查到的字面名**都报出来（`kEnumFamilyNames`，第一条 = 宿主族名）。
//   ⛔ 字形来源不变：一律 Amayui CN（名字只是**标签**；见文件头与 `SCOPE.md` §5）。
// ★ 每条 LOGFONT 除 `lfFaceName` 外完全相同；`lfCharSet` **回显请求的 charset**
//   （`Proc` 的过滤条件：`0x45EB02 cmp byte ptr [esi+17h], 80h` ⇒ 不是 0x80 就整条跳过）。
// ★ `@` 开头的竖排族名**不报**：`Proc` 在 `0x45EB4A` 调 `sub_457B90`（= 名字里第一个 '@' 的
//   下标，无 '@' 返回 -1），`0x45EB4F test eax,eax / jz` ⇒ 下标 0 是 '@' 的条目被跳过；
//   查表侧 `sub_428990` 又用 `&a2[*a2 == 64]` 剥掉查询名开头的一个 '@' ⇒ 报竖排名没有收益。
// ★ 机制：`GuestCallStack` 的"续延里再 `start()` 并返回 false"（与 DDraw 的 `EnumDisplayModes`
//   同形）⇒ 多回调是 LIFO 栈，⛔ 不是"一次 start 里循环"。`lf_ea` / `tm_ea` **只分配一次、
//   全部回调结束后释放一次**。
// ★ `--font-engine-name`（`FontConfig::use_engine_name`）的 A/B 档保持**旧语义**：只报
//   `reportedFamilyName(desc)` 那一条（日志里会明说这是 A/B 档）。

// 枚举出来的族名（顺序固定；**第一条 = 宿主族名**）。UTF-8 字面量（guest 侧按 CP932 写出）+ 逐条来路。
constexpr const char* kEnumFamilyNames[] = {
    "Amayui CN",       // 宿主族名（`corpus/assets/fonts/Amayui-CN_cnjp.ttf`；字形真的来自这里）
    "ＭＳ ゴシック",   // EA 0x52686C（`asc_52686C`）—— 引擎自己的缺省字面名
    "ＭＳ 明朝",       // 引擎配置里的第 4 个字体（父级 2026-11-10 OPTION 画面观察；exe 里没有这个字面量）
    "AGE Extend",      // EA 0x5208D8（`aAgeExtend`；`@AGE Extend` @0x5268F8 是竖排变体，不报）
                       //   —— 引擎经 `AddFontMemResourceEx` 送进来的字体（我们忽略内存，只报名字）
    "IPA Pゴシック",   // EA 0x5268CC
    "IPA P明朝",       // EA 0x5268C0
    "MS PMincho",      // EA 0x52985C（`FontLink\SystemLink` 的已知族名表，`sub_491060` 用来过滤）
    "MS Mincho",       // EA 0x529868
    "MS UI Gothic",    // EA 0x529874
    "MS PGothic MC",   // EA 0x529884
    "MS PGothic",      // EA 0x529894
    "MS Gothic",       // EA 0x5298A0
    "Meiryo UI Bold",  // EA 0x5298AC
    "Meiryo UI",       // EA 0x5298BC
    "Meiryo Bold",     // EA 0x5298C8
    "Meiryo",          // EA 0x5298D4
};
constexpr std::size_t kEnumFamilyCount = sizeof(kEnumFamilyNames) / sizeof(kEnumFamilyNames[0]);
// ★ 名单**不是**"越多越好"：`sub_45F6C0`（0x45F6C0，表的填充者）在枚举之后还会按**引擎配置里的
//   字体列表**把同名项从表里**删掉**（0x45F7A0-0x45F933 的缩表分支）⇒ 名单里某条若恰好也在那份配置
//   列表里，加了也会被删。所以"砍名单"前必须走**端到端**（`tests/title_entry_paths.cpp` 的 A/B），
//   ⛔ 不能只看单点 CTest。末尾那 10 条 `.data:0x52985C` 族名目前**没有查表证据**（只被 `sub_491060`
//   用来过滤 `FontLink\SystemLink` 的值名），保留它们是为了"用户把配置改成这些名字也不崩"。

// EnumFontFamiliesExA 的续延状态（多回调：LIFO 帧栈上共享这一份）
struct EnumFontCtx {
    std::uint32_t lf_ea = 0;         // guest 堆上的 LOGFONTA（全部回调共用）
    std::uint32_t tm_ea = 0;         // guest 堆上的 TEXTMETRICA（全部回调共用）
    std::uint32_t proc = 0;          // guest 回调
    std::uint32_t lparam = 0;
    std::uint32_t cleanup = 0;       // 原 shim 返回时要抬的栈字节（stdcall 的 4 + argc*4）
    std::uint8_t charset = 0;        // 回显的 lfCharSet（引擎 `Proc` 要求 0x80）
    GdiFontDesc desc;                // 值拷贝（⚠ 回调期间 DC 可能被释放 ⇒ ⛔ 不捕获 dc 指针）
    std::vector<std::string> names;  // 每条族名的 **CP932 字节**
    std::size_t index = 0;           // 当前**已经回调过**的下标
};

// 把第 i 条族名写进共享的 guest LOGFONTA（其余字段每次相同 ⇒ 只在收尾前写一次也行，
// 但 `lfFaceName` 必须**整段清零**再写：名字长度不同，不清零会留上一条的尾巴）。
void writeEnumLogFont(X86& cpu, const EnumFontCtx& ctx, std::size_t i) {
    const std::uint32_t lf_ea = ctx.lf_ea;
    const GdiFontDesc& desc = ctx.desc;
    cpu.mem->wr32(lf_ea + 0, static_cast<std::uint32_t>(desc.height));
    cpu.mem->wr32(lf_ea + 4, static_cast<std::uint32_t>(desc.width));
    cpu.mem->wr32(lf_ea + 8, static_cast<std::uint32_t>(desc.escapement));
    cpu.mem->wr32(lf_ea + 12, static_cast<std::uint32_t>(desc.orientation));
    cpu.mem->wr32(lf_ea + 16, static_cast<std::uint32_t>(desc.weight));
    cpu.mem->wr8(lf_ea + 20, desc.italic);
    cpu.mem->wr8(lf_ea + 21, desc.underline);
    cpu.mem->wr8(lf_ea + 22, desc.strike_out);
    cpu.mem->wr8(lf_ea + 23, ctx.charset);  // ★ 回显**请求的** charset（不是 DEFAULT_CHARSET）
    cpu.mem->wr8(lf_ea + 24, desc.out_precision);
    cpu.mem->wr8(lf_ea + 25, desc.clip_precision);
    cpu.mem->wr8(lf_ea + 26, desc.quality);
    cpu.mem->wr8(lf_ea + 27, desc.pitch_and_family);
    for (std::uint32_t k = 0; k < kLogFaceNameLen; ++k) {
        cpu.mem->wr8(lf_ea + kLogFaceNameOff + k, 0);
    }
    // 定长缓冲 ⇒ 用 `cp932Truncate`（不切断一个双字节字符；不留下半个字符）
    const std::string family = cp932Truncate(ctx.names[i], kLogFaceNameLen - 1);
    for (std::size_t k = 0; k < family.size(); ++k) {
        cpu.mem->wr8(lf_ea + kLogFaceNameOff + static_cast<std::uint32_t>(k),
                     static_cast<std::uint8_t>(family[k]));
    }
}

void shimEnumFontFamiliesExA(X86& cpu, ShimCall& call) {
    const std::uint32_t hdc = call.u32(0);
    const std::uint32_t logfont_in = call.u32(1);
    const std::uint32_t proc = call.u32(2);
    const std::uint32_t lparam = call.u32(3);
    GdiDcObject* dc = requireDc(cpu, hdc, "EnumFontFamiliesExA");
    FontEngine& fonts = requireFonts(cpu, "EnumFontFamiliesExA");
    if (proc == 0) {
        shimLog("gdi32", "EnumFontFamiliesExA", "回调为 NULL ⇒ 0");
        call.result = 0;
        return;
    }
    if (cpu.calls == nullptr || cpu.heap == nullptr || !cpu.heap->ready()) {
        shimAbort("gdi32", "EnumFontFamiliesExA", "guest 回调栈 / 堆没装配");
    }
    HostTextMetrics tm;
    if (!gdiTextMetrics(fonts, gdiEffectiveFont(dc->state), &tm)) {
        shimLog("gdi32", "EnumFontFamiliesExA", "没有可用的宿主字体 ⇒ 0");
        call.result = 0;
        return;
    }
    const std::uint32_t lf_ea = cpu.heap->alloc(kLogFontSize, true);
    const std::uint32_t tm_ea = cpu.heap->alloc(kTextMetricSize, true);
    if (lf_ea == 0 || tm_ea == 0) {
        if (lf_ea != 0) cpu.heap->free(lf_ea);
        if (tm_ea != 0) cpu.heap->free(tm_ea);
        shimAbort("gdi32", "EnumFontFamiliesExA", "guest 堆分配 LOGFONT/TEXTMETRIC 失败");
    }

    auto ctx = std::make_shared<EnumFontCtx>();
    ctx->lf_ea = lf_ea;
    ctx->tm_ea = tm_ea;
    ctx->proc = proc;
    ctx->lparam = lparam;
    ctx->cleanup = 4u + call.stdcall_bytes;
    ctx->desc = gdiEffectiveFont(dc->state);
    // ★ 枚举的 LOGFONT 是**过滤器**：引擎按 lfCharSet=SHIFTJIS_CHARSET(0x80) 收集日文字体
    //   （`Proc` 的 `[lf+17h] == 0x80`）。宿主替换后字形恒为 Amayui CN，但 **charset 必须回显
    //   请求的 Shift-JIS**，否则引擎的回调会把每一条都丢掉 ⇒ 字体名表为空。
    ctx->charset = ctx->desc.charset;
    if (logfont_in != 0 && cpu.mem->committed(logfont_in, kLogFontSize)) {
        ctx->charset = cpu.mem->rd8(logfont_in + 23);
    }
    std::string first_family;  // 日志用（UTF-8；⛔ 不把 CP932 字节直接打进 UTF-8 日志）
    if (fontConfig().use_engine_name) {
        // A/B 对照档：**只报一条**（旧语义不变）。
        // ★ `desc.face_name` 存的是 `CreateFontIndirectA` 读到的 **CP932 原始字节**（`gdi32_font.cpp`
        //   的 CreateFontIndirectA 不做转换）⇒ 这里 ⛔ **不能**再过 `utf8ToCp932`（那会把 CP932 当
        //   UTF-8 编一遍，非空字面名会变成乱码）。宿主族名 `Amayui CN` 已是纯 ASCII，两种口径同字节。
        first_family = reportedFamilyName(ctx->desc);
        // 写进 guest 的字节：`face_name` 非空时**已经是 CP932**（见上），否则把宿主族名按 CP932 编。
        ctx->names.push_back(ctx->desc.face_name.empty() ? utf8ToCp932(first_family) : ctx->desc.face_name);
        shimLog("gdi32", "EnumFontFamiliesExA",
                "A/B 对照档（--font-engine-name）：只报引擎字面名一条（字形仍来自宿主 Amayui CN）");
    } else {
        std::size_t bad = 0;
        ctx->names.reserve(kEnumFamilyCount);
        for (const char* name : kEnumFamilyNames) {
            std::size_t one_bad = 0;
            ctx->names.push_back(utf8ToCp932(name, &one_bad));
            bad += one_bad;
        }
        first_family = kEnumFamilyNames[0];
        if (bad != 0) {
            shimLog("gdi32", "EnumFontFamiliesExA",
                    "族名列表里有 %zu 个字符编不成 CP932（按 '?' 写出，⛔ 不静默跳过）", bad);
        }
    }
    writeTextMetricA(*cpu.mem, tm_ea, tm);
    writeEnumLogFont(cpu, *ctx, 0);

    X86* const cp = &cpu;
    // 续延：`result` = 这一次 guest 回调的 EAX。
    // ★★ 极性（2026-11-10 实测 + 文档）：**非 0 = 继续枚举，0 = 停止**
    //   —— MSDN `EnumFontFamExProc`："The return value must be a nonzero value to **continue**
    //   enumeration; to stop enumeration, the return value must be **zero**."
    //   引擎的回调 `Proc`（EA 0x45EAC0）在所有路径上都是 `mov eax,1`
    //   （0x45EAF4 / 0x45EB85 → 0x45EB8A 的公共出口 `retn 10h`）⇒ "永远继续"。
    //   ⛔ 不要按"非 0 = 停止"写：那会让引擎的字体名表永远只有 1 条（实测：枚举在第 1 条后就被
    //   我们自己的 shim 掐断 ⇒ 引擎查 `ＭＳ ゴシック` 仍是 -1 ⇒ 崩溃照旧）。
    //   `EnumFontFamiliesEx` 的返回值 = **回调最后一次的返回值**（同页"Return value"）。
    auto cont = [cp](std::uint32_t result, std::uint32_t* shim_result,
                     const std::shared_ptr<EnumFontCtx>& c) -> bool {
        if (result == 0) {
            shimLog("gdi32", "EnumFontFamiliesExA",
                    "第 %zu 条族名后 guest 回调返回 0 ⇒ 停止枚举（共报出 %zu 条）", c->index + 1,
                    c->index + 1);
            if (cp->heap != nullptr) {
                cp->heap->free(c->lf_ea);
                cp->heap->free(c->tm_ea);
            }
            *shim_result = 0;  // 文档：返回值 = 回调最后一次的返回值（这里就是那个 0）
            return true;
        }
        ++c->index;
        if (c->index >= c->names.size()) {
            shimLog("gdi32", "EnumFontFamiliesExA", "枚举完成：%zu 条族名（回调全部返回非 0 = 继续）",
                    c->names.size());
            if (cp->heap != nullptr) {
                cp->heap->free(c->lf_ea);
                cp->heap->free(c->tm_ea);
            }
            *shim_result = result;  // 文档：返回值 = 回调最后一次的返回值
            return true;
        }
        writeEnumLogFont(*cp, *c, c->index);
        std::string start_err;
        // ⛔ 子帧（nested）不需要自己的续延：`resume()` 把结果交回**本帧**的续延。
        if (!cp->calls->start(*cp, c->proc, {c->lf_ea, c->tm_ea, 4u /* TRUETYPE_FONTTYPE */, c->lparam},
                              c->cleanup, "EnumFontFamiliesExA", nullptr, &start_err)) {
            shimLog("gdi32", "EnumFontFamiliesExA", "铺第 %zu 个回调帧失败：%s ⇒ 停止枚举（返回 0）",
                    c->index + 1, start_err.c_str());
            if (cp->heap != nullptr) {
                cp->heap->free(c->lf_ea);
                cp->heap->free(c->tm_ea);
            }
            *shim_result = 0;
            return true;
        }
        return false;  // 控制权又交给 guest；下一次回调返回后 `resume()` 继续进本续延
    };

    call.take_control = true;  // 控制权交给 guest 回调（续延桩会恢复）
    // 回调签名：int CALLBACK EnumFontFamExProcA(const LOGFONTA*, const TEXTMETRICA*, DWORD, LPARAM)
    std::string err;
    const bool started = cpu.calls->start(
        cpu, proc, {lf_ea, tm_ea, 4u /* TRUETYPE_FONTTYPE */, lparam}, ctx->cleanup, "EnumFontFamiliesExA",
        [cont, ctx](X86&, std::uint32_t r, std::uint32_t* out) { return cont(r, out, ctx); }, &err);
    if (!started) {
        cpu.heap->free(lf_ea);
        cpu.heap->free(tm_ea);
        call.take_control = false;
        shimAbort("gdi32", "EnumFontFamiliesExA", "启动 guest 回调失败：%s", err.c_str());
    }
    shimLog("gdi32", "EnumFontFamiliesExA",
            "回调 %zu 条族名（首条 \"%s\"；charset=0x%02X；回调返回 0 即停止枚举）", ctx->names.size(),
            first_family.c_str(), ctx->charset);
}

// =====================================================================================
// AddFontMemResourceEx / RemoveFontMemResourceEx
// =====================================================================================
void shimAddFontMemResourceEx(X86& cpu, ShimCall& call) {
    const std::uint32_t memory = call.u32(0);
    const std::uint32_t size = call.u32(1);
    const std::uint32_t count_out = call.u32(3);
    if (count_out == 0 || !cpu.mem->committed(count_out, 4)) {
        shimLog("gdi32", "AddFontMemResourceEx", "pNumFonts 指针无效（0x%08X）⇒ 0", count_out);
        call.result = 0;
        return;
    }
    if (memory == 0 || size == 0) {
        shimLog("gdi32", "AddFontMemResourceEx", "内存字体指针/长度为 0 ⇒ 0（Win32 失败值）");
        call.result = 0;
        return;
    }
    if (cpu.handles == nullptr) shimAbort("gdi32", "AddFontMemResourceEx", "cpu.handles 为空");
    // ★ 宿主替换（cxx §1.1）：**不解析**送进来的内存，字体来源恒为 Amayui CN。
    //   真机合同是"成功 ⇒ 返回非 0 + *pNumFonts = 加入的字体数"；这里如实按 1 报（宿主字体已可用），
    //   但**明确记一行**"内存字体被忽略"（SCOPE 登记）。
    cpu.mem->wr32(count_out, 1u);
    const std::uint32_t handle =
        cpu.handles->add(HandleKind::GdiFontMem, "fontmem", nullptr, size);
    shimLog("gdi32", "AddFontMemResourceEx",
            "内存字体（%u 字节 @0x%08X）**被忽略**（宿主替换：Amayui CN）⇒ 句柄 0x%08X, *pNumFonts=1",
            size, memory, handle);
    call.result = handle;
}

void shimRemoveFontMemResourceEx(X86& cpu, ShimCall& call) {
    const std::uint32_t handle = call.u32(0);
    if (cpu.handles == nullptr) shimAbort("gdi32", "RemoveFontMemResourceEx", "cpu.handles 为空");
    HandleEntry* entry = cpu.handles->get(handle);
    if (entry == nullptr || entry->kind != HandleKind::GdiFontMem) {
        shimLog("gdi32", "RemoveFontMemResourceEx", "句柄 0x%08X 不是 AddFontMemResourceEx 的返回值 ⇒ 0",
                handle);
        call.result = 0;
        return;
    }
    releasePayload(*cpu.handles, handle);
    cpu.handles->close(handle);
    shimLog("gdi32", "RemoveFontMemResourceEx", "移除内存字体句柄 0x%08X（字形来源不变：Amayui CN）",
            handle);
    call.result = 1;
}

}  // namespace

void registerGdiFontShims(ShimRegistry& s) {
    // DC / 对象表（gdi32_object.cpp）先登记：CreateICA / SelectObject 等是字体 API 的前置。
    registerGdiObjectShims(s);
    s.add("gdi32", "CreateFontIndirectA", Conv::Stdcall, 1, &shimCreateFontIndirectA);
    s.add("gdi32", "GetTextMetricsA", Conv::Stdcall, 2, &shimGetTextMetricsA);
    s.add("gdi32", "GetGlyphOutlineA", Conv::Stdcall, 7, &shimGetGlyphOutlineA);
    s.add("gdi32", "GetTextExtentPoint32A", Conv::Stdcall, 4, &shimGetTextExtentPoint32A);
    s.add("gdi32", "TextOutA", Conv::Stdcall, 5, &shimTextOutA);
    s.add("gdi32", "EnumFontFamiliesExA", Conv::Stdcall, 5, &shimEnumFontFamiliesExA);
    s.add("gdi32", "AddFontMemResourceEx", Conv::Stdcall, 4, &shimAddFontMemResourceEx);
    s.add("gdi32", "RemoveFontMemResourceEx", Conv::Stdcall, 1, &shimRemoveFontMemResourceEx);
}

}  // namespace lift
