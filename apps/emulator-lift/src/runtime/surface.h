// emulator-lift · guest surface arena（P0 / G4）
//
// ★ 口径：DDraw 的 surface 像素必须落在 **guest 地址空间**里（`Lock` 返回 guest 指针；
//   guest 的 BMP 缩略图代码按 pitch + 通道掩码直接读）。⛔ 不能把宿主 `malloc` 指针交给 guest。
// ★ 这里只做“像素 + 格式 + 锁计数 + 哈希”的叶子设施；DDraw 对象 / vtable 在 P1。
// ★ 分配器 = 一个独立的 `GuestHeap` 区间（不与 Win32 堆共享），这样 `destroy` 可复用。
#pragma once

#include <cstdint>
#include <map>
#include <string>

#include "runtime/heap.h"
#include "runtime/memory.h"

namespace lift {

struct SurfaceFormat {
    std::uint16_t bpp = 32;
    std::uint32_t rmask = 0x00FF0000u;
    std::uint32_t gmask = 0x0000FF00u;
    std::uint32_t bmask = 0x000000FFu;
    std::uint32_t amask = 0xFF000000u;

    std::uint32_t bytesPerPixel() const { return bpp ? static_cast<std::uint32_t>(bpp / 8u) : 0u; }
};

struct GuestSurface {
    std::uint32_t ea = 0;       // 像素首字节的 guest 地址
    std::int32_t width = 0;
    std::int32_t height = 0;
    std::uint32_t pitch = 0;    // 每行字节数（DWORD 对齐）
    SurfaceFormat format;
    std::uint32_t lock_count = 0;
};

class SurfaceArena {
public:
    bool init(GuestMemory* mem, std::uint32_t begin, std::uint32_t size);
    bool ready() const { return mem_ != nullptr && heap_.ready(); }

    // 0 = 失败；失败原因写 lastError()
    std::uint32_t create(std::int32_t width, std::int32_t height, const SurfaceFormat& fmt);
    bool destroy(std::uint32_t ea);
    GuestSurface* find(std::uint32_t ea);
    const GuestSurface* find(std::uint32_t ea) const;

    std::uint8_t* pixels(GuestSurface& s);
    const std::uint8_t* pixels(const GuestSurface& s) const;

    // Lock/Unlock 只维护计数（P1 的 DDraw 用它判嵌套锁）；越界锁返回 false。
    bool lock(std::uint32_t ea);
    bool unlock(std::uint32_t ea);

    // FNV-1a over pixels（确定性；不读宿主指针进 trace）。
    std::uint64_t hash(const GuestSurface& s) const;

    // 写 32/24 位 BMP（32bpp 时保留 BGR，alpha 丢；16bpp 565 转 24 位）。
    bool dumpBmp(const GuestSurface& s, const std::string& host_path, std::string* err) const;

    const char* lastError() const { return last_error_; }
    std::size_t surfaceCount() const { return surfaces_.size(); }
    std::uint32_t bytesInUse() const { return heap_.bytesInUse(); }
    // ★ 诊断（运行摘要 / 端到端守卫用）：峰值活跃字节 + 当前最大**连续**空闲块。
    //   两者一起才能区分"总量满"与"碎片化"（首次适配只认连续块，`heap.cpp` 的失败文本同理）。
    std::uint32_t peak() const { return heap_.peak(); }
    std::uint32_t largestFreeBlock() const { return heap_.largestFreeBlock(); }

private:
    GuestMemory* mem_ = nullptr;
    GuestHeap heap_;
    std::map<std::uint32_t, GuestSurface> surfaces_;
    char last_error_[256] = {};
};

}  // namespace lift
