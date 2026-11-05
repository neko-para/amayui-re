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

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>

#include "runtime/com.h"
#include "runtime/d3d9_pipeline.h"
#include "runtime/ddraw_surface.h"
#include "runtime/heap.h"
#include "runtime/pixel_format.h"
#include "runtime/machine.h"
#include "runtime/memory.h"
#include "runtime/present.h"
#include "runtime/surface.h"
#include "runtime/x86.h"

namespace lift {
namespace d3d9 {
namespace {

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
    std::fprintf(stderr, "[d3d9] %s: ", what);
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(stderr, fmt, ap);
    va_end(ap);
    std::fputc('\n', stderr);
    std::fflush(stderr);
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
    const GuestSurface* gs = m->surfaces().find(arena);
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
    d3d9::destroyForDevice(cpu, state);  // 流水线状态块 + 深度缓冲（含 SetTexture AddRef 的纹理）
    const std::uint32_t arena = cpu.mem->rd32(state + kDvArena);
    cpu.mem->wr32(state + kDvMagic, 0);
    if (Machine* m = activeMachine(); m && &m->cpu() == &cpu && arena != 0) m->surfaces().destroy(arena);
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

std::uint32_t clear(X86& cpu, const DeviceInfo& dev, std::uint32_t count, std::uint32_t rects_ea,
                    std::uint32_t flags, std::uint32_t color, float z, std::uint32_t stencil,
                    std::string* why) {
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
    if (GpuRaster* gpu = m->gpuRaster(); gpu != nullptr && gpu->rasterActive() && gpu->rasterDirty()) {
        std::string gerr;
        if (!gpu->rasterSyncToGuest(m->surfaces().pixels(*gs), gs->pitch, gs->width, gs->height, &gerr)) {
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
    // ★ GPU 光栅化：先把镜像读回 guest，再走 presenter 的 hash / frameStats / dump。
    if (GpuRaster* gpu = m->gpuRaster(); gpu != nullptr && gpu->rasterActive() && gpu->rasterDirty()) {
        std::string gerr;
        if (!gpu->rasterSyncToGuest(m->surfaces().pixels(*gs), gs->pitch, gs->width, gs->height, &gerr)) {
            d3d9Log("Present", "GPU 光栅化读回失败：%s", gerr.c_str());
        }
    }
    if (!m->presenter().present(m->surfaces(), *gs, dump_path, &err)) {
        if (why) *why = "D3D9：HeadlessPresenter.present 失败：" + err;
        return kFail;
    }
    if (!dump_path.empty()) d3d9Log("Present", "帧 BMP 已写入 %s", dump_path.c_str());
    cpu.mem->wr32(dev.state + kDvPresents, dev.presents + 1u);
    // ★ 第一帧的**像素判据**：后缓冲里"非背景像素数"（背景 = 出现次数最多的那个像素值）。
    //   ⛔ 不是猜，也不是"看截图"：直接数 guest 后缓冲的像素（确定性；同一份输入两次跑同值）。
    FrameStats st;
    if (frameStats(gs->format, m->surfaces().pixels(*gs), gs->pitch, gs->width, gs->height, &st)) {
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
    kTxBytes = 56,
};
constexpr std::uint32_t kTxMagicValue = 0x31584554u;  // 'TEX1'

}  // namespace

bool surfaceFormatForD3DFormat(std::uint32_t format, SurfaceFormat* out) {
    return formatForD3dFormat(format, out);
}

std::uint32_t createTextureState(X86& cpu, std::uint32_t device_object, std::uint32_t width,
                                 std::uint32_t height, std::uint32_t levels, std::uint32_t usage,
                                 std::uint32_t format, std::uint32_t pool, std::string* why) {
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
        if (why) *why = std::string("D3D9：SurfaceArena.create 失败：") + m->surfaces().lastError();
        return 0;
    }
    const GuestSurface* gs = m->surfaces().find(arena);
    const std::uint32_t state = cpu.heap->alloc(kTxBytes, /*zero=*/true);
    if (state == 0) {
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
    cpu.mem->wr32(state + kTxMagic, 0);
    if (Machine* m = activeMachine(); m && &m->cpu() == &cpu && arena != 0) m->surfaces().destroy(arena);
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
    return true;
}

}  // namespace d3d9
}  // namespace lift
