// emulator-lift · headless present（P0）
//
// ★ 只做“一帧像素 → 帧号 + 哈希 (+ 可选 BMP)”；DDraw 的 Flip 在 P1 调它。
// ★ 不建 SDL 窗口：P3 的后端实现同一个接口。
#pragma once

#include <cstdint>
#include <string>

#include "runtime/surface.h"

namespace lift {

class HeadlessPresenter {
public:
    bool present(const SurfaceArena& arena, const GuestSurface& surface, const std::string& dump_path,
                 std::string* err);
    std::uint64_t frameCount() const { return frame_count_; }
    std::uint64_t lastHash() const { return last_hash_; }
    std::string lastDump() const { return last_dump_; }

private:
    std::uint64_t frame_count_ = 0;
    std::uint64_t last_hash_ = 0;
    std::string last_dump_;
};

}  // namespace lift
