// emulator-lift · DirectShow 协议核心（`runtime/dshow/`）的 pure 夹具
//
// 判据（对应 docs/movie-dshow-protocol.md 与任务书 §纯测试 的六条）：
//   ① AddFilter(渲染器,"TextureRenderer") + AddFilter(源,NULL) 之后 Render(源 pin)：
//      `connect_hr == S_OK`，且假渲染器记录到的 AM_MEDIA_TYPE 各字段正确（pbFormat 指向的 guest
//      内存可读）；★ 源 pin **一次都没被碰**（D1：doc §2-D）。
//   ② `NotifyAllocator` 被调过、交的是**我们的**分配器、`fReadOnly == FALSE`。
//   ③ `run()` 后两个假 filter 收到 Pause（先）再 Run（后），顺序可断言。
//   ④ 假 FrameFeed（4 帧 pts 0/33/66/100 ms、4×2）+ 假时钟：33 ms 恰好 1 帧、66 ms 再 1 帧；
//      样本缓冲**自底向上**、`SetTime` 的 pts 单调；到片长后 `completed == true`。
//      （第 4 帧不是判据需要的，而是给 ⑤ 造出「有**新**帧、但分配器忙」的时刻。）
//   ⑤ 分配器忙（不 ReleaseBuffer）：第二次 Receive 不阻塞，`dropped_busy` 增长。
//   ⑥ 「会红」自检：同一批判据在**坏接线**（QI(IMemInputPin) 失败）与**逐字段改坏**的记录上
//      必须返回 false —— 否则上面那些绿是恒真的。
//
// ★ 为什么假 renderer 的 `Receive` 不自己再调一次 `GetPointer`：本模块的泵已经在一条
//   `GuestCallStack` 链里，链的 callee 里再起 guest 调用会打乱帧纪律（见 guest_invoke.h）。
//   这里改成"Receive 只记样本 EA，测试随后**从宿主正常发起** GetPointer/GetTime 的 guest 调用"
//   —— 等价地覆盖那几个槽，且更接近引擎真正读样本的方式（doc §2-C / §3-3）。
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "check.h"
#include "fixture.h"
#include "runtime/com.h"
#include "runtime/dshow/dshow.h"
#include "runtime/guest_call.h"
#include "runtime/machine.h"

namespace {

using lift_fixture::Fixture;
using lift::ShimCall;
using lift::X86;
using lift::dshow::Guid;

constexpr std::uint32_t kStubPage = 0x7FF80000u;

constexpr std::uint32_t kSOk = 0x00000000u;
constexpr std::uint32_t kSFalse = 0x00000001u;
constexpr std::uint32_t kEFail = 0x80004005u;
constexpr std::uint32_t kENoInterface = 0x80004002u;
constexpr std::uint32_t kENotImpl = 0x80004001u;
constexpr std::uint32_t kEPointer = 0x80004003u;

// `GuestCallStack::start` 的 cleanup = 4 + (this + 声明参数) * 4。
// ★ 与 shim 里的 `4 + call.stdcall_bytes` 同一个口径（com.h：argc 不含 this ⇒ 登记 argc+1）。
constexpr std::uint32_t kCleanupOneArg = 4u + 2u * 4u;  // Render(pin) / get_CurrentPosition(double*)
constexpr std::uint32_t kCleanupNoArg = 4u + 1u * 4u;   // Run / Pause / Stop

// ---------------------------------------------------------------------------------------
// GUID 记录（4 个 DWORD，正好是 guest 里的 16 字节小端布局；与 `.lst` 的 dd 写法一致）
// ---------------------------------------------------------------------------------------
struct RawGuid {
    std::uint32_t dw[4] = {};
};

// ★ 期望值一律写成**字面量**（4 个 DWORD = guest 里那 16 字节；与 `.lst` 的 `dd`/`db` 行逐字相同），
//   ⛔ 不复用 `lift::dshow::k*` —— 否则"把实现里的 GUID 常量改错"会同时改掉期望值，
//   测试永远是绿的（= 恒真守卫）。
//   证据：
//     MEDIATYPE_Video("vids") @0x52FAA8  db 'vids',0 + 10 00 80 00 00 AA 00 38 9B 71
//     MEDIASUBTYPE_RGB24      @0x52FAC8  dd 0E436EB7Dh, 11CE524Fh, 2000539Fh, 70A70BAFh
//     MEDIASUBTYPE_ARGB32     @0x52FAD8  dd 773C9AC0h, 11D03274h, 0AA0024B7h, 011A6C00h
//     FORMAT_VideoInfo        @0x52FB48  dd 05589F80h, 11CEC356h, 0AA0001BFh, 5A595500h
//   ★ 这四条正是 `CMovieTextureRenderer::CheckMediaType`（`sub_4878B0`，0x4878EA/0x4878F1/0x4878FF）
//     与 `SetMediaType`（`sub_488A80`，0x488AD8）实际比较的常量 —— 写错一个就 VFW_E_TYPE_NOT_ACCEPTED。
constexpr std::uint32_t kLitMajorVideo[4] = {0x73646976u, 0x00100000u, 0xAA000080u, 0x719B3800u};
constexpr std::uint32_t kLitSubRgb24[4] = {0xE436EB7Du, 0x11CE524Fu, 0x2000539Fu, 0x70A70BAFu};
constexpr std::uint32_t kLitSubArgb32[4] = {0x773C9AC0u, 0x11D03274u, 0x0AA0024B7u, 0x011A6C00u};
constexpr std::uint32_t kLitFormatVideoInfo[4] = {0x05589F80u, 0x11CEC356u, 0xAA0001BFu, 0x5A595500u};
// IID_IMemInputPin @0x52FCB8：56A8689D-0AD4-11CE-B03A-0020AF0BA770
constexpr std::uint32_t kLitIidMemInputPin[4] = {0x56A8689Du, 0x11CE0AD4u, 0x20003AB0u, 0x70A70BAFu};
// IID_IAsyncReader（**本引擎的键**：`.data:0x52FD18 dd 56A868AAh, 11CE0AD4h, 20003AB0h, 70A70BAFh`；
// 由 `sub_402A00`（CAsyncOutputPin::NonDelegatingQueryInterface）的 `0x402A13 push offset dword_52FD18` 取证。
// ★ 仍写成字面量（不引用 dshow::kIidAsyncReader）：否则"把实现里的常量改错"会同时改掉期望值 ⇒ 恒真。
constexpr std::uint32_t kLitIidAsyncReader[4] = {0x56A868AAu, 0x11CE0AD4u, 0x20003AB0u, 0x70A70BAFu};

RawGuid rawOfDwords(const std::uint32_t (&dw)[4]) {
    RawGuid r;
    for (int i = 0; i < 4; ++i) r.dw[i] = dw[i];
    return r;
}

bool sameGuid(const RawGuid& r, const RawGuid& w) {
    return r.dw[0] == w.dw[0] && r.dw[1] == w.dw[1] && r.dw[2] == w.dw[2] && r.dw[3] == w.dw[3];
}

// ---------------------------------------------------------------------------------------
// 假引擎侧观察到的东西
// ---------------------------------------------------------------------------------------
struct ObservedMediaType {
    std::uint32_t ea = 0;
    RawGuid major;
    RawGuid sub;
    RawGuid fmt;
    std::uint32_t cb_format = 0;
    std::uint32_t pb_format = 0;
    std::int32_t bi_width = 0;
    std::int32_t bi_height = 0;
    std::uint16_t bi_planes = 0;
    std::uint16_t bi_bitcount = 0;
    std::uint32_t bi_compression = 0;
    std::uint32_t bi_size_image = 0;
    std::int64_t avg_time = 0;
};

// 一次接线产出的"可判定记录"（⑥ 的判据函数吃它）。
struct WireRecord {
    ObservedMediaType mt;
    std::uint32_t connect_hr = 0;
    int receive_connection_calls = 0;
    int notify_calls = 0;
    std::uint32_t notify_hr = kEFail;
    std::uint32_t notify_allocator = 0;
    std::uint32_t notify_readonly = 1;
    std::uint32_t expected_allocator = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    int source_pin_calls = 0;
};

struct FakeState {
    std::uint32_t renderer = 0;
    std::uint32_t source_filter = 0;
    std::uint32_t input_pin = 0;
    std::uint32_t mem_input = 0;
    std::uint32_t source_pin = 0;
    // 假 IAsyncReader（真源读取用例）：Length 给 4，SyncRead 拷 `00 00 01 BA`。
    std::uint32_t async_reader = 0;
    std::int64_t async_total = 4;
    int sync_read_calls = 0;
    std::int64_t last_sync_read_pos = -1;
    std::uint32_t last_sync_read_len = 0;
    int async_release_calls = 0;
    // 行为开关
    std::uint32_t pin_direction = 0;  // PINDIR_INPUT
    bool qi_meminput_fails = false;   // ⑥ 的"坏接线"
    // 记录
    std::vector<std::string> calls;
    ObservedMediaType mt;
    std::uint32_t receive_connection_hr = 0xDEADBEEFu;
    int receive_connection_calls = 0;
    std::uint32_t notify_hr = kEFail;
    int notify_calls = 0;
    std::uint32_t notify_allocator = 0;
    std::uint32_t notify_readonly = 0xFFFFFFFFu;
    int receive_calls = 0;
    int source_pin_calls = 0;
    RawGuid last_qi_iid;
};

FakeState g;

void pushCall(const char* name) { g.calls.push_back(name); }
int indexOfCall(const char* name) {
    for (std::size_t i = 0; i < g.calls.size(); ++i)
        if (g.calls[i] == name) return static_cast<int>(i);
    return -1;
}

// ---------------------------------------------------------------------------------------
// 假引擎对象的 COM 方法（`ComMethod::fn` 是无上下文的函数指针 ⇒ 状态只能在文件级）
// ---------------------------------------------------------------------------------------
void fakeAddRef(X86& cpu, ShimCall& call) {
    const std::uint32_t self = call.u32(0);
    if (cpu.com == nullptr || !cpu.com->addRef(cpu, self)) {
        call.result = 0;
        return;
    }
    call.result = cpu.com->refCount(self);
}

void fakeRelease(X86& cpu, ShimCall& call) {
    const std::uint32_t self = call.u32(0);
    if (cpu.com == nullptr) {
        call.result = 0;
        return;
    }
    const std::uint32_t user = cpu.com->userOf(self);
    const std::uint32_t before = cpu.com->refCount(self);
    const bool ok = cpu.com->release(cpu, self);
    if (ok && before <= 1u && user != 0 && cpu.heap != nullptr) {
        cpu.heap->free(user);  // 枚举器的"已消费"状态块（与 objects.h 同一套纪律）
    }
    call.result = ok ? (before > 1u ? before - 1u : 0u) : 0u;
}

void fakeQueryInterfaceSelf(X86& cpu, ShimCall& call) {
    const std::uint32_t out = call.u32(2);
    if (out == 0) {
        call.result = kEPointer;
        return;
    }
    cpu.mem->wr32(out, call.u32(0));
    call.result = kSOk;
}

void fakeNotImpl(X86&, ShimCall& call) { call.result = kENotImpl; }

// ---- IBaseFilter ----
void fakeFilterPause(X86&, ShimCall& call) {
    pushCall(call.u32(0) == g.renderer ? "pause:renderer" : "pause:source");
    call.result = kSOk;
}
void fakeFilterRun(X86&, ShimCall& call) {
    pushCall(call.u32(0) == g.renderer ? "run:renderer" : "run:source");
    call.result = kSOk;
}
void fakeFilterStop(X86&, ShimCall& call) {
    pushCall(call.u32(0) == g.renderer ? "stop:renderer" : "stop:source");
    call.result = kSOk;
}
// IBaseFilter::SetSyncSource(+0x20)：真图在 Run 之前对每个 filter 调它（doc §8.10）。
void fakeFilterSetSyncSource(X86&, ShimCall& call) { call.result = kSOk; }

// ---- IEnumPins ----
void fakeEnumNext(X86& cpu, ShimCall& call) {
    const std::uint32_t self = call.u32(0);
    const std::uint32_t out_ea = call.u32(2);
    const std::uint32_t fetched_ea = call.u32(3);
    pushCall("enum:next");
    const std::uint32_t state = cpu.com != nullptr ? cpu.com->userOf(self) : 0;
    const std::uint32_t consumed = state != 0 ? cpu.mem->rd32(state) : 1u;
    if (consumed != 0 || fetched_ea == 0 || out_ea == 0) {
        if (fetched_ea != 0) cpu.mem->wr32(fetched_ea, 0);
        call.result = kSFalse;
        return;
    }
    if (state != 0) cpu.mem->wr32(state, 1u);
    cpu.mem->wr32(out_ea, g.input_pin);
    cpu.mem->wr32(fetched_ea, 1u);
    call.result = kSOk;
}

void fakeEnumPins(X86& cpu, ShimCall& call) {
    const std::uint32_t out = call.u32(1);
    pushCall("enum_pins");
    if (out == 0 || cpu.com == nullptr || cpu.heap == nullptr) {
        call.result = kEFail;
        return;
    }
    const std::uint32_t state = cpu.heap->alloc(4u, /*zero=*/true);
    static const std::vector<lift::ComMethod> methods = {
        {0x00, "QueryInterface", lift::Conv::Stdcall, 2, &fakeQueryInterfaceSelf},
        {0x04, "AddRef", lift::Conv::Stdcall, 0, &fakeAddRef},
        {0x08, "Release", lift::Conv::Stdcall, 0, &fakeRelease},
        {0x0C, "Next", lift::Conv::Stdcall, 3, &fakeEnumNext},
    };
    const std::uint32_t enumerator = cpu.com->createObject(cpu, "FakeEnumPins", methods, state);
    if (enumerator == 0) {
        call.result = kEFail;
        return;
    }
    cpu.mem->wr32(out, enumerator);
    call.result = kSOk;
}

// ---- IPin（渲染器的输入 pin）----
void fakePinReceiveConnection(X86& cpu, ShimCall& call) {
    const std::uint32_t mt_ea = call.u32(2);
    pushCall("receive_connection");
    ++g.receive_connection_calls;
    g.receive_connection_hr = kSOk;
    g.mt = ObservedMediaType{};
    g.mt.ea = mt_ea;
    if (mt_ea != 0 && cpu.mem->committed(mt_ea, lift::dshow::amt::kBytes)) {
        for (int i = 0; i < 4; ++i) g.mt.major.dw[i] = cpu.mem->rd32(mt_ea + static_cast<std::uint32_t>(i) * 4u);
        for (int i = 0; i < 4; ++i)
            g.mt.sub.dw[i] = cpu.mem->rd32(mt_ea + lift::dshow::amt::kSubType + static_cast<std::uint32_t>(i) * 4u);
        for (int i = 0; i < 4; ++i)
            g.mt.fmt.dw[i] =
                cpu.mem->rd32(mt_ea + lift::dshow::amt::kFormatType + static_cast<std::uint32_t>(i) * 4u);
        g.mt.cb_format = cpu.mem->rd32(mt_ea + lift::dshow::amt::kCbFormat);
        g.mt.pb_format = cpu.mem->rd32(mt_ea + lift::dshow::amt::kPbFormat);
        const std::uint32_t vi = g.mt.pb_format;
        if (vi != 0 && cpu.mem->committed(vi, lift::dshow::vih::kBytes)) {
            g.mt.avg_time = static_cast<std::int64_t>(cpu.mem->rd64(vi + lift::dshow::vih::kAvgTimePerFrame));
            const std::uint32_t bmi = vi + lift::dshow::vih::kBmiHeader;
            g.mt.bi_width = static_cast<std::int32_t>(cpu.mem->rd32(bmi + lift::dshow::bmi::kWidth));
            g.mt.bi_height = static_cast<std::int32_t>(cpu.mem->rd32(bmi + lift::dshow::bmi::kHeight));
            g.mt.bi_planes = cpu.mem->rd16(bmi + lift::dshow::bmi::kPlanes);
            g.mt.bi_bitcount = cpu.mem->rd16(bmi + lift::dshow::bmi::kBitCount);
            g.mt.bi_compression = cpu.mem->rd32(bmi + lift::dshow::bmi::kCompression);
            g.mt.bi_size_image = cpu.mem->rd32(bmi + lift::dshow::bmi::kSizeImage);
        }
    }
    call.result = kSOk;
}

void fakePinQueryDirection(X86& cpu, ShimCall& call) {
    const std::uint32_t out = call.u32(1);
    pushCall("query_direction");
    if (out == 0) {
        call.result = kEPointer;
        return;
    }
    cpu.mem->wr32(out, g.pin_direction);
    call.result = kSOk;
}

void fakePinQueryInterface(X86& cpu, ShimCall& call) {
    const std::uint32_t iid_ea = call.u32(1);
    const std::uint32_t out = call.u32(2);
    pushCall("pin:qi");
    if (out == 0) {
        call.result = kEPointer;
        return;
    }
    cpu.mem->wr32(out, 0);
    if (iid_ea != 0 && cpu.mem->committed(iid_ea, 16u)) {
        for (int i = 0; i < 4; ++i)
            g.last_qi_iid.dw[i] = cpu.mem->rd32(iid_ea + static_cast<std::uint32_t>(i) * 4u);
        if (!g.qi_meminput_fails && sameGuid(g.last_qi_iid, rawOfDwords(kLitIidMemInputPin))) {
            cpu.mem->wr32(out, g.mem_input);
            call.result = kSOk;
            return;
        }
    }
    call.result = kENoInterface;
}

// ---- IMemInputPin ----
void fakeNotifyAllocator(X86& cpu, ShimCall& call) {
    (void)cpu;
    pushCall("notify_allocator");
    ++g.notify_calls;
    g.notify_allocator = call.u32(1);
    g.notify_readonly = call.u32(2);
    g.notify_hr = kSOk;
    call.result = kSOk;
}

void fakeReceive(X86&, ShimCall& call) {
    pushCall("receive");
    ++g.receive_calls;
    call.result = kSOk;
}

// ---- 源的输出 pin：**只用来证明 Render 没碰它** ----
void fakeSourcePinTouched(X86&, ShimCall& call) {
    ++g.source_pin_calls;
    call.result = kSOk;
}

// ---- 源 pin 的 QI（真源读取用例）：认 IID_IAsyncReader 并交出一个 11 槽的假 IAsyncReader ----
void fakeSourcePinQueryInterface(X86& cpu, ShimCall& call) {
    const std::uint32_t iid_ea = call.u32(1);
    const std::uint32_t out = call.u32(2);
    pushCall("source_pin:qi");
    ++g.source_pin_calls;
    if (out == 0) {
        call.result = kEPointer;
        return;
    }
    cpu.mem->wr32(out, 0);
    RawGuid iid;
    if (iid_ea != 0 && cpu.mem->committed(iid_ea, 16u)) {
        for (int i = 0; i < 4; ++i) iid.dw[i] = cpu.mem->rd32(iid_ea + static_cast<std::uint32_t>(i) * 4u);
        g.last_qi_iid = iid;
        if (sameGuid(iid, rawOfDwords(kLitIidAsyncReader)) && g.async_reader != 0) {
            cpu.mem->wr32(out, g.async_reader);
            call.result = kSOk;
            return;
        }
    }
    call.result = kENoInterface;
}

// ---- 假 IAsyncReader（**本引擎的槽序**：+0x1C SyncRead、+0x20 Length；见 dshow.h）----
void fakeAsyncLength(X86& cpu, ShimCall& call) {
    const std::uint32_t total = call.u32(1);
    const std::uint32_t avail = call.u32(2);
    pushCall("async:length");
    if (total != 0) cpu.mem->wr64(total, static_cast<std::uint64_t>(g.async_total));
    if (avail != 0) cpu.mem->wr64(avail, static_cast<std::uint64_t>(g.async_total));
    call.result = kSOk;
}

void fakeAsyncSyncRead(X86& cpu, ShimCall& call) {
    const std::uint32_t pos_lo = call.u32(1);
    const std::uint32_t pos_hi = call.u32(2);
    const std::uint32_t len = call.u32(3);
    const std::uint32_t buf = call.u32(4);
    pushCall("async:syncread");
    ++g.sync_read_calls;
    const std::int64_t pos =
        static_cast<std::int64_t>((static_cast<std::uint64_t>(pos_hi) << 32) | pos_lo);
    g.last_sync_read_pos = pos;
    if (pos >= g.async_total || len == 0) {
        g.last_sync_read_len = 0;
        call.result = kSFalse;  // 到流尾
        return;
    }
    std::int64_t n = g.async_total - pos;
    if (static_cast<std::int64_t>(len) < n) n = static_cast<std::int64_t>(len);
    static const std::uint8_t kPsHeader[4] = {0x00, 0x00, 0x01, 0xBA};
    for (std::int64_t i = 0; i < n; ++i) cpu.mem->wr8(buf + static_cast<std::uint32_t>(i), kPsHeader[i & 3]);
    g.last_sync_read_len = static_cast<std::uint32_t>(n);
    call.result = kSOk;
}

void fakeAsyncRelease(X86&, ShimCall& call) {
    ++g.async_release_calls;
    call.result = 0;
}

// ---------------------------------------------------------------------------------------
// 夹具：机器 + 假引擎对象
// ---------------------------------------------------------------------------------------
struct Harness {
    lift::ComRegistry com;
    Fixture f;
    Harness() {
        f.m.shims().configureStubPage(kStubPage, 0x1000u);
        if (!f.m.heap().init(&f.m.mem(), f.m.layout().heap_base, 0x00400000u)) {
            f.ok = false;
            f.err = "guest 堆 init 失败";
            return;
        }
        f.m.cpu().com = &com;
    }

    X86& cpu() { return f.m.cpu(); }

    // 宿主发起一次会调 guest 的方法：按夹具约定铺哨兵返回地址；若期间铺了 guest 帧，
    // 就一直跑到那个哨兵（= 整条链走完）。
    bool drive(const std::function<void()>& fn) {
        f.rearmStack();
        fn();
        if (!f.m.cpu().calls->active()) return true;
        lift::RunOptions o;
        o.until_ea = lift_fixture::kSentinel;
        o.max_insns = 4000000ull;
        const lift::RunResult r = f.m.run(o);
        if (r.reason != lift::StopReason::UntilEa) {
            f.ok = false;
            f.err = std::string("意外停机：") + lift::stopReasonName(r.reason) + " — " + r.detail;
            return false;
        }
        return true;
    }

    std::uint32_t slotOf(std::uint32_t obj, std::uint32_t off) {
        return f.m.mem().rd32(f.m.mem().rd32(obj) + off);
    }

    // 从 guest 侧按 COM 约定（this 是第 0 个栈参数）调一个 vtable 槽。
    std::uint32_t callMethod(std::uint32_t obj, std::uint32_t off, std::initializer_list<std::uint32_t> args) {
        std::vector<std::uint32_t> stack_args;
        stack_args.push_back(obj);
        for (const std::uint32_t a : args) stack_args.push_back(a);
        const std::uint32_t fn = slotOf(obj, off);
        if (fn == 0) {
            f.ok = false;
            f.err = "vtable 槽为 0";
            return 0;
        }
        const std::uint32_t code_ea = f.code_cursor;
        f.code_cursor += 0x100u;
        lift_fixture::Asm a(code_ea);
        for (auto it = stack_args.rbegin(); it != stack_args.rend(); ++it) a.pushImm(*it);
        a.callRel(fn);
        a.ret();
        if (!f.m.placeCode(code_ea, a.b.data(), a.b.size(), &f.err)) {
            f.ok = false;
            return 0;
        }
        f.rearmStack();
        f.runCode(code_ea);
        // ★ stdcall 的 argc 写错时栈会漂（`ret` 落到错位置）—— 这里把它当断言钉住：
        //   正确的调用结束后 esp 恰好回到哨兵之上（= stack_top）。
        if (f.ok && f.m.cpu().esp() != f.m.layout().stack_top) {
            f.ok = false;
            f.err = "callMethod 之后 guest 栈不平衡（stdcall 的 argc / 参数个数写错？）";
        }
        return f.m.cpu().eax();
    }
};

void resetFake() { g = FakeState{}; }

void buildFakeEngine(Harness& h) {
    resetFake();
    X86& cpu = h.cpu();
    static const std::vector<lift::ComMethod> pin_methods = {
        {0x00, "QueryInterface", lift::Conv::Stdcall, 2, &fakePinQueryInterface},
        {0x04, "AddRef", lift::Conv::Stdcall, 0, &fakeAddRef},
        {0x08, "Release", lift::Conv::Stdcall, 0, &fakeRelease},
        {0x10, "ReceiveConnection", lift::Conv::Stdcall, 2, &fakePinReceiveConnection},
        {0x24, "QueryDirection", lift::Conv::Stdcall, 1, &fakePinQueryDirection},
    };
    static const std::vector<lift::ComMethod> meminput_methods = {
        {0x00, "QueryInterface", lift::Conv::Stdcall, 2, &fakeQueryInterfaceSelf},
        {0x04, "AddRef", lift::Conv::Stdcall, 0, &fakeAddRef},
        {0x08, "Release", lift::Conv::Stdcall, 0, &fakeRelease},
        {0x10, "NotifyAllocator", lift::Conv::Stdcall, 2, &fakeNotifyAllocator},
        {0x18, "Receive", lift::Conv::Stdcall, 1, &fakeReceive},
    };
    static const std::vector<lift::ComMethod> renderer_methods = {
        {0x00, "QueryInterface", lift::Conv::Stdcall, 2, &fakeQueryInterfaceSelf},
        {0x04, "AddRef", lift::Conv::Stdcall, 0, &fakeAddRef},
        {0x08, "Release", lift::Conv::Stdcall, 0, &fakeRelease},
        {0x10, "Stop", lift::Conv::Stdcall, 0, &fakeFilterStop},
        {0x14, "Pause", lift::Conv::Stdcall, 0, &fakeFilterPause},
        {0x18, "Run", lift::Conv::Stdcall, 2, &fakeFilterRun},  // this + REFERENCE_TIME(8)
        {0x20, "SetSyncSource", lift::Conv::Stdcall, 1, &fakeFilterSetSyncSource},
        {0x28, "EnumPins", lift::Conv::Stdcall, 1, &fakeEnumPins},
        {0x2C, "FindPin", lift::Conv::Stdcall, 2, &fakeNotImpl},
    };
    static const std::vector<lift::ComMethod> source_methods = {
        {0x00, "QueryInterface", lift::Conv::Stdcall, 2, &fakeQueryInterfaceSelf},
        {0x04, "AddRef", lift::Conv::Stdcall, 0, &fakeAddRef},
        {0x08, "Release", lift::Conv::Stdcall, 0, &fakeRelease},
        {0x10, "Stop", lift::Conv::Stdcall, 0, &fakeFilterStop},
        {0x14, "Pause", lift::Conv::Stdcall, 0, &fakeFilterPause},
        {0x18, "Run", lift::Conv::Stdcall, 2, &fakeFilterRun},
        {0x20, "SetSyncSource", lift::Conv::Stdcall, 1, &fakeFilterSetSyncSource},
    };
    static const std::vector<lift::ComMethod> srcpin_methods = {
        {0x00, "QueryInterface", lift::Conv::Stdcall, 2, &fakeSourcePinQueryInterface},
        {0x04, "AddRef", lift::Conv::Stdcall, 0, &fakeAddRef},
        {0x08, "Release", lift::Conv::Stdcall, 0, &fakeRelease},
        {0x10, "ReceiveConnection", lift::Conv::Stdcall, 2, &fakeSourcePinTouched},
        {0x24, "QueryDirection", lift::Conv::Stdcall, 1, &fakeSourcePinTouched},
    };
    // 11 槽 = IUnknown(3) + IAsyncReader 的 8 个方法。★ 槽序**照本引擎的 vtable**（0x51D95C）：
    // +0x18 SyncReadAligned、+0x1C SyncRead、+0x20 Length；其余槽给 E_NOTIMPL（不会被调到）。
    static const std::vector<lift::ComMethod> async_methods = {
        {0x00, "QueryInterface", lift::Conv::Stdcall, 2, &fakeQueryInterfaceSelf},
        {0x04, "AddRef", lift::Conv::Stdcall, 0, &fakeAddRef},
        {0x08, "Release", lift::Conv::Stdcall, 0, &fakeAsyncRelease},
        {0x0C, "AsyncSlot0C", lift::Conv::Stdcall, 3, &fakeNotImpl},
        {0x10, "AsyncSlot10", lift::Conv::Stdcall, 2, &fakeNotImpl},
        {0x14, "AsyncSlot14", lift::Conv::Stdcall, 3, &fakeNotImpl},
        {0x18, "SyncReadAligned", lift::Conv::Stdcall, 1, &fakeNotImpl},
        {0x1C, "SyncRead", lift::Conv::Stdcall, 4, &fakeAsyncSyncRead},  // this + llPos(2) + len + buf
        {0x20, "Length", lift::Conv::Stdcall, 2, &fakeAsyncLength},
        {0x24, "AsyncSlot24", lift::Conv::Stdcall, 0, &fakeNotImpl},
        {0x28, "AsyncSlot28", lift::Conv::Stdcall, 0, &fakeNotImpl},
    };

    g.input_pin = h.com.createObject(cpu, "FakePin", pin_methods);
    g.mem_input = h.com.createObject(cpu, "FakeMemInputPin", meminput_methods);
    g.renderer = h.com.createObject(cpu, "FakeRenderer", renderer_methods);
    g.source_filter = h.com.createObject(cpu, "FakeSourceFilter", source_methods);
    g.source_pin = h.com.createObject(cpu, "FakeSourcePin", srcpin_methods);
    g.async_reader = h.com.createObject(cpu, "FakeAsyncReader", async_methods);
}

// ---------------------------------------------------------------------------------------
// 假帧源：4×2、4 帧、pts 0 / 33.3 / 66.6 / 100 ms；每像素编码「帧号 + 行号」⇒ 可判行序
// ---------------------------------------------------------------------------------------
class FakeFeed : public lift::dshow::FrameFeed {
public:
    static constexpr std::uint32_t kW = 4;
    static constexpr std::uint32_t kH = 2;
    static constexpr int kFrames = 4;
    static constexpr std::int64_t kDuration = 1333332;  // ~133 ms（第 4 帧之后）
    static std::int64_t ptsOf(int i) { return static_cast<std::int64_t>(i) * 333333; }

    bool info(std::uint32_t* w, std::uint32_t* h, std::int64_t* duration_100ns) override {
        *w = kW;
        *h = kH;
        *duration_100ns = kDuration;
        return true;
    }
    bool at(std::int64_t now_100ns, const std::uint8_t** rgb24, std::uint32_t* stride, std::int64_t* pts_100ns,
            bool* ended) override {
        int idx = -1;
        for (int i = 0; i < kFrames; ++i)
            if (ptsOf(i) <= now_100ns) idx = i;
        if (idx < 0) {
            *ended = false;
            return false;
        }
        fill(idx);
        *rgb24 = frame_.data();
        *stride = kW * 3u;
        *pts_100ns = ptsOf(idx);
        *ended = false;
        last_index_ = idx;
        return true;
    }
    bool seek(std::int64_t) override { return true; }
    void rewind() override { last_index_ = -1; }
    int lastIndex() const { return last_index_; }
    static std::uint8_t value(int frame_index, int y) {
        return static_cast<std::uint8_t>(0x10 * (frame_index + 1) + y);
    }
    // 每个像素三通道**不同**（R=value+0、G=+1、B=+2），这样"通道序写错"才会红：
    // 样本缓冲要求 **BGR**（Windows BI_RGB 24bpp），读成 DWORD = (v+2)<<16 | (v+1)<<8 | v。
    static std::uint32_t bgrPixels(int frame_index, int y) {
        const std::uint32_t v = value(frame_index, y);
        return ((v + 2u) << 16) | ((v + 1u) << 8) | v;
    }

private:
    void fill(int idx) {
        frame_.assign(kW * kH * 3u, 0);
        for (std::uint32_t y = 0; y < kH; ++y)
            for (std::uint32_t x = 0; x < kW; ++x)
                for (std::uint32_t c = 0; c < 3u; ++c)
                    frame_[(y * kW + x) * 3u + c] = static_cast<std::uint8_t>(value(idx, static_cast<int>(y)) + c);
    }
    std::vector<std::uint8_t> frame_;
    int last_index_ = -1;
};

// ---------------------------------------------------------------------------------------
// 判据函数（⑥ 要证明它们**真的会红**）
// ---------------------------------------------------------------------------------------
bool mediaTypeOk(const ObservedMediaType& o, std::uint32_t w, std::uint32_t h) {
    const std::uint32_t stride = (w * 3u + 3u) & ~3u;
    return sameGuid(o.major, rawOfDwords(kLitMajorVideo)) &&
           sameGuid(o.sub, rawOfDwords(kLitSubRgb24)) && sameGuid(o.fmt, rawOfDwords(kLitFormatVideoInfo)) &&
           o.cb_format == 88u && o.pb_format != 0u && o.bi_width == static_cast<std::int32_t>(w) &&
           o.bi_height == static_cast<std::int32_t>(h) && o.bi_planes == 1u && o.bi_bitcount == 24u &&
           o.bi_compression == 0u && o.bi_size_image == stride * h && o.avg_time == 333333;
}

bool wireOk(const WireRecord& r) {
    return r.connect_hr == kSOk && r.receive_connection_calls == 1 && r.notify_calls == 1 &&
           r.notify_hr == kSOk && r.notify_allocator == r.expected_allocator && r.notify_allocator != 0 &&
           r.notify_readonly == 0 && mediaTypeOk(r.mt, r.width, r.height) && r.source_pin_calls == 0;
}

WireRecord currentRecord(const lift::dshow::Stats& stats, std::uint32_t w, std::uint32_t h) {
    WireRecord r;
    r.mt = g.mt;
    r.connect_hr = stats.connect_hr;
    r.receive_connection_calls = g.receive_connection_calls;
    r.notify_calls = g.notify_calls;
    r.notify_hr = stats.notify_allocator_hr;
    r.notify_allocator = g.notify_allocator;
    r.notify_readonly = g.notify_readonly;
    r.expected_allocator = stats.allocator_ea;
    r.width = w;
    r.height = h;
    r.source_pin_calls = g.source_pin_calls;
    return r;
}

// 读样本像素：宿主发起 `IMediaSample::GetPointer`（+0x0C）→ 按 stride*height 拷出来。
bool readSamplePixels(Harness& h, std::uint32_t sample, std::uint32_t stride, std::uint32_t height,
                      std::vector<std::uint8_t>* out) {
    const std::uint32_t scratch = h.f.zeroData(4);
    const std::uint32_t hr = h.callMethod(sample, 0x0C, {scratch});
    if (hr != kSOk || !h.f.ok) return false;
    const std::uint32_t ptr = h.f.m.mem().rd32(scratch);
    if (ptr == 0) return false;
    out->assign(static_cast<std::size_t>(stride) * height, 0);
    for (std::uint32_t i = 0; i < stride * height; ++i) (*out)[i] = h.f.m.mem().rd8(ptr + i);
    return true;
}

}  // namespace

int main() {
    using namespace lift_test;

    Harness h;
    if (!h.f.ok) {
        std::fprintf(stderr, "夹具失败：%s\n", h.f.err.c_str());
        LIFT_CHECK(h.f.ok);
        return finish("dshow-graph");
    }
    buildFakeEngine(h);

    std::int64_t clock100ns = 0;
    FakeFeed feed;
    lift::dshow::Graph graph(h.cpu(), [&clock100ns]() { return clock100ns; }, &feed);
    const std::uint32_t stack_top = h.f.m.layout().stack_top;

    // -- ① AddFilter 识别：渲染器按名字、源按「第二个 + 名字为空」--
    const char16_t renderer_name[] = u"TextureRenderer";
    const std::uint32_t renderer_name_ea = h.f.wideCstr(renderer_name, 16);
    {
        g_current_case = "dshow-add-filter";
        LIFT_CHECK(graph.addFilter(g.renderer, renderer_name_ea, &h.f.err));
        LIFT_CHECK(graph.addFilter(g.source_filter, 0, &h.f.err));
        LIFT_CHECK(!graph.opened());  // 还没 Render
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(graph.state()), 0u);
        LIFT_CHECK(graph.stats().connect_hr == 0u);  // 接线之前是初值（不是"成功"）
        LIFT_CHECK(graph.stats().notify_allocator_hr == kEFail);
    }

    // -- ①② Render：D1 接线 + 媒体类型 + NotifyAllocator --
    {
        g_current_case = "dshow-render-connect";
        graph.setHostCallCleanup(kCleanupOneArg);
        LIFT_CHECK(h.drive([&] { graph.render(g.source_pin, &h.f.err); }));
        LIFT_CHECK(h.f.ok);
        // ★ 栈平衡：链尾把 guest 栈抬回调用点（cleanup = 4 + (this + argc) * 4）
        LIFT_CHECK_EQ_U32(h.cpu().esp(), stack_top - 4u + kCleanupOneArg);
        LIFT_CHECK(!graph.suspended());

        LIFT_CHECK_EQ_U32(graph.stats().connect_hr, kSOk);
        LIFT_CHECK_EQ_U32(graph.stats().pin_count, 1u);
        LIFT_CHECK_EQ_U32(graph.stats().input_pin_ea, g.input_pin);
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(g.receive_connection_calls), 1u);
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(g.source_pin_calls), 0u);  // ★ D1：源 pin 一次都没碰
        LIFT_CHECK(graph.opened());
        // QI 用的 IID 必须是 `.data:0x52FCB8` 那个（56A8689D-…）
        LIFT_CHECK(sameGuid(g.last_qi_iid, rawOfDwords(kLitIidMemInputPin)));

        // 媒体类型逐字段（全部从 guest 内存读回）
        LIFT_CHECK(sameGuid(g.mt.major, rawOfDwords(kLitMajorVideo)));
        LIFT_CHECK(sameGuid(g.mt.sub, rawOfDwords(kLitSubRgb24)));
        LIFT_CHECK(!sameGuid(g.mt.sub, rawOfDwords(kLitSubArgb32)));  // ★ 不是 ARGB32（否则 4 字节/像素）
        LIFT_CHECK(sameGuid(g.mt.fmt, rawOfDwords(kLitFormatVideoInfo)));
        LIFT_CHECK_EQ_U32(g.mt.cb_format, 88u);  // sizeof(VIDEOINFOHEADER)
        LIFT_CHECK_NE_U32(g.mt.pb_format, 0u);   // 渲染器第一件事就查它
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(g.mt.bi_width), 4u);
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(g.mt.bi_height), 2u);  // ★ 正数
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(g.mt.bi_planes), 1u);
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(g.mt.bi_bitcount), 24u);
        LIFT_CHECK_EQ_U32(g.mt.bi_compression, 0u);
        LIFT_CHECK_EQ_U32(g.mt.bi_size_image, 24u);  // stride(12) * h(2)
        LIFT_CHECK_EQ_I64(g.mt.avg_time, 333333);    // 1e7/30（FrameFeed 不给 fps）
        LIFT_CHECK_EQ_U32(lift::dshow::strideForRgb24(4), 12u);
        LIFT_CHECK(mediaTypeOk(g.mt, 4, 2));

        // ② NotifyAllocator：被调、交的是我们的分配器、fReadOnly == FALSE
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(g.notify_calls), 1u);
        LIFT_CHECK_EQ_U32(graph.stats().notify_allocator_hr, kSOk);
        LIFT_CHECK_EQ_U32(g.notify_allocator, graph.stats().allocator_ea);
        LIFT_CHECK_NE_U32(graph.stats().allocator_ea, 0u);
        LIFT_CHECK_EQ_U32(g.notify_readonly, 0u);
        LIFT_CHECK_EQ_U32(graph.stats().media_type_ea, g.mt.ea);
        LIFT_CHECK(wireOk(currentRecord(graph.stats(), 4, 2)));
    }

    // -- ⑥a 「会红」自检：把同一份记录逐字段改坏 ⇒ 判据必须返回 false --
    {
        g_current_case = "dshow-guard-goes-red";
        const WireRecord good = currentRecord(graph.stats(), 4, 2);
        LIFT_CHECK(wireOk(good));

        WireRecord bad = good;
        bad.mt.major.dw[0] ^= 1u;  // majortype 改错
        LIFT_CHECK(!wireOk(bad));

        bad = good;
        bad.mt.sub = rawOfDwords(kLitSubArgb32);  // 换成 ARGB32 ⇒ 渲染器会按 4 字节/像素
        LIFT_CHECK(!wireOk(bad));

        bad = good;
        bad.mt.bi_height = -2;  // 负高（引擎允许，但宿主必须传正数）
        LIFT_CHECK(!wireOk(bad));

        bad = good;
        bad.mt.avg_time = 400000;
        LIFT_CHECK(!wireOk(bad));

        bad = good;
        bad.mt.pb_format = 0;  // 渲染器会直接失败（doc §3-2 第一条）
        LIFT_CHECK(!wireOk(bad));

        bad = good;
        bad.notify_calls = 0;  // 没调 NotifyAllocator
        LIFT_CHECK(!wireOk(bad));

        bad = good;
        bad.notify_readonly = 1;  // fReadOnly 错
        LIFT_CHECK(!wireOk(bad));

        bad = good;
        bad.notify_allocator = 0xDEADBEEFu;  // 交的不是我们的分配器
        LIFT_CHECK(!wireOk(bad));

        bad = good;
        bad.connect_hr = kEFail;
        LIFT_CHECK(!wireOk(bad));

        bad = good;
        bad.source_pin_calls = 1;  // 碰了源 pin（D1 违规）
        LIFT_CHECK(!wireOk(bad));
    }

    // -- ③ 控制顺序：Pause 先、Run 后；渲染器最后 Run（Running 才真渲染）--
    {
        g_current_case = "dshow-control-order";
        graph.setHostCallCleanup(kCleanupNoArg);
        LIFT_CHECK(h.drive([&] { graph.run(); }));
        LIFT_CHECK(h.f.ok);
        const int pr = indexOfCall("pause:renderer");
        const int ps = indexOfCall("pause:source");
        const int rs = indexOfCall("run:source");
        LIFT_CHECK(pr >= 0);
        LIFT_CHECK(ps >= 0);
        LIFT_CHECK(rs >= 0);
        LIFT_CHECK(pr < ps);  // Pause(渲染器) 在 Pause(源) 之前
        LIFT_CHECK(ps < rs);  // Pause 在 Run 之前
        // ★ 2026-11-07：渲染器**也要 Run**（Paused 下引擎只把样本挂起、由服务线程渲染；
        //   Running 下才按流时间自己渲染；否则纹理永远黑 —— 见 doc §8.13）。
        const int rr = indexOfCall("run:renderer");
        LIFT_CHECK(rr >= 0);
        LIFT_CHECK(rs < rr);  // 先 Run 源、再 Run 渲染器
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(graph.state()), 2u);  // State_Running

        LIFT_CHECK(h.drive([&] { graph.pause(); }));
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(graph.state()), 1u);
        LIFT_CHECK(std::fabs(graph.currentPosition()) < 1e-9);

        LIFT_CHECK(h.drive([&] { graph.stop(); }));
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(graph.state()), 0u);
        const int sr = indexOfCall("stop:renderer");
        const int ss = indexOfCall("stop:source");
        LIFT_CHECK(sr >= 0 && ss >= 0 && sr < ss);

        // 循环片：Stop → SetPosition(0) → Run ⇒ 再 Run 一次必须重新计时到 0
        LIFT_CHECK(h.drive([&] { graph.run(); }));
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(graph.state()), 2u);
        LIFT_CHECK(std::fabs(graph.currentPosition()) < 1e-9);

        graph.setVolumeDb(-6.0);  // IBasicAudio::put_Volume 只记账（百分之一分贝）
        LIFT_CHECK_EQ_I64(graph.volumeDb100(), -600);
    }

    // -- ④ 泵：33 ms 恰好 1 帧、66 ms 再 1 帧；自底向上；pts 单调 --
    {
        g_current_case = "dshow-pump-frames";
        graph.setHostCallCleanup(kCleanupOneArg);
        clock100ns = 0;
        LIFT_CHECK(h.drive([&] { graph.currentPosition(); }));
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(graph.stats().samples_delivered), 0u);  // t=0 不送

        clock100ns = 333333;
        LIFT_CHECK(h.drive([&] { graph.currentPosition(); }));
        LIFT_CHECK(h.f.ok);
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(graph.stats().samples_delivered), 1u);
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(graph.stats().receive_calls), 1u);
        LIFT_CHECK_EQ_I64(graph.stats().last_pts_100ns, 333333);
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(g.receive_calls), 1u);
        LIFT_CHECK(std::fabs(graph.currentPosition() - 0.0333333) < 1e-6);

        std::vector<std::uint8_t> px;
        LIFT_CHECK(readSamplePixels(h, graph.stats().sample_ea, 12u, 2u, &px));
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(px.size()), 24u);
        // ★ 自底向上：缓冲第 0 行 = 源的第 h-1 行；第 1 行 = 源的第 0 行
        // ★ 通道序：样本缓冲是 **BGR**（Windows BI_RGB 24bpp）⇒ 第 0 字节 = B = v+2。
        LIFT_CHECK_EQ_U32(px[0], static_cast<std::uint32_t>(FakeFeed::value(1, 1) + 2));   // B
        LIFT_CHECK_EQ_U32(px[1], static_cast<std::uint32_t>(FakeFeed::value(1, 1) + 1));   // G
        LIFT_CHECK_EQ_U32(px[2], static_cast<std::uint32_t>(FakeFeed::value(1, 1)));       // R
        LIFT_CHECK_EQ_U32(px[12], static_cast<std::uint32_t>(FakeFeed::value(1, 0) + 2));  // 下一行 B
        LIFT_CHECK_EQ_U32(h.callMethod(graph.stats().sample_ea, 0x10, {}), 24u);  // GetSize
        LIFT_CHECK_EQ_U32(h.callMethod(graph.stats().sample_ea, 0x2C, {}), 24u);  // GetActualDataLength
        const std::uint32_t t0 = h.f.zeroData(8);
        const std::uint32_t t1 = h.f.zeroData(8);
        LIFT_CHECK_EQ_U32(h.callMethod(graph.stats().sample_ea, 0x14, {t0, t1}), kSOk);  // GetTime
        const std::int64_t st0 = static_cast<std::int64_t>(h.f.m.mem().rd64(t0));
        const std::int64_t st1 = static_cast<std::int64_t>(h.f.m.mem().rd64(t1));
        // ★ 2026-11-07：`SetTime` 是**流时间（相对片头）**，不是绝对时钟：
        //   渲染器算"何时渲染"用 `m_rtStart + m_tStart`（`sub_4D5E50`），其中 `m_tStart` 是
        //   `IBaseFilter::Run(tStart)` 传进去的起始时钟 ⇒ 样本时间必须相对，否则和会变成 2×时钟
        //   （实测：纹理永远黑）。doc §8.10。
        LIFT_CHECK_EQ_I64(st0, 333333);
        LIFT_CHECK_EQ_I64(st1 - st0, 333333);
        // `GetMediaTime(+0x34)` 仍是**相对片头**的 pts（与 SetTime 的时钟域解耦）。
        const std::uint32_t m0 = h.f.zeroData(8);
        const std::uint32_t m1 = h.f.zeroData(8);
        LIFT_CHECK_EQ_U32(h.callMethod(graph.stats().sample_ea, 0x34, {m0, m1}), kSOk);  // GetMediaTime
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(h.f.m.mem().rd64(m0)), 333333);
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(h.f.m.mem().rd64(m1)), 333333 + 333333);

        clock100ns = 666666;
        LIFT_CHECK(h.drive([&] { graph.currentPosition(); }));
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(graph.stats().samples_delivered), 2u);
        LIFT_CHECK_EQ_I64(graph.stats().last_pts_100ns, 666666);
        LIFT_CHECK(readSamplePixels(h, graph.stats().sample_ea, 12u, 2u, &px));
        LIFT_CHECK_EQ_U32(px[0], static_cast<std::uint32_t>(FakeFeed::value(2, 1) + 2));  // 换到第 3 帧
        LIFT_CHECK_EQ_U32(px[12], static_cast<std::uint32_t>(FakeFeed::value(2, 0) + 2));
        const std::uint32_t m2 = h.f.zeroData(8);
        LIFT_CHECK_EQ_U32(h.callMethod(graph.stats().sample_ea, 0x34, {m2, h.f.zeroData(8)}), kSOk);
        const std::int64_t pts2 = static_cast<std::int64_t>(h.f.m.mem().rd64(m2));
        LIFT_CHECK_EQ_I64(pts2, 666666);
        LIFT_CHECK(pts2 > 333333);  // pts 单调

        // 同一拍再问一次：不重复送
        LIFT_CHECK(h.drive([&] { graph.currentPosition(); }));
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(graph.stats().samples_delivered), 2u);
    }

    // -- ⑤ 分配器忙：不阻塞、dropped_busy 增长；交回缓冲后继续 --
    {
        g_current_case = "dshow-allocator-busy";
        const std::uint32_t allocator = graph.stats().allocator_ea;
        const std::uint32_t out = h.f.zeroData(4);
        // guest 从 vtable 调 GetBuffer（+0x1C，4 个参数）：唯一缓冲借出（busy=1）
        LIFT_CHECK_EQ_U32(h.callMethod(allocator, 0x1C, {out, 0u, 0u, 0u}), kSOk);
        const std::uint32_t borrowed = h.f.m.mem().rd32(out);
        LIFT_CHECK_EQ_U32(borrowed, graph.stats().sample_ea);

        const std::uint32_t before_receive = static_cast<std::uint32_t>(graph.stats().receive_calls);
        const std::uint32_t before_delivered = static_cast<std::uint32_t>(graph.stats().samples_delivered);

        clock100ns = 999999;  // 第 4 帧；还没到片长
        LIFT_CHECK(h.drive([&] { graph.currentPosition(); }));  // ★ 必须立即返回（不阻塞）
        LIFT_CHECK(h.f.ok);
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(graph.stats().receive_calls), before_receive);
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(graph.stats().samples_delivered), before_delivered);
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(graph.stats().dropped_busy), 1u);

        // 交回缓冲 + 放掉 GetBuffer 那次 AddRef ⇒ 下一次泵能送
        LIFT_CHECK_EQ_U32(h.callMethod(allocator, 0x20, {borrowed}), kSOk);  // ReleaseBuffer
        // ★ 期望 2：宿主在 HostObjects::create 里**自持一份**额外引用（防引擎 Release 把对象销毁）。
        LIFT_CHECK_EQ_U32(h.callMethod(borrowed, 0x08, {}), 2u);            // Release（回到宿主那份）

        clock100ns = 1033332;
        LIFT_CHECK(h.drive([&] { graph.currentPosition(); }));
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(graph.stats().samples_delivered), before_delivered + 1);
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(graph.stats().dropped_busy), 1u);  // 只丢过那一帧
    }

    // -- ⑥b 到片长 ⇒ completed + 状态回 Stopped + 位置夹到片长 --
    {
        g_current_case = "dshow-complete";
        clock100ns = 4000000;  // 4 s，远超 ~133 ms 片长
        const double p = graph.currentPosition();
        LIFT_CHECK(graph.stats().completed);
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(graph.state()), 0u);
        LIFT_CHECK(std::fabs(p - 0.1333332) < 1e-9);  // min(rel, duration)
        LIFT_CHECK(std::fabs(graph.duration() - 0.1333332) < 1e-9);
        LIFT_CHECK(std::fabs(graph.stopPosition() - 0.1333332) < 1e-9);
        LIFT_CHECK(std::fabs(graph.currentPosition()) < 1e-9);  // Stopped 之后恒 0
    }

    // -- ⑥c 「会红」的**真实**坏接线：QI(IMemInputPin) 失败 ⇒ NotifyAllocator 根本到不了 --
    {
        g_current_case = "dshow-broken-wiring";
        resetFake();
        buildFakeEngine(h);
        g.qi_meminput_fails = true;
        graph.setHostCallCleanup(kCleanupOneArg);
        LIFT_CHECK(h.drive([&] { graph.render(g.source_pin, &h.f.err); }));
        LIFT_CHECK(h.f.ok);
        // 连接这一步**看起来成功**（这正是 doc §2-D 说的危险：引擎丢掉 Render 的 HRESULT）
        LIFT_CHECK_EQ_U32(graph.stats().connect_hr, kSOk);
        // 但 NotifyAllocator 没发生：stats 必须暴露它（否则引擎会去走 CMemAllocator 的信号量）
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(g.notify_calls), 0u);
        LIFT_CHECK_EQ_U32(graph.stats().notify_allocator_hr, kEFail);
        LIFT_CHECK(!graph.suspended());
        // 同一批判据在坏接线上必须红 ⇒ 上面 ①/② 的绿不是恒真
        LIFT_CHECK(!wireOk(currentRecord(graph.stats(), 4, 2)));
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(h.f.ok), 1u);
    }

    // -- ⑦ 真源读取：假 IAsyncReader（Length=4、SyncRead 拷 `00 00 01 BA`）--
    //    判据：QI 拿到 IAsyncReader、Length 的 total/available 原样保留、SyncRead 只调一次、
    //    宿主**真的读满 4 字节**（first4 = 0xBA010000 = MPEG-PS pack start code）。
    //    4 字节垃圾 ⇒ `MovieDecoder::open` 失败 ⇒ Warn + 无帧 + 不崩（保持"零长"行为，boot 继续）。
    //    ★「会红」：把 Length 的实现改坏（total 恒 0 / 不读 pTotal）⇒ source_total 断言必红；
    //      把 SyncRead 的块长改坏 ⇒ source_bytes_read / source_chunks / first4 必红。
    {
        g_current_case = "dshow-source-read";
        resetFake();
        buildFakeEngine(h);
        g.async_total = 4;
        h.f.err.clear();
        lift::dshow::Graph graph2(h.cpu(), [&clock100ns]() { return clock100ns; }, /*feed=*/nullptr);
        graph2.setHostCallCleanup(kCleanupOneArg);
        const std::uint32_t rn2 = h.f.wideCstr(renderer_name, 16);
        LIFT_CHECK(graph2.addFilter(g.renderer, rn2, &h.f.err));
        LIFT_CHECK(graph2.addFilter(g.source_filter, 0, &h.f.err));
        LIFT_CHECK(h.drive([&] { graph2.render(g.source_pin, &h.f.err); }));
        LIFT_CHECK(h.f.ok);
        LIFT_CHECK(!graph2.suspended());
        LIFT_CHECK_EQ_U32(graph2.stats().source_qi_hr, kSOk);
        LIFT_CHECK_EQ_U32(graph2.stats().source_pin, g.source_pin);
        LIFT_CHECK_EQ_U32(graph2.stats().source_iasync_ea, g.async_reader);
        LIFT_CHECK_EQ_U32(graph2.stats().source_vtable_ea, h.f.m.mem().rd32(g.async_reader));
        LIFT_CHECK_EQ_U32(graph2.stats().source_length_hr, kSOk);
        LIFT_CHECK_EQ_I64(graph2.stats().source_total, 4);
        LIFT_CHECK_EQ_I64(graph2.stats().source_available, 4);
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(graph2.stats().source_bytes_read), 4u);
        LIFT_CHECK_EQ_U32(graph2.stats().source_chunks, 1u);
        LIFT_CHECK_EQ_U32(graph2.stats().source_first4, 0xBA010000u);
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(g.sync_read_calls), 1u);
        LIFT_CHECK(g.last_sync_read_pos == 0 && g.last_sync_read_len == 4u);
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(g.async_release_calls), 1u);  // 读完把引用还掉
        LIFT_CHECK(!graph2.stats().decoder_opened);                               // 垃圾字节打不开
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(graph2.stats().samples_delivered), 0u);
        LIFT_CHECK_EQ_U32(graph2.stats().connect_hr, kEFail);  // 无帧源 ⇒ 没接线（但没崩）
    }

    return finish("dshow-graph");
}
