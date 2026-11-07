// emulator-lift · 宿主侧 DirectShow 服务端的**协议核心**（纹理影片真实现的第一段）
//
// ★ 实现契约是 `docs/movie-dshow-protocol.md`（下称 doc），本文件只写**接口**；
//   取值与槽序的**锚**一律是 `corpus/disasm/files/AGE.EXE__dumped.sectfix.EXE.lst` 的 EA。
// ★ 边界（SCOPE §1「视频 = 延后实现」→ 本段只做协议核心，不接线）：
//   · 这里**不碰** `src/shims/quartz_video.cpp`（下一段才把 `IGraphBuilder` / `IMediaControl` /
//     `IMediaPosition` 的槽接到本模块）；
//   · 帧源、时钟都**注入**（`FrameFeed` / `Now100ns`）⇒ pure 测试不碰真影片，本模块
//     ⛔ 不读 steady_clock（宿主只看 `cpu.clock`，由上层换算成 100ns 传进来）。
// ★ 线程：单线程（被 shim 调）。⛔ 不自建线程、⛔ 不等待（SCOPE §1 的硬错误）。
//
// ★ 两个与任务书不同的关键 ABI 事实（都有 `.lst` 逐字证据，详见各常量注释）：
//   ① `IBaseFilter::Pause` **没有** `tStart` 参数（`sub_4D5860` = `int __stdcall(int)`，`retn 4`），
//      只有 `Run` 有（`sub_4D5980` = `retn 0Ch` = this + 8 字节 `REFERENCE_TIME`）。
//      多压 4 字节会让 guest 栈每调一次漂 4 字节 ⇒ 这里按 `.lst` 走。
//   ② `IPin::QueryDirection` 在 pin vtable **+0x24**（`0x52F57C+0x24 = sub_4D1060`），
//      不是 +0x0C（+0x0C 是 `Connect`）。槽序以 `strmif.h` 为准。
#pragma once

#include <cstdarg>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "runtime/log.h"

namespace lift {
struct X86;
}

namespace lift::dshow {

// =====================================================================================
// §1 GUID / HRESULT / 布局常量
// =====================================================================================

// 16 字节 GUID（guest 侧就是这段布局：Data1/Data2/Data3 小端 + 8 字节 Data4）。
struct Guid {
    std::uint32_t data1 = 0;
    std::uint16_t data2 = 0;
    std::uint16_t data3 = 0;
    std::uint8_t data4[8] = {};
};

inline bool operator==(const Guid &a, const Guid &b) {
    if (a.data1 != b.data1 || a.data2 != b.data2 || a.data3 != b.data3)
        return false;
    for (int i = 0; i < 8; ++i)
        if (a.data4[i] != b.data4[i])
            return false;
    return true;
}
inline bool operator!=(const Guid &a, const Guid &b) {
    return !(a == b);
}

// `MEDIATYPE_Video` —— ★ **DX9 的 "vids" GUID**：引擎的影片渲染器 `CMovieTextureRenderer::CheckMediaType`
//   （`sub_4878B0`）拿 `majortype` 与 `aVids`（`.data:0x52FAA8 db 'vids',0` 起的 16 字节）比：
//     `0x4878EA push offset aVids` · `0x4878E3 call sub_4028A0`（IsEqualGUID）· 不等于 ⇒ `E_FAIL`。
//   那 16 字节 = `76 69 64 73 00 00 10 00 80 00 00 AA 00 38 9B 71` = {73646976-0000-0010-8000-00AA00389B71}。
//   ⚠ 之前这里写成旧 SDK 的 `E436EB7D`（= 0x52FAC8）⇒ CheckMediaType 直接 E_FAIL ⇒
//     `ReceiveConnection` 报 **VFW_E_TYPE_NOT_ACCEPTED(0x8004022A)**（实测）。
inline constexpr Guid kMajorTypeVideo{
    0x73646976, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};
// `MEDIASUBTYPE_RGB24` —— 引擎接受的 **subtype** 之一：`.data:0x52FAC8 dd 0E436EB7Dh, 11CE524Fh, …`
//   （`sub_4878B0` 的第二个 `sub_4028A0(a1+4, dword_52FAC8)`）。这是老 SDK 里与 MEDIATYPE_Video 同值的那个
//   GUID —— 但**在这里它是 subtype**，majortype 用上面的 "vids"。⛔ 别把两个混用（实测会被拒）。
inline constexpr Guid kSubTypeRgb24{
    0xE436EB7D, 0x524F, 0x11CE, {0x9F, 0x53, 0x00, 0x20, 0xAF, 0x0B, 0xA7, 0x70}};
// `MEDIASUBTYPE_ARGB32` —— `.data:0x52FAD8 dd 773C9AC0h, 11D03274h, 0AA0024B7h, 11A6C00h`。
//   ★ 只在这里**标注**它（免得有人「顺手」用它）：用了就变 4 字节/像素。
inline constexpr Guid kSubTypeArgb32{
    0x773C9AC0, 0x3274, 0x11D0, {0xB7, 0x24, 0x00, 0xAA, 0x00, 0x6C, 0x1A, 0x01}};
// `FORMAT_VideoInfo` —— ★ **不是** FORMAT_VideoInfo2：`.data:0x52FB48 dd 05589F80h, 11CEC356h, 0AA0001BFh,
// 5A595500h`
//   = {05589F80-C356-11CE-BF01-00AA0055595A}。`sub_4878B0` 的 `sub_4028A0(a1+11, dword_52FB48)` 比的是
//   `pmt->formattype`（AM_MEDIA_TYPE +0x2C）；不等 ⇒ `E_INVALIDARG`（`0x4878B0` 的第二条返回）。
//   ⚠ 之前这里写成 `F72A76A0`（那才是 VideoInfo2）⇒ 同样被拒。
inline constexpr Guid kFormatVideoInfo{
    0x05589F80, 0xC356, 0x11CE, {0xBF, 0x01, 0x00, 0xAA, 0x00, 0x55, 0x59, 0x5A}};
// `IID_IMemInputPin` —— `.data:0x52FCB8 dd 56A8689Dh, 11CE0AD4h, 20003AB0h, 70A70BAFh`（逐字）。
inline constexpr Guid kIidMemInputPin{
    0x56A8689D, 0x0AD4, 0x11CE, {0xB0, 0x3A, 0x00, 0x20, 0xAF, 0x0B, 0xA7, 0x70}};

// `IID_IAsyncReader` —— ★ **不是** DX9 `strmif.h` 记忆里的 56A868A0：本引擎二进制里这个键是
//   `.data:0x52FD18 dd 56A868AAh, 11CE0AD4h, 20003AB0h, 70A70BAFh`，由**源 pin 自己的
//   NonDelegatingQueryInterface** 比较它：
//     `sub_402A00`（= `CAsyncOutputPin` 的 `NonDelegatingQueryInterface`，vtable @`.data:0x51D920`）：
//       `0x402A13 push offset dword_52FD18` · `0x402A15 call sub_4028A0`（IsEqualGUID）·
//       命中 ⇒ `[this+0xA0]=1`（`m_bQueriedForAsyncReader`）· `sub_4D3430(this-8, ppv)`（返回 **对象基址**
//       = `IAsyncReader*`，见 RTTI `??_R2CAsyncOutputPin@@8` @`.data:0x53C434`：IAsyncReader @0）。
//   ⇒ 用错 IID 只会得到 E_NOINTERFACE（引擎对象上不会有别的接口认这个值）。
//   ⚠ `sub_402970` 是 `DECLARE_IUNKNOWN` 的转发 thunk（`[this+0xC]` = m_pUnknown = 自己的
//   CBasePin 子对象 ⇒ 转到上面这个 NonDelegatingQueryInterface），它自己**没有** GUID 比较。
inline constexpr Guid kIidAsyncReader{
    0x56A868AA, 0x0AD4, 0x11CE, {0xB0, 0x3A, 0x00, 0x20, 0xAF, 0x0B, 0xA7, 0x70}};

// ---- IAsyncReader 的 vtable 槽（**按本引擎的槽序**，不是记忆里的 strmif.h 顺序）----
//   vtable @`.data:0x51D95C`（RTTI `??_7CAsyncOutputPin@@6B@`），逐槽 `retn` 与实现体对过：
//     +0x0C sub_402C70（retn 10h）· +0x10 sub_402DA0（retn 0Ch）· +0x14 sub_403030（retn 10h）
//     +0x18 sub_402EE0（retn 8，参数是 `IMediaSample*` ⇒ SyncReadAligned）
//     +0x1C sub_403090（retn 14h ⇒ `(this, llPosition_lo, llPosition_hi, lLength, pBuffer)`）⇒ **SyncRead**
//     +0x20 sub_4030C0（retn 0Ch ⇒ `(this, LONGLONG* pTotal, LONGLONG* pAvailable)`）⇒ **Length**
//     +0x24 sub_4030E0（retn 4）· +0x28 sub_403100（retn 4）
//   ★ 槽内实现：`sub_403090` → `sub_401EF0(CAsyncIo, pos_lo, pos_hi, len, buf)`（同步 SetPointer+Read）；
//     `sub_4030C0` → `sub_401E90(CAsyncIo, pTotal, pAvailable)`。两者都**不含** `WaitForSingleObject`。
inline constexpr std::uint32_t kAsyncVtblLength = 0x20;
inline constexpr std::uint32_t kAsyncVtblSyncRead = 0x1C;

// HRESULT（只放本模块真的用到 / 有证据的那几个；⛔ 不凭记忆编 VFW 码）。
inline constexpr std::uint32_t kSOk = 0x00000000u;
inline constexpr std::uint32_t kSFalse = 0x00000001u;
inline constexpr std::uint32_t kENotImpl = 0x80004001u;
inline constexpr std::uint32_t kENoInterface = 0x80004002u;
inline constexpr std::uint32_t kEPointer = 0x80004003u;
inline constexpr std::uint32_t kEFail = 0x80004005u;
inline constexpr std::uint32_t kEOutOfMemory = 0x8007000Eu;
inline constexpr std::uint32_t kEInvalidArg = 0x80070057u;
// `VFW_E_NOT_COMMITTED`（doc §2-C 逐字：分配器忙时用它，⛔ 不阻塞）
inline constexpr std::uint32_t kVfwENotCommitted = 0x80040209u;
// `CBasePin::ReceiveConnection` 的 `0x4D0D6F mov eax,80040204h` = VFW_E_ALREADY_CONNECTED
inline constexpr std::uint32_t kVfwEAlreadyConnected = 0x80040204u;
// doc §3-2：`0x488C5B mov eax,8004022Ah` = VFW_E_TYPE_NOT_ACCEPTED
inline constexpr std::uint32_t kVfwETypeNotAccepted = 0x8004022Au;
// `CBasePin::ReceiveConnection` 的第三条拒绝路径 `0x4D0D9E mov eax,80040224h`（未定名，照抄）
inline constexpr std::uint32_t kVfwEUnknown40224 = 0x80040224u;

inline bool succeeded(std::uint32_t hr) {
    return static_cast<std::int32_t>(hr) >= 0;
}

// `AM_MEDIA_TYPE` 的 guest 布局（doc §3-2：`0x488AB3 mov eax,[ecx+44h]` ⇒ pbFormat 在 +0x44）。
namespace amt {
constexpr std::uint32_t kMajorType = 0;
constexpr std::uint32_t kSubType = 16;
constexpr std::uint32_t kFixedSizeSamples = 32;
constexpr std::uint32_t kTemporalCompression = 36;
constexpr std::uint32_t kSampleSize = 40;
constexpr std::uint32_t kFormatType = 44;
constexpr std::uint32_t kUnk = 60;
constexpr std::uint32_t kCbFormat = 64;
constexpr std::uint32_t kPbFormat = 68; // 0x44
constexpr std::uint32_t kBytes = 72;
} // namespace amt

// `VIDEOINFOHEADER`：`bmiHeader` 在 **+0x30**（doc §3-2），`biWidth`/`biHeight` 在 +0x34/+0x38。
namespace vih {
constexpr std::uint32_t kRcSource = 0;
constexpr std::uint32_t kRcTarget = 16;
constexpr std::uint32_t kBitRate = 32;
constexpr std::uint32_t kBitErrorRate = 36;
constexpr std::uint32_t kAvgTimePerFrame = 40; // REFERENCE_TIME（i64）
constexpr std::uint32_t kBmiHeader = 48;       // 0x30
constexpr std::uint32_t kBytes = 88;
} // namespace vih

namespace bmi {
constexpr std::uint32_t kSize = 0;
constexpr std::uint32_t kWidth = 4;  // [eax+34h]
constexpr std::uint32_t kHeight = 8; // [eax+38h]（doc §3-2 允许负高）
constexpr std::uint32_t kPlanes = 12;
constexpr std::uint32_t kBitCount = 14;
constexpr std::uint32_t kCompression = 16;
constexpr std::uint32_t kSizeImage = 20;
constexpr std::uint32_t kBytes = 40;
} // namespace bmi

// =====================================================================================
// §2 日志（本模块唯一出口；分类 LogVideo —— README §2.5 / SCOPE §3「必须留痕」）
// =====================================================================================
void dshowLog(LogLevel level, const char *fmt, ...);

// =====================================================================================
// §3 注入的时钟与帧源
// =====================================================================================

// 宿主只认 `cpu.clock`（虚拟时钟）换算出来的 100ns；⛔ 本模块不自己读 steady_clock。
using Now100ns = std::function<std::int64_t()>;

// 由上层用 `video::MovieDecoder` 实现（本模块只消费）。
// ★ `at()` 是**时间寻址**的：给 `now` ⇒ 返回「该显示的那一帧」（`pts <= now` 里最新的一帧）。
//   与 `MovieDecoder::decodeTo` 的语义一致（同一帧不重复返回 ⇒ 返回 false）。
// 一段解码好的音频（宿主侧播放；同步在 `AudioRenderer` 里，见 audio_render.h）。
struct AudioChunk {
    std::int64_t pts_100ns = 0;  // 相对影片开头
    std::uint32_t samplerate = 0;
    std::uint32_t channels = 0;  // 1 / 2
    std::vector<std::int16_t> pcm;  // 交织（单声道时只有一个通道）
};

struct FrameFeed {
    virtual ~FrameFeed() = default;
    virtual bool info(std::uint32_t *w, std::uint32_t *h, std::int64_t *duration_100ns) = 0;
    // RGB24 顶朝下（top-down）；`*rgb24` 只需在下次 `at()` 之前有效。
    virtual bool at(std::int64_t now_100ns, const std::uint8_t **rgb24, std::uint32_t *stride,
                    std::int64_t *pts_100ns, bool *ended) = 0;
    virtual bool seek(std::int64_t t_100ns) = 0;
    virtual void rewind() = 0;
    // 音频（可选；默认"没有音频"）。到结尾 ⇒ false 且 `*ended = true`。
    virtual bool nextAudio(AudioChunk *out, bool *ended) {
        (void)out;
        if (ended) *ended = false;
        return false;
    }
};

// =====================================================================================
// §4 诊断
// =====================================================================================
struct Stats {
    std::uint32_t connect_hr = 0; // IPin::ReceiveConnection 的 HRESULT
    std::uint32_t pin_count = 0;  // 本次 Render 枚举到的 pin 数
    std::uint64_t samples_delivered = 0;
    std::uint64_t frames_decoded = 0; // 帧源交出过多少帧（含被丢的）
    std::uint64_t receive_calls = 0;
    std::uint64_t dropped_busy = 0; // 渲染器忙 / 分配器无空缓冲而丢的帧
    std::int64_t last_pts_100ns = 0;
    bool completed = false;
    // ---- 以下为**追加**诊断（任务书的 Stats 之外的取证用字段；下一段接线要看它们）----
    std::uint32_t notify_allocator_hr = kEFail; // IMemInputPin::NotifyAllocator 的 HRESULT
    std::uint32_t allocator_ea = 0;             // 交给 NotifyAllocator 的宿主分配器（guest EA）
    std::uint32_t sample_ea = 0;                // 宿主 IMediaSample（guest EA）
    std::uint32_t input_pin_ea = 0;             // 渲染器输入 pin（IPin*，guest EA）
    std::uint32_t mem_input_ea = 0;             // 该 pin 的 IMemInputPin*（guest EA）
    std::uint32_t media_type_ea = 0;            // 送进 ReceiveConnection 的 AM_MEDIA_TYPE*

    // ---- 以下为**源读取**（真 `IAsyncReader`）诊断；判据只看这些 + `[dshow]` 日志 ----
    std::uint32_t source_qi_hr = kEFail;     // 源 pin QI(IID_IAsyncReader) 的 HRESULT
    std::uint32_t source_pin = 0;            // 引擎给的源输出 IPin*（0x489645 的入参）
    std::uint32_t source_length_hr = kEFail; // IAsyncReader::Length 的 HRESULT
    std::int64_t source_total = 0;           // Length 的 pTotal（原样）
    std::int64_t source_available = 0;       // Length 的 pAvailable（原样）
    std::uint64_t source_bytes_read = 0;     // 实际拷进宿主 vector 的字节数
    std::uint32_t source_first4 = 0;         // 头 4 字节（小端 DWORD；MPEG-PS 期望 0xBA010000）
    std::uint32_t source_chunks = 0;         // SyncRead 成功次数
    std::uint32_t source_iasync_ea = 0;      // IAsyncReader*（guest EA）
    std::uint32_t source_vtable_ea = 0;      // 它的 vtable（guest EA；对 0x51D95C 就是 CAsyncOutputPin）
    std::uint32_t source_read_buf_bytes = 0; // 每块读多大（宿主实际采用的）
    bool decoder_opened = false;
    std::uint32_t decoder_width = 0;
    std::uint32_t decoder_height = 0;
};

// =====================================================================================
// §5 图（被 COM shim 调；宿主建对象、方法由 shim 路由）
// =====================================================================================
class HostObjects; // objects.h（实现细节：宿主 IPin / IMemAllocator / IMediaSample）

// ★ 线程 / 生命周期：单线程，被 shim 调。**Graph 必须活到所有在飞的 guest 调用链结束**
//   （链的续延捕获了 `this`）；每次 `run/stop/pause/render/currentPosition` 之后
//   shim 必须检查 `suspended()`。
class AudioRenderer;

class Graph {
  public:
    Graph(X86 &cpu, Now100ns now, FrameFeed *feed);
    ~Graph();
    Graph(const Graph &) = delete;
    Graph &operator=(const Graph &) = delete;

    // 图管理器对象（guest 侧的 `IGraphBuilder`/`IFilterGraph` 视图 EA）。
    // ★ 只用于 `IBaseFilter::JoinFilterGraph(pGraph, pName)`：`CBaseFilter::Pause/Run` 会
    //   `mov eax,[filter+0x6C]; cmp dword ptr [eax+18h],0`（`0x4D5896`/`0x4D59C6`）——
    //   `[filter+0x6C]` 为 0 ⇒ **guest 读 [0+0x18] 直接 fault**。引擎自己 AddFilter 之后不会替我们 join。
    void setGraphUnknown(std::uint32_t ea) {
        graph_unknown_ = ea;
    }

    // ---- 图管理器（被 COM shim 调）----
    // `filter_obj` = 引擎传进来的 **IBaseFilter\***（渲染器是 `renderer+0xC`、源是 `source+0xC`）。
    // `name_ea` = guest 侧 `LPCWSTR`（渲染器 = `L"TextureRenderer"`，源为 NULL）；只用来**识别**与
    //             转交给 `JoinFilterGraph`，⛔ 不当作唯一依据（名字为空时按「先加的是渲染器、后加的是源」）。
    bool addFilter(std::uint32_t filter_obj, std::uint32_t name_ea, std::string *err);
    // D1（doc §2-D）：先**从引擎自己的源 pin 取字节**（QI IAsyncReader → Length → 分块 SyncRead →
    // `video::MovieDecoder`），再接渲染器一侧（EnumPins → ReceiveConnection → QI(IMemInputPin) →
    // NotifyAllocator）。⛔ 宿主不解析 ALF / 不猜文件名：字节只有这一条来路。
    // 返回值口径：true = 接线已发起/已完成（真正的成败看 `stats()`：引擎**不看** Render 的 HRESULT）。
    bool render(std::uint32_t source_pin, std::string *err);

    // ---- 控制 ----
    // run() = Pause(渲染器) + Pause(源) + Run(源)，然后开始计时。
    // ★ 渲染状态策略 = **保持渲染器在 Paused**（见 graph.cpp 的说明与 doc §2-E 的证据）：
    //   Paused 下 `CBaseRenderer::Receive`（sub_4D4000，`[this+0x14]==1` 分支）当场渲染，
    //   不必依赖多媒体定时器回调（那是 guest 函数 ⇒ 只能在块边界插，且会引入时序耦合）。
    void run();
    void pause();
    void stop();
    int state() const; // 0=Stopped 1=Paused 2=Running（State_*）

    // ---- 时间（秒；引擎按 double 读）----
    // ★ 非 const：会推进泵（可能发起一次 guest `Receive`）。任务书写的是 const，这里点明偏离。
    double currentPosition();
    double stopPosition() const; // 片长；未 open ⇒ 0
    double duration() const;

    // ---- 声音（只记账）----
    void setVolumeDb(double db);
    int volumeDb100() const {
        return volume_db100_;
    }

    // ---- 诊断 ----
    const Stats &stats() const {
        return stats_;
    }
    bool opened() const {
        return opened_;
    }

    // ---- 与 shim 的接缝（下一段接线用；本段只被纯测试用）----
    // true = 当前有 guest 调用在飞 ⇒ shim **必须** `call.take_control = true`。
    bool suspended() const {
        return suspended_;
    }
    // = 4 + argc*4（`GuestCallStack::start` 收尾时要把 guest 栈抬回调用点，见 guest_call.h）。
    // ★ 每次 shim 调用前设一次（shim 手里有 `call.stdcall_bytes`）；不设 ⇒ 默认 4（0 参 stdcall）。
    void setHostCallCleanup(std::uint32_t bytes) {
        cleanup_bytes_ = bytes ? bytes : 4u;
    }
    std::uint32_t hostCallCleanup() const {
        return cleanup_bytes_;
    }

  private:
    struct Filter {
        std::uint32_t ea = 0; // IBaseFilter*（guest EA）
        std::string name;     // LPCWSTR 转来的（只用于识别/日志）
    };

    bool ensureObjects(std::string *err);
    bool startChain(int first_step, const char *api);
    // 按步号发起**这一步**的 guest 调用（args[0] = this，stdcall 的 this 在栈上）。
    bool launch(int step, std::string *err);
    bool continueChain(std::uint32_t result, std::uint32_t *shim_result);
    bool stepLaunch(std::string *err);
    bool finishChain();
    // 「源读取」阶段的下一步（同一套链；在续延里调用）。
    bool startSourceRead(std::string *err);
    // 源读完之后：组 media type / 分配像素缓冲 / 填 stats —— **不**发起 guest 调用。
    bool prepareWiring(std::string *err);
    // 链尾的状态收尾（Run 开始计时 / Pause 冻结位置 / Stop 归零）
    bool endRunChain();
    bool endPauseChain();
    bool endStopChain();
    void pump();
    // 把一帧 RGB24（顶朝下）自底向上拷进样本缓冲 + 写时间戳/长度
    bool copyFrame(const std::uint8_t *rgb, std::uint32_t src_stride, std::int64_t pts);
    void complete();
    void releaseAll();
void pumpAudio(std::int64_t now_100ns);
    void releaseSource();
    // 读 guest 侧 LPCWSTR（只用于识别/日志；⛔ 不是引擎对象偏移）
    std::string readName(std::uint32_t name_ea) const;

    X86 &cpu_;
    Now100ns now_;
    FrameFeed *feed_ = nullptr;
    // ★ 注入的 feed（pure 测试）与"真源读取后由本对象持有"的 feed 分开记：
    //   注入 ⇒ `render` **完全不碰源 pin**（doc §2-D 的 D1 口径，旧用例的判据）；
    //   非注入 ⇒ `render` 先走 IAsyncReader 读取，再把 `owned_feed_` 当帧源。
    bool feed_injected_ = false;
    std::unique_ptr<FrameFeed> owned_feed_;

    std::vector<Filter> filters_;
    std::uint32_t renderer_ = 0;
    std::uint32_t source_ = 0;
    std::uint32_t graph_unknown_ = 0;       // JoinFilterGraph 的 pGraph（见 setGraphUnknown）
    std::uint32_t addfilter_join_ea_ = 0;   // 正在补 JoinFilterGraph 的 filter
    std::uint32_t addfilter_join_name_ = 0; // 它的 LPCWSTR

    // ---- 源读取现场 ----
    std::uint32_t source_pin_ = 0;       // 引擎给的 IPin*（0x489645 的入参）
    std::uint32_t iasync_ = 0;           // QI 到的 IAsyncReader*
    std::uint32_t iid_async_reader_ = 0; // 16 字节 guest 内存：IID_IAsyncReader
    std::uint32_t read_buf_ = 0;         // 分块读的 guest 缓冲
    std::uint32_t read_buf_bytes_ = 0;
    std::int64_t source_total_ = 0;          // Length 的 pTotal（<=0 ⇒ 未知）
    std::int64_t source_pos_ = 0;            // 已读到的位置
    std::int64_t source_cap_ = 0;            // 未知总长时的保守上限
    std::uint32_t read_chunk_ = 0;           // 本块请求的字节数
    std::vector<std::uint8_t> source_bytes_; // ★ 必须活到 decoder 销毁（MovieDecoder::open 零拷贝）

    Stats stats_;
    int state_ = 0; // State_Stopped
    bool opened_ = false;
    bool wired_ = false;
    bool suspended_ = false;
    bool completed_ = false;
    std::uint32_t cleanup_bytes_ = 4;
    double volume_db_ = 0.0;
    int volume_db100_ = 0;

    std::int64_t duration_100ns_ = 0;
    double duration_sec_ = 0.0;
    std::int64_t start_100ns_ = 0;
    std::int64_t last_pump_100ns_ = 0;
    std::int64_t last_pts_100ns_ = 0;
    bool has_pts_ = false;
    std::int64_t frozen_100ns_ = 0;

    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
    std::uint32_t stride_ = 0;

    // 链的现场
    int step_ = 0;
    std::uint32_t renderer_enum_ = 0;
    std::uint32_t candidate_pin_ = 0;
    std::uint32_t input_pin_ = 0;
    std::uint32_t mem_input_ = 0;

    // guest 侧的出参暂存 / 媒体类型 / 宿主对象
    std::uint32_t scratch_ = 0;       // 32 字节（出参）
    std::uint32_t iid_mem_input_ = 0; // 16 字节（IID_IMemInputPin）
    std::uint32_t media_type_ = 0;
    std::uint32_t video_info_ = 0;
    std::unique_ptr<AudioRenderer> audio_;  // 影片音频支路（同步在它里面，见 audio_render.h）

std::unique_ptr<HostObjects> objs_;
};

// =====================================================================================
// §6 AM_MEDIA_TYPE / VIDEOINFOHEADER 的构造与释放（media_type.cpp）
// =====================================================================================

struct MediaTypeBlock {
    std::uint32_t media_type_ea = 0;
    std::uint32_t video_info_ea = 0;
    std::uint32_t bytes = 0; // 两块加起来（诊断）
};

// RGB24 的行距（渲染器口径：`(w*3+3)&~3`，doc §3-2 `0x488B10`）。
std::uint32_t strideForRgb24(std::uint32_t width);

// 在 guest 堆上建 AM_MEDIA_TYPE + VIDEOINFOHEADER：
//   majortype = MEDIATYPE_Video，subtype = MEDIASUBTYPE_RGB24，formattype = FORMAT_VideoInfo，
//   cbFormat = 88，bFixedSizeSamples = 1，lSampleSize = stride*h，
//   biWidth = w，**biHeight = +h**（渲染器自己按自底向上读），biBitCount = 24，
//   biCompression = BI_RGB(0)，biSizeImage = stride*h，AvgTimePerFrame = avg_time。
bool buildVideoMediaType(X86 &cpu, std::uint32_t width, std::uint32_t height, std::int64_t avg_time_100ns,
                         MediaTypeBlock *out, std::string *err);
void freeMediaType(X86 &cpu, MediaTypeBlock *mt);

} // namespace lift::dshow
