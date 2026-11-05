// emulator-lift · D3DX9 Sprite 簇的**状态 / 批次 / 合成层**（M2 · 第一帧的直接缺口）
//
// ★ 分工（与 D3D9 同形）：本文件只做"guest 内存里的状态 + 批次表 + 像素合成"，
//   **不认 COM** —— 对象 / vtable / refcount 在 `runtime/com.h`，方法 shim 在 `shims/d3dx9.cpp`。
// ★ 现状（2026-11-05，实测 boot）：`d3dx9_43!D3DXCreateSprite` 是**第一个** d3dx 调用
//   （`.lst` 0x4A2BD0 在 sub_4A2BA0；返回地址 0x4A2BD5）。⇒ 本簇先落地：
//     D3DXCreateSprite → ID3DXSprite：QueryInterface/AddRef/Release · GetDevice · GetTransform ·
//     SetTransform · Begin · Draw · Flush · End · OnLostDevice · OnResetDevice
//   ⛔ 未实现的槽（SetWorldViewRH/LH，+0x18/+0x1C）= loud trap（见 shims/d3dx9.cpp）。
//
// ★ vtable 顺序 = **`.lst` 调用点取证**，不是照记忆排（用户明确要求）：
//     槽   偏移    方法              取证 EA（mov reg,[vtbl+off] → call）
//     #3   +0x0C   GetDevice         ⚠ 未取证（SDK 推断；本段 boot 引擎不调）
//     #4   +0x10   GetTransform      ⚠ 未取证（同上）
//     #5   +0x14   SetTransform      ★ 0x4A30D3（`mov eax,[ecx+14h]` → 0x4A30DB call）
//                                     ★ 0x4A789A、★ 0x49FE1A（同一偏移，另一函数）
//     #6   +0x18   SetWorldViewRH    ⚠ 未取证 ⇒ loud trap
//     #7   +0x1C   SetWorldViewLH    ⚠ 未取证 ⇒ loud trap
//     #8   +0x20   Begin             ★ 0x4A30E5（`mov edx,[ecx+20h]` → 0x4A30EB call，参数 0x10）
//     #9   +0x24   Draw              ★ 0x48AE6E（`mov edx,[edx+24h]` → 0x48AE84 call；
//                                      sub_48AE60 是 NormalTexture::DrawSprite 的转发壳）
//     #10  +0x28   Flush             ⚠ 未取证（SDK 推断；End 的"半程"版本）
//     #11  +0x2C   End               ★ 0x4A00EC（`mov edx,[ecx+2Ch]` → 0x4A00F0 call）
//                                     ★ 0x4A356A（同一偏移）
//     #12  +0x30   OnLostDevice      ★ 0x499C80（`mov edx,[ecx+30h]` → 0x499C84 call）
//     #13  +0x34   OnResetDevice     ★ 0x4A8634（`mov edx,[ecx+34h]` → 0x4A8638 call）
//     #2   +0x08   Release           ★ 0x4A2BB6（`mov edx,[ecx+8]` → 0x4A2BBA call）
//   ⇒ 已取证的 7 个槽（0x08/0x14/0x20/0x24/0x2C/0x30/0x34）**相对顺序与 SDK 的 13 槽布局完全一致**，
//     所以未取证的 4 个（0x0C/0x10/0x28 与两个 SetWorldView）按同一份布局补 —— 这是**推断**，不是取证，
//     注释与交付里都这么写。⛔ 不把推断说成实测。
//
// ★ 语义（为什么"不是占位"）：
//   · `Begin(Flags)` 打开一个批次，只**记录** flags（ALPHABLEND 生效，其余"记录并明确忽略"，
//     未定义位也记录并忽略 —— 见 begin() 的注释）。
//   · `Draw(...)` 在**Draw 时**就把变换应用到四个角点（与 D3DX 的"顶点当场生成、Flush 才提交"一致），
//     把一次绘制压进 guest 堆上的批次表；⛔ 不在 Draw 里写像素。
//   · `End()` / `Flush()` 把批次**一次性合成到设备的 guest 后缓冲**（与 `d3d9::clear` 同一块
//     `SurfaceArena` 表面）：源 × 调制色（Color 的 A/R/G/B）→ 目标；按目标边界裁剪；
//     ALPHABLEND ⇒ SRCALPHA/INVSRCALPHA；⛔ 不是"只计数不写像素"。
//   · 纹理：`Draw` 支持两种源 ——
//     ① `pTexture == NULL`：D3D9 固定功能"没有绑定纹理 ⇒ 采样白色"（几何尺寸来自 pSrcRect；
//        pSrcRect 也为 NULL ⇒ 没有尺寸来源 ⇒ loud trap，⛔ 不猜 1×1）；
//     ② `pTexture` 是**本模块建的** `IDirect3DTexture9`（`d3d9::createTextureState` /
//        `D3DXCreateTexture`，像素在 `SurfaceArena`）：按源矩形采样纹理，越界源矩形**裁剪**，
//        Draw 时对纹理 AddRef（除非 Begin 给了 D3DXSPRITE_DO_NOT_ADDREF_TEXTURE）、End/Flush 对称
//        Release。⛔ 不是本模块的纹理对象 ⇒ loud trap（不猜像素）。
//     ③ `pTexture != 0` 且 `pSrcRect == NULL` ⇒ 用整张纹理（D3DX 语义）。
//   · 变换：`Draw` 时把当前 `SetTransform` 矩阵作用到四角（行向量约定 `v' = v*M`）；只建模 2D 仿射
//     部分（透视 / 非仿射矩阵会**明确记一行日志**后按仿射处理）。
//
// ★ 状态放哪儿：与 D3D9 设备同一套理由 —— 状态块（'SPR1'）与批次数组都从 **guest 堆**分配，
//   EA 挂在对象的 `obj+8` 扩展位（`ComRegistry::createObject(..., user)`）⇒ 生命周期跟 refcount 走。
// ★ 现状（2026-11-05）：boot 越过 Sprite + 纹理簇之后，在 `IDirect3DDevice9::SetTransform(+0xB0)`
//   停住（3D fixed-function 流水线 = 下一个**大子系统**，见 `.lst` 0x4A1D79 的
//   `mov edx,[ecx+0B0h]` → 0x4A1D89 call，参数 State=3/2/0x100 ⇒ PROJECTION/VIEW/WORLD）。
//   ⛔ 这不是本簇的缺口 —— 本簇（及纹理簇）的语义已经落到像素。
#pragma once

#include <cstdint>
#include <string>

namespace lift {

struct X86;

namespace d3dx9 {

// =====================================================================================
// D3DXSPRITE_*（值来自 SDK d3dx9core.h；下面 *** 标的是本线**真的**看的那一位）
// =====================================================================================
constexpr std::uint32_t kSpriteDoNotSaveState = 0x00000001u;
constexpr std::uint32_t kSpriteDoNotModifyRenderState = 0x00000002u;
constexpr std::uint32_t kSpriteObjectSpace = 0x00000004u;
constexpr std::uint32_t kSpriteBillboard = 0x00000008u;
constexpr std::uint32_t kSpriteAlphaBlend = 0x00000010u;  // *** 本线唯一生效的位
constexpr std::uint32_t kSpriteSortTexture = 0x00000020u;
constexpr std::uint32_t kSpriteSortDepthFrontToBack = 0x00000040u;
constexpr std::uint32_t kSpriteSortDepthBackToFront = 0x00000080u;
constexpr std::uint32_t kSpriteDoNotAddRefTexture = 0x00000100u;
constexpr std::uint32_t kSpriteKnownFlags = 0x000001FFu;

// HRESULT（与 d3d9 同一套值；这里只再导出**本簇用到的**几个，避免头文件互相依赖）
constexpr std::uint32_t kOk = 0x00000000u;
constexpr std::uint32_t kNoInterface = 0x80004002u;
constexpr std::uint32_t kInvalidArg = 0x80070057u;
constexpr std::uint32_t kFail = 0x80004005u;
constexpr std::uint32_t kInvalidCall = 0x8876086Cu;  // D3DERR_INVALIDCALL

// 批次条目在 guest 堆上的布局（**我们自己的**布局；⛔ 不是 Win32 结构体）。
// 每个条目 64 B = 一次 Draw 的全部输入 + **Draw 时**算好的四个屏幕空间角点。
constexpr std::uint32_t kBatchEntryBytes = 64u;
enum BatchOff : std::uint32_t {
    kBeTexture = 0,   // pTexture（本线只可能是 0；非 0 在 shim 就 loud trap）
    kBeFlags = 4,     // bit0 = 有 pSrcRect / bit1 = 有 pCenter / bit2 = 有 pPosition
    kBeSrc = 8,       // RECT：left +8 / top +12 / right +16 / bottom +20
    kBeQuad = 24,     // 四角：x0,y0 (u=0,v=0) · x1,y1 (u=w,v=0) · x2,y2 (u=0,v=h) · x3,y3
    kBeColor = 56,    // D3DCOLOR（0xAARRGGBB）
    kBeReserved = 60,
};
constexpr std::uint32_t kBeHasSrcRect = 0x1u;
constexpr std::uint32_t kBeHasCenter = 0x2u;
constexpr std::uint32_t kBeHasPosition = 0x4u;

// 一次 Sprite 的可观察状态（夹具 / 日志用；字段全部来自 guest 状态块）
struct SpriteInfo {
    std::uint32_t state = 0;   // 'SPR1' 状态块 EA
    std::uint32_t device = 0;  // 建它时的 IDirect3DDevice9 对象 EA
    std::uint32_t flags = 0;   // 最近一次 Begin 的 Flags
    std::uint32_t ignored_flags = 0;  // 最近一次 Begin 里**本线不建模**的位（记录，不静默）
    bool drawing = false;
    std::uint32_t begins = 0;
    std::uint32_t draws = 0;
    std::uint32_t flushes = 0;
    std::uint32_t ends = 0;
    std::uint32_t lost = 0;
    std::uint32_t resets = 0;
    std::uint32_t batch_count = 0;
    std::uint32_t batch_capacity = 0;
    std::uint32_t composed_pixels = 0;  // 累计合成过的像素数（诊断；⛔ 不是"画面判据"）
    float matrix[16] = {0};             // SetTransform 的当前值（默认单位阵）
};

// ---------------------------------------------------------------------------------------
// 状态块 / 对象
// ---------------------------------------------------------------------------------------
// 建 Sprite 状态块（guest 堆）+ 默认单位阵 + 空批次；返回状态块 EA（0 = 失败，why 点名）。
// ⛔ 不在这里建 COM 对象（shim 拿到 state 后调 ComRegistry::createObject）。
std::uint32_t createSpriteState(X86& cpu, std::uint32_t device_object, std::string* why);

bool spriteInfoOfState(X86& cpu, std::uint32_t state, SpriteInfo* out, std::string* why);
bool spriteInfo(X86& cpu, std::uint32_t object, SpriteInfo* out, std::string* why);

// 释放状态块 + 批次数组（Release 到 0 时由 shim 调；重复调用安全）。
void destroySpriteState(X86& cpu, std::uint32_t state);

// GetDevice 用：返回建它时那台设备对象 EA（0 = 状态坏）。
std::uint32_t spriteDevice(X86& cpu, std::uint32_t state);

// ---------------------------------------------------------------------------------------
// ID3DXSprite 方法（返回 HRESULT；`state` 是 Sprite 状态块 EA）
// ---------------------------------------------------------------------------------------
std::uint32_t begin(X86& cpu, std::uint32_t state, std::uint32_t flags, std::string* why);
// Draw：pTexture 只接受 0（非 0 由 shim loud trap，见文件头）；三个指针都可为 0。
std::uint32_t draw(X86& cpu, std::uint32_t state, std::uint32_t texture, std::uint32_t rect_ea,
                   std::uint32_t center_ea, std::uint32_t position_ea, std::uint32_t color,
                   std::string* why);
// Flush：合成当前批次并**保持** Begin 状态（批次清空）。
std::uint32_t flush(X86& cpu, std::uint32_t state, std::string* why);
// End：合成当前批次并关闭 Begin 状态。空批次 ⇒ 幂等 no-op（不改像素）。
std::uint32_t end(X86& cpu, std::uint32_t state, std::string* why);

std::uint32_t setTransform(X86& cpu, std::uint32_t state, std::uint32_t matrix_ea, std::string* why);
std::uint32_t getTransform(X86& cpu, std::uint32_t state, std::uint32_t matrix_ea, std::string* why);
std::uint32_t onLostDevice(X86& cpu, std::uint32_t state, std::string* why);
std::uint32_t onResetDevice(X86& cpu, std::uint32_t state, std::string* why);

// ---------------------------------------------------------------------------------------
// 合成（导出给夹具 / 日志；正常路径只经 end()/flush()）
// ---------------------------------------------------------------------------------------
// 把当前批次合成到设备后缓冲；返回合成条目数 / 像素数，失败 ⇒ false + why。
// ★ `pixels_written` 可以是 nullptr。
bool compositeBatch(X86& cpu, std::uint32_t state, std::uint32_t* entries, std::uint64_t* pixels_written,
                    std::string* why);

}  // namespace d3dx9
}  // namespace lift
