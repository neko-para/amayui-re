// emulator-lift · 缺字形 fallback（`REQ-01M79N1ZV391BCM7MYSKK0GVV0`）
//
// ★ 同一个可执行文件跑两档（**档位由注册位置决定**，见 AGENTS.md §5；先例 = `lift-overlay`）：
//   · `gdi-font-fallback`（**pure**，顶层无条件注册，无参数）：用**接缝替身**守判断与记账；
//   · `gdi-font-fallback-assets`（**assets**，`--assets <安装根>`）：从真游戏安装取 `AGE-EXTEND.TTF`
//     守真字体的一侧。缺安装 / 缺宿主字体 / 索引里没这条 ⇒ `LIFT_SKIP`(77) + 原因（⛔ 不假绿）。
//
// ★ 守的是什么（每一条都会红）：
//   ① `cp932/pua`：A 版 `uChar` 的 `0xF050` 必须解成 **U+E010**（CP932 用户定义区 → PUA），
//      反向也必须是 `F0 50`；⛔ 不许有人把它"顺手"解成 U+2661（♡ 的普通码位）—— 那会改字节语义。
//   ② `fallback/mask`：主 face（`face_slot=0`）对 U+E010 **缺字**（`char_index == 0`）时，
//      `glyph()` 必须把这一码位交给**后备提供者**（`face_slot=1`）并返回**后备的 mask**
//      （修前：主 face 的 `.notdef` 被当成功画出去 / 或根本没有 fallback 概念）。
//   ③ `fallback/cache`：同一 (cp, size, bold) 第二次取字命中缓存，**不重复记账**。
//   ④ `fallback/primary-ok`：主 face 有字形时**永不**问后备（引擎把 `AGE Extend` 硬编码给所有文本，
//      按名字 / 一律走后备都会把中文切回日文字形 —— 那是倒退）。
//   ⑤ `fallback/none`：主 face 与全部后备都缺 ⇒ `glyph()` 返回 **true + `.notdef`**（诚实：GDI 会画
//      方块），**不是 false**；并且 `notdef` 计数 +1（⛔ 不静默当成功字形）。
//   ⑥ `fallback/reinstall-clears-cache`：换掉后备来源之后必须重新解析（旧 `.notdef` 不能继续命中）。
//   ⑦ assets 一侧：`AGE-EXTEND.TTF` 的族名 = `AGE Extend`、`FT_Get_Char_Index(face, U+E010) != 0` 且
//      `FT_Load_Char` 位图非空；宿主 Amayui CN 下 `glyph(0xE010)` = `.notdef`（notdef +1）；
//      登记成后备之后同一码位由 `AGE Extend` 提供（fallback +1、mask 有墨迹、advance>0）；
//      移除后备 ⇒ 回到 `.notdef`（notdef +1）。
//   ⑧ assets 一侧（**经 shim**，缺陷的另一半）：guest 调用形状调 `AddFontMemResourceEx`
//      （`src/shims/gdi32_font.cpp`）⇒ 句柄非 0 + `*pNumFonts==1` + 引擎 `memoryFontCount()==1`；
//      `glyph(0xE010)` 的 mask 与 ⑦ 直接登记那条**逐项一致**（⛔ 不是 `.notdef`）；
//      `RemoveFontMemResourceEx` ⇒ 计数回 0、`glyph(0xE010)` 回 `.notdef`。
//      ★ 为什么非要这一条：⑦ 走的是 `addMemoryFont`，pure 档走的是接缝替身，**两者都绕过 shim**
//        ⇒ shim 一旦退回"只记日志 / 传空 bytes"，全部 CTest 仍绿（原缺陷形态复发不可见）。
//   ⑨ `--profile` 的 `font!glyph-fallback fallback=… notdef=… memory-fonts=…` 必须与存取器一致
//      （先例 `tests/gdi_font_metrics_cache.cpp` 的 `font!TEXTMETRIC-cache`）。
//
// ★ 为什么 pure 档能在没有字体文件时跑：`FontEngine::setGlyphSourceForTest` 只替换 **FT 的取字原语**
//   （`char_index` = `FT_Get_Char_Index`、`load` = `FT_Load_Char` + 位图拷贝）；缺字判据 / 后备顺序 /
//   缓存键 / 记账全部走生产路径（`FontEngine::glyph`）。替身的 `load` 在缺字时**也返回 true 并给出
//   `.notdef` 位图** —— 与 FreeType 的真实语义一致，这正是"load 成功"不能当"有字形"的原因。
//
// ★ 变异自检（`tools/mutate-check.mjs`）：
//   · `ctest:gdi-font-fallback`：把 ② 的判据从 `glyphIndexFor(...) != 0` 改回 `loadGlyphFor(...)`
//     （= `FT_Load_Char` 的返回值）⇒ `fallback/mask` 红（主 face 的 `.notdef` 被当成有字形）。
//   · `ctest:gdi-font-fallback-assets`：把 `shimAddFontMemResourceEx` 的"拷贝 + `addMemoryFont`"
//     改成"只记日志 + 假句柄"（= 原缺陷的丢字节形态）⇒ `shim/add-font-mem` 红
//     （`memoryFontCount()` 仍是 0，且 U+E010 回 `.notdef`）。
//
// ★ ALF 索引格式（**测试侧最小读者**，只读、不写；格式真源是 `packages/age-format/src/alf.mts`
//   的文件头：S4 头 300 B、`S4SECTHDR` 12 B + LZSS 目录区、目录 = archive_count / 每个 256 B 归档名 /
//   file_count / 每个 80 B 条目）。LZSS 解压**复用生产实现** `lift::lzssDecode`（不另写一份算法）。
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <ft2build.h>
#include FT_FREETYPE_H

#include "check.h"
#include "runtime/cp932.h"
#include "runtime/font.h"
#include "runtime/machine.h"
#include "runtime/native_overrides.h"
#include "shims/register.h"

namespace fs = std::filesystem;

namespace {

// ---------------------------------------------------------------------------------------
// 一、pure 档的接缝替身
// ---------------------------------------------------------------------------------------
// 主 face 的 `.notdef` 签名（FreeType 对缺字画出来的那块）：1×1、advance=3。
constexpr int kNotdefAdvance = 3;
constexpr int kNotdefBox = 1;
constexpr std::uint8_t kNotdefInk = 255;

// 后备字体（`AGE Extend` 槽）的签名：9×7、advance=11、第一格 200 —— 与 `.notdef` 可区分。
constexpr int kFallbackAdvance = 11;
constexpr int kFallbackBoxX = 9;
constexpr int kFallbackBoxY = 7;
constexpr std::uint8_t kFallbackInk = 200;

// 主 face 真有的字形（`fallback/primary-ok`）：5×6、advance=7。
constexpr int kPrimaryAdvance = 7;
constexpr int kPrimaryBoxX = 5;
constexpr int kPrimaryBoxY = 6;

struct FakeSource {
    // 主 face 只有 `primary_cp` 有字形；其余码位都缺（缺字时 `load` 仍成功并给 `.notdef`）。
    char32_t primary_cp = 0x41;
    bool fallback_has_glyph = true;  // 后备提供者有没有 U+E010
    int fallback_queries = 0;        // 后备被问了几次（`fallback/primary-ok` 要证明是 0）
};

void fillMask(int w, int h, int advance, std::uint8_t ink, lift::GlyphMask* out) {
    out->black_box_x = w;
    out->black_box_y = h;
    out->origin_x = 0;
    out->origin_y = h;
    out->advance = advance;
    out->coverage.assign(static_cast<std::size_t>(w) * static_cast<std::size_t>(h), 0);
    out->coverage[0] = ink;
}

std::uint32_t fakeCharIndex(void* user, int face_slot, char32_t cp) {
    FakeSource* fake = static_cast<FakeSource*>(user);
    if (face_slot == 0) return cp == fake->primary_cp ? 1u : 0u;
    ++fake->fallback_queries;
    return (fake->fallback_has_glyph && cp == 0xE010u) ? 1u : 0u;
}

bool fakeLoad(void* user, int face_slot, char32_t cp, int pixel_size, bool /*bold*/,
              lift::GlyphMask* out) {
    FakeSource* fake = static_cast<FakeSource*>(user);
    if (face_slot == 0) {
        if (cp == fake->primary_cp) {
            fillMask(kPrimaryBoxX, kPrimaryBoxY, kPrimaryAdvance, 77, out);
        } else {
            // ★ 与 FreeType 一致：缺字时 load **成功**，给出 `.notdef`。判据不能看这个返回值。
            fillMask(kNotdefBox, kNotdefBox, kNotdefAdvance, kNotdefInk, out);
        }
        return true;
    }
    (void)cp;
    (void)pixel_size;
    fillMask(kFallbackBoxX, kFallbackBoxY, kFallbackAdvance, kFallbackInk, out);
    return true;
}

lift::FontEngine::GlyphSourceForTest sourceFor(FakeSource* fake, int fallback_count) {
    lift::FontEngine::GlyphSourceForTest src;
    src.face_count = 1 + fallback_count;
    src.char_index = &fakeCharIndex;
    src.load = &fakeLoad;
    src.user = fake;
    return src;
}

bool hasInk(const lift::GlyphMask& mask) {
    for (const std::uint8_t v : mask.coverage) {
        if (v != 0) return true;
    }
    return false;
}

int runPure() {
    using namespace lift_test;

    // ---- ① 码位：A 版 uChar 0xF050 ⇒ U+E010（不是 U+2661）----
    g_current_case = "cp932/pua";
    LIFT_CHECK_EQ_U32(lift::gdiCp932Char(0xF050u), 0xE010u);
    LIFT_CHECK(lift::gdiCp932Char(0xF050u) != 0x2661u);  // ⛔ 不许"顺手"改成 ♡ 的普通码位
    {
        std::uint8_t bytes[2] = {0, 0};
        LIFT_CHECK_EQ_I64(lift::cp932Encode(static_cast<char16_t>(0xE010), bytes), 2);
        LIFT_CHECK_EQ_U32(bytes[0], 0xF0u);
        LIFT_CHECK_EQ_U32(bytes[1], 0x50u);
    }

    lift::FontEngine fonts;
    LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(fonts.glyphFallbackStats().fallback_uses), 0u);
    LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(fonts.glyphFallbackStats().notdefs), 0u);
    LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(fonts.memoryFontCount()), 0u);
    // 没有登记过的 id ⇒ 如实 false（⛔ 不假装移除成功）
    LIFT_CHECK(!fonts.removeMemoryFont(12345u));

    FakeSource fake;
    fonts.setGlyphSourceForTest(sourceFor(&fake, 1));

    // ---- ② 主 face 缺字 ⇒ 交给后备，返回**后备的 mask** ----
    g_current_case = "fallback/mask";
    {
        lift::GlyphMask mask;
        LIFT_CHECK(fonts.glyph(0xE010, 20, false, &mask));
        LIFT_CHECK_EQ_I64(mask.advance, kFallbackAdvance);
        LIFT_CHECK_EQ_I64(mask.black_box_x, kFallbackBoxX);
        LIFT_CHECK_EQ_I64(mask.black_box_y, kFallbackBoxY);
        LIFT_CHECK_EQ_U32(mask.coverage.empty() ? 0u : mask.coverage[0], kFallbackInk);
        const lift::FontEngine::GlyphFallbackStats stats = fonts.glyphFallbackStats();
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(stats.fallback_uses), 1u);
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(stats.notdefs), 0u);
    }

    // ---- ③ 同一 (cp, size, bold) 第二次命中缓存：不重复记账 ----
    g_current_case = "fallback/cache";
    {
        lift::GlyphMask mask;
        LIFT_CHECK(fonts.glyph(0xE010, 20, false, &mask));
        LIFT_CHECK_EQ_I64(mask.advance, kFallbackAdvance);
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(fonts.glyphFallbackStats().fallback_uses), 1u);
        // 换字号 ⇒ 必须重新解析（键里有字号）；仍然是后备提供的
        LIFT_CHECK(fonts.glyph(0xE010, 24, false, &mask));
        LIFT_CHECK_EQ_I64(mask.advance, kFallbackAdvance);
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(fonts.glyphFallbackStats().fallback_uses), 2u);
    }

    // ---- ④ 主 face 有字形 ⇒ 不碰后备（⛔ 不做"按名字一律用游戏字体"）----
    g_current_case = "fallback/primary-ok";
    {
        fake.fallback_queries = 0;
        lift::GlyphMask mask;
        LIFT_CHECK(fonts.glyph(0x41, 20, false, &mask));
        LIFT_CHECK_EQ_I64(mask.advance, kPrimaryAdvance);
        LIFT_CHECK_EQ_I64(mask.black_box_x, kPrimaryBoxX);
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(fake.fallback_queries), 0u);
        const lift::FontEngine::GlyphFallbackStats stats = fonts.glyphFallbackStats();
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(stats.fallback_uses), 2u);  // 没涨
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(stats.notdefs), 0u);
    }

    // ---- ⑤ 都缺 ⇒ 返回 true + `.notdef`（诚实），并且计数涨 ----
    g_current_case = "fallback/none";
    {
        fake.fallback_has_glyph = false;
        fonts.setGlyphSourceForTest(sourceFor(&fake, 1));  // 重装 = 清字形缓存
        lift::GlyphMask mask;
        LIFT_CHECK(fonts.glyph(0xE010, 20, false, &mask));  // ★ 不是 false：GDI 会画方块
        LIFT_CHECK_EQ_I64(mask.advance, kNotdefAdvance);
        LIFT_CHECK_EQ_I64(mask.black_box_x, kNotdefBox);
        LIFT_CHECK_EQ_U32(mask.coverage.empty() ? 0u : mask.coverage[0], kNotdefInk);
        const lift::FontEngine::GlyphFallbackStats stats = fonts.glyphFallbackStats();
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(stats.notdefs), 1u);
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(stats.fallback_uses), 2u);  // 没涨
        // 缓存命中不重复记账
        LIFT_CHECK(fonts.glyph(0xE010, 20, false, &mask));
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(fonts.glyphFallbackStats().notdefs), 1u);
    }

    // ---- ⑥ 后备来源变了 ⇒ 旧 `.notdef` 不能继续命中 ----
    g_current_case = "fallback/reinstall-clears-cache";
    {
        fake.fallback_has_glyph = true;
        fonts.setGlyphSourceForTest(sourceFor(&fake, 1));
        lift::GlyphMask mask;
        LIFT_CHECK(fonts.glyph(0xE010, 20, false, &mask));
        LIFT_CHECK_EQ_I64(mask.advance, kFallbackAdvance);
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(fonts.glyphFallbackStats().fallback_uses), 3u);
    }

    // ---- 卸下替身（回到真字体路径）也不崩；本 pure 档不依赖宿主字体是否在场 ----
    g_current_case = "fallback/uninstall";
    {
        fonts.setGlyphSourceForTest(lift::FontEngine::GlyphSourceForTest{});
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(fonts.glyphFallbackStats().notdefs), 1u);
    }

    return finish("gdi-font-fallback");
}

// ---------------------------------------------------------------------------------------
// 二、assets 档：真游戏安装的 `AGE-EXTEND.TTF`
// ---------------------------------------------------------------------------------------
std::uint32_t rd32(const std::vector<std::uint8_t>& b, std::size_t off) {
    if (off + 4 > b.size()) return 0;
    return static_cast<std::uint32_t>(b[off]) | (static_cast<std::uint32_t>(b[off + 1]) << 8) |
           (static_cast<std::uint32_t>(b[off + 2]) << 16) |
           (static_cast<std::uint32_t>(b[off + 3]) << 24);
}

std::string fixedName(const std::vector<std::uint8_t>& b, std::size_t off, std::size_t len) {
    std::string out;
    for (std::size_t i = 0; i < len && off + i < b.size() && b[off + i] != 0; ++i) {
        out.push_back(static_cast<char>(b[off + i]));
    }
    return out;
}

bool readWholeFile(const fs::path& path, std::vector<std::uint8_t>* out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    in.seekg(0, std::ios::end);
    const std::streamoff size = in.tellg();
    if (size <= 0) return false;
    in.seekg(0, std::ios::beg);
    out->resize(static_cast<std::size_t>(size));
    in.read(reinterpret_cast<char*>(out->data()), size);
    return static_cast<bool>(in);
}

struct AlfEntry {
    std::string name;
    std::uint32_t archive_index = 0;
    std::uint32_t offset = 0;
    std::uint32_t length = 0;
};

// 只读 SYS4INI.BIN 的 ALF 索引（S4 系：`S?IC` 头 300 B / `S?AC` 头 268 B）。
bool readSys4IniIndex(const std::vector<std::uint8_t>& index, std::vector<std::string>* archives,
                      std::vector<AlfEntry>* entries, std::string* err) {
    if (index.size() < 312) {
        *err = "索引文件太短";
        return false;
    }
    const std::string sig(reinterpret_cast<const char*>(index.data()), 4);
    std::size_t section_pos = 0;
    if (sig == "S3IC" || sig == "S4IC") {
        section_pos = 300;
    } else if (sig == "S3AC" || sig == "S4AC") {
        section_pos = 268;  // 加装档：段起点提前到 addonPos
    } else {
        *err = "不是 S4 系 ALF 索引（前 4 字节 = \"" + sig + "\"）";
        return false;
    }
    if (section_pos + 12 > index.size()) {
        *err = "段头越界";
        return false;
    }
    const std::uint32_t original_length = rd32(index, section_pos + 0);
    const std::uint32_t packed_length = rd32(index, section_pos + 8);
    if (original_length == 0 || packed_length == 0 ||
        section_pos + 12 + packed_length > index.size()) {
        *err = "段长度自相矛盾（目录区解压不可信）";
        return false;
    }
    std::vector<std::uint8_t> toc(original_length, 0);
    std::uint8_t ring[4096] = {};
    if (!lift::lzssDecode(index.data() + section_pos + 12, packed_length, toc.data(), original_length,
                          ring)) {
        *err = "目录区 LZSS 解压失败";
        return false;
    }

    const std::uint32_t archive_count = rd32(toc, 0);
    std::size_t off = 4;
    if (archive_count > 64) {
        *err = "archive_count 不合理";
        return false;
    }
    for (std::uint32_t i = 0; i < archive_count; ++i, off += 256) {
        if (off + 256 > toc.size()) {
            *err = "归档名表越界";
            return false;
        }
        archives->push_back(fixedName(toc, off, 256));
    }
    if (off > toc.size()) {
        *err = "目录区太短（缺 file_count）";
        return false;
    }
    const std::uint32_t file_count = rd32(toc, off);
    off += 4;
    if (static_cast<std::size_t>(file_count) > (toc.size() - off) / 80) {
        *err = "文件条目表越界（file_count 与目录区长度不符）";
        return false;
    }
    for (std::uint32_t i = 0; i < file_count; ++i, off += 80) {
        AlfEntry entry;
        entry.name = fixedName(toc, off, 64);
        entry.archive_index = rd32(toc, off + 64);
        entry.offset = rd32(toc, off + 72);
        entry.length = rd32(toc, off + 76);
        entries->push_back(entry);
    }
    return true;
}

bool readArchiveMember(const fs::path& archive, std::uint32_t offset, std::uint32_t length,
                       std::vector<std::uint8_t>* out) {
    std::ifstream in(archive, std::ios::binary);
    if (!in) return false;
    in.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    if (!in) return false;
    out->resize(length);
    in.read(reinterpret_cast<char*>(out->data()), length);
    return static_cast<bool>(in);
}

// ---------------------------------------------------------------------------------------
// 三、**经 shim** 的路径（裸 Machine + guest 调用形状；先例 `tests/gdi.cpp`）
// ---------------------------------------------------------------------------------------
// ★ 这一条补的是缺陷的另一半：⑦ 直接调 `addMemoryFont`、pure 档用接缝替身，**两者都绕过
//   `shimAddFontMemResourceEx`** ⇒ shim 退回"只记日志 / 传空 bytes"时全部 CTest 仍绿。
//   这里把 `AGE-EXTEND.TTF` 的字节放进 guest 堆，以 guest 调用形状调 shim，判据在**引擎侧**：
//   真的多了一个内存字体，且 U+E010 的 mask 与直接登记那条一致（⛔ 不是 `.notdef`）。
constexpr std::uint32_t kShimCodeAdd = 0x00403000u;
constexpr std::uint32_t kShimCodeRemove = 0x00403100u;
constexpr std::uint32_t kShimSlots = 0x00403200u;  // [0] = *pNumFonts，[4] = 句柄，[8] = Remove 返回值
constexpr std::uint32_t kShimStubBase = 0x7FF80000u;
constexpr std::uint32_t kShimSentinel = 0xFFFFFFFFu;
constexpr std::uint32_t kShimHeapSize = 0x00100000u;  // 1 MiB（够放 ~316 KiB 的 TTF；同 tests/gdi.cpp）

struct ShimAsm {
    explicit ShimAsm(std::uint32_t code_base) : base(code_base) {}
    std::vector<std::uint8_t> b;
    std::uint32_t base = 0;
    std::uint32_t ea() const { return base + static_cast<std::uint32_t>(b.size()); }
    void u8(std::uint8_t v) { b.push_back(v); }
    void u32(std::uint32_t v) {
        for (int i = 0; i < 4; ++i) b.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFFu));
    }
    void pushImm(std::uint32_t v) {
        u8(0x68);
        u32(v);
    }
    void pushMem(std::uint32_t slot) {
        u8(0xFF);
        u8(0x35);
        u32(slot);
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

std::uint32_t shimStubOf(lift::Machine& m, const char* name) {
    const lift::ShimDef* d = m.shims().find("gdi32", name);
    if (!d || !d->stub_ea) {
        std::fprintf(stderr, "stub missing: %s\n", name);
        return 0;
    }
    return d->stub_ea;
}

void reprimeShimStack(lift::Machine& m) {
    m.cpu().esp() = m.layout().stack_top - 4u;
    m.mem().wr32(m.cpu().esp(), kShimSentinel);
    m.cpu().running = true;
}

bool runShimCode(lift::Machine& m, std::uint32_t code_ea) {
    m.cpu().eip = code_ea;
    lift::RunOptions o;
    o.until_ea = kShimSentinel;
    o.max_insns = 200000;
    return m.run(o).reason == lift::StopReason::UntilEa;
}

int runAssets(const char* game_dir_arg) {
    using namespace lift_test;
    const fs::path game_dir(game_dir_arg);
    if (!fs::exists(game_dir / "SYS4INI.BIN")) {
        const std::string why = "安装树里没有 SYS4INI.BIN（" + (game_dir / "SYS4INI.BIN").string() + "）";
        LIFT_SKIP(why.c_str());
    }

    g_current_case = "alf/index";
    std::vector<std::uint8_t> index_bytes;
    LIFT_CHECK(readWholeFile(game_dir / "SYS4INI.BIN", &index_bytes));
    std::vector<std::string> archives;
    std::vector<AlfEntry> entries;
    std::string err;
    if (!readSys4IniIndex(index_bytes, &archives, &entries, &err)) {
        const std::string why = "ALF 索引读不出来：" + err;
        LIFT_SKIP(why.c_str());
    }
    const AlfEntry* age_extend = nullptr;
    for (const AlfEntry& entry : entries) {
        if (entry.name == "AGE-EXTEND.TTF") {
            age_extend = &entry;
            break;
        }
    }
    if (age_extend == nullptr) LIFT_SKIP("SYS4INI.BIN 的 ALF 索引里没有 AGE-EXTEND.TTF");
    if (age_extend->archive_index >= archives.size()) LIFT_SKIP("AGE-EXTEND.TTF 的 archive_index 越界");

    g_current_case = "alf/payload";
    std::vector<std::uint8_t> ttf;
    if (!readArchiveMember(game_dir / archives[age_extend->archive_index], age_extend->offset,
                           age_extend->length, &ttf)) {
        const std::string why =
            "取不到 AGE-EXTEND.TTF 的载荷（归档 " + archives[age_extend->archive_index] + "）";
        LIFT_SKIP(why.c_str());
    }
    LIFT_CHECK(ttf.size() == static_cast<std::size_t>(age_extend->length));
    // sfnt 魔数 = 大端 0x00010000（TrueType outlines）。`rd32` 是**小端**读数，所以逐个字节看；
    // 版本表细节不在这里查 —— FreeType 能不能建 face 才是判据（下一步）。
    LIFT_CHECK(ttf.size() > 12);
    LIFT_CHECK(ttf[0] == 0x00 && ttf[1] == 0x01 && ttf[2] == 0x00 && ttf[3] == 0x00);
    std::printf("AGE-EXTEND.TTF: %zu 字节（archive=%s offset=%u length=%u）\n", ttf.size(),
                archives[age_extend->archive_index].c_str(), age_extend->offset, age_extend->length);

    // ---- ① 直接问 FreeType：这个字体**有** U+E010，且能栅格化出非空位图 ----
    g_current_case = "ft/u-e010-mapped";
    FT_Library lib = nullptr;
    LIFT_CHECK(FT_Init_FreeType(&lib) == 0);
    if (lib == nullptr) return finish("gdi-font-fallback-assets");
    FT_Face face = nullptr;
    LIFT_CHECK(FT_New_Memory_Face(lib, ttf.data(), static_cast<FT_Long>(ttf.size()), 0, &face) == 0);
    if (face == nullptr) {
        FT_Done_FreeType(lib);
        return finish("gdi-font-fallback-assets");
    }
    std::printf("family=\"%s\" glyphs=%ld\n", face->family_name ? face->family_name : "?",
                static_cast<long>(face->num_glyphs));
    LIFT_CHECK(face->family_name != nullptr && std::strcmp(face->family_name, "AGE Extend") == 0);
    const FT_UInt heart_index = FT_Get_Char_Index(face, 0xE010);
    std::printf("FT_Get_Char_Index(AGE Extend, U+E010) = %u\n", static_cast<unsigned>(heart_index));
    LIFT_CHECK_NE_U32(static_cast<std::uint32_t>(heart_index), 0u);
    LIFT_CHECK(FT_Set_Pixel_Sizes(face, 0, 20) == 0);
    LIFT_CHECK(FT_Load_Char(face, 0xE010, FT_LOAD_RENDER | FT_LOAD_TARGET_NORMAL) == 0);
    LIFT_CHECK(face->glyph->bitmap.width > 0);
    LIFT_CHECK(face->glyph->bitmap.rows > 0);
    std::printf("AGE Extend U+E010: bitmap=%ux%u advance26.6=%ld\n",
                static_cast<unsigned>(face->glyph->bitmap.width),
                static_cast<unsigned>(face->glyph->bitmap.rows),
                static_cast<long>(face->glyph->advance.x));
    FT_Done_Face(face);
    FT_Done_FreeType(lib);

    // ---- ② FontEngine：宿主 Amayui CN 缺字 ⇒ `.notdef`；登记后备之后 ⇒ 由 AGE Extend 提供 ----
    g_current_case = "engine/host-font";
    lift::FontEngine fonts;
    if (!fonts.fontFileAvailable(false)) {
        LIFT_SKIP("缺宿主字体 Amayui-CN_cnjp.ttf（LIFT_HOST_FONT_PATH / corpus/assets/fonts）");
    }
    lift::GlyphMask notdef_mask;
    LIFT_CHECK(fonts.glyph(0xE010, 20, false, &notdef_mask));
    LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(fonts.glyphFallbackStats().notdefs), 1u);
    std::printf("宿主字体下 U+E010：blackbox=%dx%d advance=%d（.notdef，已计数）\n",
                notdef_mask.black_box_x, notdef_mask.black_box_y, notdef_mask.advance);

    g_current_case = "engine/fallback";
    std::string family;
    const std::uint32_t font_id = fonts.addMemoryFont(ttf.data(), ttf.size(), &family);
    LIFT_CHECK_NE_U32(font_id, 0u);
    LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(fonts.memoryFontCount()), 1u);
    LIFT_CHECK(family == "AGE Extend");
    lift::GlyphMask heart;
    LIFT_CHECK(fonts.glyph(0xE010, 20, false, &heart));
    LIFT_CHECK(hasInk(heart));
    LIFT_CHECK(heart.advance > 0);
    LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(fonts.glyphFallbackStats().fallback_uses), 1u);
    LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(fonts.glyphFallbackStats().notdefs), 1u);
    std::printf("后备字体下 U+E010：family=\"%s\" blackbox=%dx%d advance=%d（fallback=1）\n",
                family.c_str(), heart.black_box_x, heart.black_box_y, heart.advance);
    // 同一个码位在登记前后**不是**同一张 mask（否则"有后备"没有可观察后果）
    LIFT_CHECK(heart.advance != notdef_mask.advance ||
               heart.black_box_x != notdef_mask.black_box_x ||
               heart.black_box_y != notdef_mask.black_box_y);

    // ---- ③ 普通中文码位仍走主字体（后备只补缺字，不改中文来源）----
    g_current_case = "engine/primary-not-overridden";
    {
        lift::GlyphMask mask;
        LIFT_CHECK(fonts.glyph(U'你', 20, false, &mask));
        LIFT_CHECK(hasInk(mask));
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(fonts.glyphFallbackStats().fallback_uses), 1u);
    }

    // ---- ④ 移除后备 ⇒ 回到 `.notdef`（计数再 +1）----
    g_current_case = "engine/remove";
    LIFT_CHECK(fonts.removeMemoryFont(font_id));
    LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(fonts.memoryFontCount()), 0u);
    {
        lift::GlyphMask mask;
        LIFT_CHECK(fonts.glyph(0xE010, 20, false, &mask));
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(fonts.glyphFallbackStats().notdefs), 2u);
    }

    // ---- ⑤ **经 shim**：guest 调 AddFontMemResourceEx ⇒ 引擎真的多一个内存字体；Remove ⇒ 归零 ----
    // ★ 缺陷的另一半（见文件头 ⑧）：这里**不许**直接碰 `FontEngine::addMemoryFont` —— 字节必须
    //   由 guest 内存经 `shimAddFontMemResourceEx` 交进去；判据全在引擎侧（句柄 / 计数 / mask）。
    g_current_case = "shim/setup";
    lift::Machine m;
    std::string merr;
    LIFT_CHECK(m.initBare(&merr));
    LIFT_CHECK(m.prepareStack(&merr));
    LIFT_CHECK(m.heap().init(&m.mem(), m.layout().heap_base, kShimHeapSize));
    lift::registerGdiFontShims(m.shims());
    {
        // bare 机器没有 bind：给每条**已实现**的 def 一个确定性桩地址（同 tests/gdi.cpp）。
        const std::size_t count = m.shims().all().size();
        for (std::uint32_t id = 0; id < count; ++id) {
            lift::ShimDef* d = const_cast<lift::ShimDef*>(m.shims().byId(id));
            if (!d || !d->fn) continue;
            d->stub_ea = kShimStubBase + id * 4u;
            m.cache().installShim(d->stub_ea, d->id);
        }
    }
    const std::uint32_t ttf_ea = m.heap().alloc(static_cast<std::uint32_t>(ttf.size()), true);
    LIFT_CHECK_NE_U32(ttf_ea, 0u);
    if (ttf_ea == 0) return finish("gdi-font-fallback-assets");
    m.mem().writeBytes(ttf_ea, ttf.data(), ttf.size());
    const std::vector<std::uint8_t> slots_zero(12, 0);
    LIFT_CHECK(m.placeData(kShimSlots, slots_zero.data(), slots_zero.size(), &merr));

    // 装 shim **之前**：这台 Machine 的引擎对 U+E010 也是 `.notdef`（基线在本机成立）
    g_current_case = "shim/host-font-notdef";
    {
        lift::GlyphMask before;
        LIFT_CHECK(m.fonts().glyph(0xE010, 20, false, &before));
        LIFT_CHECK_EQ_I64(before.advance, notdef_mask.advance);
        LIFT_CHECK_EQ_I64(before.black_box_x, notdef_mask.black_box_x);
        LIFT_CHECK_EQ_I64(before.black_box_y, notdef_mask.black_box_y);
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(m.fonts().glyphFallbackStats().notdefs), 1u);
    }

    g_current_case = "shim/add-font-mem";
    {
        ShimAsm a(kShimCodeAdd);
        a.pushImm(kShimSlots);                              // pNumFonts
        a.pushImm(0);                                       // pdv（保留）
        a.pushImm(static_cast<std::uint32_t>(ttf.size()));  // cjBytes
        a.pushImm(ttf_ea);                                  // pvFile（guest 堆上的 TTF 字节）
        a.callRel(shimStubOf(m, "AddFontMemResourceEx"));
        a.storeEax(kShimSlots + 4u);  // 句柄
        a.ret();
        LIFT_CHECK(m.placeCode(kShimCodeAdd, a.b.data(), a.b.size(), &merr));
        reprimeShimStack(m);
        LIFT_CHECK(runShimCode(m, kShimCodeAdd));
        LIFT_CHECK_NE_U32(m.mem().rd32(kShimSlots + 4u), 0u);
        LIFT_CHECK_MEM_U32(m.mem(), kShimSlots, 1u);  // *pNumFonts == 1
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(m.fonts().memoryFontCount()), 1u);
        LIFT_CHECK(m.fonts().memoryFontFamily(0) == "AGE Extend");
    }

    g_current_case = "shim/fallback-glyph";
    {
        lift::GlyphMask via_shim;
        LIFT_CHECK(m.fonts().glyph(0xE010, 20, false, &via_shim));
        LIFT_CHECK(hasInk(via_shim));
        // ★ 与**直接登记**那条（`engine/fallback` 的 heart）逐项一致 ⇒ shim 交出去的就是同一份
        //   TTF（⛔ 不是"传了空 bytes 却碰巧画了点什么"）。
        LIFT_CHECK_EQ_I64(via_shim.advance, heart.advance);
        LIFT_CHECK_EQ_I64(via_shim.black_box_x, heart.black_box_x);
        LIFT_CHECK_EQ_I64(via_shim.black_box_y, heart.black_box_y);
        LIFT_CHECK(via_shim.advance != notdef_mask.advance ||
                   via_shim.black_box_x != notdef_mask.black_box_x ||
                   via_shim.black_box_y != notdef_mask.black_box_y);  // ⛔ 不是 .notdef
        const lift::FontEngine::GlyphFallbackStats fb = m.fonts().glyphFallbackStats();
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(fb.fallback_uses), 1u);
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(fb.notdefs), 1u);  // 只有基线那次；没新增
    }

    g_current_case = "shim/remove-font-mem";
    {
        ShimAsm a(kShimCodeRemove);
        a.pushMem(kShimSlots + 4u);  // 用 Add 返回的**真句柄**（不是常量）
        a.callRel(shimStubOf(m, "RemoveFontMemResourceEx"));
        a.storeEax(kShimSlots + 8u);
        a.ret();
        LIFT_CHECK(m.placeCode(kShimCodeRemove, a.b.data(), a.b.size(), &merr));
        reprimeShimStack(m);
        LIFT_CHECK(runShimCode(m, kShimCodeRemove));
        LIFT_CHECK_MEM_U32(m.mem(), kShimSlots + 8u, 1u);
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(m.fonts().memoryFontCount()), 0u);
        lift::GlyphMask back;
        LIFT_CHECK(m.fonts().glyph(0xE010, 20, false, &back));
        LIFT_CHECK_EQ_I64(back.advance, notdef_mask.advance);
        LIFT_CHECK_EQ_I64(back.black_box_x, notdef_mask.black_box_x);
        LIFT_CHECK_EQ_I64(back.black_box_y, notdef_mask.black_box_y);
        const lift::FontEngine::GlyphFallbackStats fb = m.fonts().glyphFallbackStats();
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(fb.notdefs), 2u);  // 回 `.notdef` ⇒ notdef +1
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(fb.fallback_uses), 1u);
    }

    // ---- ⑥ `--profile` 的摘要行：必须打得出，且数字与存取器一致（先例 `font!TEXTMETRIC-cache`）----
    g_current_case = "profile/glyph-fallback-line";
    {
        std::FILE* tmp = std::tmpfile();
        LIFT_CHECK(tmp != nullptr);
        if (tmp != nullptr) {
            m.dumpProfile(tmp, 5);
            std::rewind(tmp);
            std::string text(65536, '\0');
            const std::size_t got = std::fread(&text[0], 1, text.size() - 1, tmp);
            text.resize(got);
            std::fclose(tmp);
            const lift::FontEngine::GlyphFallbackStats fb = m.fonts().glyphFallbackStats();
            char expect[160];
            std::snprintf(expect, sizeof(expect),
                          "font!glyph-fallback fallback=%llu notdef=%llu memory-fonts=%zu",
                          static_cast<unsigned long long>(fb.fallback_uses),
                          static_cast<unsigned long long>(fb.notdefs),
                          m.fonts().memoryFontCount());
            LIFT_CHECK(text.find("font!glyph-fallback") != std::string::npos);
            LIFT_CHECK(text.find(expect) != std::string::npos);
        }
    }

    return finish("gdi-font-fallback-assets");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc >= 2) {
        // assets 臂（`--assets <安装根>`）：CTest 只在安装树存在时注册这条（档位 = 注册位置）。
        if (std::strcmp(argv[1], "--assets") == 0) {
            if (argc < 3) {
                std::printf("SKIP: --assets 需要安装根参数（CTest 传入 ${LIFT_ASSETS_GAME_DIR}）\n");
                return 77;
            }
            return runAssets(argv[2]);
        }
        std::printf("用法：lift-gdi-font-fallback [--assets <安装根>]\n");
        return 2;
    }
    return runPure();
}
