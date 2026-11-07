// emulator-lift · **pure**：影片音频的同步机制（`dshow::AudioRenderer`）
//
// 判据（红了意味着什么）：
//   ① `begin/pump` 的"前导"语义：`pts <= 流now + 200ms` 才交给设备，更远的块留在队列里；
//   ② 迟到块（`pts + 时长 < 流now - 300ms`）**被丢**并计数（追不上就别硬塞）；
//   ③ 队列空而时间在走 ⇒ 记 underrun（设备放静音），不假装成功；
//   ④ 音量：`-100 dB` ⇒ 静音；`0 dB` ⇒ 原样（重采样后不失真到 0）；
//   ⑤ 重采样：44.1k → 48k，输出帧数 ≈ 输入帧数 × 48000/44100；单声道复制成双声道。
// ★ 这里是**时间语义**的守卫，不碰真影片 / 不碰 SDL / 不碰 guest。
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "check.h"
#include "runtime/dshow/audio_render.h"

namespace {

using lift::dshow::AudioChunk;
using lift::dshow::AudioRenderer;

// 造一段 `frames` × `channels` 的 PCM，值 = `value`。
AudioChunk makeChunk(std::int64_t pts_100ns, std::uint32_t frames, std::uint32_t channels, std::int16_t value,
                     std::uint32_t rate = 44100) {
    AudioChunk c;
    c.pts_100ns = pts_100ns;
    c.samplerate = rate;
    c.channels = channels;
    c.pcm.assign(static_cast<std::size_t>(frames) * channels, value);
    return c;
}

struct Sink {
    std::int64_t now = 0;
    std::vector<std::int16_t> received;
    std::uint64_t calls = 0;
};

}  // namespace

int main() {
    using namespace lift_test;
    g_current_case = "movie-audio";

    // ---- ① 前导 + ② 迟到丢弃 ----
    {
        Sink sink;
        AudioRenderer r;
        r.bind([&sink] { return sink.now; },
               [&sink](const std::int16_t* pcm, std::uint32_t frames) {
                   ++sink.calls;
                   sink.received.insert(sink.received.end(), pcm, pcm + static_cast<std::size_t>(frames) * 2u);
               });
        r.begin(0);
        r.feed(makeChunk(0, 1152, 2, 1000));          // 0 ms 的块
        r.feed(makeChunk(10'000'000, 1152, 2, 2000));  // 1 s 的块（还没到期）
        r.pump(0);
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(sink.calls), 1u);  // 只有第一块
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(r.queueChunks()), 1u);

        r.pump(10'000'000);
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(sink.calls), 2u);

        // 迟到：pts=2s 的块，在 2.5s 才泵 ⇒ 2s + 26ms < 2.5s - 300ms ⇒ 丢。
        r.feed(makeChunk(20'000'000, 1152, 2, 3000));
        r.pump(25'000'000);
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(r.droppedChunks()), 1u);
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(sink.calls), 2u);
        // 2.5 s 才泵、中间 1.5 s 没有数据 ⇒ 记一次 underrun（这是真空档，不是 bug）。
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(r.underruns()), 1u);
    }

    // ---- ③ underrun ----
    {
        Sink sink;
        AudioRenderer r;
        r.bind([&sink] { return sink.now; },
               [&sink](const std::int16_t* pcm, std::uint32_t frames) {
                   ++sink.calls;
                   (void)pcm;
                   (void)frames;
               });
        r.begin(0);
        r.pump(10'000'000);  // 队列空、时间已走 1 s
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(r.underruns()), 1u);
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(sink.calls), 0u);
    }

    // ---- ④ 音量 ----
    {
        Sink sink;
        AudioRenderer r;
        r.bind([&sink] { return sink.now; },
               [&sink](const std::int16_t* pcm, std::uint32_t frames) {
                   sink.received.insert(sink.received.end(), pcm,
                                        pcm + static_cast<std::size_t>(frames) * 2u);
               });
        r.begin(0);
        r.setVolumeDb(-100.0);
        r.feed(makeChunk(0, 1152, 2, 10000));
        r.pump(0);
        LIFT_CHECK(!sink.received.empty());
        bool all_zero = true;
        for (std::int16_t v : sink.received) all_zero = all_zero && (v == 0);
        LIFT_CHECK(all_zero);  // -100 dB = 静音

        sink.received.clear();
        r.setVolumeDb(0.0);
        r.feed(makeChunk(10'000'000, 1152, 2, 10000));
        r.pump(10'000'000);
        bool any_nonzero = false;
        for (std::int16_t v : sink.received) any_nonzero = any_nonzero || (v != 0);
        LIFT_CHECK(any_nonzero);
    }

    // ---- ⑤ 重采样 / 单声道 ----
    {
        Sink sink;
        AudioRenderer r;
        r.bind([&sink] { return sink.now; },
               [&sink](const std::int16_t* pcm, std::uint32_t frames) {
                   sink.received.insert(sink.received.end(), pcm,
                                        pcm + static_cast<std::size_t>(frames) * 2u);
               });
        r.begin(0);
        r.feed(makeChunk(0, 4410, 1, 1234));  // 100 ms @44.1k mono
        r.pump(0);
        const std::uint32_t out_frames = static_cast<std::uint32_t>(sink.received.size() / 2u);
        // 100 ms @48k = 4800 帧（±2 取整误差）
        LIFT_CHECK(out_frames >= 4798u && out_frames <= 4802u);
        // 单声道复制：L 和 R 相同（重采样线性插值后仍相等）
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(static_cast<std::uint16_t>(sink.received[0])),
                          static_cast<std::uint32_t>(static_cast<std::uint16_t>(sink.received[1])));
        LIFT_CHECK(r.pushedFrames() == out_frames);
    }

    return finish("movie-audio");
}
