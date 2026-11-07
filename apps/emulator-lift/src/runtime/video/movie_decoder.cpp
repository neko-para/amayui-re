// emulator-lift · MPEG-1 影片解码器的 pl_mpeg 包装（实现细节；公共头不 include pl_mpeg.h）
//
// ★ 数据源选择（**零拷贝**内存源）：`plm_buffer_create_with_memory()`（即
//   `plm_create_with_memory(bytes, size, free_when_done = 0)`）。
//   为什么不用 `plm_buffer_create_with_callbacks()`：本仓 vendored 的那版 pl_mpeg 里
//   `plm_buffer_create_with_callbacks()` 建的 buffer 是 `PLM_BUFFER_MODE_FILE` 且
//   `discard_read_bytes = FALSE`，而清掉已读字节的 `plm_buffer_discard_read_bytes()` 只在
//   **实现段**可见（`PLM_MPEG_IMPLEMENTATION` 之外没有声明）。⇒ 回调只能把数据 `plm_buffer_write()`
//   进一个只增不减的缓冲（它按需 realloc），128 KiB 之后要么涨到整段影片、要么停在 128 KiB。
//   要想既不整段拷贝、又能回收已读窗口，就得改 vendored 副本（⛔ 三处逐字节守卫不允许）。
//   内存源则是真零拷贝：pl_mpeg 只持有裸指针、只 seek/read，`plm_buffer_write` 对 FIXED_MEM
//   直接返回 0。⇒ 峰值内存 = 调用方手里的影片字节（本来就有）+ 解码器工作集。
//
// ★ 时间：pl_mpeg 的 `frame->time` 是秒（`frames_decoded / framerate`，从 0 起）；
//   `plm_video_get_time()` 也是同一口径。转 100ns 一律走 `secondsTo100ns()`（llround）。
//
// ★ 日志：本模块是「被请求的低层能力」，open 失败只记 Debug（探测/候选回退是调用方的正常路径）；
//   真正的用户可见失败由请求影片的那一层按 SCOPE §3.1 记 Warn/Error。
#include "runtime/video/movie_decoder.h"

#include <cmath>
#include <cstdarg>
#include <cstring>
#include <memory>
#include <vector>

// pl_mpeg.h 的**公共声明段**用了 `FILE`，而它到实现段才 include `<stdio.h>`
// ⇒ 消费者（本文件）必须先给 `<cstdio>`（emulator-cxx 的 `movie.cpp` 同口径）。
#include <cstdio>

#include "pl_mpeg.h"          // 只有本 .cpp（与 pl_mpeg_impl.cpp）看见第三方的声明
#include "runtime/log.h"

namespace lift::video {
namespace {

constexpr std::int64_t kTicksPerSecond = 10'000'000;  // 100ns 计数

// 秒 → 100ns：**先四舍五入再取整**（llround），避免 0.9999999 被截成 9999999。
// 负数 / NaN / inf ⇒ 0（时间轴没有负值；不让坏值变成 UB 的转换）。
std::int64_t secondsTo100ns(double seconds) {
    if (!(seconds > 0.0) || seconds > 1.0e9) return 0;
    return static_cast<std::int64_t>(std::llround(seconds * static_cast<double>(kTicksPerSecond)));
}

void setError(std::string* err, const std::string& message) {
    if (err != nullptr) *err = message;
}

void vlog(LogLevel level, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    logV(level, LogVideo, "[video] movie: ", fmt, ap);
    va_end(ap);
}

// pl_mpeg 的真实帧源：内存里的 MPEG-1 PS（零拷贝）。
class PlMpegFrameSource final : public FrameSource {
public:
    PlMpegFrameSource() = default;
    ~PlMpegFrameSource() override {
        if (plm_ != nullptr) plm_destroy(plm_);
    }

    PlMpegFrameSource(const PlMpegFrameSource&) = delete;
    PlMpegFrameSource& operator=(const PlMpegFrameSource&) = delete;

    bool init(const std::uint8_t* bytes, std::size_t size, std::string* err) {
        // pl_mpeg 的签名是 `uint8_t*`，但 FIXED_MEM 模式下它**只读**（`plm_buffer_write()` 对
        // FIXED_MEM 直接返回 0；demux 只做 seek / read）⇒ const_cast 不会写穿调用方的 const。
        plm_ = plm_create_with_memory(const_cast<std::uint8_t*>(bytes), size, 0);
        if (plm_ == nullptr) {
            setError(err, "plm_create_with_memory 失败");
            return false;
        }
        // 规范不要求音频输出（`data/spec/chapters/60-ops-wait.md` 的「成立条件」）：
        // 关掉以后 demux 不再把 MP2 packet 写进音频缓冲，也不会有 MP2 解码。
        // ★ 影片音频：**常开**（宿主侧播放；同步在 dshow::AudioRenderer 里，见 doc §8.18）。
        plm_set_audio_enabled(plm_, 1);
        plm_set_loop(plm_, 0);  // 循环由调用方决定（本模块不接管）

        if (!plm_has_headers(plm_)) {
            setError(err, "不是 MPEG-1 program stream（找不到 pack/system header）");
            return false;
        }
        const int width = plm_get_width(plm_);
        const int height = plm_get_height(plm_);
        if (width <= 0 || height <= 0) {
            setError(err, "没有可用的视频流（宽/高为 0）");
            return false;
        }
        const double fps = plm_get_framerate(plm_);
        if (!(fps > 0.0)) {
            setError(err, "视频头没有可用帧率");
            return false;
        }
        // duration 取不到（PLM_PACKET_INVALID_TS = -1）时按 0 记：不编造时长。
        double duration = plm_get_duration(plm_);
        if (!(duration > 0.0)) duration = 0.0;

        info_.width = static_cast<std::uint32_t>(width);
        info_.height = static_cast<std::uint32_t>(height);
        info_.fps = fps;
        info_.duration_100ns = secondsTo100ns(duration);
        return true;
    }

    bool info(Info* out) override {
        if (out == nullptr) return false;
        *out = info_;
        return true;
    }

    bool next(std::int64_t* pts_100ns, Frame* frame, bool* ended, std::string* err) override {
        if (ended != nullptr) *ended = false;
        if (plm_ == nullptr) {
            setError(err, "帧源未初始化");
            return false;
        }
        // seek 之后的第一帧已经解出来了（见 seek()）：先交付它，再继续往后解。
        if (has_pending_) {
            *frame = pending_;
            *pts_100ns = pending_.pts_100ns;
            has_pending_ = false;
            return true;
        }
        plm_frame_t* decoded = plm_decode_video(plm_);
        if (decoded == nullptr) {
            if (plm_has_ended(plm_)) {
                if (ended != nullptr) *ended = true;
                return false;
            }
            setError(err, "plm_decode_video 返回空（数据损坏或截断，且未到流尾）");
            return false;
        }
        return convert(decoded, pts_100ns, frame, err);
    }

    bool nextAudio(AudioFrame* out, bool* ended, std::string* err) override {
        if (plm_ == nullptr) {
            setError(err, "未打开");
            return false;
        }
        plm_samples_t* s = plm_decode_audio(plm_);
        if (s == nullptr) {
            if (plm_has_ended(plm_)) {
                if (ended) *ended = true;
                return false;
            }
            if (ended) *ended = false;
            return false;  // 没有音频流 / 数据还没到：不是错误
        }
        if (ended) *ended = false;
        audio_i16_.resize(static_cast<std::size_t>(s->count) * 2u);
        for (std::uint32_t i = 0; i < s->count * 2u; ++i) {
            float v = s->interleaved[i];
            if (v > 1.0f) v = 1.0f;
            if (v < -1.0f) v = -1.0f;
            audio_i16_[i] = static_cast<std::int16_t>(std::lround(v * 32767.0f));
        }
        out->samples = audio_i16_.data();
        out->frames = s->count;
        out->samplerate = static_cast<std::uint32_t>(plm_get_samplerate(plm_));
        if (out->samplerate == 0) out->samplerate = 44100;
        out->channels = 2;  // pl_mpeg 默认输出交织立体声（单声道源会复制到两声道）
        out->pts_100ns = static_cast<std::int64_t>(std::llround(s->time * 1e7));
        return true;
    }

    void rewind() override {
        has_pending_ = false;
        if (plm_ != nullptr) plm_rewind(plm_);
    }

    bool seek(std::int64_t t_100ns, std::string* err) override {
        if (plm_ == nullptr) {
            setError(err, "帧源未初始化");
            return false;
        }
        has_pending_ = false;
        const double seconds = static_cast<double>(t_100ns) / static_cast<double>(kTicksPerSecond);
        // seek_exact = 0：从 **t 之前最近的 I 帧** 开始。pl_mpeg 的 seek 已经在内部解出了这一帧；
        // 把它当 pending 交给上层，`decodeTo(now)` 再从它一路追到 now（最多一个 GOP 的解码量）。
        plm_frame_t* found = plm_seek_frame(plm_, seconds, 0);
        if (found == nullptr) {
            setError(err, "plm_seek_frame 失败（源不支持 seek 或位置不可达）");
            return false;
        }
        if (!convert(found, &pending_.pts_100ns, &pending_, err)) return false;
        has_pending_ = true;
        return true;
    }

private:
    bool convert(plm_frame_t* decoded, std::int64_t* pts_100ns, Frame* frame, std::string* err) {
        if (!audio_logged_) {
            audio_logged_ = true;
            const int n = plm_get_num_audio_streams(plm_);
            const int rate = plm_get_samplerate(plm_);
            vlog(LogLevel::Info, "音频流：%d 个，samplerate=%d（宿主侧播放，同步见 audio_render.h）", n, rate);
        }
        const std::uint32_t width = decoded->width;
        const std::uint32_t height = decoded->height;
        const std::size_t packed = static_cast<std::size_t>(width) * height * 3u;
        if (width == 0 || height == 0 || packed == 0) {
            setError(err, "解出的帧尺寸为 0");
            return false;
        }
        if (rgb_.size() != packed) rgb_.assign(packed, 0);
        // RGB24（R,G,B 顺序）。不是 BGRA：MPEG-1 没有 alpha、四通道多 33% 写带宽；
        // 需要 BGR/BGRA 的那一层自己做通道重排（本模块不猜上层的纹理格式）。
        plm_frame_to_rgb(decoded, rgb_.data(), static_cast<int>(width * 3u));
        frame->pixels = rgb_.data();
        frame->width = width;
        frame->height = height;
        frame->stride = width * 3u;
        frame->pts_100ns = secondsTo100ns(decoded->time);
        *pts_100ns = frame->pts_100ns;
        return true;
    }

    plm_t* plm_ = nullptr;
    Info info_{};
    std::vector<std::uint8_t> rgb_;  // 单帧暂存；上层会立刻拷进它自己的双缓冲
    Frame pending_{};
    bool has_pending_ = false;
    bool audio_logged_ = false;
    std::vector<std::int16_t> audio_i16_;
};

}  // namespace

MovieDecoder::MovieDecoder() = default;

MovieDecoder::MovieDecoder(FrameSource* source) : source_(source), injected_(source != nullptr) {}

MovieDecoder::~MovieDecoder() { close(); }

bool MovieDecoder::open(const std::uint8_t* bytes, std::size_t size, std::string* err) {
    if (err != nullptr) err->clear();
    close();

    if (injected_) {
        // 测试注入路径：不解析 bytes（fake source 自己就是数据源）。
    } else {
        if (bytes == nullptr || size == 0) {
            setError(err, "open: 空输入（bytes 为空或 size 为 0）");
            vlog(LogLevel::Debug, "open failed: empty input");
            return false;
        }
        auto owned = std::make_unique<PlMpegFrameSource>();
        std::string source_err;
        if (!owned->init(bytes, size, &source_err)) {
            setError(err, source_err);
            vlog(LogLevel::Debug, "open failed: %s", source_err.c_str());
            return false;
        }
        owned_source_ = std::move(owned);
        source_ = owned_source_.get();
    }

    if (source_ == nullptr) {
        setError(err, "open: 没有帧源");
        return false;
    }
    Info info{};
    if (!source_->info(&info)) {
        setError(err, "open: 帧源没有可用的影片信息");
        close();
        return false;
    }
    if (info.width == 0 || info.height == 0) {
        setError(err, "open: 影片宽/高为 0");
        close();
        return false;
    }
    info_ = info;
    opened_ = true;
    vlog(LogLevel::Info, "opened: %ux%u @ %.3f fps, duration %.3f s", info.width, info.height,
         info.fps, static_cast<double>(info.duration_100ns) / 1e7);
    return true;
}

void MovieDecoder::close() {
    owned_source_.reset();
    source_ = injected_ ? source_ : nullptr;  // 注入的源不归我们所有：只放开引用
    opened_ = false;
    info_ = Info{};
    resetPlaybackState();
    frame_bytes_[0].reset();
    frame_bytes_[1].reset();
    frame_capacity_[0] = 0;
    frame_capacity_[1] = 0;
}

bool MovieDecoder::info(Info* out) const {
    if (!opened_ || out == nullptr) return false;
    *out = info_;
    return true;
}

void MovieDecoder::resetPlaybackState() {
    source_ended_ = false;
    ended_reported_ = false;
    has_lookahead_ = false;
    has_current_ = false;
    has_returned_ = false;
    returned_pts_ = 0;
    lookahead_ = Frame{};
    current_ = Frame{};
    next_slot_ = 0;
}

bool MovieDecoder::pullFrame(std::string* err) {
    if (source_ == nullptr) {
        setError(err, "pullFrame: 没有帧源");
        return false;
    }
    Frame frame{};
    std::int64_t pts = 0;
    bool ended = false;
    std::string source_err;
    if (!source_->next(&pts, &frame, &ended, &source_err)) {
        if (ended) {
            source_ended_ = true;
            return false;
        }
        setError(err, source_err.empty() ? "帧源解码失败" : source_err);
        return false;
    }
    if (frame.pixels == nullptr || frame.width == 0 || frame.height == 0) {
        setError(err, "帧源返回了非法帧（pixels/width/height 为空）");
        return false;
    }
    const std::uint32_t row_bytes = frame.width * 3u;
    const std::uint32_t stride = frame.stride != 0 ? frame.stride : row_bytes;
    const std::size_t packed = static_cast<std::size_t>(row_bytes) * frame.height;
    if (stride < row_bytes) {
        setError(err, "帧源返回的 stride 小于 width*3");
        return false;
    }

    // ★ 拷贝进解码器自持的 A/B 双缓冲：返回给调用方的 Frame 不依赖帧源缓冲的寿命
    //   （同一趟 decodeTo 可能连续解出多帧，帧源的单缓冲会被下一帧覆盖）。
    const int slot = next_slot_;
    if (frame_capacity_[slot] < packed) {
        frame_bytes_[slot] = std::make_unique<std::uint8_t[]>(packed);
        frame_capacity_[slot] = packed;
    }
    std::uint8_t* const dest = frame_bytes_[slot].get();
    for (std::uint32_t y = 0; y < frame.height; ++y) {
        std::memcpy(dest + static_cast<std::size_t>(y) * row_bytes,
                    frame.pixels + static_cast<std::size_t>(y) * stride, row_bytes);
    }
    next_slot_ = slot ^ 1;

    lookahead_ = frame;
    lookahead_.pixels = dest;
    lookahead_.stride = row_bytes;  // 我们的缓冲是紧凑的
    lookahead_.pts_100ns = pts;     // ★ 以 next() 的 out 参数为权威时间
    has_lookahead_ = true;
    return true;
}

bool MovieDecoder::commitLookahead() {
    if (!has_lookahead_) return false;
    current_ = lookahead_;
    has_current_ = true;
    has_lookahead_ = false;
    return true;
}

bool MovieDecoder::decodeTo(std::int64_t now_100ns, Frame* out, bool* ended, std::string* err) {
    if (err != nullptr) err->clear();
    if (ended != nullptr) *ended = false;
    if (out == nullptr) {
        setError(err, "decodeTo: out 为空");
        return false;
    }
    if (!opened_ || source_ == nullptr) {
        setError(err, "decodeTo: 尚未 open");
        return false;
    }
    if (now_100ns < 0) now_100ns = 0;

    // ① 追赶：把 pts <= now 的帧全部解出来。lookahead_ 是「下一帧」，它的 pts 最大。
    while (!source_ended_) {
        if (!has_lookahead_ && !pullFrame(err)) {
            if (!source_ended_) return false;  // 真错误（err 已填）
            break;                             // 这一趟刚好解到结尾
        }
        if (lookahead_.pts_100ns > now_100ns) break;  // 下一帧还没到
        commitLookahead();
    }

    // ② 只返回「比已经返回过的帧更新」的那一帧（单调 + 不重复）。
    if (has_current_ && (!has_returned_ || current_.pts_100ns > returned_pts_)) {
        *out = current_;
        has_returned_ = true;
        returned_pts_ = current_.pts_100ns;
        return true;
    }

    // ③ 「刚刚到结尾」这个事件只报一次（之后的调用 `*ended` 都是 false）。
    if (source_ended_ && !ended_reported_) {
        ended_reported_ = true;
        if (ended != nullptr) *ended = true;
    }
    return false;
}

bool MovieDecoder::seek(std::int64_t t_100ns, std::string* err) {
    if (err != nullptr) err->clear();
    if (!opened_ || source_ == nullptr) {
        setError(err, "seek: 尚未 open");
        return false;
    }
    std::string source_err;
    if (!source_->seek(t_100ns, &source_err)) {
        setError(err, source_err.empty() ? "seek: 帧源不支持 seek" : source_err);
        return false;
    }
    // 源里已经排好「seek 帧」；这里清掉上层的 lookahead/current/已返回/已报结束。
    resetPlaybackState();
    return true;
}

bool MovieDecoder::nextAudio(AudioFrame* out, bool* ended, std::string* err) {
    if (err) err->clear();
    if (ended) *ended = false;
    if (!opened_ || source_ == nullptr) {
        setError(err, "未打开");
        return false;
    }
    bool e = false;
    if (!source_->nextAudio(out, &e, err)) {
        if (e) {
            audio_ended_ = true;
            if (ended) *ended = true;
        }
        return false;
    }
    return true;
}

void MovieDecoder::rewind() {
    if (!opened_ || source_ == nullptr) return;
    source_->rewind();
    resetPlaybackState();
}

}  // namespace lift::video
