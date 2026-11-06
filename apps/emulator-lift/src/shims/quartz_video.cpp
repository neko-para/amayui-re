// emulator-lift · DirectShow / AMultiMediaStream 视频形状层（M4 的"可跳过"起步）
//
// ★ 为什么需要它：`.lst` 的影片路径（`Movie` / `MovieIVideoWindow` / `CMovieToTexture`）在
//   `CoCreateInstance` 处调用 DirectShow（`CLSID_FilterGraph`）或 `CLSID_AMMultiMediaStream`。
//   在 M1/M2 里这条导入被登记成"未实现"⇒ 执行到就 **loud trap**（`ole32!CoCreateInstance`），
//   于是"准备播放视频"这一步把进程停住，永远到不了标题 / 后续脚本。
//
// ★ 本文件的口径：**形状层，不是解码器**。目标是把"影片初始化 / 播放 / 等待结束"这一串
//   COM 调用变成**立即完成**（零长影片）：
//     · `CoCreateInstance(CLSID_FilterGraph, IID_IGraphBuilder)` 返回 guest 可见的 IGraphBuilder；
//     · `QueryInterface` 按 IID 返回同一图的 IMediaControl / IMediaEvent / IMediaSeeking /
//       IMediaPosition / IVideoWindow / IBasicAudio / IBasicVideo 视图；
//     · `Run` / `Render` / `AddFilter` / `Connect` / `RenderFile` 返回 S_OK；
//     · `IMediaEvent::WaitForCompletion` **立即**返回 `EC_COMPLETE`；
//     · `IMediaSeeking` / `IMediaPosition` 的时长 / 位置恒为 0；
//     · `IVideoWindow` 的 `put_*` 只记账（不建宿主窗口）。
//   ⇒ 引擎看到"影片已经放完"，继续执行下一条脚本，而不是卡在视频上。
//   ⛔ 不声称画面 / 声音正确：M4 的真解码是后续单；这里只解除"缺一条导入"的硬停。
//
// ★ `CLSID_AMMultiMediaStream`（`Movie` 类走的老 DirectShow 视频）同样返回一个形状对象：
//   `Movie::Init` 只要求 `IAMMultiMediaStream::Initialize` / `AddMediaStream` 不失败；
//   `GetMediaStream` 返回一个通用的 `IMultiMediaStream` 形状对象（所有方法 S_OK + 清零出参）。
//   这足以让 `Movie::Init` 返回 1；后续 `Movie::Open` 若要求真实样本，会走它的失败分支而不是 trap。
//
// ★ 每个方法都打一行（同一方法只打第一次，避免每帧刷屏）；⛔ 不做"静默返回 0"。
//   未实现的槽仍然保持 `comUnsupported` 的 loud trap（方法表只填**.lst 取证到的槽**）。
//
// ★ 取证（EA，只作注释；实现不读语料）：
//   · `CoCreateInstance` 站点：0x486715（IAMMultiMediaStream）· 0x487455（IGraphBuilder，MovieIVideoWindow）
//     · 0x488E4B（IGraphBuilder，CMovieToTexture）；
//   · `IMediaControl` 是 **IDispatch 派生**：Run 在 +0x1C（0x4883C7 `[ecx+1Ch]`），Stop +0x24
//     （0x4885E0 `[ecx+24h]`），GetState +0x28（0x4870BE / 0x4870E0 / 0x487102 `[+28h]` 带 2 个参数）；
//   · `IMediaEvent` 也是 IDispatch 派生：WaitForCompletion 在 +0x24（0x487292 `[+24h]`，
//     参数 `(timeout, &evcode)`）；`IMediaSeeking` 是 IUnknown 派生：SetPositions +0x38
//     （0x487169 `[+38h]`，4 个参数）、GetCurrentPosition +0x30（0x487256 `[+30h]`）；
//   · `IVideoWindow` 也是 IDispatch 派生：put_Owner +0x74、SetWindowPosition +0x9C 等。
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "runtime/com.h"
#include "runtime/exec.h"
#include "runtime/heap.h"
#include "runtime/memory.h"
#include "runtime/x86.h"
#include "shims/register.h"

namespace lift {
namespace {

// ---------------------------------------------------------------------------------------
// 返回码（SDK：winerror.h / strmif.h）
// ---------------------------------------------------------------------------------------
constexpr std::uint32_t kSOk = 0x00000000u;
constexpr std::uint32_t kSFalse = 0x00000001u;
constexpr std::uint32_t kENoInterface = 0x80004002u;
constexpr std::uint32_t kEPointer = 0x80004003u;
constexpr std::uint32_t kEInvalidArg = 0x80070057u;
constexpr std::uint32_t kENotImpl = 0x80004001u;
constexpr std::uint32_t kEOutOfMemory = 0x8007000Eu;
constexpr std::uint32_t kEfail = 0x80004005u;
constexpr std::uint32_t kClassENoAggregation = 0x80040110u;
constexpr std::uint32_t kClassEClassNotAvailable = 0x80040111u;

// `IMediaEvent` 的事件码（strmif.h）
constexpr std::uint32_t kEcComplete = 0x00000001u;

// ---------------------------------------------------------------------------------------
// IID / CLSID（16 字节，内存字节序；与 `.lst` 的数据段逐字节一致）
// ---------------------------------------------------------------------------------------
constexpr std::uint8_t kIidUnknown[16] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                          0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46};
// IID_IGraphBuilder {56A868A9-0AD4-11CE-B03A-0020AF0BA770}
constexpr std::uint8_t kIidGraphBuilder[16] = {0xA9, 0x68, 0xA8, 0x56, 0xD4, 0x0A, 0xCE, 0x11,
                                               0xB0, 0x3A, 0x00, 0x20, 0xAF, 0x0B, 0xA7, 0x70};
// IID_IFilterGraph {56A868A3-...}（IGraphBuilder 的前缀接口；形状层复用同一 vtable 前缀）
constexpr std::uint8_t kIidFilterGraph[16] = {0xA3, 0x68, 0xA8, 0x56, 0xD4, 0x0A, 0xCE, 0x11,
                                              0xB0, 0x3A, 0x00, 0x20, 0xAF, 0x0B, 0xA7, 0x70};
// IID_IMediaControl {56A868B1-...}
constexpr std::uint8_t kIidMediaControl[16] = {0xB1, 0x68, 0xA8, 0x56, 0xD4, 0x0A, 0xCE, 0x11,
                                               0xB0, 0x3A, 0x00, 0x20, 0xAF, 0x0B, 0xA7, 0x70};
// IID_IMediaSeeking {56A868B3-...}
constexpr std::uint8_t kIidMediaSeeking[16] = {0xB3, 0x68, 0xA8, 0x56, 0xD4, 0x0A, 0xCE, 0x11,
                                               0xB0, 0x3A, 0x00, 0x20, 0xAF, 0x0B, 0xA7, 0x70};
// IID_IMediaPosition {56A868B2-...}
constexpr std::uint8_t kIidMediaPosition[16] = {0xB2, 0x68, 0xA8, 0x56, 0xD4, 0x0A, 0xCE, 0x11,
                                                0xB0, 0x3A, 0x00, 0x20, 0xAF, 0x0B, 0xA7, 0x70};
// IID_IVideoWindow {56A868B4-...}
constexpr std::uint8_t kIidVideoWindow[16] = {0xB4, 0x68, 0xA8, 0x56, 0xD4, 0x0A, 0xCE, 0x11,
                                              0xB0, 0x3A, 0x00, 0x20, 0xAF, 0x0B, 0xA7, 0x70};
// IID_IMediaEvent {56A868B6-...}
constexpr std::uint8_t kIidMediaEvent[16] = {0xB6, 0x68, 0xA8, 0x56, 0xD4, 0x0A, 0xCE, 0x11,
                                             0xB0, 0x3A, 0x00, 0x20, 0xAF, 0x0B, 0xA7, 0x70};
// IID_IMediaEventEx {56A868C0-...}
constexpr std::uint8_t kIidMediaEventEx[16] = {0xC0, 0x68, 0xA8, 0x56, 0xD4, 0x0A, 0xCE, 0x11,
                                               0xB0, 0x3A, 0x00, 0x20, 0xAF, 0x0B, 0xA7, 0x70};
// IID_IBasicAudio {56A868B9-...}
constexpr std::uint8_t kIidBasicAudio[16] = {0xB9, 0x68, 0xA8, 0x56, 0xD4, 0x0A, 0xCE, 0x11,
                                             0xB0, 0x3A, 0x00, 0x20, 0xAF, 0x0B, 0xA7, 0x70};
// IID_IBasicVideo {56A868B5-...}
constexpr std::uint8_t kIidBasicVideo[16] = {0xB5, 0x68, 0xA8, 0x56, 0xD4, 0x0A, 0xCE, 0x11,
                                             0xB0, 0x3A, 0x00, 0x20, 0xAF, 0x0B, 0xA7, 0x70};
// 36B73880-C2C8-11CF-8B46-00805F6CEF60（`.data:0052FCD8`；引擎在 0x487430 用它 QI，错误串写的是 IMediaSeeking）
// ★ 这里按 IMediaSeeking 的 vtable 提供 SetPositions(+0x38) 等方法；形状层统一立即完成。
constexpr std::uint8_t kIidMediaSample2[16] = {0x80, 0x38, 0xB7, 0x36, 0xC8, 0xC2, 0xCF, 0x11,
                                               0x8B, 0x46, 0x00, 0x80, 0x5F, 0x6C, 0xEF, 0x60};
// IID_IAMMultiMediaStream {BEBE595C-9A6F-11D0-8FDE-00C04FD9189D}
constexpr std::uint8_t kIidAmMultiMediaStream[16] = {0x5C, 0x59, 0xBE, 0xBE, 0x6F, 0x9A, 0xD0, 0x11,
                                                     0x8F, 0xDE, 0x00, 0xC0, 0x4F, 0xD9, 0x18, 0x9D};
// IID_IMultiMediaStream {B502D1BD-9A57-11D0-8FDE-00C04FD9189D}
constexpr std::uint8_t kIidMultiMediaStream[16] = {0xBD, 0xD1, 0x02, 0xB5, 0x57, 0x9A, 0xD0, 0x11,
                                                   0x8F, 0xDE, 0x00, 0xC0, 0x4F, 0xD9, 0x18, 0x9D};
// CLSID_FilterGraph {E436EBB3-524F-11CE-9F53-0020AF0BA770}
constexpr std::uint8_t kClsidFilterGraph[16] = {0xB3, 0xEB, 0x36, 0xE4, 0x4F, 0x52, 0xCE, 0x11,
                                                0x9F, 0x53, 0x00, 0x20, 0xAF, 0x0B, 0xA7, 0x70};
// CLSID_FilterGraphNoThread {E436EBB2-524F-11CE-9F53-0020AF0BA770}
constexpr std::uint8_t kClsidFilterGraphNoThread[16] = {0xB2, 0xEB, 0x36, 0xE4, 0x4F, 0x52, 0xCE, 0x11,
                                                        0x9F, 0x53, 0x00, 0x20, 0xAF, 0x0B, 0xA7, 0x70};
// CLSID_AMMultiMediaStream {49C47CE5-9BA4-11D0-8212-00C04FC32C45}
constexpr std::uint8_t kClsidAmMultiMediaStream[16] = {0xE5, 0x7C, 0xC4, 0x49, 0xA4, 0x9B, 0xD0, 0x11,
                                                       0x82, 0x12, 0x00, 0xC0, 0x4F, 0xC3, 0x2C, 0x45};

// ---------------------------------------------------------------------------------------
// 共享状态块（guest 堆；每个图一个；所有接口对象用 `ComRegistry::userOf` 指回它）
// ---------------------------------------------------------------------------------------
constexpr std::uint32_t kStateMagic = 0x31534756u;  // 'VGS1'
constexpr std::uint32_t kOffMagic = 0x00;
constexpr std::uint32_t kOffGraph = 0x04;
constexpr std::uint32_t kOffControl = 0x08;
constexpr std::uint32_t kOffEvent = 0x0C;
constexpr std::uint32_t kOffSeeking = 0x10;
constexpr std::uint32_t kOffPosition = 0x14;
constexpr std::uint32_t kOffVideo = 0x18;
constexpr std::uint32_t kOffBasicAudio = 0x1C;
constexpr std::uint32_t kOffBasicVideo = 0x20;
constexpr std::uint32_t kOffStream = 0x24;
constexpr std::uint32_t kOffChild = 0x28;
constexpr std::uint32_t kOffRunState = 0x2C;  // 0=stopped 1=paused 2=running
constexpr std::uint32_t kOffCompleted = 0x30;
constexpr std::uint32_t kOffSeen = 0x34;      // 两个 DWORD 的首次日志位图
constexpr std::uint32_t kOffStateBytes = 0x40;

struct Role {
    enum Kind { Unknown = 0, Graph, Control, Event, Seeking, Position, Video, BasicAudio, BasicVideo, Stream, Child, Self };
};

bool guidEq(const std::uint8_t a[16], const std::uint8_t b[16]) {
    return std::memcmp(a, b, 16) == 0;
}

bool readGuid(X86& cpu, std::uint32_t ea, std::uint8_t out[16]) {
    if (ea == 0 || !cpu.mem || !cpu.mem->committed(ea, 16)) return false;
    for (int i = 0; i < 16; ++i) out[i] = cpu.mem->rd8(ea + static_cast<std::uint32_t>(i));
    return true;
}

bool readClsid(X86& cpu, std::uint32_t ea, std::uint8_t out[16]) {
    return readGuid(cpu, ea, out);
}

void writeU32(X86& cpu, std::uint32_t ea, std::uint32_t v) {
    if (ea && cpu.mem && cpu.mem->committed(ea, 4)) cpu.mem->wr32(ea, v);
}

void writeU64(X86& cpu, std::uint32_t ea, std::uint64_t v) {
    if (ea && cpu.mem && cpu.mem->committed(ea, 8)) cpu.mem->wr64(ea, v);
}

void writeGuidZero(X86& cpu, std::uint32_t ea) {
    if (ea && cpu.mem && cpu.mem->committed(ea, 16)) {
        for (int i = 0; i < 4; ++i) cpu.mem->wr32(ea + static_cast<std::uint32_t>(i) * 4u, 0);
    }
}

std::uint32_t stateU32(X86& cpu, std::uint32_t st, std::uint32_t off) {
    return (st && cpu.mem && cpu.mem->committed(st + off, 4)) ? cpu.mem->rd32(st + off) : 0;
}

void stateWr32(X86& cpu, std::uint32_t st, std::uint32_t off, std::uint32_t v) {
    if (st && cpu.mem && cpu.mem->committed(st + off, 4)) cpu.mem->wr32(st + off, v);
}

std::uint32_t stateOf(X86& cpu, std::uint32_t self) {
    if (!cpu.com || !cpu.com->find(self)) return 0;
    return cpu.com->userOf(self);
}

bool firstTime(X86& cpu, std::uint32_t st, int bit) {
    if (!st) return false;
    const std::uint32_t word = st + kOffSeen + (static_cast<std::uint32_t>(bit / 32) * 4u);
    const std::uint32_t mask = 1u << (static_cast<std::uint32_t>(bit) % 32u);
    const std::uint32_t v = cpu.mem->rd32(word);
    if (v & mask) return false;
    cpu.mem->wr32(word, v | mask);
    return true;
}

void vlog(const char* what, const char* fmt, ...) {
    std::fprintf(stderr, "[shim] quartz!%s: ", what);
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(stderr, fmt, ap);
    va_end(ap);
    std::fputc('\n', stderr);
    std::fflush(stderr);
}

const char* roleName(Role::Kind r) {
    switch (r) {
        case Role::Graph: return "IGraphBuilder";
        case Role::Control: return "IMediaControl";
        case Role::Event: return "IMediaEvent";
        case Role::Seeking: return "IMediaSeeking";
        case Role::Position: return "IMediaPosition";
        case Role::Video: return "IVideoWindow";
        case Role::BasicAudio: return "IBasicAudio";
        case Role::BasicVideo: return "IBasicVideo";
        case Role::Stream: return "IAMMultiMediaStream";
        case Role::Child: return "IMultiMediaStream";
        case Role::Self: return "IUnknown";
        default: return "?";
    }
}

Role::Kind roleForIid(const std::uint8_t g[16]) {
    if (guidEq(g, kIidUnknown)) return Role::Self;
    if (guidEq(g, kIidGraphBuilder) || guidEq(g, kIidFilterGraph)) return Role::Graph;
    if (guidEq(g, kIidMediaControl)) return Role::Control;
    if (guidEq(g, kIidMediaEvent) || guidEq(g, kIidMediaEventEx)) return Role::Event;
    if (guidEq(g, kIidMediaSeeking) || guidEq(g, kIidMediaSample2)) return Role::Seeking;
    if (guidEq(g, kIidMediaPosition)) return Role::Position;
    if (guidEq(g, kIidVideoWindow)) return Role::Video;
    if (guidEq(g, kIidBasicAudio)) return Role::BasicAudio;
    if (guidEq(g, kIidBasicVideo)) return Role::BasicVideo;
    if (guidEq(g, kIidAmMultiMediaStream)) return Role::Stream;
    if (guidEq(g, kIidMultiMediaStream)) return Role::Child;
    return Role::Unknown;
}

// ---------------------------------------------------------------------------------------
// 方法表（每个槽的名字 / argc 按 SDK；偏移是**取证到的**，见文件头）
// ---------------------------------------------------------------------------------------
void videoDispatch(X86& cpu, ShimCall& call);
void streamDispatch(X86& cpu, ShimCall& call);
void streamChildDispatch(X86& cpu, ShimCall& call);

const std::vector<ComMethod>& graphMethods() {
    static const std::vector<ComMethod> m = {
        {0x00u, "QueryInterface", Conv::Stdcall, 2, &videoDispatch},
        {0x04u, "AddRef", Conv::Stdcall, 0, &videoDispatch},
        {0x08u, "Release", Conv::Stdcall, 0, &videoDispatch},
        // ★ argc=2 是**guest 实测**（0x487726 / 0x486E75 / 0x4893A2 都 push this+2），
        //   SDK 的 AddFilter 只声明 1 个参数；按 guest 的调用形状走。
        {0x0Cu, "AddFilter", Conv::Stdcall, 2, &videoDispatch},
        {0x10u, "RemoveFilter", Conv::Stdcall, 1, &videoDispatch},
        {0x14u, "EnumFilters", Conv::Stdcall, 1, &videoDispatch},
        {0x18u, "FindFilterByName", Conv::Stdcall, 2, &videoDispatch},
        {0x1Cu, "ConnectDirect", Conv::Stdcall, 3, &videoDispatch},
        {0x20u, "Reconnect", Conv::Stdcall, 1, &videoDispatch},
        {0x24u, "Disconnect", Conv::Stdcall, 1, &videoDispatch},
        {0x28u, "SetDefaultSyncSource", Conv::Stdcall, 0, &videoDispatch},
        {0x2Cu, "Connect", Conv::Stdcall, 2, &videoDispatch},
        {0x30u, "Render", Conv::Stdcall, 1, &videoDispatch},
        {0x34u, "RenderFile", Conv::Stdcall, 2, &videoDispatch},
        {0x38u, "AddSourceFilter", Conv::Stdcall, 3, &videoDispatch},
        {0x3Cu, "SetLogFile", Conv::Stdcall, 1, &videoDispatch},
        {0x40u, "Abort", Conv::Stdcall, 0, &videoDispatch},
        {0x44u, "ShouldOperationContinue", Conv::Stdcall, 0, &videoDispatch},
    };
    return m;
}

const std::vector<ComMethod>& controlMethods() {
    static const std::vector<ComMethod> m = {
        {0x00u, "QueryInterface", Conv::Stdcall, 2, &videoDispatch},
        {0x04u, "AddRef", Conv::Stdcall, 0, &videoDispatch},
        {0x08u, "Release", Conv::Stdcall, 0, &videoDispatch},
        {0x0Cu, "GetTypeInfoCount", Conv::Stdcall, 1, &videoDispatch},
        {0x10u, "GetTypeInfo", Conv::Stdcall, 3, &videoDispatch},
        {0x14u, "GetIDsOfNames", Conv::Stdcall, 5, &videoDispatch},
        {0x18u, "Invoke", Conv::Stdcall, 8, &videoDispatch},
        {0x1Cu, "Run", Conv::Stdcall, 0, &videoDispatch},
        {0x20u, "Pause", Conv::Stdcall, 0, &videoDispatch},
        {0x24u, "Stop", Conv::Stdcall, 0, &videoDispatch},
        // ★ argc=2 是**guest 实测**（0x4870BE / 0x4870E0 / 0x487102 push timeout + &state），
        //   不是 SDK 的 `GetState(LONG, OAFilterState*)` 误读。
        {0x28u, "GetState", Conv::Stdcall, 2, &videoDispatch},
        {0x2Cu, "RenderFile", Conv::Stdcall, 1, &videoDispatch},
        {0x30u, "AddSourceFilter", Conv::Stdcall, 2, &videoDispatch},
        {0x34u, "SetFilterGraph", Conv::Stdcall, 1, &videoDispatch},
        {0x38u, "GetFilterGraph", Conv::Stdcall, 1, &videoDispatch},
    };
    return m;
}

const std::vector<ComMethod>& eventMethods() {
    static const std::vector<ComMethod> m = {
        {0x00u, "QueryInterface", Conv::Stdcall, 2, &videoDispatch},
        {0x04u, "AddRef", Conv::Stdcall, 0, &videoDispatch},
        {0x08u, "Release", Conv::Stdcall, 0, &videoDispatch},
        {0x0Cu, "GetTypeInfoCount", Conv::Stdcall, 1, &videoDispatch},
        {0x10u, "GetTypeInfo", Conv::Stdcall, 3, &videoDispatch},
        {0x14u, "GetIDsOfNames", Conv::Stdcall, 5, &videoDispatch},
        {0x18u, "Invoke", Conv::Stdcall, 8, &videoDispatch},
        {0x1Cu, "GetEventHandle", Conv::Stdcall, 1, &videoDispatch},
        {0x20u, "GetEvent", Conv::Stdcall, 4, &videoDispatch},
        {0x24u, "WaitForCompletion", Conv::Stdcall, 2, &videoDispatch},
        {0x28u, "CancelDefaultHandling", Conv::Stdcall, 1, &videoDispatch},
        {0x2Cu, "RestoreDefaultHandling", Conv::Stdcall, 1, &videoDispatch},
        {0x30u, "FreeEventParams", Conv::Stdcall, 3, &videoDispatch},
        {0x34u, "SetNotifyWindow", Conv::Stdcall, 3, &videoDispatch},
        {0x38u, "SetNotifyFlags", Conv::Stdcall, 1, &videoDispatch},
        {0x3Cu, "GetNotifyFlags", Conv::Stdcall, 1, &videoDispatch},
    };
    return m;
}

const std::vector<ComMethod>& seekingMethods() {
    static const std::vector<ComMethod> m = {
        {0x00u, "QueryInterface", Conv::Stdcall, 2, &videoDispatch},
        {0x04u, "AddRef", Conv::Stdcall, 0, &videoDispatch},
        {0x08u, "Release", Conv::Stdcall, 0, &videoDispatch},
        {0x0Cu, "GetCapabilities", Conv::Stdcall, 1, &videoDispatch},
        {0x10u, "CheckCapabilities", Conv::Stdcall, 1, &videoDispatch},
        {0x14u, "IsFormatSupported", Conv::Stdcall, 1, &videoDispatch},
        {0x18u, "QueryPreferredFormat", Conv::Stdcall, 1, &videoDispatch},
        {0x1Cu, "GetTimeFormat", Conv::Stdcall, 1, &videoDispatch},
        {0x20u, "IsUsingTimeFormat", Conv::Stdcall, 1, &videoDispatch},
        {0x24u, "SetTimeFormat", Conv::Stdcall, 1, &videoDispatch},
        {0x28u, "GetDuration", Conv::Stdcall, 1, &videoDispatch},
        {0x2Cu, "GetStopPosition", Conv::Stdcall, 1, &videoDispatch},
        {0x30u, "GetCurrentPosition", Conv::Stdcall, 1, &videoDispatch},
        {0x34u, "ConvertTimeFormat", Conv::Stdcall, 5, &videoDispatch},
        {0x38u, "SetPositions", Conv::Stdcall, 4, &videoDispatch},
        {0x3Cu, "GetPositions", Conv::Stdcall, 2, &videoDispatch},
        {0x40u, "GetAvailable", Conv::Stdcall, 2, &videoDispatch},
        {0x44u, "SetRate", Conv::Stdcall, 2, &videoDispatch},
        {0x48u, "GetRate", Conv::Stdcall, 1, &videoDispatch},
        {0x4Cu, "GetPreroll", Conv::Stdcall, 1, &videoDispatch},
    };
    return m;
}

const std::vector<ComMethod>& positionMethods() {
    static const std::vector<ComMethod> m = {
        {0x00u, "QueryInterface", Conv::Stdcall, 2, &videoDispatch},
        {0x04u, "AddRef", Conv::Stdcall, 0, &videoDispatch},
        {0x08u, "Release", Conv::Stdcall, 0, &videoDispatch},
        {0x0Cu, "GetTypeInfoCount", Conv::Stdcall, 1, &videoDispatch},
        {0x10u, "GetTypeInfo", Conv::Stdcall, 3, &videoDispatch},
        {0x14u, "GetIDsOfNames", Conv::Stdcall, 5, &videoDispatch},
        {0x18u, "Invoke", Conv::Stdcall, 8, &videoDispatch},
        {0x1Cu, "get_Duration", Conv::Stdcall, 1, &videoDispatch},
        {0x20u, "put_CurrentPosition", Conv::Stdcall, 2, &videoDispatch},
        {0x24u, "get_CurrentPosition", Conv::Stdcall, 1, &videoDispatch},
        {0x28u, "get_StopTime", Conv::Stdcall, 1, &videoDispatch},
        {0x2Cu, "put_StopTime", Conv::Stdcall, 2, &videoDispatch},
        {0x30u, "get_PrerollTime", Conv::Stdcall, 1, &videoDispatch},
        {0x34u, "put_PrerollTime", Conv::Stdcall, 2, &videoDispatch},
        {0x38u, "put_Rate", Conv::Stdcall, 2, &videoDispatch},
        {0x3Cu, "get_Rate", Conv::Stdcall, 1, &videoDispatch},
        {0x40u, "CanSeekForward", Conv::Stdcall, 1, &videoDispatch},
        {0x44u, "CanSeekBackward", Conv::Stdcall, 1, &videoDispatch},
    };
    return m;
}

const std::vector<ComMethod>& videoWindowMethods() {
    static const std::vector<ComMethod> m = {
        {0x00u, "QueryInterface", Conv::Stdcall, 2, &videoDispatch},
        {0x04u, "AddRef", Conv::Stdcall, 0, &videoDispatch},
        {0x08u, "Release", Conv::Stdcall, 0, &videoDispatch},
        {0x0Cu, "GetTypeInfoCount", Conv::Stdcall, 1, &videoDispatch},
        {0x10u, "GetTypeInfo", Conv::Stdcall, 3, &videoDispatch},
        {0x14u, "GetIDsOfNames", Conv::Stdcall, 5, &videoDispatch},
        {0x18u, "Invoke", Conv::Stdcall, 8, &videoDispatch},
        {0x1Cu, "put_Caption", Conv::Stdcall, 1, &videoDispatch},
        {0x20u, "get_Caption", Conv::Stdcall, 1, &videoDispatch},
        {0x24u, "put_WindowStyle", Conv::Stdcall, 1, &videoDispatch},
        {0x28u, "get_WindowStyle", Conv::Stdcall, 1, &videoDispatch},
        {0x2Cu, "put_WindowStyleEx", Conv::Stdcall, 1, &videoDispatch},
        {0x30u, "get_WindowStyleEx", Conv::Stdcall, 1, &videoDispatch},
        {0x34u, "put_AutoShow", Conv::Stdcall, 1, &videoDispatch},
        {0x38u, "get_AutoShow", Conv::Stdcall, 1, &videoDispatch},
        {0x3Cu, "put_WindowState", Conv::Stdcall, 1, &videoDispatch},
        {0x40u, "get_WindowState", Conv::Stdcall, 1, &videoDispatch},
        {0x44u, "put_BackgroundPalette", Conv::Stdcall, 1, &videoDispatch},
        {0x48u, "get_BackgroundPalette", Conv::Stdcall, 1, &videoDispatch},
        {0x4Cu, "put_Visible", Conv::Stdcall, 1, &videoDispatch},
        {0x50u, "get_Visible", Conv::Stdcall, 1, &videoDispatch},
        {0x54u, "put_Left", Conv::Stdcall, 1, &videoDispatch},
        {0x58u, "get_Left", Conv::Stdcall, 1, &videoDispatch},
        {0x5Cu, "put_Width", Conv::Stdcall, 1, &videoDispatch},
        {0x60u, "get_Width", Conv::Stdcall, 1, &videoDispatch},
        {0x64u, "put_Top", Conv::Stdcall, 1, &videoDispatch},
        {0x68u, "get_Top", Conv::Stdcall, 1, &videoDispatch},
        {0x6Cu, "put_Height", Conv::Stdcall, 1, &videoDispatch},
        {0x70u, "get_Height", Conv::Stdcall, 1, &videoDispatch},
        {0x74u, "put_Owner", Conv::Stdcall, 1, &videoDispatch},
        {0x78u, "get_Owner", Conv::Stdcall, 1, &videoDispatch},
        {0x7Cu, "put_MessageDrain", Conv::Stdcall, 1, &videoDispatch},
        {0x80u, "get_MessageDrain", Conv::Stdcall, 1, &videoDispatch},
        {0x84u, "get_BorderColor", Conv::Stdcall, 1, &videoDispatch},
        {0x88u, "put_BorderColor", Conv::Stdcall, 1, &videoDispatch},
        {0x8Cu, "get_FullScreenMode", Conv::Stdcall, 1, &videoDispatch},
        {0x90u, "put_FullScreenMode", Conv::Stdcall, 1, &videoDispatch},
        {0x94u, "SetWindowForeground", Conv::Stdcall, 1, &videoDispatch},
        {0x98u, "NotifyOwnerMessage", Conv::Stdcall, 4, &videoDispatch},
        {0x9Cu, "SetWindowPosition", Conv::Stdcall, 4, &videoDispatch},
        {0xA0u, "GetWindowPosition", Conv::Stdcall, 4, &videoDispatch},
        {0xA4u, "GetMinIdealImageSize", Conv::Stdcall, 2, &videoDispatch},
        {0xA8u, "GetMaxIdealImageSize", Conv::Stdcall, 2, &videoDispatch},
        {0xACu, "GetRestorePosition", Conv::Stdcall, 4, &videoDispatch},
        {0xB0u, "HideCursor", Conv::Stdcall, 1, &videoDispatch},
        {0xB4u, "IsCursorHidden", Conv::Stdcall, 1, &videoDispatch},
    };
    return m;
}

const std::vector<ComMethod>& basicAudioMethods() {
    static const std::vector<ComMethod> m = {
        {0x00u, "QueryInterface", Conv::Stdcall, 2, &videoDispatch},
        {0x04u, "AddRef", Conv::Stdcall, 0, &videoDispatch},
        {0x08u, "Release", Conv::Stdcall, 0, &videoDispatch},
        {0x0Cu, "GetTypeInfoCount", Conv::Stdcall, 1, &videoDispatch},
        {0x10u, "GetTypeInfo", Conv::Stdcall, 3, &videoDispatch},
        {0x14u, "GetIDsOfNames", Conv::Stdcall, 5, &videoDispatch},
        {0x18u, "Invoke", Conv::Stdcall, 8, &videoDispatch},
        {0x1Cu, "put_Volume", Conv::Stdcall, 1, &videoDispatch},
        {0x20u, "get_Volume", Conv::Stdcall, 1, &videoDispatch},
        {0x24u, "put_Balance", Conv::Stdcall, 1, &videoDispatch},
        {0x28u, "get_Balance", Conv::Stdcall, 1, &videoDispatch},
    };
    return m;
}

const std::vector<ComMethod>& basicVideoMethods() {
    static const std::vector<ComMethod> m = {
        {0x00u, "QueryInterface", Conv::Stdcall, 2, &videoDispatch},
        {0x04u, "AddRef", Conv::Stdcall, 0, &videoDispatch},
        {0x08u, "Release", Conv::Stdcall, 0, &videoDispatch},
        {0x0Cu, "GetTypeInfoCount", Conv::Stdcall, 1, &videoDispatch},
        {0x10u, "GetTypeInfo", Conv::Stdcall, 3, &videoDispatch},
        {0x14u, "GetIDsOfNames", Conv::Stdcall, 5, &videoDispatch},
        {0x18u, "Invoke", Conv::Stdcall, 8, &videoDispatch},
        {0x1Cu, "get_AvgTimePerFrame", Conv::Stdcall, 1, &videoDispatch},
        {0x20u, "get_BitRate", Conv::Stdcall, 1, &videoDispatch},
        {0x24u, "get_BitErrorRate", Conv::Stdcall, 1, &videoDispatch},
        {0x28u, "get_VideoWidth", Conv::Stdcall, 1, &videoDispatch},
        {0x2Cu, "get_VideoHeight", Conv::Stdcall, 1, &videoDispatch},
        {0x30u, "put_SourceLeft", Conv::Stdcall, 1, &videoDispatch},
        {0x34u, "get_SourceLeft", Conv::Stdcall, 1, &videoDispatch},
        {0x38u, "put_SourceWidth", Conv::Stdcall, 1, &videoDispatch},
        {0x3Cu, "get_SourceWidth", Conv::Stdcall, 1, &videoDispatch},
        {0x40u, "put_SourceTop", Conv::Stdcall, 1, &videoDispatch},
        {0x44u, "get_SourceTop", Conv::Stdcall, 1, &videoDispatch},
        {0x48u, "put_SourceHeight", Conv::Stdcall, 1, &videoDispatch},
        {0x4Cu, "get_SourceHeight", Conv::Stdcall, 1, &videoDispatch},
        {0x50u, "put_DestinationLeft", Conv::Stdcall, 1, &videoDispatch},
        {0x54u, "get_DestinationLeft", Conv::Stdcall, 1, &videoDispatch},
        {0x58u, "put_DestinationWidth", Conv::Stdcall, 1, &videoDispatch},
        {0x5Cu, "get_DestinationWidth", Conv::Stdcall, 1, &videoDispatch},
        {0x60u, "put_DestinationTop", Conv::Stdcall, 1, &videoDispatch},
        {0x64u, "get_DestinationTop", Conv::Stdcall, 1, &videoDispatch},
        {0x68u, "put_DestinationHeight", Conv::Stdcall, 1, &videoDispatch},
        {0x6Cu, "get_DestinationHeight", Conv::Stdcall, 1, &videoDispatch},
        {0x70u, "SetSourcePosition", Conv::Stdcall, 4, &videoDispatch},
        {0x74u, "GetSourcePosition", Conv::Stdcall, 4, &videoDispatch},
        {0x78u, "SetDestinationPosition", Conv::Stdcall, 4, &videoDispatch},
        {0x7Cu, "GetDestinationPosition", Conv::Stdcall, 4, &videoDispatch},
        {0x80u, "put_Width", Conv::Stdcall, 1, &videoDispatch},
        {0x84u, "get_Width", Conv::Stdcall, 1, &videoDispatch},
        {0x88u, "put_Height", Conv::Stdcall, 1, &videoDispatch},
        {0x8Cu, "get_Height", Conv::Stdcall, 1, &videoDispatch},
        {0x90u, "SetDefaultSourcePosition", Conv::Stdcall, 0, &videoDispatch},
        {0x94u, "SetDefaultDestinationPosition", Conv::Stdcall, 0, &videoDispatch},
        {0x98u, "GetVideoSize", Conv::Stdcall, 2, &videoDispatch},
        {0x9Cu, "GetVideoPaletteEntries", Conv::Stdcall, 4, &videoDispatch},
        {0xA0u, "GetCurrentImage", Conv::Stdcall, 2, &videoDispatch},
        {0xA4u, "IsUsingDefaultSource", Conv::Stdcall, 0, &videoDispatch},
        {0xA8u, "IsUsingDefaultDestination", Conv::Stdcall, 0, &videoDispatch},
    };
    return m;
}

const std::vector<ComMethod>& amStreamMethods() {
    // ★ 布局按 `amstream.h` / `mmstream.h`（`IMultiMediaStream` 9 槽 + `IAMMultiMediaStream` 8 槽）：
    //   +0x0C GetInformation · +0x10 GetMediaStream · +0x14 EnumMediaStreams · +0x18 GetState
    //   +0x1C SetState · +0x20 GetTime · +0x24 GetDuration · +0x28 Seek · +0x2C GetEndOfStreamEventHandle
    //   +0x30 Initialize · +0x34 GetFilterGraph · +0x38 GetFilter · +0x3C AddMediaStream
    //   +0x40 OpenFile · +0x44 OpenMoniker · +0x48 Render
    //   取证：0x48675E `[ecx+30h]` + 3 个 0 = Initialize；0x4869C0 `[+3Ch]` = AddMediaStream；
    //   0x4869C0 `[+10h]` + 2 参 = GetMediaStream（不是 EnumMediaStreams）。
    static const std::vector<ComMethod> m = {
        {0x00u, "QueryInterface", Conv::Stdcall, 2, &streamDispatch},
        {0x04u, "AddRef", Conv::Stdcall, 0, &streamDispatch},
        {0x08u, "Release", Conv::Stdcall, 0, &streamDispatch},
        {0x0Cu, "GetInformation", Conv::Stdcall, 2, &streamDispatch},
        {0x10u, "GetMediaStream", Conv::Stdcall, 2, &streamDispatch},
        {0x14u, "EnumMediaStreams", Conv::Stdcall, 2, &streamDispatch},
        {0x18u, "GetState", Conv::Stdcall, 1, &streamDispatch},
        {0x1Cu, "SetState", Conv::Stdcall, 1, &streamDispatch},
        {0x20u, "GetTime", Conv::Stdcall, 1, &streamDispatch},
        {0x24u, "GetDuration", Conv::Stdcall, 1, &streamDispatch},
        {0x28u, "Seek", Conv::Stdcall, 2, &streamDispatch},
        {0x2Cu, "GetEndOfStreamEventHandle", Conv::Stdcall, 1, &streamDispatch},
        {0x30u, "Initialize", Conv::Stdcall, 3, &streamDispatch},
        {0x34u, "GetFilterGraph", Conv::Stdcall, 1, &streamDispatch},
        {0x38u, "GetFilter", Conv::Stdcall, 1, &streamDispatch},
        {0x3Cu, "AddMediaStream", Conv::Stdcall, 4, &streamDispatch},
        {0x40u, "OpenFile", Conv::Stdcall, 2, &streamDispatch},
        {0x44u, "OpenMoniker", Conv::Stdcall, 3, &streamDispatch},
        {0x48u, "Render", Conv::Stdcall, 1, &streamDispatch},
    };
    return m;
}

const std::vector<ComMethod>& multiMediaStreamMethods() {
    // ★ 这是 `IAMMultiMediaStream` 的**子接口**（`IMultiMediaStream`）形状；先按 base 9 槽排。
    //   0x10 才是 GetMediaStream；`Movie::Init` 的 `&unk_52FC18` QI 之后就是靠 +0x10 取流。
    static const std::vector<ComMethod> m = {
        {0x00u, "QueryInterface", Conv::Stdcall, 2, &streamChildDispatch},
        {0x04u, "AddRef", Conv::Stdcall, 0, &streamChildDispatch},
        {0x08u, "Release", Conv::Stdcall, 0, &streamChildDispatch},
        {0x0Cu, "GetInformation", Conv::Stdcall, 2, &streamChildDispatch},
        {0x10u, "GetMediaStream", Conv::Stdcall, 2, &streamChildDispatch},
        {0x14u, "EnumMediaStreams", Conv::Stdcall, 2, &streamChildDispatch},
        {0x18u, "GetState", Conv::Stdcall, 1, &streamChildDispatch},
        {0x1Cu, "SetState", Conv::Stdcall, 1, &streamChildDispatch},
        {0x20u, "GetTime", Conv::Stdcall, 1, &streamChildDispatch},
        {0x24u, "GetDuration", Conv::Stdcall, 1, &streamChildDispatch},
        {0x28u, "Seek", Conv::Stdcall, 2, &streamChildDispatch},
        {0x2Cu, "GetEndOfStreamEventHandle", Conv::Stdcall, 1, &streamChildDispatch},
    };
    return m;
}

// ---------------------------------------------------------------------------------------
// 建对象 / 查询接口
// ---------------------------------------------------------------------------------------
std::uint32_t createState(X86& cpu) {
    if (!cpu.heap || !cpu.heap->ready()) return 0;
    const std::uint32_t st = cpu.heap->alloc(kOffStateBytes, true);
    if (!st) return 0;
    cpu.mem->wr32(st + kOffMagic, kStateMagic);
    return st;
}

const std::vector<ComMethod>& methodsFor(Role::Kind role) {
    switch (role) {
        case Role::Control: return controlMethods();
        case Role::Event: return eventMethods();
        case Role::Seeking: return seekingMethods();
        case Role::Position: return positionMethods();
        case Role::Video: return videoWindowMethods();
        case Role::BasicAudio: return basicAudioMethods();
        case Role::BasicVideo: return basicVideoMethods();
        case Role::Child: return multiMediaStreamMethods();
        case Role::Stream: return amStreamMethods();
        default: return graphMethods();
    }
}

std::uint32_t slotFor(Role::Kind role) {
    switch (role) {
        case Role::Graph: return kOffGraph;
        case Role::Control: return kOffControl;
        case Role::Event: return kOffEvent;
        case Role::Seeking: return kOffSeeking;
        case Role::Position: return kOffPosition;
        case Role::Video: return kOffVideo;
        case Role::BasicAudio: return kOffBasicAudio;
        case Role::BasicVideo: return kOffBasicVideo;
        case Role::Stream: return kOffStream;
        case Role::Child: return kOffChild;
        default: return 0;
    }
}

std::uint32_t ensureInterface(X86& cpu, std::uint32_t st, Role::Kind role, bool* created) {
    if (created) *created = false;
    if (!st || !cpu.com) return 0;
    const std::uint32_t slot = slotFor(role);
    if (slot == 0) return 0;
    std::uint32_t obj = stateU32(cpu, st, slot);
    if (obj && cpu.com->find(obj)) {
        const char* face = cpu.com->ifaceOf(obj);
        // ★ 不能只看 "这个 EA 上还有对象"：释放后 12 字节块可能被另一接口复用 ⇒ 会拿错 vtable。
        //   接口名必须对得上；对不上就当缓存失效，重新建。
        if (face && std::strcmp(face, roleName(role)) == 0) return obj;
    }
    obj = cpu.com->createObject(cpu, roleName(role), methodsFor(role), st);
    if (!obj) return 0;
    stateWr32(cpu, st, slot, obj);
    // ★ 状态槽自己持一份引用：否则 guest 释放 QI 拿到的唯一引用后，槽里就是悬垂对象。
    cpu.com->addRef(cpu, obj);
    if (created) *created = true;
    return obj;
}

std::uint32_t createGraph(X86& cpu) {
    if (!cpu.com) return 0;
    const std::uint32_t st = createState(cpu);
    if (!st) return 0;
    std::uint32_t graph = 0;
    {
        bool made = false;
        graph = ensureInterface(cpu, st, Role::Graph, &made);
    }
    if (!graph) {
        cpu.heap->free(st);
        return 0;
    }
    return st;
}

std::uint32_t ensureGraphOf(X86& cpu, std::uint32_t st) {
    bool created = false;
    std::uint32_t graph = stateU32(cpu, st, kOffGraph);
    if (graph && cpu.com && cpu.com->find(graph)) return graph;
    graph = ensureInterface(cpu, st, Role::Graph, &created);
    // `ensureInterface` 给了 cache + creation 两份引用；这里只要 cache 的那份，
    // **出参**由调用方再 AddRef（否则 guest 的第一次 Release 只掉到 2，缓存/创建引用混在一起）。
    if (graph && created) cpu.com->release(cpu, graph);
    return graph;
}

void releaseStateIfOwned(X86& cpu, std::uint32_t st) {
    // 状态块是所有接口共享的；接口按引用计数释放。这里只在"图对象本身"释放到 0 时
    // **不**回收状态（接口可能比图晚释放），避免悬垂；0x40 字节的 guest 堆开销可以接受。
    (void)cpu;
    (void)st;
}

void doQueryInterface(X86& cpu, ShimCall& call, Role::Kind self_role, std::uint32_t self, std::uint32_t st) {
    const std::uint32_t riid = call.u32(1);
    const std::uint32_t ppv = call.u32(2);
    if (ppv == 0) {
        call.result = kEPointer;
        return;
    }
    if (!cpu.mem->committed(ppv, 4)) {
        call.result = kEInvalidArg;
        return;
    }
    writeU32(cpu, ppv, 0);
    std::uint8_t guid[16] = {};
    if (!readGuid(cpu, riid, guid)) {
        call.result = kEInvalidArg;
        return;
    }
    const Role::Kind role = roleForIid(guid);
    if (role == Role::Unknown) {
        if (firstTime(cpu, st, 63)) {
            vlog("QueryInterface", "this 接口=%s，未知 IID ⇒ E_NOINTERFACE（形状层只认 DirectShow 的影片接口）",
                 roleName(self_role));
        }
        call.result = kENoInterface;
        return;
    }
    std::uint32_t obj = self;
    bool addref = true;
    if (role == Role::Self) {
        obj = self;
    } else if (role == self_role) {
        obj = self;
    } else if (role == Role::Graph) {
        obj = stateU32(cpu, st, kOffGraph);
        if (!obj || !cpu.com || !cpu.com->find(obj)) obj = ensureGraphOf(cpu, st);
    } else {
        bool created = false;
        obj = ensureInterface(cpu, st, role, &created);
        addref = !created;
    }
    if (!obj || !cpu.com) {
        call.result = kEOutOfMemory;
        return;
    }
    if (addref) cpu.com->addRef(cpu, obj);
    writeU32(cpu, ppv, obj);
    if (firstTime(cpu, st, 20 + static_cast<int>(role))) {
        vlog("QueryInterface", "%s -> %s 对象 0x%08X（形状层）", roleName(self_role), roleName(role), obj);
    }
    call.result = kSOk;
}

// ---------------------------------------------------------------------------------------
// IGraphBuilder / IMediaControl / IMediaEvent / IMediaSeeking / IMediaPosition /
// IVideoWindow / IBasicAudio / IBasicVideo 的统一分派
// ---------------------------------------------------------------------------------------
void videoDispatch(X86& cpu, ShimCall& call) {
    const std::uint32_t self = call.u32(0);
    const std::string full = call.def ? call.def->name : "";
    const std::size_t sep = full.find("::");
    const std::string iface = sep == std::string::npos ? full : full.substr(0, sep);
    const std::string name = sep == std::string::npos ? full : full.substr(sep + 2);
    const std::uint32_t st = stateOf(cpu, self);

    Role::Kind role = Role::Unknown;
    if (iface == "IGraphBuilder") role = Role::Graph;
    else if (iface == "IMediaControl") role = Role::Control;
    else if (iface == "IMediaEvent") role = Role::Event;
    else if (iface == "IMediaSeeking") role = Role::Seeking;
    else if (iface == "IMediaPosition") role = Role::Position;
    else if (iface == "IVideoWindow") role = Role::Video;
    else if (iface == "IBasicAudio") role = Role::BasicAudio;
    else if (iface == "IBasicVideo") role = Role::BasicVideo;

    if (name == "QueryInterface") {
        doQueryInterface(cpu, call, role, self, st);
        return;
    }
    if (name == "AddRef") {
        if (!cpu.com || !cpu.com->addRef(cpu, self)) {
            call.result = 0;
            return;
        }
        call.result = cpu.com->refCount(self);
        return;
    }
    if (name == "Release") {
        if (!cpu.com || !cpu.com->find(self)) {
            call.result = 0;
            return;
        }
        const bool last = cpu.com->refCount(self) == 1u;
        const std::uint32_t st_local = st;
        if (!cpu.com->release(cpu, self)) {
            call.result = 0;
            return;
        }
        if (last && role == Role::Graph) releaseStateIfOwned(cpu, st_local);
        call.result = last ? 0u : cpu.com->refCount(self);
        return;
    }
    if (!st) {
        vlog(iface.c_str(), "%s: 对象没有共享状态（非本层对象？）⇒ E_FAIL", name.c_str());
        call.result = kEfail;
        return;
    }
    if (firstTime(cpu, st, static_cast<int>(role) * 8 + 1)) {
        vlog(iface.c_str(), "%s: 形状层 ⇒ 立即完成（不建真实滤镜 / 不解码）", name.c_str());
    }

    // ---- IDispatch：形状层不提供自动化 ----
    if (name == "GetTypeInfoCount") { writeU32(cpu, call.u32(1), 0); call.result = kSOk; return; }
    if (name == "GetTypeInfo") { writeU32(cpu, call.u32(3), 0); call.result = kSOk; return; }
    if (name == "GetIDsOfNames") { call.result = kENotImpl; return; }
    if (name == "Invoke") { call.result = kENotImpl; return; }

    // ---- IGraphBuilder ----
    if (role == Role::Graph) {
        if (name == "EnumFilters" || name == "FindFilterByName" || name == "AddSourceFilter") {
            if (name == "FindFilterByName") writeU32(cpu, call.u32(2), 0);
            if (name == "AddSourceFilter") writeU32(cpu, call.u32(3), 0);
            if (name == "EnumFilters") writeU32(cpu, call.u32(1), 0);
            call.result = kENotImpl;
            return;
        }
        if (name == "ShouldOperationContinue") { call.result = kSOk; return; }
        // AddFilter / RemoveFilter / Connect* / Reconnect / Disconnect / SetDefaultSyncSource /
        // Render / RenderFile / SetLogFile / Abort ⇒ S_OK
        call.result = kSOk;
        return;
    }

    // ---- IMediaControl ----
    if (role == Role::Control) {
        if (name == "Run") { stateWr32(cpu, st, kOffRunState, 2); stateWr32(cpu, st, kOffCompleted, 0); call.result = kSOk; return; }
        if (name == "Pause") { stateWr32(cpu, st, kOffRunState, 1); call.result = kSOk; return; }
        if (name == "Stop") { stateWr32(cpu, st, kOffRunState, 0); call.result = kSOk; return; }
        if (name == "GetState") {
            writeU32(cpu, call.u32(2), stateU32(cpu, st, kOffRunState));
            call.result = kSOk;
            return;
        }
        if (name == "GetFilterGraph") {
            const std::uint32_t graph = ensureGraphOf(cpu, st);
            writeU32(cpu, call.u32(1), graph);
            call.result = graph ? kSOk : kEfail;
            return;
        }
        if (name == "SetFilterGraph") { call.result = kSOk; return; }
        if (name == "AddSourceFilter") {
            writeU32(cpu, call.u32(2), 0);
            call.result = kSOk;
            return;
        }
        if (name == "RenderFile") { call.result = kSOk; return; }
        call.result = kSOk;
        return;
    }

    // ---- IMediaEvent ----
    if (role == Role::Event) {
        if (name == "GetEventHandle") { writeU32(cpu, call.u32(1), 0); call.result = kSOk; return; }
        if (name == "GetEvent") {
            writeU32(cpu, call.u32(1), kEcComplete);
            writeU32(cpu, call.u32(2), 0);
            writeU32(cpu, call.u32(3), 0);
            call.result = kSOk;
            return;
        }
        if (name == "WaitForCompletion") {
            writeU32(cpu, call.u32(2), kEcComplete);
            stateWr32(cpu, st, kOffCompleted, 1);
            if (firstTime(cpu, st, 62)) vlog("IMediaEvent", "WaitForCompletion ⇒ 立即 EC_COMPLETE（零长影片）");
            call.result = kSOk;
            return;
        }
        if (name == "GetNotifyFlags") { writeU32(cpu, call.u32(1), 0); call.result = kSOk; return; }
        call.result = kSOk;
        return;
    }

    // ---- IMediaSeeking ----
    if (role == Role::Seeking) {
        const std::string n = name;
        if (n == "GetAvailable") {
            writeU64(cpu, call.u32(1), 0);
            writeU64(cpu, call.u32(2), 0);
            call.result = kSOk;
            return;
        }
        if (n == "GetDuration" || n == "GetStopPosition" || n == "GetCurrentPosition" ||
            n == "GetPreroll") {
            writeU64(cpu, call.u32(1), 0);
            call.result = kSOk;
            return;
        }
        if (n == "GetRate") { writeU64(cpu, call.u32(1), 0x3FF0000000000000ull); call.result = kSOk; return; }
        if (n == "QueryPreferredFormat" || n == "GetTimeFormat" || n == "IsUsingTimeFormat") {
            writeGuidZero(cpu, call.u32(1));
            call.result = kSOk;
            return;
        }
        if (n == "CheckCapabilities") { writeU32(cpu, call.u32(1), 0); call.result = kSOk; return; }
        if (n == "GetCapabilities") { writeU32(cpu, call.u32(1), 0); call.result = kSOk; return; }
        if (n == "ConvertTimeFormat") { writeU64(cpu, call.u32(1), 0); call.result = kSOk; return; }
        if (n == "GetPositions") { writeU64(cpu, call.u32(1), 0); writeU64(cpu, call.u32(2), 0); call.result = kSOk; return; }
        // IsFormatSupported / QueryPreferredFormat / GetTimeFormat / IsUsingTimeFormat / SetTimeFormat /
        // SetPositions / SetRate ⇒ S_OK
        call.result = kSOk;
        return;
    }

    // ---- IMediaPosition ----
    if (role == Role::Position) {
        const std::string n = name;
        if (n == "get_Duration" || n == "get_CurrentPosition" || n == "get_StopTime" ||
            n == "get_PrerollTime") {
            writeU64(cpu, call.u32(1), 0);
            call.result = kSOk;
            return;
        }
        if (n == "get_Rate") { writeU64(cpu, call.u32(1), 0x3FF0000000000000ull); call.result = kSOk; return; }
        if (n == "CanSeekForward" || n == "CanSeekBackward") { writeU32(cpu, call.u32(1), 0); call.result = kSOk; return; }
        // put_* ⇒ S_OK
        call.result = kSOk;
        return;
    }

    // ---- IVideoWindow ----
    if (role == Role::Video) {
        // get_*（+0x20..）的最后一个参数是出参；put_* 没有出参。
        if (name.rfind("get_", 0) == 0) {
            writeU32(cpu, call.u32(1), 0);
            call.result = kSOk;
            return;
        }
        if (name == "GetWindowPosition" || name == "GetRestorePosition") {
            for (int i = 1; i <= 4; ++i) writeU32(cpu, call.u32(i), 0);
            call.result = kSOk;
            return;
        }
        if (name == "GetMinIdealImageSize" || name == "GetMaxIdealImageSize") {
            writeU32(cpu, call.u32(1), 0);
            writeU32(cpu, call.u32(2), 0);
            call.result = kSOk;
            return;
        }
        if (name == "IsCursorHidden") { writeU32(cpu, call.u32(1), 0); call.result = kSOk; return; }
        call.result = kSOk;
        return;
    }

    // ---- IBasicAudio ----
    if (role == Role::BasicAudio) {
        if (name == "get_Volume" || name == "get_Balance") { writeU32(cpu, call.u32(1), 0); call.result = kSOk; return; }
        call.result = kSOk;
        return;
    }

    // ---- IBasicVideo ----
    if (role == Role::BasicVideo) {
        if (name == "get_AvgTimePerFrame") { writeU64(cpu, call.u32(1), 0); call.result = kSOk; return; }
        if (name.rfind("get_", 0) == 0) { writeU32(cpu, call.u32(1), 0); call.result = kSOk; return; }
        if (name == "GetVideoSize") { writeU32(cpu, call.u32(1), 0); writeU32(cpu, call.u32(2), 0); call.result = kSOk; return; }
        if (name == "GetCurrentImage") { writeU32(cpu, call.u32(1), 0); writeU32(cpu, call.u32(2), 0); call.result = kSOk; return; }
        if (name == "GetSourcePosition" || name == "GetDestinationPosition") {
            for (int i = 1; i <= 4; ++i) writeU32(cpu, call.u32(i), 0);
            call.result = kSOk;
            return;
        }
        call.result = kSOk;
        return;
    }

    call.result = kSOk;
}

// ---------------------------------------------------------------------------------------
// IAMMultiMediaStream / IMultiMediaStream
// ---------------------------------------------------------------------------------------
void streamDispatch(X86& cpu, ShimCall& call) {
    const std::uint32_t self = call.u32(0);
    const std::string full = call.def ? call.def->name : "";
    const std::size_t sep = full.find("::");
    const std::string name = sep == std::string::npos ? full : full.substr(sep + 2);
    const std::uint32_t st = stateOf(cpu, self);

    if (name == "QueryInterface") {
        const std::uint32_t riid = call.u32(1);
        const std::uint32_t ppv = call.u32(2);
        if (!ppv) { call.result = kEPointer; return; }
        if (!cpu.mem->committed(ppv, 4)) { call.result = kEInvalidArg; return; }
        writeU32(cpu, ppv, 0);
        std::uint8_t guid[16] = {};
        if (!readGuid(cpu, riid, guid)) { call.result = kEInvalidArg; return; }
        const Role::Kind role = roleForIid(guid);
        if (role == Role::Self || role == Role::Stream) {
            cpu.com->addRef(cpu, self);
            writeU32(cpu, ppv, self);
            call.result = kSOk;
            return;
        }
        if (role == Role::Child) {
            bool created = false;
            std::uint32_t child = ensureInterface(cpu, st, Role::Child, &created);
            if (!child) { call.result = kEOutOfMemory; return; }
            if (!created) cpu.com->addRef(cpu, child);
            writeU32(cpu, ppv, child);
            call.result = kSOk;
            return;
        }
        // `Movie::Init` 还会 QI 引擎自己的媒体流接口（.lst 0x4869C0 `&unk_52FC18` = IID_IMediaStream；
        // 0x486B01 `&unk_52FC08` = DDraw 媒体流）。那些接口的方法要真 DDraw surface / sample；形状层给不了，
        // 所以**返回 E_NOINTERFACE**（让 `Movie::Init` 走它自己的失败分支），
        // ⛔ 不要返回一个通用对象：后续 `[+24h]` / `[+34h]` 的参数个数不同，会让 stdcall 弹错栈。
        if (firstTime(cpu, st, 61)) {
            vlog("IAMMultiMediaStream", "未知 IID ⇒ E_NOINTERFACE（引擎媒体流接口需要真 DDraw surface；形状层不冒充）");
        }
        call.result = kENoInterface;
        return;
    }
    if (name == "AddRef") { cpu.com->addRef(cpu, self); call.result = cpu.com->refCount(self); return; }
    if (name == "Release") {
        const bool last = cpu.com->refCount(self) == 1u;
        if (!cpu.com->release(cpu, self)) { call.result = 0; return; }
        call.result = last ? 0u : cpu.com->refCount(self);
        return;
    }
    if (!st) { call.result = kEfail; return; }
    if (firstTime(cpu, st, 60)) vlog("IAMMultiMediaStream", "%s: 形状层 ⇒ 立即完成", name.c_str());

    if (name == "GetInformation") {
        writeU32(cpu, call.u32(1), 0);
        writeU32(cpu, call.u32(2), 0);
        call.result = kSOk;
        return;
    }
    if (name == "GetFilterGraph") {
        const std::uint32_t graph = ensureGraphOf(cpu, st);
        // 出参接口必须带一份新引用（guest 会 Release 它）。
        if (graph) cpu.com->addRef(cpu, graph);
        writeU32(cpu, call.u32(1), graph);
        call.result = graph ? kSOk : kEfail;
        return;
    }
    if (name == "GetFilter") { writeU32(cpu, call.u32(1), 0); call.result = kSOk; return; }
    if (name == "AddMediaStream") {
        // (pStreamObject, pPurposeId, dwFlags, ppNewStream)：ppNewStream 可能为 NULL（Movie::Init 就是这样）
        writeU32(cpu, call.u32(4), 0);
        call.result = kSOk;
        return;
    }
    if (name == "GetMediaStream") {
        bool created = false;
        const std::uint32_t child = ensureInterface(cpu, st, Role::Child, &created);
        if (child && created) cpu.com->release(cpu, child);  // 只留 cache 引用
        if (child) cpu.com->addRef(cpu, child);               // 出参引用
        writeU32(cpu, call.u32(2), child);
        call.result = child ? kSOk : kEfail;
        return;
    }
    if (name == "EnumMediaStreams") { writeU32(cpu, call.u32(2), 0); call.result = kSFalse; return; }
    if (name == "GetEndOfStreamEventHandle") { writeU32(cpu, call.u32(1), 0); call.result = kSOk; return; }
    if (name == "GetState") { writeU32(cpu, call.u32(1), 0); call.result = kSOk; return; }
    if (name == "GetTime" || name == "GetDuration") { writeU64(cpu, call.u32(1), 0); call.result = kSOk; return; }
    call.result = kSOk;
}

void streamChildDispatch(X86& cpu, ShimCall& call) {
    const std::uint32_t self = call.u32(0);
    const std::string full = call.def ? call.def->name : "";
    const std::size_t sep = full.find("::");
    const std::string name = sep == std::string::npos ? full : full.substr(sep + 2);
    const std::uint32_t st = stateOf(cpu, self);

    if (name == "QueryInterface") {
        const std::uint32_t ppv = call.u32(2);
        if (!ppv) { call.result = kEPointer; return; }
        if (!cpu.mem->committed(ppv, 4)) { call.result = kEInvalidArg; return; }
        writeU32(cpu, ppv, 0);
        std::uint8_t guid[16] = {};
        if (!readGuid(cpu, call.u32(1), guid)) { call.result = kEInvalidArg; return; }
        const Role::Kind role = roleForIid(guid);
        if (role == Role::Self || role == Role::Child) {
            cpu.com->addRef(cpu, self);
            writeU32(cpu, ppv, self);
            call.result = kSOk;
            return;
        }
        if (role == Role::Graph) {
            const std::uint32_t graph = ensureGraphOf(cpu, st);
            if (graph) cpu.com->addRef(cpu, graph);
            writeU32(cpu, ppv, graph);
            call.result = graph ? kSOk : kEfail;
            return;
        }
        call.result = kENoInterface;
        return;
    }
    if (name == "AddRef") { cpu.com->addRef(cpu, self); call.result = cpu.com->refCount(self); return; }
    if (name == "Release") {
        const bool last = cpu.com->refCount(self) == 1u;
        if (!cpu.com->release(cpu, self)) { call.result = 0; return; }
        call.result = last ? 0u : cpu.com->refCount(self);
        return;
    }
    if (name == "GetInformation") {
        writeU32(cpu, call.u32(1), 0);
        writeU32(cpu, call.u32(2), 0);
        call.result = kSOk;
        return;
    }
    if (name == "GetMediaStream") {
        // `Movie` 用它拿"视频流 / 音频流"：返回同一个形状对象，后续方法统一立即完成。
        writeU32(cpu, call.u32(2), self);
        cpu.com->addRef(cpu, self);
        call.result = kSOk;
        return;
    }
    if (name == "EnumMediaStreams") {
        // 枚举到头 = S_FALSE（不是"永远 S_OK + 同一个流"；那会让调用方的 for 循环停不下来）。
        writeU32(cpu, call.u32(2), 0);
        call.result = kSFalse;
        return;
    }
    if (name == "GetState") { writeU32(cpu, call.u32(1), 0); call.result = kSOk; return; }
    if (name == "GetTime" || name == "GetDuration") { writeU64(cpu, call.u32(1), 0); call.result = kSOk; return; }
    if (name == "GetEndOfStreamEventHandle") { writeU32(cpu, call.u32(1), 0); call.result = kSOk; return; }
    call.result = kSOk;
}

// ---------------------------------------------------------------------------------------
// 导出 / 变体
// ---------------------------------------------------------------------------------------
void shimCoCreateInstance(X86& cpu, ShimCall& call) {
    const std::uint32_t rclsid = call.u32(0);
    const std::uint32_t punk_outer = call.u32(1);
    const std::uint32_t riid = call.u32(3);
    const std::uint32_t ppv = call.u32(4);

    if (ppv == 0) {
        vlog("CoCreateInstance", "ppv == NULL ⇒ E_POINTER");
        call.result = kEPointer;
        return;
    }
    if (!cpu.mem->committed(ppv, 4)) {
        call.result = kEInvalidArg;
        return;
    }
    writeU32(cpu, ppv, 0);
    if (punk_outer != 0) {
        vlog("CoCreateInstance", "pUnkOuter != NULL ⇒ CLASS_E_NOAGGREGATION（本层不支持聚合）");
        call.result = kClassENoAggregation;
        return;
    }
    if (!cpu.com || !cpu.heap || !cpu.heap->ready()) {
        vlog("CoCreateInstance", "cpu.com / guest 堆 未就绪 ⇒ E_FAIL");
        call.result = kEfail;
        return;
    }

    std::uint8_t clsid[16] = {};
    if (!readClsid(cpu, rclsid, clsid)) {
        vlog("CoCreateInstance", "rclsid 不可读 ⇒ E_INVALIDARG");
        call.result = kEInvalidArg;
        return;
    }
    std::uint8_t iid[16] = {};
    if (!readGuid(cpu, riid, iid)) {
        vlog("CoCreateInstance", "riid 不可读 ⇒ E_INVALIDARG");
        call.result = kEInvalidArg;
        return;
    }

    if (guidEq(clsid, kClsidFilterGraph) || guidEq(clsid, kClsidFilterGraphNoThread)) {
        const Role::Kind role = roleForIid(iid);
        if (role == Role::Unknown) {
            vlog("CoCreateInstance", "FilterGraph 的 riid 不是已知 DirectShow 接口 ⇒ E_NOINTERFACE");
            call.result = kENoInterface;
            return;
        }
        const std::uint32_t st = createGraph(cpu);
        if (!st) {
            vlog("CoCreateInstance", "FilterGraph：建共享状态/对象失败 ⇒ E_OUTOFMEMORY");
            call.result = kEOutOfMemory;
            return;
        }
        const std::uint32_t graph = stateU32(cpu, st, kOffGraph);
        std::uint32_t obj = 0;
        bool addref = false;
        if (role == Role::Self || role == Role::Graph) {
            obj = graph;
        } else {
            bool created = false;
            obj = ensureInterface(cpu, st, role, &created);
            addref = !created;
            // 图对象本身不返回给 guest（返回的是它的接口视图）⇒ 放掉 createGraph 的 creation 引用，
            // cache 引用仍在 `st+kOffGraph`。
            if (graph) cpu.com->release(cpu, graph);
        }
        if (!obj) {
            vlog("CoCreateInstance", "CLSID_FilterGraph：接口对象创建失败 ⇒ E_OUTOFMEMORY");
            call.result = kEOutOfMemory;
            return;
        }
        if (addref) cpu.com->addRef(cpu, obj);
        writeU32(cpu, ppv, obj);
        vlog("CoCreateInstance", "FilterGraph/NoThread ⇒ %s 对象 0x%08X（形状层：影片立即完成）",
             roleName(role == Role::Self ? Role::Graph : role), obj);
        call.result = kSOk;
        return;
    }

    if (guidEq(clsid, kClsidAmMultiMediaStream)) {
        const Role::Kind role = roleForIid(iid);
        if (role != Role::Self && role != Role::Stream && role != Role::Child) {
            vlog("CoCreateInstance", "CLSID_AMMultiMediaStream 的 riid 不是 IAMMultiMediaStream/IUnknown ⇒ E_NOINTERFACE");
            call.result = kENoInterface;
            return;
        }
        std::uint32_t st = createState(cpu);
        if (!st) { call.result = kEOutOfMemory; return; }
        bool created = false;
        const std::uint32_t stream = ensureInterface(cpu, st, Role::Stream, &created);
        if (!stream) { cpu.heap->free(st); call.result = kEOutOfMemory; return; }
        if (!created) cpu.com->addRef(cpu, stream);
        writeU32(cpu, ppv, stream);
        vlog("CoCreateInstance", "CLSID_AMMultiMediaStream ⇒ IAMMultiMediaStream 对象 0x%08X（形状层）", stream);
        call.result = kSOk;
        return;
    }

    vlog("CoCreateInstance", "不支持的 CLSID（Data1=0x%08X）⇒ CLASS_E_CLASSNOTAVAILABLE（本层只做 DirectShow 影片的 CLSID）",
         cpu.mem->rd32(rclsid));
    call.result = kClassEClassNotAvailable;
}

// ---- OLE 内存 / VARIANT ----
void shimPropVariantClear(X86& cpu, ShimCall& call) {
    const std::uint32_t pv = call.u32(0);
    if (pv == 0) { call.result = kSOk; return; }
    // PROPVARIANT：vt + 3 个 WORD 保留，value 从 +8 开始。只做"清零"，不冒充释放复杂类型。
    if (cpu.mem->committed(pv, 16)) {
        const std::uint16_t vt = cpu.mem->rd16(pv);
        static bool warned = false;
        if (vt != 0 && !warned) {
            vlog("PropVariantClear", "vt=%u（复杂类型未实现释放；只清零，避免把错误当成功；后续同类不再刷屏）", static_cast<unsigned>(vt));
            warned = true;
        }
        for (int i = 0; i < 16; ++i) cpu.mem->wr8(pv + static_cast<std::uint32_t>(i), 0);
    }
    call.result = kSOk;
}

void shimSysStringLen(X86& cpu, ShimCall& call) {
    const std::uint32_t bstr = call.u32(0);
    if (bstr < 4 || !cpu.mem->committed(bstr - 4, 4)) { call.result = 0; return; }
    call.result = cpu.mem->rd32(bstr - 4) / 2u;  // BSTR 头是**字节**长度；SysStringLen 返回**字符**数
}

void shimVariantInit(X86& cpu, ShimCall& call) {
    const std::uint32_t pv = call.u32(0);
    if (pv && cpu.mem->committed(pv, 16)) {
        for (int i = 0; i < 16; ++i) cpu.mem->wr8(pv + static_cast<std::uint32_t>(i), 0);
    }
    call.result = kSOk;
}

void shimVariantClear(X86& cpu, ShimCall& call) {
    const std::uint32_t pv = call.u32(0);
    if (pv == 0) { call.result = kSOk; return; }
    if (cpu.mem->committed(pv, 16)) {
        const std::uint16_t vt = cpu.mem->rd16(pv);
        static bool warned = false;
        if (vt != 0 && !warned) {
            vlog("VariantClear", "vt=%u（复杂类型未实现释放；只清零；后续同类不再刷屏）", static_cast<unsigned>(vt));
            warned = true;
        }
        for (int i = 0; i < 16; ++i) cpu.mem->wr8(pv + static_cast<std::uint32_t>(i), 0);
    }
    call.result = kSOk;
}

}  // namespace

void registerQuartzVideoShims(ShimRegistry& s) {
    // ★ 用户 2026-11-06 口径：视频**不再是硬 trap**；先做"形状层 + 立即完成"，
    //   让 boot / 标题前的影片准备不再停在 ole32!CoCreateInstance。真解码是 M4 的后续。
    s.addStub("ole32", "CoCreateInstance", Conv::Stdcall, 5, &shimCoCreateInstance,
              "M4 视频形状层：CLSID_FilterGraph / CLSID_AMMultiMediaStream 返回立即完成的形状对象；其余 CLSID ⇒ CLASS_E_CLASSNOTAVAILABLE（不冒充支持）");
    s.addStub("ole32", "PropVariantClear", Conv::Stdcall, 1, &shimPropVariantClear,
              "M2/M4：PROPVARIANT 清零（复杂类型只记一行，不冒充释放）");
    s.addStub("oleaut32", "#7", Conv::Stdcall, 1, &shimSysStringLen,
              "M2/M4：oleaut32 序号 7 = SysStringLen（按 BSTR 头读长度）");
    s.addStub("oleaut32", "#8", Conv::Stdcall, 1, &shimVariantInit,
              "M2/M4：oleaut32 序号 8 = VariantInit（16 字节清零）");
    s.addStub("oleaut32", "#9", Conv::Stdcall, 1, &shimVariantClear,
              "M2/M4：oleaut32 序号 9 = VariantClear（简单清零；复杂类型留日志）");
}

}  // namespace lift
