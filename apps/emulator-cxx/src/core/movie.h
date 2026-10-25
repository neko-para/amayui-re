// 影片播放后端：**平台边界**（`CORE-OPS-MOVIE` / `CORE-VM-MOVIE`）。
//
// 规范把「影片时长 / 解码 / 声音呈现」划给外部模块；核心只发请求、读回
// 「当前位置 / 停止位置 / 是否已结束 / 有没有新帧」。⛔ 语义层不要在这里写引擎字段。
//
// 实现见 `movie.cpp`（pl_mpeg，MPEG-1 program stream，只视频）。
#pragma once

#include "core/bytes.h"

#include <cstdint>
#include <memory>
#include <string>

namespace core {

// 一帧已解码画面：top-down RGBA8，alpha = 255（MPEG-1 没有 alpha 通道）。
struct MovieFrame {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    Bytes rgba;
};

class MovieBackend {
public:
    virtual ~MovieBackend() = default;

    // 取文件并配置播放后端。失败 ⇒ false + error；对象仍由调用方持有。
    virtual bool open(Bytes bytes, std::string& error) = 0;

    // 启动 / 继续播放（幂等，不重置位置）。
    virtual bool play() = 0;

    // 推进到「启播后经过 seconds 秒」；这一趟解出了新帧 ⇒ true。
    virtual bool advance_to(double seconds) = 0;

    // 后端报告的当前位置 / 停止位置（秒）。取不到 ⇒ 0（核心按「尚未结束」处理）。
    virtual double position() const = 0;
    virtual double stop_time() const = 0;
    // 后端报告已到流尾（与「停止时间 <= 当前位置」是两条等价的收尾信号，见残差）。
    virtual bool ended() const = 0;

    // 当前帧（还没解出任何帧时 width = 0）。
    virtual const MovieFrame& frame() const = 0;

    // 回到开头（循环位）。
    virtual bool rewind() = 0;

    // 停止 / 释放播放后端；不碰纹理对象。
    virtual void close() = 0;
};

// MPEG-1 program stream 后端（pl_mpeg）。
std::unique_ptr<MovieBackend> make_mpeg_backend();

}  // namespace core
