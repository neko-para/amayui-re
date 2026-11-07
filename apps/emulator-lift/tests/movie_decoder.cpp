// emulator-lift · 影片解码模块夹具（pure：自造字节 + fake 帧源，不碰游戏 / LFS）
//
// 判据（红了意味着什么）：
//   ① 空 / 垃圾字节 ⇒ `open()` 返回 false + `err` 非空，不崩、不抛异常（用户给的影片可能是坏的）；
//   ② `info()` 在 open 之前返回 false（别让"没打开"看起来像"打开了一个 0x0 的影片"）；
//   ③ `decodeTo()` 的时间语义（用可注入的 FrameSource seam 验）：
//      单调、不重复返回同一帧、`ended` 只在「刚解到流尾」那一次置 true；
//   ④ 返回的 `Frame::pixels` 在**下一次 decode 之前**稳定（同一 now 再调一次不会改内容）；
//   ⑤ `seek` / `rewind` 之后能重新从目标位置起解。
//
// ★ 为什么用 fake 帧源：仓库里没有影片资产（`.MPG` 不进 LFS），而时间语义与 pl_mpeg 无关，
//   是 MovieDecoder 自己的逻辑 —— 所以把它抽成 seam 单独测（真解像素的正确性靠 pl_mpeg 自身
//   与 `tools/test/emulator-cxx-pl-mpeg-sync.test.mjs` 的逐字节守卫）。
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "check.h"
#include "runtime/video/movie_decoder.h"

namespace {

using lift::video::Frame;
using lift::video::FrameSource;
using lift::video::Info;
using lift::video::MovieDecoder;

constexpr std::uint32_t kWidth = 2;
constexpr std::uint32_t kHeight = 2;
constexpr std::uint32_t kStride = kWidth * 3u;

// 30fps 下 1/29.97… 的 100ns 计数（四舍五入后 333667）：正好覆盖"不要因 0.999999 抖动"的场景。
constexpr std::int64_t kFrameTicks = 333667;

// 假帧源：pts 由用例给定；每帧把全部字节填成 `pts & 0xFF`，这样"同一帧内容有没有被覆盖"一测就知。
class FakeSource final : public FrameSource {
public:
    explicit FakeSource(std::vector<std::int64_t> pts) : pts_(std::move(pts)) {}

    bool info(Info* out) override {
        if (out == nullptr) return false;
        out->width = kWidth;
        out->height = kHeight;
        out->fps = 30.0;
        out->duration_100ns = pts_.empty() ? 0 : pts_.back();
        return true;
    }

    bool next(std::int64_t* pts_100ns, Frame* frame, bool* ended, std::string* err) override {
        if (ended != nullptr) *ended = false;
        if (index_ >= pts_.size()) {
            if (ended != nullptr) *ended = true;
            return false;
        }
        if (++calls_ > 1000) {  // 防"时间语义写错导致无限解码"把测试挂死
            if (err != nullptr) *err = "fake: 解了 1000 帧还没结束";
            return false;
        }
        const std::int64_t pts = pts_[index_++];
        std::memset(pixels_, static_cast<int>(pts & 0xFF), sizeof(pixels_));
        frame->pixels = pixels_;
        frame->width = kWidth;
        frame->height = kHeight;
        frame->stride = kStride;
        frame->pts_100ns = pts;
        *pts_100ns = pts;
        return true;
    }

    void rewind() override {
        index_ = 0;
        has_pending_ = false;
    }

    // 简单 seek：从第一个 pts >= t 的帧起（够验 MovieDecoder 的状态重置）。
    bool seek(std::int64_t t_100ns, std::string* err) override {
        (void)err;
        std::size_t i = 0;
        while (i < pts_.size() && pts_[i] < t_100ns) ++i;
        index_ = i;
        has_pending_ = false;
        return true;
    }

    std::size_t calls() const { return calls_; }

private:
    std::vector<std::int64_t> pts_;
    std::size_t index_ = 0;
    std::size_t calls_ = 0;
    bool has_pending_ = false;
    std::uint8_t pixels_[kStride * kHeight] = {};
};

std::vector<std::uint8_t> makeGarbage(std::size_t size) {
    static const char kText[] = "not an mpeg-1 program stream";
    std::vector<std::uint8_t> bytes(size);
    for (std::size_t i = 0; i < size; ++i) {
        bytes[i] = static_cast<std::uint8_t>(kText[i % (sizeof(kText) - 1)]);
    }
    return bytes;
}

// ① / ②：坏输入 + open 之前的状态。
void testBadInputAndUnopenedState() {
    lift_test::g_current_case = "bad-input";
    MovieDecoder decoder;
    Info info;
    Frame frame;
    bool ended = true;
    std::string err = "stale";

    // info() 在 open 之前必须是 false（不是"0x0 的影片"）。
    LIFT_CHECK(!decoder.info(&info));

    // decodeTo 在 open 之前也是 false + err（不能静默当成"没有新帧"）。
    LIFT_CHECK(!decoder.decodeTo(0, &frame, &ended, &err));
    LIFT_CHECK(!err.empty());
    LIFT_CHECK(!ended);

    // 未 open 时 rewind / close 是 no-op（⛔ 不崩）。
    decoder.rewind();
    decoder.close();
    LIFT_CHECK(!decoder.info(&info));

    // 空输入：nullptr/0 与 非空指针/0 都必须被拒。
    err = "stale";
    LIFT_CHECK(!decoder.open(nullptr, 0, &err));
    LIFT_CHECK(!err.empty());
    const std::uint8_t one = 0;
    err = "stale";
    LIFT_CHECK(!decoder.open(&one, 0, &err));
    LIFT_CHECK(!err.empty());

    // 垃圾字节（纯文本）：找不到 MPEG-PS 的 pack/system header。
    const std::vector<std::uint8_t> garbage = makeGarbage(4096);
    err = "stale";
    LIFT_CHECK(!decoder.open(garbage.data(), garbage.size(), &err));
    LIFT_CHECK(!err.empty());

    // 伪 pack header（00 00 01 BA）+ 全 0：pl_mpeg 读进去以后必须失败，而不是越界/崩。
    std::vector<std::uint8_t> fakePack(512, 0);
    fakePack[0] = 0x00;
    fakePack[1] = 0x00;
    fakePack[2] = 0x01;
    fakePack[3] = 0xBA;
    err = "stale";
    LIFT_CHECK(!decoder.open(fakePack.data(), fakePack.size(), &err));
    LIFT_CHECK(!err.empty());

    // 失败之后仍然"未打开"。
    LIFT_CHECK(!decoder.info(&info));
    ended = true;
    err = "stale";
    LIFT_CHECK(!decoder.decodeTo(0, &frame, &ended, &err));
    LIFT_CHECK(!err.empty());
}

// ③ / ④：时间推进语义 + 帧指针稳定。
void testTimeAdvanceSemantics() {
    lift_test::g_current_case = "time-advance";
    const std::vector<std::int64_t> pts = {0, kFrameTicks, 2 * kFrameTicks, 3 * kFrameTicks,
                                           4 * kFrameTicks};
    FakeSource source(pts);
    MovieDecoder decoder(&source);

    std::string err = "stale";
    LIFT_CHECK(decoder.open(nullptr, 0, &err));
    LIFT_CHECK(err.empty());

    Info info;
    LIFT_CHECK(decoder.info(&info));
    LIFT_CHECK_EQ_U32(info.width, kWidth);
    LIFT_CHECK_EQ_U32(info.height, kHeight);
    LIFT_CHECK(info.fps > 29.0 && info.fps < 31.0);
    LIFT_CHECK_EQ_I64(info.duration_100ns, 4 * kFrameTicks);

    // 第 1 帧：pts = 0（`now = 0` 就够）。
    Frame frame;
    bool ended = true;
    LIFT_CHECK(decoder.decodeTo(0, &frame, &ended, &err));
    LIFT_CHECK(!ended);
    LIFT_CHECK(err.empty());
    LIFT_CHECK_EQ_I64(frame.pts_100ns, 0);
    LIFT_CHECK_EQ_U32(frame.width, kWidth);
    LIFT_CHECK_EQ_U32(frame.height, kHeight);
    LIFT_CHECK_EQ_U32(frame.stride, kStride);
    LIFT_CHECK(frame.pixels != nullptr);
    const std::uint8_t* const first_pixels = frame.pixels;
    const std::uint8_t first_value = static_cast<std::uint8_t>(0 & 0xFF);
    LIFT_CHECK_EQ_U32(frame.pixels[0], first_value);

    // now 不前进 ⇒ false（不重复返回同一帧），且已返回帧的存储**不被覆盖**。
    LIFT_CHECK(!decoder.decodeTo(0, &frame, &ended, &err));
    LIFT_CHECK(!ended);
    LIFT_CHECK_EQ_U32(frame.pixels[0], first_value);  // frame 没被改写
    LIFT_CHECK_EQ_U32(first_pixels[0], first_value);  // 存储仍然是第 1 帧
    LIFT_CHECK_EQ_I64(frame.pts_100ns, 0);

    // 在下一帧到达之前（now 前进一点点）仍然没有新帧。
    LIFT_CHECK(!decoder.decodeTo(kFrameTicks - 1, &frame, &ended, &err));
    LIFT_CHECK(!ended);

    // now 到达第 2 帧 ⇒ 返回 pts 最大的那一帧（= kFrameTicks）。
    LIFT_CHECK(decoder.decodeTo(kFrameTicks, &frame, &ended, &err));
    LIFT_CHECK(!ended);
    LIFT_CHECK_EQ_I64(frame.pts_100ns, kFrameTicks);
    LIFT_CHECK_EQ_U32(frame.pixels[0], static_cast<std::uint8_t>(kFrameTicks & 0xFF));

    // 大跳：一次调用把 2..4 帧全解出来，返回最新一帧（4 帧）；这时**还没有**报 ended。
    LIFT_CHECK(decoder.decodeTo(10 * kFrameTicks, &frame, &ended, &err));
    LIFT_CHECK(!ended);
    LIFT_CHECK(err.empty());
    LIFT_CHECK_EQ_I64(frame.pts_100ns, 4 * kFrameTicks);
    LIFT_CHECK_EQ_U32(frame.pixels[0], static_cast<std::uint8_t>((4 * kFrameTicks) & 0xFF));

    // 再要一帧 ⇒ false + ended 置一次。
    LIFT_CHECK(!decoder.decodeTo(11 * kFrameTicks, &frame, &ended, &err));
    LIFT_CHECK(ended);
    LIFT_CHECK(err.empty());

    // 再来一次 ⇒ 还是 false，但 ended **不再**置位（只报一次）。
    LIFT_CHECK(!decoder.decodeTo(12 * kFrameTicks, &frame, &ended, &err));
    LIFT_CHECK(!ended);

    // 单调：所有返回过的 pts 严格递增（上面已经按顺序断言；这里再验一次"不会倒退"）。
    LIFT_CHECK(source.calls() <= pts.size() + 1);  // 每帧最多解一次（+1 次是流尾探测）
}

// ⑤：seek / rewind 之后重新起解；seek 也会清掉"已报结束"的状态。
void testSeekAndRewind() {
    lift_test::g_current_case = "seek-rewind";
    const std::vector<std::int64_t> pts = {0, kFrameTicks, 2 * kFrameTicks, 3 * kFrameTicks};
    FakeSource source(pts);
    MovieDecoder decoder(&source);

    std::string err;
    LIFT_CHECK(decoder.open(nullptr, 0, &err));

    Frame frame;
    bool ended = false;
    LIFT_CHECK(decoder.decodeTo(100 * kFrameTicks, &frame, &ended, &err));
    LIFT_CHECK_EQ_I64(frame.pts_100ns, 3 * kFrameTicks);
    LIFT_CHECK(!decoder.decodeTo(101 * kFrameTicks, &frame, &ended, &err));
    LIFT_CHECK(ended);

    // seek 到第 3 帧 ⇒ 后续 decodeTo 从该帧起，且 ended 状态被清掉。
    LIFT_CHECK(decoder.seek(2 * kFrameTicks, &err));
    LIFT_CHECK(err.empty());
    ended = true;
    LIFT_CHECK(decoder.decodeTo(2 * kFrameTicks, &frame, &ended, &err));
    LIFT_CHECK(!ended);
    LIFT_CHECK_EQ_I64(frame.pts_100ns, 2 * kFrameTicks);
    LIFT_CHECK_EQ_U32(frame.pixels[0], static_cast<std::uint8_t>((2 * kFrameTicks) & 0xFF));

    // rewind ⇒ 回到第 1 帧。
    decoder.rewind();
    ended = true;
    LIFT_CHECK(decoder.decodeTo(0, &frame, &ended, &err));
    LIFT_CHECK(!ended);
    LIFT_CHECK_EQ_I64(frame.pts_100ns, 0);
    LIFT_CHECK_EQ_U32(frame.pixels[0], 0);

    // 未 open 的 decoder：seek 必须 false + err（⛔ 不假装成功）。
    MovieDecoder never;
    err = "stale";
    LIFT_CHECK(!never.seek(0, &err));
    LIFT_CHECK(!err.empty());
}

}  // namespace

int main() {
    testBadInputAndUnopenedState();
    testTimeAdvanceSemantics();
    testSeekAndRewind();
    return lift_test::finish("movie-decoder");
}
