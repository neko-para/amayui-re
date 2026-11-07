// emulator-lift · 「源字节 → FrameFeed」的桥（`video::MovieDecoder` 的唯一消费者）
//
// ★ 为什么要这一层：`runtime/dshow/` 只认 `FrameFeed`（dshow.h），而字节 → 帧的解码在
//   `runtime/video/movie_decoder.*`（**已冻结**：本段只调用、不改它）。
//   把两者接起来的代码放这里，`dshow.h` 就仍然看不见 `video/`（边界不变）。
//
// ★ 零拷贝（movie_decoder.h 的「缓冲所有权」）：`MovieDecoder::open` 只持有裸指针 ⇒
//   返回的 feed **拥有** 那份 `bytes`，并保证它活到 decoder 销毁（成员声明顺序：
//   `bytes_` 在 `decoder_` 之前 ⇒ 析构反序，decoder 先走）。
//
// ★ 线程 / 等待：单线程、不 sleep、不建线程（SCOPE §1）。
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "runtime/dshow/dshow.h"

namespace lift::dshow {

// 用 `video::MovieDecoder` 包一份 MPEG-1 program stream 字节，变成 `Graph` 的帧源。
// 失败（空 / 垃圾 / 无视频流）⇒ 返回 nullptr 且 `*err` 非空（⛔ 不抛、不崩）。
// 成功 ⇒ 一条 Info 日志（w/h/fps/时长）。调用方把返回值存进 `Graph`（生命周期同 Graph）。
std::unique_ptr<FrameFeed> makeDecoderFeed(std::vector<std::uint8_t> bytes, std::string *err);

} // namespace lift::dshow
