// emulator-lift · 宿主侧 COM 对象的实现（IPin / IMemAllocator / IMediaSample）
//
// ★ 槽序一律以 `strmif.h` 为准（头注释里逐个标偏移 + 本模块读到过的调用点）：
//   `IPin`（18 槽）       : +0x0C Connect · +0x10 ReceiveConnection · +0x24 QueryDirection …
//   `IMemAllocator`（9 槽）: +0x0C SetProperties · +0x10 GetProperties · +0x14 Commit ·
//                            +0x18 Decommit · +0x1C GetBuffer · +0x20 ReleaseBuffer
//   `IMediaSample`（19 槽）: +0x0C GetPointer · … · +0x48 SetMediaType
// ★ 每个方法都是**同步**的、**不阻塞**（doc §5-2：任何 `WaitForSingleObject` 都是硬错误）。
// ★ ⛔ 这些 shim 里**不许**再发起 guest 调用：它们是在 guest 调我们的路径上，
//   在 shim 里铺 guest 帧会打乱 `GuestCallStack` 的链（见 guest_invoke.h 的说明）。
#include "runtime/dshow/objects.h"

#include <string>
#include <vector>

#include "runtime/com.h"
#include "runtime/heap.h"
#include "runtime/memory.h"
#include "runtime/shims.h"
#include "runtime/watch.h"
#include "runtime/x86.h"

#include "runtime/dshow/dshow.h"

namespace lift::dshow {
namespace {

// IID_IUnknown（COM 的约定值；任何对象都必须答它）。
constexpr Guid kIidUnknown{0x00000000, 0x0000, 0x0000, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
// IID_IPin —— `.data:0x52FC28 dd 56A86891h, 11CE0AD4h, 20003AB0h, 70A70BAFh`（逐字）
constexpr Guid kIidPin{0x56A86891, 0x0AD4, 0x11CE, {0xB0, 0x3A, 0x00, 0x20, 0xAF, 0x0B, 0xA7, 0x70}};
// IID_IMediaSample —— `.data:0x52FC78 dd 56A8689Ah, …`（逐字）
constexpr Guid kIidMediaSample{0x56A8689A, 0x0AD4, 0x11CE, {0xB0, 0x3A, 0x00, 0x20, 0xAF, 0x0B, 0xA7, 0x70}};
// IID_IMediaSample2 —— `.data:0x52FC88 dd 36B73884h, 11CFC2C8h, 8000468Bh, 60EF6C5Fh`
//   = {36B73884-C2C8-11CF-8B46-00805F6CEF60}。★ 影片渲染器的 `PrepareReceive`（`sub_4D13F0`）**优先**
//   QI 这个接口（`0x4D142B push offset dword_52FC88`）：成功 ⇒ 只调 `GetProperties(0x30, pin+0x10)`
//   + Release，**不进** 那 8 个 fallback 槽（+0x3C/24/1C/14/34/0C/2C/10）。⇒ 样本必须实现它。
constexpr Guid kIidMediaSample2{0x36B73884, 0xC2C8, 0x11CF, {0x8B, 0x46, 0x00, 0x80, 0x5F, 0x6C, 0xEF, 0x60}};
// IID_IReferenceClock —— 标准值 `56A86897-0AD4-11CE-B03A-0020AF0BA770`。
// ★ 引擎二进制里**没有**这个常量（`.lst` 无 56A86897 / 无 Clock 类）：它从图管理器拿到时钟后
//   只按 vtable 调 `GetTime`（`sub_4D5E90 mov eax,[ecx+0Ch]; call eax`），**不 QI** ⇒ 这个 IID 只用于
//   我们自己的 QI 语义；真正要紧的是 vtable 槽序（+0x0C GetTime / +0x10 AdviseTime / +0x14
//   AdvisePeriodic / +0x18 Unadvise，`strmif.h`）。
constexpr Guid kIidReferenceClock{
    0x56A86897, 0x0AD4, 0x11CE, {0xB0, 0x3A, 0x00, 0x20, 0xAF, 0x0B, 0xA7, 0x70}};
// IID_IMemAllocator —— `.data:0x52FC98 dd 56A8689Ch, …`（GUID 表里的位置；名字是标准 IMemAllocator）
constexpr Guid kIidMemAllocator{0x56A8689C, 0x0AD4, 0x11CE, {0xB0, 0x3A, 0x00, 0x20, 0xAF, 0x0B, 0xA7, 0x70}};

bool guidAt(X86 &cpu, std::uint32_t ea, const Guid &want) {
    if (ea == 0 || !cpu.mem->committed(ea, 16u))
        return false;
    if (cpu.mem->rd32(ea) != want.data1)
        return false;
    if (cpu.mem->rd16(ea + 4u) != want.data2)
        return false;
    if (cpu.mem->rd16(ea + 6u) != want.data3)
        return false;
    for (std::uint32_t i = 0; i < 8u; ++i)
        if (cpu.mem->rd8(ea + 8u + i) != want.data4[i])
            return false;
    return true;
}

std::uint32_t stateOf(X86 &cpu, std::uint32_t self, const char *what) {
    if (cpu.com == nullptr) {
        dshowLog(LogLevel::Error, "%s: cpu.com 为空（装配错误）⇒ E_FAIL", what);
        return 0;
    }
    const std::uint32_t state = cpu.com->userOf(self);
    if (state == 0) {
        // ⛔ 不静默：要么 guest 伪造了 vtable，要么对象已经释放（refcount 到 0 后对象头被抹掉）。
        dshowLog(LogLevel::Warn, "%s: this=0x%08X 没有扩展块（不是本模块建的对象 / 已释放）⇒ E_FAIL", what,
                 self);
    } else {
        // ★ 诊断（Debug）：每次进宿主对象都留一行 this / 对象头 / 状态块 —— 排查"样本被谁改坏"。
        dshowLog(LogLevel::Debug, "%s: this=0x%08X [this]=0x%08X state=0x%08X", what, self,
                 cpu.mem->committed(self, 4u) ? cpu.mem->rd32(self) : 0u, state);
    }
    return state;
}

// IUnknown：三个对象共用同一套语义（refcount 的权威在 ComRegistry，见 runtime/com.h）。
// `extra_iid` 非空 ⇒ 额外认一个接口（样本用：`IID_IMediaSample2`）。
void shimQueryInterfaceImpl(X86 &cpu, ShimCall &call, const Guid &iid, const char *what,
                            const Guid *extra_iid = nullptr) {
    const std::uint32_t self = call.u32(0);
    const std::uint32_t iid_ea = call.u32(1);
    const std::uint32_t out = call.u32(2);
    dshowLog(LogLevel::Debug, "%s: this=0x%08X [this]=0x%08X iid=0x%08X out=0x%08X", what, self,
             cpu.mem->committed(self, 4u) ? cpu.mem->rd32(self) : 0u, iid_ea, out);
    if (out == 0) {
        call.result = kEPointer;
        return;
    }
    if (!cpu.mem->committed(out, 4u)) {
        call.result = kEInvalidArg;
        return;
    }
    cpu.mem->wr32(out, 0);
    if (guidAt(cpu, iid_ea, kIidUnknown) || guidAt(cpu, iid_ea, iid) ||
        (extra_iid != nullptr && guidAt(cpu, iid_ea, *extra_iid))) {
        if (cpu.com == nullptr || !cpu.com->addRef(cpu, self)) {
            call.result = kEFail;
            return;
        }
        cpu.mem->wr32(out, self);
        call.result = kSOk;
        return;
    }
    dshowLog(LogLevel::Debug, "%s: QI(0x%08X) 不认这个 IID ⇒ E_NOINTERFACE", what, iid_ea);
    call.result = kENoInterface;
}

void shimAddRefImpl(X86 &cpu, ShimCall &call, const char *what) {
    const std::uint32_t self = call.u32(0);
    dshowLog(LogLevel::Debug, "%s: this=0x%08X [this]=0x%08X", what, self,
             cpu.mem->committed(self, 4u) ? cpu.mem->rd32(self) : 0u);
    if (cpu.com == nullptr || !cpu.com->addRef(cpu, self)) {
        dshowLog(LogLevel::Warn, "%s: AddRef(0x%08X) 失败（不是本模块的对象）", what, self);
        call.result = 0;
        return;
    }
    call.result = cpu.com->refCount(self);
}

// Release：到 0 时 ComRegistry 会释放对象与 vtable，**但不会**释放我们挂在 `user` 上的状态块
// ⇒ 这里补上（最后一个引用消失时）。
void shimReleaseImpl(X86 &cpu, ShimCall &call, const char *what) {
    const std::uint32_t self = call.u32(0);
    dshowLog(LogLevel::Debug, "%s: this=0x%08X [this]=0x%08X", what, self,
             cpu.mem->committed(self, 4u) ? cpu.mem->rd32(self) : 0u);
    if (cpu.com == nullptr) {
        call.result = 0;
        return;
    }
    const std::uint32_t state = cpu.com->userOf(self);
    const std::uint32_t before = cpu.com->refCount(self);
    const bool ok = cpu.com->release(cpu, self);
    if (!ok) {
        dshowLog(LogLevel::Warn, "%s: Release(0x%08X) 失败（重复 release / 野指针）", what, self);
        call.result = 0;
        return;
    }
    if (before <= 1u && state != 0 && cpu.heap != nullptr)
        cpu.heap->free(state);
    if (before <= 1u) {
        // ★ 诊断：对象被释放到 0。引擎若在 Receive 里 Release 我们交出去的样本，样本对象会被
        //   ComRegistry 销毁 ⇒ 后续 `[sample]` 读到垃圾（实测 guest fault READ 0x55D3712C）。
        dshowLog(LogLevel::Debug, "%s: Release(0x%08X) ⇒ 0（对象被销毁）", what, self);
    }
    call.result = before > 1u ? before - 1u : 0u;
}

// 未实现的槽：**响亮地**返回 E_NOTIMPL（一条 Debug 日志；⛔ 不是 loud trap —— 这些是我们自己的
// 宿主对象，槽位齐全本来就是为了不让引擎撞上 `comUnsupported` 停机器）。
void shimNotImpl(X86 &cpu, ShimCall &call) {
    (void)cpu;
    dshowLog(LogLevel::Debug, "%s 未实现 ⇒ E_NOTIMPL（this=0x%08X）", call.def ? call.def->name.c_str() : "?",
             call.u32(0));
    call.result = kENotImpl;
}

// ---- IPin ----
void shimPinQueryInterface(X86 &cpu, ShimCall &call) {
    shimQueryInterfaceImpl(cpu, call, kIidPin, "宿主 IPin::QueryInterface");
}
void shimPinAddRef(X86 &cpu, ShimCall &call) {
    shimAddRefImpl(cpu, call, "宿主 IPin::AddRef");
}
void shimPinRelease(X86 &cpu, ShimCall &call) {
    shimReleaseImpl(cpu, call, "宿主 IPin::Release");
}

// 我们这端是**输出** pin（PINDIR_OUTPUT = 1）：引擎若真来问方向，答案必须一致。
void shimPinQueryDirection(X86 &cpu, ShimCall &call) {
    const std::uint32_t out = call.u32(1);
    if (out == 0) {
        call.result = kEPointer;
        return;
    }
    cpu.mem->wr32(out, 1u); // PINDIR_OUTPUT
    call.result = kSOk;
}

void shimPinReceiveConnection(X86 &cpu, ShimCall &call) {
    (void)cpu;
    // 我们不会去连别人的输入 pin（宿主是"上游"）⇒ 按 `CBasePin::ReceiveConnection` 的
    // 已连接值回：`0x4D0D6F mov eax,80040204h` = VFW_E_ALREADY_CONNECTED。
    dshowLog(LogLevel::Debug, "宿主 IPin::ReceiveConnection(this=0x%08X) ⇒ VFW_E_ALREADY_CONNECTED",
             call.u32(0));
    call.result = kVfwEAlreadyConnected;
}

// ---- IMemAllocator ----
void shimAllocQueryInterface(X86 &cpu, ShimCall &call) {
    shimQueryInterfaceImpl(cpu, call, kIidMemAllocator, "宿主 IMemAllocator::QueryInterface");
}
void shimAllocAddRef(X86 &cpu, ShimCall &call) {
    shimAddRefImpl(cpu, call, "宿主 IMemAllocator::AddRef");
}
void shimAllocRelease(X86 &cpu, ShimCall &call) {
    shimReleaseImpl(cpu, call, "宿主 IMemAllocator::Release");
}

void shimAllocSetProperties(X86 &cpu, ShimCall &call) {
    const std::uint32_t self = call.u32(0);
    const std::uint32_t request = call.u32(1);
    const std::uint32_t actual = call.u32(2);
    const std::uint32_t state = stateOf(cpu, self, "IMemAllocator::SetProperties");
    if (state == 0) {
        call.result = kEFail;
        return;
    }
    std::uint32_t cb_buffer = 0;
    std::uint32_t cb_align = 1;
    if (request != 0 && cpu.mem->committed(request, 16u)) {
        // ALLOCATOR_PROPERTIES: cBuffers(0) cbBuffer(4) cbAlign(8) cbPrefix(12)
        cb_buffer = cpu.mem->rd32(request + 4u);
        cb_align = cpu.mem->rd32(request + 8u);
    }
    if (cb_align == 0)
        cb_align = 1;
    cpu.mem->wr32(state + alloc::kPropsCbBuffer, cb_buffer);
    cpu.mem->wr32(state + alloc::kPropsCbAlign, cb_align);
    if (actual != 0 && cpu.mem->committed(actual, 16u)) {
        // ★ 我们只有 1 个缓冲（doc §2-C：「只用 1 个缓冲即可」）⇒ 报 1，缓冲字节数按真实值。
        cpu.mem->wr32(actual + 0u, 1u);
        cpu.mem->wr32(actual + 4u, cpu.mem->rd32(state + alloc::kSize));
        cpu.mem->wr32(actual + 8u, cb_align);
        cpu.mem->wr32(actual + 12u, 0u);
    }
    dshowLog(LogLevel::Debug, "IMemAllocator::SetProperties: 请求 cbBuffer=%u cbAlign=%u ⇒ 实际 cBuffers=1",
             cb_buffer, cb_align);
    call.result = kSOk;
}

void shimAllocGetProperties(X86 &cpu, ShimCall &call) {
    const std::uint32_t state = stateOf(cpu, call.u32(0), "IMemAllocator::GetProperties");
    const std::uint32_t out = call.u32(1);
    if (state == 0) {
        call.result = kEFail;
        return;
    }
    if (out == 0) {
        call.result = kEPointer;
        return;
    }
    cpu.mem->wr32(out + 0u, 1u);
    cpu.mem->wr32(out + 4u, cpu.mem->rd32(state + alloc::kSize));
    cpu.mem->wr32(out + 8u, cpu.mem->rd32(state + alloc::kPropsCbAlign));
    cpu.mem->wr32(out + 12u, 0u);
    call.result = kSOk;
}

void shimAllocCommit(X86 &cpu, ShimCall &call) {
    const std::uint32_t state = stateOf(cpu, call.u32(0), "IMemAllocator::Commit");
    if (state == 0) {
        call.result = kEFail;
        return;
    }
    cpu.mem->wr32(state + alloc::kCommitted, 1u);
    dshowLog(LogLevel::Debug, "IMemAllocator::Commit ⇒ S_OK（缓冲 %u 字节；⛔ 不等待）",
             cpu.mem->rd32(state + alloc::kSize));
    call.result = kSOk;
}

void shimAllocDecommit(X86 &cpu, ShimCall &call) {
    const std::uint32_t state = stateOf(cpu, call.u32(0), "IMemAllocator::Decommit");
    if (state == 0) {
        call.result = kEFail;
        return;
    }
    cpu.mem->wr32(state + alloc::kCommitted, 0u);
    cpu.mem->wr32(state + alloc::kBusy, 0u);
    call.result = kSOk;
}

// ★ 核心判据（doc §2-C）：**绝不阻塞**。唯一缓冲被占用 ⇒ 立即 VFW_E_NOT_COMMITTED。
//   真 `CMemAllocator` 在这里 `WaitForSingleObject` 信号量 ⇒ SCOPE §1 的硬错误。
void shimAllocGetBuffer(X86 &cpu, ShimCall &call) {
    const std::uint32_t self = call.u32(0);
    const std::uint32_t out = call.u32(1);
    const std::uint32_t t_start = call.u32(2);
    const std::uint32_t t_end = call.u32(3);
    const std::uint32_t state = stateOf(cpu, self, "IMemAllocator::GetBuffer");
    if (state == 0) {
        call.result = kEFail;
        return;
    }
    if (out == 0) {
        call.result = kEPointer;
        return;
    }
    cpu.mem->wr32(out, 0u);
    if (cpu.mem->rd32(state + alloc::kBusy) != 0) {
        cpu.mem->wr32(state + alloc::kDropped, cpu.mem->rd32(state + alloc::kDropped) + 1u);
        dshowLog(LogLevel::Debug,
                 "IMemAllocator::GetBuffer: 唯一缓冲被占用 ⇒ VFW_E_NOT_COMMITTED(0x%08X)（⛔ 不阻塞）",
                 kVfwENotCommitted);
        call.result = kVfwENotCommitted;
        return;
    }
    const std::uint32_t sample = cpu.mem->rd32(state + alloc::kSample);
    if (sample == 0) {
        call.result = kEFail;
        return;
    }
    cpu.mem->wr32(state + alloc::kBusy, 1u);
    if (cpu.com != nullptr)
        cpu.com->addRef(cpu, sample); // 借出 = 多一个引用（COM 惯例）
    cpu.mem->wr32(out, sample);
    // 真分配器会把请求的时间写进样本；这里照做（宿主泵自己也会写）。
    const std::uint32_t sample_state = cpu.com != nullptr ? cpu.com->userOf(sample) : 0;
    if (sample_state != 0 && t_start != 0 && cpu.mem->committed(t_start, 8u)) {
        cpu.mem->wr64(sample_state + smp::kTimeStart, cpu.mem->rd64(t_start));
        cpu.mem->wr32(sample_state + smp::kFlags, cpu.mem->rd32(sample_state + smp::kFlags) | smp::kFlagTime);
        if (t_end != 0 && cpu.mem->committed(t_end, 8u))
            cpu.mem->wr64(sample_state + smp::kTimeEnd, cpu.mem->rd64(t_end));
    }
    call.result = kSOk;
}

void shimAllocReleaseBuffer(X86 &cpu, ShimCall &call) {
    const std::uint32_t self = call.u32(0);
    const std::uint32_t buffer = call.u32(1);
    const std::uint32_t state = stateOf(cpu, self, "IMemAllocator::ReleaseBuffer");
    if (state == 0) {
        call.result = kEFail;
        return;
    }
    const std::uint32_t sample = cpu.mem->rd32(state + alloc::kSample);
    if (buffer != 0 && buffer != sample) {
        dshowLog(LogLevel::Warn, "IMemAllocator::ReleaseBuffer: 交回的不是本分配器的样本（0x%08X ≠ 0x%08X）",
                 buffer, sample);
    }
    cpu.mem->wr32(state + alloc::kBusy, 0u);
    // ★ 这里**不** Release 样本：`GetBuffer` 给的是"多一个引用"，按 COM 惯例由**调用方**在
    //   `Receive` 之后自己 Release（`pSample->Release(); pAlloc->ReleaseBuffer(pSample);`）。
    //   在这里顺手 Release 会让分配器自己持有那唯一一份引用被减掉 ⇒ 样本被提前释放。
    call.result = kSOk;
}

// ---- IMediaSample ----
void shimSampleQueryInterface(X86 &cpu, ShimCall &call) {
    // ★ 同时认 IID_IMediaSample2：`PrepareReceive` 会优先 QI 它，成功就只读一次
    //   `GetProperties(0x30)`，**不再走**那 8 个旧槽（那条路曾把返回地址写坏）。
    shimQueryInterfaceImpl(cpu, call, kIidMediaSample, "宿主 IMediaSample::QueryInterface",
                           &kIidMediaSample2);
}
void shimSampleAddRef(X86 &cpu, ShimCall &call) {
    shimAddRefImpl(cpu, call, "宿主 IMediaSample::AddRef");
}
void shimSampleRelease(X86 &cpu, ShimCall &call) {
    shimReleaseImpl(cpu, call, "宿主 IMediaSample::Release");
}

void shimSampleGetPointer(X86 &cpu, ShimCall &call) {
    const std::uint32_t state = stateOf(cpu, call.u32(0), "IMediaSample::GetPointer");
    const std::uint32_t out = call.u32(1);
    if (state == 0) {
        call.result = kEFail;
        return;
    }
    if (out == 0) {
        call.result = kEPointer;
        return;
    }
    // ★ 渲染器 `DoRenderSample` 就是拿这个指针按自底向上 DIB 读（doc §3-3 `0x487CCE`）。
    cpu.mem->wr32(out, cpu.mem->rd32(state + smp::kPointer));
    call.result = kSOk;
}

void shimSampleGetSize(X86 &cpu, ShimCall &call) {
    const std::uint32_t state = stateOf(cpu, call.u32(0), "IMediaSample::GetSize");
    if (state == 0) {
        call.result = 0;
        return;
    }
    call.result = cpu.mem->rd32(state + smp::kSize); // LONG（返回值，不是出参）
}

void shimSampleGetTime(X86 &cpu, ShimCall &call) {
    const std::uint32_t state = stateOf(cpu, call.u32(0), "IMediaSample::GetTime");
    if (state == 0) {
        call.result = kEFail;
        return;
    }
    // ★ doc §6-3：渲染器**是否**读时间戳还没实测。我们照标准给：有就 S_OK，没有就 S_FALSE
    //   （⛔ 不编一个没证据的 VFW_E_* 错误码）。
    const std::uint32_t flags = cpu.mem->rd32(state + smp::kFlags);
    if ((flags & smp::kFlagTime) == 0) {
        call.result = kSFalse;
        return;
    }
    const std::uint32_t t0 = call.u32(1);
    const std::uint32_t t1 = call.u32(2);
    if (t0 != 0 && cpu.mem->committed(t0, 8u))
        cpu.mem->wr64(t0, cpu.mem->rd64(state + smp::kTimeStart));
    if (t1 != 0 && cpu.mem->committed(t1, 8u))
        cpu.mem->wr64(t1, cpu.mem->rd64(state + smp::kTimeEnd));
    call.result = kSOk;
}

void shimSampleSetTime(X86 &cpu, ShimCall &call) {
    const std::uint32_t state = stateOf(cpu, call.u32(0), "IMediaSample::SetTime");
    if (state == 0) {
        call.result = kEFail;
        return;
    }
    const std::uint32_t t0 = call.u32(1);
    const std::uint32_t t1 = call.u32(2);
    cpu.mem->wr64(state + smp::kTimeStart, t0 != 0 ? cpu.mem->rd64(t0) : 0u);
    cpu.mem->wr64(state + smp::kTimeEnd, t1 != 0 ? cpu.mem->rd64(t1) : 0u);
    cpu.mem->wr32(state + smp::kFlags, cpu.mem->rd32(state + smp::kFlags) | smp::kFlagTime);
    call.result = kSOk;
}

void shimSampleIsSyncPoint(X86 &cpu, ShimCall &call) {
    const std::uint32_t state = stateOf(cpu, call.u32(0), "IMediaSample::IsSyncPoint");
    if (state == 0) {
        call.result = kEFail;
        return;
    }
    call.result = (cpu.mem->rd32(state + smp::kFlags) & smp::kFlagSync) != 0 ? kSOk : kSFalse;
}

void shimSampleSetSyncPoint(X86 &cpu, ShimCall &call) {
    const std::uint32_t state = stateOf(cpu, call.u32(0), "IMediaSample::SetSyncPoint");
    if (state == 0) {
        call.result = kEFail;
        return;
    }
    std::uint32_t flags = cpu.mem->rd32(state + smp::kFlags);
    if (call.u32(1) != 0)
        flags |= smp::kFlagSync;
    else
        flags &= ~smp::kFlagSync;
    cpu.mem->wr32(state + smp::kFlags, flags);
    call.result = kSOk;
}

void shimSampleIsPreroll(X86 &cpu, ShimCall &call) {
    const std::uint32_t state = stateOf(cpu, call.u32(0), "IMediaSample::IsPreroll");
    if (state == 0) {
        call.result = kEFail;
        return;
    }
    call.result = (cpu.mem->rd32(state + smp::kFlags) & smp::kFlagPreroll) != 0 ? kSOk : kSFalse;
}

void shimSampleSetPreroll(X86 &cpu, ShimCall &call) {
    const std::uint32_t state = stateOf(cpu, call.u32(0), "IMediaSample::SetPreroll");
    if (state == 0) {
        call.result = kEFail;
        return;
    }
    std::uint32_t flags = cpu.mem->rd32(state + smp::kFlags);
    if (call.u32(1) != 0)
        flags |= smp::kFlagPreroll;
    else
        flags &= ~smp::kFlagPreroll;
    cpu.mem->wr32(state + smp::kFlags, flags);
    call.result = kSOk;
}

void shimSampleGetActualDataLength(X86 &cpu, ShimCall &call) {
    const std::uint32_t state = stateOf(cpu, call.u32(0), "IMediaSample::GetActualDataLength");
    if (state == 0) {
        call.result = 0;
        return;
    }
    call.result = cpu.mem->rd32(state + smp::kActual); // = stride*h
}

void shimSampleSetActualDataLength(X86 &cpu, ShimCall &call) {
    const std::uint32_t state = stateOf(cpu, call.u32(0), "IMediaSample::SetActualDataLength");
    if (state == 0) {
        call.result = kEFail;
        return;
    }
    cpu.mem->wr32(state + smp::kActual, call.u32(1));
    call.result = kSOk;
}

void shimSampleGetMediaTime(X86 &cpu, ShimCall &call) {
    const std::uint32_t state = stateOf(cpu, call.u32(0), "IMediaSample::GetMediaTime");
    if (state == 0) {
        call.result = kEFail;
        return;
    }
    if ((cpu.mem->rd32(state + smp::kFlags) & smp::kFlagMediaTime) == 0) {
        call.result = kSFalse;
        return;
    }
    const std::uint32_t t0 = call.u32(1);
    const std::uint32_t t1 = call.u32(2);
    if (t0 != 0 && cpu.mem->committed(t0, 8u))
        cpu.mem->wr64(t0, cpu.mem->rd64(state + smp::kMediaTimeStart));
    if (t1 != 0 && cpu.mem->committed(t1, 8u))
        cpu.mem->wr64(t1, cpu.mem->rd64(state + smp::kMediaTimeEnd));
    call.result = kSOk;
}

void shimSampleSetMediaTime(X86 &cpu, ShimCall &call) {
    const std::uint32_t state = stateOf(cpu, call.u32(0), "IMediaSample::SetMediaTime");
    if (state == 0) {
        call.result = kEFail;
        return;
    }
    const std::uint32_t t0 = call.u32(1);
    const std::uint32_t t1 = call.u32(2);
    cpu.mem->wr64(state + smp::kMediaTimeStart, t0 != 0 ? cpu.mem->rd64(t0) : 0u);
    cpu.mem->wr64(state + smp::kMediaTimeEnd, t1 != 0 ? cpu.mem->rd64(t1) : 0u);
    cpu.mem->wr32(state + smp::kFlags, cpu.mem->rd32(state + smp::kFlags) | smp::kFlagMediaTime);
    call.result = kSOk;
}

void shimSampleIsDiscontinuity(X86 &cpu, ShimCall &call) {
    const std::uint32_t state = stateOf(cpu, call.u32(0), "IMediaSample::IsDiscontinuity");
    if (state == 0) {
        call.result = kEFail;
        return;
    }
    call.result = (cpu.mem->rd32(state + smp::kFlags) & smp::kFlagDiscontinuity) != 0 ? kSOk : kSFalse;
}

void shimSampleSetDiscontinuity(X86 &cpu, ShimCall &call) {
    const std::uint32_t state = stateOf(cpu, call.u32(0), "IMediaSample::SetDiscontinuity");
    if (state == 0) {
        call.result = kEFail;
        return;
    }
    std::uint32_t flags = cpu.mem->rd32(state + smp::kFlags);
    if (call.u32(1) != 0)
        flags |= smp::kFlagDiscontinuity;
    else
        flags &= ~smp::kFlagDiscontinuity;
    cpu.mem->wr32(state + smp::kFlags, flags);
    call.result = kSOk;
}

void shimSampleGetMediaType(X86 &cpu, ShimCall &call) {
    const std::uint32_t state = stateOf(cpu, call.u32(0), "IMediaSample::GetMediaType");
    const std::uint32_t out = call.u32(1);
    if (state == 0) {
        call.result = kEFail;
        return;
    }
    if (out == 0) {
        call.result = kEPointer;
        return;
    }
    const std::uint32_t mt = cpu.mem->rd32(state + smp::kMediaType);
    cpu.mem->wr32(out, mt);
    // ★ 真 `CMediaSample::GetMediaType` 会 `CoTaskMemAlloc` 一份**拷贝**；我们直接给同一块
    //   （它只在 `SetMediaType` 之前有效）。文档口径：本模块自己释放（`freeMediaType`）。
    call.result = mt != 0 ? kSOk : kSFalse;
}

void shimSampleSetMediaType(X86 &cpu, ShimCall &call) {
    const std::uint32_t state = stateOf(cpu, call.u32(0), "IMediaSample::SetMediaType");
    if (state == 0) {
        call.result = kEFail;
        return;
    }
    cpu.mem->wr32(state + smp::kMediaType, call.u32(1));
    call.result = kSOk;
}

// ---- IMediaSample2（`IID = 0x52FC88`）----
// `AM_SAMPLE2_PROPERTIES`（x86，0x30 字节）：
//   0x00 cbData · 0x04 dwTypeSpecificFlags · 0x08 dwSampleFlags · 0x0C lActual
//   0x10 tStart(i64) · 0x18 tStop(i64) · 0x20 dwStreamId · 0x24 pMediaType
//   0x28 pbBuffer · 0x2C cbBuffer
constexpr std::uint32_t kSample2PropsBytes = 0x30u;
constexpr std::uint32_t kAmSampleSplicePoint = 0x1u;

void shimSampleGetProperties(X86 &cpu, ShimCall &call) {
    const std::uint32_t self = call.u32(0);
    const std::uint32_t cb = call.u32(1);
    const std::uint32_t out = call.u32(2);
    const std::uint32_t state = stateOf(cpu, self, "IMediaSample2::GetProperties");
    if (state == 0) {
        call.result = kEFail;
        return;
    }
    if (out == 0) {
        call.result = kEPointer;
        return;
    }
    // ⛔ 只写 `cbProperties` 允许的范围（引擎传 0x30，写多了就是踩栈/踩对象）。
    if (cb < kSample2PropsBytes || !cpu.mem->committed(out, kSample2PropsBytes)) {
        dshowLog(LogLevel::Warn,
                 "IMediaSample2::GetProperties: cb=0x%X / p=0x%08X 非法（需要 0x%X 且已提交）⇒ E_INVALIDARG",
                 cb, out, kSample2PropsBytes);
        call.result = kEInvalidArg;
        return;
    }
    const std::uint32_t ptr = cpu.mem->rd32(state + smp::kPointer);
    const std::uint32_t bytes = cpu.mem->rd32(state + smp::kSize);
    const std::int64_t t0 = static_cast<std::int64_t>(cpu.mem->rd64(state + smp::kTimeStart));
    const std::int64_t t1 = static_cast<std::int64_t>(cpu.mem->rd64(state + smp::kTimeEnd));
    cpu.mem->wr32(out + 0x00u, kSample2PropsBytes);   // cbData
    cpu.mem->wr32(out + 0x04u, 0u);                   // dwTypeSpecificFlags
    cpu.mem->wr32(out + 0x08u, kAmSampleSplicePoint); // dwSampleFlags
    cpu.mem->wr32(out + 0x0Cu, bytes);                // lActual
    cpu.mem->wr64(out + 0x10u, static_cast<std::uint64_t>(t0));
    cpu.mem->wr64(out + 0x18u, static_cast<std::uint64_t>(t1));
    cpu.mem->wr32(out + 0x20u, 0u); // dwStreamId
    // ★ `pMediaType` **必须 NULL**：真样本只有被 `SetMediaType` 过才非空。给非 0 会让
    //   `PrepareReceive`（`sub_4D5C60` 的 `[pin+0xCC]!=0` 分支）**每帧**重新 ApplyMediaType ⇒
    //   渲染器 `sub_488A80` 每帧 `operator new[](w*h)`（1280×720=921600）且不释放 ⇒
    //   128 MiB guest 堆在 ~130 帧耗尽（实测 `HeapAlloc 921600 ⇒ 0` + `RaiseException`）。
    cpu.mem->wr32(out + 0x24u, 0u);
    cpu.mem->wr32(out + 0x28u, ptr);   // pbBuffer
    cpu.mem->wr32(out + 0x2Cu, bytes); // cbBuffer
    dshowLog(LogLevel::Debug,
             "IMediaSample2::GetProperties(cb=0x%X, p=0x%08X) ⇒ 0x30 字节（buffer=0x%08X %uB t=%lld..%lld）",
             cb, out, ptr, bytes, static_cast<long long>(t0), static_cast<long long>(t1));
    call.result = kSOk;
}

void shimSampleSetProperties(X86 &cpu, ShimCall &call) {
    (void)cpu;
    // ⛔ 不写任何内存（引擎若传只读/栈上属性块，写它只会制造难查的破坏）。
    dshowLog(LogLevel::Debug, "IMediaSample2::SetProperties(cb=0x%X) ⇒ S_OK（记账，不写内存）", call.u32(1));
    call.result = kSOk;
}

// ---- 方法表 ----
const std::vector<ComMethod> &pinMethods() {
    static const std::vector<ComMethod> methods = {
        {0x00, "QueryInterface", Conv::Stdcall, 2, &shimPinQueryInterface},
        {0x04, "AddRef", Conv::Stdcall, 0, &shimPinAddRef},
        {0x08, "Release", Conv::Stdcall, 0, &shimPinRelease},
        {0x0C, "Connect", Conv::Stdcall, 2, &shimNotImpl},
        {0x10, "ReceiveConnection", Conv::Stdcall, 2, &shimPinReceiveConnection},
        {0x14, "Disconnect", Conv::Stdcall, 0, &shimNotImpl},
        {0x18, "ConnectedTo", Conv::Stdcall, 1, &shimNotImpl},
        {0x1C, "ConnectionMediaType", Conv::Stdcall, 1, &shimNotImpl},
        {0x20, "QueryPinInfo", Conv::Stdcall, 1, &shimNotImpl},
        {0x24, "QueryDirection", Conv::Stdcall, 1, &shimPinQueryDirection},
        {0x28, "QueryId", Conv::Stdcall, 1, &shimNotImpl},
        {0x2C, "QueryAccept", Conv::Stdcall, 1, &shimNotImpl},
        {0x30, "EnumMediaTypes", Conv::Stdcall, 1, &shimNotImpl},
        {0x34, "QueryInternalConnections", Conv::Stdcall, 2, &shimNotImpl},
        {0x38, "EndOfStream", Conv::Stdcall, 0, &shimNotImpl},
        {0x3C, "BeginFlush", Conv::Stdcall, 0, &shimNotImpl},
        {0x40, "EndFlush", Conv::Stdcall, 0, &shimNotImpl},
        {0x44, "NewSegment", Conv::Stdcall, 3, &shimNotImpl},
    };
    return methods;
}

const std::vector<ComMethod> &allocatorMethods() {
    static const std::vector<ComMethod> methods = {
        {0x00, "QueryInterface", Conv::Stdcall, 2, &shimAllocQueryInterface},
        {0x04, "AddRef", Conv::Stdcall, 0, &shimAllocAddRef},
        {0x08, "Release", Conv::Stdcall, 0, &shimAllocRelease},
        {0x0C, "SetProperties", Conv::Stdcall, 2, &shimAllocSetProperties},
        {0x10, "GetProperties", Conv::Stdcall, 1, &shimAllocGetProperties},
        {0x14, "Commit", Conv::Stdcall, 0, &shimAllocCommit},
        {0x18, "Decommit", Conv::Stdcall, 0, &shimAllocDecommit},
        {0x1C, "GetBuffer", Conv::Stdcall, 4, &shimAllocGetBuffer},
        {0x20, "ReleaseBuffer", Conv::Stdcall, 1, &shimAllocReleaseBuffer},
    };
    return methods;
}

const std::vector<ComMethod> &sampleMethods() {
    static const std::vector<ComMethod> methods = {
        {0x00, "QueryInterface", Conv::Stdcall, 2, &shimSampleQueryInterface},
        {0x04, "AddRef", Conv::Stdcall, 0, &shimSampleAddRef},
        {0x08, "Release", Conv::Stdcall, 0, &shimSampleRelease},
        {0x0C, "GetPointer", Conv::Stdcall, 1, &shimSampleGetPointer},
        {0x10, "GetSize", Conv::Stdcall, 0, &shimSampleGetSize},
        {0x14, "GetTime", Conv::Stdcall, 2, &shimSampleGetTime},
        {0x18, "SetTime", Conv::Stdcall, 2, &shimSampleSetTime},
        {0x1C, "IsSyncPoint", Conv::Stdcall, 0, &shimSampleIsSyncPoint},
        {0x20, "SetSyncPoint", Conv::Stdcall, 1, &shimSampleSetSyncPoint},
        {0x24, "IsPreroll", Conv::Stdcall, 0, &shimSampleIsPreroll},
        {0x28, "SetPreroll", Conv::Stdcall, 1, &shimSampleSetPreroll},
        {0x2C, "GetActualDataLength", Conv::Stdcall, 0, &shimSampleGetActualDataLength},
        {0x30, "SetActualDataLength", Conv::Stdcall, 1, &shimSampleSetActualDataLength},
        {0x34, "GetMediaTime", Conv::Stdcall, 2, &shimSampleGetMediaTime},
        {0x38, "SetMediaTime", Conv::Stdcall, 2, &shimSampleSetMediaTime},
        {0x3C, "IsDiscontinuity", Conv::Stdcall, 0, &shimSampleIsDiscontinuity},
        {0x40, "SetDiscontinuity", Conv::Stdcall, 1, &shimSampleSetDiscontinuity},
        {0x44, "GetMediaType", Conv::Stdcall, 1, &shimSampleGetMediaType},
        {0x48, "SetMediaType", Conv::Stdcall, 1, &shimSampleSetMediaType},
        // IMediaSample2（同一对象，QI 也认它）：引擎的 `PrepareReceive` 走这条路只读一次属性。
        {0x4C, "GetProperties", Conv::Stdcall, 2, &shimSampleGetProperties},
        {0x50, "SetProperties", Conv::Stdcall, 2, &shimSampleSetProperties},
    };
    return methods;
}

// ---- IReferenceClock（宿主时钟；真图在 Run 前 SetSyncSource 给每个 filter）----
// ★ 为什么必须给：`CBaseRenderer::WaitForRenderTime` 在 `[filter+0x18]`（m_pClock）为 0 时走
//   "无时钟"分支 ⇒ `sub_4D5710` 的 `WaitForMultipleObjects(2, …, 10000)`（每 10 s 重试；实测卡死）。
//   有了时钟它才会算 `m_rtStart - clock->GetTime()` 并按到期时间决定等多久。
void shimClockQueryInterface(X86 &cpu, ShimCall &call) {
    shimQueryInterfaceImpl(cpu, call, kIidReferenceClock, "宿主 IReferenceClock::QueryInterface");
}
void shimClockAddRef(X86 &cpu, ShimCall &call) {
    shimAddRefImpl(cpu, call, "宿主 IReferenceClock::AddRef");
}
void shimClockRelease(X86 &cpu, ShimCall &call) {
    shimReleaseImpl(cpu, call, "宿主 IReferenceClock::Release");
}

// `GetTime(LONGLONG* pTime)`：写 100ns。★ 必须与引擎 `timeGetTime()` 同域（毫秒 × 10000），
// 不是 steady epoch（`--real-time` 下 `now100ns()` 是 1.7e16 那种"纪元"值）。
void shimClockGetTime(X86 &cpu, ShimCall &call) {
    const std::uint32_t out = call.u32(1);
    if (out == 0) {
        call.result = kEPointer;
        return;
    }
    if (!cpu.mem->committed(out, 8u)) {
        call.result = kEInvalidArg;
        return;
    }
    const std::uint64_t now100 = cpu.clock ? cpu.clock->now100ns() : 0u;
    const std::uint64_t ms_aligned = (now100 / 10000ull) * 10000ull;
    cpu.mem->wr64(out, ms_aligned);
    dshowLog(LogLevel::Debug, "IReferenceClock::GetTime = %llu（100ns，timeGetTime 同域）",
             static_cast<unsigned long long>(ms_aligned));
    call.result = kSOk;
}

// `AdviseTime(baseTime, streamTime, hEvent, pdwCookie)`：本线**立即 signal** —— 我们只在样本到期时
// 才送帧，所以"等到期"这件事已经由泵做完了。若引擎真调到，签名是 6 个 DWORD（两个 i64 + 句柄 + 出参）。
void shimClockAdviseTime(X86 &cpu, ShimCall &call) {
    (void)cpu;
    const std::uint32_t h_event = call.u32(5);
    const std::uint32_t cookie = call.u32(6);
    dshowLog(LogLevel::Debug,
             "IReferenceClock::AdviseTime(hEvent=0x%08X) ⇒ S_OK（立即到期；cookie 出参 0x%08X）", h_event,
             cookie);
    call.result = kSOk;
}
void shimClockAdvisePeriodic(X86 &cpu, ShimCall &call) {
    (void)cpu;
    dshowLog(LogLevel::Debug, "IReferenceClock::AdvisePeriodic ⇒ E_NOTIMPL（渲染器不走）");
    call.result = kENotImpl;
}
void shimClockUnadvise(X86 &, ShimCall &call) {
    call.result = kSOk;
}

const std::vector<ComMethod> &clockMethods() {
    static const std::vector<ComMethod> methods = {
        {0x00, "QueryInterface", Conv::Stdcall, 2, &shimClockQueryInterface},
        {0x04, "AddRef", Conv::Stdcall, 0, &shimClockAddRef},
        {0x08, "Release", Conv::Stdcall, 0, &shimClockRelease},
        {0x0C, "GetTime", Conv::Stdcall, 1, &shimClockGetTime},
        // AdviseTime(REFERENCE_TIME base, REFERENCE_TIME stream, HEVENT h, DWORD_PTR* cookie)
        //   = 2 + 2 + 1 + 1 = 6 个 DWORD（call.u32(i) 按 DWORD 计）。
        {0x10, "AdviseTime", Conv::Stdcall, 6, &shimClockAdviseTime},
        {0x14, "AdvisePeriodic", Conv::Stdcall, 6, &shimClockAdvisePeriodic},
        {0x18, "Unadvise", Conv::Stdcall, 1, &shimClockUnadvise},
    };
    return methods;
}

// 最后一个引用消失时把状态块还给 guest 堆（`ComRegistry` 只管对象头与 vtable）。
void releaseOwned(X86 &cpu, std::uint32_t &obj, std::uint32_t &state, const char *what) {
    if (obj == 0)
        return;
    if (cpu.com == nullptr) {
        obj = 0;
        state = 0;
        return;
    }
    const std::uint32_t before = cpu.com->refCount(obj);
    const bool ok = cpu.com->release(cpu, obj);
    if (!ok) {
        dshowLog(LogLevel::Warn, "%s: release(0x%08X) 失败（对象已不在表里）", what, obj);
    } else if (before <= 1u && state != 0 && cpu.heap != nullptr) {
        cpu.heap->free(state);
        state = 0;
    } else if (before > 1u) {
        // 引擎还挂着引用（例如渲染器把我们记成 m_Connected）⇒ 状态块由它最后那次 Release 回收
        // （见 shimReleaseImpl）。
        dshowLog(LogLevel::Info, "%s: 还有 %u 个外部引用 ⇒ 对象与状态块留给对方 release", what, before - 1u);
        state = 0;
    }
    obj = 0;
}

} // namespace

bool HostObjects::create(X86 &cpu, std::string *err) {
    if (ready())
        return true;
    if (cpu.com == nullptr) {
        if (err)
            *err = "HostObjects::create: cpu.com 为空（ComRegistry 没装配）";
        return false;
    }
    if (cpu.heap == nullptr || !cpu.heap->ready()) {
        if (err)
            *err = "HostObjects::create: guest 堆没就绪";
        return false;
    }
    sample_state_ = cpu.heap->alloc(smp::kBytes, /*zero=*/true);
    if (sample_state_ == 0) {
        if (err)
            *err = std::string("HostObjects::create: 分配样本状态块失败：") + cpu.heap->lastError();
        return false;
    }
    sample_ = cpu.com->createObject(cpu, "IMediaSample", sampleMethods(), sample_state_);
    if (sample_ == 0) {
        if (err)
            *err = "HostObjects::create: 建 IMediaSample 对象失败（见上一行 COM 日志）";
        return false;
    }
    // ★ 调试观察口：登记样本对象头（`[sample]` = vtable）。`LIFT_WATCH_SAMPLE=1` 时
    //   `Machine::run` 每个 block 入口读一次，改写者会以 `[watch]` 一行出现（见 runtime/watch.h）。
    lift::setWatchSampleEa(sample_);
    alloc_state_ = cpu.heap->alloc(alloc::kBytes, /*zero=*/true);
    if (alloc_state_ == 0) {
        if (err)
            *err = std::string("HostObjects::create: 分配分配器状态块失败：") + cpu.heap->lastError();
        return false;
    }
    allocator_ = cpu.com->createObject(cpu, "IMemAllocator", allocatorMethods(), alloc_state_);
    if (allocator_ == 0) {
        if (err)
            *err = "HostObjects::create: 建 IMemAllocator 对象失败（见上一行 COM 日志）";
        return false;
    }
    cpu.mem->wr32(alloc_state_ + alloc::kSample, sample_);
    cpu.mem->wr32(alloc_state_ + alloc::kPropsCbAlign, 1u);
    clock_ = cpu.com->createObject(cpu, "IReferenceClock", clockMethods(), /*user=*/0);
    if (clock_ == 0) {
        if (err)
            *err = "HostObjects::create: 建 IReferenceClock 对象失败（见上一行 COM 日志）";
        return false;
    }
    output_pin_ = cpu.com->createObject(cpu, "IPin", pinMethods(), /*user=*/0);
    if (output_pin_ == 0) {
        if (err)
            *err = "HostObjects::create: 建 IPin 对象失败（见上一行 COM 日志）";
        return false;
    }
    // ★ 宿主**自持一份**额外引用：引擎在 `ReceiveConnection` / `NotifyAllocator` / `Receive` 里会
    //   `Release` 它拿到的接口（真图也这么做）。若只有创建时的那一份，引擎一 Release 对象就被
    //   ComRegistry 销毁 ⇒ 之后 `[sample]` 读到垃圾 vtable（实测：guest fault @0x4D1425 READ 0x55D3712C）。
    //   多这一份之后：引擎的 Release 只掉到 1，对象一直可用；`destroy()` 里的 releaseOwned 再收尾。
    cpu.com->addRef(cpu, sample_);
    cpu.com->addRef(cpu, allocator_);
    cpu.com->addRef(cpu, output_pin_);
    cpu.com->addRef(cpu, clock_);
    dshowLog(LogLevel::Info,
             "宿主侧 DShow 对象：输出 pin=0x%08X 分配器=0x%08X(%uB) 样本=0x%08X(%uB) 时钟=0x%08X",
             output_pin_, allocator_, alloc::kBytes, sample_, smp::kBytes, clock_);
    return true;
}

void HostObjects::destroy(X86 &cpu) {
    if (buffer_ != 0 && cpu.heap != nullptr) {
        cpu.heap->free(buffer_);
        buffer_ = 0;
        buffer_bytes_ = 0;
    }
    releaseOwned(cpu, sample_, sample_state_, "HostObjects::destroy 样本");
    releaseOwned(cpu, allocator_, alloc_state_, "HostObjects::destroy 分配器");
    releaseOwned(cpu, output_pin_, pin_state_, "HostObjects::destroy 输出 pin");
    std::uint32_t no_state = 0;
    releaseOwned(cpu, clock_, no_state, "HostObjects::destroy 参考时钟");
}

bool HostObjects::prepareBuffer(X86 &cpu, std::uint32_t bytes, std::string *err) {
    if (bytes == 0) {
        if (err)
            *err = "prepareBuffer: 0 字节";
        return false;
    }
    if (buffer_ != 0 && buffer_bytes_ == bytes)
        return true;
    if (buffer_ != 0 && cpu.heap != nullptr) {
        cpu.heap->free(buffer_);
        buffer_ = 0;
        buffer_bytes_ = 0;
    }
    if (cpu.heap == nullptr || !cpu.heap->ready()) {
        if (err)
            *err = "prepareBuffer: guest 堆没就绪";
        return false;
    }
    buffer_ = cpu.heap->alloc(bytes, /*zero=*/true); // 行距填充位保持 0（确定性）
    if (buffer_ == 0) {
        if (err)
            *err = std::string("prepareBuffer: 分配 ") + std::to_string(bytes) + " 字节失败：" +
                   cpu.heap->lastError();
        return false;
    }
    buffer_bytes_ = bytes;
    cpu.mem->wr32(alloc_state_ + alloc::kBuffer, buffer_);
    cpu.mem->wr32(alloc_state_ + alloc::kSize, bytes);
    cpu.mem->wr32(alloc_state_ + alloc::kBusy, 0u);
    cpu.mem->wr32(alloc_state_ + alloc::kCommitted, 1u); // 宿主自己 Commit（引擎的 Commit 也接受）
    cpu.mem->wr32(sample_state_ + smp::kPointer, buffer_);
    cpu.mem->wr32(sample_state_ + smp::kSize, bytes);
    cpu.mem->wr32(sample_state_ + smp::kActual, bytes);
    dshowLog(LogLevel::Debug, "像素缓冲 = 0x%08X（%u 字节）", buffer_, bytes);
    return true;
}

bool HostObjects::acquire(X86 &cpu) {
    if (alloc_state_ == 0)
        return false;
    if (cpu.mem->rd32(alloc_state_ + alloc::kBusy) != 0)
        return false;
    cpu.mem->wr32(alloc_state_ + alloc::kBusy, 1u);
    return true;
}

void HostObjects::releaseBuffer(X86 &cpu) {
    if (alloc_state_ == 0)
        return;
    cpu.mem->wr32(alloc_state_ + alloc::kBusy, 0u);
}

void HostObjects::setSampleTiming(X86 &cpu, std::int64_t t_start, std::int64_t t_end) {
    if (sample_state_ == 0)
        return;
    cpu.mem->wr64(sample_state_ + smp::kTimeStart, static_cast<std::uint64_t>(t_start));
    cpu.mem->wr64(sample_state_ + smp::kTimeEnd, static_cast<std::uint64_t>(t_end));
    cpu.mem->wr32(sample_state_ + smp::kFlags, cpu.mem->rd32(sample_state_ + smp::kFlags) | smp::kFlagTime);
}

// 媒体时间（`IMediaSample::GetMediaTime`）：**相对片头**的 pts，与 `SetTime` 的时钟域解耦。
void HostObjects::setSampleMediaTime(X86 &cpu, std::int64_t t_start, std::int64_t t_end) {
    if (sample_state_ == 0)
        return;
    cpu.mem->wr64(sample_state_ + smp::kMediaTimeStart, static_cast<std::uint64_t>(t_start));
    cpu.mem->wr64(sample_state_ + smp::kMediaTimeEnd, static_cast<std::uint64_t>(t_end));
    cpu.mem->wr32(sample_state_ + smp::kFlags,
                  cpu.mem->rd32(sample_state_ + smp::kFlags) | smp::kFlagMediaTime);
}

void HostObjects::setSampleLength(X86 &cpu, std::uint32_t bytes) {
    if (sample_state_ == 0)
        return;
    cpu.mem->wr32(sample_state_ + smp::kActual, bytes);
}

void HostObjects::setSampleMediaType(X86 &cpu, std::uint32_t media_type_ea) {
    if (sample_state_ == 0)
        return;
    cpu.mem->wr32(sample_state_ + smp::kMediaType, media_type_ea);
}

std::uint32_t HostObjects::takeDropped(X86 &cpu) {
    if (alloc_state_ == 0)
        return 0;
    const std::uint32_t n = cpu.mem->rd32(alloc_state_ + alloc::kDropped);
    if (n != 0)
        cpu.mem->wr32(alloc_state_ + alloc::kDropped, 0u);
    return n;
}

} // namespace lift::dshow
