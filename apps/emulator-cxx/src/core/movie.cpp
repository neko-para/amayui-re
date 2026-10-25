#include "core/movie.h"

#include <cstdio>
#include <cstring>
#include <utility>

// pl_mpeg 需要 FILE 声明（不开 PLM_NO_STDIO）。
#include <cstdio>

#define PL_MPEG_IMPLEMENTATION
#include "pl_mpeg.h"

namespace core {
namespace {

// pl_mpeg 的内存源不解引用所有权：字节必须活到 plm_destroy 之后。
class MpegMovieBackend final : public MovieBackend {
public:
    MpegMovieBackend() = default;
    ~MpegMovieBackend() override { close(); }

    MpegMovieBackend(const MpegMovieBackend&) = delete;
    MpegMovieBackend& operator=(const MpegMovieBackend&) = delete;

    bool open(Bytes bytes, std::string& error) override {
        close();
        if (bytes.size() < 4) {
            error = "mpeg-too-small:" + std::to_string(bytes.size());
            return false;
        }
        // pl_mpeg 在整个生命周期里持有这份缓冲的裸指针 ⇒ 先存字节、再建解码器。
        source_ = std::move(bytes);
        plm_ = plm_create_with_memory(source_.data(), source_.size(), 0);
        if (plm_ == nullptr) {
            error = "mpeg-create-failed";
            source_.clear();
            return false;
        }
        plm_set_audio_enabled(plm_, 0);  // 规范不要求音频输出
        plm_set_loop(plm_, 0);           // 循环位由引擎的收尾分支负责，不用后端自带的循环
        plm_set_video_decode_callback(plm_, &MpegMovieBackend::on_video, this);
        if (!plm_has_headers(plm_) || plm_get_width(plm_) <= 0 || plm_get_height(plm_) <= 0) {
            error = "mpeg-no-video-stream";
            close();
            return false;
        }
        frame_ = MovieFrame{};
        serial_ = 0;
        target_ = 0.0;
        return true;
    }

    bool play() override { return plm_ != nullptr; }

    bool advance_to(double seconds) override {
        if (plm_ == nullptr) return false;
        if (!(seconds > target_)) return false;  // 时间没前进 ⇒ 不解码
        const double delta = seconds - target_;
        target_ = seconds;
        const int before = serial_;
        plm_decode(plm_, delta);
        return serial_ != before;
    }

    double position() const override { return plm_ != nullptr ? plm_get_time(plm_) : 0.0; }
    double stop_time() const override { return plm_ != nullptr ? plm_get_duration(plm_) : 0.0; }
    bool ended() const override { return plm_ == nullptr || plm_has_ended(plm_) != 0; }
    const MovieFrame& frame() const override { return frame_; }

    bool rewind() override {
        if (plm_ == nullptr) return false;
        plm_rewind(plm_);
        target_ = 0.0;
        return true;
    }

    void close() override {
        if (plm_ != nullptr) {
            plm_destroy(plm_);  // 先销毁解码器（它还指着 source_），再释放字节
            plm_ = nullptr;
        }
        source_.clear();
        frame_ = MovieFrame{};
        serial_ = 0;
        target_ = 0.0;
    }

private:
    static void on_video(plm_t* self, plm_frame_t* frame, void* user) {
        (void)self;
        auto* backend = static_cast<MpegMovieBackend*>(user);
        backend->on_frame(frame);
    }

    void on_frame(plm_frame_t* frame) {
        if (frame == nullptr) return;
        const auto width = static_cast<std::uint32_t>(frame->width);
        const auto height = static_cast<std::uint32_t>(frame->height);
        const std::size_t bytes = static_cast<std::size_t>(width) * height * 4u;
        if (bytes == 0) return;
        if (frame_.rgba.size() != bytes) {
            frame_.rgba.assign(bytes, 255u);  // pl_mpeg 不写 alpha ⇒ 预填不透明
        }
        frame_.width = width;
        frame_.height = height;
        plm_frame_to_rgba(frame, frame_.rgba.data(), static_cast<int>(width) * 4);
        for (std::size_t i = 3; i < frame_.rgba.size(); i += 4) frame_.rgba[i] = 255u;
        ++serial_;
    }

    plm_t* plm_ = nullptr;
    Bytes source_;
    MovieFrame frame_;
    int serial_ = 0;
    double target_ = 0.0;
};

}  // namespace

std::unique_ptr<MovieBackend> make_mpeg_backend() {
    return std::make_unique<MpegMovieBackend>();
}

}  // namespace core
