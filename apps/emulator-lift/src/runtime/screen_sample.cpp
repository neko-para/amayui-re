#include "runtime/screen_sample.h"

#include "runtime/pixel_format.h"

namespace lift {
namespace {

// 一个矩形的采样：均值（截断）+ 解码后 RGB 的 FNV-1a。
// ★ 越界部分**夹到 surface 边界**（识别框常常贴着边，不该因为没有 1 像素余量而失败）；
//   夹完为空 ⇒ 无效采样。
RectSample sampleRect(const SurfaceArena& arena, const GuestSurface& s, const PixelRect& r) {
    RectSample out;
    if (!r.valid()) return out;
    const unsigned bpp = s.format.bpp;
    if (bpp != 16 && bpp != 32) return out;
    const std::uint8_t* base = arena.pixels(s);
    if (base == nullptr) return out;

    std::int32_t x0 = r.x < 0 ? 0 : r.x;
    std::int32_t y0 = r.y < 0 ? 0 : r.y;
    std::int32_t x1 = r.x + r.w;
    std::int32_t y1 = r.y + r.h;
    if (x1 > s.width) x1 = s.width;
    if (y1 > s.height) y1 = s.height;
    if (x1 <= x0 || y1 <= y0) return out;

    const unsigned bytes = bpp / 8u;
    std::uint64_t rsum = 0;
    std::uint64_t gsum = 0;
    std::uint64_t bsum = 0;
    std::uint64_t count = 0;
    std::uint64_t h = 0xCBF29CE484222325ull;
    for (std::int32_t y = y0; y < y1; ++y) {
        const std::uint8_t* row = base + static_cast<std::size_t>(y) * static_cast<std::size_t>(s.pitch);
        for (std::int32_t x = x0; x < x1; ++x) {
            const std::uint32_t v = pixel_format::loadPixel(
                row + static_cast<std::size_t>(x) * static_cast<std::size_t>(bytes), bpp);
            const unsigned rr = pixel_format::expandChannel(v, s.format.rmask);
            const unsigned gg = pixel_format::expandChannel(v, s.format.gmask);
            const unsigned bb = pixel_format::expandChannel(v, s.format.bmask);
            rsum += rr;
            gsum += gg;
            bsum += bb;
            ++count;
            h = (h ^ (((rr & 0xFFu) << 16) | ((gg & 0xFFu) << 8) | (bb & 0xFFu))) * 0x100000001B3ull;
            h ^= h >> 29;
        }
    }
    if (count == 0) return out;
    out.valid = true;
    const std::uint32_t ar = static_cast<std::uint32_t>(rsum / count);
    const std::uint32_t ag = static_cast<std::uint32_t>(gsum / count);
    const std::uint32_t ab = static_cast<std::uint32_t>(bsum / count);
    out.avg_rgb = ((ar & 0xFFu) << 16) | ((ag & 0xFFu) << 8) | (ab & 0xFFu);
    h ^= h >> 33;
    h *= 0xFF51AFD7ED558CCDull;
    h ^= h >> 33;
    out.hash = h;
    return out;
}

}  // namespace

std::size_t FrameSampler::watchRect(const PixelRect& r) {
    for (std::size_t i = 0; i < rects_.size(); ++i) {
        if (rects_[i] == r) return i;
    }
    rects_.push_back(r);
    rect_values_.resize(rects_.size());
    return rects_.size() - 1;
}

void FrameSampler::observe(const SurfaceArena& arena, const GuestSurface& s, std::uint64_t frame,
                           std::uint64_t hash) {
    std::vector<RectSample> vals(rects_.size());
    for (std::size_t i = 0; i < rects_.size(); ++i) vals[i] = sampleRect(arena, s, rects_[i]);
    observeValues(frame, hash, vals);
}

void FrameSampler::observeValues(std::uint64_t frame, std::uint64_t hash,
                                 const std::vector<RectSample>& rects) {
    // 一次采样 = 一个观测点。⛔ 采样计数只在这里增长（判定侧只读 `sampleCount()`）。
    const bool changed = have_last_ && hash != last_.hash;
    last_.valid = true;
    last_.frame = frame;
    last_.hash = hash;
    last_.changed = changed;
    have_last_ = true;
    ++samples_;
    if (changed) ++changes_;
    if (rects.empty()) {
        // 合成采样没给矩形值 ⇒ 已登记矩形的值全部失效（⛔ 不保留上一次的值：
        // 那会让"源失明"看起来像"值没变"）。
        rect_values_.assign(rects_.size(), RectSample{});
        // ★ **没有登记任何矩形时不存在"盲"**：只监视整帧 hash 的图里，每一次采样都会走到这里，
        //   无条件累加会让 `blindSamples()` 等于总采样数 ⇒ 报告里的 `blind=` 全是误导、
        //   `no-sample` 判据也会把"画面不对"误报成"识别源失明"（实测踩过）。
        if (!rects_.empty()) ++blind_samples_;
        return;
    }
    rect_values_.assign(rects_.size(), RectSample{});
    const std::size_t n = rects.size() < rect_values_.size() ? rects.size() : rect_values_.size();
    bool any_valid = false;
    for (std::size_t i = 0; i < n; ++i) {
        rect_values_[i] = rects[i];
        if (rects[i].valid) any_valid = true;
    }
    if (!rects_.empty() && !any_valid) ++blind_samples_;
}

void FrameSampler::reset() {
    rects_.clear();
    rect_values_.clear();
    watch_frame_ = false;
    samples_ = 0;
    changes_ = 0;
    blind_samples_ = 0;
    have_last_ = false;
    last_ = FrameSample{};
}

}  // namespace lift
