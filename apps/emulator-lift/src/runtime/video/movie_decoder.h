// emulator-lift · 宿主侧 MPEG-1 影片解码（pl_mpeg 的包装层；无 COM / 无 guest / 无 SDL / 无线程）
//
// ★ 职责边界（SCOPE §1「视频 = 外部模块」）：这里只做「字节 → 某一时刻该显示的那一帧 RGB24」，
//   播放策略（什么时候推进、循环、淡入淡出、贴到哪个纹理）由上层 shim / 未来的影片 opcode 决定。
//   ⛔ 不读宿主时钟、⛔ 不 sleep、⛔ 不起线程 —— 时间完全由调用方通过 `now_100ns` 传入。
//
// ★ 为什么公共头**不** include `pl_mpeg.h`：pl_mpeg 是单文件库，它的实现是
//   「一个 TU 定义 PL_MPEG_IMPLEMENTATION」的形态；把它的声明拉进公共头会让每个消费者
//   都依赖于第三方布局（且升级时整棵树重编）。实现细节只落在 `movie_decoder.cpp`。
//
// ★ 时间基准：`pts_100ns` / `now_100ns` 都是**相对影片开头**的 100ns 计数。
//   pl_mpeg 的 `frame->time` 是秒（double，从 0 起按 `frames_decoded / fps` 递增）；
//   转换时**先四舍五入再取整**（`llround(seconds * 1e7)`），避免 `0.999999…` 把
//   29.97fps 的第 30 帧算成 9999999 而不是 10000000 这类抖动。
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace lift::video {

// 一帧已解码画面：**RGB24**（每像素 R,G,B 三个字节，行内紧凑）。
struct Frame {
    const std::uint8_t* pixels = nullptr;  // RGB24: R,G,B 顺序（不是 BGR / BGRA）
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t stride = 0;  // 一行多少字节（本实现恒为 width * 3）
    std::int64_t pts_100ns = 0;  // 相对影片开头
};

struct Info {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    double fps = 0.0;
    std::int64_t duration_100ns = 0;
};

// 帧源 seam：真实实现包 pl_mpeg；测试注入 fake 来验证**时间语义**（不需要真影片）。
// 契约：
//   · `next()` 解出**解码顺序**的下一帧；成功 ⇒ true，填 `*pts_100ns` 与 `frame` 的像素字段
//     （decoder 会把 `*pts_100ns` 写进 `Frame::pts_100ns`）；
//   · 到结尾 ⇒ false 且 `*ended = true`；出错 ⇒ false 且 `*ended = false` + `err`；
//   · `info()` 返回影片信息；`seek()/rewind()` 让后续 `next()` 从新位置继续；
//   · `pixels` 只需在**下一次 `next()` 之前**有效 —— 解码器会把每一帧拷进自己的存储，
//     所以返回给调用方的 `Frame` 不依赖源缓冲的寿命。
class FrameSource {
public:
    virtual ~FrameSource() = default;

    virtual bool info(Info* out) = 0;
    virtual bool next(std::int64_t* pts_100ns, Frame* frame, bool* ended, std::string* err) = 0;
    virtual void rewind() = 0;
    virtual bool seek(std::int64_t t_100ns, std::string* err) = 0;
};

// MPEG-1 program stream 解码器。
//
// ★ 缓冲所有权（**零拷贝**，调用方保证字节寿命）：
//   `open(bytes, size, …)` 走 `plm_create_with_memory(bytes, size, /*free_when_done=*/0)`，
//   pl_mpeg 在整个生命周期里**只持有裸指针**、不复制、不释放。所以：
//     **`bytes` 必须一直有效到 `close()`（或 `MovieDecoder` 析构）**。
//   选这条路径的理由见 `movie_decoder.cpp` 文件头（为什么不是 callbacks）。
//   零拷贝下峰值 = 影片字节（调用方本来就有）+ 解码器工作集
//   （pl_mpeg 视频缓冲：1280×720 的 Y/Cr/Cb 三帧 ≈ 4.2 MB；本模块双缓冲 RGB24 ≈ 5.5 MB；
//   加上 demux/视频 packet 缓冲各 128 KiB）。
//
// ★ 帧存储：解码器自持两块 RGB24 缓冲（A/B 轮换），每次 `FrameSource::next()` 的结果拷进当前槽。
//   返回的 `Frame::pixels` 指向解码器自有存储 ⇒ 在下一次 `decodeTo` / `seek` / `rewind` / `close`
//   之前**稳定**（`ended` 判 false 的调用不会碰它）。
//
// ★ 颜色：用 pl_mpeg 的 `plm_frame_to_rgb`（R,G,B 顺序）。**不用 BGR / BGRA 的理由**：
//   ① MPEG-1 没有 alpha，四通道要给每像素补 255，多 33% 的写带宽与内存；
//   ② 本仓上层要用的是「RGB24 送 D3D9 的 24/32bpp 纹理或自己转」—— R,G,B 顺序是
//      pl_mpeg 的 `rgb`（不是 Windows 素材惯用的 BGR），在这里写死，免得每个消费者各猜一次；
//   ③ 通道重排放在需要它的那一层做（D3D 纹理格式、SDL 纹理格式），本模块不猜。
class MovieDecoder {
public:
    MovieDecoder();
    // 注入帧源（测试用）：**不拥有** `source`（调用方保证它活到 decoder 析构）。
    // 注入后 `open()` 不再解析 `bytes`，直接用该 source（`bytes/size` 被忽略）。
    explicit MovieDecoder(FrameSource* source);
    ~MovieDecoder();

    MovieDecoder(const MovieDecoder&) = delete;
    MovieDecoder& operator=(const MovieDecoder&) = delete;

    // 打开内存里的 MPEG-1 PS。空 / 垃圾 / 无视频流 ⇒ false + `*err` 非空（不崩、不抛）。
    // 成功后 `bytes` 的所有权仍归调用方，但**必须活到 close()**（见上「缓冲所有权」）。
    bool open(const std::uint8_t* bytes, std::size_t size, std::string* err);

    // 幂等：销毁 pl_mpeg 实例并清空全部状态。之后 `info()` 返回 false、`decodeTo()` 返回 false。
    void close();

    // open 之前 / 失败之后 ⇒ false。成功后填 width/height/fps/duration。
    bool info(Info* out) const;

    // 解到「最新一帧 pts <= now_100ns」，返回 true 并填 `*out`；否则 false。
    // 精确语义：
    //   · 追赶：一次调用可能连续解出多帧（从上次位置一直解到 `pts > now`），返回其中 pts 最大的；
    //   · 单调 / 不重复：同一个（或更小的）`now` 再来一次**不会**倒退、也不会把同一帧再返回一次
    //     —— 此时返回 false（调用方继续显示手里那一帧）；
    //   · 到结尾：源报告结束的那一次调用返回 false 且 `*ended = true`；**只置一次**
    //     （之后的调用仍返回 false，但 `*ended = false`，因为「刚刚结束」这个事件已经报过了）；
    //   · 未 open / 源报错 ⇒ false + `*err` 非空（`*ended` 不置位）；
    //   · 成功路径会把 `*ended` 清成 false、`*err` 清空。
    bool decodeTo(std::int64_t now_100ns, Frame* out, bool* ended, std::string* err);

    // 跳到 `t_100ns`：pl_mpeg 的 seek 需要「从该时刻之前最近的 I 帧解起」，
    // 所以这之后第一次 `decodeTo(now >= t)` 会从 I 帧一路追到 `now`（最多一个 GOP 的解码量）。
    // 未 open / 源不支持 ⇒ false（`*err` 非空，若传了）。
    bool seek(std::int64_t t_100ns, std::string* err = nullptr);

    // 回到影片开头（清掉「已返回帧 / 已报结束」状态）。
    void rewind();

private:
    void resetPlaybackState();
    bool pullFrame(std::string* err);
    bool commitLookahead();

    std::unique_ptr<FrameSource> owned_source_;  // 真实路径：pl_mpeg 包装
    FrameSource* source_ = nullptr;              // 真实路径指向 owned_source_；注入路径指向外部 fake
    bool injected_ = false;                      // true ⇒ open() 不解析 bytes（测试注入的 fake source）
    bool opened_ = false;
    bool source_ended_ = false;      // 源报告过「到结尾」
    bool ended_reported_ = false;    // `*ended = true` 已经报过一次（只报一次）
    bool has_lookahead_ = false;
    bool has_current_ = false;
    bool has_returned_ = false;
    std::int64_t returned_pts_ = 0;
    Frame lookahead_{};
    Frame current_{};
    Info info_{};

    // 自持的 RGB24 双缓冲：返回给调用方的 pixels 指向这里（A/B 轮换，见类注释）。
    std::unique_ptr<std::uint8_t[]> frame_bytes_[2];
    std::size_t frame_capacity_[2] = {0, 0};
    int next_slot_ = 0;
};

}  // namespace lift::video
