// emulator-lift · 宿主侧 COM 对象：输出 pin（IPin）/ 分配器（IMemAllocator）/ 样本（IMediaSample）
//
// ★ 为什么要造它们：D1 接线（doc §2-D）要由**宿主**充当"上游 filter"：
//   `g:` 渲染器的 `ReceiveConnection` 会 `pConnector->AddRef()`（`0x4D0E1D`）并把它记成
//   `m_Connected`；`NotifyAllocator(h:alloc, FALSE)` 必须在我们自己的分配器上成功，否则引擎会去
//   `CoCreateInstance(CLSID_MemoryAllocator)` 走 `CMemAllocator` 的信号量 ⇒ `WaitForSingleObject`
//   硬错误（SCOPE §1）。`Receive` 交出去的样本由渲染器 `DoRenderSample` 取 `GetPointer`
//   （`0x487CCE`，`IMediaSample` vtbl +0x0C）。
//
// ★ 对象状态放**guest 内存**（`ComRegistry::createObject` 的 `user` 扩展块，见 runtime/com.h）：
//   ⛔ 不建"按 EA 分派的宿主表"—— 那样一台机器两份图 / 测试里堆地址复用时就会串线。
//   布局常量在下面，**每个偏移都有注释**；读写的唯一入口是本文件的 shim 与 HostObjects 方法
//   （两者服务的是**同一份**状态 ⇒ 宿主直接写状态与 guest 调 SetTime/SetActualDataLength 等价）。
#pragma once

#include <cstdint>
#include <string>

namespace lift {
struct X86;
}

namespace lift::dshow {

// ---- 分配器状态块（guest 内存；`user` 扩展块）----
namespace alloc {
constexpr std::uint32_t kBuffer = 0;     // 像素缓冲的 guest EA
constexpr std::uint32_t kSize = 4;       // 缓冲字节数
constexpr std::uint32_t kBusy = 8;       // 0/1：已被借出（GetBuffer 或宿主泵）
constexpr std::uint32_t kCommitted = 12; // Commit / Decommit
constexpr std::uint32_t kDropped = 16;   // GetBuffer 撞上 busy 的次数（Graph 折进 stats）
constexpr std::uint32_t kPropsCbBuffer = 20;
constexpr std::uint32_t kPropsCbAlign = 24;
constexpr std::uint32_t kSample = 28; // 本分配器交出去的 IMediaSample*（guest EA）
constexpr std::uint32_t kBytes = 32;
} // namespace alloc

// ---- 样本状态块（guest 内存；`user` 扩展块）----
namespace smp {
constexpr std::uint32_t kPointer = 0;    // GetPointer 返回的 guest EA（= 像素缓冲）
constexpr std::uint32_t kSize = 4;       // GetSize
constexpr std::uint32_t kActual = 8;     // GetActualDataLength
constexpr std::uint32_t kFlags = 12;     // 见下面的 kFlag*
constexpr std::uint32_t kTimeStart = 16; // REFERENCE_TIME（i64）
constexpr std::uint32_t kTimeEnd = 24;
constexpr std::uint32_t kMediaTimeStart = 32;
constexpr std::uint32_t kMediaTimeEnd = 40;
constexpr std::uint32_t kMediaType = 48; // AM_MEDIA_TYPE*（0 = 没设）
constexpr std::uint32_t kBytes = 56;
constexpr std::uint32_t kFlagTime = 1u << 0;
constexpr std::uint32_t kFlagSync = 1u << 1;
constexpr std::uint32_t kFlagPreroll = 1u << 2;
constexpr std::uint32_t kFlagDiscontinuity = 1u << 3;
constexpr std::uint32_t kFlagMediaTime = 1u << 4;
} // namespace smp

// 三个宿主 COM 对象的宿主侧把手（guest EA 存这里；状态在 guest 内存）。
class HostObjects {
  public:
    bool create(X86 &cpu, std::string *err);
    void destroy(X86 &cpu);

    bool ready() const {
        return output_pin_ != 0 && allocator_ != 0 && sample_ != 0;
    }
    // 分配像素缓冲（guest 堆）+ 把指针/大小写进分配器与样本；重复调用会先释放旧缓冲。
    bool prepareBuffer(X86 &cpu, std::uint32_t bytes, std::string *err);

    std::uint32_t outputPin() const {
        return output_pin_;
    }
    std::uint32_t allocator() const {
        return allocator_;
    }
    std::uint32_t sample() const {
        return sample_;
    }
    // 宿主 `IReferenceClock`（guest COM 对象；`GetTime` = `cpu.clock->now100ns()`）。
    // ★ 图的 Run 之前要对两个 filter 各调一次 `IBaseFilter::SetSyncSource(+0x20)` 把它设进去 ——
    //   否则 `CBaseRenderer::WaitForRenderTime` 走"无时钟"分支（`[filter+0x18]==0`）⇒
    //   `WaitForMultipleObjects(2, …, 10000)` 的 10 s 等待（SCOPE §1 硬错误）。
    std::uint32_t clock() const {
        return clock_;
    }
    void setSampleMediaTime(X86 &cpu, std::int64_t t_start, std::int64_t t_end);
    std::uint32_t buffer() const {
        return buffer_;
    }
    std::uint32_t sampleStateEa() const {
        return sample_state_;
    } // 诊断：样本状态块（不是样本对象）
    std::uint32_t bufferBytes() const {
        return buffer_bytes_;
    }

    // 分配器忙语义（`IMemAllocator::GetBuffer` 的同一份状态）：
    //   acquire() 成功 = 唯一缓冲借出；⛔ 绝不阻塞（doc §2-C）。
    bool acquire(X86 &cpu);
    void releaseBuffer(X86 &cpu);

    void setSampleTiming(X86 &cpu, std::int64_t t_start, std::int64_t t_end);
    void setSampleLength(X86 &cpu, std::uint32_t bytes);
    void setSampleMediaType(X86 &cpu, std::uint32_t media_type_ea);

    // 读走"分配器因忙而拒绝的次数"（清零）；Graph 把它折进 `Stats::dropped_busy`。
    std::uint32_t takeDropped(X86 &cpu);

  private:
    std::uint32_t output_pin_ = 0;
    std::uint32_t allocator_ = 0;
    std::uint32_t sample_ = 0;
    std::uint32_t alloc_state_ = 0;
    std::uint32_t sample_state_ = 0;
    std::uint32_t pin_state_ = 0; // 输出 pin 没有扩展块（`releaseOwned` 需要一个引用形参）
    std::uint32_t clock_ = 0;     // IReferenceClock（guest COM 对象）
    std::uint32_t buffer_ = 0;
    std::uint32_t buffer_bytes_ = 0;
};

// native override 用：从样本对象 EA 取像素缓冲 EA（0 = 不是我们的样本 / 未分配）。
std::uint32_t sampleBufferEa(X86 &cpu, std::uint32_t sample_ea);

} // namespace lift::dshow
