// emulator-lift · guest 堆（Win32 形状 API 的**唯一**底层分配器）
//
// ★ 口径（README §2.6）：`HeapAlloc` / `LocalAlloc` / `GlobalAlloc` / `CoTaskMemAlloc` /
//   `VirtualAlloc`(小请求) 全部落到**同一个 guest 堆**，这样 `HeapFree` 能释放 `LocalAlloc` 的块
//   （⛔ 不许出现"两个分配器各管一半"⇒ free 跨 API 就炸）。
// ★ 返回的永远是 **guest 地址**（不是宿主指针）；元数据放宿主侧，guest 只看到指针。
// ★ 对齐：8 字节（README §2.6）。`HeapAlloc(HEAP_ZERO_MEMORY)` / `LMEM_ZEROINIT` 清零。
// ★ 越界 / 野指针 / double free 必须**响亮失败**（句柄校验是 S8 变异清单的一条）。
#pragma once

#include <cstdint>
#include <map>
#include <string>

#include "runtime/memory.h"

namespace lift {

class GuestHeap {
public:
    struct Options {
        std::uint32_t align = 8;        // 载荷对齐
        std::uint32_t header = 8;       // 载荷前的头（8 字节：size + magic）
        std::uint32_t min_split = 32;   // 小于这个的余块不切分
    };

    bool init(GuestMemory* mem, std::uint32_t begin, std::uint32_t size);
    bool init(GuestMemory* mem, std::uint32_t begin, std::uint32_t size, const Options& opt);
    bool ready() const { return mem_ != nullptr && end_ > begin_; }

    std::uint32_t begin() const { return begin_; }
    std::uint32_t end() const { return end_; }
    std::uint32_t bytesInUse() const { return in_use_; }
    std::uint32_t peak() const { return peak_; }
    std::uint32_t allocCount() const { return alloc_count_; }
    // 最大**空闲**块的总字节数（含块头）。★ 诊断用：`alloc` 失败时"总量满"与"碎片化"在
    //   `lastError()` 里本来是同一句话，这个数把两者分开（首次适配只认**连续**块）。
    std::uint32_t largestFreeBlock() const;
    std::uint32_t sizeOf(std::uint32_t ptr) const;
    bool owns(std::uint32_t ptr) const;

    // 0 = 失败（out_of_memory）；失败原因写 lastError()
    std::uint32_t alloc(std::uint32_t bytes, bool zero = false);
    bool free(std::uint32_t ptr);
    std::uint32_t realloc(std::uint32_t ptr, std::uint32_t bytes, bool zero = false);
    const char* lastError() const { return last_error_; }

private:
    struct Block {
        std::uint32_t off = 0;   // 相对 begin_ 的偏移（块头起点）
        std::uint32_t size = 0;  // 块总大小（含头 + 对齐后的载荷）
        std::uint32_t req = 0;   // 调用方请求的字节数（HeapSize 返回它）
        bool used = false;
    };
    void* payloadOf(std::uint32_t off) const;
    bool findBlock(std::uint32_t ptr, std::uint32_t* off) const;
    void coalesce();
    void dumpLeak(std::uint32_t ptr, const char* api) const;

    GuestMemory* mem_ = nullptr;
    std::uint32_t begin_ = 0;
    std::uint32_t end_ = 0;
    Options opt_;
    std::map<std::uint32_t, Block> blocks_;
    std::uint32_t in_use_ = 0;
    std::uint32_t peak_ = 0;
    std::uint32_t alloc_count_ = 0;
    char last_error_[256] = {};
};

}  // namespace lift
