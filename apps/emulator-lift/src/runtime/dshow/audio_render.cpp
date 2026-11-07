// emulator-lift · 影片音频渲染器的实现（见 audio_render.h 的口径）
#include "runtime/dshow/audio_render.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "runtime/dshow/dshow.h"

namespace lift::dshow {
namespace {

constexpr std::int64_t kTicks = 10'000'000;  // 1 秒 = 1e7 × 100ns

std::int64_t durationOf(const AudioChunk& c) {
    if (c.samplerate == 0) return 0;
    return static_cast<std::int64_t>(c.pcm.size() / std::max<std::uint32_t>(c.channels, 1u)) * kTicks /
           static_cast<std::int64_t>(c.samplerate);
}

double gainOf(double db) {
    if (db <= -99.0) return 0.0;  // -100 dB = 静音（put_Volume 的"关"）
    return std::pow(10.0, db / 20.0);
}

}  // namespace

void AudioRenderer::bind(NowFn now, SinkFn sink) {
    now_ = std::move(now);
    sink_ = std::move(sink);
}

void AudioRenderer::unbind() {
    now_ = nullptr;
    sink_ = nullptr;
}

void AudioRenderer::begin(std::int64_t stream_start_100ns) {
    start_100ns_ = stream_start_100ns;
    running_ = true;
    started_ = true;
    if (next_pts_100ns_ < 0) next_pts_100ns_ = 0;
}

void AudioRenderer::pause() { running_ = false; }

void AudioRenderer::stop() {
    running_ = false;
    queue_.clear();
    next_pts_100ns_ = 0;
    dshowLog(LogLevel::Info, "音频渲染器：Stop（%s）", summary().c_str());
}

void AudioRenderer::rewind() {
    // 循环 / SetPosition(0)：队列清空，下一段从流 0 重新排。
    queue_.clear();
    next_pts_100ns_ = 0;
}

void AudioRenderer::feed(AudioChunk&& chunk) {
    if (chunk.pcm.empty() || chunk.samplerate == 0 || chunk.channels == 0) return;
    const std::int64_t dur = durationOf(chunk);
    // 已完全落在"已交给设备"的区间之前 ⇒ 丢（解码追不上时的正常现象）。
    if (chunk.pts_100ns + dur <= next_pts_100ns_) {
        ++dropped_chunks_;
        return;
    }
    fed_frames_ += static_cast<std::uint64_t>(chunk.pcm.size() / chunk.channels);
    queue_.push_back(std::move(chunk));
}

void AudioRenderer::pump(std::int64_t stream_now_100ns) {
    if (!running_ || !sink_) return;
    const std::int64_t lead = stream_now_100ns + kLead100ns;
    while (!queue_.empty()) {
        const AudioChunk& c = queue_.front();
        const std::int64_t dur = durationOf(c);
        if (c.pts_100ns + dur < stream_now_100ns - kLate100ns) {
            // 迟到：设备已经放过去了；丢掉并计数。
            ++dropped_chunks_;
            queue_.pop_front();
            continue;
        }
        if (c.pts_100ns > lead) break;
        pushToSink(c);
        next_pts_100ns_ = c.pts_100ns + dur;
        queue_.pop_front();
    }
    // underrun：队列空、且时间已经越过"下一段该到的位置"超过 100 ms。
    if (started_ && queue_.empty() && next_pts_100ns_ < stream_now_100ns - kTicks / 10) {
        ++underruns_;
        next_pts_100ns_ = stream_now_100ns;  // 不重复记同一段空档
    }
}

void AudioRenderer::pushToSink(const AudioChunk& chunk) {
    const std::vector<std::int16_t> out = resample(chunk, gainOf(volume_db_));
    if (out.empty()) return;
    const std::uint32_t frames = static_cast<std::uint32_t>(out.size() / 2u);
    sink_(out.data(), frames);
    pushed_frames_ += frames;
}

std::vector<std::int16_t> AudioRenderer::resample(const AudioChunk& chunk, double gain) {
    const double ratio = static_cast<double>(chunk.samplerate) / static_cast<double>(kOutRate);
    const std::uint32_t in_frames = static_cast<std::uint32_t>(chunk.pcm.size() / chunk.channels);
    if (in_frames == 0 || !(ratio > 0.0)) return {};
    const std::uint32_t out_frames =
        static_cast<std::uint32_t>(std::llround(static_cast<double>(in_frames) / ratio));
    std::vector<std::int16_t> out(static_cast<std::size_t>(out_frames) * 2u);
    auto sampleAt = [&](std::uint32_t frame, std::uint32_t ch) -> double {
        if (chunk.channels == 1) ch = 0;
        if (frame >= in_frames) frame = in_frames - 1u;
        return static_cast<double>(chunk.pcm[static_cast<std::size_t>(frame) * chunk.channels + ch]);
    };
    for (std::uint32_t j = 0; j < out_frames; ++j) {
        const double pos = static_cast<double>(j) * ratio;
        const std::uint32_t i0 = static_cast<std::uint32_t>(pos);
        const double frac = pos - static_cast<double>(i0);
        const std::uint32_t i1 = std::min(i0 + 1u, in_frames - 1u);
        for (std::uint32_t ch = 0; ch < 2u; ++ch) {
            const double a = sampleAt(i0, ch);
            const double b = sampleAt(i1, ch);
            double v = (a + (b - a) * frac) * gain;
            v = std::clamp(v, -32768.0, 32767.0);
            out[static_cast<std::size_t>(j) * 2u + ch] = static_cast<std::int16_t>(std::llround(v));
        }
    }
    return out;
}

std::string AudioRenderer::summary() const {
    char buf[256];
    std::snprintf(buf, sizeof(buf), "fed=%llu frames, pushed=%llu frames, dropped_chunks=%llu, underruns=%llu",
                  static_cast<unsigned long long>(fed_frames_), static_cast<unsigned long long>(pushed_frames_),
                  static_cast<unsigned long long>(dropped_chunks_),
                  static_cast<unsigned long long>(underruns_));
    return std::string(buf);
}

}  // namespace lift::dshow
