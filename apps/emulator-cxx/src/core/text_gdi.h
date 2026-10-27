#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace core {

// ★ 引擎的**真机字形来源**：Windows GDI 的 `GetGlyphOutline*`。
//
// 真机不是"用某个字体库栅格化"，而是把字形请求交给**平台**：
//   · `0x471723` / `0x46F5BC` = 取字形位图（长度 / 本体两次平台请求）；
//   · `fuFormat` 由 `Font+0x548`（AA 开关）决定：`GGO_BITMAP` / `GGO_GRAY4` / `GGO_GRAY8`
//     （`0x471259..0x47127B`；级数 `Font+0x35584`，`0x471260`）；
//   · 字体由 LOGFONT 描述（字面名 / 高度 −字号 / 粗细），`0x465390`/`0x459F40` 那族建句柄。
// ⇒ 这一档**没有"宿主近似"**：字型、hinting、栅格化、灰度级数全来自平台，与引擎同源。
//   它替换的是"无 bake 数据时的 FT 回退"，不是引擎语义的一部分（引擎语义仍是"向平台要字形"）。
//
// ★ 只在 `_WIN32` 有真实实现；其它平台这些函数一律返回 false（调用方回退到 FreeType）。

enum class GdiFormat {
    Mono = 0,   // GGO_BITMAP（引擎默认；`Font+0x548 == 0`）
    Gray4 = 1,  // GGO_GRAY4_BITMAP（17 级）
    Gray8 = 2,  // GGO_GRAY8_BITMAP（65 级，`message:AntiFontLevel == 1`）
};

struct GdiGlyph {
    int width = 0;
    int height = 0;
    int pitch = 0;      // 行距（字节）：单色按 DWORD 对齐，灰度按 4 字节对齐（与 GDI 的返回布局一致）
    int left = 0;       // 相对基线的横向原点（`GLYPHMETRICS.gmptGlyphOrigin.x`）
    int top = 0;        // 字形顶相对基线（`gmptGlyphOrigin.y`，正 = 基线上方）
    int advance = 0;    // `gmCellIncX`
    int max_value = 0;  // 灰度档里实际出现过的最大原始值（诊断口径：GDI 的 GRAY8 是 0..64 还是 0..255）
    std::vector<std::uint8_t> pixels;
};

// 平台能不能走这条路（`_WIN32` 且能建内存 DC）。不等于"这个字面名一定存在"。
bool gdi_glyph_available();

// 字面名的两种来源（**解码在平台层做**，核心只传字节/UTF-8）：
//   · 引擎给的是 CP932（`0x1a5` 写的那一格）；
//   · 宿主给的是 UTF-8（`EMULATOR_CXX_GLYPH_GDI_FACE` / 宿主字体族名）。
// 非 Windows 上两个都返回空串（调用方反正会回退）。
std::wstring gdi_face_from_sjis(const std::string& cp932);
std::wstring gdi_face_from_utf8(const std::string& utf8);

// 该（字面名 / 字号 / 粗细）的 tmAscent —— 引擎的"本次量度的 ascent"就是这一格
// （`GetTextMetricsA` 的出参，见 ops-forensics §28）。
bool gdi_font_ascent(const std::wstring& face, int size, int weight, int* ascent);

// 取一个码点的字形。`codepoint` 是 SJIS 解码后的 Unicode 码点：引擎在日文代码页下把 SJIS 码交给
// ANSI 版 API，这里用 W 版 + 同一个字符，避开宿主代码页（**不是**语义差异）。
bool gdi_load_glyph(const std::wstring& face, int size, int weight, GdiFormat format,
                    std::uint32_t codepoint, GdiGlyph* out);

}  // namespace core
