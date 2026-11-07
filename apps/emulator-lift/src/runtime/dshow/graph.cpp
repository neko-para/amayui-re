// emulator-lift · DirectShow 图的宿主实现（filter 表 / 接线 / 控制 / 位置 / 泵）
//
// ★ 契约：`docs/movie-dshow-protocol.md`。本文件的每个 **vtable 偏移** 都标了 `.lst` 依据。
// ★ 一条链 = 一串 guest 调用（`GuestCallStack` 的续延机制，见 guest_invoke.h）：
//   宿主方法同步返回，`suspended()` 为 true ⇒ shim 把控制权交回 dispatcher，
//   后续步骤在**同一个续延**里跑（`resume()` 每次都把结果交回最外层的续延，见 guest_call.h）。
//   ⛔ 同一时刻只允许一条链（`suspended_`；重入直接拒绝 + 日志）。
// ★ 线程：单线程；本文件不建线程、不 sleep、不等待（SCOPE §1）。
#include "runtime/com.h"
#include "runtime/d3d9.h"
#include "runtime/dshow/decoder_feed.h"
#include "runtime/dshow/dshow.h"
#include "runtime/dshow/guest_invoke.h"
#include "runtime/dshow/objects.h"
#include "runtime/guest_call.h"
#include "runtime/heap.h"
#include "runtime/memory.h"
#include "runtime/x86.h"
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <utility>
namespace lift::dshow {
namespace {
// ---- 引擎对象的 vtable 偏移（doc §2-E / §2-B，`.lst` 逐字）----
// 渲染器 IBaseFilter vtable @`.data:0x52F43C`：+0x0C sub_4CFCF0、+0x10 sub_4D5780(Stop)、
// +0x14 sub_4D5860(Pause)、+0x18 sub_4D5980(Run)、+0x1C sub_4D3BB0(GetState)、
// +0x28 sub_4D30A0(EnumPins)、+0x34 sub_4D02B0(JoinFilterGraph)。
constexpr std::uint32_t kFilterStop = 0x10;
constexpr std::uint32_t kFilterPause = 0x14;
constexpr std::uint32_t kFilterRun = 0x18;
constexpr std::uint32_t kFilterSetSyncSource = 0x20; // `sub_4CFD60`（retn 8 = this + clock）
constexpr std::uint32_t kFilterEnumPins = 0x28;
// ★ `IBaseFilter::JoinFilterGraph(IFilterGraph*, LPCWSTR)` = `sub_4D02B0`（`retn 0Ch` = this + 2 参）。
//   `CBaseFilter::Pause` 在 `0x4D5896` 读 `[filter+0x6C]` 再 `cmp dword ptr [eax+18h],0`；
//   `Run` 在 `0x4D59C6` 同形。`[filter+0x6C]` 由这里写入 ⇒ 不 join 就是 guest 读 [0+0x18] fault。
constexpr std::uint32_t kFilterJoinFilterGraph = 0x34;
constexpr std::uint32_t kFilterGraphField = 0x6C; // m_pGraph（EA 见上）
// IPin vtable @`.data:0x52F57C`：+0x0C Connect(sub_4D2C70)、+0x10 ReceiveConnection(sub_4D0D00)、
// +0x24 QueryDirection(sub_4D1060)。
constexpr std::uint32_t kPinQueryInterface = 0x00;
constexpr std::uint32_t kPinRelease = 0x08;
constexpr std::uint32_t kPinReceiveConnection = 0x10;
// ★ QueryDirection 在 **+0x24**（任务书写 +0x0C = Connect 的槽）；见文件头与 dshow.h 的说明。
constexpr std::uint32_t kPinQueryDirection = 0x24;
// IEnumPins：+0x0C Next（`IEnumPins` = 3 个 IUnknown + Next/Reset/Skip/Clone）。
constexpr std::uint32_t kEnumNext = 0x0C;
constexpr std::uint32_t kEnumRelease = 0x08;
// IMemInputPin vtable @`.data:0x52F53C`：+0x10 NotifyAllocator(sub_4D1310)、+0x18 Receive(sub_4D4730)。
constexpr std::uint32_t kMemInputNotifyAllocator = 0x10;
constexpr std::uint32_t kMemInputReceive = 0x18;
// IMemInputPin* = 对象 +0x98（`0x4D44FB mov dword ptr [esi+98h], offset ??_7…@@6B@_2`），
// 而 IPin* = 对象 +0xC（`0x4D44ED mov [esi+0Ch], …@@6B@_0`）⇒ 从 IPin* 换算要减去 0xC 再加上 0x98。
constexpr std::uint32_t kMemInputFromPinDelta = 0x98 - 0x0C;
// IUnknown::AddRef / Release（任何 COM 接口的第 2/3 槽）。
constexpr std::uint32_t kUnknownAddRef = 0x04;
constexpr std::uint32_t kUnknownRelease = 0x08;
// ---- 源读取（真 IAsyncReader）----
// 每块读多大：8 MiB（任务书建议值）。分配失败时逐级退到 1 MiB / 64 KiB（guest 堆可能碎片化）。
constexpr std::uint32_t kSourceChunks[] = {8u << 20, 1u << 20, 64u << 10};
// `Length` 不可用时的保守上限（512 MiB）：读到 `SyncRead` 返回 S_FALSE/失败为止。
constexpr std::int64_t kSourceUnknownCap = 512ll << 20;
// ---- guest 侧出参暂存（一块 32 字节；避免每步都临时分配）----
enum : std::uint32_t {
    kScratchEnumPins = 0,   // IEnumPins*
    kScratchPin = 4,        // IPin*
    kScratchFetched = 8,    // ULONG
    kScratchDirection = 12, // PIN_DIRECTION
    kScratchQiOut = 16,     // void*
};
constexpr std::uint32_t kScratchBytes = 32;
// `AvgTimePerFrame` 的缺省值：`1e7/fps`，拿不到 fps（`FrameFeed::info` 不给）⇒ 1/30 s。
constexpr std::int64_t kDefaultAvgTimePerFrame100ns = 10000000 / 30; // 333333
constexpr double kTicksPerSecond = 1e7;
// 一次泵最多送几帧：时间寻址的源（`at()`）每次只给"该显示的那一帧" ⇒ 正常是 1；
// 上限 2 只是兜底（防止某个源在同一个 `now` 上连续报新帧导致一次泵无限循环）。
constexpr int kMaxSamplesPerPump = 2;
// ---- 链的步号 ----
enum Step : int {
    StepNone = 0,
    // AddFilter（AddRef + JoinFilterGraph）
    StepAddFilterAddRef,
    StepAddFilterJoin,
    // 源读取（真 IAsyncReader）
    StepSourceQueryInterface,
    StepSourceLength,
    StepSourceRead,
    StepSourceRelease,
    // Render（D1 接线）
    StepRenderEnumPins,
    StepRenderNextPin,
    StepRenderQueryDirection,
    StepRenderReleaseOutputPin,
    StepRenderReleaseEnum,
    StepRenderReleaseEnumEnd,
    StepRenderReceiveConnection,
    StepRenderQiMemInput,
    StepRenderNotifyAllocator,
    // 控制
    StepRunClockRenderer,
    StepRunClockSource,
    StepRunPauseRenderer,
    StepRunPauseSource,
    StepRunSource,
    StepRunRenderer,
    StepPausePauseRenderer,
    StepPausePauseSource,
    StepStopRenderer,
    StepStopSource,
    // 泵
    StepPumpReceive,
};
void writeGuidAt(X86 &cpu, std::uint32_t ea, const Guid &g) {
    cpu.mem->wr32(ea, g.data1);
    cpu.mem->wr16(ea + 4u, g.data2);
    cpu.mem->wr16(ea + 6u, g.data3);
    for (std::uint32_t i = 0; i < 8u; ++i)
        cpu.mem->wr8(ea + 8u + i, g.data4[i]);
}
bool equalsIgnoreCase(const std::string &a, const char *b) {
    const std::size_t n = std::strlen(b);
    if (a.size() != n)
        return false;
    for (std::size_t i = 0; i < n; ++i) {
        char ca = a[i];
        char cb = b[i];
        if (ca >= 'A' && ca <= 'Z')
            ca = static_cast<char>(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z')
            cb = static_cast<char>(cb - 'A' + 'a');
        if (ca != cb)
            return false;
    }
    return true;
}
} // namespace
// LPCWSTR（guest EA）→ 窄串（只用于识别/日志；文档里出现的名字都是 ASCII）。
std::string Graph::readName(std::uint32_t name_ea) const {
    std::string out;
    if (name_ea == 0 || cpu_.mem == nullptr)
        return out;
    for (std::uint32_t i = 0; i < 256u; ++i) {
        const std::uint32_t ea = name_ea + i * 2u;
        if (!cpu_.mem->committed(ea, 2u))
            break;
        const std::uint16_t ch = cpu_.mem->rd16(ea);
        if (ch == 0)
            break;
        out.push_back(ch >= 0x20 && ch <= 0x7E ? static_cast<char>(ch) : '?');
    }
    return out;
}
// =====================================================================================
// 日志（分类 LogVideo）
// =====================================================================================
void dshowLog(LogLevel level, const char *fmt, ...) {
    char prefix[64];
    std::snprintf(prefix, sizeof(prefix), "[dshow] ");
    va_list ap;
    va_start(ap, fmt);
    logV(level, LogVideo, prefix, fmt, ap);
    va_end(ap);
}
// =====================================================================================
// 生命周期
// =====================================================================================
Graph::Graph(X86 &cpu, Now100ns now, FrameFeed *feed) : cpu_(cpu), now_(std::move(now)), feed_(feed) {
    if (!now_)
        now_ = [] { return std::int64_t{0}; };
    feed_injected_ = feed != nullptr;
    dshowLog(LogLevel::Info, "Graph 建（now100ns=%s；帧源=%s）", now_ ? "cpu.clock 换算" : "无",
             feed_injected_ ? "注入（pure 测试，不碰源 pin）" : "真 IAsyncReader");
}
Graph::~Graph() {
    if (suspended_) {
        // ⛔ 不是"清理一下就好"：链的续延捕获了 this，销毁到一半会被 resume 用到。
        dshowLog(LogLevel::Error, "Graph 析构时还有 guest 调用在飞（suspended）—— 调用方必须先收尾");
    }
    releaseAll();
}
void Graph::releaseAll() {
    releaseSource();
    owned_feed_.reset();
    if (objs_) {
        objs_->destroy(cpu_);
        objs_.reset();
    }
    if (media_type_ != 0) {
        MediaTypeBlock mt{media_type_, video_info_, 0};
        freeMediaType(cpu_, &mt);
        media_type_ = 0;
        video_info_ = 0;
    }
    if (cpu_.heap != nullptr) {
        if (scratch_ != 0) {
            cpu_.heap->free(scratch_);
            scratch_ = 0;
        }
        if (iid_mem_input_ != 0) {
            cpu_.heap->free(iid_mem_input_);
            iid_mem_input_ = 0;
        }
        if (iid_async_reader_ != 0) {
            cpu_.heap->free(iid_async_reader_);
            iid_async_reader_ = 0;
        }
    }
}
bool Graph::ensureObjects(std::string *err) {
    if (!objs_)
        objs_ = std::make_unique<HostObjects>();
    if (!objs_->create(cpu_, err))
        return false;
    if (scratch_ == 0) {
        if (cpu_.heap == nullptr || !cpu_.heap->ready()) {
            if (err)
                *err = "ensureObjects: guest 堆没就绪";
            return false;
        }
        scratch_ = cpu_.heap->alloc(kScratchBytes, /*zero=*/true);
        iid_mem_input_ = cpu_.heap->alloc(16u, /*zero=*/true);
        iid_async_reader_ = cpu_.heap->alloc(16u, /*zero=*/true);
        if (scratch_ == 0 || iid_mem_input_ == 0 || iid_async_reader_ == 0) {
            if (err)
                *err = std::string("ensureObjects: 分配暂存失败：") + cpu_.heap->lastError();
            return false;
        }
        writeGuidAt(cpu_, iid_mem_input_, kIidMemInputPin);
        writeGuidAt(cpu_, iid_async_reader_, kIidAsyncReader);
    }
    return true;
}
// =====================================================================================
// filter 表
// =====================================================================================
bool Graph::addFilter(std::uint32_t filter_obj, std::uint32_t name_ea, std::string *err) {
    if (filter_obj == 0) {
        if (err)
            *err = "AddFilter: pFilter 为 NULL";
        return false;
    }
    if (suspended_) {
        if (err)
            *err = "AddFilter: 上一次 guest 调用还没结束";
        return false;
    }
    if (cpu_.mem == nullptr || !cpu_.mem->committed(filter_obj, 4u)) {
        if (err)
            *err = "AddFilter: pFilter 的 vtable 指针读不到（未提交）";
        dshowLog(LogLevel::Warn, "AddFilter(0x%08X): 对象头未提交 ⇒ 拒绝", filter_obj);
        return false;
    }
    const std::string n = readName(name_ea);
    filters_.push_back(Filter{filter_obj, n});
    const bool named_renderer = equalsIgnoreCase(n, "TextureRenderer");
    // ① 名字不可用时的兜底：按加入顺序（doc §1-5 先加渲染器、§1-8 后加源）。
    if (renderer_ == 0)
        renderer_ = filters_[0].ea;
    if (source_ == 0 && filters_.size() >= 2)
        source_ = filters_[1].ea;
    // ② 名字优先：`L"TextureRenderer"` 就是渲染器（覆盖 ① 的猜测）。
    if (named_renderer) {
        if (source_ == filter_obj)
            source_ = 0;
        renderer_ = filter_obj;
    }
    // ③ 名字为空的那个是源（覆盖 ① 的猜测）。
    if (n.empty() && filter_obj != renderer_)
        source_ = filter_obj;
    if (source_ == renderer_) {
        source_ = 0;
        dshowLog(LogLevel::Warn, "AddFilter: 源与渲染器识别成了同一个对象（0x%08X）⇒ 清空源", renderer_);
    }
    // ★ 第一件必须解决的事：`CBaseFilter::Pause/Run` 会 `mov eax,[filter+0x6C]; cmp dword ptr [eax+18h],0`
    //   （`0x4D5896` / `0x4D59C6`）。`[filter+0x6C]` = m_pGraph，只有 `JoinFilterGraph`（+0x34）会写它。
    //   引擎自己 AddFilter 之后**不会**替我们 join ⇒ 为 0 时必须由宿主补（否则 guest 读 [0+0x18] fault）。
    const std::uint32_t graph_field = cpu_.mem->committed(filter_obj + kFilterGraphField, 4u)
                                          ? cpu_.mem->rd32(filter_obj + kFilterGraphField)
                                          : 0u;
    dshowLog(LogLevel::Info,
             "AddFilter(pFilter=0x%08X, name=\"%s\") ⇒ 共 %zu 个 filter；渲染器=0x%08X 源=0x%08X；"
             "[filter+0x6C]=0x%08X",
             filter_obj, n.empty() ? "(NULL)" : n.c_str(), filters_.size(), renderer_, source_, graph_field);
    if (graph_field == 0) {
        if (graph_unknown_ == 0) {
            dshowLog(LogLevel::Warn,
                     "AddFilter(0x%08X): [filter+0x6C]==0 且没有图对象 EA ⇒ 无法 JoinFilterGraph；"
                     "guest 的 Pause/Run 会读 [0+0x18] fault",
                     filter_obj);
            return true;
        }
        dshowLog(LogLevel::Info,
                 "AddFilter(0x%08X): [filter+0x6C]==0 ⇒ JoinFilterGraph(filter, graph=0x%08X, name=%s)",
                 filter_obj, graph_unknown_, name_ea ? "LPCWSTR" : "NULL");
        addfilter_join_ea_ = filter_obj;
        addfilter_join_name_ = name_ea;
        // ★ 真图在 AddFilter 里会持有 filter 的引用：引擎在 Render 之后立刻 `Release(filter)`（`0x489647`），
        //   不 AddRef 的话源 filter 会被销毁 —— 后面 `Pause(源)` / `Run(源)` 的 vtable 槽就是 0
        //   （实测：`this=0x103271D4 vtbl+0x14 读不到实现`）。⇒ 先 AddRef（IBaseFilter +0x04），再 join。
        step_ = StepAddFilterJoin;
        return startChain(StepAddFilterAddRef, "IBaseFilter::AddRef");
    }
    dshowLog(LogLevel::Info, "AddFilter(0x%08X): [filter+0x6C]=0x%08X 已非 0 ⇒ 不重复 JoinFilterGraph",
             filter_obj, graph_field);
    return true;
}
// =====================================================================================
// Render（D1）
// =====================================================================================
bool Graph::render(std::uint32_t source_pin, std::string *err) {
    if (suspended_) {
        if (err)
            *err = "Render: 上一次 guest 调用还没结束";
        return false;
    }
    if (renderer_ == 0) {
        if (err)
            *err = "Render: 还没有渲染器 filter（AddFilter 先调）";
        dshowLog(LogLevel::Warn, "Render: 渲染器 filter 为 0 ⇒ 无法接线（引擎不会发现，只有这条日志）");
        return false;
    }
    if (!ensureObjects(err))
        return false;
    source_pin_ = source_pin;
    // ★ 帧源策略：
    //   · 注入（pure 测试）⇒ **完全不碰源 pin**（旧用例的 D1 判据，doc §2-D）。
    //   · 否则 ⇒ 先 QI 引擎的源 pin 要 IAsyncReader，再从它读字节（宿主不解析 ALF / 不猜文件名）。
    if (feed_injected_ && feed_ != nullptr) {
        if (!prepareWiring(err))
            return false;
        return startChain(StepRenderEnumPins, "IGraphBuilder::Render");
    }
    if (source_pin == 0) {
        if (err)
            *err = "Render: source_pin 为 NULL ⇒ 没有字节来路";
        dshowLog(LogLevel::Warn, "Render: source_pin 为 NULL ⇒ 无帧（引擎不看 Render 的 HRESULT）");
        return false;
    }
    stats_.source_pin = source_pin;
    return startChain(StepSourceQueryInterface, "IGraphBuilder::Render(源读取)");
}
// 源读完之后（或注入 feed 时）：组 media type / 分配像素缓冲 / 填 stats —— **不**发起 guest 调用。
bool Graph::prepareWiring(std::string *err) {
    std::uint32_t w = 0;
    std::uint32_t h = 0;
    std::int64_t dur = 0;
    if (feed_ == nullptr || !feed_->info(&w, &h, &dur) || w == 0 || h == 0) {
        if (err)
            *err = "Render: 帧源 info() 失败或宽高为 0 ⇒ 组不出 media type";
        stats_.connect_hr = kEFail;
        dshowLog(LogLevel::Error, "Render: 帧源 info() 失败/宽高为 0 ⇒ 接线失败（引擎不会报）");
        return false;
    }
    opened_ = true;
    duration_100ns_ = dur < 0 ? 0 : dur;
    duration_sec_ = static_cast<double>(duration_100ns_) / kTicksPerSecond;
    width_ = w;
    height_ = h;
    stride_ = strideForRgb24(w);
    if (media_type_ != 0) {
        MediaTypeBlock old{media_type_, video_info_, 0};
        freeMediaType(cpu_, &old);
        media_type_ = 0;
        video_info_ = 0;
    }
    MediaTypeBlock mt;
    if (!buildVideoMediaType(cpu_, w, h, kDefaultAvgTimePerFrame100ns, &mt, err))
        return false;
    media_type_ = mt.media_type_ea;
    video_info_ = mt.video_info_ea;
    if (!objs_->prepareBuffer(cpu_, stride_ * h, err))
        return false;
    objs_->setSampleMediaType(cpu_, media_type_);
    stats_.pin_count = 0;
    stats_.connect_hr = kEFail; // 没走到 ReceiveConnection 之前的缺省（不是"成功"）
    stats_.notify_allocator_hr = kEFail;
    stats_.media_type_ea = media_type_;
    stats_.allocator_ea = objs_->allocator();
    stats_.sample_ea = objs_->sample();
    dshowLog(LogLevel::Info,
             "Render: media type %ux%u RGB24 stride=%u AvgTimePerFrame=%lld 时长=%.3fs；"
             "分配器=0x%08X 样本=0x%08X 输出 pin=0x%08X（源 pin=0x%08X）",
             w, h, stride_, static_cast<long long>(kDefaultAvgTimePerFrame100ns),
             static_cast<double>(duration_100ns_) / kTicksPerSecond, objs_->allocator(), objs_->sample(),
             objs_->outputPin(), source_pin_);
    return true;
}
// =====================================================================================
// 源读取（真 IAsyncReader：QI → Length → 分块 SyncRead）
// =====================================================================================
bool Graph::startSourceRead(std::string *err) {
    // 决定块大小 / 是否结束。
    const std::int64_t end = source_total_ > 0 ? source_total_ : source_cap_;
    if (source_pos_ >= end)
        return true; // 调用方据此进入 Release
    std::int64_t remain = end - source_pos_;
    std::uint32_t chunk = read_buf_bytes_;
    if (remain < static_cast<std::int64_t>(chunk))
        chunk = static_cast<std::uint32_t>(remain);
    if (source_total_ <= 0 && chunk == 0)
        return true;
    if (read_buf_ == 0 || read_buf_bytes_ == 0) {
        if (err)
            *err = "源读取：没有读缓冲";
        return false;
    }
    read_chunk_ = chunk;
    step_ = StepSourceRead;
    return stepLaunch(err);
}
// =====================================================================================
// 链的驱动
// =====================================================================================
bool Graph::startChain(int first_step, const char *api) {
    if (suspended_) {
        dshowLog(LogLevel::Warn, "%s: 上一次 guest 调用还没结束 ⇒ 拒绝重入", api ? api : "?");
        return false;
    }
    step_ = first_step;
    suspended_ = true;
    std::string err;
    if (!launch(step_, &err)) {
        suspended_ = false;
        step_ = StepNone;
        dshowLog(LogLevel::Error, "%s: 发起 guest 调用失败：%s", api ? api : "?", err.c_str());
        return false;
    }
    return true;
}
bool Graph::finishChain() {
    suspended_ = false;
    step_ = StepNone;
    return true;
}
bool Graph::stepLaunch(std::string *err) {
    if (!launch(step_, err)) {
        dshowLog(LogLevel::Error, "链上这一步（step=%d）发不出去：%s ⇒ 收尾", step_,
                 err ? err->c_str() : "?");
        return finishChain();
    }
    return false;
}
bool Graph::launch(int step, std::string *err) {
    GuestContinuation cont = [this](X86 &, std::uint32_t callback_result,
                                    std::uint32_t *shim_result) -> bool {
        return continueChain(callback_result, shim_result);
    };
    const std::uint32_t cleanup = cleanup_bytes_;
    switch (static_cast<Step>(step)) {
    case StepAddFilterAddRef:
        // IBaseFilter::AddRef（IUnknown +0x04，`CBasePin`/`CBaseFilter` 的 NonDelegatingAddRef）。
        return invokeGuest(cpu_, addfilter_join_ea_, kUnknownAddRef, {addfilter_join_ea_}, cleanup,
                           "dshow:filter::AddRef", nullptr, cont, err);
    case StepAddFilterJoin:
        // IBaseFilter::JoinFilterGraph(IFilterGraph* pGraph, LPCWSTR pName)（+0x34 = `sub_4D02B0`，
        // `retn 0Ch` ⇒ this + 2 参）。★ 名字用**引擎自己传进来的那个 guest LPCWSTR**（源为 NULL）。
        return invokeGuest(cpu_, addfilter_join_ea_, kFilterJoinFilterGraph,
                           {addfilter_join_ea_, graph_unknown_, addfilter_join_name_}, cleanup,
                           "dshow:filter::JoinFilterGraph", nullptr, cont, err);
    case StepSourceQueryInterface:
        // IPin::QueryInterface(IID_IAsyncReader, void**)（IPin vtable +0x00）。
        // ★ IID = `kIidAsyncReader`（56A868AA，见 dshow.h 的 EA 证据）；返回 IAsyncReader*（对象基址）。
        return invokeGuest(cpu_, source_pin_, kPinQueryInterface,
                           {source_pin_, iid_async_reader_, scratch_ + kScratchQiOut}, cleanup,
                           "dshow:srcpin::QueryInterface(IAsyncReader)", nullptr, cont, err);
    case StepSourceLength:
        // IAsyncReader::Length(LONGLONG* pTotal, LONGLONG* pAvailable)（**本引擎 +0x20**，见 dshow.h）。
        return invokeGuest(cpu_, iasync_, kAsyncVtblLength, {iasync_, scratch_, scratch_ + 8u}, cleanup,
                           "dshow:IAsyncReader::Length", nullptr, cont, err);
    case StepSourceRead:
        // IAsyncReader::SyncRead(LONGLONG llPosition, LONG lLength, BYTE* pBuffer)（**本引擎 +0x1C**）。
        // ★ 栈序 = (this, llPosition_lo, llPosition_hi, lLength, pBuffer)（`sub_403090` → `sub_401EF0`）。
        return invokeGuest(cpu_, iasync_, kAsyncVtblSyncRead,
                           {iasync_,
                            static_cast<std::uint32_t>(static_cast<std::uint64_t>(source_pos_) & 0xFFFFFFFFu),
                            static_cast<std::uint32_t>(static_cast<std::uint64_t>(source_pos_) >> 32),
                            read_chunk_, read_buf_},
                           cleanup, "dshow:IAsyncReader::SyncRead", nullptr, cont, err);
    case StepSourceRelease:
        return invokeGuest(cpu_, iasync_, kUnknownRelease, {iasync_}, cleanup, "dshow:IAsyncReader::Release",
                           nullptr, cont, err);
    case StepRenderEnumPins:
        // IBaseFilter::EnumPins(IEnumPins**) ⇒ retn 8（`sub_4D30A0 int __stdcall(int, _DWORD*)`）
        return invokeGuest(cpu_, renderer_, kFilterEnumPins, {renderer_, scratch_ + kScratchEnumPins},
                           cleanup, "dshow:filter::EnumPins", nullptr, cont, err);
    case StepRenderNextPin:
        // IEnumPins::Next(cPins, ppPins, pcFetched)（+0x0C）—— 一次一个。
        return invokeGuest(cpu_, renderer_enum_, kEnumNext,
                           {renderer_enum_, 1u, scratch_ + kScratchPin, scratch_ + kScratchFetched}, cleanup,
                           "dshow:EnumPins::Next", nullptr, cont, err);
    case StepRenderQueryDirection:
        // IPin::QueryDirection(PIN_DIRECTION*)（+0x24；`sub_4D1060 int __stdcall(int, _DWORD*)`）
        return invokeGuest(cpu_, candidate_pin_, kPinQueryDirection,
                           {candidate_pin_, scratch_ + kScratchDirection}, cleanup,
                           "dshow:pin::QueryDirection", nullptr, cont, err);
    case StepRenderReleaseOutputPin:
        // IUnknown::Release（+0x08）：跳过一个输出 pin 时把引用还掉。
        return invokeGuest(cpu_, candidate_pin_, kPinRelease, {candidate_pin_}, cleanup, "dshow:pin::Release",
                           nullptr, cont, err);
    case StepRenderReleaseEnum:
    case StepRenderReleaseEnumEnd:
        return invokeGuest(cpu_, renderer_enum_, kEnumRelease, {renderer_enum_}, cleanup,
                           "dshow:EnumPins::Release", nullptr, cont, err);
    case StepRenderReceiveConnection:
        // IPin::ReceiveConnection(IPin* pConnector, const AM_MEDIA_TYPE* pmt)（+0x10，retn 0Ch）
        // ★ arg 顺序 = (this, pConnector, pmt)：我们的输出 pin + 宿主组的 media type。
        return invokeGuest(cpu_, input_pin_, kPinReceiveConnection,
                           {input_pin_, objs_->outputPin(), media_type_}, cleanup,
                           "dshow:pin::ReceiveConnection", nullptr, cont, err);
    case StepRenderQiMemInput:
        // IPin::QueryInterface(IID_IMemInputPin, void**)（+0x00）。
        return invokeGuest(cpu_, input_pin_, kPinQueryInterface,
                           {input_pin_, iid_mem_input_, scratch_ + kScratchQiOut}, cleanup,
                           "dshow:pin::QueryInterface(IMemInputPin)", nullptr, cont, err);
    case StepRenderNotifyAllocator:
        // IMemInputPin::NotifyAllocator(IMemAllocator*, BOOL)（+0x10，retn 0Ch）
        // ★ fReadOnly = FALSE：引擎的 `sub_4D1310` 只记 bReadOnly、不校验类型（doc §6-6）。
        return invokeGuest(cpu_, mem_input_, kMemInputNotifyAllocator, {mem_input_, objs_->allocator(), 0u},
                           cleanup, "dshow:NotifyAllocator", nullptr, cont, err);
    case StepRunClockRenderer:
    case StepRunClockSource: {
        // IBaseFilter::SetSyncSource(IReferenceClock*)（+0x20，`sub_4CFD60`，retn 8 = this + 1 参）。
        const std::uint32_t filter = step == StepRunClockRenderer ? renderer_ : source_;
        return invokeGuest(cpu_, filter, kFilterSetSyncSource, {filter, objs_->clock()}, cleanup,
                           "dshow:filter::SetSyncSource", nullptr, cont, err);
    }
    case StepRunPauseRenderer:
    case StepRunPauseSource:
    case StepPausePauseRenderer:
    case StepPausePauseSource: {
        // IBaseFilter::Pause()（+0x14）—— ★ **没有** tStart（`sub_4D5860 int __stdcall(int)` + `retn 4`）。
        const bool is_renderer = (step == StepRunPauseRenderer || step == StepPausePauseRenderer);
        const std::uint32_t filter = is_renderer ? renderer_ : source_;
        return invokeGuest(cpu_, filter, kFilterPause, {filter}, cleanup, "dshow:filter::Pause", nullptr,
                           cont, err);
    }
    case StepRunSource:
    case StepRunRenderer: {
        // IBaseFilter::Run(REFERENCE_TIME tStart)（+0x18）⇒ retn 0Ch = this + 8 字节。
        // 栈上是 (this, tStart_lo, tStart_hi)。
        const std::uint32_t filter = (step == StepRunRenderer) ? renderer_ : source_;
        const std::int64_t t = now_();
        const std::uint32_t lo = static_cast<std::uint32_t>(static_cast<std::uint64_t>(t) & 0xFFFFFFFFu);
        const std::uint32_t hi = static_cast<std::uint32_t>(static_cast<std::uint64_t>(t) >> 32);
        return invokeGuest(cpu_, filter, kFilterRun, {filter, lo, hi}, cleanup, "dshow:filter::Run",
                           nullptr, cont, err);
    }
    case StepStopRenderer:
    case StepStopSource: {
        // IBaseFilter::Stop()（+0x10，`sub_4D5780 int __stdcall(int)` ⇒ retn 4）
        const std::uint32_t filter = step == StepStopRenderer ? renderer_ : source_;
        return invokeGuest(cpu_, filter, kFilterStop, {filter}, cleanup, "dshow:filter::Stop", nullptr, cont,
                           err);
    }
    case StepPumpReceive:
        // IMemInputPin::Receive(IMediaSample*)（+0x18，`sub_4D4730 int __stdcall(int,int)` ⇒ retn 8）
        return invokeGuest(cpu_, mem_input_, kMemInputReceive, {mem_input_, objs_->sample()}, cleanup,
                           "dshow:IMemInputPin::Receive", nullptr, cont, err);
    case StepNone:
        break;
    }
    if (err)
        *err = "非法步号 " + std::to_string(step);
    return false;
}

bool Graph::continueChain(std::uint32_t result, std::uint32_t *shim_result) {
    *shim_result = kSOk; // 给调用点的 EAX（引擎不看 Render 的返回值；控制方法要 S_OK）
    std::string err;
    switch (static_cast<Step>(step_)) {
    // ---------------------------- AddFilter（AddRef + JoinFilterGraph）----------------------------
    case StepAddFilterAddRef: {
        dshowLog(result != 0 ? LogLevel::Debug : LogLevel::Warn, "AddRef(filter=0x%08X) = %u（真图持有引用）",
                 addfilter_join_ea_, result);
        step_ = StepAddFilterJoin;
        return stepLaunch(&err);
    }

    case StepAddFilterJoin: {

        const std::uint32_t field = cpu_.mem->committed(addfilter_join_ea_ + kFilterGraphField, 4u)
                                        ? cpu_.mem->rd32(addfilter_join_ea_ + kFilterGraphField)
                                        : 0u;

        dshowLog(result == kSOk ? LogLevel::Info : LogLevel::Warn,
                 "JoinFilterGraph(filter=0x%08X, graph=0x%08X) = 0x%08X ⇒ [filter+0x6C]=0x%08X",

                 addfilter_join_ea_, graph_unknown_, result, field);

        if (field == 0) {

            dshowLog(LogLevel::Error,
                     "JoinFilterGraph 返回 0x%08X 但 [filter+0x6C] 仍是 0 ⇒ guest 的 Pause/Run 会读 [0+0x18] "
                     "fault",
                     result);
        }

        addfilter_join_ea_ = 0;

        addfilter_join_name_ = 0;

        return finishChain();
    }

        // ---------------------------- 源读取（真 IAsyncReader）----------------------------

    case StepSourceQueryInterface: {

        stats_.source_qi_hr = result;

        std::uint32_t ia = 0;

        if (result == kSOk && cpu_.mem->committed(scratch_ + kScratchQiOut, 4u))
            ia = cpu_.mem->rd32(scratch_ + kScratchQiOut);

        if (ia == 0) {

            // ★ 失败不静默：保持"零长"行为（不崩、boot 继续），但**无帧**。

            dshowLog(LogLevel::Warn,
                     "源 pin QI(IAsyncReader{56A868AA-0AD4-11CE-B03A-0020AF0BA770}) = 0x%08X 出参=0x%08X"
                     " ⇒ 取不到源字节，无帧（引擎不看 Render 的 HRESULT）",
                     result, ia);

            return finishChain();
        }

        iasync_ = ia;

        stats_.source_iasync_ea = ia;

        stats_.source_vtable_ea = cpu_.mem->committed(ia, 4u) ? cpu_.mem->rd32(ia) : 0u;

        dshowLog(LogLevel::Info,
                 "srcpin=0x%08X iasync=0x%08X vtable=0x%08X（期望 0x51D95C = CAsyncOutputPin）",

                 source_pin_, iasync_, stats_.source_vtable_ea);

        step_ = StepSourceLength;

        return stepLaunch(&err);
    }

    case StepSourceLength: {

        stats_.source_length_hr = result;

        std::int64_t total = 0;

        std::int64_t avail = 0;

        if (result == kSOk) {
            total = static_cast<std::int64_t>(cpu_.mem->rd64(scratch_));
            avail = static_cast<std::int64_t>(cpu_.mem->rd64(scratch_ + 8u));

            stats_.source_total = total;

            stats_.source_available = avail;

            dshowLog(LogLevel::Info, "IAsyncReader::Length ⇒ total=%lld available=%lld", (long long)total,
                     (long long)avail);

        } else {

            dshowLog(LogLevel::Warn,
                     "IAsyncReader::Length = 0x%08X ⇒ 总长未知，按保守上限 %lld 字节读到 S_FALSE/失败为止",
                     result, (long long)kSourceUnknownCap);
        }

        source_total_ = total > 0 ? total : -1;

        source_cap_ = total > 0 ? 0 : kSourceUnknownCap;

        source_pos_ = 0;

        source_bytes_.clear();

        // 分块缓冲（guest 堆）：先按 Length 的剩余量，失败逐级退。

        read_buf_bytes_ = 0;

        for (const std::uint32_t want : kSourceChunks) {

            std::uint32_t bytes = want;

            if (source_total_ > 0 && static_cast<std::uint64_t>(source_total_) < bytes)
                bytes = static_cast<std::uint32_t>(source_total_);

            if (bytes == 0)
                continue;

            read_buf_ = cpu_.heap->alloc(bytes, /*zero=*/false);

            if (read_buf_ != 0) {

                read_buf_bytes_ = bytes;
                break;
            }
        }
        if (read_buf_ == 0) {
            dshowLog(LogLevel::Warn, "源读取：guest 堆分配读缓冲失败（%s）⇒ 无帧", cpu_.heap->lastError());
            step_ = StepSourceRelease;
            return stepLaunch(&err);
        }
        stats_.source_read_buf_bytes = read_buf_bytes_;
        if (source_total_ > 0 && source_total_ < static_cast<std::int64_t>(1) << 31)
            source_bytes_.reserve(static_cast<std::size_t>(source_total_));
        dshowLog(LogLevel::Info, "源读取：块大小 = %u 字节（total=%lld）", read_buf_bytes_,
                 (long long)source_total_);
        if (!startSourceRead(&err))
            return false;
        step_ = StepSourceRelease;
        return stepLaunch(&err);
    }
    case StepSourceRead: {
        bool done = false;
        std::uint32_t got = 0;
        if (result == kSOk) {
            got = read_chunk_;
        } else if (result == kSFalse) {

            // S_FALSE = 到流尾/不满一块；有总长时按请求量算（IAsyncReader::SyncRead 没有出参长度）。

            if (source_total_ > 0 && source_pos_ + read_chunk_ <= source_total_)
                got = read_chunk_;

            dshowLog(LogLevel::Info, "IAsyncReader::SyncRead = S_FALSE（流尾）⇒ 收尾（已读 %llu 字节）",
                     (unsigned long long)stats_.source_bytes_read);
            done = true;

        } else {

            dshowLog(LogLevel::Warn,
                     "IAsyncReader::SyncRead(0x%08X) = 0x%08X ⇒ 停止读取（已读 %llu 字节；无帧或短影片）",
                     static_cast<std::uint32_t>(source_pos_), result,
                     (unsigned long long)stats_.source_bytes_read);
            done = true;
        }

        if (got != 0) {

            const std::size_t old = source_bytes_.size();

            source_bytes_.resize(old + got);

            cpu_.mem->readBytes(read_buf_, source_bytes_.data() + old, got);

            if (old == 0 && got >= 4)
                stats_.source_first4 = cpu_.mem->rd32(read_buf_);

            source_pos_ += got;
            ++stats_.source_chunks;

            stats_.source_bytes_read += got;

            dshowLog(LogLevel::Info, "SyncRead 块 #%u：pos=%lld 读到 %u 字节（累计 %llu）",
                     stats_.source_chunks, (long long)(source_pos_ - got), got,
                     (unsigned long long)stats_.source_bytes_read);
        }

        if (done) {

            step_ = StepSourceRelease;

            return stepLaunch(&err);
        }

        if (!startSourceRead(&err))
            return false;

        step_ = StepSourceRelease;

        return stepLaunch(&err);
    }

    case StepSourceRelease: {

        dshowLog(LogLevel::Debug, "Release(IAsyncReader 0x%08X) = 0x%08X", iasync_, result);

        iasync_ = 0;

        if (read_buf_ != 0 && cpu_.heap != nullptr) {

            cpu_.heap->free(read_buf_);

            read_buf_ = 0;
        }

        read_buf_bytes_ = 0;

        dshowLog(LogLevel::Info, "源读取完成：%llu 字节 / %u 块；first4=0x%08X（MPEG-PS 期望 0xBA010000）",
                 (unsigned long long)stats_.source_bytes_read, stats_.source_chunks, stats_.source_first4);

        if (stats_.source_bytes_read == 0) {

            dshowLog(LogLevel::Warn, "源读取到 0 字节 ⇒ 无帧");

            stats_.connect_hr = kEFail; // 没接线（不是"成功"）
            return finishChain();
        }
        std::string ferr;
        owned_feed_ = makeDecoderFeed(std::move(source_bytes_), &ferr);
        if (!owned_feed_) {
            dshowLog(LogLevel::Warn, "MovieDecoder::open 失败：%s ⇒ 无帧（引擎不看 Render 的 HRESULT）",
                     ferr.c_str());
            stats_.connect_hr = kEFail; // 没接线
            return finishChain();
        }
        feed_ = owned_feed_.get();
        stats_.decoder_opened = true;
        std::uint32_t w = 0, h = 0;
        std::int64_t dur = 0;
        if (feed_->info(&w, &h, &dur)) {
            stats_.decoder_width = w;
            stats_.decoder_height = h;
            dshowLog(LogLevel::Info, "解码器 open：%ux%u 时长=%.3fs", w, h,
                     static_cast<double>(dur) / kTicksPerSecond);
        }
        if (!prepareWiring(&err))
            return finishChain();
        step_ = StepRenderEnumPins;
        return stepLaunch(&err);
    }

        // ---------------------------- Render（D1）----------------------------

    case StepRenderEnumPins: {

        if (result != kSOk) {

            dshowLog(LogLevel::Warn, "EnumPins 返回 0x%08X ⇒ 接线失败", result);

            stats_.connect_hr = result;

            return finishChain();
        }

        renderer_enum_ = cpu_.mem->rd32(scratch_ + kScratchEnumPins);

        if (renderer_enum_ == 0) {

            dshowLog(LogLevel::Warn, "EnumPins 成功但出参为 NULL ⇒ 接线失败");

            stats_.connect_hr = kEFail;

            return finishChain();
        }

        step_ = StepRenderNextPin;

        return stepLaunch(&err);
    }

    case StepRenderNextPin: {

        if (result != kSOk) {

            dshowLog(LogLevel::Warn, "IEnumPins::Next 返回 0x%08X ⇒ 接线失败", result);

            stats_.connect_hr = result;

            step_ = StepRenderReleaseEnumEnd;

            return stepLaunch(&err);
        }

        const std::uint32_t fetched = cpu_.mem->rd32(scratch_ + kScratchFetched);

        candidate_pin_ = cpu_.mem->rd32(scratch_ + kScratchPin);

        if (fetched == 0 || candidate_pin_ == 0) {

            dshowLog(LogLevel::Warn, "渲染器枚举完 %u 个 pin 都没找到输入 pin ⇒ 接线失败", stats_.pin_count);

            stats_.connect_hr = kVfwETypeNotAccepted;

            step_ = StepRenderReleaseEnumEnd;

            return stepLaunch(&err);
        }
        ++stats_.pin_count;

        step_ = StepRenderQueryDirection;

        return stepLaunch(&err);
    }

    case StepRenderQueryDirection: {

        if (result != kSOk) {

            dshowLog(LogLevel::Debug, "QueryDirection(pin=0x%08X) 返回 0x%08X ⇒ 跳过这个 pin", candidate_pin_,
                     result);

            step_ = StepRenderReleaseOutputPin;

            return stepLaunch(&err);
        }

        const std::uint32_t dir = cpu_.mem->rd32(scratch_ + kScratchDirection);

        if (dir != 0) { // PINDIR_OUTPUT

            dshowLog(LogLevel::Debug, "pin 0x%08X 是输出 pin（PINDIR_OUTPUT=%u）⇒ 跳过", candidate_pin_, dir);

            step_ = StepRenderReleaseOutputPin;

            return stepLaunch(&err);
        }

        input_pin_ = candidate_pin_;

        stats_.input_pin_ea = input_pin_;

        dshowLog(LogLevel::Info, "渲染器的输入 pin = 0x%08X（第 %u 个 pin）", input_pin_, stats_.pin_count);

        step_ = StepRenderReleaseEnum;

        return stepLaunch(&err);
    }

    case StepRenderReleaseOutputPin: {

        dshowLog(LogLevel::Debug, "Release(输出 pin 0x%08X) = 0x%08X", candidate_pin_, result);

        candidate_pin_ = 0;

        step_ = StepRenderNextPin;

        return stepLaunch(&err);
    }

    case StepRenderReleaseEnum: {

        dshowLog(LogLevel::Debug, "Release(枚举器 0x%08X) = 0x%08X", renderer_enum_, result);

        renderer_enum_ = 0;

        step_ = StepRenderReceiveConnection;

        return stepLaunch(&err);
    }

    case StepRenderReleaseEnumEnd: {

        dshowLog(LogLevel::Debug, "Release(枚举器 0x%08X) = 0x%08X（接线失败后收尾）", renderer_enum_,
                 result);

        renderer_enum_ = 0;

        return finishChain();
    }

    case StepRenderReceiveConnection: {

        stats_.connect_hr = result;
        if (result == kSOk) {
            dshowLog(LogLevel::Info,
                     "ReceiveConnection(渲染器输入 pin 0x%08X, 我的输出 pin 0x%08X, mt=0x%08X) = S_OK",
                     input_pin_, objs_->outputPin(), media_type_);
        } else {
            // ★ 引擎不看 Render 的返回值（`0x489647` 直接 Release）⇒ 接线失败只能靠这条日志 + stats。
            // ★ 诊断：`CBasePin::ReceiveConnection`（`sub_4D0D00`）在 `CheckMediaType` 非 0 时把它映射成
            //   `VFW_E_TYPE_NOT_ACCEPTED(0x8004022A)`（`0x8004022A` 也出现在渲染器的纹理格式分支，
            //   两处同名不同因 —— 把 pin 的 C++ 槽打出来才能区分）。
            const std::uint32_t pin_vt =
                cpu_.mem->committed(input_pin_, 4u) ? cpu_.mem->rd32(input_pin_) : 0u;
            const std::uint32_t cxx_vt = (input_pin_ >= 0x0Cu && cpu_.mem->committed(input_pin_ - 0x0Cu, 4u))
                                             ? cpu_.mem->rd32(input_pin_ - 0x0Cu)
                                             : 0u;
            auto cxxSlot = [&](std::uint32_t off) -> std::uint32_t {
                return (cxx_vt != 0 && cpu_.mem->committed(cxx_vt + off, 4u)) ? cpu_.mem->rd32(cxx_vt + off)
                                                                              : 0u;
            };
            dshowLog(LogLevel::Info,
                     "诊断：input_pin=0x%08X IPin-vtable=0x%08X [pin-0xC]=0x%08X ⇒ "
                     "CheckMediaType(+0x20)=0x%08X SetMediaType(+0x24)=0x%08X CheckConnect(+0x28)=0x%08X "
                     "CompleteConnect(+0x30)=0x%08X",
                     input_pin_, pin_vt, cxx_vt, cxxSlot(0x20), cxxSlot(0x24), cxxSlot(0x28), cxxSlot(0x30));
            dshowLog(LogLevel::Warn, "ReceiveConnection(...) = 0x%08X ⇒ 接线失败（引擎**不会**发现！）",
                     result);
        }
        step_ = StepRenderQiMemInput;
        return stepLaunch(&err);
    }
    case StepRenderQiMemInput: {
        std::uint32_t mem = 0;
        if (result == kSOk)
            mem = cpu_.mem->rd32(scratch_ + kScratchQiOut);
        if (mem == 0) {
            // 退回**布局**：`0x4D44FB` 把 IMemInputPin vtable 写在 [esi+0x98]（doc §2-B）。
            mem = input_pin_ + kMemInputFromPinDelta;
            dshowLog(LogLevel::Warn,
                     "QI(IID_IMemInputPin) 返回 0x%08X ⇒ 按布局退回 pin+0x8C = 0x%08X"
                     "（证据：0x4D44FB mov [esi+98h], ??_7CRendererInputPin@@6B@_2）",
                     result, mem);
        }
        mem_input_ = mem;
        stats_.mem_input_ea = mem_input_;
        step_ = StepRenderNotifyAllocator;
        return stepLaunch(&err);
    }
    case StepRenderNotifyAllocator: {
        stats_.notify_allocator_hr = result;
        if (result == kSOk) {
            dshowLog(LogLevel::Info,
                     "NotifyAllocator(宿主分配器 0x%08X, fReadOnly=FALSE) = S_OK"
                     " ⇒ 引擎不会去 CoCreateInstance(CLSID_MemoryAllocator)",
                     objs_->allocator());
        } else {
            dshowLog(LogLevel::Warn,
                     "NotifyAllocator(0x%08X, FALSE) = 0x%08X ⇒ 引擎可能退到 CMemAllocator，"
                     "那会在 GetBuffer 里 WaitForSingleObject（SCOPE §1 硬错误）",
                     objs_->allocator(), result);
        }
        wired_ = (stats_.connect_hr == kSOk && stats_.notify_allocator_hr == kSOk);
        dshowLog(wired_ ? LogLevel::Info : LogLevel::Warn, "接线%s（connect=0x%08X notify=0x%08X）",
                 wired_ ? "完成" : "未完成", stats_.connect_hr, stats_.notify_allocator_hr);
        return finishChain();
    }

    // ---------------------------- Run ----------------------------
    case StepRunClockRenderer:
    case StepRunClockSource: {
        // `CBaseFilter::SetSyncSource`：存 m_pClock + AddRef（成功 = S_OK）。
        dshowLog(LogLevel::Info, "SetSyncSource(filter=0x%08X, clock=0x%08X) = 0x%08X",
                 step_ == StepRunClockRenderer ? renderer_ : source_, objs_->clock(), result);
        if (step_ == StepRunClockRenderer && source_ != 0) {
            step_ = StepRunClockSource;
            return stepLaunch(&err);
        }

        step_ = renderer_ != 0 ? StepRunPauseRenderer : StepRunPauseSource;

        return stepLaunch(&err);
    }

    case StepRunPauseRenderer: {

        dshowLog(LogLevel::Info, "Pause(渲染器 0x%08X) = 0x%08X", renderer_, result);

        if (source_ != 0) {

            step_ = StepRunPauseSource;

            return stepLaunch(&err);
        }

        return endRunChain();
    }

    case StepRunPauseSource: {

        dshowLog(LogLevel::Info, "Pause(源 0x%08X) = 0x%08X", source_, result);

        step_ = StepRunSource;

        return stepLaunch(&err);
    }

    case StepRunSource: {

        dshowLog(LogLevel::Info, "Run(源 0x%08X) = 0x%08X", source_, result);

        if (renderer_ != 0) {
            step_ = StepRunRenderer;
            return stepLaunch(&err);
        }
        return endRunChain();
    }

    case StepRunRenderer: {

        dshowLog(LogLevel::Info, "Run(渲染器 0x%08X) = 0x%08X（Running ⇒ 由引擎自己按流时间渲染）",
                 renderer_, result);

        return endRunChain();
    }

        // ---------------------------- Pause ----------------------------

    case StepPausePauseRenderer: {

        dshowLog(LogLevel::Info, "Pause(渲染器 0x%08X) = 0x%08X", renderer_, result);

        if (source_ != 0) {

            step_ = StepPausePauseSource;

            return stepLaunch(&err);
        }

        return endPauseChain();
    }

    case StepPausePauseSource: {

        dshowLog(LogLevel::Info, "Pause(源 0x%08X) = 0x%08X", source_, result);

        return endPauseChain();
    }

        // ---------------------------- Stop ----------------------------

    case StepStopRenderer: {

        dshowLog(LogLevel::Debug, "Stop(渲染器 0x%08X) = 0x%08X", renderer_, result);

        if (source_ != 0) {

            step_ = StepStopSource;

            return stepLaunch(&err);
        }

        return endStopChain();
    }

    case StepStopSource: {

        dshowLog(LogLevel::Debug, "Stop(源 0x%08X) = 0x%08X", source_, result);

        return endStopChain();
    }

        // ---------------------------- 泵 ----------------------------

    case StepPumpReceive: {
        ++stats_.receive_calls;
        if (result == kSOk) {
            ++stats_.samples_delivered;
            dshowLog(LogLevel::Debug, "Receive(sample=0x%08X) = S_OK（第 %llu 帧，pts=%lld）",
                     objs_->sample(), static_cast<unsigned long long>(stats_.samples_delivered),
                     static_cast<long long>(last_pts_100ns_));
            if (stats_.samples_delivered == 1) {
                dshowLog(LogLevel::Info, "首帧已送渲染器：pts=%lld（纹理应该开始有像素）",
                         static_cast<long long>(last_pts_100ns_));
            } else if (stats_.samples_delivered % 30u == 0u) {
                dshowLog(LogLevel::Info, "samples=%llu（pts=%.3fs / 时长 %.3fs）",
                         static_cast<unsigned long long>(stats_.samples_delivered),
                         static_cast<double>(last_pts_100ns_) / kTicksPerSecond, duration_sec_);
            }
            // ★ 像素判据（doc §8.13）：Receive 返回后，引擎的 `DoRenderSample` 已经把这一帧写进
            //   **影片纹理**（渲染器保持 Paused ⇒ 当场渲染，doc §2-E）。纹理对象在 `renderer + 0x56C`
            //   （doc §3-2：`0x488AB0/0x488AE0` 的前置）。只读；每 30 帧 + 首帧各抽一次。
            if (stats_.samples_delivered == 1u || stats_.samples_delivered % 30u == 0u) {
                // `renderer_` 是 IBaseFilter 子对象（= 对象基址 + 0xC）；纹理字段在**基址** + 0x56C
                // （实测：`[renderer_+0x560]` 非 0、`[renderer_+0x56C]` 是 0 ⇒ 基址 = renderer_ - 0xC）。
                const std::uint32_t renderer_base = renderer_ >= 0x0Cu ? renderer_ - 0x0Cu : 0u;
                std::uint32_t tex = 0;
                if (renderer_base != 0 && cpu_.mem->committed(renderer_base + 0x56Cu, 4u)) {
                    tex = cpu_.mem->rd32(renderer_base + 0x56Cu);
                }
                if (stats_.samples_delivered == 1u) {
                    d3d9::TextureInfo ti;
                    std::string why;
                    const bool ok = tex != 0 && d3d9::textureInfo(cpu_, tex, &ti, &why);
                    const bool cm = ok && cpu_.mem->committed(ti.arena, 4u);
                    dshowLog(LogLevel::Debug,
                             "tex info: tex=0x%08X ok=%d w=%u h=%u bpp=%u pitch=%u arena=0x%08X committed=%d "
                             "first=0x%08X why=%s",
                             tex, ok ? 1 : 0, ok ? ti.width : 0u, ok ? ti.height : 0u, ok ? ti.bpp : 0u,
                             ok ? ti.pitch : 0u, ok ? ti.arena : 0u, cm ? 1 : 0,
                             cm ? cpu_.mem->rd32(ti.arena) : 0u, why.c_str());
                }
                d3d9::TexturePixels px;
                if (tex != 0 && d3d9::sampleTexture(cpu_, tex, &px)) {
                    dshowLog(LogLevel::Info, "tex movie #%llu w=%u h=%u nonzero=%u sampled=%u hash=%016llx",
                             static_cast<unsigned long long>(stats_.samples_delivered), px.width, px.height,
                             px.nonzero, px.sampled, static_cast<unsigned long long>(px.hash));
                }
            }
        } else {
            dshowLog(LogLevel::Debug, "Receive(sample=0x%08X) = 0x%08X", objs_->sample(), result);
        }
        if (objs_)
            objs_->releaseBuffer(cpu_);
        return finishChain();
    }
    case StepNone:
        break;
    }
    dshowLog(LogLevel::Error, "续延收到未知步号 %d ⇒ 收尾", step_);
    return finishChain();
}
// =====================================================================================
// 控制：run / pause / stop
// =====================================================================================
bool Graph::endRunChain() {
    // ★ 计时从链尾开始（Pause/Run 都走完之后）：引擎每拍读的就是"相对 Run 的偏移"。
    start_100ns_ = now_();
    last_pump_100ns_ = 0;
    frozen_100ns_ = 0;
    completed_ = false;
    state_ = 2; // State_Running
    dshowLog(LogLevel::Info,
             "IMediaControl::Run：渲染器保持 Paused（Paused 下 Receive 当场渲染；依据 doc §2-E "
             "sub_4D4000 的 [this+0x14]==1 分支），源 filter 已 Run；t0=%lld",
             static_cast<long long>(start_100ns_));
    return finishChain();
}
bool Graph::endPauseChain() {
    if (state_ == 2) {
        const std::int64_t rel = now_() - start_100ns_;
        frozen_100ns_ = rel > 0 ? rel : 0;
        if (duration_100ns_ > 0 && frozen_100ns_ > duration_100ns_)
            frozen_100ns_ = duration_100ns_;
    }
    state_ = 1; // State_Paused
    dshowLog(LogLevel::Info, "IMediaControl::Pause：两个 filter 已 Pause；冻结位置 %.3f s",
             static_cast<double>(frozen_100ns_) / kTicksPerSecond);
    return finishChain();
}
bool Graph::endStopChain() {
    state_ = 0; // State_Stopped
    completed_ = false;
    has_pts_ = false;
    last_pts_100ns_ = 0;
    last_pump_100ns_ = 0;
    start_100ns_ = 0;
    frozen_100ns_ = 0;
    if (objs_)
        objs_->releaseBuffer(cpu_);
    if (feed_ != nullptr)
        feed_->rewind(); // 循环播放：引擎会 Stop → SetPosition(0) → Run
    dshowLog(LogLevel::Info, "IMediaControl::Stop：两个 filter 已 Stop；播放位置归零");
    return finishChain();
}
void Graph::run() {
    if (suspended_) {
        dshowLog(LogLevel::Warn, "IMediaControl::Run: 上一次 guest 调用还没结束 ⇒ 忽略");
        return;
    }
    if (renderer_ == 0 && source_ == 0) {
        dshowLog(LogLevel::Warn, "IMediaControl::Run: 一个 filter 都没有（AddFilter 没被调？）⇒ 只记状态");
        state_ = 2;
        start_100ns_ = now_();
        last_pump_100ns_ = 0;
        return;
    }
    if (!wired_) {
        dshowLog(LogLevel::Warn,
                 "IMediaControl::Run: 接线没成功（connect=0x%08X notify=0x%08X）⇒ 仍按协议驱动 filter，"
                 "但不会有帧",
                 stats_.connect_hr, stats_.notify_allocator_hr);
    }
    // ★ `Run` 之前先给两个 filter `IBaseFilter::SetSyncSource(我们的 IReferenceClock)`（+0x20）：
    //   真图管理器就是这么做的；不设的话 `WaitForRenderTime` 走"无时钟"分支 ⇒
    //   `sub_4D5710` 的 `WaitForMultipleObjects(2, …, 10000)` 每 10 s 重试（实测卡死）。
    const int first = renderer_ != 0 ? StepRunClockRenderer : StepRunClockSource;
    if (!startChain(first, "IMediaControl::Run"))
        endRunChain();
}
void Graph::pause() {
    if (suspended_) {
        dshowLog(LogLevel::Warn, "IMediaControl::Pause: 上一次 guest 调用还没结束 ⇒ 忽略");
        return;
    }
    if (renderer_ == 0 && source_ == 0) {
        state_ = 1;
        return;
    }
    const int first = renderer_ != 0 ? StepPausePauseRenderer : StepPausePauseSource;
    if (!startChain(first, "IMediaControl::Pause"))
        endPauseChain();
}
void Graph::stop() {
    if (suspended_) {
        dshowLog(LogLevel::Warn, "IMediaControl::Stop: 上一次 guest 调用还没结束 ⇒ 忽略");
        return;
    }
    if (renderer_ == 0 && source_ == 0) {
        endStopChain();
        return;
    }
    const int first = renderer_ != 0 ? StepStopRenderer : StepStopSource;
    if (!startChain(first, "IMediaControl::Stop"))
        endStopChain();
}
int Graph::state() const {
    return state_;
}
double Graph::stopPosition() const {
    return duration_sec_;
}
double Graph::duration() const {
    return duration_sec_;
}
void Graph::setVolumeDb(double db) {
    volume_db_ = db;
    // IBasicAudio::put_Volume 的入参是**百分之一分贝**（LONG）：`sub_4879E0` / `0x487A2F`（doc §0-2）。
    volume_db100_ = static_cast<int>(db * 100.0);
    dshowLog(LogLevel::Info, "IBasicAudio::put_Volume(%.3f dB) ⇒ 记账 %d（⛔ 不碰真设备）", db,
             volume_db100_);
}
// =====================================================================================
// 位置 / 泵
// =====================================================================================
double Graph::currentPosition() {
    if (state_ == 0)
        return 0.0;
    if (state_ == 1)
        return static_cast<double>(frozen_100ns_) / kTicksPerSecond;
    // ★ 诊断：样本对象头是否还是我们的 vtable（排查"PrepareReceive 读到野 vtable"）。
    if (objs_) {
        dshowLog(LogLevel::Debug, "currentPosition 入口：state=%d wired=%d [sample]=0x%08X 期望=0x%08X",
                 state_, wired_ ? 1 : 0, cpu_.mem->rd32(objs_->sample()),
                 cpu_.com ? cpu_.com->vtableOf(objs_->sample()) : 0u);
    }
    pump();
    const std::int64_t rel = now_() - start_100ns_;
    if (rel <= 0)
        return 0.0;
    double sec = static_cast<double>(rel) / kTicksPerSecond;
    if (duration_sec_ > 0.0 && sec > duration_sec_)
        sec = duration_sec_;
    return sec;
}
void Graph::pump() {
    if (state_ != 2 || suspended_)
        return;
    // ★ 接线没成功就**绝不**送帧：`ReceiveConnection` 失败时渲染器的输入 pin 没有 media type /
    //   分配器绑定，`CBaseRenderer::Receive`（`sub_4D5C60` → 取分配器）会读到垃圾指针
    //   （实测 guest fault @`0x4D1427` READ 0x6E554831）。任务口径：失败 ⇒ Warn + **无帧**。
    if (!wired_) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            dshowLog(
                LogLevel::Warn,
                "泵不送帧：接线未完成（connect=0x%08X notify=0x%08X）⇒ 无帧（⛔ 不把样本塞给未连接的渲染器）",
                stats_.connect_hr, stats_.notify_allocator_hr);
        }
        return;
    }
    if (objs_)
        stats_.dropped_busy += objs_->takeDropped(cpu_);
    const std::int64_t rel = now_() - start_100ns_;
    if (rel <= last_pump_100ns_)
        return; // 同一拍 / 倒退 ⇒ 不再送
    last_pump_100ns_ = rel;
    if (!opened_ || feed_ == nullptr || !objs_ || !objs_->ready())
        return;
    if (duration_100ns_ > 0 && rel >= duration_100ns_) {
        dshowLog(LogLevel::Info, "到达片长 %.3f s ⇒ 泵停（completed=true；引擎随后会调 Stop/销毁）",
                 duration_sec_);
        complete();
        return;
    }
    for (int i = 0; i < kMaxSamplesPerPump; ++i) {
        const std::uint8_t *rgb = nullptr;
        std::uint32_t src_stride = 0;
        std::int64_t pts = 0;
        bool ended = false;
        dshowLog(LogLevel::Debug, "泵：调 feed_->at(rel=%lld)（buffer=0x%08X sample=0x%08X）",
                 static_cast<long long>(rel), objs_->buffer(), objs_->sample());
        if (!feed_->at(rel, &rgb, &src_stride, &pts, &ended)) {
            if (ended) {
                dshowLog(LogLevel::Info, "帧源报告结束（%.3f s）⇒ completed=true",
                         static_cast<double>(rel) / kTicksPerSecond);
                complete();
            } else {
                dshowLog(LogLevel::Debug, "泵：feed_->at 没有新帧（rel=%lld）", static_cast<long long>(rel));
            }
            return;
        }
        if (rgb == nullptr) {
            dshowLog(LogLevel::Warn, "FrameFeed::at 返回 true 但 rgb24 为空 ⇒ 这一拍不送样本");
            return;
        }
        ++stats_.frames_decoded;
        dshowLog(LogLevel::Debug, "泵：解出帧 pts=%lld rgb=%p stride=%u", static_cast<long long>(pts),
                 static_cast<const void *>(rgb), src_stride);
        if (has_pts_ && pts <= last_pts_100ns_)
            return; // 同一帧不重复送（时间戳必须单调）
        dshowLog(
            LogLevel::Debug, "copyFrame 前：buffer=0x%08X bytes=%u sample=0x%08X ⇒ 缓冲%s样本",
            objs_->buffer(), objs_->bufferBytes(), objs_->sample(),
            (objs_->sample() >= objs_->buffer() && objs_->sample() < objs_->buffer() + objs_->bufferBytes())
                ? "**覆盖**"
                : "不含");
        if (!objs_->acquire(cpu_)) {
            ++stats_.dropped_busy; // 渲染器还占着唯一缓冲 ⇒ 丢这一帧（⛔ 不等）
            dshowLog(LogLevel::Debug, "分配器忙 ⇒ 丢这一帧（pts=%lld）", static_cast<long long>(pts));
            return;
        }
        if (!copyFrame(rgb, src_stride, pts)) {
            objs_->releaseBuffer(cpu_);
            return;
        }
        dshowLog(LogLevel::Debug,
                 "copyFrame 后：buffer=0x%08X state=0x%08X sample=0x%08X [sample]=0x%08X 期望=0x%08X",
                 objs_->buffer(), objs_->sampleStateEa(), objs_->sample(), cpu_.mem->rd32(objs_->sample()),
                 cpu_.com ? cpu_.com->vtableOf(objs_->sample()) : 0u);
        last_pts_100ns_ = pts;
        has_pts_ = true;
        stats_.last_pts_100ns = pts;
        // ★ 诊断（Debug）：送 Receive 之前确认样本对象头还是我们建的那一份。
        if (cpu_.com != nullptr) {
            dshowLog(LogLevel::Debug, "送帧前：sample=0x%08X [sample]=0x%08X 期望 vtable=0x%08X refcount=%u",
                     objs_->sample(), cpu_.mem->rd32(objs_->sample()), cpu_.com->vtableOf(objs_->sample()),
                     cpu_.com->refCount(objs_->sample()));
        }
        step_ = StepPumpReceive;
        suspended_ = true;
        std::string err;
        dshowLog(LogLevel::Debug, "送帧前：esp=0x%08X sample=0x%08X [sample]=0x%08X pin=0x%08X", cpu_.esp(),
                 objs_->sample(), cpu_.mem->rd32(objs_->sample()), mem_input_);
        if (!launch(step_, &err)) {
            dshowLog(LogLevel::Error, "IMemInputPin::Receive 发不出去：%s ⇒ 丢这一帧", err.c_str());
            suspended_ = false;
            step_ = StepNone;
            objs_->releaseBuffer(cpu_);
        } else {
            dshowLog(LogLevel::Debug, "launch 后（guest 还没跑）：[sample]=0x%08X esp=0x%08X",
                     cpu_.mem->rd32(objs_->sample()), cpu_.esp());
        }
        return; // 时间寻址的源每次最多一帧（见 kMaxSamplesPerPump 的说明）
    }
}
bool Graph::copyFrame(const std::uint8_t *rgb, std::uint32_t src_stride, std::int64_t pts) {
    const std::uint32_t buf = objs_->buffer();
    if (buf == 0 || rgb == nullptr)
        return false;
    const std::uint32_t row_bytes = width_ * 3u;
    const std::uint32_t use_stride = src_stride != 0 ? src_stride : row_bytes;
    if (use_stride < row_bytes) {
        dshowLog(LogLevel::Warn, "帧源的 stride=%u 小于 w*3=%u ⇒ 按 w*3 拷会越界，改成逐行最多 %u 字节",
                 use_stride, row_bytes, use_stride);
    }
    const std::uint32_t copy_bytes = use_stride < row_bytes ? use_stride : row_bytes;
    // ★ **自底向上**：渲染器按 `base + (h-1-y)*stride` 读（doc §3-3 `0x487E3D`），
    //   而 `FrameFeed::at` 给的是**顶朝下** RGB24 ⇒ 目标行 y 取源行 (h-1-y)。
    for (std::uint32_t y = 0; y < height_; ++y) {
        const std::uint8_t *src = rgb + static_cast<std::size_t>(height_ - 1u - y) * use_stride;
        cpu_.mem->writeBytes(buf + y * stride_, src, copy_bytes);
    }
    objs_->setSampleLength(cpu_, stride_ * height_);
    // ★ 时间基（**关键，别改成绝对时钟**）：渲染器算"何时该渲染"用的是
    //   `m_rtStart + m_tStart`（`sub_4D5E50`：`*(QWORD*)(this+32) + *(QWORD*)(this+184)`），
    //   其中 `m_tStart` 是 `IBaseFilter::Run(tStart)` 传进去的**起始时钟**，`m_rtStart` 是样本时间。
    //   ⇒ 样本时间是**流时间（相对片头）**，不是绝对时钟；写成绝对时钟会让和变成 2×时钟 ⇒
    //   渲染器用 `timeSetEvent` 排一个永远不到期的定时器 ⇒ 纹理永远是黑的（实测）。
    //   媒体时间（`GetMediaTime`）同样是相对片头。
    objs_->setSampleTiming(cpu_, pts, pts + kDefaultAvgTimePerFrame100ns);
    objs_->setSampleMediaTime(cpu_, pts, pts + kDefaultAvgTimePerFrame100ns);
    dshowLog(LogLevel::Debug, "样本时间：tStart=%lld..%lld（流时间，Run.tStart 由渲染器相加） mediaTime 同域",
             static_cast<long long>(pts), static_cast<long long>(pts + kDefaultAvgTimePerFrame100ns));
    return true;
}
void Graph::complete() {
    if (!completed_)
        dshowLog(LogLevel::Info, "completed=true：影片结束（已送 %llu 帧，最后 pts=%lld）",
                 static_cast<unsigned long long>(stats_.samples_delivered),
                 static_cast<long long>(last_pts_100ns_));
    completed_ = true;
    stats_.completed = true;
    state_ = 0; // State_Stopped（引擎随后会调 Stop / 销毁）
    if (objs_)
        objs_->releaseBuffer(cpu_);
}
void Graph::releaseSource() {
    if (read_buf_ != 0 && cpu_.heap != nullptr)
        cpu_.heap->free(read_buf_);
    read_buf_ = 0;
    read_buf_bytes_ = 0;
    if (iasync_ != 0) {
        // ⛔ 这里不发起 guest 调用（析构 / 宿主失败路径都不在链上）：只留痕。
        dshowLog(LogLevel::Debug, "releaseSource: IAsyncReader 0x%08X 仍未 Release（链没走到释放步）",
                 iasync_);
    }
    iasync_ = 0;
    source_bytes_.clear();
}
} // namespace lift::dshow
