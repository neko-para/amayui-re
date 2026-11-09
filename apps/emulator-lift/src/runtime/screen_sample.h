// emulator-lift · 画面特征采样（交互模型 `screen` 源的**唯一**采样点）
//
// ★ 为什么单列一份：交互模型（`docs/03-emulator/interaction-model.md`）的判定必须是
//   **观测值的纯函数** —— 采样发生在哪一刻、采样到什么值，与"什么时候判定"分开。
//   这里只做采样与记账；判定在 `runtime/interaction_graph.h`。⛔ 不要把两者混在一起：
//   混起来以后"同一份画面 ⇒ 同一串判定"就只能靠测试碰运气。
// ★ 采样点 = **Present**（唯一的呈现汇点：D3D9 Present 与 DDraw Flip 都走
//   `HeadlessPresenter::present()`；`arena.hash(surface)` 也是在那里算的）。
//   ⇒ 采样值 = `{整帧 hash, 每个受监视矩形的均值 + 区域 hash}`，全部是确定性量。
// ★ 只在**有消费者**时才挂上：`HeadlessPresenter` 没有 sampler 就一次也不读像素
//   （矩形均值要逐像素解码，不是免费的）。
#pragma once

#include <cstdint>
#include <vector>

#include "runtime/surface.h"

namespace lift {

// 画面上的一个矩形（guest 像素坐标；左上角原点，y 向下）。
struct PixelRect {
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::int32_t w = 0;
    std::int32_t h = 0;

    bool valid() const { return w > 0 && h > 0; }
    bool operator==(const PixelRect& o) const { return x == o.x && y == o.y && w == o.w && h == o.h; }
};

// 一个矩形的采样值。`avg_rgb` = 各通道**算术均值**（整数除法截断；0xRRGGBB）。
// `hash` = 该矩形内**解码后的 RGB** 的 FNV-1a（16bpp / 32bpp 同内容同值 ⇒ 跨格式可比）。
struct RectSample {
    bool valid = false;
    std::uint32_t avg_rgb = 0;
    std::uint64_t hash = 0;
};

// 最近一次采样的整帧量。
struct FrameSample {
    bool valid = false;
    std::uint64_t frame = 0;   // Present 计数（1 起；取自 `HeadlessPresenter::frameCount()`）
    std::uint64_t hash = 0;    // 整帧 hash（= `SurfaceArena::hash` = `HeadlessPresenter::lastHash()`）
    bool changed = false;      // 与**上一次**采样不同（第一次采样 = false）
};

// 单次采样里"一个矩形命中"的判据：均值每通道容差内即命中。
// ★ 为什么要容差：均值是整数截断的结果，引擎的淡入 / 抗锯齿会让它差 1~2；
//   要求逐位相等等于要求"画面一个像素都不许动"，那不叫识别。
constexpr std::uint32_t kRectAvgTolerance = 8;

class FrameSampler {
public:
    // 登记一个矩形（同一矩形去重；返回索引）。解析期一次性登记，之后不再增删。
    std::size_t watchRect(const PixelRect& r);
    // 这张图**用不用**整帧 hash（`screen hash` / `screen changed` 节点）。★ 它只是记账：
    // 整帧 hash 由 `HeadlessPresenter::present` 无条件算（帧日志 / A4 判据也要它），
    // 所以这个开关不改变采样成本，⛔ 不要把它当"打不打开"。
    void setWatchFrame(bool on) { watch_frame_ = on; }
    bool watchFrame() const { return watch_frame_; }
    bool watchesAnything() const { return watch_frame_ || !rects_.empty(); }
    std::size_t rectCount() const { return rects_.size(); }
    const PixelRect& rectAt(std::size_t i) const { return rects_[i]; }

    // presenter 每次 present 之后调用（主线程；⛔ 不抛异常）。
    //   像素读不到（未提交 / 非 16·32bpp）⇒ 该矩形 `valid=false`（**不**记为一次有效采样）。
    void observe(const SurfaceArena& arena, const GuestSurface& s, std::uint64_t frame, std::uint64_t hash);

    // 测试 / 非像素源：直接注入一次"合成采样"（不读像素）。⛔ 生产路径不调用。
    //   `rects` 必须与登记顺序一致或为空；空 ⇒ 已登记的矩形全部 `valid=false`。
    void observeValues(std::uint64_t frame, std::uint64_t hash, const std::vector<RectSample>& rects);

    // ★ 两个单调计数（判定只读它们 ⇒ 判定与采样节奏解耦）：
    //   `sampleCount()` = 至今采到多少次（= Present 次数）；`changeCount()` = 其中多少次
    //   "与上一次采样不同"。都没有回退路径。
    std::uint64_t sampleCount() const { return samples_; }
    std::uint64_t changeCount() const { return changes_; }
    const FrameSample& last() const { return last_; }
    const std::vector<RectSample>& rectValues() const { return rect_values_; }
    // 采样总数里有多少次**读不到像素**（矩形全部无效）—— 诊断用（"源接上了但读不到"）。
    std::uint64_t blindSamples() const { return blind_samples_; }
    void reset();

private:
    std::vector<PixelRect> rects_;
    std::vector<RectSample> rect_values_;
    bool watch_frame_ = false;
    std::uint64_t samples_ = 0;
    std::uint64_t changes_ = 0;
    std::uint64_t blind_samples_ = 0;
    bool have_last_ = false;
    FrameSample last_;
};

}  // namespace lift
