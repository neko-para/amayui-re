// emulator-lift · D3D9 状态 / 像素层（M2 · 第一帧的前置）
//
// ★ 读 `runtime/d3d9.h` 的文件头先：范围、结构偏移的来源（实测 EA vs 头文件推断）、状态放哪儿。
// ★ 本文件**不认 COM**：IDirect3D9 / IDirect3DDevice9 的对象与 vtable 在 `runtime/com.h`，
//   方法 shim 在 `shims/d3d9.cpp`；这里只提供它们调用的语义。
// ★ 一切"真的做"的部分都在这里落地：
//     · Clear  **真的**把后缓冲像素清成给定颜色（按像素格式换算，32bpp 写 0xAARRGGBB）
//     · Present **真的**把后缓冲交给 HeadlessPresenter（frameCount / lastHash / 帧号日志）
//   ⛔ 不是"返回 S_OK 就算做了"。
#include "runtime/d3d9.h"

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "runtime/com.h"
#include "runtime/d3d9_pipeline.h"
#include "runtime/ddraw_surface.h"
#include "runtime/gpu_raster.h"
#include "runtime/heap.h"
#include "runtime/pixel_format.h"
#include "runtime/machine.h"
#include "runtime/memory.h"
#include "runtime/present.h"
#include "runtime/surface.h"
#include "runtime/log.h"
#include "runtime/x86.h"

namespace lift {
namespace d3d9 {

namespace {
std::uint64_t steadyNowNs() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());
}
}  // namespace
namespace {

// 抽样统计：每 64 个像素取一个（1280×720 ⇒ 14400 个），返回非黑个数 / 采样数 / FNV-1a hash。
void samplePixels(X86& cpu, std::uint32_t arena, std::uint32_t w, std::uint32_t h, std::uint32_t pitch,
                  std::uint32_t bpp, std::uint32_t* nonzero, std::uint32_t* sampled, std::uint64_t* hash) {
    std::uint64_t hs = 1469598103934665603ull;
    std::uint32_t nz = 0;
    std::uint32_t ns = 0;
    const std::uint64_t total = static_cast<std::uint64_t>(w) * h;
    for (std::uint64_t i = 0; i < total; i += 64u) {
        const std::uint32_t y = static_cast<std::uint32_t>(i / w);
        const std::uint32_t x = static_cast<std::uint32_t>(i % w);
        const std::uint32_t ea = arena + y * pitch + x * bpp;
        if (!cpu.mem->committed(ea, bpp)) continue;
        const std::uint32_t px = (bpp == 2u) ? cpu.mem->rd16(ea) : cpu.mem->rd32(ea);
        ++ns;
        if ((px & (bpp == 2u ? 0x0FFFu : 0x00FFFFFFu)) != 0u) ++nz;
        hs ^= px;
        hs *= 1099511628211ull;
    }
    if (nonzero) *nonzero = nz;
    if (sampled) *sampled = ns;
    if (hash) *hash = hs;
}

// 设备状态块布局（**我们自己的**布局；⛔ 不是 Win32 结构体）
enum DevOff : std::uint32_t {
    kDvMagic = 0,       // 'D9D1'
    kDvArena = 4,       // 后缓冲像素块（SurfaceArena EA）
    kDvWidth = 8,
    kDvHeight = 12,
    kDvFormat = 16,     // D3DFORMAT
    kDvBpp = 20,
    kDvPitch = 24,
    kDvDeviceType = 28,
    kDvBehavior = 32,
    kDvHwnd = 36,
    kDvWindowed = 40,
    kDvAutoDepthFormat = 44,
    kDvPresents = 48,
    kDvClears = 52,
    kDvBytes = 64,
};
constexpr std::uint32_t kDvMagicValue = 0x31443944u;  // 'D9D1'

void d3d9Log(const char* what, const char* fmt, ...) {
    char prefix[256];
    std::snprintf(prefix, sizeof(prefix), "[d3d9] %s: ", what ? what : "?");
    va_list ap;
    va_start(ap, fmt);
    logV(LogLevel::Info, LogD3d9, prefix, fmt, ap);
    va_end(ap);
}

// 契约级失败（guest 违约）用它：默认 warn 级别就看得见（Info 要 --verbose）。
void d3d9Warn(const char* what, const char* fmt, ...) {
    char prefix[256];
    std::snprintf(prefix, sizeof(prefix), "[d3d9] %s: ", what ? what : "?");
    va_list ap;
    va_start(ap, fmt);
    logV(LogLevel::Warn, LogD3d9, prefix, fmt, ap);
    va_end(ap);
}

// 与 DDraw 的 requireMachine 同形（同一条装配口径：activeMachine 必须是这台 cpu 的 Machine）
bool requireMachine(X86& cpu, Machine** out, std::string* why) {
    Machine* m = activeMachine();
    if (!m || &m->cpu() != &cpu) {
        if (why) *why = "D3D9：activeMachine() 不是这台 cpu 的 Machine（装配问题）";
        return false;
    }
    if (!m->surfaces().ready()) {
        if (why) *why = "D3D9：SurfaceArena 未初始化";
        return false;
    }
    *out = m;
    return true;
}

// ---- 适配器模式表（确定性；与 DDraw 的虚拟显示器同一组分辨率）----
struct Resolution {
    int width;
    int height;
};
constexpr Resolution kResolutions[] = {
    {640, 480},   {800, 600},   {1024, 768},  {1152, 864},  {1280, 720},
    {1280, 800},  {1280, 960},  {1280, 1024}, {1366, 768},  {1440, 900},
    {1600, 900},  {1600, 1200}, {1680, 1050}, {1920, 1080}, {1920, 1200},
};
constexpr std::size_t kResolutionCount = sizeof(kResolutions) / sizeof(kResolutions[0]);

bool isSupportedModeFormat(std::uint32_t format) { return format == kFmtX8R8G8B8 || format == kFmtR5G6B5; }

// D3DFORMAT → 像素布局（⛔ 不用 formatForBpp 一刀切：桌面 32bpp 是 **X**8R8G8B8，顶字节无意义）
bool formatForD3dFormat(std::uint32_t format, SurfaceFormat* out) {
    SurfaceFormat f;
    switch (format) {
        case kFmtA8R8G8B8:
            f.bpp = 32;
            f.rmask = 0x00FF0000u;
            f.gmask = 0x0000FF00u;
            f.bmask = 0x000000FFu;
            f.amask = 0xFF000000u;
            break;
        case kFmtX8R8G8B8:
            f.bpp = 32;
            f.rmask = 0x00FF0000u;
            f.gmask = 0x0000FF00u;
            f.bmask = 0x000000FFu;
            f.amask = 0u;  // X：顶字节不携带信息
            break;
        case kFmtR5G6B5:
            f.bpp = 16;
            f.rmask = 0xF800u;
            f.gmask = 0x07E0u;
            f.bmask = 0x001Fu;
            f.amask = 0u;
            break;
        case kFmtX1R5G5B5:
            f.bpp = 16;
            f.rmask = 0x7C00u;
            f.gmask = 0x03E0u;
            f.bmask = 0x001Fu;
            f.amask = 0u;
            break;
        case kFmtA1R5G5B5:
            f.bpp = 16;
            f.rmask = 0x7C00u;
            f.gmask = 0x03E0u;
            f.bmask = 0x001Fu;
            f.amask = 0x8000u;
            break;
        case kFmtA4R4G4B4:
            f.bpp = 16;
            f.rmask = 0x0F00u;
            f.gmask = 0x00F0u;
            f.bmask = 0x000Fu;
            f.amask = 0xF000u;
            break;
        default:
            return false;
    }
    *out = f;
    return true;
}

bool bppForD3dFormat(std::uint32_t format, unsigned* bpp) {
    SurfaceFormat f;
    if (!formatForD3dFormat(format, &f)) return false;
    *bpp = f.bpp;
    return true;
}

bool committedOut(X86& cpu, std::uint32_t ea, std::size_t n, const char* what, std::string* why) {
    if (ea == 0) {
        if (why) *why = std::string("D3D9：") + what + " 为 NULL";
        return false;
    }
    if (!cpu.mem->committed(ea, n)) {
        char buf[192];
        std::snprintf(buf, sizeof(buf), "D3D9：%s = 0x%08X 起 %u 字节未提交（guest 缓冲区非法）", what, ea,
                      static_cast<unsigned>(n));
        if (why) *why = buf;
        return false;
    }
    return true;
}

void wrFloat(X86& cpu, std::uint32_t ea, float v) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &v, sizeof(bits));
    cpu.mem->wr32(ea, bits);
}

// CheckDeviceFormat 的环形诊断记录（宿主侧；⛔ 不是语义状态，见 d3d9.h）
CheckFormatQuery g_check_format_log[kCheckFormatLogCapacity];
std::size_t g_check_format_count = 0;  // 累计调用次数（环形只保留最近 16 条）

}  // namespace

// ---------------------------------------------------------------------------------------
// 适配器
// ---------------------------------------------------------------------------------------
std::uint32_t displayFormatForBpp(unsigned bpp) {
    // 桌面模式没有 alpha ⇒ 32bpp 报 X8R8G8B8；16bpp 报 R5G6B5（本模拟器只有这两种像素布局）
    return bpp == 16 ? kFmtR5G6B5 : kFmtX8R8G8B8;
}

bool adapterDisplayMode(X86& cpu, AdapterMode* out, std::string* why) {
    int width = 0;
    int height = 0;
    unsigned bpp = 0;
    // ★ 与 DDraw **共用**同一台虚拟显示器：`set:ScreenX/ScreenY/FullScreenBit` 落在 DDraw 的设备状态页，
    //   D3D9 报同一份模式 ⇒ 两个 API 不会各自发明一个桌面。
    if (!ddraw::displayMode(cpu, &width, &height, &bpp, nullptr)) {
        if (why) *why = "D3D9：读不到虚拟显示器的显示模式（DDraw 设备状态页读失败）";
        return false;
    }
    out->width = width;
    out->height = height;
    out->refresh = ddraw::kRefreshRate;
    out->format = displayFormatForBpp(bpp);
    return true;
}

std::uint32_t adapterModeCount(std::uint32_t format) {
    return isSupportedModeFormat(format) ? static_cast<std::uint32_t>(kResolutionCount) : 0u;
}

bool adapterModeAt(std::uint32_t format, std::uint32_t index, AdapterMode* out) {
    if (!isSupportedModeFormat(format)) return false;
    if (index >= kResolutionCount) return false;
    const Resolution& r = kResolutions[index];
    out->width = r.width;
    out->height = r.height;
    out->refresh = ddraw::kRefreshRate;
    out->format = format;
    return true;
}

bool writeDisplayMode(X86& cpu, std::uint32_t ea, const AdapterMode& mode, std::string* why) {
    if (!committedOut(cpu, ea, kDisplayModeBytes, "D3DDISPLAYMODE", why)) return false;
    cpu.mem->wr32(ea + kDmWidth, static_cast<std::uint32_t>(mode.width));
    cpu.mem->wr32(ea + kDmHeight, static_cast<std::uint32_t>(mode.height));
    cpu.mem->wr32(ea + kDmRefreshRate, mode.refresh);
    cpu.mem->wr32(ea + kDmFormat, mode.format);
    return true;
}

// ---------------------------------------------------------------------------------------
// D3DPRESENT_PARAMETERS
// ---------------------------------------------------------------------------------------
bool readPresentParams(X86& cpu, std::uint32_t ea, PresentParams* out, std::string* why) {
    if (!committedOut(cpu, ea, kPresentParamsBytes, "D3DPRESENT_PARAMETERS", why)) return false;
    PresentParams p;
    p.width = cpu.mem->rd32(ea + kPpBackBufferWidth);
    p.height = cpu.mem->rd32(ea + kPpBackBufferHeight);
    p.format = cpu.mem->rd32(ea + kPpBackBufferFormat);
    p.back_buffer_count = cpu.mem->rd32(ea + kPpBackBufferCount);
    p.multi_sample_type = cpu.mem->rd32(ea + kPpMultiSampleType);
    p.multi_sample_quality = cpu.mem->rd32(ea + kPpMultiSampleQuality);
    p.swap_effect = cpu.mem->rd32(ea + kPpSwapEffect);
    p.device_window = cpu.mem->rd32(ea + kPpHDeviceWindow);
    p.windowed = cpu.mem->rd32(ea + kPpWindowed) != 0;
    p.auto_depth_stencil = cpu.mem->rd32(ea + kPpEnableAutoDepthStencil) != 0;
    p.auto_depth_stencil_format = cpu.mem->rd32(ea + kPpAutoDepthStencilFormat);
    p.flags = cpu.mem->rd32(ea + kPpFlags);
    p.refresh_hz = cpu.mem->rd32(ea + kPpFullScreenRefreshRateInHz);
    p.presentation_interval = cpu.mem->rd32(ea + kPpPresentationInterval);
    *out = p;
    return true;
}

// ---------------------------------------------------------------------------------------
// 设备状态
// ---------------------------------------------------------------------------------------
std::uint32_t createDeviceState(X86& cpu, const CreateRequest& req, std::string* why) {
    Machine* m = nullptr;
    if (!requireMachine(cpu, &m, why)) return 0;
    if (!cpu.mem) {
        if (why) *why = "D3D9：cpu.mem 为空";
        return 0;
    }
    if (!cpu.heap || !cpu.heap->ready()) {
        if (why) *why = "D3D9：guest 堆未就绪（设备状态块要放在 guest 堆上）";
        return 0;
    }
    // ① 后缓冲尺寸 / 格式（0 = 用当前显示模式，与真机的窗口化语义一致）
    AdapterMode desktop;
    const bool need_desktop = req.present.width == 0 || req.present.height == 0 ||
                              req.present.format == kFmtUnknown;
    if (need_desktop && !adapterDisplayMode(cpu, &desktop, why)) return 0;
    const std::int32_t width = static_cast<std::int32_t>(
        req.present.width != 0 ? req.present.width : static_cast<std::uint32_t>(desktop.width));
    const std::int32_t height = static_cast<std::int32_t>(
        req.present.height != 0 ? req.present.height : static_cast<std::uint32_t>(desktop.height));
    const std::uint32_t format = req.present.format != kFmtUnknown ? req.present.format : desktop.format;
    unsigned bpp = 0;
    if (!bppForD3dFormat(format, &bpp)) {
        char buf[160];
        std::snprintf(buf, sizeof(buf),
                      "D3D9：后缓冲格式 D3DFORMAT=%u 不在本线支持集（21/22 的 32bpp、23/24/25/26 的 16bpp）",
                      format);
        if (why) *why = buf;
        return 0;
    }
    if (width <= 0 || height <= 0 || width > 16384 || height > 16384) {
        char buf[160];
        std::snprintf(buf, sizeof(buf), "D3D9：后缓冲尺寸非法（%dx%d）", width, height);
        if (why) *why = buf;
        return 0;
    }
    SurfaceFormat fmt;
    if (!formatForD3dFormat(format, &fmt)) {  // bppForD3dFormat 已经过了 ⇒ 这里不可能失败，但别假设
        if (why) *why = "D3D9：像素格式换算失败";
        return 0;
    }
    // ② 后缓冲像素（guest 地址空间；SurfaceArena 会清零）
    const std::uint32_t arena = m->surfaces().create(width, height, fmt);
    if (arena == 0) {
        if (why) *why = std::string("D3D9：SurfaceArena.create 失败：") + m->surfaces().lastError();
        return 0;
    }
    GuestSurface* gs = m->surfaces().find(arena);
    // ★ 两个 D3DPRESENT_PARAMETERS 字段今天**被丢弃**，而它们决定后缓冲能不能锁：
    //   · Flags & D3DPRESENTFLAG_LOCKABLE_BACKBUFFER（本安装实测 = 1，`0x498F1B/0x498F98`）
    //   · MultiSampleType != NONE ⇒ 多重采样后缓冲**不能**锁（D3D9 文档明说）
    //   落点只能是**像素块**（`GuestSurface`）：`'D9D1'` 64 B 全满、`'TEX1'` 的锁字段会踩别人的。
    if (gs != nullptr) {
        gs->d3d9_lockable = (req.present.flags & kPresentFlagLockableBackbuffer) != 0;
        gs->d3d9_multisample = req.present.multi_sample_type;
    }
    // ③ 状态块（guest 堆；EA 挂在对象 obj+8 的扩展位上）
    const std::uint32_t state = cpu.heap->alloc(kDvBytes, /*zero=*/true);
    if (state == 0) {
        m->surfaces().destroy(arena);
        if (why) *why = std::string("D3D9：guest 堆分配设备状态块失败：") + cpu.heap->lastError();
        return 0;
    }
    cpu.mem->wr32(state + kDvMagic, kDvMagicValue);
    cpu.mem->wr32(state + kDvArena, arena);
    cpu.mem->wr32(state + kDvWidth, static_cast<std::uint32_t>(width));
    cpu.mem->wr32(state + kDvHeight, static_cast<std::uint32_t>(height));
    cpu.mem->wr32(state + kDvFormat, format);
    cpu.mem->wr32(state + kDvBpp, bpp);
    cpu.mem->wr32(state + kDvPitch, gs ? gs->pitch : 0u);
    cpu.mem->wr32(state + kDvDeviceType, req.device_type);
    cpu.mem->wr32(state + kDvBehavior, req.behavior_flags);
    cpu.mem->wr32(state + kDvHwnd, req.hfocus_window);
    cpu.mem->wr32(state + kDvWindowed, req.present.windowed ? 1u : 0u);
    cpu.mem->wr32(state + kDvAutoDepthFormat, req.present.auto_depth_stencil_format);
    // ★ 流水线状态机（fixed-function：变换 / 渲染状态 / 光栅 / 深度缓冲）—— 追加式挂在这里，
    //   与设备状态块同一生命周期（Release 到 0 时由 destroyDeviceState 一并回收）。
    if (d3d9::createForDevice(cpu, state, width, height, why) == 0) {
        cpu.mem->wr32(state + kDvMagic, 0u);
        m->surfaces().destroy(arena);
        cpu.heap->free(state);
        return 0;
    }
    d3d9Log("CreateDevice",
            "后缓冲 %dx%d D3DFORMAT=%u（%ubpp，pitch=%u）· device_type=%u · behavior=0x%X · hFocusWindow=0x%08X "
            "· windowed=%d · 像素块 0x%08X · 状态块 0x%08X",
            width, height, format, bpp, gs ? gs->pitch : 0u, req.device_type, req.behavior_flags,
            req.hfocus_window, req.present.windowed ? 1 : 0, arena, state);
    return state;
}

bool deviceInfoOfState(X86& cpu, std::uint32_t state, DeviceInfo* out, std::string* why) {
    if (state == 0) {
        if (why) *why = "D3D9：设备状态块 EA 为 0";
        return false;
    }
    if (!cpu.mem->committed(state, kDvBytes)) {
        if (why) *why = "D3D9：设备状态块未提交（野指针？）";
        return false;
    }
    const std::uint32_t magic = cpu.mem->rd32(state + kDvMagic);
    if (magic != kDvMagicValue) {
        char buf[160];
        std::snprintf(buf, sizeof(buf), "D3D9：设备状态块 0x%08X 的 magic 是 0x%08X（应为 'D9D1'）", state, magic);
        if (why) *why = buf;
        return false;
    }
    DeviceInfo d;
    d.state = state;
    d.object = cpu.mem->rd32(state + kDeviceStateObjectOff);
    d.arena = cpu.mem->rd32(state + kDvArena);
    d.pipeline = cpu.mem->rd32(state + 56u);  // 流水线状态块（'DL01'；见 d3d9_pipeline.h）
    d.width = static_cast<int>(cpu.mem->rd32(state + kDvWidth));
    d.height = static_cast<int>(cpu.mem->rd32(state + kDvHeight));
    d.format = cpu.mem->rd32(state + kDvFormat);
    d.bpp = cpu.mem->rd32(state + kDvBpp);
    d.pitch = cpu.mem->rd32(state + kDvPitch);
    d.device_type = cpu.mem->rd32(state + kDvDeviceType);
    d.behavior = cpu.mem->rd32(state + kDvBehavior);
    d.hwnd = cpu.mem->rd32(state + kDvHwnd);
    d.windowed = cpu.mem->rd32(state + kDvWindowed) != 0;
    d.auto_depth_format = cpu.mem->rd32(state + kDvAutoDepthFormat);
    d.presents = cpu.mem->rd32(state + kDvPresents);
    d.clears = cpu.mem->rd32(state + kDvClears);
    *out = d;
    return true;
}

bool deviceInfo(X86& cpu, std::uint32_t object, DeviceInfo* out, std::string* why) {
    if (!cpu.com) {
        if (why) *why = "D3D9：cpu.com 为空（不是 COM 对象）";
        return false;
    }
    const std::uint32_t state = cpu.com->userOf(object);
    if (state == 0) {
        char buf[160];
        std::snprintf(buf, sizeof(buf), "D3D9：0x%08X 不是本模块建的设备对象（obj+8 没有状态块）", object);
        if (why) *why = buf;
        return false;
    }
    return deviceInfoOfState(cpu, state, out, why);
}

void destroyDeviceState(X86& cpu, std::uint32_t state) {
    if (state == 0 || !cpu.mem || !cpu.mem->committed(state, kDvBytes)) return;
    if (cpu.mem->rd32(state + kDvMagic) != kDvMagicValue) return;
    // ★ 先取 owner 再动 magic（本单）：清 magic 与 free 状态块都是不可逆的，判据只能在此之前成立。
    //   用 `machineOf`（装配时捕获的归属）而不是 `activeMachine()`（会被别的 Machine / 析构改写）。
    Machine* m = machineOf(cpu);
    // ★ 设备销毁：GPU 队列与镜像指向即将释放的 guest 像素 ⇒ 丢掉（⛔ 不回读到已释放/复用的 arena）。
    //   放在 destroyForDevice 之前：它可能 release 掉"当 RT 用的纹理"（其像素与镜像目标相关）。
    //   `rasterDiscardTarget()` 只碰宿主态（队列 / staging / 缓存），不读 guest ⇒ 顺序安全。
    if (m != nullptr) {
        if (GpuRaster* gpu = m->gpuRaster(); gpu != nullptr) gpu->rasterDiscardTarget();
    }
    d3d9::destroyForDevice(cpu, state);  // 流水线状态块 + 深度缓冲（含 SetTexture AddRef 的纹理）
    const std::uint32_t arena = cpu.mem->rd32(state + kDvArena);
    cpu.mem->wr32(state + kDvMagic, 0);
    if (m != nullptr && arena != 0) m->surfaces().destroy(arena);
    if (cpu.heap) cpu.heap->free(state);
}

// ---------------------------------------------------------------------------------------
// D3DCAPS9
// ---------------------------------------------------------------------------------------
std::uint32_t fillCaps(X86& cpu, std::uint32_t caps_ea, std::string* why) {
    if (!committedOut(cpu, caps_ea, kCapsBytes, "D3DCAPS9", why)) return kInvalidCall;
    // ① 先整块清零：引擎按 352 B 栈缓冲读，残留字节会变成随机能力（⛔ 不留未初始化）
    cpu.mem->fillBytes(caps_ea, 0, kCapsBytes);
    // ② 再把**有意义的**字段写成确定值
    cpu.mem->wr32(caps_ea + kCapsDeviceType, kDevTypeHal);
    cpu.mem->wr32(caps_ea + kCapsAdapterOrdinal, 0u);
    cpu.mem->wr32(caps_ea + kCapsPresentationIntervals, 0x80000001u);  // IMMEDIATE | ONE
    cpu.mem->wr32(caps_ea + kCapsCursorCaps, 0x00000001u);             // D3DCURSORCAPS_COLOR
    // 固定功能流水线可用（本线不实现绘制，但**能力**这层不谎称没有硬件光栅化）
    cpu.mem->wr32(caps_ea + kCapsDevCaps, 0x00080250u);
    cpu.mem->wr32(caps_ea + kCapsRasterCaps, 0x00200000u);  // D3DPRASTERCAPS_ZTEST
    cpu.mem->wr32(caps_ea + kCapsZCmpCaps, 0x000000FFu);
    cpu.mem->wr32(caps_ea + kCapsSrcBlendCaps, 0x000003FFu);
    cpu.mem->wr32(caps_ea + kCapsDestBlendCaps, 0x000003FFu);
    cpu.mem->wr32(caps_ea + kCapsAlphaCmpCaps, 0x000000FFu);
    cpu.mem->wr32(caps_ea + kCapsShadeCaps, 0x00084208u);
    cpu.mem->wr32(caps_ea + kCapsTextureCaps, 0x00010000u);
    cpu.mem->wr32(caps_ea + kCapsTextureFilterCaps, 0x03030303u);
    cpu.mem->wr32(caps_ea + kCapsTextureAddressCaps, 0x0000001Fu);
    cpu.mem->wr32(caps_ea + kCapsLineCaps, 0x0000001Fu);
    // ★ 引擎唯一读的两个字段（0x499128 / 0x49912E）
    cpu.mem->wr32(caps_ea + kCapsMaxTextureWidth, kMaxTextureWidth);
    cpu.mem->wr32(caps_ea + kCapsMaxTextureHeight, kMaxTextureHeight);
    cpu.mem->wr32(caps_ea + kCapsMaxVolumeExtent, 256u);
    cpu.mem->wr32(caps_ea + kCapsMaxTextureRepeat, 8192u);
    cpu.mem->wr32(caps_ea + kCapsMaxTextureAspectRatio, kMaxTextureWidth);
    cpu.mem->wr32(caps_ea + kCapsMaxAnisotropy, 4u);
    wrFloat(cpu, caps_ea + kCapsMaxVertexW, 1.0e10f);
    cpu.mem->wr32(caps_ea + kCapsStencilCaps, 0x000001FFu);
    cpu.mem->wr32(caps_ea + kCapsFVFCaps, 0x00000008u);   // 8 个纹理坐标
    cpu.mem->wr32(caps_ea + kCapsTextureOpCaps, 0x02000000u);
    cpu.mem->wr32(caps_ea + kCapsMaxTextureBlendStages, 8u);
    cpu.mem->wr32(caps_ea + kCapsMaxSimultaneousTextures, 8u);
    cpu.mem->wr32(caps_ea + kCapsVertexProcessingCaps, 0x0000003Fu);
    cpu.mem->wr32(caps_ea + kCapsMaxActiveLights, 8u);
    cpu.mem->wr32(caps_ea + kCapsMaxUserClipPlanes, 6u);
    cpu.mem->wr32(caps_ea + kCapsMaxVertexBlendMatrices, 4u);
    cpu.mem->wr32(caps_ea + kCapsMaxVertexBlendMatrixIndex, 0u);
    wrFloat(cpu, caps_ea + kCapsMaxPointSize, 64.0f);
    cpu.mem->wr32(caps_ea + kCapsMaxPrimitiveCount, 0xFFFFFu);
    cpu.mem->wr32(caps_ea + kCapsMaxVertexIndex, 0xFFFFFu);
    cpu.mem->wr32(caps_ea + kCapsMaxStreams, 16u);
    cpu.mem->wr32(caps_ea + kCapsMaxStreamStride, 255u);
    // ★ 本线**没有 shader** ⇒ 两个版本号与常量数报 0（⛔ 不谎称 vs_3_0 让引擎走进没实现的路径）
    cpu.mem->wr32(caps_ea + kCapsVertexShaderVersion, 0u);
    cpu.mem->wr32(caps_ea + kCapsMaxVertexShaderConst, 0u);
    cpu.mem->wr32(caps_ea + kCapsPixelShaderVersion, 0u);
    wrFloat(cpu, caps_ea + kCapsMaxPixelShaderValue, 0.0f);
    d3d9Log("GetDeviceCaps", "D3DCAPS9（%u B）= 0 ⇒ MaxTextureWidth=%u MaxTextureHeight=%u（shader 版本报 0）",
            kCapsBytes, kMaxTextureWidth, kMaxTextureHeight);
    return kOk;
}

// ---------------------------------------------------------------------------------------
// CheckDeviceFormat 诊断记录
// ---------------------------------------------------------------------------------------
void resetCheckFormatQueries() {
    g_check_format_count = 0;
    for (CheckFormatQuery& q : g_check_format_log) q = CheckFormatQuery{};
}
void recordCheckFormatQuery(const CheckFormatQuery& q) {
    g_check_format_log[g_check_format_count % kCheckFormatLogCapacity] = q;
    ++g_check_format_count;
}
std::size_t checkFormatQueryCount() { return g_check_format_count; }
bool checkFormatQueryAt(std::size_t newest, CheckFormatQuery* out) {
    if (newest >= g_check_format_count || newest >= kCheckFormatLogCapacity) return false;
    *out = g_check_format_log[(g_check_format_count - 1 - newest) % kCheckFormatLogCapacity];
    return true;
}

// ---------------------------------------------------------------------------------------
// 渲染动作
// ---------------------------------------------------------------------------------------
// ★ 实现在 runtime/pixel_format（唯一一份掩码换算）；这里只留**薄转发**，既有 API 不变。
std::uint32_t pixelFromColor(const SurfaceFormat& fmt, std::uint32_t argb) {
    return pixel_format::pixelFromColor(fmt, argb);
}

// ★ 诊断（见 `frameSeq()` 的注释）：清过后缓冲的次数 = 帧序号。只读、不参与语义、不进基线。
static std::uint32_t g_frame_seq = 0;

std::uint32_t frameSeq() { return g_frame_seq; }

std::uint32_t clear(X86& cpu, const DeviceInfo& dev, std::uint32_t count, std::uint32_t rects_ea,
                    std::uint32_t flags, std::uint32_t color, float z, std::uint32_t stencil,
                    std::string* why) {
    // ★ 诊断计数：清后缓冲 = 新的一帧（见 `frameSeq()` 的注释）。只读、不参与语义、不进基线。
    if ((flags & kClearTarget) != 0) ++g_frame_seq;
    Machine* m = nullptr;
    if (!requireMachine(cpu, &m, why)) return kFail;
    const std::uint32_t allowed = kClearTarget | kClearZBuffer | kClearStencil;
    if ((flags & ~allowed) != 0) {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "D3D9：Clear 的 Flags=0x%08X 含未定义的位（只认 TARGET/ZBUFFER/STENCIL）",
                      flags);
        if (why) *why = buf;
        return kInvalidCall;
    }
    // ★ 深度清必须**真的落到 GPU**：D3D9 的深度缓冲在 guest 内存里只有一份 CPU 镜像
    //   （`clearDepth` 写它），宿主的 D32 是另一块纹理。这里把"清深度"的请求交给后端，
    //   由它在下一个 render pass 用 CLEAR 落地 —— ⛔ 不要指望"颜色同步顺带清"。
    //   ★ 放在 TARGET 位的早退之前：`Clear(0,NULL,D3DCLEAR_ZBUFFER,..)` 也要生效。
    if ((flags & kClearZBuffer) != 0) {
        if (GpuRaster* gpu = m->gpuRaster(); gpu != nullptr && gpu->rasterActive()) gpu->rasterRequestDepthClear();
    }
    const std::uint32_t color_arena = dev.pipeline != 0 ? renderTargetArena(cpu, dev.pipeline) : 0u;
    GuestSurface* gs = m->surfaces().find(color_arena != 0 ? color_arena : dev.arena);
    if (!gs) {
        if (why) *why = color_arena != 0 ? "D3D9：Clear 的渲染目标像素不在 arena 里"
                                         : "D3D9：Clear 的后缓冲像素不在 arena 里（设备状态与 arena 不一致）";
        return kInvalidCall;
    }
    if ((flags & kClearTarget) == 0) {
        // 真机：没有 TARGET 位 ⇒ 不动颜色（S_OK）。也要出声，否则"什么都没清"会被当成清了。
        d3d9Log("Clear", "Flags=0x%08X 没有 D3DCLEAR_TARGET ⇒ 颜色保持不变（z=%.3f stencil=%u）", flags,
                static_cast<double>(z), stencil);
        cpu.mem->wr32(dev.state + kDvClears, dev.clears + 1u);
        return kOk;
    }
    // GPU 光栅化：CPU 即将改写 guest 像素，先把 GPU 未读回的绘制落回 guest。
    // ★ 合批后 `rasterDirty()` 含"队列非空" ⇒ 这条屏障同时完成 flush + 读回。
    // ★ 但只有**这块 guest 像素就是当前镜像的目标**时才回读：同尺寸纹理当 RT 时，把后缓冲镜像
    //   写进纹理是错的（⛔ 静默写错 surface）。不是目标就只 flush，让队列落到它自己的 target。
    if (GpuRaster* gpu = m->gpuRaster(); gpu != nullptr && gpu->rasterActive() && gpu->rasterDirty()) {
        std::string gerr;
        std::uint8_t* base = m->surfaces().pixels(*gs);
        if (gpu->rasterTargetBase() != nullptr && gpu->rasterTargetBase() != base) {
            if (!gpu->rasterFlush(&gerr)) d3d9Log("Clear", "GPU 队列 flush 失败：%s", gerr.c_str());
        } else if (!gpu->rasterSyncToGuest(base, gs->pitch, gs->width, gs->height, &gerr)) {
            d3d9Log("Clear", "GPU 光栅化读回失败：%s", gerr.c_str());
        }
    }
    const std::uint32_t pixel = pixelFromColor(gs->format, color);
    const std::uint32_t bytes_per_pixel = gs->format.bytesPerPixel();
    std::uint8_t* base = m->surfaces().pixels(*gs);
    if (!base || bytes_per_pixel == 0) {
        if (why) *why = "D3D9：Clear 的后缓冲像素指针为空";
        return kFail;
    }
    // 目标矩形：Count==0 ⇒ 整块；否则读 Count 个 D3DRECT（4 个 LONG = 16 B）
    std::uint32_t cleared_pixels = 0;
    auto clear_area = [&](int x1, int y1, int x2, int y2) {
        if (x1 < 0) x1 = 0;
        if (y1 < 0) y1 = 0;
        if (x2 > gs->width) x2 = gs->width;
        if (y2 > gs->height) y2 = gs->height;
        if (x2 <= x1 || y2 <= y1) return;
        for (int y = y1; y < y2; ++y) {
            for (int x = x1; x < x2; ++x) {
                std::uint8_t* p = base + static_cast<std::size_t>(y) * gs->pitch +
                                  static_cast<std::size_t>(x) * bytes_per_pixel;
                ddraw::storePixel(p, gs->format.bpp, pixel);
            }
        }
        cleared_pixels += static_cast<std::uint32_t>((x2 - x1) * (y2 - y1));
    };
    if (count == 0) {
        clear_area(0, 0, gs->width, gs->height);
    } else {
        const std::size_t need = static_cast<std::size_t>(count) * 16u;
        if (!committedOut(cpu, rects_ea, need, "Clear 的 pRects 表", why)) return kInvalidCall;
        for (std::uint32_t i = 0; i < count; ++i) {
            const std::uint32_t ea = rects_ea + i * 16u;
            const int x1 = static_cast<int>(cpu.mem->rd32(ea + 0));
            const int y1 = static_cast<int>(cpu.mem->rd32(ea + 4));
            const int x2 = static_cast<int>(cpu.mem->rd32(ea + 8));
            const int y2 = static_cast<int>(cpu.mem->rd32(ea + 12));
            if (x2 <= x1 || y2 <= y1) {
                d3d9Log("Clear", "第 %u 个 D3DRECT 是空矩形（%d,%d,%d,%d）⇒ 跳过", i, x1, y1, x2, y2);
                continue;
            }
            clear_area(x1, y1, x2, y2);
        }
    }
    cpu.mem->wr32(dev.state + kDvClears, dev.clears + 1u);
    if ((flags & kClearZBuffer) != 0) {
        // ★ 2026-11-05：fixed-function 流水线落地后**有**深度缓冲了 ⇒ 真的清（不再是"只记录"）
        if (dev.pipeline != 0) {
            std::string zwhy;
            if (!clearDepth(cpu, dev.pipeline, z, &zwhy)) {
                d3d9Log("Clear", "ZBUFFER 位在，但深度清失败：%s", zwhy.c_str());
            } else {
                d3d9Log("Clear", "ZBUFFER 位 ⇒ 深度缓冲清成 z=%.6f", static_cast<double>(z));
            }
        } else {
            d3d9Log("Clear", "ZBUFFER 位在，但这个设备没有流水线状态块 ⇒ 深度没清（⛔ 不谎称清了）");
        }
    }
    if ((flags & kClearStencil) != 0) {
        // 本线不建模板缓冲（D3DPRESENT_PARAMETERS 的 AutoDepthStencilFormat 只记录）
        d3d9Log("Clear", "STENCIL 位在，但本线没有模板缓冲 ⇒ 只记录（⛔ 不谎称清了，stencil=%u）", stencil);
    }
    d3d9Log("Clear", "清 %u 个像素为 D3DCOLOR=0x%08X（像素值 0x%08X，%ubpp，%dx%d）", cleared_pixels, color, pixel,
            gs->format.bpp, gs->width, gs->height);
    return kOk;
}

std::uint32_t present(X86& cpu, const DeviceInfo& dev, std::string* why) {
    Machine* m = nullptr;
    if (!requireMachine(cpu, &m, why)) return kFail;
    GuestSurface* gs = m->surfaces().find(dev.arena);
    if (!gs) {
        if (why) *why = "D3D9：Present 的后缓冲像素不在 arena 里（设备状态与 arena 不一致）";
        return kInvalidCall;
    }
    // ★ 帧导出（宿主诊断）：路径里的 `{frame}` 换成**即将呈现**的帧号（4 位）。
    //   不含占位符时每次覆盖同一文件；配合 --max-frames 1 就能稳定拿第一帧。
    std::string dump_path = cpu.shim.frame_dump;
    if (!dump_path.empty()) {
        char frame_buf[16];
        std::snprintf(frame_buf, sizeof(frame_buf), "%04llu",
                      static_cast<unsigned long long>(m->presenter().frameCount() + 1u));
        const std::string token = "{frame}";
        for (std::size_t pos = dump_path.find(token); pos != std::string::npos;
             pos = dump_path.find(token, pos + std::strlen(frame_buf))) {
            dump_path.replace(pos, token.size(), frame_buf);
        }
    }
    std::string err;
    // ★ GPU 光栅化 + 呈现。两种情形分清（决定窗口能不能直接用 GPU target 的像素）：
    //   ① `dirty` = target 里有尚未落回 guest 的绘制 ⇒ target 才是最新的。
    //      先读回 guest（hash / frameStats / --dump-frame / 后缓冲语义都读 guest，⛔ 不砍），
    //      读回后 target == guest，直出可以直接 blit。
    //   ② `!dirty` = 本帧没有 GPU 绘制（`Clear` / CPU 回退 / sprite 的 CPU 路径都先同步过）
    //      ⇒ **guest 才是最新的**，target 可能是上一帧 ⇒ 由呈现汇点在 blit 之前把 guest 上传
    //      （`directSetTargetCurrent(false)`；见 present_sdl.cpp 的 submitDirect）。
    PresentBreakdown& pb = presentBreakdown();
    pb = PresentBreakdown{};
    GpuRaster* gpu = m->gpuRaster();
    const bool gpu_active = gpu != nullptr && gpu->rasterActive();
    GpuDirectPresenter* direct = gpu_active ? gpu->directPresenter() : nullptr;
    const bool direct_active = direct != nullptr && direct->directActive();
    bool gpu_dirty = gpu_active && gpu->rasterDirty();
    pb.direct = direct_active;
    const std::uint64_t readback_begin = steadyNowNs();
    if (gpu_dirty) {
        std::string gerr;
        std::uint8_t* back = m->surfaces().pixels(*gs);
        // ★ 镜像身份必须是后缓冲：同尺寸纹理当 RT 且没切回来时，把镜像读进后缓冲是错的。
        //   这种情况下先 flush（队列落到它自己的 target），并按"guest 才是后缓冲最新内容"处理。
        if (gpu->rasterTargetBase() != nullptr && gpu->rasterTargetBase() != back) {
            if (!gpu->rasterFlush(&gerr)) d3d9Log("Present", "GPU 队列 flush 失败：%s", gerr.c_str());
            gpu_dirty = false;
        } else if (!gpu->rasterSyncToGuest(back, gs->pitch, gs->width, gs->height, &gerr)) {
            d3d9Log("Present", "GPU 光栅化读回失败：%s", gerr.c_str());
        }
    }
    pb.readback_ns = steadyNowNs() - readback_begin;
    // ★ 告诉呈现汇点"本帧的像素在哪儿"：
    //   · 刚读过回 ⇒ target == guest（target 就是最新内容）⇒ 直出可以直接 blit；
    //   · 没读过回（本帧没有 GPU 绘制）⇒ target 可能是上一帧 ⇒ 由 sink 在呈现前把 guest 上传。
    //   ⛔ 这是"直出"唯一可能静默出错的点，所以状态显式传给 sink，而不是让 sink 猜。
    if (direct_active) direct->directSetTargetCurrent(gpu_dirty);
    if (!m->presenter().present(m->surfaces(), *gs, dump_path, &err)) {
        if (why) *why = "D3D9：HeadlessPresenter.present 失败：" + err;
        return kFail;
    }
    // ★ 诊断（`LIFT_GPU_SUBMITS=1`，缺省关、零成本）：每帧的 GPU 提交/切段/上传计数。
    //   "提交次数"是合批改造的判据 ⇒ 把它变成可观测数字，⛔ 不靠"从 End 次数推导"。
    //   记在 present 之后 ⇒ 本帧的 delta 含"画的那一次 + 读回那一次 + 直出那一次"。
    if (gpu_active && std::getenv("LIFT_GPU_SUBMITS") != nullptr) {
        static std::uint64_t last_submits = 0;
        static std::uint64_t last_flush = 0;
        static std::uint64_t last_readback = 0;
        static std::uint64_t last_upload = 0;
        static std::uint64_t last_present = 0;
        static std::uint64_t last_cuts = 0;
        const GpuRasterStats now = gpuRasterStats();
        std::fprintf(stderr,
                     "[gpu-submits] frame=%llu submits=%llu (flush=%llu readback=%llu upload=%llu "
                     "present=%llu) cuts=%llu uploads=%llu full=%llu\n",
                     static_cast<unsigned long long>(m->presenter().frameCount()),
                     static_cast<unsigned long long>(now.submits - last_submits),
                     static_cast<unsigned long long>(now.submits_flush - last_flush),
                     static_cast<unsigned long long>(now.submits_readback - last_readback),
                     static_cast<unsigned long long>(now.submits_upload - last_upload),
                     static_cast<unsigned long long>(now.submits_present - last_present),
                     static_cast<unsigned long long>(now.cuts - last_cuts),
                     static_cast<unsigned long long>(now.uploads),
                     static_cast<unsigned long long>(now.full_uploads));
        last_submits = now.submits;
        last_flush = now.submits_flush;
        last_readback = now.submits_readback;
        last_upload = now.submits_upload;
        last_present = now.submits_present;
        last_cuts = now.cuts;
    }
    if (!dump_path.empty()) d3d9Log("Present", "帧 BMP 已写入 %s", dump_path.c_str());
    cpu.mem->wr32(dev.state + kDvPresents, dev.presents + 1u);
    // ★ 第一帧的**像素判据**：后缓冲里"非背景像素数"（背景 = 出现次数最多的那个像素值）。
    //   ⛔ 不是猜，也不是"看截图"：直接数 guest 后缓冲的像素（确定性；同一份输入两次跑同值）。
    // ★ 这个直方图只喂下面那条 **Info** 日志（缺省 `warn` 级别根本看不到它），但它是逐像素
    //   `std::map` ⇒ 1280×720 的 TITLE 帧实测 ~54 ms（比整条呈现路径还贵）。
    //   ⇒ 按**日志可见性**门控：`--log-level info` / `--verbose` 时算法与输出逐字不变，
    //   默认级别下不再为一个看不见的行付一次全帧直方图。⛔ 这不是"删功能"，也不是跳过 guest 语义
    //   （hash / frameCount / dump 都在别处，照旧每帧算）。
    if (logVisible(LogLevel::Info, LogD3d9)) {
        FrameStats st;
        const std::uint64_t stats_begin = steadyNowNs();
        const bool stats_ok =
            frameStats(gs->format, m->surfaces().pixels(*gs), gs->pitch, gs->width, gs->height, &st);
        presentBreakdown().stats_ns = steadyNowNs() - stats_begin;
        if (stats_ok) {
            d3d9Log("Present",
                    "第 %llu 帧 hash=0x%016llX（%dx%d %ubpp）· 背景像素值 0x%08X（%u 个）· 非背景 %u / %u · "
                    "不同颜色 %u · 像素哈希 0x%016llX",
                    static_cast<unsigned long long>(m->presenter().frameCount()),
                    static_cast<unsigned long long>(m->presenter().lastHash()), dev.width, dev.height, dev.bpp,
                    st.background_color, st.background_pixels, st.non_background_pixels, st.total_pixels,
                    st.distinct_colors, static_cast<unsigned long long>(st.hash));
        } else {
            d3d9Log("Present", "第 %llu 帧 hash=0x%016llX（像素统计失败：后缓冲指针/尺寸非法）",
                    static_cast<unsigned long long>(m->presenter().frameCount()),
                    static_cast<unsigned long long>(m->presenter().lastHash()));
        }
    }
    if (cpu.shim.max_frames != 0 && m->presenter().frameCount() >= cpu.shim.max_frames) {
        // 帧数上限是宿主侧控制；停止原因走既有的 StopReason::Stopped。
        cpu.running = false;
        cpu.stop_why = "max-frames";
        d3d9Log("Present", "已到 --max-frames %u ⇒ 主动停", cpu.shim.max_frames);
    }
    return kOk;
}

// ---------------------------------------------------------------------------------------
// IDirect3DTexture9（追加式；M2 纹理簇）
// ---------------------------------------------------------------------------------------
namespace {

// 纹理状态块布局（**我们自己的**；⛔ 不是 Win32 结构体）
enum TexOff : std::uint32_t {
    kTxMagic = 0,   // 'TEX1'
    kTxDevice = 4,  // IDirect3DDevice9 对象 EA
    kTxArena = 8,   // 像素块（SurfaceArena）
    kTxWidth = 12,
    kTxHeight = 16,
    kTxFormat = 20,
    kTxBpp = 24,
    kTxPitch = 28,
    kTxLevels = 32,
    kTxUsage = 36,
    kTxPool = 40,
    kTxLocks = 44,
    kTxLocked = 48,
    // ★ 内容版本（GPU 纹理上传缓存）：0 = 未知；非 0 = 已知内容的**单调序列号**。
    //   112 行附近的 `TextureInfo::version` 注释是口径真源；这里只放布局。
    kTxVersion = 52,
    kTxVersionSeq = 56,  // 下一个可发的序列号（⛔ 永不复用已发过的号；0 保留给"未知"）
    kTxSerial = 60,      // 进程级单调纹理身份（GPU 缓存键；⛔ 不复用 guest 指针/状态块 EA）
    // ★ 内容写入区域（方案 C 的脏矩形判据）：最近一次 UnlockRect 的写入矩形 + 版本链。
    kTxWriteX0 = 64,
    kTxWriteY0 = 68,
    kTxWriteX1 = 72,
    kTxWriteY1 = 76,
    kTxWriteVersion = 80,      // 这次写的版本号（0 = 写区不可信：没解锁 / RT 降级）
    kTxPrevWriteVersion = 84,  // 上一次写的版本号（0 = 没有）
    kTxPendingX0 = 88,         // 本次 LockRect 的矩形（解锁时搬到 kTxWrite*）
    kTxPendingY0 = 92,
    kTxPendingX1 = 96,
    kTxPendingY1 = 100,
    kTxBytes = 104,
};
constexpr std::uint32_t kTxMagicValue = 0x31584554u;  // 'TEX1'

}  // namespace

bool surfaceFormatForD3DFormat(std::uint32_t format, SurfaceFormat* out) {
    return formatForD3dFormat(format, out);
}

std::uint32_t createTextureState(X86& cpu, std::uint32_t device_object, std::uint32_t width,
                                 std::uint32_t height, std::uint32_t levels, std::uint32_t usage,
                                 std::uint32_t format, std::uint32_t pool, std::string* why,
                                 bool* out_of_memory) {
    if (out_of_memory) *out_of_memory = false;
    Machine* m = nullptr;
    if (!requireMachine(cpu, &m, why)) return 0;
    if (!cpu.heap || !cpu.heap->ready()) {
        if (why) *why = "D3D9：guest 堆未就绪（纹理状态块要放在 guest 堆上）";
        return 0;
    }
    DeviceInfo dev;
    if (!deviceInfo(cpu, device_object, &dev, why)) {
        if (why) *why = std::string("D3D9：建纹理的设备不是本模块的 IDirect3DDevice9：") + *why;
        return 0;
    }
    SurfaceFormat fmt;
    if (!formatForD3dFormat(format, &fmt)) {
        char buf[160];
        std::snprintf(buf, sizeof(buf),
                      "D3D9：纹理格式 D3DFORMAT=%u 不在本线支持集（21/22 的 32bpp、23/24/25/26 的 16bpp）",
                      format);
        if (why) *why = buf;
        return 0;
    }
    if (width == 0 || height == 0 || width > kMaxTextureWidth || height > kMaxTextureHeight) {
        char buf[160];
        std::snprintf(buf, sizeof(buf), "D3D9：纹理尺寸非法（%ux%u；上限 %u）", width, height, kMaxTextureWidth);
        if (why) *why = buf;
        return 0;
    }
    if (levels != 1u) {
        // 本线不生成 mip 链：D3DXCreateTexture 传 0（= 全链）或 >1 都会到这里。
        // ⛔ 不假装有 mip：明确拒绝（真机会建出链；本线只要 level 0）。
        char buf[160];
        std::snprintf(buf, sizeof(buf), "D3D9：本线只建 level 0，MipLevels=%u 不支持（⛔ 不假装有 mip 链）",
                      levels);
        if (why) *why = buf;
        return 0;
    }
    const std::uint32_t arena = m->surfaces().create(static_cast<int>(width), static_cast<int>(height), fmt);
    if (arena == 0) {
        // ★ 资源耗尽（不是"这条导出没实现"）：调用方必须报 E_OUTOFMEMORY / 独立措辞。
        if (out_of_memory) *out_of_memory = true;
        if (why) *why = std::string("D3D9：SurfaceArena.create 失败：") + m->surfaces().lastError();
        return 0;
    }
    const GuestSurface* gs = m->surfaces().find(arena);
    const std::uint32_t state = cpu.heap->alloc(kTxBytes, /*zero=*/true);
    if (state == 0) {
        if (out_of_memory) *out_of_memory = true;
        m->surfaces().destroy(arena);
        if (why) *why = std::string("D3D9：guest 堆分配纹理状态块失败：") + cpu.heap->lastError();
        return 0;
    }
    cpu.mem->wr32(state + kTxMagic, kTxMagicValue);
    cpu.mem->wr32(state + kTxDevice, device_object);
    cpu.mem->wr32(state + kTxArena, arena);
    cpu.mem->wr32(state + kTxWidth, width);
    cpu.mem->wr32(state + kTxHeight, height);
    cpu.mem->wr32(state + kTxFormat, format);
    cpu.mem->wr32(state + kTxBpp, fmt.bpp);
    cpu.mem->wr32(state + kTxPitch, gs ? gs->pitch : 0u);
    cpu.mem->wr32(state + kTxLevels, levels);
    cpu.mem->wr32(state + kTxUsage, usage);
    cpu.mem->wr32(state + kTxPool, pool);
    // ★ 进程级单调纹理身份：⛔ 不用指针 / 状态块 EA（arena 与 guest 堆都会复用）。
    static std::uint32_t next_texture_serial = 0;
    std::uint32_t serial = ++next_texture_serial;
    if (serial == 0) serial = ++next_texture_serial;  // 跳过 0（0 = "没有身份"）
    cpu.mem->wr32(state + kTxSerial, serial);
    // ★ 内容版本：像素块是 `SurfaceArena::create` 分配的（`GuestHeap::alloc(bytes, zero=true)` ⇒
    //   整块清零，回收复用的块也一样）⇒ **建完的内容就是已知的全 0**，版本可以从 1 起。
    //   序列号从 2 起：1 已经用给初值，下一次"已知"写（UnlockRect）必须是一个**没出现过**的号。
    cpu.mem->wr32(state + kTxVersion, 1u);
    cpu.mem->wr32(state + kTxVersionSeq, 2u);
    d3d9Log("CreateTexture",
            "%ux%u D3DFORMAT=%u（%ubpp，pitch=%u）· MipLevels=%u · Usage=0x%X · Pool=%u · 设备 0x%08X"
            " ⇒ 像素块 0x%08X / 状态块 0x%08X",
            width, height, format, fmt.bpp, gs ? gs->pitch : 0u, levels, usage, pool, device_object, arena,
            state);
    return state;
}

bool textureInfoOfState(X86& cpu, std::uint32_t state, TextureInfo* out, std::string* why) {
    if (state == 0) {
        if (why) *why = "D3D9：纹理状态块 EA 为 0";
        return false;
    }
    if (!cpu.mem->committed(state, kTxBytes)) {
        if (why) *why = "D3D9：纹理状态块未提交（野指针？）";
        return false;
    }
    const std::uint32_t magic = cpu.mem->rd32(state + kTxMagic);
    if (magic != kTxMagicValue) {
        char buf[160];
        std::snprintf(buf, sizeof(buf), "D3D9：纹理状态块 0x%08X 的 magic 是 0x%08X（应为 'TEX1'）", state, magic);
        if (why) *why = buf;
        return false;
    }
    TextureInfo t;
    t.state = state;
    t.device = cpu.mem->rd32(state + kTxDevice);
    t.arena = cpu.mem->rd32(state + kTxArena);
    t.width = cpu.mem->rd32(state + kTxWidth);
    t.height = cpu.mem->rd32(state + kTxHeight);
    t.format = cpu.mem->rd32(state + kTxFormat);
    t.bpp = cpu.mem->rd32(state + kTxBpp);
    t.pitch = cpu.mem->rd32(state + kTxPitch);
    t.levels = cpu.mem->rd32(state + kTxLevels);
    t.usage = cpu.mem->rd32(state + kTxUsage);
    t.pool = cpu.mem->rd32(state + kTxPool);
    t.locks = cpu.mem->rd32(state + kTxLocks);
    t.locked = cpu.mem->rd32(state + kTxLocked) != 0;
    t.version = cpu.mem->rd32(state + kTxVersion);
    t.serial = cpu.mem->rd32(state + kTxSerial);
    t.write_version = cpu.mem->rd32(state + kTxWriteVersion);
    t.prev_write_version = cpu.mem->rd32(state + kTxPrevWriteVersion);
    for (int i = 0; i < 4; ++i) {
        t.write_rect[i] = static_cast<std::int32_t>(cpu.mem->rd32(state + kTxWriteX0 + static_cast<std::uint32_t>(i) * 4u));
    }
    *out = t;
    return true;
}

bool textureInfo(X86& cpu, std::uint32_t object, TextureInfo* out, std::string* why) {
    if (!cpu.com) {
        if (why) *why = "D3D9：cpu.com 为空（不是 COM 对象）";
        return false;
    }
    const std::uint32_t state = cpu.com->userOf(object);
    if (state == 0) {
        char buf[160];
        std::snprintf(buf, sizeof(buf), "D3D9：0x%08X 不是本模块建的纹理对象（obj+8 没有状态块）", object);
        if (why) *why = buf;
        return false;
    }
    return textureInfoOfState(cpu, state, out, why);
}

// 纹理像素抽样（**只读**；`tests/movie_logo.cpp` 的像素判据用）。
// 每 64 个像素取一个（1280×720 ⇒ 14400 个），只支持 4bpp（影片纹理 = A8R8G8B8）。
bool sampleTexture(X86& cpu, std::uint32_t object, TexturePixels* out) {
    if (out == nullptr) return false;
    *out = TexturePixels{};
    TextureInfo tex;
    if (!textureInfo(cpu, object, &tex, nullptr)) return false;
    // ★ `TextureInfo::bpp` 是**位**（A8R8G8B8 ⇒ 32）；`samplePixels` 要的是**字节/像素**。
    const std::uint32_t bytes_per_pixel = tex.bpp / 8u;
    if ((bytes_per_pixel != 4u && bytes_per_pixel != 2u) || tex.width == 0 || tex.height == 0 ||
        tex.arena == 0) {
        return false;
    }
    out->width = tex.width;
    out->height = tex.height;
    samplePixels(cpu, tex.arena, tex.width, tex.height, tex.pitch, bytes_per_pixel, &out->nonzero,
                 &out->sampled, &out->hash);
    return out->sampled != 0;
}

bool surfaceInfoOfState(X86& cpu, std::uint32_t state, TextureInfo* out, std::string* why) {
    TextureInfo tex;
    if (textureInfoOfState(cpu, state, &tex, nullptr)) {
        *out = tex;
        return true;
    }
    DeviceInfo dev;
    if (!deviceInfoOfState(cpu, state, &dev, why)) return false;
    TextureInfo t;
    t.state = state;
    t.device = dev.object;  // 后缓冲 surface 的 GetDevice 回答设备对象（+60）
    t.arena = dev.arena;
    t.width = static_cast<std::uint32_t>(dev.width);
    t.height = static_cast<std::uint32_t>(dev.height);
    t.format = dev.format;
    t.bpp = dev.bpp;
    t.pitch = dev.pitch;
    t.levels = 1;
    t.usage = 0;
    t.pool = 0;
    *out = t;
    return true;
}

void destroyTextureState(X86& cpu, std::uint32_t state) {
    if (state == 0 || !cpu.mem || !cpu.mem->committed(state, kTxBytes)) return;
    if (cpu.mem->rd32(state + kTxMagic) != kTxMagicValue) return;
    const std::uint32_t arena = cpu.mem->rd32(state + kTxArena);
    // ★ 先取 owner，**再**清 magic / free 状态块（本单 REQ-01M6X4MSCMMFEXYXCSK6V61QR2）：
    //   这两步不可逆（magic 清了就再进不了本函数、状态块会随堆复用），所以"该不该回收"的判据
    //   必须在它们之前成立。owner 来自 `Machine::wire()`，不受 `activeMachine()` 被改写的影响。
    Machine* m = machineOf(cpu);
    cpu.mem->wr32(state + kTxMagic, 0);
    if (m != nullptr && arena != 0) m->surfaces().destroy(arena);
    if (cpu.heap) cpu.heap->free(state);
}

bool writeTextureDesc(X86& cpu, const TextureInfo& tex, std::uint32_t level, std::uint32_t desc_ea,
                      std::string* why) {
    if (level != 0) {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "D3D9：GetLevelDesc 的 Level=%u（本线只有 level 0）", level);
        if (why) *why = buf;
        return false;
    }
    if (!committedOut(cpu, desc_ea, kTextureDescBytes, "D3DSURFACE_DESC", why)) return false;
    cpu.mem->fillBytes(desc_ea, 0, kTextureDescBytes);
    cpu.mem->wr32(desc_ea + kSdFormat, tex.format);
    cpu.mem->wr32(desc_ea + kSdType, kRTypeSurface);
    cpu.mem->wr32(desc_ea + kSdUsage, tex.usage);
    cpu.mem->wr32(desc_ea + kSdPool, tex.pool);
    cpu.mem->wr32(desc_ea + kSdMultiSampleType, 0u);
    cpu.mem->wr32(desc_ea + kSdMultiSampleQuality, 0u);
    cpu.mem->wr32(desc_ea + kSdWidth, tex.width);
    cpu.mem->wr32(desc_ea + kSdHeight, tex.height);
    return true;
}

bool lockRect(X86& cpu, const TextureInfo& tex, std::uint32_t level, std::uint32_t locked_rect_ea,
              std::uint32_t rect_ea, std::uint32_t flags, std::string* why) {
    if (level != 0) {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "D3D9：LockRect 的 Level=%u（本线只有 level 0）", level);
        if (why) *why = buf;
        return false;
    }
    if (tex.locked) {
        if (why) *why = "D3D9：LockRect 在已锁的 level 上（真机也是 D3DERR_INVALIDCALL）";
        return false;
    }
    if (!committedOut(cpu, locked_rect_ea, 8u, "D3DLOCKED_RECT", why)) return false;
    Machine* m = nullptr;
    if (!requireMachine(cpu, &m, why)) return false;
    GuestSurface* gs = m->surfaces().find(tex.arena);
    if (!gs) {
        if (why) *why = "D3D9：纹理像素不在 arena 里（纹理状态与 arena 不一致）";
        return false;
    }
    // ★ 屏障（屏障清单「LockRect」）：如果这块纹理正是当前 GPU 镜像的目标（纹理当 RT），
    //   guest 像素是过期的 ⇒ 先 flush + 读回。不是目标就不热路径（⛔ 不把别的 target 读进来）。
    if (GpuRaster* gpu = m->gpuRaster(); gpu != nullptr && gpu->rasterActive() && gpu->rasterDirty()) {
        std::uint8_t* base = m->surfaces().pixels(*gs);
        if (gpu->rasterTargetBase() != nullptr && gpu->rasterTargetBase() == base) {
            std::string sync_err;
            if (!gpu->rasterSyncToGuest(base, gs->pitch, gs->width, gs->height, &sync_err)) {
                if (why) *why = "D3D9：LockRect 前 GPU 像素读回失败：" + sync_err;
                return false;
            }
        }
    }
    const int bpp = static_cast<int>(gs->format.bytesPerPixel());
    int left = 0;
    int top = 0;
    int right = gs->width;
    int bottom = gs->height;
    if (rect_ea != 0) {
        if (!committedOut(cpu, rect_ea, 16u, "LockRect 的 RECT", why)) return false;
        left = static_cast<int>(cpu.mem->rd32(rect_ea + 0));
        top = static_cast<int>(cpu.mem->rd32(rect_ea + 4));
        right = static_cast<int>(cpu.mem->rd32(rect_ea + 8));
        bottom = static_cast<int>(cpu.mem->rd32(rect_ea + 12));
        if (left < 0 || top < 0 || right > gs->width || bottom > gs->height || right <= left || bottom <= top) {
            char buf[160];
            std::snprintf(buf, sizeof(buf), "D3D9：LockRect 的 RECT=(%d,%d,%d,%d) 越界 / 空（纹理 %dx%d）", left, top,
                          right, bottom, gs->width, gs->height);
            if (why) *why = buf;
            return false;
        }
    }
    const std::uint32_t bits = tex.arena + static_cast<std::uint32_t>(top) * gs->pitch +
                               static_cast<std::uint32_t>(left) * static_cast<std::uint32_t>(bpp);
    // D3DLOCKED_RECT = { INT Pitch; void* pBits; }（32 位进程 ⇒ 8 B）
    cpu.mem->wr32(locked_rect_ea + 0, gs->pitch);
    cpu.mem->wr32(locked_rect_ea + 4, bits);
    cpu.mem->wr32(tex.state + kTxLocks, tex.locks + 1u);
    cpu.mem->wr32(tex.state + kTxLocked, 1u);
    // ★ 记下本次锁允许写的矩形（GPU 后端只上传这块；`rect==NULL` = 整幅）。
    //   ⚠ 语义根据：D3D9 契约只允许 guest 写锁定矩形内的像素；矩形外直写是未定义行为。
    cpu.mem->wr32(tex.state + kTxPendingX0, static_cast<std::uint32_t>(left));
    cpu.mem->wr32(tex.state + kTxPendingY0, static_cast<std::uint32_t>(top));
    cpu.mem->wr32(tex.state + kTxPendingX1, static_cast<std::uint32_t>(right));
    cpu.mem->wr32(tex.state + kTxPendingY1, static_cast<std::uint32_t>(bottom));
    // ★ 锁期间内容 = "未知"：guest 拿到 pBits 之后可能随时改像素，而它**不一定**会 Unlock 再被采样。
    //   置 0 ⇒ 采样退回内容哈希（未优化前的行为）。⛔ 不要留旧版本号：那会让"锁内写"被缓存跳过。
    cpu.mem->wr32(tex.state + kTxVersion, 0u);
    if (flags != 0) {
        // DISCARD/NOOVERWRITE/READONLY 对"guest 内存里的像素"没有可观察差异 ⇒ 只记录（不静默）
        d3d9Log("LockRect", "Flags=0x%08X 被记录（guest 像素没有 DISCARD/READONLY 语义）", flags);
    }
    d3d9Log("LockRect", "Level=0 ⇒ Pitch=%u pBits=0x%08X（矩形 %d,%d..%d,%d）", gs->pitch, bits, left, top, right,
            bottom);
    return true;
}

bool unlockRect(X86& cpu, const TextureInfo& tex, std::uint32_t level, std::string* why) {
    if (level != 0) {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "D3D9：UnlockRect 的 Level=%u（本线只有 level 0）", level);
        if (why) *why = buf;
        return false;
    }
    if (!tex.locked) {
        if (why) *why = "D3D9：UnlockRect 在未锁的 level 上（真机也是 D3DERR_INVALIDCALL）";
        return false;
    }
    cpu.mem->wr32(tex.state + kTxLocked, 0u);
    // ★ 解锁 ⇒ 内容**已知**（guest 内存此刻就是真值），发一个**没出现过**的序列号。
    //   为什么不是 `version + 1`：version 可能被 lock（或 RT 降级）置成 0，加 1 会回落到 1/旧号，
    //   撞上 GPU 缓存里同号 generation ⇒ 静默用旧纹理。序列号只增不复用 ⇒ 不可能撞。
    //
    // ★ 例外：这块纹理**正被当成 render target**（`SetRenderTarget` 绑的就是它的表面）。
    //   那种情况下解锁之后宿主仍会往同一块像素写（Clear / CPU 光栅回退 / GPU 读回），
    //   而它们都不经过 unlockRect ⇒ 版本号不能再声称"已知"。保持 0（未知）。
    //   （不做这条的话：bind 置 0 → 解锁赋新号 → 再写 RT → 采样时快路径命中 ⇒ 旧图。）
    std::uint32_t version = 0;  // 0 = 保持"未知"
    // 判"这块纹理是不是当前 render target"。判不出来（设备状态块不在了）也按"未知"处理：
    // 宁可多哈希（慢），也不要发一个可能错的"已知"号（静默旧图）。
    const bool is_render_target = [&] {
        DeviceInfo dev;
        if (!deviceInfo(cpu, tex.device, &dev, nullptr)) return true;
        return dev.pipeline != 0 && renderTargetArena(cpu, dev.pipeline) == tex.arena;
    }();
    if (!is_render_target) {
        std::uint32_t seq = cpu.mem->rd32(tex.state + kTxVersionSeq);
        if (seq == 0) seq = 2u;  // 序列号回绕保护（0 是"未知"哨兵，⛔ 不能发出去）
        version = seq;
        // 4e9 次 unlock 之后才会回绕到 0；那时重新从 2 起发号 —— 实际上不可达，写在这里只是
        // 让"0 永不作为版本值"这条不变量成立（真到那一步，理论上可能撞旧号）。
        cpu.mem->wr32(tex.state + kTxVersionSeq, seq + 1u);
    }
    cpu.mem->wr32(tex.state + kTxVersion, version);
    // ★ 写区历史（方案 C）：解锁 ⇒ 本次 LockRect 的矩形成为"这一版内容"的写入区域；
    //   上一次的版本号退到 prev。RT 降级（version==0）⇒ 整条链作废：后端必须按整幅处理。
    if (version != 0) {
        cpu.mem->wr32(tex.state + kTxPrevWriteVersion, cpu.mem->rd32(tex.state + kTxWriteVersion));
        for (std::uint32_t i = 0; i < 4u; ++i) {
            cpu.mem->wr32(tex.state + kTxWriteX0 + i * 4u, cpu.mem->rd32(tex.state + kTxPendingX0 + i * 4u));
        }
        cpu.mem->wr32(tex.state + kTxWriteVersion, version);
    } else {
        cpu.mem->wr32(tex.state + kTxWriteVersion, 0u);
        cpu.mem->wr32(tex.state + kTxPrevWriteVersion, 0u);
    }

    return true;
}

// ★ 顶点声明对象 → 元素表（'VDCL' 头 + `CreateVertexDeclaration` 当时那份 D3DVERTEXELEMENT9 数组）。
//   光栅侧要按声明取顶点就必须能读到它；⛔ 不猜布局：magic / 长度不对就返回 false，由调用方响亮拒绝。
bool declarationElements(X86& cpu, std::uint32_t decl_object, const std::uint8_t** data, std::uint32_t* bytes) {
    if (data) *data = nullptr;
    if (bytes) *bytes = 0;
    if (decl_object == 0 || !cpu.com) return false;
    const std::uint32_t state = cpu.com->userOf(decl_object);
    if (state == 0 || !cpu.mem->committed(state, kVdHeader)) return false;
    if (cpu.mem->rd32(state) != kVdMagicValue) return false;
    const std::uint32_t n = cpu.mem->rd32(state + kVdBytes);
    if (n == 0 || n % 8u != 0 || !cpu.mem->committed(state + kVdData, n)) return false;
    if (data) *data = cpu.mem->host(state + kVdData);
    if (bytes) *bytes = n;
    return data != nullptr ? *data != nullptr : true;
}

// ---------------------------------------------------------------------------------------
// 后缓冲 surface 的 LockRect / UnlockRect（状态块 = 'D9D1'）
// ---------------------------------------------------------------------------------------
namespace {

// 状态块 → 像素块。判据不成立 = **本仓状态崩坏**（`*broken=true`），不是 guest 违约。
// ★ 这个判据必须在 `deviceInfoOfState` 之后：调用方（shim）已经用它区分了纹理路（'TEX1'）与后缓冲路。
bool backBufferSurface(X86& cpu, std::uint32_t state, Machine** m_out, GuestSurface** gs_out, bool* broken,
                       std::string* why) {
    if (broken) *broken = false;
    if (m_out) *m_out = nullptr;
    if (gs_out) *gs_out = nullptr;
    Machine* m = nullptr;
    if (!requireMachine(cpu, &m, why)) {
        if (broken) *broken = true;
        return false;
    }
    DeviceInfo dev;
    if (!deviceInfoOfState(cpu, state, &dev, why)) {
        if (broken) *broken = true;
        return false;
    }
    GuestSurface* gs = m->surfaces().find(dev.arena);
    if (gs == nullptr) {
        if (broken) *broken = true;
        if (why) {
            char buf[160];
            std::snprintf(buf, sizeof(buf), "D3D9：后缓冲像素块 0x%08X 不在 arena 里（设备状态与 arena 不一致）",
                          dev.arena);
            *why = buf;
        }
        return false;
    }
    *m_out = m;
    *gs_out = gs;
    return true;
}

}  // namespace

bool lockBackBuffer(X86& cpu, std::uint32_t state, std::uint32_t locked_rect_ea, std::uint32_t rect_ea,
                    std::uint32_t flags, bool* broken, std::string* why) {
    if (broken) *broken = false;
    Machine* m = nullptr;
    GuestSurface* gs = nullptr;
    if (!backBufferSurface(cpu, state, &m, &gs, broken, why)) return false;

    // ---- 契约级检查（D3D9 的正常失败面 ⇒ 调用方答 D3DERR_INVALIDCALL；⛔ 不 trap）----
    // ① 后缓冲只有 swap chain 带 D3DPRESENTFLAG_LOCKABLE_BACKBUFFER 才可锁。
    if (!gs->d3d9_lockable) {
        if (why) *why = "D3D9：后缓冲没有 D3DPRESENTFLAG_LOCKABLE_BACKBUFFER ⇒ 不能锁";
        return false;
    }
    // ② 多重采样后缓冲不能锁（文档明说；本线 CreateDevice 先拒绝了 MSAA，这里是 LockRect 侧的兜底）。
    if (gs->d3d9_multisample != 0) {
        char buf[160];
        std::snprintf(buf, sizeof(buf), "D3D9：后缓冲是多重采样（MultiSampleType=%u）⇒ 不能锁",
                      gs->d3d9_multisample);
        if (why) *why = buf;
        return false;
    }
    // ③ 面 LockRect 的合法 flags 里没有 NOOVERWRITE；未定义的位也不静默吞掉。
    if ((flags & ~kLockSurfaceAllowed) != 0) {
        char buf[160];
        std::snprintf(buf, sizeof(buf), "D3D9：后缓冲 LockRect 的 Flags=0x%08X 含面不认识的位（合法集 = "
                                        "DISCARD/DONOTWAIT/NO_DIRTY_UPDATE/NOSYSLOCK/READONLY）",
                      flags);
        if (why) *why = buf;
        return false;
    }
    // ④ DISCARD 只对 dynamic 资源合法；后缓冲不是 dynamic ⇒ 必失败（且 DISCARD 还禁止带子矩形）。
    if ((flags & kLockDiscard) != 0) {
        if (why) *why = "D3D9：后缓冲不是 dynamic 资源 ⇒ D3DLOCK_DISCARD 非法";
        return false;
    }
    // ⑤ 非嵌套：已锁再锁 = INVALIDCALL（纹理路同义，但那边失败会 trap —— 见 defects.md 的已知分歧）。
    if (gs->d3d9_locked) {
        if (why) *why = "D3D9：LockRect 在已锁的后缓冲上（D3DERR_INVALIDCALL）";
        return false;
    }
    if (!committedOut(cpu, locked_rect_ea, 8u, "D3DLOCKED_RECT", why)) return false;
    int left = 0;
    int top = 0;
    int right = gs->width;
    int bottom = gs->height;
    if (rect_ea != 0) {
        if (!committedOut(cpu, rect_ea, 16u, "LockRect 的 RECT", why)) return false;
        left = static_cast<int>(cpu.mem->rd32(rect_ea + 0));
        top = static_cast<int>(cpu.mem->rd32(rect_ea + 4));
        right = static_cast<int>(cpu.mem->rd32(rect_ea + 8));
        bottom = static_cast<int>(cpu.mem->rd32(rect_ea + 12));
        if (left < 0 || top < 0 || right > gs->width || bottom > gs->height || right <= left || bottom <= top) {
            char buf[160];
            std::snprintf(buf, sizeof(buf), "D3D9：LockRect 的 RECT=(%d,%d,%d,%d) 越界 / 空（后缓冲 %dx%d）", left,
                          top, right, bottom, gs->width, gs->height);
            if (why) *why = buf;
            return false;
        }
    }
    const std::uint32_t bpp = gs->format.bytesPerPixel();
    if (bpp == 0 || gs->pitch == 0) {
        if (broken) *broken = true;
        if (why) *why = "D3D9：后缓冲的像素格式 / 行距非法（状态崩坏）";
        return false;
    }

    // ★ 屏障（屏障清单「LockRect」，与纹理路同一形状）：这块 guest 像素若是当前 GPU 镜像目标，
    //   guest 拿到 pBits 之前必须把未读回的绘制落回 guest；不是目标 ⇒ 队列里的 draw 不写这块像素，
    //   只 flush（⛔ 不把别的 target 读回来）。
    if (GpuRaster* gpu = m->gpuRaster(); gpu != nullptr && gpu->rasterActive() && gpu->rasterDirty()) {
        std::uint8_t* base = m->surfaces().pixels(*gs);
        if (base == nullptr) {
            if (broken) *broken = true;
            if (why) *why = "D3D9：后缓冲像素指针为空（状态崩坏）";
            return false;
        }
        std::string gerr;
        if (gpu->rasterTargetBase() != nullptr && gpu->rasterTargetBase() == base) {
            if (!gpu->rasterSyncToGuest(base, gs->pitch, gs->width, gs->height, &gerr)) {
                if (broken) *broken = true;
                if (why) *why = "D3D9：LockRect 前 GPU 像素读回失败：" + gerr;
                return false;
            }
        } else if (!gpu->rasterFlush(&gerr)) {
            // 队列里的绘制不写这块像素 ⇒ flush 失败不影响本次锁的像素；但要出声（⛔ 不静默）。
            d3d9Warn("LockRect(backbuffer)", "GPU 队列 flush 失败：%s（锁的像素不是当前目标，继续）",
                     gerr.c_str());
        }
    }

    // ★ D3DLOCKED_RECT = { INT Pitch; void* pBits; }（32 位进程 ⇒ 8 B）
    //   · Pitch = **整幅**行距（锁子矩形不改）—— 真机逐行 memcpy 的源行距就是它（0x4A5018）
    //   · pBits = 像素首字节 + top*pitch + left*bpp ⇒ **矩形左上**（真机把它当目标首指针）
    const std::uint32_t bits = gs->ea + static_cast<std::uint32_t>(top) * gs->pitch +
                               static_cast<std::uint32_t>(left) * bpp;
    cpu.mem->wr32(locked_rect_ea + 0, gs->pitch);
    cpu.mem->wr32(locked_rect_ea + 4, bits);
    gs->d3d9_locked = true;
    gs->d3d9_lock_rect[0] = left;
    gs->d3d9_lock_rect[1] = top;
    gs->d3d9_lock_rect[2] = right;
    gs->d3d9_lock_rect[3] = bottom;
    ++gs->d3d9_lock_serial;
    if (flags != 0) {
        d3d9Log("LockRect(backbuffer)", "Flags=0x%08X 被记录（guest 像素没有 READONLY/NOSYSLOCK 语义）", flags);
    }
    d3d9Log("LockRect(backbuffer)",
            "Pitch=%u pBits=0x%08X（矩形 %d,%d..%d,%d）· 第 %u 次锁", gs->pitch, bits, left, top, right, bottom,
            gs->d3d9_lock_serial);
    return true;
}

bool unlockBackBuffer(X86& cpu, std::uint32_t state, bool* broken, std::string* why) {
    if (broken) *broken = false;
    Machine* m = nullptr;
    GuestSurface* gs = nullptr;
    if (!backBufferSurface(cpu, state, &m, &gs, broken, why)) return false;
    if (!gs->d3d9_locked) {
        if (why) *why = "D3D9：UnlockRect 在未锁的后缓冲上（D3DERR_INVALIDCALL）";
        return false;
    }
    gs->d3d9_locked = false;
    // ★ 解锁**不**标脏 / 不发版本号：guest 是权威。下一次绘制在 `!rasterDirty()` 时会
    //   `rasterSyncFromGuest` 把 guest 像素上传（d3d9_pipeline / d3dx9_sprite 的既有口径）。
    //
    // ★ 锁期间竟然画了东西（D3D9 契约外）⇒ 出声。判据只在"这块像素就是当前镜像目标"时成立：
    //   非目标分支的 flush 也会把 dirty 置回 true（`d3d9_gpu.cpp` 的 rasterFlush 语义），那不是
    //   "锁期间有人改像素"。
    if (GpuRaster* gpu = m->gpuRaster(); gpu != nullptr && gpu->rasterActive() && gpu->rasterDirty()) {
        const std::uint8_t* base = m->surfaces().pixels(*gs);
        if (base != nullptr && gpu->rasterTargetBase() == base) {
            d3d9Warn("UnlockRect(backbuffer)",
                     "锁期间有未读回的 GPU 绘制 ⇒ guest 的写入可能在下一次读回时被覆盖");
        }
    }
    d3d9Log("UnlockRect(backbuffer)", "解锁（第 %u 次锁的矩形 %d,%d..%d,%d）", gs->d3d9_lock_serial,
            gs->d3d9_lock_rect[0], gs->d3d9_lock_rect[1], gs->d3d9_lock_rect[2], gs->d3d9_lock_rect[3]);
    return true;
}

void markTextureContentUnknown(X86& cpu, std::uint32_t state) {
    // ★ 判据必须是 `'TEX1'` magic（`textureInfoOfState`）：`surfaceInfoOfState` 对后缓冲的
    //   `'D9D1'` 设备状态也返回 true，按它写会踩坏设备状态块（+52 = Clear 计数、+56/60 = 设备字段）。
    TextureInfo ti;
    if (!textureInfoOfState(cpu, state, &ti, nullptr)) return;
    cpu.mem->wr32(state + kTxVersion, 0u);
    // ★ 写区链也要**一起作废**：宿主（Clear / CPU 光栅回退 / GPU 读回）会往这块像素写而不经过
    //   `unlockRect`。留着旧链的话，下一次"只锁一小块 + Unlock"会拿到 `prev = 旧 write_version`，
    //   后端据此只上传那一小块 ⇒ RT 期间改过的其余像素**永远不进缓存**（静默旧纹理）。
    //   作废之后 `prev = 0` ⇒ `gpuContentDirtyRegion` 退整幅（保守但不猜）。
    cpu.mem->wr32(state + kTxWriteVersion, 0u);
    cpu.mem->wr32(state + kTxPrevWriteVersion, 0u);
}

}  // namespace d3d9
}  // namespace lift
