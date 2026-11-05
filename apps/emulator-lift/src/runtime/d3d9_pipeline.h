// emulator-lift · D3D9 **fixed-function 流水线**（M2 第一帧：设备状态机 + 顶点处理 + 软件光栅）
//
// ★ 范围（用户 2026-11-05 裁决"确认需要实现 D3D 接口"之后的第一帧判据）：
//     设备状态机（变换 / 渲染状态 / 采样器 / 纹理级状态 / 流 / 材质 / 灯光 / 视口 / 裁剪矩形）
//     → 固定功能 T&L（WORLD→VIEW→PROJECTION、近平面裁剪、视口映射）
//     → 软件光栅（重心坐标 + 左上填充规则、Z 测试/写、纹理采样、纹理级运算、alpha test、混合）
//   ⛔ 不是"返回 S_OK 就算画了"：真值落在**设备后缓冲的 guest 像素**上（与 `d3d9::clear` 同一块 SurfaceArena）。
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
#include <string>

#include "runtime/surface.h"  // SurfaceFormat（后缓冲 / 纹理的像素布局）

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
constexpr std::uint32_t kRsSeparateAlphaBlendEnable = 206;
constexpr std::uint32_t kRsSrcBlendAlpha = 207;
constexpr std::uint32_t kRsDestBlendAlpha = 208;
constexpr std::uint32_t kRsBlendOpAlpha = 209;

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
    bool in_scene = false;
    std::uint32_t begins = 0;     // BeginScene 成功次数
    std::uint32_t ends = 0;       // EndScene 成功次数
    std::uint32_t draws = 0;      // 通过前置检查的 Draw 次数
    std::uint32_t draw_calls = 0; // 进入 Draw 的次数（含失败）
    std::uint32_t triangles = 0;  // 进入光栅的三角形数（含被裁掉的）
    std::uint32_t rasterized = 0; // 真正光栅化（过了背面剔除）的三角形数
    std::uint64_t pixels = 0;     // 通过深度/alpha 测试并写进后缓冲的像素数
    std::uint32_t viewport_x = 0;
    std::uint32_t viewport_y = 0;
    std::uint32_t viewport_w = 0;
    std::uint32_t viewport_h = 0;
};

// =====================================================================================
// 状态块生命周期（createDeviceState / destroyDeviceState 里挂钩）
// =====================================================================================
// 建流水线状态块（guest 堆）+ 深度缓冲（SurfaceArena）。返回 EA；0 = 失败（why 点名）。
std::uint32_t createForDevice(X86& cpu, std::uint32_t device_state, int width, int height, std::string* why);
void destroyForDevice(X86& cpu, std::uint32_t device_state);
std::uint32_t pipelineOfDevice(X86& cpu, std::uint32_t device_state);
bool pipelineInfoOfState(X86& cpu, std::uint32_t state, PipelineInfo* out, std::string* why);
bool pipelineInfoOfDevice(X86& cpu, std::uint32_t device_state, PipelineInfo* out, std::string* why);

// =====================================================================================
// 方法语义（返回 HRESULT；`dev` 是设备状态块 EA）
// ⚠ 每个函数都先自己校验参数（真机的失败码），失败**不**改状态。
// =====================================================================================
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
std::uint32_t getTexture(X86& cpu, std::uint32_t dev, std::uint32_t stage, std::uint32_t out_ea,
                         std::string* why);
std::uint32_t setFVF(X86& cpu, std::uint32_t dev, std::uint32_t fvf, std::string* why);
std::uint32_t getFVF(X86& cpu, std::uint32_t dev, std::uint32_t out_ea, std::string* why);
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

// SetStreamSource / SetIndices
std::uint32_t setStreamSource(X86& cpu, std::uint32_t dev, std::uint32_t stream, std::uint32_t vb,
                              std::uint32_t offset, std::uint32_t stride, std::string* why);
std::uint32_t setIndices(X86& cpu, std::uint32_t dev, std::uint32_t ib, std::string* why);

// Draw*：真的做顶点处理 + 光栅。**未 BeginScene ⇒ D3DERR_INVALIDCALL**（真机语义）。
std::uint32_t drawPrimitive(X86& cpu, std::uint32_t dev, std::uint32_t prim, std::uint32_t start_vertex,
                            std::uint32_t prim_count, std::string* why);
std::uint32_t drawIndexedPrimitive(X86& cpu, std::uint32_t dev, std::uint32_t prim, std::uint32_t base_vertex,
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
