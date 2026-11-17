#include "runtime/window_scale.h"

#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace lift {
namespace {

// `round` 在 C++ 是"远离 0 取整"（与 `lround` 同口径）；本文件只处理正数。
double roundHalfAway(double v) { return v < 0.0 ? -std::floor(-v + 0.5) : std::floor(v + 0.5); }

// 数值 → readout 文本：`%.4f` 之后去掉尾随 0，但**至少留一位小数**（1.5000 ⇒ "1.5"，1.0000 ⇒ "1.0"）。
// 非有限值不猜（打印成"非有限值"）—— 它是诊断面，⛔ 不许伪装成一个数。
std::string formatScale(double scale) {
    if (!std::isfinite(scale)) return "非有限值";
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.4f", scale);
    std::string s(buf);
    while (s.size() > 2 && s.back() == '0' && s[s.size() - 2] != '.') s.pop_back();
    return s;
}

}  // namespace

ScaleParse parseScaleSpec(const char* text) {
    ScaleParse r;
    if (text == nullptr || text[0] == '\0') {
        r.error = "需要一个值（只能是 auto 或正数，例：--scale 1.5）";
        return r;
    }
    if (std::strcmp(text, "auto") == 0) {
        r.ok = true;
        r.spec.kind = ScaleSpec::Kind::Auto;
        r.spec.value = kDefaultScale;
        return r;
    }
    // ★ 只认"整串都是数字"：`strtod` 的 `end` 必须落在 `\0` 上 ⇒ `1.5x` / `1.5 `（尾随空格）都拒。
    //   `strtod` 会接受 "nan" / "inf"（它们是合法的 double 字面量）⇒ 必须显式用 `isfinite` 挡掉。
    char* end = nullptr;
    const double v = std::strtod(text, &end);
    if (end == text || end == nullptr || *end != '\0' || !std::isfinite(v) || v <= 0.0) {
        r.error = "只能是 auto 或正数（例：--scale 1.5）";
        return r;
    }
    r.ok = true;
    r.spec.kind = ScaleSpec::Kind::Fixed;
    r.spec.value = v;
    return r;
}

int scaleWindowDimension(int base, double scale) {
    if (base <= 0) return 0;
    if (!std::isfinite(scale) || scale <= 0.0) return base;  // 防御：调用方已校验
    const double v = roundHalfAway(static_cast<double>(base) * scale);
    if (v < 1.0) return 1;  // ⛔ 不建 0 宽的窗口
    // 超大值不在这里"猜一个合理上界"：转成 int 会 UB，钳到 INT_MAX 让 SDL 自己响亮地建窗口失败。
    if (v >= static_cast<double>(INT_MAX)) return INT_MAX;
    return static_cast<int>(v);
}

WindowSize windowSizeForScale(double scale) {
    WindowSize size;
    size.w = scaleWindowDimension(kBaseWindowW, scale);
    size.h = scaleWindowDimension(kBaseWindowH, scale);
    return size;
}

double quantizeScale(double scale) {
    if (!std::isfinite(scale) || scale <= 0.0) return kDefaultScale;
    const double q = roundHalfAway(scale * kScaleQuantum) / kScaleQuantum;
    return q > 0.0 && std::isfinite(q) ? q : kDefaultScale;
}

bool autoContentScaleUsable(double content_scale) {
    // `> 0` 已经挡掉 0（SDL 的失败返回值）与负数；`isfinite` 挡 NaN / Inf；上界见 kMaxAutoScale。
    return std::isfinite(content_scale) && content_scale > 0.0 && content_scale <= kMaxAutoScale;
}

double autoScaleFromContentScale(double content_scale) {
    if (!autoContentScaleUsable(content_scale)) return kDefaultScale;  // ★ "取不到就回 1.0" 的唯一决定
    return quantizeScale(content_scale);
}

std::string scaleReadoutLine(const ScaleReport& report) {
    std::string out = "scale: ";
    // ---- headless：接受 --scale 但**没有窗口可施**（⛔ 不静默：这一行就是那条 readout）----
    if (!report.applied) {
        out += report.auto_requested ? std::string("auto") : formatScale(report.scale);
        out += "（--present headless ⇒ 无窗口可施，不生效；image ";
        out += std::to_string(report.image_w) + "x" + std::to_string(report.image_h) + "）";
        return out;
    }
    out += formatScale(report.scale);
    out += "（window " + std::to_string(report.window_w) + "x" + std::to_string(report.window_h) + "，image " +
           std::to_string(report.image_w) + "x" + std::to_string(report.image_h);
    // ★ 范围裁决（2026-11-18）：`--scale` **只改窗口，不改帧**。窗口比画面大时，画面是**放大**出来的
    //   （模糊可接受：真机也没做特殊处理；⛔ 不做高分辨率重渲染 / 不改帧尺寸 / 不加 upscale pass）。
    //   这一句让 readout 自己说清"image 仍是 1280×720"，而不是让人以为帧被重渲染成 1920×1080。
    //   scale < 1 时说"缩小"（⛔ 不冒充"放大"）；恰好 1.0 不加这一句（1:1）。
    if (report.scale > 1.0 + 1e-9) {
        out += "；放大，接受模糊";
    } else if (report.scale < 1.0 - 1e-9) {
        out += "；缩小";
    }
    if (report.auto_requested) {
        if (report.auto_fallback) {
            // ★ 取不到 ⇒ 一行一次性 readout 说明**为什么**回 1.0（⛔ 不许静默退回缺省）。
            out += "；auto 取不到 display content scale（SDL 返回 " + formatScale(report.auto_content_scale) +
                   "）⇒ 回 1.0";
        } else {
            out += "；auto ← display content scale " + formatScale(report.auto_content_scale);
        }
    }
    out += "）";
    return out;
}

}  // namespace lift
