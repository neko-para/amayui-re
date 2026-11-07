// emulator-lift · `FrameFeed` 的真实现：`video::MovieDecoder`（见 decoder_feed.h 的口径）
//
// ★ 时间口径：**全部**由调用方通过 `now_100ns` 传入（Graph 用 `cpu.clock->now100ns()` 换算）。
//   本文件不读宿主时钟、不 sleep、不建线程（SCOPE §1）。
// ★ 零拷贝：`bytes_` 必须活到 `decoder_` 销毁 —— 见类里的成员顺序注释与 movie_decoder.h。
#include "runtime/dshow/decoder_feed.h"

#include <cstring>
#include <string>
#include <utility>

#include "runtime/video/movie_decoder.h"

namespace lift::dshow {
namespace {

class DecoderFeed final : public FrameFeed {
  public:
    explicit DecoderFeed(std::vector<std::uint8_t> bytes) : bytes_(std::move(bytes)) {}

    bool open(std::string *err) {
        if (bytes_.empty()) {
            if (err)
                *err = "源字节为 0";
            return false;
        }
        return decoder_.open(bytes_.data(), bytes_.size(), err);
    }

    bool info(std::uint32_t *w, std::uint32_t *h, std::int64_t *duration_100ns) override {
        video::Info i;
        if (!decoder_.info(&i) || i.width == 0 || i.height == 0)
            return false;
        *w = i.width;
        *h = i.height;
        *duration_100ns = i.duration_100ns;
        return true;
    }

    bool at(std::int64_t now_100ns, const std::uint8_t **rgb24, std::uint32_t *stride,
            std::int64_t *pts_100ns, bool *ended) override {
        *ended = false;
        video::Frame f;
        bool e = false;
        std::string err;
        if (!decoder_.decodeTo(now_100ns, &f, &e, &err)) {
            if (!err.empty())
                dshowLog(LogLevel::Warn, "MovieDecoder::decodeTo(%.3fs) 失败：%s",
                         static_cast<double>(now_100ns) / 1e7, err.c_str());
            *ended = e;
            return false;
        }
        if (f.pixels == nullptr || f.width == 0 || f.height == 0)
            return false;
        *rgb24 = f.pixels;
        *stride = f.stride;
        *pts_100ns = f.pts_100ns;
        return true;
    }

    bool seek(std::int64_t t_100ns) override {
        return decoder_.seek(t_100ns);
    }
    void rewind() override {
        decoder_.rewind();
    }

    double fps() const {
        video::Info i;
        return decoder_.info(&i) ? i.fps : 0.0;
    }

  private:
    // ★ 声明顺序 = 析构的反序：`decoder_` 先销毁，`bytes_` 后销毁（MovieDecoder 只持裸指针）。
    std::vector<std::uint8_t> bytes_;
    video::MovieDecoder decoder_;
};

} // namespace

std::unique_ptr<FrameFeed> makeDecoderFeed(std::vector<std::uint8_t> bytes, std::string *err) {
    auto feed = std::make_unique<DecoderFeed>(std::move(bytes));
    std::string open_err;
    if (!feed->open(&open_err)) {
        if (err)
            *err = open_err.empty() ? "MovieDecoder::open 失败" : open_err;
        return nullptr;
    }
    std::uint32_t w = 0, h = 0;
    std::int64_t dur = 0;
    const bool has_info = feed->info(&w, &h, &dur);
    dshowLog(LogLevel::Info, "MovieDecoder open：%ux%u fps=%.3f 时长=%.3fs", w, h, feed->fps(),
             static_cast<double>(dur) / 1e7);
    if (!has_info) {
        if (err)
            *err = "MovieDecoder::info 失败（无视频流？）";
        return nullptr;
    }
    return feed;
}

} // namespace lift::dshow
