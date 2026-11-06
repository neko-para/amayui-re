// emulator-lift · D3D9 最小设备簇的**状态与像素层**（M2 第一帧的前置；用户 2026-11-05 裁决）
//
// ★ 分工（与 DDraw 同形）：本文件只做"guest 内存里的状态 + 像素搬运 + 适配器模式表 + caps 填充"，
//   **不认 COM** —— 对象 / vtable / refcount 在 `runtime/com.h`，方法 shim 在 `shims/d3d9.cpp`。
// ★ 范围 = **取证到的那一小簇**（用户 2026-11-05 已裁决"确认需要实现 D3D 接口"，不再是 stub）：
//     Direct3DCreate9 → GetAdapterDisplayMode → GetAdapterModeCount/EnumAdapterModes
//     → CreateDevice → SetDialogBoxMode → GetDeviceCaps → CheckDeviceFormat → Clear → Present
//   ⛔ 其余槽 = loud trap（见 shims/d3d9.cpp）；"没实现"绝不许伪装成"返回 0 / D3DERR"。
// ★ 结构偏移**不是凭记忆**：D3DDISPLAYMODE / D3DPRESENT_PARAMETERS / D3DCAPS9 的字段偏移在下面
//   逐条标了来源 —— `.lst` 站点 EA = **实测**；没有站点的 = **按头文件推断**（并在此明说）。
//   ⚠ 实测已经纠正了 plan 文档里的一处说法：`D3DDISPLAYMODE` **不是 12 B**、字段序也不是
//     "Width/Height/Format/RefreshRate" ⇒ 见下面的 `DisplayModeOff`（Format 在 **+0x0C**）。
// ★ 状态放哪儿（与 DDraw 同一套理由）：设备的**后缓冲像素**在 `SurfaceArena`（P0，guest 地址空间），
//   设备的**状态块**（'D9D1'）从 guest 堆分配、EA 挂在对象 `obj+8` 的扩展位（`ComRegistry::createObject(..., user)`）。
//   ⇒ 生命周期跟着 refcount 走，⛔ 不需要宿主侧静态表（Machine 反复建在同一栈地址时会串台）。
// ★ 适配器显示模式 = **与 DDraw 共用同一台虚拟显示器**（`ddraw::displayMode`）：本安装的
//   `set:ScreenX/ScreenY/FullScreenBit` 落在这份设备状态里，D3D9 报同一份 ⇒ 两个 API 不会互相矛盾。
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "runtime/surface.h"  // SurfaceFormat（像素换算与后缓冲格式）

namespace lift {

struct X86;
class Machine;

namespace d3d9 {

// =====================================================================================
// Win32 形状常量（值来自 SDK d3d9.h / winerror.h）
// =====================================================================================
// D3DFORMAT（d3d9types.h 的枚举值）
constexpr std::uint32_t kFmtUnknown = 0;
constexpr std::uint32_t kFmtA8R8G8B8 = 21;
constexpr std::uint32_t kFmtX8R8G8B8 = 22;
constexpr std::uint32_t kFmtR5G6B5 = 23;
constexpr std::uint32_t kFmtX1R5G5B5 = 24;
constexpr std::uint32_t kFmtA1R5G5B5 = 25;
constexpr std::uint32_t kFmtA4R4G4B4 = 26;

// D3DDEVTYPE
constexpr std::uint32_t kDevTypeHal = 1;
constexpr std::uint32_t kDevTypeRef = 2;

// D3DCLEAR_*
constexpr std::uint32_t kClearTarget = 0x00000001u;
constexpr std::uint32_t kClearZBuffer = 0x00000002u;
constexpr std::uint32_t kClearStencil = 0x00000004u;

// D3D_SDK_VERSION（Direct3DCreate9 的唯一参数；引擎 `0x498D0D push 20h`）
constexpr std::uint32_t kSdkVersion = 32u;

// HRESULT（winerror.h / d3d9.h 的 MAKE_D3DHRESULT）
constexpr std::uint32_t kOk = 0x00000000u;
constexpr std::uint32_t kNoInterface = 0x80004002u;      // E_NOINTERFACE
constexpr std::uint32_t kInvalidArg = 0x80070057u;       // E_INVALIDARG
constexpr std::uint32_t kFail = 0x80004005u;             // E_FAIL
constexpr std::uint32_t kNotAvailable = 0x8876086Au;     // D3DERR_NOTAVAILABLE
constexpr std::uint32_t kInvalidCall = 0x8876086Cu;      // D3DERR_INVALIDCALL
constexpr std::uint32_t kInvalidDevice = 0x88760868u;    // D3DERR_INVALIDDEVICE

inline bool succeeded(std::uint32_t hr) { return (hr & 0x80000000u) == 0u; }

// 适配器能力（引擎只读 MaxTextureWidth/Height；⛔ 其余字段也必须**确定**，不留未初始化字节）
constexpr std::uint32_t kMaxTextureWidth = 4096u;
constexpr std::uint32_t kMaxTextureHeight = 4096u;

// =====================================================================================
// D3DDISPLAYMODE（**16 B**）
//   实测：`0x498C71` 读 `+0x0C` 当 D3DFORMAT（打印 "カラーモード・D3DFORMAT：%d" 并传给
//         GetAdapterModeCount）；`0x498EEC` / `0x498F5C` 读 `+0` / `+4` 当宽 / 高；
//         `0x498D39` 把 `lea [ebp-0x14]` 当出参传进 GetAdapterDisplayMode。
//   ⇒ Width +0 / Height +4 / RefreshRate +8 / Format +0x0C（⛔ 不是 plan 文档写的 12 B）
// =====================================================================================
constexpr std::uint32_t kDisplayModeBytes = 16u;
enum DisplayModeOff : std::uint32_t {
    kDmWidth = 0,
    kDmHeight = 4,
    kDmRefreshRate = 8,   // 按头文件推断（本路径不读它）
    kDmFormat = 12,       // ★ 实测（0x498C71）
};

// =====================================================================================
// D3DPRESENT_PARAMETERS（**56 B**）
//   实测：`0x498F0A call _memset(&[esi+414h], 0, 38h)` ⇒ 基址 0x414、长度 0x38；逐字段站点：
//     +0x00 (0x498F77 写 ecx=宽) · +0x04 (0x498F7C 写 eax=高) · +0x08 (0x498F82 写 ecx=Format，
//     来源 = 显示模式的 +0x0C) · +0x0C (0x498F38 写 1=BackBufferCount) · +0x10 (0x499021，来自 arg_24)
//     · +0x18 (0x498FA6 写 3 / 0x498FB2 写 1 = SwapEffect) · +0x1C (0x498F9E 写 ebx = **hDeviceWindow**)
//     · +0x20 (0x498F24/0x498F2C 写 1 = Windowed) · +0x24 (0x498F88 写 1 = EnableAutoDepthStencil)
//     · +0x28 (0x498F8E 写 4Bh=75 = AutoDepthStencilFormat=D3DFMT_D24S8)
//     · +0x2C (0x498F98 写 1 / 0x499011 写 0 = Flags) · +0x34 (0x498FBF 写 80000000h / 0x498FCF 写 1
//       / 0x498FDC 写 2 / 0x498FE9 写 4 / 0x498FFA 写 8 = PresentationInterval)
//   ⇒ 与 SDK 头文件逐字段一致（**实测**，⛔ 没按记忆改写）。
// =====================================================================================
constexpr std::uint32_t kPresentParamsBytes = 56u;
enum PresentParamsOff : std::uint32_t {
    kPpBackBufferWidth = 0,
    kPpBackBufferHeight = 4,
    kPpBackBufferFormat = 8,
    kPpBackBufferCount = 12,
    kPpMultiSampleType = 16,
    kPpMultiSampleQuality = 20,
    kPpSwapEffect = 24,
    kPpHDeviceWindow = 28,   // ★ 实测（0x498F9E）
    kPpWindowed = 32,
    kPpEnableAutoDepthStencil = 36,  // ★ 实测（0x498F88 = 1）
    kPpAutoDepthStencilFormat = 40,  // ★ 实测（0x498F8E = 75）
    kPpFlags = 44,                   // ★ 实测（0x498F98 = 1）
    kPpFullScreenRefreshRateInHz = 48,
    kPpPresentationInterval = 52,
};

// =====================================================================================
// D3DCAPS9（**0xD4 B** = d3d9types.h 的字段数）
//   实测的只有两个字段：`0x499126 call [ecx+1Ch]` 后 `0x499128` 读 `[ebp-0xE8]`、
//   `0x49912E` 读 `[ebp-0xEC]`，其中 caps 基址 = `[ebp-0x144]` ⇒ **MaxTextureWidth +0x58 /
//   MaxTextureHeight +0x5C**（引擎只读这两个）。其余偏移 = **按头文件推断**（见 fillCaps）。
//   ⚠ 整个结构必须**全部写成确定值**（引擎把它当 352 B 的栈缓冲读，残留会变成随机能力）。
// =====================================================================================
constexpr std::uint32_t kCapsBytes = 0xD4u;
enum CapsOff : std::uint32_t {
    kCapsDeviceType = 0x00,
    kCapsAdapterOrdinal = 0x04,
    kCapsCaps = 0x08,
    kCapsCaps2 = 0x0C,
    kCapsPresentationIntervals = 0x14,
    kCapsCursorCaps = 0x18,
    kCapsDevCaps = 0x1C,
    kCapsPrimitiveMiscCaps = 0x20,
    kCapsRasterCaps = 0x24,
    kCapsZCmpCaps = 0x28,
    kCapsSrcBlendCaps = 0x2C,
    kCapsDestBlendCaps = 0x30,
    kCapsAlphaCmpCaps = 0x34,
    kCapsShadeCaps = 0x38,
    kCapsTextureCaps = 0x3C,
    kCapsTextureFilterCaps = 0x40,
    kCapsTextureAddressCaps = 0x4C,
    kCapsLineCaps = 0x54,
    kCapsMaxTextureWidth = 0x58,   // ★ 实测（0x499128 / 0x49912E）
    kCapsMaxTextureHeight = 0x5C,  // ★ 实测
    kCapsMaxVolumeExtent = 0x60,
    kCapsMaxTextureRepeat = 0x64,
    kCapsMaxTextureAspectRatio = 0x68,
    kCapsMaxAnisotropy = 0x6C,
    kCapsMaxVertexW = 0x70,
    kCapsStencilCaps = 0x88,
    kCapsFVFCaps = 0x8C,
    kCapsTextureOpCaps = 0x90,
    kCapsMaxTextureBlendStages = 0x94,
    kCapsMaxSimultaneousTextures = 0x98,
    kCapsVertexProcessingCaps = 0x9C,
    kCapsMaxActiveLights = 0xA0,
    kCapsMaxUserClipPlanes = 0xA4,
    kCapsMaxVertexBlendMatrices = 0xA8,
    kCapsMaxVertexBlendMatrixIndex = 0xAC,
    kCapsMaxPointSize = 0xB0,
    kCapsMaxPrimitiveCount = 0xB4,
    kCapsMaxVertexIndex = 0xB8,
    kCapsMaxStreams = 0xBC,
    kCapsMaxStreamStride = 0xC0,
    kCapsVertexShaderVersion = 0xC4,
    kCapsMaxVertexShaderConst = 0xC8,
    kCapsPixelShaderVersion = 0xCC,
    kCapsMaxPixelShaderValue = 0xD0,
};

// ---- 适配器（虚拟显示器）----
struct AdapterMode {
    int width = 0;
    int height = 0;
    unsigned refresh = 0;
    std::uint32_t format = 0;  // D3DFORMAT
};

// bpp → 桌面 D3DFORMAT：32 → X8R8G8B8(22)、16 → R5G6B5(23)。
// ★ 为什么桌面模式一律报 X 格式：真机的桌面模式**没有 alpha 通道**；且本簇的引擎把
//   GetAdapterDisplayMode 的 Format 原样当 BackBufferFormat 用（0x498F79 → 0x498F82）。
std::uint32_t displayFormatForBpp(unsigned bpp);
// 当前显示模式（= DDraw 设备状态里的那一份；`set:ScreenX/ScreenY` 就落在那里）。
bool adapterDisplayMode(X86& cpu, AdapterMode* out, std::string* why);

// 适配器模式表（确定性顺序；⛔ 不依赖宿主显示设备）。
// `format` 是本模拟器真的能呈现的两种（22 / 23）；其它格式 ⇒ 0 条（真机形状：没有该格式的模式）。
std::uint32_t adapterModeCount(std::uint32_t format);
bool adapterModeAt(std::uint32_t format, std::uint32_t index, AdapterMode* out);

// 把模式写进 guest 的 D3DDISPLAYMODE（16 B；越界 / 未提交 ⇒ false + why）。
bool writeDisplayMode(X86& cpu, std::uint32_t ea, const AdapterMode& mode, std::string* why);

// ---- D3DPRESENT_PARAMETERS（从 guest 读出来）----
struct PresentParams {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t format = 0;
    std::uint32_t back_buffer_count = 0;
    std::uint32_t multi_sample_type = 0;
    std::uint32_t multi_sample_quality = 0;
    std::uint32_t swap_effect = 0;
    std::uint32_t device_window = 0;
    bool windowed = false;
    bool auto_depth_stencil = false;
    std::uint32_t auto_depth_stencil_format = 0;
    std::uint32_t flags = 0;
    std::uint32_t refresh_hz = 0;
    std::uint32_t presentation_interval = 0;
};
bool readPresentParams(X86& cpu, std::uint32_t ea, PresentParams* out, std::string* why);

// ---- 设备创建 ----
struct CreateRequest {
    std::uint32_t adapter = 0;
    std::uint32_t device_type = 0;  // D3DDEVTYPE
    std::uint32_t hfocus_window = 0;
    std::uint32_t behavior_flags = 0;
    PresentParams present;
};
// 建后缓冲像素（SurfaceArena）+ 状态块（guest 堆，'D9D1'）。返回**状态块** EA；0 = 失败（why 点名）。
// ★ 后缓冲尺寸 / 像素格式来自 present params（Width/Height 为 0 ⇒ 用当前显示模式，真机同义）。
std::uint32_t createDeviceState(X86& cpu, const CreateRequest& req, std::string* why);

// 设备状态块 +60：IDirect3DDevice9 对象 EA（后缓冲 surface 的 GetDevice 要回答它）。
// 状态块共 64 B；+56 是流水线状态 EA，+60 原先是空的。
constexpr std::uint32_t kDeviceStateObjectOff = 60u;

struct DeviceInfo {
    std::uint32_t state = 0;   // 'D9D1' 状态块 EA
    std::uint32_t object = 0;  // IDirect3DDevice9 对象 EA（+60；0 = 尚未绑定）
    std::uint32_t arena = 0;   // 后缓冲像素块 EA（SurfaceArena）
    std::uint32_t pipeline = 0;  // 'DL01' 流水线状态块 EA（runtime/d3d9_pipeline.{h,cpp}；0 = 没有）
    int width = 0;
    int height = 0;
    std::uint32_t format = 0;
    std::uint32_t bpp = 0;
    std::uint32_t pitch = 0;
    std::uint32_t device_type = 0;
    std::uint32_t behavior = 0;
    std::uint32_t hwnd = 0;
    bool windowed = true;
    std::uint32_t auto_depth_format = 0;
    std::uint32_t presents = 0;
    std::uint32_t clears = 0;
};
bool deviceInfo(X86& cpu, std::uint32_t object, DeviceInfo* out, std::string* why);
bool deviceInfoOfState(X86& cpu, std::uint32_t state, DeviceInfo* out, std::string* why);
void destroyDeviceState(X86& cpu, std::uint32_t state);

// ---- D3DCAPS9 ----
// 全部 0xD4 字节都写确定值（MaxTextureWidth/Height = kMaxTextureWidth/Height）。
// ★ 本线**没有 shader / 没有绘制** ⇒ VertexShaderVersion / PixelShaderVersion 报 0
//   （⛔ 不谎称 3.0 让引擎走进没实现的路径）。
std::uint32_t fillCaps(X86& cpu, std::uint32_t caps_ea, std::string* why);

// ---- CheckDeviceFormat 的**诊断记录**（宿主侧环形缓冲；⛔ 不是语义状态）----
// ★ 为什么要有：`CheckDeviceFormat` 本线一律答 S_OK（不真的枚举格式），但不许"对什么都答 S_OK
//   又不留痕" ⇒ 每次调用记一条，夹具可以断言"引擎问了什么"。
struct CheckFormatQuery {
    std::uint32_t adapter = 0;
    std::uint32_t device_type = 0;
    std::uint32_t adapter_format = 0;
    std::uint32_t usage = 0;
    std::uint32_t resource_type = 0;
    std::uint32_t check_format = 0;
};
constexpr std::size_t kCheckFormatLogCapacity = 16u;
void resetCheckFormatQueries();
std::size_t checkFormatQueryCount();
// `newest` = 0 是最新一条；越界 ⇒ false。
bool checkFormatQueryAt(std::size_t newest, CheckFormatQuery* out);
void recordCheckFormatQuery(const CheckFormatQuery& q);

// ---- 渲染动作（本簇的"真的做"那一半）----
// Clear：把**后缓冲**像素清成给定颜色（D3DCOLOR = 0xAARRGGBB，按 back buffer 的像素格式换算）。
// 只支持 Count==0（整块）与 Count>0（D3DRECT 表，16 B/条）；Z/Stencil 位**记录**但不清
// （本线不建深度缓冲）⇒ 不谎称清了。
std::uint32_t clear(X86& cpu, const DeviceInfo& dev, std::uint32_t count, std::uint32_t rects_ea,
                    std::uint32_t flags, std::uint32_t color, float z, std::uint32_t stencil,
                    std::string* why);
// Present：后缓冲 → `HeadlessPresenter`（frameCount / lastHash），并打帧号。
std::uint32_t present(X86& cpu, const DeviceInfo& dev, std::string* why);

// 像素换算（导出给夹具算期望值用；与 Clear 同一份实现）
std::uint32_t pixelFromColor(const SurfaceFormat& fmt, std::uint32_t argb);

// =====================================================================================
// IDirect3DTexture9 簇（**追加式**；M2 第一帧的直接缺口，2026-11-05 boot 实测）
//   boot 越过 D3DXCreateSprite 后的下一条 unimplemented = `d3dx9_43!D3DXCreateTexture`
//   （`.lst` 0x48AD07，调用点 NormalTexture::Create：Usage=1/D3DUSAGE_RENDERTARGET、
//    Pool=0/D3DPOOL_DEFAULT、MipLevels=1、Format=0x1A 或 0x1E）。
//   ★ 纹理像素同样落在 `SurfaceArena`（guest 地址空间）里，`LockRect` 返回 guest 指针 ——
//     与 DDraw surface 同一套理由（guest 直接按 pitch + 掩码读写像素）。
//   ★ vtable 顺序**不是**按记忆：`.lst` 0x48AD18 `mov edx,[eax+44h]` → 0x48AD22 call，
//     参数 (this, Level=0, pDesc) ⇒ `+0x44 = GetLevelDesc`。按 SDK d3d9.h 的 22 槽布局：
//     IUnknown 3 + IDirect3DResource9 8 + IDirect3DBaseTexture9 6 = **17 槽**（0x00..0x40），
//     所以 GetLevelDesc 是第 18 槽（+0x44）、GetSurfaceLevel +0x48、LockRect +0x4C、
//     UnlockRect +0x50、AddDirtyRect +0x54。**实测与 SDK 槽序一致**。
//     实测：+0x44 被当作 GetLevelDesc(this, Level, pDesc) 调，且 desc 的 +0x18/+0x1C 被当
//     宽/高读（0x48AD2A/0x48AD2D 读 [ebp-0Ch]/[ebp-8]，desc 基址 [ebp-24h]）⇒ 与
//     D3DSURFACE_DESC（32 B：Format+0 / Type+4 / Usage+8 / Pool+0xC / MSType+0x10 /
//     MSQuality+0x14 / Width+0x18 / Height+0x1C）**逐字段一致**。
// =====================================================================================
constexpr std::uint32_t kTextureDescBytes = 32u;
enum TextureDescOff : std::uint32_t {
    kSdFormat = 0,
    kSdType = 4,
    kSdUsage = 8,
    kSdPool = 12,
    kSdMultiSampleType = 16,
    kSdMultiSampleQuality = 20,
    kSdWidth = 24,   // ★ 实测（0x48AD2A 读 desc+0x18）
    kSdHeight = 28,  // ★ 实测（0x48AD2D 读 desc+0x1C）
};
// D3DRESOURCETYPE
constexpr std::uint32_t kRTypeSurface = 1;
constexpr std::uint32_t kRTypeTexture = 3;
// D3DPOOL
constexpr std::uint32_t kPoolDefault = 0;
constexpr std::uint32_t kPoolManaged = 1;

struct TextureInfo {
    std::uint32_t state = 0;
    std::uint32_t device = 0;  // 建它时的 IDirect3DDevice9 对象 EA
    std::uint32_t arena = 0;   // 像素块（SurfaceArena）
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t format = 0;  // D3DFORMAT
    std::uint32_t bpp = 0;
    std::uint32_t pitch = 0;
    std::uint32_t levels = 0;
    std::uint32_t usage = 0;
    std::uint32_t pool = 0;
    std::uint32_t locks = 0;
    bool locked = false;
    // 内容版本：每次 UnlockRect +1（Lock 期间 guest 可能改像素）。GPU 纹理缓存用它判断
    // "像素未变 ⇒ 不重传"；0 = 还没发生过 Unlock（未知，调用方退回内容哈希）。
    std::uint32_t version = 0;
    // 进程级纹理身份（createTextureState 分配的单调 serial，永不复用；0 = 不是本模块的纹理）。
    // ★ 缓存键必须用它：guest 指针 / 状态块 EA 都可能被 arena·堆复用，只有 serial 不会。
    std::uint32_t serial = 0;
};

// D3DFORMAT → SurfaceFormat（= Clear / Sprite 用的同一份换算；⛔ 不另写一份）
bool surfaceFormatForD3DFormat(std::uint32_t format, SurfaceFormat* out);

// 建纹理状态块（'TEX1'）+ 像素块。返回状态块 EA（0 = 失败，why 点名）。
std::uint32_t createTextureState(X86& cpu, std::uint32_t device_object, std::uint32_t width,
                                 std::uint32_t height, std::uint32_t levels, std::uint32_t usage,
                                 std::uint32_t format, std::uint32_t pool, std::string* why);
bool textureInfoOfState(X86& cpu, std::uint32_t state, TextureInfo* out, std::string* why);
bool textureInfo(X86& cpu, std::uint32_t object, TextureInfo* out, std::string* why);
// IDirect3DSurface9 的状态块：纹理表面（'TEX1'）或后缓冲（'D9D1'）。
// 返回的 TextureInfo 只填 surface 需要的字段（state 是原状态块；后缓冲的 device = 设备状态块 +60）。
bool surfaceInfoOfState(X86& cpu, std::uint32_t state, TextureInfo* out, std::string* why);
void destroyTextureState(X86& cpu, std::uint32_t state);

// GetLevelDesc：写 32 B 的 D3DSURFACE_DESC（level != 0 / 越界 ⇒ false + why）
bool writeTextureDesc(X86& cpu, const TextureInfo& tex, std::uint32_t level, std::uint32_t desc_ea,
                      std::string* why);
// LockRect：D3DLOCKED_RECT = { INT Pitch; void* pBits; }（8 B）。只支持 level 0。
// 支持 rect==NULL（整块）与 rect（越界 / 空矩形 ⇒ false）；flags 只记录（DISCARD/READONLY 对
// 本线的 guest 内存没有意义，但**不静默**：写进 locks 计数与日志）。
bool lockRect(X86& cpu, const TextureInfo& tex, std::uint32_t level, std::uint32_t locked_rect_ea,
              std::uint32_t rect_ea, std::uint32_t flags, std::string* why);
bool unlockRect(X86& cpu, const TextureInfo& tex, std::uint32_t level, std::string* why);

}  // namespace d3d9
}  // namespace lift
