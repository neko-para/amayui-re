// emulator-lift · D3D9 **fixed-function 流水线**（M2 第一帧：设备状态机 + 顶点处理 + 软件光栅）
//
// ★ 范围（用户 2026-11-05 裁决"确认需要实现 D3D 接口"之后的第一帧判据）：
//     设备状态机（变换 / 渲染状态 / 采样器 / 纹理级状态 / 流 / 材质 / 灯光 / 视口 / 裁剪矩形）
//     → 固定功能 T&L（WORLD→VIEW→PROJECTION、**六平面裁剪**、视口映射）
//     → 软件光栅（重心坐标 + 左上填充规则、Z 测试/写、纹理采样、纹理级运算、alpha test、混合）
//   ⛔ 不是"返回 S_OK 就算画了"：真值落在**设备后缓冲的 guest 像素**上（与 `d3d9::clear` 同一块 SurfaceArena）。
//
// ★ 每像素状态读（2026-11-05 消除的偏差 #4）：纹理级 / 采样器状态**每次 Draw 只读一遍**
//   （`prepareContext` 里快照进 `DrawContext`），像素循环只读快照。可观察量：
//   `PipelineInfo::stage_snapshots`（= 完成准备的 Draw 次数；⛔ 不随三角形/像素增长）。
//
// ★ 裁剪（2026-11-05 消除的偏差 #5）：XYZ/XYZW 顶点在**齐次裁剪空间**对 6 个面裁剪
//   （近 `z>=0` / 远 `w-z>=0` / 左 `x+w>=0` / 右 `w-x>=0` / 下 `y+w>=0` / 上 `w-y>=0`）；
//   XYZRHW 顶点的 x/y 已是屏幕坐标 ⇒ 改为裁**视口矩形**（z 仍是 [0,1]）。未做的面**不静默**：
//   用户裁剪面（`D3DRS_CLIPPLANEENABLE`）仍只记录（`recordUnsupported`）；`D3DRS_CLIPPING=0`
//   （真机关裁剪）同样记录 —— 本线**总是**裁剪。
//
// ★ 与 `runtime/d3d9.{h,cpp}` 的分工：那边是"设备簇的叶子语义"（适配器 / caps / Clear / Present /
//   纹理状态块）；本文件是"**状态机 + 流水线**"（追加式，不改那边既有语义）。
//   状态块放哪：与设备状态块同一套理由 —— **guest 堆**上的 'D9PL' 块，EA 挂在设备状态块的 +56。
//   于是生命周期跟着设备 refcount 走（⛔ 不建宿主侧静态表：Machine 反复建在同一栈地址时会串台）。
//
// ★ 结构 / 枚举值的来源（⛔ 不凭记忆排 vtable）：
//   · 枚举值（D3DRS_* / D3DSAMP_* / D3DTSS_* / D3DFVF_* / D3DBLEND / D3DCMP / D3DPT …）来自
//     **SDK d3d9types.h**（Wine 的同名头是同一份 ABI 的公开副本，逐项对照过；值此处只抄用到的）。
//   · **vtable 偏移**由 `.lst` 实测决定，见 `shims/d3d9.cpp` 的方法表注释：那里的每一条都带站点 EA。
//     ⚠ 实测纠正了一处广为流传的记法：`SetFVF` 在 **+0x164（槽 89）**、`DrawPrimitive` 在
//     **+0x144（槽 81）**；`+0x124`（槽 73）是 `SetCurrentTexturePalette`。证据：
//       0x4A3610 `mov eax,[edx+164h]; push 244h` ⇒ SetFVF(0x244=XYZRHW|DIFFUSE|TEX1)
//       0x4A37C5 `mov ecx,[eax+144h]; push 2; push 0; push 5` ⇒ DrawPrimitive(TRIANGLESTRIP,0,2)
//       0x4A35FE `mov ecx,[eax+190h]; push 24h; push 0; push vb; push 0` ⇒ SetStreamSource(0,vb,0,0x24)
//       0x48E16D `mov eax,[ecx+14Ch]; push 1Ch; push data; push 1; push 2` ⇒ DrawPrimitiveUP(LINELIST,1,data,0x1C)
//
// ★ 判据口径（`SCOPE.md` §1 的平台偏差口径）：本流水线**只需自洽 + 确定性**，
//   ⛔ **不承诺与真机逐像素一致**。下面每一处"近似"都在实现里逐条标注（光照 / 雾 / 各向异性过滤 /
//   多重采样 / patch 等），未实现的位**不静默**：记录 + 一行日志（见 `recordUnsupported`）。
#pragma once

#include <cstdint>
#include <map>
#include <string>

#include "runtime/gpu_raster.h"  // GpuRasterState / GpuTexMode（GPU 准入的映射目标）
#include "runtime/surface.h"     // SurfaceFormat（后缓冲 / 纹理的像素布局）

namespace lift {

struct X86;

namespace d3d9 {

// =====================================================================================
// 枚举值（SDK d3d9types.h；只抄本线用到的）
// =====================================================================================
// D3DFVF_*
constexpr std::uint32_t kFvfPositionMask = 0x400Eu;
constexpr std::uint32_t kFvfXyz = 0x0002u;
constexpr std::uint32_t kFvfXyzRhw = 0x0004u;
constexpr std::uint32_t kFvfXyzw = 0x4002u;
constexpr std::uint32_t kFvfNormal = 0x0010u;
constexpr std::uint32_t kFvfPSize = 0x0020u;
constexpr std::uint32_t kFvfDiffuse = 0x0040u;
constexpr std::uint32_t kFvfSpecular = 0x0080u;
constexpr std::uint32_t kFvfTexCountMask = 0x0F00u;
constexpr std::uint32_t kFvfTexCountShift = 8u;

// D3DPRIMITIVETYPE
constexpr std::uint32_t kPtPointList = 1;
constexpr std::uint32_t kPtLineList = 2;
constexpr std::uint32_t kPtLineStrip = 3;
constexpr std::uint32_t kPtTriangleList = 4;
constexpr std::uint32_t kPtTriangleStrip = 5;
constexpr std::uint32_t kPtTriangleFan = 6;

// D3DTRANSFORMSTATETYPE（D3DTS_TEXTURE0..7 = 16..23；D3DTS_WORLD..WORLD3 = 256..259）
constexpr std::uint32_t kTsView = 2;
constexpr std::uint32_t kTsProjection = 3;
constexpr std::uint32_t kTsTexture0 = 16;
constexpr std::uint32_t kTsWorld = 256;

// D3DRENDERSTATETYPE
constexpr std::uint32_t kRsZEnable = 7;
constexpr std::uint32_t kRsFillMode = 8;
constexpr std::uint32_t kRsShadeMode = 9;
constexpr std::uint32_t kRsZWriteEnable = 14;
constexpr std::uint32_t kRsAlphaTestEnable = 15;
constexpr std::uint32_t kRsLastPixel = 16;
constexpr std::uint32_t kRsSrcBlend = 19;
constexpr std::uint32_t kRsDestBlend = 20;
constexpr std::uint32_t kRsCullMode = 22;
constexpr std::uint32_t kRsZFunc = 23;
constexpr std::uint32_t kRsAlphaRef = 24;
constexpr std::uint32_t kRsAlphaFunc = 25;
constexpr std::uint32_t kRsDitherEnable = 26;
constexpr std::uint32_t kRsAlphaBlendEnable = 27;
constexpr std::uint32_t kRsFogEnable = 28;
constexpr std::uint32_t kRsSpecularEnable = 29;
constexpr std::uint32_t kRsFogColor = 34;
constexpr std::uint32_t kRsFogTableMode = 35;
constexpr std::uint32_t kRsFogStart = 36;
constexpr std::uint32_t kRsFogEnd = 37;
constexpr std::uint32_t kRsFogDensity = 38;
constexpr std::uint32_t kRsStencilEnable = 52;
constexpr std::uint32_t kRsTextureFactor = 60;
constexpr std::uint32_t kRsClipping = 136;
constexpr std::uint32_t kRsLighting = 137;
constexpr std::uint32_t kRsAmbient = 139;
constexpr std::uint32_t kRsFogVertexMode = 140;
constexpr std::uint32_t kRsColorVertex = 141;
constexpr std::uint32_t kRsLocalViewer = 142;
constexpr std::uint32_t kRsNormalizeNormals = 143;
constexpr std::uint32_t kRsDiffuseMaterialSource = 145;
constexpr std::uint32_t kRsSpecularMaterialSource = 146;
constexpr std::uint32_t kRsAmbientMaterialSource = 147;
constexpr std::uint32_t kRsEmissiveMaterialSource = 148;
constexpr std::uint32_t kRsVertexBlend = 151;
constexpr std::uint32_t kRsClipPlaneEnable = 152;
constexpr std::uint32_t kRsMultiSampleAntiAlias = 161;
constexpr std::uint32_t kRsMultiSampleMask = 162;
constexpr std::uint32_t kRsIndexedVertexBlendEnable = 167;
constexpr std::uint32_t kRsColorWriteEnable = 168;
constexpr std::uint32_t kRsTweenFactor = 170;
constexpr std::uint32_t kRsBlendOp = 171;
constexpr std::uint32_t kRsScissorTestEnable = 174;
// ★ C 档（**值敏感**）：状态本身是合法 D3DRS，但本线不实现它的**有效值**；
//   惰性值 0 放行，非 0 ⇒ `D3DERR_INVALIDCALL` + `recordUnsupported(bit26)`，⛔ 不写状态块。
constexpr std::uint32_t kRsSlopeScaleDepthBias = 175;
constexpr std::uint32_t kRsAntiAliasedLineEnable = 176;
constexpr std::uint32_t kRsEnableAdaptiveTessellation = 184;
constexpr std::uint32_t kRsSrgbWriteEnable = 194;
constexpr std::uint32_t kRsDepthBias = 195;
constexpr std::uint32_t kRsSeparateAlphaBlendEnable = 206;
constexpr std::uint32_t kRsSrcBlendAlpha = 207;
constexpr std::uint32_t kRsDestBlendAlpha = 208;
constexpr std::uint32_t kRsBlendOpAlpha = 209;
// ★ D3D9 的 `D3DRENDERSTATETYPE` 最大合法值（210..255 **不是** D3DRS 值）：
//   Set/Get 的上界都用它；状态数组本身仍保留 256 槽（快照/Apply 按 256 项走，⛔ 不缩）。
constexpr std::uint32_t kLastD3dRenderState = kRsBlendOpAlpha;

// D3DBLEND
constexpr std::uint32_t kBlendZero = 1;
constexpr std::uint32_t kBlendOne = 2;
constexpr std::uint32_t kBlendSrcColor = 3;
constexpr std::uint32_t kBlendInvSrcColor = 4;
constexpr std::uint32_t kBlendSrcAlpha = 5;
constexpr std::uint32_t kBlendInvSrcAlpha = 6;
constexpr std::uint32_t kBlendDestAlpha = 7;
constexpr std::uint32_t kBlendInvDestAlpha = 8;
constexpr std::uint32_t kBlendDestColor = 9;
constexpr std::uint32_t kBlendInvDestColor = 10;
constexpr std::uint32_t kBlendSrcAlphaSat = 11;
constexpr std::uint32_t kBlendBothSrcAlpha = 12;
constexpr std::uint32_t kBlendBothInvSrcAlpha = 13;

// D3DBLENDOP
constexpr std::uint32_t kBlendOpAdd = 1;
constexpr std::uint32_t kBlendOpSubtract = 2;
constexpr std::uint32_t kBlendOpRevSubtract = 3;
constexpr std::uint32_t kBlendOpMin = 4;
constexpr std::uint32_t kBlendOpMax = 5;

// D3DCMPFUNC
constexpr std::uint32_t kCmpNever = 1;
constexpr std::uint32_t kCmpLess = 2;
constexpr std::uint32_t kCmpEqual = 3;
constexpr std::uint32_t kCmpLessEqual = 4;
constexpr std::uint32_t kCmpGreater = 5;
constexpr std::uint32_t kCmpNotEqual = 6;
constexpr std::uint32_t kCmpGreaterEqual = 7;
constexpr std::uint32_t kCmpAlways = 8;

// D3DCULL
constexpr std::uint32_t kCullNone = 1;
constexpr std::uint32_t kCullCw = 2;
constexpr std::uint32_t kCullCcw = 3;

// D3DSHADEMODE
constexpr std::uint32_t kShadeFlat = 1;
constexpr std::uint32_t kShadeGouraud = 2;

// D3DSAMPLERSTATETYPE
constexpr std::uint32_t kSampAddressU = 1;
constexpr std::uint32_t kSampAddressV = 2;
constexpr std::uint32_t kSampAddressW = 3;
constexpr std::uint32_t kSampBorderColor = 4;
constexpr std::uint32_t kSampMagFilter = 5;
constexpr std::uint32_t kSampMinFilter = 6;
constexpr std::uint32_t kSampMipFilter = 7;
constexpr std::uint32_t kSampMaxMipLevel = 9;
constexpr std::uint32_t kSampMaxAnisotropy = 10;

// D3DTEXTUREADDRESS
constexpr std::uint32_t kAddressWrap = 1;
constexpr std::uint32_t kAddressMirror = 2;
constexpr std::uint32_t kAddressClamp = 3;
constexpr std::uint32_t kAddressBorder = 4;
constexpr std::uint32_t kAddressMirrorOnce = 5;

// D3DTEXTUREFILTERTYPE
constexpr std::uint32_t kTexfNone = 0;
constexpr std::uint32_t kTexfPoint = 1;
constexpr std::uint32_t kTexfLinear = 2;
constexpr std::uint32_t kTexfAnisotropic = 3;

// D3DTEXTURESTAGESTATETYPE
constexpr std::uint32_t kTssColorOp = 1;
constexpr std::uint32_t kTssColorArg1 = 2;
constexpr std::uint32_t kTssColorArg2 = 3;
constexpr std::uint32_t kTssAlphaOp = 4;
constexpr std::uint32_t kTssAlphaArg1 = 5;
constexpr std::uint32_t kTssAlphaArg2 = 6;
constexpr std::uint32_t kTssTexCoordIndex = 11;
constexpr std::uint32_t kTssTextureTransformFlags = 24;
constexpr std::uint32_t kTssColorArg0 = 26;
constexpr std::uint32_t kTssAlphaArg0 = 27;
constexpr std::uint32_t kTssResultArg = 28;
constexpr std::uint32_t kTssConstant = 32;

// D3DTEXTUREOP
constexpr std::uint32_t kTopDisable = 1;
constexpr std::uint32_t kTopSelectArg1 = 2;
constexpr std::uint32_t kTopSelectArg2 = 3;
constexpr std::uint32_t kTopModulate = 4;
constexpr std::uint32_t kTopModulate2x = 5;
constexpr std::uint32_t kTopModulate4x = 6;
constexpr std::uint32_t kTopAdd = 7;
constexpr std::uint32_t kTopAddSigned = 8;
constexpr std::uint32_t kTopAddSigned2x = 9;
constexpr std::uint32_t kTopSubtract = 10;
constexpr std::uint32_t kTopAddSmooth = 11;
constexpr std::uint32_t kTopBlendDiffuseAlpha = 12;
constexpr std::uint32_t kTopBlendTextureAlpha = 13;
constexpr std::uint32_t kTopBlendFactorAlpha = 14;
constexpr std::uint32_t kTopBlendTextureAlphaPM = 15;
constexpr std::uint32_t kTopBlendCurrentAlpha = 16;
constexpr std::uint32_t kTopModulateAlphaAddColor = 18;
constexpr std::uint32_t kTopModulateColorAddAlpha = 19;
constexpr std::uint32_t kTopMultiplyAdd = 25;
constexpr std::uint32_t kTopLerp = 26;

// D3DTA_*
constexpr std::uint32_t kTaSelectMask = 0x0000000Fu;
constexpr std::uint32_t kTaDiffuse = 0x00000000u;
constexpr std::uint32_t kTaCurrent = 0x00000001u;
constexpr std::uint32_t kTaTexture = 0x00000002u;
constexpr std::uint32_t kTaTFactor = 0x00000003u;
constexpr std::uint32_t kTaSpecular = 0x00000004u;
constexpr std::uint32_t kTaTemp = 0x00000005u;
constexpr std::uint32_t kTaConstant = 0x00000006u;
constexpr std::uint32_t kTaComplement = 0x00000010u;
constexpr std::uint32_t kTaAlphaReplicate = 0x00000020u;

// D3DLIGHTTYPE
constexpr std::uint32_t kLightPoint = 1;
constexpr std::uint32_t kLightSpot = 2;
constexpr std::uint32_t kLightDirectional = 3;

// 本线自己的上限（与 D3DCAPS9 报的一致：8 个混合级 / 8 个同时纹理 / 8 个灯 / 16 个流）
constexpr std::uint32_t kMaxTextureStages = 8;
constexpr std::uint32_t kMaxSamplers = 16;
constexpr std::uint32_t kMaxStreams = 16;
constexpr std::uint32_t kMaxLights = 8;

// =====================================================================================
// 可观察状态（夹具 / 日志；字段全部来自 guest 状态块）
// =====================================================================================
struct PipelineInfo {
    std::uint32_t state = 0;      // 'D9PL' 状态块 EA
    std::uint32_t device = 0;     // 设备状态块 EA
    std::uint32_t fvf = 0;        // SetFVF 的当前值
    std::uint32_t vertex_decl = 0;  // SetVertexDeclaration 的对象 EA（0 = FVF 路）
    std::uint32_t depth_arena = 0;  // 深度缓冲（SurfaceArena 的 32bpp 表面，像素存 float 位模式）
    std::uint32_t color_arena = 0;  // 当前渲染目标像素块（0 = 设备后缓冲）
    std::uint32_t color_surface = 0;  // 当前渲染目标的 IDirect3DSurface9 对象（0 = 后缓冲）
    bool in_scene = false;
    std::uint32_t begins = 0;     // BeginScene 成功次数
    std::uint32_t ends = 0;       // EndScene 成功次数
    std::uint32_t draws = 0;      // 通过前置检查的 Draw 次数
    std::uint32_t draw_calls = 0; // 进入 Draw 的次数（含失败）
    std::uint32_t stage_snapshots = 0;  // 纹理级/采样器状态快照次数（每次 Draw 一次；见文件头）
    std::uint32_t triangles = 0;  // 进入光栅的三角形数（含被裁掉的）
    std::uint32_t rasterized = 0; // 真正光栅化（过了背面剔除）的三角形数
    std::uint64_t pixels = 0;     // 通过深度/alpha 测试并写进后缓冲的像素数
    std::uint32_t viewport_x = 0;
    std::uint32_t viewport_y = 0;
    std::uint32_t viewport_w = 0;
    std::uint32_t viewport_h = 0;
    // ★ 未支持状态的位图（8 × 32 = 256 位；诊断，不是设备状态）。
    //   ★ 为什么必须在 `PipelineInfo` 里：位图以前**只写不读** ⇒ 守卫只能断言"日志里出现过"。
    //   位名见 `d3d9_pipeline.cpp` 的 `enum UnsupportedBit`。
    std::uint32_t unsupported[8] = {};
};

// =====================================================================================
// 采样过滤决策（pure；夹具直接调，⛔ 不再"参数传了却不用"）
// =====================================================================================
// 两个过滤器（MAG / MIN）的要求并起来：**任一**要求 LINEAR（含 ANISOTROPIC，按 LINEAR 近似）
// 就走双线性，否则走 POINT。★ 这是"按 LOD 分别选 MIN/MAG"的**近似**（本线不做各向异性）；
// mip 级的选择在下面 `mipLevelForLod`（`REQ-01M73G2724479XAVS176WBZ65Q`）。
bool samplerUsesLinear(std::uint32_t mag_filter, std::uint32_t min_filter);

// =====================================================================================
// 派生 mip 的 LOD 选择（REQ-01M73G2724479XAVS176WBZ65Q；纯函数 ⇒ 守卫直接打）
//
// ★ 判据链（每三角形常量，⛔ 不做逐像素）：屏幕空间 UV 导数（texel / 屏幕像素）→ LOD → 级号。
//   · "1:1 或放大时**仍走 level 0**"靠 `mipLevelForLod` 的 clamp 保证（放大 ⇒ lod<0 ⇒ 0 级）；
//   · 采到的级是**最近级**（不做三线性）；`MIPFILTER = NONE` 时调用方根本不选级（只 level 0）。
// =====================================================================================
// 透视校正后的屏幕空间 UV 导数：顶点 = 屏幕 (x,y) + `1/w` + `u/w` / `v/w`（与 `SVertex` 同口径）。
// ★ 式子与采样器**同一个**：`u = U/W`，`U = Σ l_i·(u_i/w_i)`、`W = Σ l_i·(1/w_i)` ⇒
//   `du/dx = (U_x − u·W_x) / W`。⛔ 不要直接对"顶点 uv"做仿射梯度 —— 那在 w≠1 时系统性错。
// 退化三角形（|det| 太小）⇒ 返回 false 且导数全 0（调用方按 level 0 走）。
bool screenUvDerivatives(const double vx[3], const double vy[3], const double invw[3],
                         const double u_over_w[3], const double v_over_w[3], double* du_dx, double* dv_dx,
                         double* du_dy, double* dv_dy);
// 纹理足迹（texel / 屏幕像素）= max(两轴的 |d(uv)| 在**该纹理尺寸**上的长度) ⇒ `LOD = log2(足迹)`。
// 尺寸 <= 0 / 非有限 ⇒ 返回一个必然落到 0 级的 LOD（`-1000`）。
double lodFromUvDerivatives(double du_dx, double dv_dx, double du_dy, double dv_dy, double tex_width,
                            double tex_height);
// LOD → 级号：`clamp(floor(lod + 0.5), 0, levels-1)`（最近级）。非有限 / `levels <= 1` ⇒ 0。
std::uint32_t mipLevelForLod(double lod, std::uint32_t levels);

// =====================================================================================
// 状态块生命周期（createDeviceState / destroyDeviceState 里挂钩）
// =====================================================================================
// 建流水线状态块（guest 堆）+ 深度缓冲（SurfaceArena）。返回 EA；0 = 失败（why 点名）。
std::uint32_t createForDevice(X86& cpu, std::uint32_t device_state, int width, int height, std::string* why);
void destroyForDevice(X86& cpu, std::uint32_t device_state);
std::uint32_t pipelineOfDevice(X86& cpu, std::uint32_t device_state);
bool pipelineInfoOfState(X86& cpu, std::uint32_t state, PipelineInfo* out, std::string* why);
bool pipelineInfoOfDevice(X86& cpu, std::uint32_t device_state, PipelineInfo* out, std::string* why);
// 运行摘要的一行 `unsupported : …`（进程级镜像；判据以 `PipelineInfo::unsupported` 为准）。
//   ★ `non_benign_only=true` ⇒ 跳过"已知并接受"的近似位（见下）；**全被跳过 ⇒ 返回空串**
//     （异常段据此不打这一行）。`false` ⇒ 全量位图（`--verbose` / `--profile` 用）。
std::string unsupportedBitsOfRun(bool non_benign_only = false);
// 纯函数形态（不读进程镜像）：同上的过滤 + 具名表，供守卫直接构造位图断言。
std::string unsupportedBitsText(const std::uint32_t* bits, bool non_benign_only);
// 该位是否属于"已知并接受的近似"：位照旧记录，只是**默认不当异常**。
//   目前只有 bit11 `MIPFILTER`（引擎设 `D3DSAMP_MIPFILTER=LINEAR`，GPU sampler 三线性按最近级近似
//   ⇒ 每个正常 2D 场景都置位；它进异常段等于恒真噪声）。
bool unsupportedBitIsBenign(std::uint32_t bit);
// 进程镜像里的某一位是否置位（`read :` 行的 `mipfilter≈nearest` 标记用）。
bool unsupportedBitOfRun(std::uint32_t bit);

// =====================================================================================
// IDirect3DStateBlock9 的状态快照（`REQ-01M7372BPVGBD247KAGHB53YK7`）
//
// ★ 为什么要它：引擎每帧 Live2D 都走 `CreateStateBlock(D3DSBT_ALL=1) → draw → Apply → Release`
//   （`.lst` 0x47843A–0x47847B）。修前 Capture / Apply 是纯 stub（直接 S_OK）⇒ draw 里改的
//   FVF / 纹理绑定 / 渲染状态 / 变换**不还原**，泄漏到后续 2D 绘制。
// ★ 快照放**宿主堆**（⛔ 不是 guest 可见内存）。guest 只看到状态块对象的 `user` 扩展块
//   `'SB09'`（16 B，下面那 5 个常量）：magic / 设备对象 EA / 进程内唯一 serial / D3DSBT 类型。
//   快照本体按 serial 存在 `d3d9_pipeline.cpp` 的宿主表里。
// ★ 为什么是 "guest 句柄 + `on_zero`"，而不是"宿主表按 `(ComRegistry*, 代际)` 做键"：
//   `ComRegistry` 的代际**每个 registry 各自从 1 起**（`runtime/com.h`），而夹具的 Machine 反复
//   建在同一栈地址 ⇒ 那种键会跨 Machine 串台（本文件头第 22 行那句禁令说的就是它）。句柄里的
//   serial 只由 `shims/d3d9.cpp` 的 `nextStateBlockSerial()` 发（进程内只增不复用）；回收挂在
//   `createObject` 的 `on_zero` = `destroyStateBlockSnapshot`（对象销毁的**唯一收敛点**，见
//   `runtime/com.h` 文件头）⇒ guest Release 与将来任何宿主侧裸 release 都覆盖得到。
constexpr std::uint32_t kSbMagic = 0;                  // 'SB09'
constexpr std::uint32_t kSbMagicValue = 0x39304253u;  // 'SB09'
constexpr std::uint32_t kSbDevice = 4;                 // 设备**对象** EA（GetDevice / Apply 从这里取）
constexpr std::uint32_t kSbSerial = 8;                 // 进程内唯一 serial（宿主快照表的键；0 = 未分配）
constexpr std::uint32_t kSbType = 12;                  // D3DSBT_ALL(1) / PIXELSTATE(2) / VERTEXSTATE(3)
constexpr std::uint32_t kSbBytes = 16;

// 捕获（`CreateStateBlock` = 首次、`Capture` = 重捕；同一个实现：宿主表里已有该 serial 就替换）。
// 返回 S_OK / E_OUTOFMEMORY（快照分配失败，⛔ 不 trap、⛔ 不留半个快照）/
//      D3DERR_INVALIDCALL（句柄 / 设备 / 流水线状态不对）。
std::uint32_t captureStateBlock(X86& cpu, std::uint32_t handle, std::string* why, bool* out_of_memory);
// 写回：**先**用既有 setter（`setTexture` / `setStreamSource` / `setIndices` / `setVertexDeclaration`
// / `setFVF`）恢复四类绑定 —— 引用计数 / 代际 / 身份校验 / 日志全走同一份既有约定，⛔ 不新造一套；
// **再**写纯标量区（渲染状态 / 变换 / 采样器 / 纹理级 / 视口 / 裁剪矩形 / 材质 / 灯）。
// 任一绑定恢复失败 ⇒ 记一条 `pipeWarn`（默认可见）并返回 D3DERR_INVALIDCALL（已写回的标量不回滚，
// 但⛔ 不谎报 S_OK）。
std::uint32_t applyStateBlock(X86& cpu, std::uint32_t handle, std::string* why);
// `createObject` 的 `on_zero`：把快照持有的绑定引用还回去（`releaseIf`，带代际）+ 从宿主表摘除
// + free 句柄块。**幂等**（先清 magic；重复调用只返回）。
void destroyStateBlockSnapshot(X86& cpu, std::uint32_t handle);

// =====================================================================================
// 方法语义（返回 HRESULT；`dev` 是设备状态块 EA）
// ⚠ 每个函数都先自己校验参数（真机的失败码），失败**不**改状态。
// =====================================================================================
// 当前渲染目标像素块（0 = 设备后缓冲）。Clear / Draw / 夹具都读这一份。
std::uint32_t renderTargetArena(X86& cpu, std::uint32_t pipeline_state);

// ★ 当前颜色目标的**唯一一份**判据（`prepareContext` 与 D3DXSprite 合成共用）：
//   Sprite 的 End/Flush 合成目标必须与 D3D9 的 Draw 目标同源 —— 绑定纹理 RT 时两者都要写进
//   那张纹理的像素块（`REQ-01M79V2TVJH98BKH0C0WQV2FET`），⛔ 不要各读一遍状态块、
//   更不要复制一份"是不是纹理"的判断。
//   · `arena`   = `kPlColorArena`（0 = 设备后缓冲；非 0 = 绑定的 surface 像素块）；
//   · `surface` = `kPlColorSurface`（0 = 后缓冲）；
//   · `is_texture` / `usage` = surface 是纹理表面（`'TEX1'` magic）及其 `D3DUSAGE_*` 位。
struct ColorTarget {
    std::uint32_t arena = 0;
    std::uint32_t surface = 0;
    bool is_texture = false;
    std::uint32_t usage = 0;
};
bool colorTargetOfState(X86& cpu, std::uint32_t pipeline_state, ColorTarget* out, std::string* why);
// SetRenderTarget：target=0 ⇒ 后缓冲；否则必须是本模块的 IDirect3DSurface9。
std::uint32_t setRenderTarget(X86& cpu, std::uint32_t dev, std::uint32_t target, std::uint32_t zstencil,
                              std::string* why);
std::uint32_t beginScene(X86& cpu, std::uint32_t dev, std::string* why);
std::uint32_t endScene(X86& cpu, std::uint32_t dev, std::string* why);
std::uint32_t setTransform(X86& cpu, std::uint32_t dev, std::uint32_t state, std::uint32_t matrix_ea,
                           std::string* why);
std::uint32_t getTransform(X86& cpu, std::uint32_t dev, std::uint32_t state, std::uint32_t matrix_ea,
                           std::string* why);
std::uint32_t multiplyTransform(X86& cpu, std::uint32_t dev, std::uint32_t state, std::uint32_t matrix_ea,
                                std::string* why);
std::uint32_t setRenderState(X86& cpu, std::uint32_t dev, std::uint32_t state, std::uint32_t value,
                             std::string* why);
std::uint32_t getRenderState(X86& cpu, std::uint32_t dev, std::uint32_t state, std::uint32_t out_ea,
                             std::string* why);
std::uint32_t setSamplerState(X86& cpu, std::uint32_t dev, std::uint32_t stage, std::uint32_t type,
                              std::uint32_t value, std::string* why);
std::uint32_t getSamplerState(X86& cpu, std::uint32_t dev, std::uint32_t stage, std::uint32_t type,
                              std::uint32_t out_ea, std::string* why);
std::uint32_t setTextureStageState(X86& cpu, std::uint32_t dev, std::uint32_t stage, std::uint32_t type,
                                   std::uint32_t value, std::string* why);
std::uint32_t getTextureStageState(X86& cpu, std::uint32_t dev, std::uint32_t stage, std::uint32_t type,
                                   std::uint32_t out_ea, std::string* why);
// SetTexture：伪 0 / 本模块建的纹理对象 EA。真机会对绑定的纹理 AddRef（本线照做）。
std::uint32_t setTexture(X86& cpu, std::uint32_t dev, std::uint32_t stage, std::uint32_t texture,
                         std::string* why);
// GetTexture / GetVertexDeclaration：都**借出一份新引用**（出参带 +1；调用方必须 Release）。
//   GetTexture 的 AddRef 是微软文档明确写的；GetVertexDeclaration 的页面没写，按 D3D9 `Get*`
//   家族一致性办（缺陷单 REQ-01M701NYF2B3YV1YFSZBMPQZ41 的裁决）。⛔ 别再把裸 EA 交回去。
std::uint32_t getTexture(X86& cpu, std::uint32_t dev, std::uint32_t stage, std::uint32_t out_ea,
                         std::string* why);
std::uint32_t setFVF(X86& cpu, std::uint32_t dev, std::uint32_t fvf, std::string* why);
std::uint32_t getFVF(X86& cpu, std::uint32_t dev, std::uint32_t out_ea, std::string* why);
// SetVertexDeclaration / SetStreamSource / SetIndices（绑定槽的契约，段 2 之后）：
//   · 入参是 guest 可见的**对象 EA**；槽里存 **(对象 EA, 绑定时代际)** + 该流自己的 offset/stride。
//     ⛔ 不再存状态块 EA —— EA 会被 guest 堆复用，"只验 magic"会让同长度新缓冲静默顶替旧缓冲。
//   · 三者都**替 guest 持一份绑定引用**（与 `setTexture` 同口径）：换绑 / 解绑（参数给 0）/ 设备销毁
//     时成对还回去（`releaseIf(EA, 代际)`，代际不符只出声、不减到新占有者）。guest 可见差异：
//     绑定期对绑定对象 `Release` 的返回值是 +1（例如 `SetStreamSource` 之后 guest 放自己那份 = 1）。
//   · Draw 侧读槽时先过身份校验（`find` + 代际 + iface + 状态块 magic）；不符 ⇒ `D3DERR_INVALIDCALL`
//     + **默认级别可见**的告警（`pipeWarn`），⛔ 绝不改读别人的顶点 / 索引。
std::uint32_t setVertexDeclaration(X86& cpu, std::uint32_t dev, std::uint32_t decl, std::string* why);
std::uint32_t getVertexDeclaration(X86& cpu, std::uint32_t dev, std::uint32_t out_ea, std::string* why);
std::uint32_t setViewport(X86& cpu, std::uint32_t dev, std::uint32_t viewport_ea, std::string* why);
std::uint32_t getViewport(X86& cpu, std::uint32_t dev, std::uint32_t viewport_ea, std::string* why);
std::uint32_t setScissorRect(X86& cpu, std::uint32_t dev, std::uint32_t rect_ea, std::string* why);
std::uint32_t getScissorRect(X86& cpu, std::uint32_t dev, std::uint32_t rect_ea, std::string* why);
// SetMaterial / SetLight / LightEnable：D3DMATERIAL9（68 B）/ D3DLIGHT9（104 B）整块拷进状态块。
std::uint32_t setMaterial(X86& cpu, std::uint32_t dev, std::uint32_t material_ea, std::string* why);
std::uint32_t setLight(X86& cpu, std::uint32_t dev, std::uint32_t index, std::uint32_t light_ea,
                       std::string* why);
std::uint32_t lightEnable(X86& cpu, std::uint32_t dev, std::uint32_t index, std::uint32_t enable,
                          std::string* why);
std::uint32_t validateDevice(X86& cpu, std::uint32_t dev, std::uint32_t passes_ea, std::string* why);

// 顶点缓冲（IDirect3DVertexBuffer9 的状态块 'VB09'；顶点数据同样落在 guest 堆）
std::uint32_t createVertexBufferState(X86& cpu, std::uint32_t device_object, std::uint32_t length,
                                      std::uint32_t usage, std::uint32_t fvf, std::uint32_t pool,
                                      std::string* why);
bool vertexBufferInfoOfState(X86& cpu, std::uint32_t state, std::uint32_t* length, std::uint32_t* data_ea,
                             std::uint32_t* fvf, std::string* why);
std::uint32_t vertexBufferDevice(X86& cpu, std::uint32_t state);  // 0 = 状态块无效
void destroyVertexBufferState(X86& cpu, std::uint32_t state);
std::uint32_t vertexBufferLock(X86& cpu, std::uint32_t state, std::uint32_t offset, std::uint32_t size,
                               std::uint32_t out_ea, std::uint32_t flags, std::string* why);
std::uint32_t vertexBufferUnlock(X86& cpu, std::uint32_t state, std::string* why);

// 索引缓冲（'IB09'；D3DFMT_INDEX16=101 / INDEX32=102）
std::uint32_t createIndexBufferState(X86& cpu, std::uint32_t device_object, std::uint32_t length,
                                     std::uint32_t usage, std::uint32_t format, std::uint32_t pool,
                                     std::string* why);
bool indexBufferInfoOfState(X86& cpu, std::uint32_t state, std::uint32_t* length, std::uint32_t* data_ea,
                            std::uint32_t* format, std::string* why);
std::uint32_t indexBufferDevice(X86& cpu, std::uint32_t state);  // 0 = 状态块无效
void destroyIndexBufferState(X86& cpu, std::uint32_t state);
std::uint32_t indexBufferLock(X86& cpu, std::uint32_t state, std::uint32_t offset, std::uint32_t size,
                              std::uint32_t out_ea, std::uint32_t flags, std::string* why);
std::uint32_t indexBufferUnlock(X86& cpu, std::uint32_t state, std::string* why);

// SetStreamSource / SetIndices（契约见上面 `setVertexDeclaration` 那一段：对象 EA + 绑定时代际、
// 持绑定引用、Draw 侧身份校验；`vb`/`ib` 给 0 = 解绑并还回旧绑定的那份引用）。
std::uint32_t setStreamSource(X86& cpu, std::uint32_t dev, std::uint32_t stream, std::uint32_t vb,
                              std::uint32_t offset, std::uint32_t stride, std::string* why);
std::uint32_t setIndices(X86& cpu, std::uint32_t dev, std::uint32_t ib, std::string* why);

// Draw*：真的做顶点处理 + 光栅。**未 BeginScene ⇒ D3DERR_INVALIDCALL**（真机语义）。
std::uint32_t drawPrimitive(X86& cpu, std::uint32_t dev, std::uint32_t prim, std::uint32_t start_vertex,
                            std::uint32_t prim_count, std::string* why);
std::uint32_t drawIndexedPrimitive(X86& cpu, std::uint32_t dev, std::uint32_t prim, std::int32_t base_vertex,
                                   std::uint32_t min_index, std::uint32_t num_vertices, std::uint32_t start_index,
                                   std::uint32_t prim_count, std::string* why);
std::uint32_t drawPrimitiveUP(X86& cpu, std::uint32_t dev, std::uint32_t prim, std::uint32_t prim_count,
                              std::uint32_t data_ea, std::uint32_t stride, std::string* why);
std::uint32_t drawIndexedPrimitiveUP(X86& cpu, std::uint32_t dev, std::uint32_t prim, std::uint32_t min_index,
                                     std::uint32_t num_vertices, std::uint32_t prim_count,
                                     std::uint32_t index_ea, std::uint32_t index_format, std::uint32_t data_ea,
                                     std::uint32_t stride, std::string* why);

// Clear(D3DCLEAR_ZBUFFER) 的深度清（把深度缓冲写成 z；没有流水线状态块 ⇒ false）
bool clearDepth(X86& cpu, std::uint32_t pipeline_ea, float z, std::string* why);

// GPU 光栅化诊断（static 计数；CPU 模式恒为 0）。供 dumpRunSummary / CTest 断言。
std::uint64_t gpuDrawCount();
std::uint64_t gpuTriangleCount();

// `[draw] fa=` 的**取数口**（`REQ-01M78BF1FNNH6K8N8GEY6GQYXX`）：上一批 Draw 的"真正参与混合的 α"。
// · `from_gpu == false` ⇒ 数的是 CPU 光栅**真的写下去的像素**（`n` = 像素数，受 `LIFT_SLOW_DRAW` 门控）；
// · `from_gpu == true`  ⇒ 数的是交给 GPU 后端的**顶点** α（`n` = 顶点数，逐批、不受门控）。
//   ⛔ 两者都**不是**粗略估计：CPU 走像素收尾的唯一一处，GPU 走真的入队的那批顶点。
// · `mn/mx/mean` 全是 0..255 的整数语义（`mean` 是 double 但由整数和算）。
struct DrawAlphaAgg {
    std::uint64_t n = 0;
    double mean = 0.0;
    unsigned mn = 255;
    unsigned mx = 0;
    bool from_gpu = false;
};
DrawAlphaAgg lastDrawAlphaAgg();

// =====================================================================================
// GPU 准入 / 状态映射（**纯函数**；夹具直接调，⛔ 不建机器）
//
// ★ 把 D3D9 的混合状态映射到 `GpuRasterState` 的 color/alpha 两段。
//   · 返回 false = 这个组合**不能**用 SDL_GPU 等价表达 ⇒ 调用方必须回退 CPU 并记账。
//   · 未开启 `blend` 时忽略 factor/op（真机语义：ALPHABLENDENABLE=0 不用它们）。
//   · separate alpha 只有在 `blend` 为真时才生效（与真机一致），但映射本身照做。
// ★ 纯函数：不读 guest、不碰 SDL；守卫 `tests/gpu_mapping.cpp` 用 TITLE 的 5/6 + 2/6 做像素级
//   期望，另一个用例用 DESTCOLOR 证明"不支持的组合会红"。
// =====================================================================================
bool gpuBlendStateFor(std::uint32_t src_color, std::uint32_t dst_color, std::uint32_t color_op,
                      bool separate_alpha, std::uint32_t src_alpha, std::uint32_t dst_alpha,
                      std::uint32_t alpha_op, bool blend_enabled, GpuRasterState* out, std::string* why);

// 纹理 stage 0 的 (color op/args, alpha op/args) → GpuTexMode。
// `tex_coord_index` 是 D3DTSS_TEXCOORDINDEX 的值（TITLE 用它选 FVF 坐标，⛔ 不硬编码 TEXCOORD0）：
// 只接受 0..7（本线的 FVF 纹理坐标上限），越界 ⇒ false。
// 只接受 TEXTURE / DIFFUSE（stage 0 的 CURRENT 等价于 DIFFUSE）/ TFACTOR 的原样选择
// （⛔ 不处理 COMPLEMENT / ALPHAREPLICATE / TEMP）。TFACTOR 是**每次 Draw 的常量色**：
// 需要它时 `*color_from_tfactor=true`，调用方把 GPU 顶点色整批替换成 TFACTOR
// （TITLE 实测就是 MODULATE(TEXTURE, TFACTOR) 的 color+alpha）。
bool gpuTextureModeFor(std::uint32_t color_op, std::uint32_t color_arg1, std::uint32_t color_arg2,
                       std::uint32_t alpha_op, std::uint32_t alpha_arg1, std::uint32_t alpha_arg2,
                       bool texture_present, std::uint32_t tex_coord_index, GpuTexMode* mode,
                       bool* color_from_tfactor, std::string* why);

// 无纹理（CPU 采样=白色）时，stage 0 的 op/arg 是否**恒等于顶点色**（GPU 无纹理着色器只输出顶点色）。
// `DISABLE` 按 CPU 语义取 arg1（⛔ 不是无条件 DIFFUSE）——所以 DISABLE(arg1=TEXTURE) 必须 false。
bool gpuUntexturedIsIdentity(std::uint32_t op, std::uint32_t arg1, std::uint32_t arg2);

// =====================================================================================
// GPU 覆盖账本（每帧 / 每次 Draw；⛔ 不把回退清零冒充覆盖）
//
// · `draw_calls` / `gpu_draws` / `fallback_draws` = D3D9 `Draw*` 的**批次数**
//   （一次 Draw* 可能产生多个三角形/多批，仍算一次调用）。
// · `fallback_pixels` = 这一批真的由 CPU 软光栅写进后缓冲的像素数（不是包围盒估计）。
// · `sprite_*` = D3DXSprite 的每个批次条目（TITLE 的 Sprite 路径单独可见）；
//   `sprite_skipped` = 被裁剪掉的条目（空源矩形 / 屏外 / 退化），**不是** fallback。
// · `fallback_reasons` 按**原因字符串**聚合；`gpuNoteFallback` 在进程内首次出现某原因时
//   打印一行 `[gpu] fallback ...` 到 stderr（同原因不刷屏）。
// ★ 本单（`REQ-01M73PZN6R3AC3RCQ8WZEA0Y0M`）把"入队成功"与"真的覆盖"拆成三个数：
//   · `queued`    = 至少成功入队 1 条 draw 的**批数**（`rasterQueue` 返回 true 且顶点非空）；
//   · `recorded`  = 其中后端真的把它们录进过 render pass 的批数 —— 判据在后端**命令缓冲提交
//                   成功之后**的回报（`gpuLedgerNoteRecordedDraw`），⛔ 不是 `rasterQueue` 的返回值；
//   · `fragments` = 入队的 GPU **顶点**数（片元数后端拿不到 ⇒ 给能拿到的那个量）。
//   `empty` = `gpu_draws` 账上"覆盖了"、却没有一条 draw 真的进过 render pass 的批数
//             （`queued>0 && recorded==0` 是它的主要形态；**也包含** `queued==0` 那种更坏的情况：
//             顶点全被裁掉 ⇒ 连队都没入）。
// =====================================================================================
struct GpuCoverage {
    std::uint64_t draw_calls = 0;
    std::uint64_t gpu_draws = 0;
    // ★ `REQ-01M7AAXVGY9SG75XCREHPX641Z`（M1 账本）：两个数**分开**，⛔ 不许合流。
    //   · `fallback_draws` = 这一批 **CPU 真写了像素**（`fallback_pixels > 0`）—— "回退"的口径；
    //   · `skipped_draws`  = 这一批 **GPU 没画**（准入拒绝 / 准入后拒绝 / 后端失败）—— "没画"的口径。
    //   本单之前只有前者 ⇒ `gpuEligible` 拒批且 CPU 一个像素都没写时**连账都没有**（设计 §A.3 的盲区）。
    //   真实场景（TITLE / ADV / OPTION / SAVE）两者都应 0。
    std::uint64_t fallback_draws = 0;
    std::uint64_t fallback_pixels = 0;
    std::uint64_t skipped_draws = 0;
    std::uint64_t gpu_failures = 0;  // 准入通过、但 GPU 后端真的画失败了（⛔ 不再 CPU 重放该批）
    // ---- 本单：入队 / 录进 pass / 顶点数（口径见上）----
    std::uint64_t queued = 0;     // 入队成功的批数
    std::uint64_t recorded = 0;   // 真的进过 render pass 的批数
    std::uint64_t empty = 0;      // 账上覆盖、但 recorded==0 的批数（= 没有像素的"已覆盖"）
    std::uint64_t fragments = 0;  // 入队的 GPU 顶点数
    std::uint64_t sprite_draws = 0;
    std::uint64_t sprite_gpu_draws = 0;
    std::uint64_t sprite_fallback_draws = 0;
    std::uint64_t sprite_fallback_pixels = 0;
    // sprite 条目被**裁剪掉**（空源矩形 / 退化 / 完全屏外 / 源矩形在纹理外）而没画：不是回退，
    // 但覆盖账要可见（否则"没画"与"没走 GPU"分不清）。
    std::uint64_t sprite_skipped = 0;
    std::map<std::string, std::uint64_t> fallback_reasons;
    // `skipped` 的按原因聚合（与 `fallback_reasons` **不同一张表**：口径不同，⛔ 不合流）。
    std::map<std::string, std::uint64_t> skipped_reasons;
};
const GpuCoverage& gpuCoverage();
void gpuCoverageReset();
// `skipped top:` 那一行（与 `fallback top:` 同形；最多 5 条，按次数降序、再按原因升序）。
// ★ 返回**整行**（含换行）；没有跳过时返回空串。`machine.cpp` 直接 fputs 它；
//   夹具（`tests/gpu_batch.cpp` 的 `gpu-batch/skip-ledger`）断言原因真的出现在这里。
std::string gpuSkippedTopLine();
// 记一次 CPU 回退（reason 是稳定文本；pixels 是这一批 CPU 写出的像素数）。
void gpuNoteFallback(const std::string& reason, std::uint64_t pixels);
// 记一次 D3DXSprite 条目的 GPU / CPU 结果（CPU 时 reason 必填）。
void gpuNoteSpriteResult(bool gpu, std::uint64_t pixels, const std::string& reason);
// 记一次 D3DXSprite 条目被裁剪（不计 fallback、不计 GPU）。
void gpuNoteSpriteSkipped();

// =====================================================================================
// 默认级别告警（`pipeWarn`）的**配额类别**与**拒绝计数**
//
// · 配额 key = **(调用点, 类别)**（`REQ-01M70SHFT6RZA7RRY1R051Z2PB` #2）：同一个 `DrawPrimitive`
//   里的"顶点流陈旧"与"纹理陈旧"是两个桶 ⇒ 一类每帧刷屏不会吃掉另一类的可见配额。
//   ⛔ 退回"只用调用点名"就是原始缺陷（第 5 次起两类告警一起变 debug）。
// · 每类拒绝次数（**与限流桶同一份**，进程级）进 `dumpRunSummary` 的 `pipe-warn` 行 ——
//   "默认级别看不见"的事件在摘要里仍然数得到。
// =====================================================================================
enum class PipeWarnCategory : unsigned {
    VertexStream = 0,  // 顶点流 / 顶点缓冲绑定
    Index = 1,         // 索引缓冲绑定
    Texture = 2,       // 纹理绑定
    Other = 3,         // 顶点声明 / 其它（含设备销毁时的绑定释放）
};
constexpr unsigned kPipeWarnCategoryCount = 4;

struct PipeWarnCounts {
    std::uint64_t by_category[kPipeWarnCategoryCount] = {0, 0, 0, 0};
    std::uint64_t of(PipeWarnCategory cat) const { return by_category[static_cast<unsigned>(cat)]; }
    std::uint64_t total() const {
        return by_category[0] + by_category[1] + by_category[2] + by_category[3];
    }
};
const PipeWarnCounts& pipeWarnCounts();

// Present 的像素统计（"非背景像素数"的判据；用后缓冲像素算，⛔ 不是猜）
struct FrameStats {
    std::uint32_t total_pixels = 0;
    std::uint32_t background_pixels = 0;   // 出现次数最多的那个像素值
    std::uint32_t background_color = 0;    // 它是什么颜色（原始像素值）
    std::uint32_t non_background_pixels = 0;
    std::uint32_t distinct_colors = 0;
    std::uint64_t hash = 0;
};
bool frameStats(const SurfaceFormat& fmt, const std::uint8_t* base, std::uint32_t pitch, int width,
                int height, FrameStats* out);

}  // namespace d3d9
}  // namespace lift
