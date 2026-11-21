// emulator-lift · `--scale <auto|数值>` 的纯几何 / 解析 / readout 判据（无窗口、无 SDL）
//
// 判据（红了意味着什么）：
//   ① **缺省 1.0 = 现状**：`ScaleSpec{}` 是 Fixed 1.0，窗口 = 1280×720，readout 逐字是
//      `scale: 1.0（window 1280x720，image 1280x720；filter=linear）`。红了 ⇒ `--scale` 的缺省不再与
//      `SDL_CreateWindow(…,1280,720,…)` 等价（"缺省行为不变"是承诺，不是巧合）。
//   ② **数值倍率**：窗口客户区 = `round(1280×s) × round(720×s)`，**画面仍是 1280×720**。
//      1.5 ⇒ 1920×1080；1.25 ⇒ 1600×900（非整数倍率，取整点在这里）；0.5 ⇒ 640×360。
//      红了 ⇒ 有人把窗口尺寸写成 `1280×s` 的截断 / 写死 / 顺手改了 image 尺寸。
//   ③ **鼠标映射复用既有公式**（⛔ 不另写一套）：窗口像素 → 画面像素**只**走
//      `runtime/cursor_map.h` 的 `mapWindowPixelToImage`，且与 `control_endpoint.cpp`
//      的判据 4 同一条（`referenceToClientPixel` 的反算回到同一个参考点）：
//        · 1.5（1920×1080）：窗口 (1070,480) → 画面 (713,320)；参考系 (713,320) → 客户区 (1070,480)；
//        · 1.25（1600×900）：窗口 (1070,480) → 画面 (856,384)；取整点 (1001,601) → (801,481)；
//        · 1.33（1702×958，**非 16:9**）：letterbox 有 0.3125 px 的纵向残差偏移 —— 丢掉偏移时
//          窗口 (0,2) 会算成画面 y=2（正确是 y=1）⇒ 这一格直接钉住"偏移参与映射"。
//      红了 ⇒ 窗口放大后鼠标系统性偏移（点 1.5 倍窗口的右上角会点偏），或有人又写了一套公式。
//   ④ **`auto` 的 DPI → scale 是纯函数**：内容缩放 1.0/1.25/1.5/2.0 原样、1.2499999 量化到 1.25；
//      0 / 负数 / NaN / Inf / > 8 ⇒ **回 1.0**，且 readout 里写明"取不到"（⛔ 不静默）。
//      红了 ⇒ auto 要么静默退回 1.0，要么把离谱值拿去建一个几万像素宽的窗口。
//   ⑤ **参数校验**：`auto` / 正数接受；`0` / 负数 / `nan` / `inf` / `abc` / `1.5x` / 空串拒绝
//      （CLI 侧照 `parseScaleSpec` 的 `ok=false` 走 exit 2 + 点名 `--scale`）。
//      红了 ⇒ 用户给了垃圾倍率却跑成了一个静默的缺省窗口。
//   ⑥ **headless 无副作用**：`applied=false` 时 readout 明说"不生效"，且不印 window（没有窗口），
//      仍带 `image 1280x720`。红了 ⇒ `--present headless` 下 `--scale` 被静默接受/静默忽略。
//   ⑦ **`--scale-filter` 只有两个值、缺省 linear**（2026-11-21）：`linear` / `nearest` 接受，
//      大小写不符 / 尾随空格 / 前缀 / 数值 / 空串一律拒（CLI 侧 exit 2 + 点名该参数），且拒绝原因
//      点名值域；readout 印出**当前**滤波器（1.5 + nearest 换一句"硬边"）。
//      红了 ⇒ 用户要的 nearest 对照静默变成 linear（或反之），A/B 与像素断言一起失效。
//
// ★ 本用例**只** include 纯头（`window_scale.h` / `cursor_map.h`）：不建窗口、不碰 GPU、不读资产
//   ⇒ 顶层无条件注册 = pure 档（`AGENTS.md` §12.4 的判据 = 注册位置）。
// ★ 窗口尺寸的**最终事实**来自 `SDL_GetWindowSize` 回读（`present_sdl.cpp`）；这一层只钉公式，
//   两者靠"公式只有一份"对齐（`windowScale` 的常量在 `window_scale.h`）。
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>

#include "check.h"
#include "runtime/cursor_map.h"
#include "runtime/window_scale.h"

namespace {

bool approxEq(double a, double b, double eps = 1e-9) { return std::fabs(a - b) <= eps; }

void expectSize(double scale, int want_w, int want_h, const char* what) {
    const lift::WindowSize size = lift::windowSizeForScale(scale);
    ++lift_test::g_checks;
    if (size.w != want_w || size.h != want_h) {
        lift_test::failf(__FILE__, __LINE__, "%s: scale %.4f expected window %dx%d, got %dx%d", what, scale, want_w,
                         want_h, size.w, size.h);
    }
}

// ---- ① 缺省 1.0 = 现状（窗口 1280×720；画面不动）----
void caseDefaultUnchanged() {
    lift_test::g_current_case = "window-scale/default-is-1.0";
    const lift::ScaleSpec spec;
    LIFT_CHECK(spec.kind == lift::ScaleSpec::Kind::Fixed);
    LIFT_CHECK(approxEq(spec.value, 1.0));
    expectSize(spec.value, 1280, 720, "缺省 1.0 ⇒ 1280×720（与 SDL_CreateWindow(…,1280,720,…) 相同）");
    // 缺省档 readout 的**逐字**形态（格式只在一处，改格式必须改这里 —— 这是"人读的那一行"）。
    lift::ScaleReport report;
    const std::string line = lift::scaleReadoutLine(report);
    std::printf("[readout-default] %s\n", line.c_str());
    ++lift_test::g_checks;
    if (line != "scale: 1.0（window 1280x720，image 1280x720；filter=linear）") {
        lift_test::failf(__FILE__, __LINE__, "缺省 readout 不是期望的那一行：`%s`", line.c_str());
    }
    // ★ 缺省滤波器 = **linear**（2026-11-21）：红了两件事之一 —— 有人把缺省退回 NEAREST
    //   （放大后比真机糙），或 `ScaleSpec{}` 的滤波器字段没被 readout 读到。
    LIFT_CHECK(spec.filter == lift::ScaleFilter::Linear);
    LIFT_CHECK(report.filter == lift::ScaleFilter::Linear);
    LIFT_CHECK(std::string(lift::scaleFilterName(lift::kDefaultScaleFilter)) == "linear");
}

// ---- ② 数值倍率：round(1280×s) × round(720×s)，画面仍 1280×720 ----
void caseFixedScales() {
    lift_test::g_current_case = "window-scale/fixed-sizes";
    expectSize(1.0, 1280, 720, "1.0");
    expectSize(1.25, 1600, 900, "1.25（非整数倍率）");
    expectSize(1.5, 1920, 1080, "1.5");
    expectSize(1.75, 2240, 1260, "1.75");
    expectSize(2.0, 2560, 1440, "2.0");
    expectSize(0.5, 640, 360, "0.5（缩小）");
    // 取整点：1.33 ⇒ 1280×1.33 = 1702.4 → 1702；720×1.33 = 957.6 → 958（四舍五入，不是截断）。
    expectSize(1.33, 1702, 958, "1.33（两个轴各自取整 ⇒ 非 16:9）");
    // 非正 / 非有限 ⇒ 退回基准（防御；CLI 已经在 parse 期拒绝，这里钉"兜底不炸 / 不出 0 宽"）。
    expectSize(0.0, 1280, 720, "scale=0 的兜底");
    expectSize(-1.5, 1280, 720, "负 scale 的兜底");
    expectSize(std::nan(""), 1280, 720, "NaN 的兜底");
    // 极小倍率也不许出 0 宽窗口。
    expectSize(0.0001, 1, 1, "0.0001 ⇒ 至少 1×1");
    // 窗口尺寸公式**不**碰画面尺寸：1.5 倍窗口仍然呈现 1280×720（由 `mapWindowPixelToImage` 的
    // image 实参体现；这里用 letterbox 的 image 尺寸间接验证）。
    const lift::LetterboxMap map = lift::letterboxFor(1920, 1080, lift::kBaseWindowW, lift::kBaseWindowH);
    ++lift_test::g_checks;
    if (!approxEq(map.scale, 1.5) || !approxEq(map.off_x, 0.0) || !approxEq(map.off_y, 0.0)) {
        lift_test::failf(__FILE__, __LINE__,
                         "1920x1080 窗口 + 1280x720 画面：expected scale=1.5 off=(0,0), got %.9f off=(%.9f,%.9f)",
                         map.scale, map.off_x, map.off_y);
    }
    // ★ 范围裁决（2026-11-18）：`--scale` **只改窗口，不改帧** ⇒ readout 必须自己说清
    //   "image 仍是 1280×720"，⛔ 不许让人读成"帧被重渲染成 1920×1080"。
    //   ★ 滤波器（2026-11-21）：同一行印 `filter=linear`（缺省）+ 一句放大口径（⛔ 不承诺"变清晰"）。
    {
        lift::ScaleReport scaled;
        scaled.scale = 1.5;
        scaled.window_w = 1920;
        scaled.window_h = 1080;
        const std::string line = lift::scaleReadoutLine(scaled);
        std::printf("[readout-1.5] %s\n", line.c_str());
        ++lift_test::g_checks;
        if (line !=
            "scale: 1.5（window 1920x1080，image 1280x720；filter=linear；"
            "放大：与真机 150% DPI 同档的平滑放大）") {
            lift_test::failf(__FILE__, __LINE__, "1.5 readout 不是期望的那一行：`%s`", line.c_str());
        }
    }
    // `--scale-filter nearest` 档：同一行换口径（硬边 / 锯齿），且⛔ 不冒充 linear 那句。
    {
        lift::ScaleReport scaled;
        scaled.scale = 1.5;
        scaled.window_w = 1920;
        scaled.window_h = 1080;
        scaled.filter = lift::ScaleFilter::Nearest;
        const std::string line = lift::scaleReadoutLine(scaled);
        std::printf("[readout-1.5-nearest] %s\n", line.c_str());
        ++lift_test::g_checks;
        if (line != "scale: 1.5（window 1920x1080，image 1280x720；filter=nearest；放大：硬边，非整数倍率有锯齿）") {
            lift_test::failf(__FILE__, __LINE__, "1.5/nearest readout 不是期望的那一行：`%s`", line.c_str());
        }
    }
    // 缩小档（s < 1）说"缩小"，⛔ 不冒充"放大"；恰好 1.0 不加这一句（缺省那一行已钉）。
    {
        lift::ScaleReport half;
        half.scale = 0.5;
        half.window_w = 640;
        half.window_h = 360;
        const std::string line = lift::scaleReadoutLine(half);
        std::printf("[readout-0.5] %s\n", line.c_str());
        ++lift_test::g_checks;
        if (line != "scale: 0.5（window 640x360，image 1280x720；filter=linear；缩小）") {
            lift_test::failf(__FILE__, __LINE__, "0.5 readout 不是期望的那一行：`%s`", line.c_str());
        }
    }
}

// ---- ③ 鼠标映射：窗口像素 → 画面像素（只走 cursor_map.h 的既有公式）----
void checkMap(int window_w, int window_h, double window_x, double window_y, int want_x, int want_y,
              const char* what) {
    int ix = 0;
    int iy = 0;
    const bool ok = lift::mapWindowPixelToImage(window_w, window_h, lift::kBaseWindowW, lift::kBaseWindowH, window_x,
                                                window_y, &ix, &iy);
    ++lift_test::g_checks;
    if (!ok || ix != want_x || iy != want_y) {
        lift_test::failf(__FILE__, __LINE__, "%s: window %dx%d pixel(%.1f,%.1f) expected image(%d,%d), got (%d,%d)",
                         what, window_w, window_h, window_x, window_y, want_x, want_y, ix, iy);
    }
}

void checkReferenceToClient(int client_w, int client_h, int ref_x, int ref_y, int want_x, int want_y,
                            const char* what) {
    int x = 0;
    int y = 0;
    const bool ok = lift::referenceToClientPixel(client_w, client_h, ref_x, ref_y, &x, &y);
    ++lift_test::g_checks;
    if (!ok || x != want_x || y != want_y) {
        lift_test::failf(__FILE__, __LINE__,
                         "%s: reference(%d,%d) in %dx%d expected client(%d,%d), got (%d,%d)", what, ref_x, ref_y,
                         client_w, client_h, want_x, want_y, x, y);
    }
}

void caseMouseMapping() {
    lift_test::g_current_case = "window-scale/mouse-mapping";

    // ---- 1.5 倍：窗口 1920×1080，画面 1280×720（letterbox scale=1.5、无黑边）----
    // 窗口像素 (1070,480) → 画面 (1070/1.5, 480/1.5) = (713.33, 320) ⇒ round (713,320)。
    checkMap(1920, 1080, 1070.0, 480.0, 713, 320, "1.5 倍窗口像素 → 画面像素");
    // ★ "同一条公式"：把画面点按 `control_endpoint.cpp` 判据 4 的 `referenceToClientPixel` 投回
    //   客户区，必须回到**同一个**窗口像素（两个既有公式在这条几何上互逆 ⇒ 没有第二套换算）。
    checkReferenceToClient(1920, 1080, 713, 320, 1070, 480, "1.5 倍：画面点回投客户区");
    // 顺带钉住 `control_endpoint.cpp` 判据 4 的既有读数（参考系 (1070,480) ⇒ 客户区 (1605,720)）：
    //   `--scale 1.5` 的窗口客户区就是那条用例里的 1920×1080，所以两个方向的数必须自洽。
    checkReferenceToClient(1920, 1080, 1070, 480, 1605, 720, "1.5 倍：参考系 (1070,480) ⇒ 客户区 (1605,720)");
    checkMap(1920, 1080, 0.0, 0.0, 0, 0, "1.5 倍：左上角");
    checkMap(1920, 1080, 1919.0, 1079.0, 1279, 719, "1.5 倍：右下角");

    // ---- 1.25 倍（**非整数倍率**）：窗口 1600×900，scale=1.25 精确 ----
    checkMap(1600, 900, 1070.0, 480.0, 856, 384, "1.25 倍窗口像素 → 画面像素");
    checkReferenceToClient(1600, 900, 856, 384, 1070, 480, "1.25 倍：画面点回投客户区");
    // 取整点：1070/1.25 = 856（整）；这里挑**会产生 .8 小数**的一对，钉住 lround（不是截断）：
    //   1001/1.25 = 800.8 → 801；601/1.25 = 480.8 → 481。
    checkMap(1600, 900, 1001.0, 601.0, 801, 481, "1.25 倍：小数取整（lround）");
    checkReferenceToClient(1600, 900, 801, 481, 1001, 601, "1.25 倍：取整点回投客户区");
    // 四角 + clamp（黑边里 / 窗口外落到最近的画面边缘是 `windowPixelToImage` 的既有口径）。
    checkMap(1600, 900, 0.0, 0.0, 0, 0, "1.25 倍：左上角");
    checkMap(1600, 900, 1599.0, 899.0, 1279, 719, "1.25 倍：右下角");
    checkMap(1600, 900, -50.0, 2000.0, 0, 719, "1.25 倍：越界 ⇒ clamp");

    // ---- 1.33 倍：窗口 1702×958（非 16:9 ⇒ letterbox 有纵向残差偏移）----
    //   scale = min(1702/1280, 958/720) = 1702/1280 = 1.3296875；off_x = 0；
    //   off_y = (958 − 720×1.3296875)/2 = (958 − 957.375)/2 = 0.3125。
    const lift::LetterboxMap map = lift::letterboxFor(1702, 958, lift::kBaseWindowW, lift::kBaseWindowH);
    ++lift_test::g_checks;
    if (!approxEq(map.scale, 1702.0 / 1280.0) || !approxEq(map.off_x, 0.0) || !approxEq(map.off_y, 0.3125, 1e-6)) {
        lift_test::failf(__FILE__, __LINE__, "1702x958 的 letterbox 期望 scale=%.9f off=(0,0.3125)，实得 %.9f (%.9f,%.9f)",
                         1702.0 / 1280.0, map.scale, map.off_x, map.off_y);
    }
    // 窗口 (0,2) 的**正确**画面 y = lround((2 − 0.3125)/1.3296875) = lround(1.269…) = 1。
    // ⛔ 丢掉 off_y 的实现会得到 lround(2/1.3296875) = lround(1.504…) = 2 ⇒ 这一格会红。
    checkMap(1702, 958, 0.0, 2.0, 0, 1, "1.33 倍：纵向残差偏移参与映射（⛔ 不是简单的 x/s）");
    checkMap(1702, 958, 1070.0, 480.0, 805, 361, "1.33 倍：中段像素");
    checkReferenceToClient(1702, 958, 805, 361, 1070, 480, "1.33 倍：画面点回投客户区（同一参考点）");
    checkMap(1702, 958, 1701.0, 957.0, 1279, 719, "1.33 倍：右下角 ⇒ clamp 到 image-1");
}

// ---- ④ `auto`：DPI content scale → 缩放（纯函数；取不到 ⇒ 1.0 且必须打出来）----
void caseAutoDpi() {
    lift_test::g_current_case = "window-scale/auto-dpi";
    // 可用判据：唯一一份。
    LIFT_CHECK(lift::autoContentScaleUsable(1.0));
    LIFT_CHECK(lift::autoContentScaleUsable(1.25));
    LIFT_CHECK(lift::autoContentScaleUsable(lift::kMaxAutoScale));
    LIFT_CHECK(!lift::autoContentScaleUsable(0.0));   // SDL 的失败返回值
    LIFT_CHECK(!lift::autoContentScaleUsable(-1.25));  // 负
    LIFT_CHECK(!lift::autoContentScaleUsable(std::nan("")));
    LIFT_CHECK(!lift::autoContentScaleUsable(INFINITY));
    LIFT_CHECK(!lift::autoContentScaleUsable(lift::kMaxAutoScale * 2.0));  // 不是 DPI（平台给了别的东西）

    // 常见 DPI 档：1.0 / 1.25 / 1.5 / 1.75 / 2.0 原样。
    struct DpiCase {
        double content_scale;
        double want;
    };
    const DpiCase kCases[] = {{1.0, 1.0}, {1.25, 1.25}, {1.5, 1.5}, {1.75, 1.75}, {2.0, 2.0}};
    for (const DpiCase& c : kCases) {
        const double got = lift::autoScaleFromContentScale(c.content_scale);
        ++lift_test::g_checks;
        if (!approxEq(got, c.want)) {
            lift_test::failf(__FILE__, __LINE__, "DPI %.2f expected scale %.4f, got %.9f", c.content_scale, c.want,
                             got);
        }
    }
    // float 噪声：1.2499999 量化到 1.25（readout 印的数 == 拿去算窗口尺寸的数）。
    LIFT_CHECK(approxEq(lift::autoScaleFromContentScale(1.2499999), 1.25));
    LIFT_CHECK(approxEq(lift::autoScaleFromContentScale(1.5000001), 1.5));
    // 取不到（0 / 负数 / NaN / Inf / 离谱）⇒ **1.0**（唯一决定点）。
    const double kInf = std::numeric_limits<double>::infinity();
    for (const double bad : {0.0, -1.5, std::nan(""), kInf, 9.0}) {
        LIFT_CHECK(approxEq(lift::autoScaleFromContentScale(bad), 1.0));
    }
    // 量化后的值真的能建出整数窗口（不是"印 1.25、用 1.2499999"）。
    expectSize(lift::autoScaleFromContentScale(1.2499999), 1600, 900, "auto 1.25 ⇒ 1600×900");

    // readout：auto 成功 ⇒ 印出原始 content scale；取不到 ⇒ 写明"SDL 返回 0.0 ⇒ 回 1.0"（⛔ 不静默）。
    lift::ScaleReport ok_report;
    ok_report.auto_requested = true;
    ok_report.auto_content_scale = 1.25;
    ok_report.scale = lift::autoScaleFromContentScale(1.25);
    const lift::WindowSize ok_size = lift::windowSizeForScale(ok_report.scale);
    ok_report.window_w = ok_size.w;
    ok_report.window_h = ok_size.h;
    const std::string ok_line = lift::scaleReadoutLine(ok_report);
    std::printf("[readout-auto-ok] %s\n", ok_line.c_str());
    LIFT_CHECK(ok_line ==
               "scale: 1.25（window 1600x900，image 1280x720；filter=linear；"
               "放大：与真机 150% DPI 同档的平滑放大；"
               "auto ← display content scale 1.25）");

    lift::ScaleReport fb_report;
    fb_report.auto_requested = true;
    fb_report.auto_fallback = true;
    fb_report.auto_content_scale = 0.0;
    fb_report.scale = lift::autoScaleFromContentScale(0.0);
    const lift::WindowSize fb_size = lift::windowSizeForScale(fb_report.scale);
    fb_report.window_w = fb_size.w;
    fb_report.window_h = fb_size.h;
    const std::string fb_line = lift::scaleReadoutLine(fb_report);
    std::printf("[readout-auto-fallback] %s\n", fb_line.c_str());
    LIFT_CHECK(fb_line ==
               "scale: 1.0（window 1280x720，image 1280x720；filter=linear；"
               "auto 取不到 display content scale（SDL 返回 0.0）"
               "⇒ 回 1.0）");
}

// ---- ⑤ `--scale` 的解析：auto / 正数接受，其余拒绝 ----
void caseParse() {
    lift_test::g_current_case = "window-scale/parse";
    // 接受：auto（大小写敏感，与 --audio 同口径）。
    {
        const lift::ScaleParse p = lift::parseScaleSpec("auto");
        LIFT_CHECK(p.ok);
        LIFT_CHECK(p.spec.kind == lift::ScaleSpec::Kind::Auto);
    }
    struct Good {
        const char* text;
        double want;
    };
    const Good kGood[] = {{"1", 1.0}, {"1.0", 1.0}, {"1.5", 1.5}, {"1.25", 1.25}, {"0.5", 0.5}, {"2", 2.0}};
    for (const Good& g : kGood) {
        const lift::ScaleParse p = lift::parseScaleSpec(g.text);
        ++lift_test::g_checks;
        if (!p.ok || p.spec.kind != lift::ScaleSpec::Kind::Fixed || !approxEq(p.spec.value, g.want)) {
            lift_test::failf(__FILE__, __LINE__, "`%s` 应被接受为 Fixed %.4f（ok=%d kind=%d value=%.9f）", g.text,
                             g.want, p.ok ? 1 : 0, static_cast<int>(p.spec.kind), p.spec.value);
        }
    }
    // 拒绝：0 / 负数 / NaN / Inf / 垃圾 / 尾随字符 / 空串 / 大小写错的 AUTO。
    const char* const kBads[] = {"0",     "0.0",   "-1",   "-1.5", "nan",  "NaN",   "inf",   "-inf",
                                 "abc",   "1.5x",  "x1.5", "1,5",  "",     "AUTO",  "auto ", " "};
    for (const char* bad : kBads) {
        const lift::ScaleParse p = lift::parseScaleSpec(bad);
        ++lift_test::g_checks;
        if (p.ok) {
            lift_test::failf(__FILE__, __LINE__, "`%s` 应被拒绝（parseScaleSpec 却 ok=true, value=%.9f）", bad,
                             p.spec.value);
        }
    }
    // 空指针不崩（CLI 的 needValue 已经挡住"缺值"，这里是防御面）。
    LIFT_CHECK(!lift::parseScaleSpec(nullptr).ok);
}

// ---- ⑥ headless：接受 --scale 但不生效（readout 明说；没有 window 字段）----
void caseHeadlessReadout() {
    lift_test::g_current_case = "window-scale/headless-readout";
    lift::ScaleReport fixed;
    fixed.applied = false;
    fixed.scale = 1.5;
    const std::string fixed_line = lift::scaleReadoutLine(fixed);
    std::printf("[readout-headless] %s\n", fixed_line.c_str());
    LIFT_CHECK(fixed_line == "scale: 1.5（--present headless ⇒ 无窗口可施，不生效；image 1280x720）");
    LIFT_CHECK(fixed_line.find("window ") == std::string::npos);

    lift::ScaleReport auto_mode;
    auto_mode.applied = false;
    auto_mode.auto_requested = true;
    const std::string auto_line = lift::scaleReadoutLine(auto_mode);
    std::printf("[readout-headless-auto] %s\n", auto_line.c_str());
    LIFT_CHECK(auto_line == "scale: auto（--present headless ⇒ 无窗口可施，不生效；image 1280x720）");
}

// ---- ⑤' `--scale-filter` 的解析：linear / nearest 接受，其余拒绝（口径照 `--scale`）----
void caseParseFilter() {
    lift_test::g_current_case = "window-scale/parse-filter";
    // 接受：两个值，大小写敏感、整串精确匹配。
    {
        const lift::ScaleFilterParse p = lift::parseScaleFilterSpec("linear");
        LIFT_CHECK(p.ok);
        LIFT_CHECK(p.filter == lift::ScaleFilter::Linear);
        LIFT_CHECK(std::string(lift::scaleFilterName(p.filter)) == "linear");
    }
    {
        const lift::ScaleFilterParse p = lift::parseScaleFilterSpec("nearest");
        LIFT_CHECK(p.ok);
        LIFT_CHECK(p.filter == lift::ScaleFilter::Nearest);
        LIFT_CHECK(std::string(lift::scaleFilterName(p.filter)) == "nearest");
    }
    // 拒绝：大小写不符 / 尾随空格 / 前缀 / 数值 / 空串 / 空指针（CLI 侧 ⇒ exit 2 + 点名
    // `--scale-filter`）。★ 不做"认不出就当 linear"的放宽：那会让 nearest 对照静默变成 linear。
    const char* const kBads[] = {"LINEAR", "Linear", "NEAREST", "Nearest", "linear ", " nearest",
                                 "lin",    "near",    "0",       "1",      "auto",     "1.5", ""};
    for (const char* bad : kBads) {
        const lift::ScaleFilterParse p = lift::parseScaleFilterSpec(bad);
        ++lift_test::g_checks;
        if (p.ok) {
            lift_test::failf(__FILE__, __LINE__, "`%s` 应被拒绝（parseScaleFilterSpec 却 ok=true, filter=%s）", bad,
                             lift::scaleFilterName(p.filter));
        }
    }
    LIFT_CHECK(!lift::parseScaleFilterSpec(nullptr).ok);
    // 拒绝原因必须**点名合法值域**（CLI 原样拼进报错 ⇒ 人照它改）。
    {
        const lift::ScaleFilterParse p = lift::parseScaleFilterSpec("blur");
        ++lift_test::g_checks;
        const std::string why = p.error != nullptr ? p.error : "";
        if (why.find("nearest") == std::string::npos || why.find("linear") == std::string::npos) {
            lift_test::failf(__FILE__, __LINE__, "拒绝原因没点名值域（nearest / linear）：`%s`", why.c_str());
        }
    }
}

}  // namespace

int main() {
    caseDefaultUnchanged();
    caseFixedScales();
    caseMouseMapping();
    caseAutoDpi();
    caseParse();
    caseParseFilter();
    caseHeadlessReadout();
    return lift_test::finish("window-scale");
}
