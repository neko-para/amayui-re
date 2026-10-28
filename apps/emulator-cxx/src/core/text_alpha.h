// 字形位深 / 灰度级 → alpha：**引擎侧的算术**，纯函数（无平台依赖 ⇒ 可纯测）。
//
// ★ 为什么单独一个头：这段算术决定了"文字看起来多亮"。引擎把平台的灰度字形
//   （`GetGlyphOutline` 的 `GGO_GRAY4_BITMAP` / `GGO_GRAY8_BITMAP`）换算成 alpha 时用的是
//   **17 / 65 级**（`0x46DAAF` / `0x46DA9C`），不是 2^n：
//     GRAY4：原始值 0..16 ⇒ alpha = 值 × 255 / 17 ∈ {0,15,…,240}
//     GRAY8：原始值 0..64 ⇒ alpha = 值 × 255 / 65 ∈ {0,…,251}
//   ⇒ 满覆盖的 alpha 是 **240 / 251**，不是 255。按 2^n（16 / 64）换算会得到 255 ——
//   文字比真机**亮一档**，这正是"设置界面文字纯白"那一类观测的一半来源（另一半是配置根读错、
//   AA 开关根本没打开）。⛔ 不要把它"修正"成 2 的幂。
#pragma once

#include <cstdint>

namespace core {

// 请求的字形格式（`CORE-OPS-RENDER-TEXT-204`）：总开关与子开关**都**非 0 才开；
// 级数 == 1 ⇒ 8 位，其余 ⇒ 4 位。任一开关为 0 / 缺键 ⇒ 1 位掩码。
enum class GlyphFormat { Mono, Gray4, Gray8 };

// 级数（含 0）：GRAY4 = 17 级、GRAY8 = 65 级。
constexpr int kGray4Levels = 17;
constexpr int kGray8Levels = 65;

constexpr GlyphFormat glyph_format(bool enable_antifont, bool use_antifont, int anti_font_level) {
    if (!enable_antifont || !use_antifont) return GlyphFormat::Mono;
    return anti_font_level == 1 ? GlyphFormat::Gray8 : GlyphFormat::Gray4;
}

// 平台直接给的原始级数 → alpha（引擎同式）。
constexpr std::uint8_t level_to_alpha(int level, int levels) {
    if (level <= 0 || levels <= 0) return 0;
    if (level > levels - 1) level = levels - 1;
    const int alpha = level * 255 / levels;
    return static_cast<std::uint8_t>(alpha > 255 ? 255 : alpha);
}

// 覆盖率 0..255（宿主栅格化给的连续值）→ 先量化到 `levels` 级 → 再按引擎换算。
// ★ 四舍五入取级：`(覆盖率 × (级数−1) + 127) / 255` —— 满覆盖 ⇒ 最高级 ⇒ 240 / 251。
constexpr std::uint8_t coverage_to_alpha(int coverage, int levels) {
    if (coverage <= 0 || levels <= 1) return 0;
    if (coverage > 255) coverage = 255;
    const int level = (coverage * (levels - 1) + 127) / 255;
    return level_to_alpha(level, levels);
}

}  // namespace core
