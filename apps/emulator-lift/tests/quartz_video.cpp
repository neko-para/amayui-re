// emulator-lift · DirectShow / AMultiMediaStream 视频形状层夹具（pure：guest 侧自造，不碰游戏）
//
// 判据（用户 2026-11-06：「准备播放视频时停在 ole32!CoCreateInstance」）：
//   ① `CoCreateInstance(CLSID_FilterGraph, IID_IGraphBuilder)` ⇒ S_OK + guest 可见的 IGraphBuilder；
//   ② 图上的 QI 能拿到 IMediaControl / IMediaEvent / IMediaSeeking；
//   ③ `IMediaControl::Run(+0x1C)` ⇒ S_OK、`GetState(+0x28)` = 2、`Stop(+0x24)` = 0；
//   ④ `IMediaEvent::WaitForCompletion(+0x24)` **立即**返回 S_OK 且事件码 = EC_COMPLETE(1)；
//   ⑤ `IMediaSeeking::GetDuration(+0x28)` / `GetCurrentPosition(+0x30)` = 0；
//   ⑥ `CoCreateInstance(CLSID_AMMultiMediaStream, IID_IAMMultiMediaStream)` ⇒ S_OK；
//      `Initialize(+0x30)` / `GetFilterGraph(+0x34)` / `AddMediaStream(+0x3C)` 不失败；
//   ⑦ 未知 CLSID ⇒ CLASS_E_CLASSNOTAVAILABLE(0x80040111) 且输出置 0；未知 IID ⇒ E_NOINTERFACE。
//   ⛔ 这些不是"真解码正确"；它们只证明"影片准备不再 loud trap，且按零长影片立即完成"。
#include <cstdint>
#include <cstring>
#include <string>

#include "check.h"
#include "fixture.h"
#include "runtime/com.h"
#include "shims/register.h"

namespace {

using lift_fixture::Fixture;

constexpr std::uint32_t kSOk = 0x00000000u;
constexpr std::uint32_t kENoInterface = 0x80004002u;
constexpr std::uint32_t kClassEClassNotAvailable = 0x80040111u;

// CLSID_FilterGraph {E436EBB3-524F-11CE-9F53-0020AF0BA770}
const std::uint8_t kClsidFilterGraph[16] = {0xB3, 0xEB, 0x36, 0xE4, 0x4F, 0x52, 0xCE, 0x11,
                                            0x9F, 0x53, 0x00, 0x20, 0xAF, 0x0B, 0xA7, 0x70};
// CLSID_FilterGraphNoThread {E436EBB2-524F-11CE-9F53-0020AF0BA770}
const std::uint8_t kClsidFilterGraphNoThread[16] = {0xB2, 0xEB, 0x36, 0xE4, 0x4F, 0x52, 0xCE, 0x11,
                                                    0x9F, 0x53, 0x00, 0x20, 0xAF, 0x0B, 0xA7, 0x70};
// IID_IFilterGraph {56A868A3-0AD4-11CE-B03A-0020AF0BA770}
const std::uint8_t kIidFilterGraph[16] = {0xA3, 0x68, 0xA8, 0x56, 0xD4, 0x0A, 0xCE, 0x11,
                                          0xB0, 0x3A, 0x00, 0x20, 0xAF, 0x0B, 0xA7, 0x70};
// CLSID_AMMultiMediaStream {49C47CE5-9BA4-11D0-8212-00C04FC32C45}
const std::uint8_t kClsidAmMultiMediaStream[16] = {0xE5, 0x7C, 0xC4, 0x49, 0xA4, 0x9B, 0xD0, 0x11,
                                                   0x82, 0x12, 0x00, 0xC0, 0x4F, 0xC3, 0x2C, 0x45};
// IID_IGraphBuilder {56A868A9-0AD4-11CE-B03A-0020AF0BA770}
const std::uint8_t kIidGraphBuilder[16] = {0xA9, 0x68, 0xA8, 0x56, 0xD4, 0x0A, 0xCE, 0x11,
                                           0xB0, 0x3A, 0x00, 0x20, 0xAF, 0x0B, 0xA7, 0x70};
// IID_IMediaControl {56A868B1-...}
const std::uint8_t kIidMediaControl[16] = {0xB1, 0x68, 0xA8, 0x56, 0xD4, 0x0A, 0xCE, 0x11,
                                           0xB0, 0x3A, 0x00, 0x20, 0xAF, 0x0B, 0xA7, 0x70};
// IID_IMediaEvent {56A868B6-...}
const std::uint8_t kIidMediaEvent[16] = {0xB6, 0x68, 0xA8, 0x56, 0xD4, 0x0A, 0xCE, 0x11,
                                         0xB0, 0x3A, 0x00, 0x20, 0xAF, 0x0B, 0xA7, 0x70};
// IID_IBasicVideo {56A868B5-...}
const std::uint8_t kIidBasicVideo[16] = {0xB5, 0x68, 0xA8, 0x56, 0xD4, 0x0A, 0xCE, 0x11,
                                         0xB0, 0x3A, 0x00, 0x20, 0xAF, 0x0B, 0xA7, 0x70};
// IID_IMediaSeeking {56A868B3-...}
const std::uint8_t kIidMediaSeeking[16] = {0xB3, 0x68, 0xA8, 0x56, 0xD4, 0x0A, 0xCE, 0x11,
                                           0xB0, 0x3A, 0x00, 0x20, 0xAF, 0x0B, 0xA7, 0x70};
// IID_IAMMultiMediaStream {BEBE595C-9A6F-11D0-8FDE-00C04FD9189D}
const std::uint8_t kIidAmMultiMediaStream[16] = {0x5C, 0x59, 0xBE, 0xBE, 0x6F, 0x9A, 0xD0, 0x11,
                                                 0x8F, 0xDE, 0x00, 0xC0, 0x4F, 0xD9, 0x18, 0x9D};
// 一个无关 IID {11..}
const std::uint8_t kIidBogus[16] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
                                    0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x01};

std::uint32_t vtblOf(Fixture& f, std::uint32_t obj) { return f.m.mem().rd32(obj); }
std::uint32_t methodEa(Fixture& f, std::uint32_t obj, std::uint32_t offset) {
    return f.m.mem().rd32(vtblOf(f, obj) + offset);
}

}  // namespace

int main() {
    using namespace lift_test;

    lift::ComRegistry com;
    Fixture f;
    f.m.shims().configureStubPage(0x7FF80000u, 0x1000u);
    lift::registerQuartzVideoShims(f.m.shims());
    if (!f.m.heap().init(&f.m.mem(), f.m.layout().heap_base, 0x00400000u)) {
        std::fprintf(stderr, "heap init failed\n");
        return 1;
    }
    f.m.cpu().com = &com;
    const std::uint32_t co = f.use("ole32", "CoCreateInstance");
    LIFT_CHECK(f.ok);

    // ★ 每次调用都检查"栈平衡"：stdcall 的 argc 写错时，`ret` 会落在错位置或 esp 漂移；
    //   只看 EAX 会把这种错误放过去（tests/shim_abi.cpp 的同一口径）。
    auto invoke = [&](std::uint32_t ea, std::initializer_list<std::uint32_t> args) -> std::uint32_t {
        const std::uint32_t r = f.call(ea, args);
        LIFT_CHECK(f.ok);
        LIFT_CHECK_EQ_U32(f.m.cpu().esp(), f.m.layout().stack_top);
        return r;
    };
    LIFT_CHECK(f.m.cache().peek(co) != nullptr);
    LIFT_CHECK(f.m.shims().find("ole32", "CoCreateInstance") != nullptr);

    const std::uint32_t clsid_fg = f.data(kClsidFilterGraph, 16);
    const std::uint32_t iid_graph = f.data(kIidGraphBuilder, 16);
    const std::uint32_t ppv = f.zeroData(4);

    // ① CoCreateInstance(CLSID_FilterGraph)
    {
        g_current_case = "quartz-cocreate-filtergraph";
        LIFT_CHECK_EQ_U32(invoke(co, {clsid_fg, 0u, 1u, iid_graph, ppv}), kSOk);
        LIFT_CHECK_NE_U32(f.m.mem().rd32(ppv), 0u);
    }
    const std::uint32_t graph = f.m.mem().rd32(ppv);

    // ①'' FilterGraphNoThread + IFilterGraph（同一形状层；0x4D26BA 站点）
    {
        g_current_case = "quartz-cocreate-filtergraph-nothread";
        const std::uint32_t clsid = f.data(kClsidFilterGraphNoThread, 16);
        const std::uint32_t iid = f.data(kIidFilterGraph, 16);
        const std::uint32_t out = f.zeroData(4);
        LIFT_CHECK_EQ_U32(invoke(co, {clsid, 0u, 1u, iid, out}), kSOk);
        LIFT_CHECK_NE_U32(f.m.mem().rd32(out), 0u);
    }

    // ①' AddFilter 的 guest 形状是 this+2（`.lst` 0x487726 / 0x486E75 / 0x4893A2）
    LIFT_CHECK_EQ_U32(invoke(methodEa(f, graph, 0x0Cu), {graph, 0u, 0u}), kSOk);

    // ② QI IMediaControl
    const std::uint32_t iid_control = f.data(kIidMediaControl, 16);
    const std::uint32_t ctl_ppv = f.zeroData(4);
    {
        g_current_case = "quartz-qi-mediacontrol";
        LIFT_CHECK_EQ_U32(invoke(methodEa(f, graph, 0x00u), {graph, iid_control, ctl_ppv}), kSOk);
        LIFT_CHECK_NE_U32(f.m.mem().rd32(ctl_ppv), 0u);
    }
    const std::uint32_t control = f.m.mem().rd32(ctl_ppv);

    // ③ Run / GetState / Stop
    {
        g_current_case = "quartz-mediacontrol-run-stop";
        LIFT_CHECK_EQ_U32(invoke(methodEa(f, control, 0x1Cu), {control}), kSOk);  // Run
        const std::uint32_t state = f.zeroData(4);
        LIFT_CHECK_EQ_U32(invoke(methodEa(f, control, 0x28u), {control, 0xFFFFFFFFu, state}), kSOk);
        LIFT_CHECK_EQ_U32(f.m.mem().rd32(state), 2u);  // State_Running
        LIFT_CHECK_EQ_U32(invoke(methodEa(f, control, 0x24u), {control}), kSOk);  // Stop
        LIFT_CHECK_EQ_U32(invoke(methodEa(f, control, 0x28u), {control, 0u, state}), kSOk);
        LIFT_CHECK_EQ_U32(f.m.mem().rd32(state), 0u);  // State_Stopped
    }

    // ④ QI IMediaEvent + WaitForCompletion 立即完成
    const std::uint32_t iid_event = f.data(kIidMediaEvent, 16);
    const std::uint32_t event_ppv = f.zeroData(4);
    {
        g_current_case = "quartz-qi-mediaevent-wait";
        LIFT_CHECK_EQ_U32(invoke(methodEa(f, control, 0x00u), {control, iid_event, event_ppv}), kSOk);
        const std::uint32_t event = f.m.mem().rd32(event_ppv);
        LIFT_CHECK_NE_U32(event, 0u);
        const std::uint32_t ev = f.zeroData(4);
        LIFT_CHECK_EQ_U32(invoke(methodEa(f, event, 0x24u), {event, 0u, ev}), kSOk);
        LIFT_CHECK_EQ_U32(f.m.mem().rd32(ev), 1u);  // EC_COMPLETE
        // SetNotifyWindow(3 args) —— 形状层接受但不建真窗口
        LIFT_CHECK_EQ_U32(invoke(methodEa(f, event, 0x34u), {event, 0u, 0u, 0u}), kSOk);
    }

    // ⑤ QI IMediaSeeking + GetDuration / GetCurrentPosition = 0
    const std::uint32_t iid_seeking = f.data(kIidMediaSeeking, 16);
    const std::uint32_t seek_ppv = f.zeroData(4);
    {
        g_current_case = "quartz-qi-mediaseeking-zero";
        LIFT_CHECK_EQ_U32(invoke(methodEa(f, graph, 0x00u), {graph, iid_seeking, seek_ppv}), kSOk);
        const std::uint32_t seeking = f.m.mem().rd32(seek_ppv);
        LIFT_CHECK_NE_U32(seeking, 0u);
        const std::uint32_t duration = f.zeroData(8);
        const std::uint32_t position = f.zeroData(8);
        LIFT_CHECK_EQ_U32(invoke(methodEa(f, seeking, 0x28u), {seeking, duration}), kSOk);
        LIFT_CHECK_EQ_U32(invoke(methodEa(f, seeking, 0x30u), {seeking, position}), kSOk);
        LIFT_CHECK_EQ_U32(f.m.mem().rd32(duration), 0u);
        LIFT_CHECK_EQ_U32(f.m.mem().rd32(duration + 4u), 0u);
        LIFT_CHECK_EQ_U32(f.m.mem().rd32(position), 0u);
        LIFT_CHECK_EQ_U32(f.m.mem().rd32(position + 4u), 0u);
        // GetAvailable(2 out ptrs)
        const std::uint32_t avail1 = f.zeroData(8);
        const std::uint32_t avail2 = f.zeroData(8);
        LIFT_CHECK_EQ_U32(invoke(methodEa(f, seeking, 0x40u), {seeking, avail1, avail2}), kSOk);
        LIFT_CHECK_EQ_U32(f.m.mem().rd32(avail1), 0u);
        LIFT_CHECK_EQ_U32(f.m.mem().rd32(avail2), 0u);
        // ConvertTimeFormat(3 ptr + by-value LONGLONG + ptr = 5 DWORD 栈)
        const std::uint32_t target = f.zeroData(8);
        LIFT_CHECK_EQ_U32(invoke(methodEa(f, seeking, 0x34u), {seeking, target, 0u, 0u, 0u, 0u}), kSOk);
        LIFT_CHECK_EQ_U32(f.m.mem().rd32(target), 0u);
        // SetPositions(4 args)
        LIFT_CHECK_EQ_U32(invoke(methodEa(f, seeking, 0x38u), {seeking, 0u, 1u, 0u, 0u}), kSOk);
    }

    // ⑤' IBasicVideo：get_AvgTimePerFrame(8 字节) / GetVideoPaletteEntries(4 参)
    {
        g_current_case = "quartz-basicvideo";
        const std::uint32_t iid_bv = f.data(kIidBasicVideo, 16);
        const std::uint32_t bv_ppv = f.zeroData(4);
        LIFT_CHECK_EQ_U32(invoke(methodEa(f, graph, 0x00u), {graph, iid_bv, bv_ppv}), kSOk);
        const std::uint32_t bv = f.m.mem().rd32(bv_ppv);
        LIFT_CHECK_NE_U32(bv, 0u);
        const std::uint32_t avg = f.zeroData(8);
        LIFT_CHECK_EQ_U32(invoke(methodEa(f, bv, 0x1Cu), {bv, avg}), kSOk);
        LIFT_CHECK_EQ_U32(f.m.mem().rd32(avg + 4u), 0u);
        LIFT_CHECK_EQ_U32(invoke(methodEa(f, bv, 0x9Cu), {bv, 0u, 0u, 0u, 0u}), kSOk);
    }

    // ⑥ AMultiMediaStream 形状对象
    const std::uint32_t clsid_mm = f.data(kClsidAmMultiMediaStream, 16);
    const std::uint32_t iid_mm = f.data(kIidAmMultiMediaStream, 16);
    const std::uint32_t mm_ppv = f.zeroData(4);
    {
        g_current_case = "quartz-ammultimediastream";
        LIFT_CHECK_EQ_U32(invoke(co, {clsid_mm, 0u, 1u, iid_mm, mm_ppv}), kSOk);
        const std::uint32_t stream = f.m.mem().rd32(mm_ppv);
        LIFT_CHECK_NE_U32(stream, 0u);
        LIFT_CHECK_EQ_U32(invoke(methodEa(f, stream, 0x30u), {stream, 0u, 0u, 0u}), kSOk);  // Initialize
        const std::uint32_t ms = f.zeroData(4);
        LIFT_CHECK_EQ_U32(invoke(methodEa(f, stream, 0x10u), {stream, 0u, ms}), kSOk);  // GetMediaStream
        LIFT_CHECK_NE_U32(f.m.mem().rd32(ms), 0u);
        const std::uint32_t fg = f.zeroData(4);
        LIFT_CHECK_EQ_U32(invoke(methodEa(f, stream, 0x34u), {stream, fg}), kSOk);  // GetFilterGraph
        LIFT_CHECK_NE_U32(f.m.mem().rd32(fg), 0u);
        // ★ 出参接口必须带新引用：guest Release 一次后，缓存槽仍活着（否则这里会 find=false）。
        const std::uint32_t graph_out = f.m.mem().rd32(fg);
        LIFT_CHECK(com.find(graph_out));
        LIFT_CHECK_EQ_U32(invoke(methodEa(f, graph_out, 0x08u), {graph_out}), 1u);  // Release
        LIFT_CHECK(com.find(graph_out));
        const std::uint32_t fg_again = f.zeroData(4);
        LIFT_CHECK_EQ_U32(invoke(methodEa(f, stream, 0x34u), {stream, fg_again}), kSOk);
        LIFT_CHECK_NE_U32(f.m.mem().rd32(fg_again), 0u);
        LIFT_CHECK_EQ_U32(invoke(methodEa(f, stream, 0x3Cu), {stream, 0u, 0u, 0u, 0u}), kSOk);  // AddMediaStream
        LIFT_CHECK_EQ_U32(invoke(methodEa(f, stream, 0x40u), {stream, 0u, 0u}), kSOk);  // OpenFile
    }

    // ⑥' OLE 变体 / BSTR 的注册名与基本语义（M2/M4 的伴生小族）
    {
        g_current_case = "quartz-oleaut32-variant";
        LIFT_CHECK(f.m.shims().find("ole32", "PropVariantClear") != nullptr);
        LIFT_CHECK(f.m.shims().find("oleaut32", "#7") != nullptr);
        LIFT_CHECK(f.m.shims().find("oleaut32", "#8") != nullptr);
        LIFT_CHECK(f.m.shims().find("oleaut32", "#9") != nullptr);
        const std::uint32_t syslen = f.use("oleaut32", "#7");
        const std::uint32_t vinit = f.use("oleaut32", "#8");
        const std::uint32_t vclear = f.use("oleaut32", "#9");
        LIFT_CHECK(f.ok);
        // BSTR：头 = 字节长度（4），后面 2 个 UTF-16 码元 ⇒ SysStringLen = 2 字符
        std::uint8_t bstr[8] = {4, 0, 0, 0, 'A', 0, 'B', 0};
        const std::uint32_t bstr_ea = f.data(bstr, sizeof(bstr));
        LIFT_CHECK_EQ_U32(invoke(syslen, {bstr_ea + 4u}), 2u);
        const std::uint32_t variant = f.u32data(0xDEADBEEFu);
        LIFT_CHECK_EQ_U32(invoke(vinit, {variant}), kSOk);
        LIFT_CHECK_EQ_U32(f.m.mem().rd32(variant), 0u);
        LIFT_CHECK_EQ_U32(invoke(vclear, {variant}), kSOk);
        LIFT_CHECK_EQ_U32(f.m.mem().rd32(variant), 0u);
    }

    // ⑦ 失败路径：未知 CLSID / 未知 IID
    const std::uint32_t clsid_bogus = f.data(kIidBogus, 16);
    const std::uint32_t iid_bogus = f.data(kIidBogus, 16);
    const std::uint32_t out_bogus = f.u32data(0xDEADBEEFu);
    {
        g_current_case = "quartz-unknown-clsid-iid";
        LIFT_CHECK_EQ_U32(invoke(co, {clsid_bogus, 0u, 1u, iid_graph, out_bogus}),
                          kClassEClassNotAvailable);
        LIFT_CHECK_EQ_U32(f.m.mem().rd32(out_bogus), 0u);
        LIFT_CHECK_EQ_U32(invoke(methodEa(f, graph, 0x00u), {graph, iid_bogus, out_bogus}),
                          kENoInterface);
        LIFT_CHECK_EQ_U32(f.m.mem().rd32(out_bogus), 0u);
    }

    return finish("quartz-video");
}
