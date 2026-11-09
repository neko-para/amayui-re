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
    // ★ DDraw 的**嵌套**锁计数（最多 8 层）；D3D9 的锁是"非嵌套、0→1"，
    //   ⛔ 不要混用这个变量（混在一起就分不清"DDraw 嵌了 8 层"与"D3D9 锁着"）。
    std::uint32_t lock_count = 0;

    // ---- ★ D3D9 后缓冲 surface 专属（只有 `d3d9::createDeviceState` 会填；纹理 / DDraw 恒 0）----
    // ★ 为什么不放设备状态块：`'D9D1'` 的 64 B 全占满（`runtime/d3d9.cpp` 的 DevOff，+44/+48/+52/
    //   +56/+60 各有主人）；`'TEX1'` 的锁字段在 +44/+48 —— 两处都会被踩坏。⛔ 不许"挤一挤"。
    // ★ 为什么不放宿主 map：键只能是状态块 EA 或 arena EA，两者都会被堆/arena 复用 ⇒ 得自己造
    //   生命周期；而锁的生命周期**就是像素块的生命周期** ⇒ 挂在 GuestSurface 上由 `destroy` 自动收。
    bool d3d9_locked = false;                       // IDirect3DSurface9::LockRect 期间
    std::int32_t d3d9_lock_rect[4] = {0, 0, 0, 0};  // x0,y0,x1,y1（右/下开区间；诊断用）
    std::uint32_t d3d9_lock_serial = 0;             // 累计锁次数（纯诊断）
    bool d3d9_lockable = false;                     // D3DPRESENTFLAG_LOCKABLE_BACKBUFFER
    std::uint32_t d3d9_multisample = 0;             // PresentParams::MultiSampleType（≠ NONE ⇒ 锁必失败）
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
