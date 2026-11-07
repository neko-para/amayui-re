// emulator-lift · 影片音频渲染器（宿主侧；对应 quartz 图里那条音频支路的输出端）
//
// ★ 为什么同步机制在这里：原版把整个 MPEG-PS 当 `MEDIATYPE_Stream` 交给 quartz，
//   `IGraphBuilder::Render` 建出「splitter→音频解码→默认音频渲染器」那条支路。
//   **我们就是 quartz** ⇒ "什么时候把 PCM 交给设备" 是图（graph）的职责，
//   与视频渲染器的 `IReferenceClock` 同一份时钟（`cpu.clock`）。
//   ⛔ 不用墙钟、⛔ 不在解码器里 sleep：时间只来自 `now()`（graph 的 `cpu.clock`）。
//
// ★ 同步策略（与真实 DirectShow 音频渲染器同形，简化版）：
//   - 解码出的 PCM 带**流时间** pts（相对影片开头）；
//   - 每次 `pump(流now)` 把 `pts <= 流now + kLead` 的块交给 sink（设备队列）；
//   - `pts < 流now - kLate` 的块**直接丢**（设备已放过去、追不上；计数）；
//   - 队列空而时间仍在走 ⇒ 记一次 underrun（设备放静音），下次有数据立刻续上；
//   - 音量 = `IBasicAudio::put_Volume` 的 dB（-100 dB ⇒ 静音），在交给 sink 前乘进样本。
//
// ★ 输出格式：统一 **48 kHz / 16-bit / 立体声**（`AudioEngine` 的设备格式）；
//   非 48k 源在这里做线性重采样，单声道复制到双声道。
#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <vector>

#include "runtime/dshow/dshow.h"

namespace lift::dshow {

class AudioRenderer {
  public:
    // `now` = 图时钟（与视频同一份）；`sink` = 宿主音频设备（48k/S16/2ch）。
    using NowFn = std::function<std::int64_t()>;
    using SinkFn = std::function<void(const std::int16_t*, std::uint32_t)>;

    void bind(NowFn now, SinkFn sink);
    void unbind();

    // 控制（由 Graph 的 Run/Pause/Stop/seek 驱动）。
    void begin(std::int64_t stream_start_100ns);  // Run：流 0 对应的时钟
    void pause();
    void stop();    // 清队列 + 记一次汇总
    void rewind();  // 循环 / SetPosition(0)：清队列，等下一段

    void setVolumeDb(double db) { volume_db_ = db; }
    double volumeDb() const { return volume_db_; }

    // 解码层送进来的块（pts 必须递增；乱序 / 重复的块会被丢）。
    void feed(AudioChunk&& chunk);
    // 块边界的泵：按流时间把到期的块交给设备。
    void pump(std::int64_t stream_now_100ns);

    // 诊断。
    std::uint64_t fedFrames() const { return fed_frames_; }
    std::uint64_t pushedFrames() const { return pushed_frames_; }
    std::uint64_t droppedChunks() const { return dropped_chunks_; }
    std::uint64_t underruns() const { return underruns_; }
    std::size_t queueChunks() const { return queue_.size(); }
    std::string summary() const;

    // 前导 / 迟到阈值（100ns）。设备侧还有自己的队列，值取保守。
    static constexpr std::int64_t kLead100ns = 2'000'000;  // 200 ms
    static constexpr std::int64_t kLate100ns = 3'000'000;  // 300 ms
    static constexpr std::uint32_t kOutRate = 48000;

  private:
    void pushToSink(const AudioChunk& chunk);
    static std::vector<std::int16_t> resample(const AudioChunk& chunk, double gain);

    NowFn now_;
    SinkFn sink_;
    std::deque<AudioChunk> queue_;
    std::int64_t start_100ns_ = 0;
    std::int64_t next_pts_100ns_ = 0;  // 已交给设备的最后一块的结束时刻
    bool running_ = false;
    bool started_ = false;
    double volume_db_ = 0.0;

    std::uint64_t fed_frames_ = 0;
    std::uint64_t pushed_frames_ = 0;
    std::uint64_t dropped_chunks_ = 0;
    std::uint64_t underruns_ = 0;
};

}  // namespace lift::dshow
