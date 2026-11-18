// emulator-lift · 窗口调试叠层（`--overlay`）的守卫
//
// 判据（红了意味着什么，缺一条都不算）：
//   ① **字体**：5×7 位图逐字栅格化 —— 选几个字符断言逐行位图（`A` / `0` / `a` / `-` / 空格），
//      未覆盖字符必须返回 false（⛔ 不猜字形）；字号/字距/行距是确定的常量。
//   ② **格式化**：给定 `OverlayState` + `OverlayStatus` ⇒ 确定的行；拿不到的字段**整行省略**
//      （⛔ 不编数字）。顺序固定：mouse / script / fps / frame / raster / present / worst。
//      身份降级写 `id:0x%08X（降级）`；`guest` 没开块计时写 `--`（⛔ 不写 0.0）。
//   ②b **3 s 窗口**：`overlayWorstPush` / `overlayWorstMax` 是纯函数 ⇒ 用**假时间戳**测：
//      窗口外的旧样本必须被丢、窗口内取最大、空窗口 ⇒ `has_worst=false`（⛔ 不编 0）。
//   ③ **只写矩形**：给一块 0xCC 哨兵缓冲，画完之后叠层矩形**外**逐字节不变；矩形内每个像素都
//      不透明（深底），且存在亮字像素。★ 把矩形写宽 1 像素 ⇒ 这条当场红（见测试记录）。
//   ④ **renderer 路径**（`--present window`、SDL_Renderer 上传）：`SdlWindow` 用 software 渲染器
//      提交一帧 ⇒ 窗口像素的**画布原点**（16:9 帧 ⇒ == 画面原点）出现宿主字体 HUD（深底 + 亮字），
//      矩形外仍是 guest 后缓冲的颜色；关掉 `setOverlay(false)` ⇒ 恢复成纯画面（逐像素）；
//      两种情况下 **guest 像素逐字节不变**。
//   ⑤ **直出路径**（`--present window`）：claim 成功 ⇒ 同一组断言在 GPU 直出上成立
//      （窗口不可 claim / 没有 GPU / 缺宿主字体 ⇒ SKIP 77 + 实测原因，⛔ 不拿 renderer 假绿）。
//   ⑤b **直出 letterbox / 画布原点**（B2 回归守卫）：把窗口强制成 1000×1000（target 320×240 ⇒
//      画面居中、上下有黑边）⇒ 叠层 blt 的 dst 原点必须 == **画布原点**
//      （画面原点 + `uiCanvasOrigin` 的居中偏移 `(0,93)` ⇒ `(0,218)`），窗口 (0,0) 是黑边。
//      ⛔ 不是"窗口 (0,0)"，⛔ 也不是"画面矩形原点"；⛔ 不依赖真实鼠标 / 真实分辨率。
// ★ 原来的 ⑥「CLI smoke（assets 档：两次真 boot + BMP 左上角色块）」已于 2026-11-13 随 CTest
//   瘦身删除（CTest `overlay-window-smoke`）：叠层是否被画出来由本文件的 ③④ 逐像素覆盖；
//   端到端"AOT `--overlay` 上屏 / `--dump-frame` 不含 UI"由 `tests/aot_overlay.cpp` 覆盖。
//
// ★ 本文件的断言只写**不变量**（行格式 / 矩形边界 / 颜色精确值），⛔ 不断言 fps 或帧号
//   的具体数值（那是读数，不是契约）。★ 生产字形 = **宿主字体**（U1）：缺字体 ⇒ SKIP 77。
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

#include "check.h"
#include "runtime/font.h"
#include "runtime/host_encoding.h"
#include "runtime/memory.h"
#include "runtime/overlay.h"
#include "runtime/present_sdl.h"
#include "runtime/surface.h"
#include "ui/canvas.h"
#include "ui/hud.h"

#include <SDL3/SDL.h>

namespace {

// =======================================================================================
// ① 字体逐字栅格化
// =======================================================================================
void expectGlyph(char ch, const std::uint8_t (&expected)[lift::kOverlayGlyphH]) {
    std::uint8_t rows[lift::kOverlayGlyphH] = {};
    LIFT_CHECK(lift::overlayGlyph(ch, rows));
    for (int y = 0; y < lift::kOverlayGlyphH; ++y) {
        LIFT_CHECK_EQ_U32(rows[y], expected[y]);
        // 低 5 位之外必须是 0（位表只有 5 列）
        LIFT_CHECK_EQ_U32(rows[y] & 0xE0u, 0u);
    }
}

void caseGlyphBitmaps() {
    lift_test::g_current_case = "overlay/glyph-bitmaps";
    // 'A'： ###  / #   # / #   # / ##### / #   # / #   # / #   #
    expectGlyph('A', {0x0Eu, 0x11u, 0x11u, 0x1Fu, 0x11u, 0x11u, 0x11u});
    // '0'： ###  / #   # / #  ## / # # # / ##  # / #   # /  ###
    expectGlyph('0', {0x0Eu, 0x11u, 0x13u, 0x15u, 0x19u, 0x11u, 0x0Eu});
    // 'a'：小写必须有**自己的**字形（⛔ 不是大写 A 的别名）
    expectGlyph('a', {0x00u, 0x00u, 0x0Eu, 0x01u, 0x0Fu, 0x11u, 0x0Fu});
    // '-'：第 4 行一条横线
    expectGlyph('-', {0x00u, 0x00u, 0x00u, 0x1Fu, 0x00u, 0x00u, 0x00u});
    // 空格：全 0，但必须**被覆盖**（返回 true）
    expectGlyph(' ', {0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u});

    // 未覆盖字符：false + 全 0（⛔ 不画方块、不猜）
    std::uint8_t rows[lift::kOverlayGlyphH] = {};
    LIFT_CHECK(!lift::overlayGlyph('@', rows));
    for (int y = 0; y < lift::kOverlayGlyphH; ++y) LIFT_CHECK_EQ_U32(rows[y], 0u);
    LIFT_CHECK(!lift::overlayGlyph('\t', rows));

    // 字号 / 间距是确定的常量（布局不提供配置）
    LIFT_CHECK_EQ_I64(lift::kOverlayGlyphW, 5);
    LIFT_CHECK_EQ_I64(lift::kOverlayGlyphH, 7);
    LIFT_CHECK_EQ_I64(lift::kOverlayAdvance, 6);
    LIFT_CHECK_EQ_I64(lift::kOverlayLineH, 8);
}

// =======================================================================================
// ② 行格式化：给定 state ⇒ 确定字符串；拿不到的字段整行省略
// =======================================================================================
lift::OverlayState fullState() {
    lift::OverlayState st;
    st.mouse_available = true;
    st.mouse_x = 812;
    st.mouse_y = 622;
    st.fps_available = true;
    st.fps = 59.9;
    st.frame_available = true;
    st.frame = 1234;
    st.raster = "gpu";
    st.present = "direct";
    return st;
}

// 两行"身份 / 卡顿"数据的满配：script 有待解析的名字、worst 有值且块计时开着。
lift::OverlayStatus fullStatus() {
    lift::OverlayStatus st;
    st.has_script = true;
    st.script = "startup";
    st.has_worst = true;
    st.wall_max_ms = 33.3;
    st.guest_max_ms = 21.5;
    st.has_guest = true;
    st.shim_max_ms = 4.2;
    return st;
}

void caseFormatLines() {
    lift_test::g_current_case = "overlay/format";

    // ---- 无 script / 无 worst ⇒ 那两行整行省略（⛔ 不编 "script ?" / "worst 0.0"）----
    const std::vector<std::string> lines = lift::formatOverlayLines(fullState(), lift::OverlayStatus{});
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(lines.size()), 5);
    if (lines.size() == 5) {
        LIFT_CHECK(lines[0] == "mouse 812,622");
        LIFT_CHECK(lines[1] == "fps 59.9");
        LIFT_CHECK(lines[2] == "frame 1234");
        LIFT_CHECK(lines[3] == "raster gpu");
        LIFT_CHECK(lines[4] == "present direct");
    }

    // ---- 满配：顺序 = mouse / script / fps / frame / raster / present / worst ----
    const std::vector<std::string> all = lift::formatOverlayLines(fullState(), fullStatus());
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(all.size()), 7);
    if (all.size() == 7) {
        LIFT_CHECK(all[0] == "mouse 812,622");
        LIFT_CHECK(all[1] == "script startup");
        LIFT_CHECK(all[2] == "fps 59.9");
        LIFT_CHECK(all[3] == "frame 1234");
        LIFT_CHECK(all[4] == "raster gpu");
        LIFT_CHECK(all[5] == "present direct");
        LIFT_CHECK(all[6] == "worst 3s  wall=33.3ms guest=21.5ms shim=4.2ms");
    }

    // ---- 有 script + 有 worst + 块计时没开 ⇒ `guest=--`（⛔ 不写 0.0 冒充读数）----
    lift::OverlayStatus no_guest = fullStatus();
    no_guest.has_guest = false;
    no_guest.guest_max_ms = 21.5;  // 就算有残留数值，显示也必须是 `--`
    const std::vector<std::string> lines2 = lift::formatOverlayLines(
        lift::OverlayState{}, no_guest);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(lines2.size()), 2);
    if (lines2.size() == 2) {
        LIFT_CHECK(lines2[0] == "script startup");
        LIFT_CHECK(lines2[1] == "worst 3s  wall=33.3ms guest=-- shim=4.2ms");
    }

    // ---- 身份降级：`script id:0x%08X（降级）`（⛔ 不猜名字）----
    LIFT_CHECK(lift::overlayScriptPayload("startup", false, 0x0000002Au) == "startup");
    LIFT_CHECK(lift::overlayScriptPayload("", true, 0x0000002Au) == "id:0x0000002A（降级）");
    lift::OverlayStatus degraded;
    degraded.has_script = true;
    degraded.script = lift::overlayScriptPayload("", true, 0x0000002Au);
    const std::vector<std::string> lines3 = lift::formatOverlayLines(lift::OverlayState{}, degraded);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(lines3.size()), 1);
    if (lines3.size() == 1) LIFT_CHECK(lines3[0] == "script id:0x0000002A（降级）");

    // 拿不到 present ⇒ 只少这一行（顺序不变）
    lift::OverlayState st = fullState();
    st.present = nullptr;
    std::vector<std::string> four = lift::formatOverlayLines(st, lift::OverlayStatus{});
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(four.size()), 4);
    if (four.size() == 4) LIFT_CHECK(four.back() == "raster gpu");

    // 鼠标 / fps 也拿不到 ⇒ 只剩 frame / raster（⛔ 不编 0,0 / 0.0）
    st.mouse_available = false;
    st.fps_available = false;
    std::vector<std::string> two = lift::formatOverlayLines(st, lift::OverlayStatus{});
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(two.size()), 2);
    if (two.size() == 2) {
        LIFT_CHECK(two[0] == "frame 1234");
        LIFT_CHECK(two[1] == "raster gpu");
    }

    // raster / present 的空串 = 省略（与 nullptr 同义）
    st = fullState();
    st.raster = "";
    st.present = "";
    std::vector<std::string> three = lift::formatOverlayLines(st, lift::OverlayStatus{});
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(three.size()), 3);

    // fps 的小数位固定 1 位；frame 是十进制整数（负数鼠标坐标照原样，不 clamp）
    st = fullState();
    st.fps = 7.0;
    st.mouse_x = -3;
    st.mouse_y = 0;
    const std::vector<std::string> edge = lift::formatOverlayLines(st, lift::OverlayStatus{});
    if (edge.size() == 5) {
        LIFT_CHECK(edge[0] == "mouse -3,0");
        LIFT_CHECK(edge[1] == "fps 7.0");
    } else {
        LIFT_CHECK(false);
    }

    // 空 state / 空 status ⇒ 0 行（⛔ 不编造任何一行）
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(
                          lift::formatOverlayLines(lift::OverlayState{}, lift::OverlayStatus{}).size()),
                      0);
}

// =======================================================================================
// ②b 最近 3 s 窗口：纯函数 + **假时间戳**（⛔ 不依赖真实时钟 / 真实帧）
// =======================================================================================
std::chrono::steady_clock::time_point fakeAt(std::int64_t ms) {
    return std::chrono::steady_clock::time_point{} + std::chrono::milliseconds(ms);
}

lift::OverlayFrameSample sampleAt(std::int64_t ms, double wall, double guest, double shim) {
    lift::OverlayFrameSample s;
    s.at = fakeAt(ms);
    s.wall_ms = wall;
    s.guest_ms = guest;
    s.shim_ms = shim;
    return s;
}

void caseWorstWindow() {
    lift_test::g_current_case = "overlay/worst-window";

    // 空窗口 ⇒ has_worst=false（⛔ 不编 0 的一行）
    const lift::OverlayWorst empty = lift::overlayWorstMax({});
    LIFT_CHECK(!empty.has_worst);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(empty.wall_max_ms * 10.0), 0);

    std::deque<lift::OverlayFrameSample> q;
    lift::overlayWorstPush(q, sampleAt(0, 100.0, 1.0, 2.0));
    lift::overlayWorstPush(q, sampleAt(1000, 50.0, 3.0, 9.0));
    lift::overlayWorstPush(q, sampleAt(2500, 75.0, 2.0, 1.0));
    // 窗口 = 最新样本往前 3 s ⇒ 这里 3 个都还在
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(q.size()), 3);
    lift::OverlayWorst w = lift::overlayWorstMax(q);
    LIFT_CHECK(w.has_worst);
    LIFT_CHECK(w.wall_max_ms == 100.0);
    LIFT_CHECK(w.guest_max_ms == 3.0);
    LIFT_CHECK(w.shim_max_ms == 9.0);

    // ★ 推一个 3500 的样本 ⇒ cutoff = 500 ⇒ t=0 的旧样本**必须被丢**
    //   （把窗口改成 30 s ⇒ 这一条当场红：旧样本还在 ⇒ size=4 / wall_max 仍 100）。
    lift::overlayWorstPush(q, sampleAt(3500, 10.0, 0.5, 0.5));
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(q.size()), 3);
    LIFT_CHECK(q.front().at == fakeAt(1000));
    w = lift::overlayWorstMax(q);
    LIFT_CHECK(w.wall_max_ms == 75.0);
    LIFT_CHECK(w.guest_max_ms == 3.0);
    LIFT_CHECK(w.shim_max_ms == 9.0);

    // 空窗之后的第一个样本：窗口内**只剩它**（⛔ 不把上一轮的读数带过来）
    lift::overlayWorstPush(q, sampleAt(10000, 12.0, 0.0, 0.0));
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(q.size()), 1);
    w = lift::overlayWorstMax(q);
    LIFT_CHECK(w.has_worst);
    LIFT_CHECK(w.wall_max_ms == 12.0);
    LIFT_CHECK(w.guest_max_ms == 0.0);
    LIFT_CHECK(w.shim_max_ms == 0.0);

    // 时间倒退的样本直接丢（窗口口径只认"最新样本"；⛔ 不排序 / 不猜）
    lift::overlayWorstPush(q, sampleAt(9000, 999.0, 999.0, 999.0));
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(q.size()), 1);
    w = lift::overlayWorstMax(q);
    LIFT_CHECK(w.wall_max_ms == 12.0);

    // 边界：恰好 3 s 差的样本仍在窗口内（窗口 = [at-3s, at]），再旧 1 ms 就丢
    std::deque<lift::OverlayFrameSample> edge;
    lift::overlayWorstPush(edge, sampleAt(0, 5.0, 0.0, 0.0));
    lift::overlayWorstPush(edge, sampleAt(3000, 6.0, 0.0, 0.0));
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(edge.size()), 2);
    lift::overlayWorstPush(edge, sampleAt(3001, 7.0, 0.0, 0.0));
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(edge.size()), 2);  // t=0 被丢，剩 3000 / 3001
    LIFT_CHECK(edge.front().at == fakeAt(3000));
}

// =======================================================================================
// ③ 尺寸 + 只写矩形
// =======================================================================================
void caseSize() {
    lift_test::g_current_case = "overlay/size";
    // w = 2*1 + chars*6 - 1；h = 2*1 + lines*8 - 1
    const std::vector<std::string> two = {"abc", "de"};
    const lift::OverlaySize s2 = lift::overlaySize(two);
    LIFT_CHECK_EQ_I64(s2.w, 2 + 3 * 6 - 1);
    LIFT_CHECK_EQ_I64(s2.h, 2 + 2 * 8 - 1);
    const lift::OverlaySize empty = lift::overlaySize({});
    LIFT_CHECK_EQ_I64(empty.w, 0);
    LIFT_CHECK_EQ_I64(empty.h, 0);

    // 图片尺寸与 overlaySize 一致（同一份口径，⛔ 两处各算一遍）
    int w = 0;
    int h = 0;
    const std::vector<std::uint8_t> image = lift::renderOverlayImage(two, &w, &h);
    LIFT_CHECK_EQ_I64(w, s2.w);
    LIFT_CHECK_EQ_I64(h, s2.h);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(image.size()),
                      static_cast<std::int64_t>(s2.w) * static_cast<std::int64_t>(s2.h) * 4);
}

bool pixelIs(const std::vector<std::uint8_t>& buf, int stride, int x, int y, const std::uint8_t (&rgba)[4]) {
    const std::uint8_t* p = buf.data() + static_cast<std::size_t>(y) * stride + static_cast<std::size_t>(x) * 4u;
    return p[0] == rgba[0] && p[1] == rgba[1] && p[2] == rgba[2] && p[3] == rgba[3];
}

void caseOnlyWritesRect() {
    lift_test::g_current_case = "overlay/only-writes-rect";
    const std::vector<std::string> lines = {"mouse 812,622", "fps 59.9", "frame 1234",
                                            "raster gpu",    "present direct"};
    const lift::OverlaySize size = lift::overlaySize(lines);
    LIFT_CHECK(size.w > 0 && size.h > 0);

    // 缓冲比矩形大：矩形外全是 0xCC 哨兵。
    const int W = size.w + 37;
    const int H = size.h + 11;
    const int stride = W * 4;
    std::vector<std::uint8_t> buf(static_cast<std::size_t>(stride) * H, 0xCC);
    lift::renderOverlay(buf.data(), stride, size.w, size.h, lines);

    // 矩形外**逐字节**不变（⛔ 不许"差不多"：多写 1 像素这条就红）
    std::size_t outside_changed = 0;
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            const bool inside = x < size.w && y < size.h;
            const std::size_t at = static_cast<std::size_t>(y) * stride + static_cast<std::size_t>(x) * 4u;
            for (int c = 0; c < 4; ++c) {
                const bool expect_sentinel = !inside;
                if (expect_sentinel && buf[at + static_cast<std::size_t>(c)] != 0xCCu) ++outside_changed;
            }
        }
    }
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(outside_changed), 0);

    // 矩形内：每个像素都不透明（深底是实心的，不靠底下画面）
    std::size_t not_opaque = 0;
    std::size_t fg_pixels = 0;
    std::size_t bg_pixels = 0;
    for (int y = 0; y < size.h; ++y) {
        for (int x = 0; x < size.w; ++x) {
            const std::uint8_t* p =
                buf.data() + static_cast<std::size_t>(y) * stride + static_cast<std::size_t>(x) * 4u;
            if (p[3] != 255u) ++not_opaque;
            if (pixelIs(buf, stride, x, y, lift::kOverlayFg)) ++fg_pixels;
            if (pixelIs(buf, stride, x, y, lift::kOverlayBg)) ++bg_pixels;
        }
    }
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(not_opaque), 0);
    // 左上角是内边距 ⇒ 一定是底色；文字必须真的画出来了（亮字像素 > 0）
    LIFT_CHECK(pixelIs(buf, stride, 0, 0, lift::kOverlayBg));
    LIFT_CHECK(fg_pixels > 0);
    LIFT_CHECK(bg_pixels > 0);
    // 底 + 字 = 全部（矩形里没有"第三种像素"）
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(fg_pixels + bg_pixels),
                      static_cast<std::int64_t>(size.w) * static_cast<std::int64_t>(size.h));

    // 内容超过矩形 ⇒ 按矩形裁剪，矩形外仍然一个字节都不动
    std::vector<std::uint8_t> small(static_cast<std::size_t>(20 * 8 * 4), 0xCC);
    lift::renderOverlay(small.data(), 20 * 4, 10, 8, lines);
    std::size_t small_outside = 0;
    for (int y = 0; y < 8; ++y) {
        for (int x = 10; x < 20; ++x) {
            const std::size_t at = static_cast<std::size_t>(y) * 20 * 4 + static_cast<std::size_t>(x) * 4u;
            for (int c = 0; c < 4; ++c) {
                if (small[at + static_cast<std::size_t>(c)] != 0xCCu) ++small_outside;
            }
        }
    }
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(small_outside), 0);

    // 空 / 非法矩形：什么都不写（不出错、不越界）
    std::vector<std::uint8_t> none(64, 0xCC);
    lift::renderOverlay(none.data(), 8 * 4, 0, 8, lines);
    lift::renderOverlay(none.data(), 8 * 4, 8, 0, lines);
    lift::renderOverlay(nullptr, 8 * 4, 8, 8, lines);
    for (const std::uint8_t b : none) LIFT_CHECK_EQ_U32(b, 0xCCu);
}

// =======================================================================================
// 窗口路径夹具（④ renderer / ⑤ 直出）
// =======================================================================================
struct Fx {
    lift::GuestMemory mem;
    lift::SurfaceArena arena;
    std::uint32_t ea = 0;
    lift::GuestSurface* s = nullptr;
};

lift::SurfaceFormat format32() {
    lift::SurfaceFormat f;
    f.bpp = 32;
    f.rmask = 0x00FF0000u;
    f.gmask = 0x0000FF00u;
    f.bmask = 0x000000FFu;
    f.amask = 0;
    return f;
}

bool make(Fx& f, int w, int h, std::uint32_t argb) {
    if (!f.mem.reserve() || !f.arena.init(&f.mem, 0x10000000u, 0x00100000u)) return false;
    f.ea = f.arena.create(w, h, format32());
    f.s = f.arena.find(f.ea);
    if (f.ea == 0 || f.s == nullptr) return false;
    auto* p = f.arena.pixels(*f.s);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            std::memcpy(p + static_cast<std::size_t>(y) * f.s->pitch + static_cast<std::size_t>(x) * 4, &argb, 4);
        }
    }
    return true;
}

constexpr std::uint32_t kGuestArgb = 0x008040C0u;  // R=80 G=40 B=C0（X8R8G8B8）
constexpr std::uint32_t kGuestRgb = kGuestArgb & 0x00FFFFFFu;

// 扫窗口像素里叠层覆盖的那块区域（起点 = **画布原点**）；返回统计。⛔ 不依赖文字内容的具体
// 长度 —— 这里只问"深底占了这块、亮字出现了、矩形外还是 guest 色"。
struct TopLeftScan {
    long bg = 0;
    long fg = 0;
    long guest = 0;
    long other = 0;
};

bool scanRegion(lift::SdlWindow& win, int x0, int y0, int w, int h, TopLeftScan* out, std::string* err) {
    for (int y = y0; y < y0 + h; ++y) {
        for (int x = x0; x < x0 + w; ++x) {
            std::uint32_t argb = 0;
            if (!win.readbackPixel(x, y, &argb, err)) return false;
            const std::uint32_t rgb = argb & 0x00FFFFFFu;
            if (rgb == (static_cast<std::uint32_t>(lift::kOverlayBg[0]) << 16 |
                        static_cast<std::uint32_t>(lift::kOverlayBg[1]) << 8 | lift::kOverlayBg[2])) {
                ++out->bg;
            } else if (rgb == (static_cast<std::uint32_t>(lift::kOverlayFg[0]) << 16 |
                               static_cast<std::uint32_t>(lift::kOverlayFg[1]) << 8 | lift::kOverlayFg[2])) {
                ++out->fg;
            } else if (rgb == kGuestRgb) {
                ++out->guest;
            } else {
                ++out->other;
            }
        }
    }
    return true;
}

// 矩形外必须**逐像素**还是 guest 色（叠层"只占画布原点那块"，⛔ 不遮别的区域）。
// ★ `rect` = `expectedPanel()` 算出的**实际**叠层矩形（⛔ 不要拍脑袋给一个 120×60 —— 那可能比
//   实际叠层更大，等于放过"叠层画到矩形外"的回归）。
void expectGuestUntouched(lift::SdlWindow& win, const lift::GuestSurface& s, int ox, int oy, int rw, int rh) {
    std::string err;
    for (int y = 0; y < s.height; y += 7) {
        for (int x = 0; x < s.width; x += 11) {
            std::uint32_t argb = 0;
            if (!win.readbackPixel(x, y, &argb, &err)) {
                LIFT_CHECK(false);
                return;
            }
            if ((argb & 0x00FFFFFFu) != kGuestRgb) {
                // 叠层实际矩形内会被覆盖 ⇒ 那里不要求 guest 色
                if (x >= ox && x < ox + rw && y >= oy && y < oy + rh) continue;
                lift_test::failf(__FILE__, __LINE__, "(%d,%d) 被遮住了：expected %06X, got %06X", x, y, kGuestRgb,
                                 argb & 0xFFFFFFu);
                ++lift_test::g_checks;
                continue;
            }
            ++lift_test::g_checks;
        }
    }
}

// 第一帧（frame=1 / fps=1.0 / 无 guest 窗口 ⇒ 无 mouse 行）的叠层矩形，**按生产的 display list
// 构建路径算出来**（⛔ 不猜宽度；面板尺寸由宿主字体度量决定）。
// ★ `frame_w/h` = 被呈现的帧尺寸（画布 `s` 从 `uiCanvasOrigin` 取）。
bool expectedPanel(const char* raster, const char* present, lift::FontEngine& font, int frame_w, int frame_h,
                   int* ox, int* oy, int* w, int* h) {
    lift::OverlayState st;
    st.fps_available = true;
    st.fps = 1.0;
    st.frame_available = true;
    st.frame = 1;
    st.raster = raster;
    st.present = present;
    const std::vector<std::string> lines = lift::formatOverlayLines(st, lift::OverlayStatus{});
    lift::ui::UiBuilder builder;
    std::string err;
    if (!lift::ui::buildHud(builder, font, lines, &err)) return false;
    if (builder.elements().empty()) return false;
    const lift::ui::CanvasOrigin canvas = lift::ui::uiCanvasOrigin(frame_w, frame_h);
    const lift::ui::Element& panel = builder.elements().front();
    *ox = canvas.offset_x;  // 画面原点 = (0,0)（窗口 = 帧尺寸）
    *oy = canvas.offset_y;
    *w = static_cast<int>(static_cast<long long>(panel.w) * canvas.scale + 0.5);
    *h = static_cast<int>(static_cast<long long>(panel.h) * canvas.scale + 0.5);
    if (*w < 1) *w = 1;
    if (*h < 1) *h = 1;
    return true;
}

constexpr std::uint32_t kOverlayBgRgb = (static_cast<std::uint32_t>(lift::kOverlayBg[0]) << 16) |
                                        (static_cast<std::uint32_t>(lift::kOverlayBg[1]) << 8) |
                                        lift::kOverlayBg[2];

std::vector<std::uint8_t> guestBytes(Fx& f) {
    const auto* p = f.arena.pixels(*f.s);
    return std::vector<std::uint8_t>(p, p + static_cast<std::size_t>(f.s->pitch) * f.s->height);
}

// ④ renderer 路径：software 渲染器（CTest 里配 SDL_VIDEODRIVER=dummy）。
// 返回 false **只**表示环境不可用（没有窗口系统 / 缺宿主字体）；真实回归用 LIFT_CHECK 记红。
bool caseRendererWindow(std::string* skip_why) {
    lift_test::g_current_case = "overlay/renderer-window";
    lift::FontEngine font;
    if (!font.fontFileAvailable(false)) {
        *skip_why = "缺宿主字体 Amayui-CN_cnjp.ttf（LIFT_HOST_FONT_PATH / corpus/assets/fonts）";
        return false;
    }
    // ★ 16:9 帧 ⇒ 画布原点 == 画面矩形原点（B2 的第一条用例）。
    Fx f;
    if (!make(f, 640, 360, kGuestArgb)) {
        LIFT_CHECK(false);
        return true;
    }
    const std::vector<std::uint8_t> before = guestBytes(f);

    lift::SdlWindow win;
    lift::SdlWindowOptions opt;
    opt.title = "emulator-lift overlay test (renderer)";
    opt.driver_list = "software";
    opt.hidden = true;
    opt.overlay = true;
    win.setCaptureForTest(true);
    std::string err;
    if (!win.init(opt, &err)) {
        *skip_why = "窗口系统不可用：" + err;
        return false;
    }
    LIFT_CHECK(win.overlayEnabled());
    int ox = 0;
    int oy = 0;
    int rw = 0;
    int rh = 0;
    if (!expectedPanel("cpu", "renderer", font, 640, 360, &ox, &oy, &rw, &rh)) {
        LIFT_CHECK(false);
        win.shutdown();
        return true;
    }
    LIFT_CHECK_EQ_I64(ox, 0);  // 16:9 ⇒ 居中偏移 0
    LIFT_CHECK_EQ_I64(oy, 0);
    if (!win.submitFrame(f.arena, *f.s, &err)) {
        LIFT_CHECK(false);
        win.shutdown();
        return true;
    }
    TopLeftScan scan;
    if (!scanRegion(win, ox, oy, rw, rh, &scan, &err)) LIFT_CHECK(false);
    // 深底实心 + 亮字真的出现（扫描范围 = 生产 display list 算出的面板矩形）
    LIFT_CHECK(scan.bg > 0);
    LIFT_CHECK(scan.fg > 0);
    LIFT_CHECK(scan.other == 0);
    LIFT_CHECK(scan.guest == 0);
    // 画布原点那一个像素是底（不是 guest / 不是黑）—— B2 的"原点 == 画布原点"像素级证据。
    std::uint32_t argb = 0;
    LIFT_CHECK(win.readbackPixel(ox, oy, &argb, &err));
    LIFT_CHECK_EQ_U32(argb & 0x00FFFFFFu, kOverlayBgRgb);
    expectGuestUntouched(win, *f.s, ox, oy, rw, rh);
    LIFT_CHECK(guestBytes(f) == before);  // ⛔ 叠层不写 guest 像素

    // 关掉开关 ⇒ 整窗**逐像素**恢复成纯画面（证明"默认关 / 可关"，不是一直在画）
    win.setOverlay(false);
    LIFT_CHECK(!win.overlayEnabled());
    if (!win.submitFrame(f.arena, *f.s, &err)) LIFT_CHECK(false);
    long not_guest = 0;
    for (int y = 0; y < f.s->height; ++y) {
        for (int x = 0; x < f.s->width; ++x) {
            if (!win.readbackPixel(x, y, &argb, &err)) {
                ++not_guest;
                continue;
            }
            if ((argb & 0x00FFFFFFu) != kGuestRgb) ++not_guest;
        }
    }
    LIFT_CHECK_EQ_I64(not_guest, 0);
    LIFT_CHECK(guestBytes(f) == before);
    win.shutdown();
    return true;
}

// ⑤ 直出路径：真 GPU claim（没有 GPU / claim 不成立 ⇒ SKIP，⛔ 不拿 renderer 假绿）。
// ★ 帧用 **4:3（320×240）**：画布纵向居中偏移 30 px ⇒ 同时钉住 B2 的非 16:9 分支。
bool caseDirectWindow(std::string* skip_why) {
    lift_test::g_current_case = "overlay/direct-window";
    lift::FontEngine font;
    if (!font.fontFileAvailable(false)) {
        *skip_why = "缺宿主字体 Amayui-CN_cnjp.ttf（LIFT_HOST_FONT_PATH / corpus/assets/fonts）";
        return false;
    }
    Fx f;
    if (!make(f, 320, 240, kGuestArgb)) {
        LIFT_CHECK(false);
        return true;
    }
    const std::vector<std::uint8_t> before = guestBytes(f);

    lift::SdlWindow win;
    lift::SdlWindowOptions opt;
    opt.title = "emulator-lift overlay test (direct)";
    opt.hidden = true;
    opt.raster_gpu = true;
    opt.overlay = true;
    win.setCaptureForTest(true);
    std::string err;
    if (!win.init(opt, &err)) {
        *skip_why = "GPU / 窗口不可用：" + err;
        return false;
    }
    if (!win.directPresentActive()) {
        *skip_why = std::string("GPU 直出未成立：") + win.describe();
        win.shutdown();
        return false;
    }
    LIFT_CHECK(win.overlayEnabled());
    int ox = 0;
    int oy = 0;
    int rw = 0;
    int rh = 0;
    if (!expectedPanel("gpu", "direct", font, 320, 240, &ox, &oy, &rw, &rh)) {
        LIFT_CHECK(false);
        win.shutdown();
        return true;
    }
    LIFT_CHECK_EQ_I64(ox, 0);
    LIFT_CHECK_EQ_I64(oy, 30);  // 4:3 ⇒ 画布纵向居中 30 px（B2）
    if (!win.submitFrame(f.arena, *f.s, &err)) {
        LIFT_CHECK(false);
        win.shutdown();
        return true;
    }
    TopLeftScan scan;
    if (!scanRegion(win, ox, oy, rw, rh, &scan, &err)) LIFT_CHECK(false);
    LIFT_CHECK(scan.bg > 0);
    LIFT_CHECK(scan.fg > 0);
    LIFT_CHECK(scan.other == 0);
    LIFT_CHECK(scan.guest == 0);
    std::uint32_t argb = 0;
    LIFT_CHECK(win.readbackPixel(ox, oy, &argb, &err));
    LIFT_CHECK_EQ_U32(argb & 0x00FFFFFFu, kOverlayBgRgb);
    // 画布原点上方（画面原点那一块）仍是 guest 色 ⇒ 证明原点不是恒 0、也不是画面原点。
    LIFT_CHECK(win.readbackPixel(0, 0, &argb, &err));
    LIFT_CHECK_EQ_U32(argb & 0x00FFFFFFu, kGuestRgb);
    expectGuestUntouched(win, *f.s, ox, oy, rw, rh);
    LIFT_CHECK(guestBytes(f) == before);

    win.setOverlay(false);
    if (!win.submitFrame(f.arena, *f.s, &err)) LIFT_CHECK(false);
    long not_guest = 0;
    for (int y = 0; y < f.s->height; ++y) {
        for (int x = 0; x < f.s->width; ++x) {
            if (!win.readbackPixel(x, y, &argb, &err)) {
                ++not_guest;
                continue;
            }
            if ((argb & 0x00FFFFFFu) != kGuestRgb) ++not_guest;
        }
    }
    LIFT_CHECK_EQ_I64(not_guest, 0);
    LIFT_CHECK(guestBytes(f) == before);

    // ---- B2 回归守卫：窗口 > 画面 ⇒ letterbox；叠层必须锚在**画布原点** ----
    // ★ 用 SDL 自己的窗口 API 强制一个与 target 不同尺寸的窗口（test-only，不改生产路径；
    //   `submitDirect` 只在 **surface 尺寸变化** 时跟随窗口 ⇒ 这里窗口会保持 1000×1000）。
    win.setOverlay(true);
    LIFT_CHECK(win.overlayEnabled());
    SDL_Window* sdl_window = SDL_GetWindowFromID(win.windowIdForTest());
    LIFT_CHECK(sdl_window != nullptr);
    if (sdl_window != nullptr) {
        LIFT_CHECK(SDL_SetWindowSize(sdl_window, 1000, 1000));
        SDL_SyncWindow(sdl_window);
        // 连提两帧：第一帧给 swapchain 跟上 resize 的机会，判据只看第二帧。
        if (!win.submitFrame(f.arena, *f.s, &err)) LIFT_CHECK(false);
        if (!win.submitFrame(f.arena, *f.s, &err)) LIFT_CHECK(false);
        int pw = 0;
        int ph = 0;
        SDL_GetWindowSizeInPixels(sdl_window, &pw, &ph);
        LIFT_CHECK(pw > 320 && ph > 240);
        // ① 画面矩形（letterbox 320×240 → 窗口像素 pw×ph）独立算一遍。
        const double sx = static_cast<double>(pw) / 320.0;
        const double sy = static_cast<double>(ph) / 240.0;
        const double scale = sx < sy ? sx : sy;
        const int dw = static_cast<int>(std::lround(320.0 * scale));
        const int dh = static_cast<int>(std::lround(240.0 * scale));
        const int ex = (pw - dw) / 2;
        const int ey = (ph - dh) / 2;
        LIFT_CHECK(ex > 0 || ey > 0);  // 真的出现黑边（否则这条断言没有区分力）
        // ② 画布原点 = 画面原点 + `uiCanvasOrigin(画面宽,画面高)` 的居中偏移（**唯一一份**算术）。
        const lift::ui::CanvasOrigin canvas = lift::ui::uiCanvasOrigin(dw, dh);
        const int want_x = ex + canvas.offset_x;
        const int want_y = ey + canvas.offset_y;
        LIFT_CHECK(want_y > ey);  // 4:3 ⇒ 画布原点真的低于画面原点（否则区分不了两个口径）
        lift::GpuDirectPresenter* dp = win.directPresenter();
        LIFT_CHECK(dp != nullptr);
        int got_x = -1;
        int got_y = -1;
        LIFT_CHECK(dp != nullptr && dp->directLastOverlayOrigin(&got_x, &got_y));
        std::printf("letterbox: window=%dx%d picture=(%d,%d %dx%d) canvas_origin=(%d,%d) overlay_origin=(%d,%d)\n",
                    pw, ph, ex, ey, dw, dh, want_x, want_y, got_x, got_y);
        LIFT_CHECK_EQ_I64(got_x, want_x);
        LIFT_CHECK_EQ_I64(got_y, want_y);
        // ③ 像素级证据：叠层底色在**画布原点**；画面原点处仍是 guest 色；窗口原点 (0,0) 是黑边。
        std::uint32_t px = 0;
        LIFT_CHECK(win.readbackPixel(want_x, want_y, &px, &err));
        LIFT_CHECK_EQ_U32(px & 0x00FFFFFFu, kOverlayBgRgb);
        LIFT_CHECK(win.readbackPixel(ex, ey, &px, &err));
        LIFT_CHECK_EQ_U32(px & 0x00FFFFFFu, kGuestRgb);
        LIFT_CHECK(win.readbackPixel(0, 0, &px, &err));
        LIFT_CHECK_EQ_U32(px & 0x00FFFFFFu, 0u);
        LIFT_CHECK(guestBytes(f) == before);
    }
    win.shutdown();
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    using namespace lift_test;
    const std::vector<std::string> args = lift::argvUtf8(argc, argv);
    const bool gpu = args.size() >= 2 && args[1] == "--gpu";

    caseGlyphBitmaps();
    caseFormatLines();
    caseWorstWindow();
    caseSize();
    caseOnlyWritesRect();

    std::string skip_why;
    if (gpu) {
        if (!caseDirectWindow(&skip_why)) LIFT_SKIP(skip_why.c_str());
    } else {
        if (!caseRendererWindow(&skip_why)) LIFT_SKIP(skip_why.c_str());
    }
    return finish("overlay");
}
