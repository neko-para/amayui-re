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
//      提交一帧 ⇒ 窗口像素左上角出现叠层（深底 + 亮字），矩形外仍是 guest 后缓冲的颜色；
//      关掉 `setOverlay(false)` ⇒ 恢复成纯画面；两种情况下 **guest 像素逐字节不变**。
//   ⑤ **直出路径**（`--present window --raster gpu`）：claim 成功 ⇒ 同一组断言在 GPU 直出上成立
//      （窗口不可 claim / 没有 GPU ⇒ SKIP 77 + 实测原因，⛔ 不拿 renderer 假绿）。
//   ⑤b **直出 letterbox**（B1 回归守卫）：把窗口强制成 1000×1000（target 320×240 ⇒ 画面居中、
//      上下有黑边）⇒ 叠层 blit 的 dst 原点必须 == **画面**矩形的左上角，且窗口 (0,0) 是黑边。
//      ⛔ 不是"窗口 (0,0)"；⛔ 不依赖真实鼠标 / 真实分辨率。
//   ⑥ **CLI smoke**（assets 档）：`--present window --present-hidden --overlay --max-frames 2`
//      exit 0，且 `--present-screenshot` 的 BMP 左上角真的被叠层覆盖（精确颜色）；同一配置去掉
//      `--overlay` 也 exit 0。窗口/GPU 不可用 ⇒ SKIP 77 + 原因。
//
// ★ 本文件的断言只写**不变量**（行格式 / 矩形边界 / 颜色精确值 / exit 0），⛔ 不断言 fps 或帧号
//   的具体数值（那是读数，不是契约）。
// ★ 子进程输出走**文件重定向**（⛔ 不走管道：受限沙箱里捕获管道会 EPERM）。
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "check.h"
#include "runtime/host_encoding.h"
#include "runtime/memory.h"
#include "runtime/overlay.h"
#include "runtime/present_sdl.h"
#include "runtime/surface.h"

#include <SDL3/SDL.h>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/wait.h>
#endif

namespace {

namespace fs = std::filesystem;

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

// 扫窗口像素左上角一块（覆盖叠层）；返回统计。⛔ 不依赖文字内容的具体长度 ——
// 这里只问"深底占了这块、亮字出现了、矩形外还是 guest 色"。
struct TopLeftScan {
    long bg = 0;
    long fg = 0;
    long guest = 0;
    long other = 0;
};

bool scanTopLeft(lift::SdlWindow& win, int w, int h, TopLeftScan* out, std::string* err) {
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
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

// 矩形外必须**逐像素**还是 guest 色（叠层"只占左上角"，⛔ 不遮别的区域）。
// ★ `rect` = `overlaySize()` 算出的**实际**矩形（⛔ 不要用一个拍脑袋的 120×60 —— 那可能比
//   实际叠层更大，等于放过"叠层画到矩形外"的回归）。
void expectGuestUntouched(lift::SdlWindow& win, const lift::GuestSurface& s, const lift::OverlaySize& rect) {
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
                if (x < rect.w && y < rect.h) continue;
                lift_test::failf(__FILE__, __LINE__, "(%d,%d) 被遮住了：expected %06X, got %06X", x, y, kGuestRgb,
                                 argb & 0xFFFFFFu);
                ++lift_test::g_checks;
                continue;
            }
            ++lift_test::g_checks;
        }
    }
}

// 第一帧（frame=1 / fps=1.0 / 无 guest 窗口 ⇒ 无 mouse 行）的叠层矩形。
// ★ 与实现走**同一份** `formatOverlayLines` + `overlaySize`（⛔ 不猜宽度）。
lift::OverlaySize expectedFirstFrameSize(const char* raster, const char* present) {
    lift::OverlayState st;
    st.fps_available = true;
    st.fps = 1.0;
    st.frame_available = true;
    st.frame = 1;
    st.raster = raster;
    st.present = present;
    return lift::overlaySize(lift::formatOverlayLines(st, lift::OverlayStatus{}));
}

constexpr std::uint32_t kOverlayBgRgb = (static_cast<std::uint32_t>(lift::kOverlayBg[0]) << 16) |
                                        (static_cast<std::uint32_t>(lift::kOverlayBg[1]) << 8) |
                                        lift::kOverlayBg[2];

std::vector<std::uint8_t> guestBytes(Fx& f) {
    const auto* p = f.arena.pixels(*f.s);
    return std::vector<std::uint8_t>(p, p + static_cast<std::size_t>(f.s->pitch) * f.s->height);
}

// ④ renderer 路径：software 渲染器（CTest 里配 SDL_VIDEODRIVER=dummy）。
// 返回 false **只**表示环境不可用（没有窗口系统）；真实回归用 LIFT_CHECK 记红。
bool caseRendererWindow(std::string* skip_why) {
    lift_test::g_current_case = "overlay/renderer-window";
    Fx f;
    if (!make(f, 256, 128, kGuestArgb)) {
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
    if (!win.submitFrame(f.arena, *f.s, &err)) {
        LIFT_CHECK(false);
        win.shutdown();
        return true;
    }
    TopLeftScan scan;
    if (!scanTopLeft(win, 80, 30, &scan, &err)) LIFT_CHECK(false);
    // 深底实心 + 亮字真的出现（80×30 完全落在叠层里：最长行 ≤ 16 字符 ⇒ 宽 97 / 高 33）
    LIFT_CHECK(scan.bg > 0);
    LIFT_CHECK(scan.fg > 0);
    LIFT_CHECK(scan.other == 0);
    expectGuestUntouched(win, *f.s, expectedFirstFrameSize("cpu", "renderer"));
    LIFT_CHECK(guestBytes(f) == before);  // ⛔ 叠层不写 guest 像素

    // 关掉开关 ⇒ 左上角恢复纯画面（证明"默认关 / 可关"，不是一直在画）
    win.setOverlay(false);
    LIFT_CHECK(!win.overlayEnabled());
    if (!win.submitFrame(f.arena, *f.s, &err)) LIFT_CHECK(false);
    std::uint32_t argb = 0;
    LIFT_CHECK(win.readbackPixel(0, 0, &argb, &err));
    LIFT_CHECK_EQ_U32(argb & 0x00FFFFFFu, kGuestRgb);
    LIFT_CHECK(guestBytes(f) == before);
    win.shutdown();
    return true;
}

// ⑤ 直出路径：真 GPU claim（没有 GPU / claim 不成立 ⇒ SKIP，⛔ 不拿 renderer 假绿）。
bool caseDirectWindow(std::string* skip_why) {
    lift_test::g_current_case = "overlay/direct-window";
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
    if (!win.submitFrame(f.arena, *f.s, &err)) {
        LIFT_CHECK(false);
        win.shutdown();
        return true;
    }
    TopLeftScan scan;
    if (!scanTopLeft(win, 80, 30, &scan, &err)) LIFT_CHECK(false);
    LIFT_CHECK(scan.bg > 0);
    LIFT_CHECK(scan.fg > 0);
    LIFT_CHECK(scan.other == 0);
    expectGuestUntouched(win, *f.s, expectedFirstFrameSize("gpu", "direct"));
    LIFT_CHECK(guestBytes(f) == before);

    win.setOverlay(false);
    if (!win.submitFrame(f.arena, *f.s, &err)) LIFT_CHECK(false);
    std::uint32_t argb = 0;
    LIFT_CHECK(win.readbackPixel(0, 0, &argb, &err));
    LIFT_CHECK_EQ_U32(argb & 0x00FFFFFFu, kGuestRgb);
    LIFT_CHECK(guestBytes(f) == before);

    // ---- B1 回归守卫：窗口 > 画面 ⇒ letterbox；叠层必须锚在**画面**矩形左上角 ----
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
        // "画面矩形原点"独立算一遍：letterbox(320×240 → 窗口像素 pw×ph)。
        const double sx = static_cast<double>(pw) / 320.0;
        const double sy = static_cast<double>(ph) / 240.0;
        const double scale = sx < sy ? sx : sy;
        const int dw = static_cast<int>(std::lround(320.0 * scale));
        const int dh = static_cast<int>(std::lround(240.0 * scale));
        const int ex = (pw - dw) / 2;
        const int ey = (ph - dh) / 2;
        LIFT_CHECK(ex > 0 || ey > 0);  // 真的出现黑边（否则这条断言没有区分力）
        lift::GpuDirectPresenter* dp = win.directPresenter();
        LIFT_CHECK(dp != nullptr);
        int ox = -1;
        int oy = -1;
        LIFT_CHECK(dp != nullptr && dp->directLastOverlayOrigin(&ox, &oy));
        std::printf("letterbox: window=%dx%d picture_origin=(%d,%d) overlay_origin=(%d,%d)\n", pw, ph, ex, ey, ox,
                    oy);
        LIFT_CHECK_EQ_I64(ox, ex);
        LIFT_CHECK_EQ_I64(oy, ey);
        // 像素级证据：叠层底色在**画面**原点；窗口原点 (0,0) 是黑边（⛔ 不是叠层）。
        std::uint32_t px = 0;
        LIFT_CHECK(win.readbackPixel(ex, ey, &px, &err));
        LIFT_CHECK_EQ_U32(px & 0x00FFFFFFu, kOverlayBgRgb);
        LIFT_CHECK(win.readbackPixel(0, 0, &px, &err));
        LIFT_CHECK_EQ_U32(px & 0x00FFFFFFu, 0u);
        LIFT_CHECK(guestBytes(f) == before);
    }
    win.shutdown();
    return true;
}

// =======================================================================================
// ⑥ CLI smoke（assets 档）：起子进程跑真游戏到第 2 帧
// =======================================================================================
struct ChildResult {
    int exit_code = -1;
    bool started = false;
};

#if defined(_WIN32)
std::wstring quoteArg(const std::wstring& s) { return L"\"" + s + L"\""; }

ChildResult runChild(const fs::path& exe, const std::vector<std::string>& args, const fs::path& log_path) {
    ChildResult r;
    std::wstring joined;
    for (const std::string& a : args) joined += L" " + quoteArg(lift::utf8ToWide(a));
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    const HANDLE h = ::CreateFileW(log_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS,
                                   FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return r;
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = h;
    si.hStdError = h;
    PROCESS_INFORMATION pi{};
    std::wstring cmd = quoteArg(exe.wstring()) + joined;
    const BOOL ok = ::CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                                     nullptr, &si, &pi);
    ::CloseHandle(h);
    if (!ok) return r;
    r.started = true;
    const DWORD waited = ::WaitForSingleObject(pi.hProcess, 480000u);
    if (waited != WAIT_OBJECT_0) {
        ::TerminateProcess(pi.hProcess, 991u);
        ::WaitForSingleObject(pi.hProcess, 10000u);
    }
    DWORD code = 0xFFFFFFFFu;
    ::GetExitCodeProcess(pi.hProcess, &code);
    r.exit_code = static_cast<int>(code);
    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);
    return r;
}
#else
std::string shellQuote(const std::string& s) {
    std::string out = "'";
    for (const char c : s) out += (c == '\'') ? std::string("'\\''") : std::string(1, c);
    out += "'";
    return out;
}

ChildResult runChild(const fs::path& exe, const std::vector<std::string>& args, const fs::path& log_path) {
    ChildResult r;
    std::string cmd = shellQuote(exe.string());
    for (const std::string& a : args) cmd += " " + shellQuote(a);
    cmd += " > " + shellQuote(log_path.string()) + " 2>&1";
    const int rc = std::system(cmd.c_str());
    if (rc == -1 || !fs::exists(log_path)) return r;
    r.started = true;
    r.exit_code = WIFEXITED(rc) ? WEXITSTATUS(rc) : (WIFSIGNALED(rc) ? 128 + WTERMSIG(rc) : 1);
    return r;
}
#endif

std::string readAllBytes(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return {};
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

bool contains(const std::string& hay, const char* needle) { return hay.find(needle) != std::string::npos; }

// 环境不满足（不是判据红）：窗口系统 / GPU / 执行源。
bool environmentUnavailable(const std::string& log) {
    return contains(log, "窗口打开失败") || contains(log, "要求 GPU 光栅化") ||
           contains(log, "GPU 光栅化后端创建失败") || contains(log, "SDL_CreateWindow 失败") ||
           contains(log, "SDL_CreateRenderer") || contains(log, "SDL_Init(VIDEO) 失败") ||
           // `--present window` 的 init 失败（窗口 / GPU / renderer 三选一）= 环境缺失。
           contains(log, "--present window 失败") || contains(log, "找不到执行源") || contains(log, "拒绝启动");
}

// 从窗口 BMP 统计左上角一块里的叠层颜色（用 SDL 自己解，不手写 BMP 解析）。
bool scanBmpTopLeft(const fs::path& bmp, long* bg, long* fg, std::string* why) {
    *bg = 0;
    *fg = 0;
    SDL_Surface* loaded = SDL_LoadBMP(bmp.string().c_str());
    if (loaded == nullptr) {
        *why = std::string("SDL_LoadBMP 失败：") + SDL_GetError();
        return false;
    }
    SDL_Surface* argb = SDL_ConvertSurface(loaded, SDL_PIXELFORMAT_ARGB8888);
    SDL_DestroySurface(loaded);
    if (argb == nullptr) {
        *why = std::string("SDL_ConvertSurface 失败：") + SDL_GetError();
        return false;
    }
    const auto bg_rgb = (static_cast<std::uint32_t>(lift::kOverlayBg[0]) << 16) |
                        (static_cast<std::uint32_t>(lift::kOverlayBg[1]) << 8) | lift::kOverlayBg[2];
    const auto fg_rgb = (static_cast<std::uint32_t>(lift::kOverlayFg[0]) << 16) |
                        (static_cast<std::uint32_t>(lift::kOverlayFg[1]) << 8) | lift::kOverlayFg[2];
    const int w = argb->w < 160 ? argb->w : 160;
    const int h = argb->h < 64 ? argb->h : 64;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            std::uint32_t px = 0;
            std::memcpy(&px, static_cast<const std::uint8_t*>(argb->pixels) + static_cast<std::size_t>(y) * argb->pitch +
                                 static_cast<std::size_t>(x) * 4u,
                        4u);
            const std::uint32_t rgb = px & 0x00FFFFFFu;
            if (rgb == bg_rgb) ++*bg;
            if (rgb == fg_rgb) ++*fg;
        }
    }
    SDL_DestroySurface(argb);
    return true;
}

// 返回 false 只表示环境不可用；真实回归用 LIFT_CHECK 记红。
bool runWindowSmoke(const std::vector<std::string>& args, std::string* skip_why) {
    lift_test::g_current_case = "overlay/window-smoke";
    if (args.size() < 5) {
        std::fprintf(stderr, "用法：lift-overlay --window-smoke <emulator-lift> <game-dir> <work-dir>\n");
        LIFT_CHECK(false);
        return true;
    }
    const fs::path exe = args[2];
    const std::string game_dir = args[3];
    const fs::path work = args[4];
    std::error_code ec;
    fs::create_directories(work, ec);
    fs::create_directories(work / "appdata-overlay", ec);
    fs::create_directories(work / "appdata-plain", ec);

    auto childArgs = [&](bool with_overlay, const fs::path& shot, const fs::path& appdata) {
        std::vector<std::string> a = {
            "--present", "window",
            "--present-hidden",
            "--max-frames", "2",
            "--skip-logo",
            "--audio", "off",
            "--game-dir", game_dir,
            "--appdata", appdata.string(),
            "--present-screenshot", shot.string(),
            "--present-screenshot-frame", "2",
            "--time-limit", "300",
        };
        if (with_overlay) a.push_back("--overlay");
        return a;
    };

    const fs::path log_overlay = work / "overlay.log";
    const fs::path shot_overlay = work / "overlay.bmp";
    const ChildResult with = runChild(exe, childArgs(true, shot_overlay, work / "appdata-overlay"), log_overlay);
    const std::string log = readAllBytes(log_overlay);
    if (!with.started) {
        *skip_why = "起不了子进程（emulator-lift 路径不对？）";
        return false;
    }
    if (with.exit_code != 0) {
        if (environmentUnavailable(log)) {
            *skip_why = "窗口 / GPU 不可用（子进程 exit " + std::to_string(with.exit_code) + "）";
            return false;
        }
        std::fprintf(stderr, "子进程 log（前 4000 字节）：\n%.4000s\n", log.c_str());
        LIFT_CHECK(false);
        return true;
    }
    // 报告实际走的是哪条路（不是断言：两条都必须能跑）
    if (contains(log, "present=direct/")) {
        std::printf("overlay smoke: 路径 = GPU 直出（present=direct）\n");
    } else {
        std::printf("overlay smoke: 路径 = SDL_Renderer 上传（没有 present=direct）\n");
    }
    LIFT_CHECK(fs::exists(shot_overlay));
    long bg = 0;
    long fg = 0;
    std::string why;
    if (!scanBmpTopLeft(shot_overlay, &bg, &fg, &why)) {
        std::fprintf(stderr, "%s\n", why.c_str());
        LIFT_CHECK(false);
    } else {
        // 精确颜色 ≠ 猜：深底一块 + 亮字。阈值很宽（只防"没画"），不是"断言当前读数"。
        LIFT_CHECK(bg >= 500);
        LIFT_CHECK(fg >= 50);
        std::printf("overlay smoke: 左上角 160×64 里 bg=%ld fg=%ld\n", bg, fg);
    }

    // 同一配置去掉 --overlay：必须照样 exit 0（开关不改变执行）
    const fs::path log_plain = work / "plain.log";
    const ChildResult plain = runChild(exe, childArgs(false, work / "plain.bmp", work / "appdata-plain"), log_plain);
    const std::string plain_log = readAllBytes(log_plain);
    if (!plain.started) {
        LIFT_CHECK(false);
        return true;
    }
    if (plain.exit_code != 0) {
        if (environmentUnavailable(plain_log)) {
            *skip_why = "窗口 / GPU 不可用（对照臂 exit " + std::to_string(plain.exit_code) + "）";
            return false;
        }
        std::fprintf(stderr, "对照臂 log（前 4000 字节）：\n%.4000s\n", plain_log.c_str());
        LIFT_CHECK(false);
        return true;
    }
    // 开关只在窗口像素上：两臂都必须真的跑到 max-frames 停机（⛔ 不断言帧号数值）
    LIFT_CHECK(contains(log, "detail    : max-frames"));
    LIFT_CHECK(contains(plain_log, "detail    : max-frames"));
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    using namespace lift_test;
    const std::vector<std::string> args = lift::argvUtf8(argc, argv);
    const bool gpu = args.size() >= 2 && args[1] == "--gpu";
    const bool smoke = args.size() >= 2 && args[1] == "--window-smoke";

    if (smoke) {
        std::string skip_why;
        if (!runWindowSmoke(args, &skip_why)) LIFT_SKIP(skip_why.c_str());
        return finish("overlay-window-smoke");
    }

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
