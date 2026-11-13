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
#include <vector>

#include "runtime/surface.h"  // SurfaceFormat（像素换算与后缓冲格式）

namespace lift {

struct X86;
class Machine;

// IDirect3DVertexDeclaration9 的状态块（'VDCL'）：**唯一一份**布局定义。
//   shim（创建/回收/Get）与流水线（按声明取顶点）都读这一份；⛔ 不要在别处再写一遍。
constexpr std::uint32_t kVdMagicValue = 0x4C434456u;  // 'VDCL'
constexpr std::uint32_t kVdDevice = 4;                 // 设备状态块 EA
constexpr std::uint32_t kVdBytes = 8;                  // 元素表字节数
constexpr std::uint32_t kVdData = 12;                  // 元素表（D3DVERTEXELEMENT9 数组）
constexpr std::uint32_t kVdHeader = 12;

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

// ★ 深度格式（⛔ **不在**上面那份纹理支持集里：`formatForD3dFormat` 不认它 ⇒ 当纹理问就该答"不支持"）。
//   本线**真的建**的深度面是 32bpp（`d3d9_pipeline.cpp` 的 `createForDevice`：
//   `pixel_format::formatForBpp(32)`），与 D3DFMT_D24S8 的字节足迹一致 ⇒ 只有 75 算"真的建了"；
//   D16(80) 是 16bpp ⇒ ⛔ 不放行（放行就等于声称有一个没建的 16bpp 深度面）。
constexpr std::uint32_t kFmtD24S8 = 75;

// D3DUSAGE（CheckDeviceFormat 只认下面这两位；判定见 `checkDeviceFormat` 的注释）
constexpr std::uint32_t kUsageRenderTarget = 0x00000001u;  // D3DUSAGE_RENDERTARGET
constexpr std::uint32_t kUsageDepthStencil = 0x00000002u;  // D3DUSAGE_DEPTHSTENCIL

// D3DDEVTYPE
constexpr std::uint32_t kDevTypeHal = 1;
constexpr std::uint32_t kDevTypeRef = 2;

// D3DCLEAR_*
constexpr std::uint32_t kClearTarget = 0x00000001u;
constexpr std::uint32_t kClearZBuffer = 0x00000002u;
constexpr std::uint32_t kClearStencil = 0x00000004u;

// D3DLOCK_*（IDirect3DSurface9::LockRect 的合法 flags；★ 面**没有** NOOVERWRITE）
// ★ 面 LockRect 的合法集 = DISCARD / DONOTWAIT / NO_DIRTY_UPDATE / NOSYSLOCK / READONLY。
//   后缓冲上：合法集之外的位 / DISCARD（对非 dynamic 资源非法）都答 D3DERR_INVALIDCALL；
//   合法但本线没有像素语义的位（READONLY / NOSYSLOCK / ...）只记录。
constexpr std::uint32_t kLockReadOnly = 0x00000010u;
constexpr std::uint32_t kLockNoSysLock = 0x00000800u;
constexpr std::uint32_t kLockDiscard = 0x00002000u;
constexpr std::uint32_t kLockDoNotWait = 0x00004000u;
constexpr std::uint32_t kLockNoDirtyUpdate = 0x00008000u;
constexpr std::uint32_t kLockSurfaceAllowed =
    kLockReadOnly | kLockNoSysLock | kLockDiscard | kLockDoNotWait | kLockNoDirtyUpdate;

// D3DPRESENTFLAG_LOCKABLE_BACKBUFFER（本安装的实测：`0x498F1B mov edx,1` → `0x498F98 [esi+440h]`
// = D3DPRESENT_PARAMETERS::Flags(+0x2C)）⇒ 引擎显式要求"后缓冲可锁"。
constexpr std::uint32_t kPresentFlagLockableBackbuffer = 0x00000001u;

// D3D_SDK_VERSION（Direct3DCreate9 的唯一参数；引擎 `0x498D0D push 20h`）
constexpr std::uint32_t kSdkVersion = 32u;

// HRESULT（winerror.h / d3d9.h 的 MAKE_D3DHRESULT）
constexpr std::uint32_t kOk = 0x00000000u;
constexpr std::uint32_t kNoInterface = 0x80004002u;      // E_NOINTERFACE
constexpr std::uint32_t kInvalidArg = 0x80070057u;       // E_INVALIDARG
constexpr std::uint32_t kFail = 0x80004005u;             // E_FAIL
constexpr std::uint32_t kOutOfMemory = 0x8007000Eu;      // E_OUTOFMEMORY
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

// =====================================================================================
// `fillCaps` 的**声明值**（实现与守卫共用一份；⛔ 测试里不另抄数字）
//
// ★ 口径（`REQ-01M79PP97ZD65BJG9GMAJ08MJ3`，已裁决）：caps 是**声明**，不是"实现集"；
//   本线只能**响亮拒绝**的能力必须清位 —— caps 没有"拒绝"动作 ⇒ **不能表达就清 0**
//   （⛔ 不"补"`Caps`/`Caps2`：`Caps` 只有 READ_SCANLINE 一位、`Caps2` 那几位本线都没有，
//   恒 0 是真话）。每个置位的行为见证在 `tests/d3d9.cpp` 的 `d3d9/caps-claims`。
// ★ `MaxTextureWidth/Height` **保留**：那是引擎唯一读的两个字段（`0x499128/0x49912E`）。
// =====================================================================================
// LineCaps：ZTEST(0x02) | BLEND(0x04) | ALPHACMP(0x08)；
//   ⛔ TEXTURE(0x01)（带纹理的线响亮拒绝，bit16）与 FOG(0x10)（全局不做雾）都不许声明。
constexpr std::uint32_t kCapsLineClaim = 0x0000000Eu;
// RasterCaps：只有 D3DPRASTERCAPS_ZTEST(0x10)。⛔ ZFOG(0x200000)（不做雾）。
constexpr std::uint32_t kCapsRasterClaim = 0x00000010u;
// ShadeCaps：COLORGOURAUDRGB(0x8) | SPECULARGOURAUDRGB(0x200) | ALPHAGOURAUDBLEND(0x4000)。
//   ⛔ FOGTABLE(0x80000) / FOGVERTEXGOURAUD(0x40000) / FOGRANGEGOURAUD(0x100000)。
constexpr std::uint32_t kCapsShadeClaim = 0x00004208u;
// TextureCaps：⛔ MIPCUBEMAP(0x10000) —— 本线没有 `CreateCubeTexture`。
constexpr std::uint32_t kCapsTextureClaim = 0x00000000u;
// 比较 / 混合位是**机械生成**的（`d3d9/caps-claims` 对 D3DCMP 1..8 / D3DBLEND 1..13 试探
//   `SetRenderState` 的返回）：本线 8 个比较函数 / 13 个 D3DBLEND 全部接受 ⇒ 位 = 1<<(v-1)。
constexpr std::uint32_t kCapsZCmpClaim = 0x000000FFu;
constexpr std::uint32_t kCapsSrcBlendClaim = 0x00001FFFu;
constexpr std::uint32_t kCapsDestBlendClaim = 0x00001FFFu;
// StencilCaps：本线没有模板缓冲（`STENCILENABLE=1` 记 bit3、像素照写）⇒ 0。
constexpr std::uint32_t kCapsStencilClaim = 0x00000000u;
// MaxUserClipPlanes：不做用户裁剪面（`CLIPPLANEENABLE` 记 bit7）⇒ 0。
constexpr std::uint32_t kCapsMaxUserClipPlanesClaim = 0u;
// MaxVertexBlendMatrices：不做顶点混合（记 bit5）⇒ 0。
constexpr std::uint32_t kCapsMaxVertexBlendMatricesClaim = 0u;
// MaxVolumeExtent：没有体积纹理 ⇒ 0。
constexpr std::uint32_t kCapsMaxVolumeExtentClaim = 0u;
// MaxAnisotropy：各向异性按 LINEAR 近似（记 bit10）⇒ 1（= 无各向异性）。
constexpr std::uint32_t kCapsMaxAnisotropyClaim = 1u;
// MaxPointSize：点尺寸未实现（按 1px 画，记 bit19）⇒ 1.0f。
constexpr float kCapsMaxPointSizeClaim = 1.0f;
// PresentationIntervals：只认 IMMEDIATE(0x80000000)；ONE(0x1) 无人消费 ⇒ 清掉。
constexpr std::uint32_t kCapsPresentationIntervalsClaim = 0x80000000u;
// CursorCaps：没有光标 API（`SetCursorProperties` 未实现）⇒ 0。
constexpr std::uint32_t kCapsCursorClaim = 0x00000000u;
// VertexProcessingCaps：DIRECTIONALLIGHTS(0x08) | POSITIONALLIGHTS(0x10)；
//   ⛔ TEXGEN(0x1) / MATERIALSOURCE7(0x2) / LOCALVIEWER(0x20) / TWEENING(0x40)。
constexpr std::uint32_t kCapsVertexProcessingClaim = 0x00000018u;

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
// ★ 诊断用（只读）：到目前为止**清过后缓冲**的次数。`Clear(TARGET)` 是"新的一帧开始"的机械判据，
//   逐帧诊断拿它给 Draw 编号（`[draw] frame=`），比时间戳可靠。⛔ 不参与任何语义、不进基线。
std::uint32_t frameSeq();
// ★ 顶点声明对象 → (元素表宿主指针, 字节数)。`IDirect3DVertexDeclaration9` 的状态块里存着
//   `CreateVertexDeclaration` 当时那份 D3DVERTEXELEMENT9 数组（'VDCL' 头 + 数据）。
//   ⛔ 不是本模块建的对象 / magic 不符 ⇒ false（不猜布局）。
bool declarationElements(X86& cpu, std::uint32_t decl_object, const std::uint8_t** data, std::uint32_t* bytes);
bool deviceInfoOfState(X86& cpu, std::uint32_t state, DeviceInfo* out, std::string* why);
void destroyDeviceState(X86& cpu, std::uint32_t state);

// ---- D3DCAPS9 ----
// 全部 0xD4 字节都写确定值（MaxTextureWidth/Height = kMaxTextureWidth/Height）。
// ★ 本线**没有 shader / 没有绘制** ⇒ VertexShaderVersion / PixelShaderVersion 报 0
//   （⛔ 不谎称 3.0 让引擎走进没实现的路径）。
std::uint32_t fillCaps(X86& cpu, std::uint32_t caps_ea, std::string* why);

// ---- CheckDeviceFormat：**真值判定** + 诊断记录（宿主侧环形缓冲；⛔ 记录不是语义状态）----
// ★ 为什么记录：判定的答案必须**能与"引擎问了什么"对账** ⇒ 每次调用记一条（含不支持的那些），
//   夹具可以断言"引擎问了什么 / 我们答了什么"。
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
// ★ CheckDeviceFormat 的**真值**判定（D3D9 语义）：支持 ⇒ `kOk`，不支持 ⇒ `kNotAvailable`。
//   支持集**与纹理创建同一份**（`runtime/d3d9.cpp` 的 `formatForD3dFormat`，⛔ 不另写一张表）；
//   规则（RType/Usage 的组合、RT 只认 32bpp、SURFACE 只认真的建的深度格式）写在实现处。
//   ⛔ 未知格式 / 未知 Usage / 其它 RType 一律不支持（不"放行未知"）。
std::uint32_t checkDeviceFormat(const CheckFormatQuery& q);

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
    // 内容版本（GPU 纹理上传缓存的"跳过重传"判据）：**0 = 未知**，非 0 = 已知内容的单调序列号。
    //   · 建纹理 = 1（像素块由 `SurfaceArena::create` 分配 ⇒ guest 堆按 zero=true 清零 ⇒ 内容已知 = 全 0）；
    //   · `LockRect` 立刻置 0（锁期间 guest 可能改像素 ⇒ 未知）；
    //   · `UnlockRect` 赋**下一个序列号**（此刻 guest 内存就是内容真值）；
    //   · 纹理当 render target ⇒ `markTextureContentUnknown` 置 0（宿主写像素的路径太多，
    //     逐条归因不值得；退回"每次采样做内容哈希" = 未优化前的行为，⛔ 不会静默用旧纹理）。
    // ⛔ 必须单调且**不复用**：0 → 已知时若用 `version+1`，会撞上缓存里旧的同号 generation。
    // ⚠ 本线的**不变量**：纹理像素只在 `LockRect` 期间由 guest 写（未 Lock 的直写是 D3D9 API 契约外的
    //   未定义行为）；宿主写像素的路径（`Clear(TARGET)` / CPU 光栅 / GPU 读回）都发生在"纹理当
    //   render target"期间 ⇒ 已被 `markTextureContentUnknown` 降级覆盖。将来若新增别的写像素路径，
    //   必须同样把版本降级为 0（否则就是"静默用旧纹理"）。
    std::uint32_t version = 0;
    // 进程级纹理身份（createTextureState 分配的单调 serial，永不复用；0 = 不是本模块的纹理）。
    // ★ 缓存键必须用它：guest 指针 / 状态块 EA 都可能被 arena·堆复用，只有 serial 不会。
    std::uint32_t serial = 0;
    // ---- 内容写入区域（方案 C 的脏区判据输入；GPU 后端只上传这一块）----
    // ★ 口径：guest 只在 `LockRect` 允许的矩形内写像素（D3D9 API 契约；矩形外的直写是未定义行为）。
    //   · `write_rect` = 最近一次 `UnlockRect` 发的那一版内容对应的写入矩形（全纹理锁 = 整幅）；
    //   · `write_version` = 那次发的版本号，`prev_write_version` = 它之前的版本号（0 = 没有）；
    //   · 只有 `write_version == version && version != 0` 时"写区已知"才成立（RT 降级 / 正在锁
    //     期间 version 是 0 ⇒ 后端必须按整幅处理，⛔ 不许猜）。
    std::uint32_t write_version = 0;
    std::uint32_t prev_write_version = 0;
    std::int32_t write_rect[4] = {0, 0, 0, 0};  // x0, y0, x1, y1（右/下开区间）
};

// D3DFORMAT → SurfaceFormat（= Clear / Sprite 用的同一份换算；⛔ 不另写一份）
bool surfaceFormatForD3DFormat(std::uint32_t format, SurfaceFormat* out);

// 建纹理状态块（'TEX1'）+ 像素块。返回状态块 EA（0 = 失败，why 点名）。
// ★ `out_of_memory`（可选出参）：失败原因是**我们这边的资源不够**（arena 连续块不够 / guest 堆
//   分配状态块失败）⇒ 调用方必须报"资源耗尽"，⛔ 不许记成 "unimplemented"。判据由本函数给，
//   不做字符串前缀嗅探（`why` 是给人读的）。
std::uint32_t createTextureState(X86& cpu, std::uint32_t device_object, std::uint32_t width,
                                 std::uint32_t height, std::uint32_t levels, std::uint32_t usage,
                                 std::uint32_t format, std::uint32_t pool, std::string* why,
                                 bool* out_of_memory = nullptr);
bool textureInfoOfState(X86& cpu, std::uint32_t state, TextureInfo* out, std::string* why);
bool textureInfo(X86& cpu, std::uint32_t object, TextureInfo* out, std::string* why);

// 纹理像素抽样（**只读**；像素判据用）。每 64 个像素取一个；只支持 4bpp（影片纹理 = A8R8G8B8）。
struct TexturePixels {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t nonzero = 0;  // 抽样里 (RGB != 0) 的个数
    std::uint32_t sampled = 0;  // 实际采样到的像素数
    std::uint64_t hash = 0;     // FNV-1a over sampled pixels
};
bool sampleTexture(X86& cpu, std::uint32_t object, TexturePixels* out);

// =====================================================================================
// 派生 mip 链（REQ-01M73G2724479XAVS176WBZ65Q：缩小采样必须有 mip，否则走样）
//
// ★ 口径（**与 guest 可见的 `MipLevels` 解耦**）：
//   · 真机 `D3DXCreateTextureFromFileInMemory` 缺省 `D3DX_DEFAULT` = **整条 mip 链**；本线建纹理时
//     `kTxLevels` 仍是调用方传的那个数（GetLevelCount 不变），链是**我们的内部派生数据**；
//   · `LockRect` / `GetLevelDesc` 仍**只支持 level 0**（其余返回既有失败码）—— 链不经 guest 内存，
//     ⛔ 也不进 SurfaceArena（那是另一本账：`surfaceCount()` / `bytesInUse()` 的基线断言会跟着动），
//     放在 `d3d9.cpp` 里的**宿主旁路表**（键 = `TextureInfo::serial`，进程级单调、永不复用）；
//   · 各级尺寸 = `w>>i × h>>i`（最小 1×1），每级 pitch = `align(w_i * bpp/8, 4)` 字节；
//     `mipChainBytes()` = level 1..N 之和（level 0 仍在 arena 里）。
// ★ 失效判据：`valid && version != 0 && version == 生成时的内容版本`。`version == 0`（正在 Lock /
//   被当 render target / `markTextureContentUnknown`）⇒ **每次取都重建**并保持 valid=false：
//   ⛔ 不许"内容未知就只采 level 0"（那正是本单要修的静默走样）。
// =====================================================================================
// 全链级数（含 level 0；`1×1` ⇒ 1；`2048×2048` ⇒ 12；`350×35` ⇒ 9）。
std::uint32_t mipLevelCount(std::uint32_t width, std::uint32_t height);
// 第 level 级的尺寸（`>> level`，最小 1）。
std::uint32_t mipLevelWidth(std::uint32_t width, std::uint32_t level);
std::uint32_t mipLevelHeight(std::uint32_t height, std::uint32_t level);
// 第 level 级的行距（字节）：`align(w_level * ceil(bpp/8), 4)`。bpp 是**位**/像素（与 TextureInfo 一致）。
std::uint32_t mipLevelPitch(std::uint32_t width, std::uint32_t bpp, std::uint32_t level);
// 第 level 级在链块里的偏移（level >= 1；level 0 **以及越界**都返回 0 —— 越界取级由调用方拒绝）。
std::uint32_t mipLevelOffset(std::uint32_t width, std::uint32_t height, std::uint32_t bpp, std::uint32_t level);
// level 1..N 的字节总数（链块大小；1×1 纹理 = 0）。
std::uint64_t mipChainBytes(std::uint32_t width, std::uint32_t height, std::uint32_t bpp);
// 单级 box filter 降采样（含 alpha；逐通道 `(sum + n/2) / n`）：
//   · 偶数尺寸 ⇒ **严格 2×2 平均**；
//   · 奇数尺寸 ⇒ 按覆盖范围 `[floor(x*sw/dw), floor((x+1)*sw/dw))` 取整块（不丢最后一行/列）。
// `dst_w` 必须 == `max(1, src_w/2)`、`dst_h` 同理（否则什么都不做）。
void mipDownsampleBox(const std::uint8_t* src, std::uint32_t src_w, std::uint32_t src_h, std::uint32_t src_pitch,
                      std::uint8_t* dst, std::uint32_t dst_w, std::uint32_t dst_h, std::uint32_t dst_pitch,
                      std::uint32_t bytes_per_pixel);

struct TextureMipChain {
    std::uint32_t serial = 0;   // 纹理身份（TextureInfo::serial）
    std::uint32_t levels = 1;   // 全链级数（含 level 0）
    std::uint32_t width = 0;    // level 0 尺寸（跨纹理防串台）
    std::uint32_t height = 0;
    std::uint32_t bpp = 0;      // 位/像素（与 TextureInfo::bpp 同口径）
    std::uint32_t version = 0;  // 生成时 level 0 的内容版本（0 = 未知）
    bool valid = false;         // 见上面的失效判据
    std::vector<std::uint8_t> bytes;  // level 1..N（每级 pitch = mipLevelPitch）
    std::uint64_t use = 0;            // LRU 用（纯回收策略，不是语义）
    std::uint64_t epoch = 0;          // 被哪一个"轮"用过（见下；同轮内的链不许被 LRU 回收）
};

// ★ 指针生命周期（**必须成对理解**）：
//   `beginMipEpoch()` 之后建/取到的链，在本"轮"内**不会被 LRU 回收** ⇒ 同一次 Draw 里多个 stage 的
//   链指针可以同时有效（否则"后一个 stage 重建链"会按 LRU 把前一个 stage 刚拿到的链收掉 ⇒ 悬垂）。
//   `d3d9_pipeline.cpp` 的 `prepareContext` **每次 Draw 调一次**（一轮 = 一个 Draw 的时间窗）。
//   ⛔ 仍然不要跨 Draw 存 `TextureMipChain*`（下一轮就可能被回收 / 被重建换掉）。
void beginMipEpoch();

// 取（必要时从 level 0 **box filter 逐级生成**）某纹理的派生链。
//   · 链有效且版本一致 ⇒ 直接返回（不重算）；
//   · 链不存在 / 版本变了 / `version == 0`（内容未知）⇒ 重建（`*rebuilt` 置 true，供 `[draw] miprb=` 记账）；
//   · 返回 nullptr ⇒ 不可用（OOM / 状态块或 arena 对不上）—— 调用方按 level 0 采样
//     并 `noteMipLevel0Sampled()` 记账（⛔ 不静默）。
// ★ 返回的指针在本轮内有效（见 `beginMipEpoch`）。
const TextureMipChain* ensureTextureMipChain(X86& cpu, const TextureInfo& tex, bool* rebuilt, std::string* why);
// 链缓存诊断 / 守卫用（宿主旁路表：链数 / 字节 / 生成次数 / 退回 level 0 的采样次数）。
struct MipCacheStats {
    std::uint64_t chains = 0;
    std::uint64_t bytes = 0;
    std::uint64_t builds = 0;
    std::uint64_t level0_fallbacks = 0;
};
MipCacheStats mipCacheStats();
void resetMipCache();
void noteMipLevel0Sampled();

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

// ---- 后缓冲 surface 的锁（状态块是 `'D9D1'` 走这条；纹理表面仍走上面的 lockRect/unlockRect）----
// 口径（D3D9 文档 + 真机调用点 `0x4A4DC0` 的逐行 memcpy）：
//   · `pBits` = 像素首字节 + top*pitch + left*bpp（**矩形左上**；rect==NULL ⇒ 整幅起点）
//   · `Pitch` = **整幅**行距（锁子矩形**不改**；真机 `0x4A5018 add edi,[ebp+var_44]` 就按它推进源指针）
//   · 契约级 guest 错误 ⇒ false + `*broken=false`（调用方答 D3DERR_INVALIDCALL，⛔ 不 trap）：
//     已锁再锁 / 未锁就解锁 / 矩形越界或空 / DISCARD（后缓冲不是 dynamic 资源）/ 没有
//     `D3DPRESENTFLAG_LOCKABLE_BACKBUFFER` / `MultiSampleType != NONE` / 未定义的 flags 位 /
//     `pLockedRect` 或 RECT 不可写。
//   · 本仓状态崩坏（状态块/arena 对不上、GPU 读回失败、bpp/pitch 为 0）⇒ false + `*broken=true`（调用方 trap）。
//   · 锁前屏障：这块 guest 像素若是当前 GPU 镜像目标且有未读回的绘制 ⇒ 先 flush + 读回；
//     不是目标 ⇒ 只 flush（队列里的 draw 不写这块像素，⛔ 不把别的 target 读回来）。
//   · 解锁**不**标脏 / 不发版本号：guest 是权威，由下一次绘制的 `!rasterDirty() ⇒ rasterSyncFromGuest` 兑现。
bool lockBackBuffer(X86& cpu, std::uint32_t state, std::uint32_t locked_rect_ea, std::uint32_t rect_ea,
                    std::uint32_t flags, bool* broken, std::string* why);
bool unlockBackBuffer(X86& cpu, std::uint32_t state, bool* broken, std::string* why);

// GetRenderTargetData：把 render target surface 的像素**复制**到一块 offscreen 普通表面
// （`CreateOffscreenPlainSurface` 建的那种，池 = SYSTEMMEM/SCRATCH）。
//   · 参数必须是本模块的两块 IDirect3DSurface9；尺寸 / 格式不同 ⇒ `kInvalidCall`（真机同形）；
//   · 源是当前 GPU 镜像目标（纹理当 RT）⇒ 先 flush + 读回（与 `lockRect` 同一条屏障）；
//   · 复制后目标的内容版本降级为"未知"（宿主写了像素，走 `markTextureContentUnknown`）；
//   · 失败原因写 `why`（⛔ 不静默返回 0 当成功）。
std::uint32_t getRenderTargetData(X86& cpu, std::uint32_t render_target, std::uint32_t dest_surface,
                                  std::string* why);

// UpdateSurface：把源 surface 的一块矩形**原样复制**到目标 surface 的某点（无缩放 / 无格式转换）。
//   · 源 / 目标都必须是本模块的 IDirect3DSurface9；格式不同 ⇒ `kInvalidCall`（真机同形）；
//   · `src_rect_ea == 0` ⇒ 整幅源；`dst_point_ea == 0` ⇒ 目标 (0,0)；
//   · 目标放不下 / 源矩形越界 ⇒ `kInvalidCall`（⛔ 不隐式裁剪）；
//   · 源是当前 GPU 镜像目标 ⇒ 先 flush + 读回（与 `lockRect` 同一条屏障）；
//   · 复制后目标内容版本降级为"未知"。
std::uint32_t updateSurface(X86& cpu, std::uint32_t src_surface, std::uint32_t src_rect_ea,
                            std::uint32_t dst_surface, std::uint32_t dst_point_ea, std::string* why);

// ★ 把一块状态块的**内容版本降级为"未知"**（0）：它被当成了 render target（`SetRenderTarget`
//   绑到纹理表面），之后宿主可能经 Clear / CPU 光栅回退 / GPU 读回写它的像素 —— 这些路径不经过
//   `unlockRect`，无法逐条归因。降级 = 退回"每次采样做内容哈希"（未优化前的行为）。
// ★ 判据是状态块的 `'TEX1'` magic（`textureInfoOfState`），**不是** `surfaceInfoOfState`：
//   后者对后缓冲的 `'D9D1'` 设备状态也返回 true，按它写版本会踩坏设备状态块。
// ★ 不是纹理状态块 / 未提交 ⇒ 静默 no-op（调用方不必先自己判）。
void markTextureContentUnknown(X86& cpu, std::uint32_t state);

}  // namespace d3d9
}  // namespace lift
