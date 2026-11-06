// emulator-lift · D3D9 fixed-function 流水线（状态机 + 顶点处理 + 软件光栅）
//
// ★ 先读 `runtime/d3d9_pipeline.h` 的文件头：枚举值来源、vtable 偏移的取证口径、判据口径。
// ★ 本文件**不认 COM**：对象 / vtable / refcount 在 `runtime/com.h`，方法 shim 在 `shims/d3d9.cpp`。
//   ⛔ 但 Draw* 会去 `cpu.com` 核对绑定的是不是**本模块建的**纹理 / 顶点缓冲对象（不是就响亮失败）。
//
// ★ 状态放哪儿：'D9PL' 块在 **guest 堆**，EA 挂在设备状态块的 +56。为什么（与 `d3d9.cpp` 同一套理由）：
//   生命周期跟着设备 refcount 走、⛔ 不需要宿主侧静态表（Machine 反复建在同一栈地址时会串台）。
//   深度缓冲放在 `SurfaceArena`（guest 地址空间里的 32bpp 表面，像素里存 float 位模式）——
//   与后缓冲同一块 arena ⇒ 一起被 `destroyForDevice` 回收，不出现"设备没了深度还在"。
//
// ★ 哪些是**近似**（⛔ 不冒充真机；每处都在代码里点明）：
//   · 光照：Gouraud per-vertex；支持 DIRECTIONAL / POINT（SPOT 按 POINT+falloff 近似）；
//     法线只做 world/view 线性变换后归一化（真机用逆转置 ⇒ 非等比缩放下会偏）。
//   · 雾：**不实现**（FOGENABLE=1 时记录 + 一行日志，颜色不参与雾化）。
//   · 纹理过滤：MIN/MAG **都参与**决策（任一要求 LINEAR ⇒ 双线性；各向异性按 LINEAR 近似）。
//     本线没有 mip 链 / LOD / 导数 ⇒ 不按 LOD 在 MIN/MAG 之间选，这是**明确记录的近似**
//     （决策见 `samplerUsesLinear`，⛔ 不是"参数传了却不用"）。
//   · 采样器 MIPFILTER / MAXMIPLEVEL / LODBIAS：本线只有 level 0 ⇒ 记录、不影响像素。
//   · 多重采样 / 抗锯齿 / 线框 / patch / 顶点混合：不支持 ⇒ 记录 + 一行日志。
//   · 顶点声明（D3DVERTEXELEMENT9）：`CreateVertexDeclaration` / `SetVertexDeclaration` 已接对象形状，
//     但 Draw 仍只认 FVF ⇒ 有声明时响亮失败（⛔ 不猜两个 stream 的语义）。
//   · 纹理级运算：支持 DISABLE/SELECTARG1/2/MODULATE(2X/4X)/ADD/ADDSIGNED(2X)/SUBTRACT/
//     ADDSMOOTH/BLEND*ALPHA/MULTIPLYADD/LERP；其余 ⇒ 记录一次 + 退回 SELECTARG1。
//   · 光栅：像素中心采样 + 左上填充规则（顶点顺序 = 屏幕空间顺时针 = D3D 正面）。
#include "runtime/d3d9_pipeline.h"

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "runtime/gpu_raster.h"
#include <map>
#include <string>
#include <vector>

#include "runtime/com.h"
#include "runtime/d3d9.h"
#include "runtime/heap.h"
#include "runtime/machine.h"
#include "runtime/math.h"
#include "runtime/memory.h"
#include "runtime/pixel_format.h"
#include "runtime/surface.h"
#include "runtime/log.h"
#include "runtime/x86.h"

namespace lift {
namespace d3d9 {
std::uint64_t g_gpu_draws = 0;
std::uint64_t g_gpu_triangles = 0;
namespace {

// =====================================================================================
// 状态块布局（**我们自己的**；⛔ 不是 Win32 结构体）
// =====================================================================================
constexpr std::uint32_t kPlMagic = 0;
constexpr std::uint32_t kPlDevice = 4;    // 设备状态块 EA
constexpr std::uint32_t kPlInScene = 8;
constexpr std::uint32_t kPlFvf = 12;
constexpr std::uint32_t kPlVertexDecl = 16;
constexpr std::uint32_t kPlViewport = 20;   // D3DVIEWPORT9：X/Y/W/H/MinZ/MaxZ（6 dword）
constexpr std::uint32_t kPlScissor = 44;    // RECT：left/top/right/bottom（4 dword）
constexpr std::uint32_t kPlTransforms = 64;  // 260 个 D3DMATRIX（64 B 一个）
constexpr std::uint32_t kPlTransformCount = 260;
constexpr std::uint32_t kPlRenderStates = kPlTransforms + kPlTransformCount * 64u;  // 256 个 DWORD
constexpr std::uint32_t kPlRenderStateCount = 256;
constexpr std::uint32_t kPlSamplers = kPlRenderStates + kPlRenderStateCount * 4u;   // 16 * 16
constexpr std::uint32_t kPlSamplerCount = kMaxSamplers * 16u;
constexpr std::uint32_t kPlTexStages = kPlSamplers + kPlSamplerCount * 4u;          // 8 * 40
constexpr std::uint32_t kPlTexStageTypes = 40;
constexpr std::uint32_t kPlTextures = kPlTexStages + kMaxTextureStages * kPlTexStageTypes * 4u;  // 16 槽
constexpr std::uint32_t kPlStreams = kPlTextures + kMaxSamplers * 4u;  // 16 流 × (vb, offset, stride, freq)
constexpr std::uint32_t kPlIndices = kPlStreams + kMaxStreams * 16u;
constexpr std::uint32_t kPlMaterial = kPlIndices + 4u;    // D3DMATERIAL9 = 68 B
constexpr std::uint32_t kPlLights = kPlMaterial + 68u;    // 8 × D3DLIGHT9 = 104 B
constexpr std::uint32_t kPlLightEnabled = kPlLights + kMaxLights * 104u;
constexpr std::uint32_t kPlDraws = kPlLightEnabled + kMaxLights * 4u;
constexpr std::uint32_t kPlDrawCalls = kPlDraws + 4u;
constexpr std::uint32_t kPlTriangles = kPlDrawCalls + 4u;
constexpr std::uint32_t kPlRasterized = kPlTriangles + 4u;
constexpr std::uint32_t kPlPixels = kPlRasterized + 8u;   // uint64
constexpr std::uint32_t kPlBegins = kPlPixels + 8u;
constexpr std::uint32_t kPlEnds = kPlBegins + 4u;
constexpr std::uint32_t kPlDepthArena = kPlEnds + 4u;
constexpr std::uint32_t kPlColorArena = kPlDepthArena + 4u;    // 0 = 设备后缓冲
constexpr std::uint32_t kPlColorSurface = kPlColorArena + 4u;  // IDirect3DSurface9 对象 EA（0 = 后缓冲）
constexpr std::uint32_t kPlStageSnapshots = kPlColorSurface + 4u;  // 纹理级/采样器状态快照次数（诊断）
constexpr std::uint32_t kPlUnsupported = kPlStageSnapshots + 4u;  // 未支持状态的位图（诊断）
constexpr std::uint32_t kPlBytes = kPlUnsupported + 32u;
constexpr std::uint32_t kPlMagicValue = 0x31304C44u;  // 'DL01'（写成可读：'D','L','0','1'）

// 顶点缓冲 / 索引缓冲状态块（都在 guest 堆；数据紧跟头部）
constexpr std::uint32_t kVbMagic = 0;
constexpr std::uint32_t kVbDevice = 4;
constexpr std::uint32_t kVbLength = 8;
constexpr std::uint32_t kVbUsage = 12;
constexpr std::uint32_t kVbFvf = 16;
constexpr std::uint32_t kVbPool = 20;
constexpr std::uint32_t kVbLocked = 24;
constexpr std::uint32_t kVbHeader = 28;
constexpr std::uint32_t kVbMagicValue = 0x30394256u;  // 'VB09'
constexpr std::uint32_t kIbMagic = 0;
constexpr std::uint32_t kIbDevice = 4;
constexpr std::uint32_t kIbLength = 8;
constexpr std::uint32_t kIbUsage = 12;
constexpr std::uint32_t kIbFormat = 16;
constexpr std::uint32_t kIbPool = 20;
constexpr std::uint32_t kIbLocked = 24;
constexpr std::uint32_t kIbHeader = 28;
constexpr std::uint32_t kIbMagicValue = 0x30394249u;  // 'IB09'
constexpr std::uint32_t kMaxBufferBytes = 64u * 1024u * 1024u;

void pipeLog(const char* what, const char* fmt, ...) {
    char prefix[256];
    std::snprintf(prefix, sizeof(prefix), "[d3d9] %s: ", what ? what : "?");
    va_list ap;
    va_start(ap, fmt);
    logV(LogLevel::Info, LogD3d9, prefix, fmt, ap);
    va_end(ap);
}

bool requireMachine(X86& cpu, Machine** out, std::string* why) {
    Machine* m = activeMachine();
    if (!m || &m->cpu() != &cpu) {
        if (why) *why = "D3D9 流水线：activeMachine() 不是这台 cpu 的 Machine（装配问题）";
        return false;
    }
    if (!m->surfaces().ready()) {
        if (why) *why = "D3D9 流水线：SurfaceArena 未初始化";
        return false;
    }
    *out = m;
    return true;
}

bool committed(X86& cpu, std::uint32_t ea, std::size_t n, const char* what, std::string* why) {
    if (ea == 0) {
        if (why) *why = std::string("D3D9 流水线：") + what + " 为 NULL";
        return false;
    }
    if (!cpu.mem->committed(ea, n)) {
        char buf[192];
        std::snprintf(buf, sizeof(buf), "D3D9 流水线：%s = 0x%08X 起 %u 字节未提交", what, ea,
                      static_cast<unsigned>(n));
        if (why) *why = buf;
        return false;
    }
    return true;
}

void wrU32(X86& cpu, std::uint32_t ea, std::uint32_t v) { cpu.mem->wr32(ea, v); }

float rdF32(X86& cpu, std::uint32_t ea) {
    const std::uint32_t bits = cpu.mem->rd32(ea);
    float f = 0.0f;
    std::memcpy(&f, &bits, sizeof(f));
    return f;
}
void wrF32(X86& cpu, std::uint32_t ea, float v) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &v, sizeof(bits));
    cpu.mem->wr32(ea, bits);
}

bool finite(double v) { return v == v && v > -1.0e30 && v < 1.0e30; }

// 单位阵：走 runtime/math 的唯一适配点（与 d3dx9_sprite 的 writeIdentityMatrix 同一份；
// ⛔ 这里不再手写 `i % 5` 的 D3DX 布局）。
void writeIdentity(X86& cpu, std::uint32_t ea) {
    float d3d[16];
    math::toD3DLayout(math::identity<float>(), d3d);
    for (int i = 0; i < 16; ++i) wrF32(cpu, ea + static_cast<std::uint32_t>(i) * 4u, d3d[i]);
}

// ---- 状态块读取（内部；magic 校验后直接用）----
std::uint32_t pipeEaOf(X86& cpu, std::uint32_t dev_state) {
    return cpu.mem->rd32(dev_state + 56u);  // 与 d3d9.cpp 的 kDvPipeline 同一格
}

std::uint32_t plGet(X86& cpu, std::uint32_t pe, std::uint32_t off) { return cpu.mem->rd32(pe + off); }
void plSet(X86& cpu, std::uint32_t pe, std::uint32_t off, std::uint32_t v) { cpu.mem->wr32(pe + off, v); }

bool pipelineOf(X86& cpu, std::uint32_t dev_state, std::uint32_t* pe, std::string* why) {
    if (dev_state == 0) {
        if (why) *why = "D3D9 流水线：设备状态块 EA 为 0";
        return false;
    }
    const std::uint32_t pe_ea = pipeEaOf(cpu, dev_state);
    if (pe_ea == 0 || !cpu.mem->committed(pe_ea, kPlBytes)) {
        if (why) *why = "D3D9 流水线：设备没有流水线状态块（createForDevice 没跑？）";
        return false;
    }
    if (cpu.mem->rd32(pe_ea + kPlMagic) != kPlMagicValue) {
        if (why) *why = "D3D9 流水线：流水线状态块的 magic 不对（块被写坏 / 野指针）";
        return false;
    }
    *pe = pe_ea;
    return true;
}

std::uint32_t rsOf(X86& cpu, std::uint32_t pe, std::uint32_t state) {
    return plGet(cpu, pe, kPlRenderStates + state * 4u);
}

void setRs(X86& cpu, std::uint32_t pe, std::uint32_t state, std::uint32_t value) {
    plSet(cpu, pe, kPlRenderStates + state * 4u, value);
}

std::uint32_t samplerOf(X86& cpu, std::uint32_t pe, std::uint32_t stage, std::uint32_t type) {
    return plGet(cpu, pe, kPlSamplers + (stage * 16u + type) * 4u);
}

std::uint32_t texStageOf(X86& cpu, std::uint32_t pe, std::uint32_t stage, std::uint32_t type) {
    return plGet(cpu, pe, kPlTexStages + (stage * kPlTexStageTypes + type) * 4u);
}

bool validTransformState(std::uint32_t state) {
    if (state == kTsView || state == kTsProjection) return true;
    if (state >= kTsTexture0 && state < kTsTexture0 + 8u) return true;
    if (state >= kTsWorld && state < kTsWorld + 4u) return true;
    return false;
}

// 未支持状态：记录位 + 只打一次日志（⛔ 不静默；也⛔ 不反复刷屏）
void recordUnsupported(X86& cpu, std::uint32_t pe, std::uint32_t bit, const char* what, std::uint32_t value) {
    const std::uint32_t word = bit / 32u;
    const std::uint32_t mask = 1u << (bit % 32u);
    const std::uint32_t off = kPlUnsupported + word * 4u;
    const std::uint32_t cur = plGet(cpu, pe, off);
    if ((cur & mask) != 0) return;
    plSet(cpu, pe, off, cur | mask);
    pipeLog("unsupported", "%s（值 0x%X）——本线不实现，已记录（⛔ 不冒充真机）", what, value);
}

// =====================================================================================
// 状态默认值（SDK 的默认状态；每条都在这里写死，⛔ 不留未初始化）
// =====================================================================================
void initDefaults(X86& cpu, std::uint32_t pe, int width, int height) {
    for (std::uint32_t i = 0; i < kPlBytes; ++i) cpu.mem->wr8(pe + i, 0);
    wrU32(cpu, pe + kPlMagic, kPlMagicValue);
    // 变换：单位阵（VIEW / PROJECTION / TEXTURE0..7 / WORLD..WORLD3）
    for (std::uint32_t i = 0; i < kPlTransformCount; ++i)
        writeIdentity(cpu, pe + kPlTransforms + i * 64u);
    // 渲染状态默认值（d3d9.h 的默认状态机）
    setRs(cpu, pe, kRsZEnable, 1u);
    setRs(cpu, pe, kRsFillMode, 3u);  // D3DFILL_SOLID
    setRs(cpu, pe, kRsShadeMode, kShadeGouraud);
    setRs(cpu, pe, kRsZWriteEnable, 1u);
    setRs(cpu, pe, kRsAlphaTestEnable, 0u);
    setRs(cpu, pe, kRsLastPixel, 1u);
    setRs(cpu, pe, kRsSrcBlend, kBlendOne);
    setRs(cpu, pe, kRsDestBlend, kBlendZero);
    setRs(cpu, pe, kRsCullMode, kCullCcw);
    setRs(cpu, pe, kRsZFunc, kCmpLessEqual);
    setRs(cpu, pe, kRsAlphaRef, 0u);
    setRs(cpu, pe, kRsAlphaFunc, kCmpAlways);
    setRs(cpu, pe, kRsDitherEnable, 0u);
    setRs(cpu, pe, kRsAlphaBlendEnable, 0u);
    setRs(cpu, pe, kRsFogEnable, 0u);
    setRs(cpu, pe, kRsSpecularEnable, 0u);
    setRs(cpu, pe, kRsFogColor, 0u);
    setRs(cpu, pe, kRsStencilEnable, 0u);
    setRs(cpu, pe, kRsTextureFactor, 0xFFFFFFFFu);
    setRs(cpu, pe, kRsClipping, 1u);
    setRs(cpu, pe, kRsLighting, 1u);
    setRs(cpu, pe, kRsAmbient, 0u);
    setRs(cpu, pe, kRsColorVertex, 1u);
    setRs(cpu, pe, kRsLocalViewer, 0u);
    setRs(cpu, pe, kRsNormalizeNormals, 0u);
    setRs(cpu, pe, kRsDiffuseMaterialSource, 0u);   // D3DMCS_MATERIAL
    setRs(cpu, pe, kRsSpecularMaterialSource, 0u);
    setRs(cpu, pe, kRsAmbientMaterialSource, 0u);
    setRs(cpu, pe, kRsEmissiveMaterialSource, 0u);
    setRs(cpu, pe, kRsVertexBlend, 0u);
    setRs(cpu, pe, kRsClipPlaneEnable, 0u);
    setRs(cpu, pe, kRsMultiSampleAntiAlias, 0u);
    setRs(cpu, pe, kRsMultiSampleMask, 0xFFFFFFFFu);
    setRs(cpu, pe, kRsIndexedVertexBlendEnable, 0u);
    setRs(cpu, pe, kRsColorWriteEnable, 0xFu);
    setRs(cpu, pe, kRsTweenFactor, 0u);
    setRs(cpu, pe, kRsBlendOp, kBlendOpAdd);
    setRs(cpu, pe, kRsScissorTestEnable, 0u);
    setRs(cpu, pe, kRsSeparateAlphaBlendEnable, 0u);
    setRs(cpu, pe, kRsSrcBlendAlpha, kBlendOne);
    setRs(cpu, pe, kRsDestBlendAlpha, kBlendZero);
    setRs(cpu, pe, kRsBlendOpAlpha, kBlendOpAdd);
    // 采样器默认值：WRAP + POINT + MIPFILTER NONE
    for (std::uint32_t s = 0; s < kMaxSamplers; ++s) {
        plSet(cpu, pe, kPlSamplers + (s * 16u + kSampAddressU) * 4u, kAddressWrap);
        plSet(cpu, pe, kPlSamplers + (s * 16u + kSampAddressV) * 4u, kAddressWrap);
        plSet(cpu, pe, kPlSamplers + (s * 16u + kSampAddressW) * 4u, kAddressWrap);
        plSet(cpu, pe, kPlSamplers + (s * 16u + kSampMagFilter) * 4u, kTexfPoint);
        plSet(cpu, pe, kPlSamplers + (s * 16u + kSampMinFilter) * 4u, kTexfPoint);
        plSet(cpu, pe, kPlSamplers + (s * 16u + kSampMipFilter) * 4u, kTexfNone);
        plSet(cpu, pe, kPlSamplers + (s * 16u + kSampMaxMipLevel) * 4u, 0u);
        plSet(cpu, pe, kPlSamplers + (s * 16u + kSampMaxAnisotropy) * 4u, 1u);
        plSet(cpu, pe, kPlSamplers + (s * 16u + kSampBorderColor) * 4u, 0u);
    }
    // 纹理级默认：级 0 = MODULATE(TEXTURE, CURRENT=DIFFUSE)；其余级 DISABLE
    plSet(cpu, pe, kPlTexStages + (0u * kPlTexStageTypes + kTssColorOp) * 4u, kTopModulate);
    plSet(cpu, pe, kPlTexStages + (0u * kPlTexStageTypes + kTssColorArg1) * 4u, kTaTexture);
    plSet(cpu, pe, kPlTexStages + (0u * kPlTexStageTypes + kTssColorArg2) * 4u, kTaCurrent);
    plSet(cpu, pe, kPlTexStages + (0u * kPlTexStageTypes + kTssAlphaOp) * 4u, kTopModulate);
    plSet(cpu, pe, kPlTexStages + (0u * kPlTexStageTypes + kTssAlphaArg1) * 4u, kTaTexture);
    plSet(cpu, pe, kPlTexStages + (0u * kPlTexStageTypes + kTssAlphaArg2) * 4u, kTaCurrent);
    for (std::uint32_t s = 1; s < kMaxTextureStages; ++s) {
        plSet(cpu, pe, kPlTexStages + (s * kPlTexStageTypes + kTssColorOp) * 4u, kTopDisable);
        plSet(cpu, pe, kPlTexStages + (s * kPlTexStageTypes + kTssAlphaOp) * 4u, kTopDisable);
    }
    // 材质默认：diffuse/ambient = 1，specular/emissive = 0，power = 0（自洽默认，见文件头）
    for (std::uint32_t c = 0; c < 4; ++c) {
        wrF32(cpu, pe + kPlMaterial + c * 4u, 1.0f);          // Diffuse
        wrF32(cpu, pe + kPlMaterial + 16u + c * 4u, 1.0f);    // Ambient
        wrF32(cpu, pe + kPlMaterial + 32u + c * 4u, 0.0f);    // Specular
        wrF32(cpu, pe + kPlMaterial + 48u + c * 4u, 0.0f);    // Emissive
    }
    wrF32(cpu, pe + kPlMaterial + 64u, 0.0f);  // Power
    // 视口 = 整个后缓冲；裁剪矩形 = 整个后缓冲
    plSet(cpu, pe, kPlViewport + 0u, 0u);
    plSet(cpu, pe, kPlViewport + 4u, 0u);
    plSet(cpu, pe, kPlViewport + 8u, static_cast<std::uint32_t>(width));
    plSet(cpu, pe, kPlViewport + 12u, static_cast<std::uint32_t>(height));
    wrF32(cpu, pe + kPlViewport + 16u, 0.0f);
    wrF32(cpu, pe + kPlViewport + 20u, 1.0f);
    plSet(cpu, pe, kPlScissor + 0u, 0u);
    plSet(cpu, pe, kPlScissor + 4u, 0u);
    plSet(cpu, pe, kPlScissor + 8u, static_cast<std::uint32_t>(width));
    plSet(cpu, pe, kPlScissor + 12u, static_cast<std::uint32_t>(height));
}

// =====================================================================================
// 纹理 / 顶点缓冲的解析（COM 对象 EA → 状态块）
// =====================================================================================
struct TextureView {
    bool present = false;
    const std::uint8_t* pixels = nullptr;
    SurfaceFormat format;
    int width = 0;
    int height = 0;
    std::uint32_t pitch = 0;
};

bool resolveTexture(X86& cpu, Machine* m, std::uint32_t object_ea, TextureView* out, std::string* why) {
    out->present = false;
    if (object_ea == 0) return true;
    TextureInfo ti;
    if (!textureInfo(cpu, object_ea, &ti, why)) return false;
    GuestSurface* gs = m->surfaces().find(ti.arena);
    if (!gs) {
        if (why) *why = "D3D9 流水线：纹理像素不在 arena 里";
        return false;
    }
    out->pixels = m->surfaces().pixels(*gs);
    out->format = gs->format;
    out->width = gs->width;
    out->height = gs->height;
    out->pitch = gs->pitch;
    out->present = out->pixels != nullptr && out->format.bytesPerPixel() != 0;
    return true;
}

// ---- 颜色换算：实现只在 runtime/pixel_format.h（唯一一份）----

// =====================================================================================
// FVF 解析
// =====================================================================================
struct FvfLayout {
    bool valid = false;
    std::uint32_t fvf = 0;
    bool xyz = false;    // D3DFVF_XYZ
    bool xyz_rhw = false;
    bool xyzw = false;
    bool has_normal = false;
    bool has_psize = false;
    bool has_diffuse = false;
    bool has_specular = false;
    std::uint32_t tex_count = 0;
    std::uint32_t tex_size[8] = {0};  // 每个坐标的 float 数（2 默认）
    std::uint32_t pos_offset = 0;
    std::uint32_t normal_offset = 0;
    std::uint32_t psize_offset = 0;
    std::uint32_t diffuse_offset = 0;
    std::uint32_t specular_offset = 0;
    std::uint32_t tex_offset[8] = {0};
    std::uint32_t stride = 0;
};

FvfLayout parseFvf(std::uint32_t fvf, std::string* why) {
    FvfLayout l;
    l.fvf = fvf;
    const std::uint32_t pos = fvf & kFvfPositionMask;
    if (pos == kFvfXyz) {
        l.xyz = true;
    } else if (pos == kFvfXyzRhw) {
        l.xyz_rhw = true;
    } else if (pos == kFvfXyzw) {
        l.xyzw = true;
    } else {
        if (why) {
            char buf[160];
            std::snprintf(buf, sizeof(buf),
                          "D3D9 流水线：FVF=0x%08X 的位置类型 0x%X 不在本线支持集（XYZ/XYZRHW/XYZW）", fvf, pos);
            *why = buf;
        }
        return l;
    }
    std::uint32_t off = 0;
    if (l.xyzw) {
        off += 16u;
    } else if (l.xyz_rhw) {
        off += 16u;  // x,y,z,rhw
    } else {
        off += 12u;  // x,y,z
        // XYZB1..B5：混权（本线不做顶点混合，但**步长要算对**）
        switch (pos) {
            case 0x0006u: off += 4u; break;
            case 0x0008u: off += 8u; break;
            case 0x000Au: off += 12u; break;
            case 0x000Cu: off += 16u; break;
            case 0x000Eu: off += 20u; break;
            default: break;
        }
    }
    l.has_normal = (fvf & kFvfNormal) != 0;
    if (l.has_normal) {
        l.normal_offset = off;
        off += 12u;
    }
    l.has_psize = (fvf & kFvfPSize) != 0;
    if (l.has_psize) {
        l.psize_offset = off;
        off += 4u;
    }
    l.has_diffuse = (fvf & kFvfDiffuse) != 0;
    if (l.has_diffuse) {
        l.diffuse_offset = off;
        off += 4u;
    }
    l.has_specular = (fvf & kFvfSpecular) != 0;
    if (l.has_specular) {
        l.specular_offset = off;
        off += 4u;
    }
    l.tex_count = (fvf & kFvfTexCountMask) >> kFvfTexCountShift;
    if (l.tex_count > 8u) {
        if (why) *why = "D3D9 流水线：FVF 的纹理坐标数 > 8（本线上限 8）";
        return l;
    }
    for (std::uint32_t i = 0; i < l.tex_count; ++i) {
        // FVF 位 16+2i 起的 2 位：0=2 floats / 1=3 / 2=4 / 3=1
        const std::uint32_t code = (fvf >> (16u + i * 2u)) & 0x3u;
        std::uint32_t n = 2u;
        if (code == 1u) n = 3u;
        else if (code == 2u) n = 4u;
        else if (code == 3u) n = 1u;
        l.tex_size[i] = n;
        l.tex_offset[i] = off;
        off += n * 4u;
    }
    l.stride = off;
    l.valid = true;
    return l;
}

// =====================================================================================
// 顶点：读 + 变换 + 光照
// =====================================================================================
constexpr std::uint32_t kMaxClipVerts = 16u;   // 三角形过 6 个裁剪面后最多 3+6=9 个（留余量）
constexpr std::uint32_t kMaxTexCoords = 8u;

struct CVertex {
    double x = 0, y = 0, z = 0, w = 1;  // 裁剪空间（XYZRHW 时 x,y 已是屏幕坐标）
    double u[kMaxTexCoords] = {0};
    double v[kMaxTexCoords] = {0};
    double r = 255, g = 255, b = 255, a = 255;   // diffuse（0..255）
    double sr = 0, sg = 0, sb = 0, sa = 0;       // specular
    bool pretransformed = false;                 // XYZRHW
};

struct SVertex {
    double x = 0, y = 0;  // 屏幕（像素）
    double z = 0;         // [0,1]
    double invw = 1;
    double u[kMaxTexCoords] = {0};  // 已乘 invw
    double v[kMaxTexCoords] = {0};
    double r = 255, g = 255, b = 255, a = 255;  // 已乘 invw
    double sr = 0, sg = 0, sb = 0, sa = 0;
};

// ---- 变换：统一走 runtime/math 的 D3DX↔GLM 适配（⛔ 这里不再有行/列主序的手写展开）----
math::Mat4d readMatrix(X86& cpu, std::uint32_t ea) {
    double d3d[16];
    for (int i = 0; i < 16; ++i) d3d[i] = static_cast<double>(rdF32(cpu, ea + static_cast<std::uint32_t>(i) * 4u));
    return math::fromD3DLayout(d3d);
}

void writeMatrix(X86& cpu, std::uint32_t ea, const math::Mat4d& m) {
    double d3d[16];
    math::toD3DLayout(m, d3d);
    for (int i = 0; i < 16; ++i) wrF32(cpu, ea + static_cast<std::uint32_t>(i) * 4u, static_cast<float>(d3d[i]));
}

struct LightState {
    std::uint32_t type = 0;
    double diffuse[4] = {0, 0, 0, 0};
    double specular[4] = {0, 0, 0, 0};
    double ambient[4] = {0, 0, 0, 0};
    double position[3] = {0, 0, 0};
    double direction[3] = {0, 0, 0};
    double range = 0;
    double falloff = 0;
    double atten0 = 0, atten1 = 0, atten2 = 0;
};

struct MaterialState {
    double diffuse[4] = {1, 1, 1, 1};
    double ambient[4] = {1, 1, 1, 1};
    double specular[4] = {0, 0, 0, 0};
    double emissive[4] = {0, 0, 0, 0};
    double power = 0;
};

void readMaterial(X86& cpu, std::uint32_t pe, MaterialState* out) {
    for (int i = 0; i < 4; ++i) {
        out->diffuse[i] = static_cast<double>(rdF32(cpu, pe + kPlMaterial + static_cast<std::uint32_t>(i) * 4u));
        out->ambient[i] =
            static_cast<double>(rdF32(cpu, pe + kPlMaterial + 16u + static_cast<std::uint32_t>(i) * 4u));
        out->specular[i] =
            static_cast<double>(rdF32(cpu, pe + kPlMaterial + 32u + static_cast<std::uint32_t>(i) * 4u));
        out->emissive[i] =
            static_cast<double>(rdF32(cpu, pe + kPlMaterial + 48u + static_cast<std::uint32_t>(i) * 4u));
    }
    out->power = static_cast<double>(rdF32(cpu, pe + kPlMaterial + 64u));
}

void readLight(X86& cpu, std::uint32_t pe, std::uint32_t index, LightState* out) {
    const std::uint32_t base = pe + kPlLights + index * 104u;
    out->type = cpu.mem->rd32(base);
    for (int i = 0; i < 4; ++i) {
        out->diffuse[i] = static_cast<double>(rdF32(cpu, base + 4u + static_cast<std::uint32_t>(i) * 4u));
        out->specular[i] = static_cast<double>(rdF32(cpu, base + 20u + static_cast<std::uint32_t>(i) * 4u));
        out->ambient[i] = static_cast<double>(rdF32(cpu, base + 36u + static_cast<std::uint32_t>(i) * 4u));
    }
    for (int i = 0; i < 3; ++i) out->position[i] = static_cast<double>(rdF32(cpu, base + 52u + static_cast<std::uint32_t>(i) * 4u));
    for (int i = 0; i < 3; ++i) out->direction[i] = static_cast<double>(rdF32(cpu, base + 64u + static_cast<std::uint32_t>(i) * 4u));
    out->range = static_cast<double>(rdF32(cpu, base + 76u));
    out->falloff = static_cast<double>(rdF32(cpu, base + 80u));
    out->atten0 = static_cast<double>(rdF32(cpu, base + 84u));
    out->atten1 = static_cast<double>(rdF32(cpu, base + 88u));
    out->atten2 = static_cast<double>(rdF32(cpu, base + 92u));
}

// 顶点颜色（D3DCOLOR 0xAARRGGBB → 0..255）：位序走 pixel_format 的唯一一份 `unpackColor`
void splitColor(std::uint32_t color, double* a, double* r, double* g, double* b) {
    unsigned ua = 0, ur = 0, ug = 0, ub = 0;
    pixel_format::unpackColor(color, ua, ur, ug, ub);
    *a = static_cast<double>(ua);
    *r = static_cast<double>(ur);
    *g = static_cast<double>(ug);
    *b = static_cast<double>(ub);
}

// 固定功能光照（Gouraud；只在 LIGHTING=1 且顶点有法线时做）
void applyLighting(X86& cpu, std::uint32_t pe, const math::Vec3d& world_pos,
                   const math::Vec3d& world_normal, const math::Mat4d& view, double lit[4]) {
    MaterialState mat;
    readMaterial(cpu, pe, &mat);
    // 材质来源（diffuse/ambient）——COLORVERTEX 为真有顶点色时用顶点色（在 Draw 里覆盖）
    const math::Vec3d cam_pos = math::transformPoint3D3D(view, world_pos);
    const math::Vec3d n =
        math::normalized3D3D(math::transformVector3D3D(view, math::normalized3D3D(world_normal)));
    const std::uint32_t ambient_argb = rsOf(cpu, pe, kRsAmbient);
    double ga = 0, gr = 0, gg = 0, gb = 0;
    splitColor(ambient_argb, &ga, &gr, &gg, &gb);
    double r = mat.emissive[0] + mat.ambient[0] * (gr / 255.0);
    double g = mat.emissive[1] + mat.ambient[1] * (gg / 255.0);
    double b = mat.emissive[2] + mat.ambient[2] * (gb / 255.0);
    const double alpha = mat.diffuse[3];
    for (std::uint32_t i = 0; i < kMaxLights; ++i) {
        if (plGet(cpu, pe, kPlLightEnabled + i * 4u) == 0) continue;
        LightState ls;
        readLight(cpu, pe, i, &ls);
        math::Vec3d ldir(0.0);  // 从顶点指向光源
        double atten = 1.0;
        switch (ls.type) {
            case kLightDirectional:
                ldir = math::normalized3D3D(math::Vec3d(-ls.direction[0], -ls.direction[1], -ls.direction[2]));
                break;
            case kLightPoint:
            case kLightSpot: {
                const math::Vec3d pos =
                    math::transformPoint3D3D(view, math::Vec3d(ls.position[0], ls.position[1], ls.position[2]));
                ldir = pos - cam_pos;
                const double dist = glm::length(ldir);
                if (ls.range > 0.0 && dist > ls.range) continue;
                const double denom = ls.atten0 + ls.atten1 * dist + ls.atten2 * dist * dist;
                atten = denom > 1e-12 ? 1.0 / denom : 1.0;
                if (atten > 1.0) atten = 1.0;
                ldir = math::normalized3D3D(ldir);
                if (ls.type == kLightSpot) {
                    // 本线把 SPOT 按 POINT + falloff 近似（⛔ 没做锥角）
                    atten *= 1.0;
                }
                break;
            }
            default:
                continue;
        }
        double ndl = glm::dot(n, ldir);
        if (ndl < 0.0) ndl = 0.0;
        if (ndl > 0.0) {
            r += ls.diffuse[0] * mat.diffuse[0] * ndl * atten;
            g += ls.diffuse[1] * mat.diffuse[1] * ndl * atten;
            b += ls.diffuse[2] * mat.diffuse[2] * ndl * atten;
        }
        // 镜面：LOCALVIEWER=FALSE 时 D3D 用 (0,0,-1)；这里统一用"顶点 → 眼"（更接近 LOCALVIEWER=TRUE）
        if (mat.power > 0.0 && rsOf(cpu, pe, kRsSpecularEnable) != 0) {
            const math::Vec3d vdir = math::normalized3D3D(-cam_pos);
            const math::Vec3d h = math::normalized3D3D(ldir + vdir);
            double ndh = glm::dot(n, h);
            if (ndh < 0.0) ndh = 0.0;
            const double spec = std::pow(ndh, mat.power) * atten;
            r += ls.specular[0] * mat.specular[0] * spec;
            g += ls.specular[1] * mat.specular[1] * spec;
            b += ls.specular[2] * mat.specular[2] * spec;
        }
    }
    lit[0] = r > 1.0 ? 1.0 : (r < 0.0 ? 0.0 : r);
    lit[1] = g > 1.0 ? 1.0 : (g < 0.0 ? 0.0 : g);
    lit[2] = b > 1.0 ? 1.0 : (b < 0.0 ? 0.0 : b);
    lit[3] = alpha > 1.0 ? 1.0 : (alpha < 0.0 ? 0.0 : alpha);
}

// =====================================================================================
// 光栅化上下文（含**每次 Draw 的状态快照**）
// ★ 消除偏差 #4：像素循环不再逐像素读 guest —— guest 在 Draw 期间不会执行（本线不做嵌套回调），
//   所以纹理级 / 采样器状态在 Draw 入口读一遍就定型，像素循环只读 DrawContext 里的副本
//   （⛔ 不再调 texStageOf/samplerOf）。
// =====================================================================================
struct TextureStageSnapshot {
    std::uint32_t color_op = kTopDisable;
    std::uint32_t alpha_op = kTopDisable;
    std::uint32_t color_arg0 = 0;
    std::uint32_t color_arg1 = 0;
    std::uint32_t color_arg2 = 0;
    std::uint32_t alpha_arg1 = 0;
    std::uint32_t alpha_arg2 = 0;
    std::uint32_t constant = 0;         // D3DCOLOR（TFactor 之外的另一份常量）
    std::uint32_t tex_coord_index = 0;  // D3DTSS_TEXCOORDINDEX
};

struct SamplerSnapshot {
    std::uint32_t address_u = kAddressWrap;
    std::uint32_t address_v = kAddressWrap;
    std::uint32_t mag_filter = kTexfPoint;
    std::uint32_t min_filter = kTexfPoint;
};

struct DrawContext {
    X86* cpu = nullptr;
    Machine* machine = nullptr;
    GpuRaster* gpu = nullptr;  // --raster gpu 且 sink 提供时非空
    std::uint32_t pe = 0;
    std::uint8_t* color_base = nullptr;
    SurfaceFormat color_fmt;
    int color_width = 0;
    int color_height = 0;
    std::uint32_t color_pitch = 0;
    std::uint32_t color_surface = 0;  // 0 = 设备后缓冲
    float* depth_base = nullptr;  // 后缓冲尺寸的 float 深度
    std::uint32_t depth_pitch_floats = 0;
    int viewport_x = 0, viewport_y = 0, viewport_w = 0, viewport_h = 0;
    int scissor_x0 = 0, scissor_y0 = 0, scissor_x1 = 0, scissor_y1 = 0;
    // 状态快照
    bool z_enable = true;
    bool z_write = true;
    std::uint32_t z_func = kCmpLessEqual;
    std::uint32_t cull = kCullCcw;
    std::uint32_t shade = kShadeGouraud;
    bool alpha_test = false;
    std::uint32_t alpha_func = kCmpAlways;
    std::uint32_t alpha_ref = 0;
    bool blend = false;
    std::uint32_t src_blend = kBlendOne;
    std::uint32_t dst_blend = kBlendZero;
    std::uint32_t blend_op = kBlendOpAdd;
    bool separate_alpha_blend = false;
    std::uint32_t src_blend_a = kBlendOne;
    std::uint32_t dst_blend_a = kBlendZero;
    std::uint32_t blend_op_a = kBlendOpAdd;
    std::uint32_t color_write = 0xF;
    std::uint32_t texture_factor = 0xFFFFFFFFu;
    TextureView textures[kMaxTextureStages];
    // ★ 每次 Draw 的快照（prepareContext 里一次读完；像素循环只读这里）
    TextureStageSnapshot stages[kMaxTextureStages];
    SamplerSnapshot samplers[kMaxTextureStages];
    std::uint64_t pixels_written = 0;
    int serial = 0;
};

// 8 位通道的混合因子
// ★ 这一族（blendFactors8 / applyBlendOp）是**设备混合状态**（D3DBLEND / D3DBLENDOP）的语义，
//   ⛔ 与 pixel_format 的 mask/expand 或 Sprite 的 ALPHABLEND `mul255` 不是同一件事
//   （公式不同；合并会改像素结果）—— 审计结论：只保留各自一份，不强行收敛。
void blendFactors8(std::uint32_t factor, unsigned sr, unsigned sg, unsigned sb, unsigned sa, unsigned dr,
                   unsigned dg, unsigned db, unsigned da, unsigned* fr, unsigned* fg, unsigned* fb,
                   unsigned* fa) {
    switch (factor) {
        case kBlendZero: *fr = *fg = *fb = *fa = 0u; break;
        case kBlendOne: *fr = *fg = *fb = *fa = 255u; break;
        case kBlendSrcColor: *fr = sr; *fg = sg; *fb = sb; *fa = sa; break;
        case kBlendInvSrcColor: *fr = 255u - sr; *fg = 255u - sg; *fb = 255u - sb; *fa = 255u - sa; break;
        case kBlendSrcAlpha: *fr = *fg = *fb = *fa = sa; break;
        case kBlendInvSrcAlpha: *fr = *fg = *fb = *fa = 255u - sa; break;
        case kBlendDestAlpha: *fr = *fg = *fb = *fa = da; break;
        case kBlendInvDestAlpha: *fr = *fg = *fb = *fa = 255u - da; break;
        case kBlendDestColor: *fr = dr; *fg = dg; *fb = db; *fa = da; break;
        case kBlendInvDestColor: *fr = 255u - dr; *fg = 255u - dg; *fb = 255u - db; *fa = 255u - da; break;
        case kBlendSrcAlphaSat: {
            const unsigned f = sa < (255u - da) ? sa : (255u - da);
            *fr = *fg = *fb = f;
            *fa = 255u;
            break;
        }
        case kBlendBothSrcAlpha: *fr = sr; *fg = sg; *fb = sb; *fa = sa; break;
        case kBlendBothInvSrcAlpha: *fr = 255u - sr; *fg = 255u - sg; *fb = 255u - sb; *fa = 255u - sa; break;
        default: *fr = *fg = *fb = *fa = 0u; break;
    }
}

unsigned applyBlendOp(std::uint32_t op, unsigned s, unsigned d) {
    switch (op) {
        case kBlendOpAdd: return (s + d + 127u) / 255u > 255u ? 255u : (s + d + 127u) / 255u;
        case kBlendOpSubtract: return s >= d ? (s - d + 127u) / 255u : 0u;
        case kBlendOpRevSubtract: return d >= s ? (d - s + 127u) / 255u : 0u;
        case kBlendOpMin: return s < d ? s : d;
        case kBlendOpMax: return s > d ? s : d;
        default: return (s + d + 127u) / 255u > 255u ? 255u : (s + d + 127u) / 255u;
    }
}

unsigned clamp255(double v) {
    if (v <= 0.0) return 0u;
    if (v >= 255.0) return 255u;
    return static_cast<unsigned>(v + 0.5);
}

// 采样：本线只有 level 0（没有 mip/LOD/导数）⇒ MIN/MAG 合成一条决策：
//   任一要求 LINEAR（ANISOTROPIC 也按 LINEAR 近似）⇒ 双线性；否则 POINT。
//   ★ 这是**近似**：真机会按 LOD 在 MIN 与 MAG 之间选，本线没有足迹信息，无法区分。
void sampleTexture(const TextureView& t, const std::uint32_t address_u, const std::uint32_t address_v,
                   const std::uint32_t mag_filter, const std::uint32_t min_filter, double u, double v,
                   unsigned out[4]) {
    out[0] = 255u;
    out[1] = 255u;
    out[2] = 255u;
    out[3] = 255u;
    if (!t.present || t.width <= 0 || t.height <= 0) return;
    const std::uint32_t bpp = t.format.bytesPerPixel();
    if (bpp == 0) return;
    auto applyAddress = [](std::uint32_t mode, double coord, int size) -> int {
        double c = coord;
        switch (mode) {
            case kAddressWrap:
                c = c - std::floor(c);
                break;
            case kAddressMirror: {
                // 镜像：周期 2（0..1 正序、1..2 逆序）
                double m = c - std::floor(c / 2.0) * 2.0;
                c = m > 1.0 ? 2.0 - m : m;
                break;
            }
            case kAddressClamp:
            case kAddressMirrorOnce:
                if (c < 0.0) c = 0.0;
                if (c > 1.0) c = 1.0;
                break;
            case kAddressBorder:
                if (c < 0.0 || c > 1.0) return -1;
                break;
            default:
                c = c - std::floor(c);
                break;
        }
        int i = static_cast<int>(std::floor(c * static_cast<double>(size)));
        if (i < 0) i = 0;
        if (i >= size) i = size - 1;
        return i;
    };
    auto load = [&](int x, int y, unsigned rgba[4]) {
        if (x < 0 || y < 0 || x >= t.width || y >= t.height) {
            rgba[0] = rgba[1] = rgba[2] = rgba[3] = 0u;
            return;
        }
        const std::uint8_t* p =
            t.pixels + static_cast<std::size_t>(y) * t.pitch + static_cast<std::size_t>(x) * bpp;
        const std::uint32_t raw = pixel_format::loadPixel(p, t.format.bpp);
        rgba[0] = pixel_format::expandChannel(raw, t.format.rmask);
        rgba[1] = pixel_format::expandChannel(raw, t.format.gmask);
        rgba[2] = pixel_format::expandChannel(raw, t.format.bmask);
        // ★ mask==0（无 alpha 通道）⇒ expandChannel 返回 255（= 不透明）；这份语义只在 pixel_format 里
        rgba[3] = pixel_format::expandChannel(raw, t.format.amask);
    };
    // ★ MIN 与 MAG 都参与（MIN 不再被忽略）；没有 mip/LOD ⇒ 这是明确记录的近似。
    const bool linear = samplerUsesLinear(mag_filter, min_filter);
    if (mag_filter == kTexfNone && min_filter == kTexfNone) {
        // 不可见组合（D3D 不会这样用）：不猜像素 ⇒ 透明黑；SetSamplerState 已 recordUnsupported。
        out[0] = out[1] = out[2] = out[3] = 0u;
        return;
    }
    if (!linear) {
        const int x = applyAddress(address_u, u, t.width);
        const int y = applyAddress(address_v, v, t.height);
        if (x < 0 || y < 0) {
            out[0] = out[1] = out[2] = out[3] = 0u;
            return;
        }
        unsigned rgba[4];
        load(x, y, rgba);
        out[0] = rgba[0];
        out[1] = rgba[1];
        out[2] = rgba[2];
        out[3] = rgba[3];
        return;
    }
    // LINEAR：双线性（texel 中心对齐；D3D 的 -0.5 偏移）
    const double fx = u * static_cast<double>(t.width) - 0.5;
    const double fy = v * static_cast<double>(t.height) - 0.5;
    const double x0f = std::floor(fx);
    const double y0f = std::floor(fy);
    const double tx = fx - x0f;
    const double ty = fy - y0f;
    const int x0 = applyAddress(address_u, (x0f + 0.5) / static_cast<double>(t.width), t.width);
    const int x1 = applyAddress(address_u, (x0f + 1.5) / static_cast<double>(t.width), t.width);
    const int y0 = applyAddress(address_v, (y0f + 0.5) / static_cast<double>(t.height), t.height);
    const int y1 = applyAddress(address_v, (y0f + 1.5) / static_cast<double>(t.height), t.height);
    unsigned c00[4], c10[4], c01[4], c11[4];
    load(x0, y0, c00);
    load(x1, y0, c10);
    load(x0, y1, c01);
    load(x1, y1, c11);
    for (int i = 0; i < 4; ++i) {
        const double a = static_cast<double>(c00[i]) * (1.0 - tx) + static_cast<double>(c10[i]) * tx;
        const double b = static_cast<double>(c01[i]) * (1.0 - tx) + static_cast<double>(c11[i]) * tx;
        out[i] = clamp255(a * (1.0 - ty) + b * ty);
    }
}

struct StageColor {
    double c[4] = {1.0, 1.0, 1.0, 1.0};  // 0..255
};

// 取一个 stage 的源操作数（D3DTA_*）
void stageArg(X86& cpu, const DrawContext& ctx, std::uint32_t arg, const StageColor& current,
              const StageColor& diffuse, const StageColor& specular, const StageColor& texture,
              const StageColor& temp, const StageColor& constant, double out[4]) {
    (void)cpu;
    const std::uint32_t sel = arg & kTaSelectMask;
    const StageColor* src = &diffuse;
    switch (sel) {
        case kTaDiffuse: src = &diffuse; break;
        case kTaCurrent: src = &current; break;
        case kTaTexture: src = &texture; break;
        case kTaTFactor: {
            double a = 1.0, r = 1.0, g = 1.0, b = 1.0;
            splitColor(ctx.texture_factor, &a, &r, &g, &b);
            out[0] = r;
            out[1] = g;
            out[2] = b;
            out[3] = a;
            if ((arg & kTaAlphaReplicate) != 0) out[0] = out[1] = out[2] = out[3];
            if ((arg & kTaComplement) != 0)
                for (int i = 0; i < 4; ++i) out[i] = 255.0 - out[i];
            return;
        }
        case kTaSpecular: src = &specular; break;
        case kTaTemp: src = &temp; break;
        case kTaConstant: src = &constant; break;
        default: src = &diffuse; break;
    }
    for (int i = 0; i < 4; ++i) out[i] = src->c[i];
    if ((arg & kTaAlphaReplicate) != 0) out[0] = out[1] = out[2] = out[3];
    if ((arg & kTaComplement) != 0)
        for (int i = 0; i < 4; ++i) out[i] = 255.0 - out[i];
}

void applyTextureOp(X86& cpu, const DrawContext& ctx, std::uint32_t stage, const StageColor& diffuse,
                    const StageColor& specular, const StageColor& texture, const StageColor& current_in,
                    StageColor* color_out, StageColor* alpha_out) {
    // ★ 状态全部来自 Draw 入口的快照（⛔ 不在像素循环里 texStageOf）
    //   唯一还会碰 guest 的是"不支持的纹理级 OP"的 `recordUnsupported`（幂等、只在真出现时）；
    //   本线支持的 OP 集合内，像素循环零 guest 读。
    const TextureStageSnapshot& ts = ctx.stages[stage];
    const StageColor temp;  // 本线没有 TEMP 寄存器：全 0（记录见下）
    StageColor constant;
    {
        double a = 1.0, r = 1.0, g = 1.0, b = 1.0;
        splitColor(ts.constant, &a, &r, &g, &b);
        constant.c[0] = r;
        constant.c[1] = g;
        constant.c[2] = b;
        constant.c[3] = a;
    }
    const std::uint32_t color_op = ts.color_op;
    const std::uint32_t alpha_op = ts.alpha_op;
    const std::uint32_t arg0 = ts.color_arg0;
    double a1[4], a2[4], a0[4];
    stageArg(cpu, ctx, ts.color_arg1, current_in, diffuse, specular, texture, temp, constant, a1);
    stageArg(cpu, ctx, ts.color_arg2, current_in, diffuse, specular, texture, temp, constant, a2);
    stageArg(cpu, ctx, arg0, current_in, diffuse, specular, texture, temp, constant, a0);
    double aa1[4], aa2[4];
    stageArg(cpu, ctx, ts.alpha_arg1, current_in, diffuse, specular, texture, temp, constant, aa1);
    stageArg(cpu, ctx, ts.alpha_arg2, current_in, diffuse, specular, texture, temp, constant, aa2);
    // 颜色通道的 op（alpha 通道单独走下面那段：D3D 的 alpha op 只作用在 alpha 分量上）
    auto run = [&](std::uint32_t code, const double x[4], const double y[4], const double z[4], double out[3]) {
        std::uint32_t unsupported = 0;
        switch (code) {
            case kTopSelectArg1: out[0] = x[0]; out[1] = x[1]; out[2] = x[2]; break;
            case kTopSelectArg2: out[0] = y[0]; out[1] = y[1]; out[2] = y[2]; break;
            case kTopModulate: out[0] = x[0] * y[0] / 255.0; out[1] = x[1] * y[1] / 255.0; out[2] = x[2] * y[2] / 255.0; break;
            case kTopModulate2x:
                out[0] = x[0] * y[0] / 255.0 * 2.0;
                out[1] = x[1] * y[1] / 255.0 * 2.0;
                out[2] = x[2] * y[2] / 255.0 * 2.0;
                break;
            case kTopModulate4x:
                out[0] = x[0] * y[0] / 255.0 * 4.0;
                out[1] = x[1] * y[1] / 255.0 * 4.0;
                out[2] = x[2] * y[2] / 255.0 * 4.0;
                break;
            case kTopAdd: out[0] = x[0] + y[0]; out[1] = x[1] + y[1]; out[2] = x[2] + y[2]; break;
            case kTopAddSigned:
                out[0] = x[0] + y[0] - 127.5;
                out[1] = x[1] + y[1] - 127.5;
                out[2] = x[2] + y[2] - 127.5;
                break;
            case kTopAddSigned2x:
                out[0] = (x[0] + y[0] - 127.5) * 2.0;
                out[1] = (x[1] + y[1] - 127.5) * 2.0;
                out[2] = (x[2] + y[2] - 127.5) * 2.0;
                break;
            case kTopSubtract: out[0] = x[0] - y[0]; out[1] = x[1] - y[1]; out[2] = x[2] - y[2]; break;
            case kTopAddSmooth:
                out[0] = x[0] + y[0] - x[0] * y[0] / 255.0;
                out[1] = x[1] + y[1] - x[1] * y[1] / 255.0;
                out[2] = x[2] + y[2] - x[2] * y[2] / 255.0;
                break;
            case kTopBlendDiffuseAlpha:
            case kTopBlendTextureAlpha:
            case kTopBlendFactorAlpha:
            case kTopBlendCurrentAlpha:
            case kTopBlendTextureAlphaPM: {
                double t = 0.0;
                if (code == kTopBlendDiffuseAlpha) t = x[3];
                else if (code == kTopBlendTextureAlpha) t = y[3];
                else if (code == kTopBlendFactorAlpha) t = 255.0;
                else if (code == kTopBlendCurrentAlpha) t = x[3];
                else t = y[3];
                t /= 255.0;
                out[0] = x[0] * t + y[0] * (1.0 - t);
                out[1] = x[1] * t + y[1] * (1.0 - t);
                out[2] = x[2] * t + y[2] * (1.0 - t);
                break;
            }
            case kTopMultiplyAdd:
                out[0] = z[0] + x[0] * y[0] / 255.0;
                out[1] = z[1] + x[1] * y[1] / 255.0;
                out[2] = z[2] + x[2] * y[2] / 255.0;
                break;
            case kTopLerp: {
                const double t = z[0] / 255.0;
                out[0] = x[0] * t + y[0] * (1.0 - t);
                out[1] = x[1] * t + y[1] * (1.0 - t);
                out[2] = x[2] * t + y[2] * (1.0 - t);
                break;
            }
            case kTopModulateAlphaAddColor:
                out[0] = x[0] + y[0] * x[3] / 255.0;
                out[1] = x[1] + y[1] * x[3] / 255.0;
                out[2] = x[2] + y[2] * x[3] / 255.0;
                break;
            case kTopModulateColorAddAlpha:
                out[0] = x[3] + y[0] * x[0] / 255.0;
                out[1] = x[3] + y[1] * x[1] / 255.0;
                out[2] = x[3] + y[2] * x[2] / 255.0;
                break;
            default:
                unsupported = code;
                out[0] = x[0];
                out[1] = x[1];
                out[2] = x[2];
                break;
        }
        if (unsupported != 0) {
            char buf[96];
            std::snprintf(buf, sizeof(buf), "纹理级 OP 0x%X（级 %u）", unsupported, stage);
            recordUnsupported(cpu, ctx.pe, 24u, buf, unsupported);
        }
        for (int i = 0; i < 3; ++i) {
            if (out[i] < 0.0) out[i] = 0.0;
            if (out[i] > 255.0) out[i] = 255.0;
        }
    };
    double rgb[3];
    run(color_op, a1, a2, a0, rgb);
    color_out->c[0] = rgb[0];
    color_out->c[1] = rgb[1];
    color_out->c[2] = rgb[2];
    color_out->c[3] = current_in.c[3];
    // alpha 级（D3D 的 alpha op 只作用在 alpha 分量上）
    alpha_out->c[0] = current_in.c[0];
    alpha_out->c[1] = current_in.c[1];
    alpha_out->c[2] = current_in.c[2];
    double alpha_val = 0.0;
    {
        std::uint32_t unsupported = 0;
        switch (alpha_op) {
            case kTopSelectArg1: alpha_val = aa1[3]; break;
            case kTopSelectArg2: alpha_val = aa2[3]; break;
            case kTopModulate: alpha_val = aa1[3] * aa2[3] / 255.0; break;
            case kTopModulate2x: alpha_val = aa1[3] * aa2[3] / 255.0 * 2.0; break;
            case kTopModulate4x: alpha_val = aa1[3] * aa2[3] / 255.0 * 4.0; break;
            case kTopAdd: alpha_val = aa1[3] + aa2[3]; break;
            case kTopAddSigned: alpha_val = aa1[3] + aa2[3] - 127.5; break;
            case kTopAddSigned2x: alpha_val = (aa1[3] + aa2[3] - 127.5) * 2.0; break;
            case kTopSubtract: alpha_val = aa1[3] - aa2[3]; break;
            case kTopAddSmooth: alpha_val = aa1[3] + aa2[3] - aa1[3] * aa2[3] / 255.0; break;
            case kTopBlendDiffuseAlpha:
            case kTopBlendTextureAlpha:
            case kTopBlendFactorAlpha:
            case kTopBlendCurrentAlpha:
            case kTopBlendTextureAlphaPM: {
                double t = 0.0;
                if (alpha_op == kTopBlendDiffuseAlpha) t = aa1[3];
                else if (alpha_op == kTopBlendTextureAlpha) t = aa2[3];
                else if (alpha_op == kTopBlendFactorAlpha) t = 255.0;
                else if (alpha_op == kTopBlendCurrentAlpha) t = aa1[3];
                else t = aa2[3];
                t /= 255.0;
                alpha_val = aa1[3] * t + aa2[3] * (1.0 - t);
                break;
            }
            default:
                unsupported = alpha_op;
                alpha_val = aa1[3];
                break;
        }
        if (unsupported != 0) {
            char buf[96];
            std::snprintf(buf, sizeof(buf), "纹理级 ALPHAOP 0x%X（级 %u）", unsupported, stage);
            recordUnsupported(cpu, ctx.pe, 25u, buf, unsupported);
        }
        if (alpha_val < 0.0) alpha_val = 0.0;
        if (alpha_val > 255.0) alpha_val = 255.0;
        alpha_out->c[3] = alpha_val;
    }
}

// ---------------------------------------------------------------------------------------
// 光栅化一个屏幕空间三角形
// ---------------------------------------------------------------------------------------
void rasterTriangle(DrawContext& ctx, const SVertex& va, const SVertex& vb, const SVertex& vc) {
    X86& cpu = *ctx.cpu;
    const double area = (vb.x - va.x) * (vc.y - va.y) - (vb.y - va.y) * (vc.x - va.x);
    if (!finite(area) || std::fabs(area) < 1e-12) return;
    // 正面 = 屏幕空间顺时针（y 向下时 area > 0）；CULLMODE 决定丢哪一侧
    if (area > 0.0) {
        if (ctx.cull == kCullCw) return;  // 剔除正面
    } else {
        if (ctx.cull == kCullCcw) return;  // 剔除背面
    }
    // 统一成 area > 0 的绕序（只影响插值符号，不影响可见性）
    const SVertex* p0 = &va;
    const SVertex* p1 = &vb;
    const SVertex* p2 = &vc;
    double a2 = area;
    if (a2 < 0.0) {
        p1 = &vc;
        p2 = &vb;
        a2 = -a2;
    }
    // 包围盒 ∩ 视口 ∩ 裁剪矩形
    double minx = p0->x < p1->x ? p0->x : p1->x;
    if (p2->x < minx) minx = p2->x;
    double maxx = p0->x > p1->x ? p0->x : p1->x;
    if (p2->x > maxx) maxx = p2->x;
    double miny = p0->y < p1->y ? p0->y : p1->y;
    if (p2->y < miny) miny = p2->y;
    double maxy = p0->y > p1->y ? p0->y : p1->y;
    if (p2->y > maxy) maxy = p2->y;
    int x0 = static_cast<int>(std::floor(minx));
    int x1 = static_cast<int>(std::ceil(maxx));
    int y0 = static_cast<int>(std::floor(miny));
    int y1 = static_cast<int>(std::ceil(maxy));
    if (x0 < ctx.scissor_x0) x0 = ctx.scissor_x0;
    if (y0 < ctx.scissor_y0) y0 = ctx.scissor_y0;
    if (x1 > ctx.scissor_x1) x1 = ctx.scissor_x1;
    if (y1 > ctx.scissor_y1) y1 = ctx.scissor_y1;
    if (x1 <= x0 || y1 <= y0) return;
    // 边函数（p0→p1→p2，area>0 ⇒ 内部为正）
    auto edge = [](const SVertex& a, const SVertex& b, double px, double py) {
        return (b.x - a.x) * (py - a.y) - (b.y - a.y) * (px - a.x);
    };
    // 左上规则：top = 水平边且 dx > 0；left = dy < 0（y 向下、area>0 的顺时针绕序）
    auto topLeft = [](const SVertex& a, const SVertex& b) {
        const double dx = b.x - a.x;
        const double dy = b.y - a.y;
        if (dy < 0.0) return true;                 // left edge
        if (dy == 0.0 && dx > 0.0) return true;    // top edge
        return false;
    };
    const bool tl0 = topLeft(*p0, *p1);
    const bool tl1 = topLeft(*p1, *p2);
    const bool tl2 = topLeft(*p2, *p0);
    // 状态快照
    const bool z_enable = ctx.z_enable;
    const bool z_write = ctx.z_write;
    const std::uint32_t z_func = ctx.z_func;
    for (int py = y0; py < y1; ++py) {
        const double cy = static_cast<double>(py) + 0.5;
        for (int px = x0; px < x1; ++px) {
            const double cx = static_cast<double>(px) + 0.5;
            double e0 = edge(*p0, *p1, cx, cy);
            double e1 = edge(*p1, *p2, cx, cy);
            double e2 = edge(*p2, *p0, cx, cy);
            // 填充规则
            if (!(e0 > 0.0 || (e0 == 0.0 && tl0))) continue;
            if (!(e1 > 0.0 || (e1 == 0.0 && tl1))) continue;
            if (!(e2 > 0.0 || (e2 == 0.0 && tl2))) continue;
            // ★ 边函数 E_i 是"边 i→i+1 对第三个顶点"的面积 ⇒ 权重是**轮换**的：
            //   w0 = e1/a2（v0 对面是边 v1→v2）、w1 = e2/a2、w2 = e0/a2。
            const double l0 = e1 / a2;
            const double l1 = e2 / a2;
            const double l2 = e0 / a2;
            const double wsum = l0 * p0->invw + l1 * p1->invw + l2 * p2->invw;
            const double invw_sum = wsum;
            if (invw_sum == 0.0) continue;
            const double z = l0 * p0->z + l1 * p1->z + l2 * p2->z;
            std::size_t depth_off = static_cast<std::size_t>(py) * ctx.depth_pitch_floats +
                                    static_cast<std::size_t>(px);
            if (z_enable) {
                const float old = ctx.depth_base[depth_off];
                bool pass = false;
                switch (z_func) {
                    case kCmpNever: pass = false; break;
                    case kCmpLess: pass = z < static_cast<double>(old); break;
                    case kCmpEqual: pass = z == static_cast<double>(old); break;
                    case kCmpLessEqual: pass = z <= static_cast<double>(old); break;
                    case kCmpGreater: pass = z > static_cast<double>(old); break;
                    case kCmpNotEqual: pass = z != static_cast<double>(old); break;
                    case kCmpGreaterEqual: pass = z >= static_cast<double>(old); break;
                    case kCmpAlways: pass = true; break;
                    default: pass = false; break;
                }
                if (!pass) continue;
            }
            // 顶点属性（透视校正）
            double u[kMaxTexCoords];
            double v[kMaxTexCoords];
            for (std::uint32_t t = 0; t < kMaxTexCoords; ++t) {
                u[t] = (l0 * p0->u[t] + l1 * p1->u[t] + l2 * p2->u[t]) / invw_sum;
                v[t] = (l0 * p0->v[t] + l1 * p1->v[t] + l2 * p2->v[t]) / invw_sum;
            }
            StageColor diffuse;
            diffuse.c[0] = (l0 * p0->r + l1 * p1->r + l2 * p2->r) / invw_sum;
            diffuse.c[1] = (l0 * p0->g + l1 * p1->g + l2 * p2->g) / invw_sum;
            diffuse.c[2] = (l0 * p0->b + l1 * p1->b + l2 * p2->b) / invw_sum;
            diffuse.c[3] = (l0 * p0->a + l1 * p1->a + l2 * p2->a) / invw_sum;
            StageColor specular;
            specular.c[0] = (l0 * p0->sr + l1 * p1->sr + l2 * p2->sr) / invw_sum;
            specular.c[1] = (l0 * p0->sg + l1 * p1->sg + l2 * p2->sg) / invw_sum;
            specular.c[2] = (l0 * p0->sb + l1 * p1->sb + l2 * p2->sb) / invw_sum;
            specular.c[3] = (l0 * p0->sa + l1 * p1->sa + l2 * p2->sa) / invw_sum;
            if (ctx.shade == kShadeFlat) {
                // FLAT：用 p0（D3D 的 provoking vertex 是第 0 个顶点；本线按此实现）
                diffuse.c[0] = p0->r;
                diffuse.c[1] = p0->g;
                diffuse.c[2] = p0->b;
                diffuse.c[3] = p0->a;
                specular.c[0] = p0->sr;
                specular.c[1] = p0->sg;
                specular.c[2] = p0->sb;
                specular.c[3] = p0->sa;
                for (std::uint32_t t = 0; t < kMaxTexCoords; ++t) {
                    u[t] = p0->u[t] / (p0->invw != 0.0 ? p0->invw : 1.0);
                    v[t] = p0->v[t] / (p0->invw != 0.0 ? p0->invw : 1.0);
                }
            }
            // 纹理级链（★ 状态全来自 Draw 快照；⛔ 不再每像素读 guest）
            StageColor current = diffuse;
            for (std::uint32_t stage = 0; stage < kMaxTextureStages; ++stage) {
                const TextureStageSnapshot& ts = ctx.stages[stage];
                if (ts.color_op == kTopDisable && ts.alpha_op == kTopDisable) break;
                StageColor texture;
                texture.c[0] = texture.c[1] = texture.c[2] = texture.c[3] = 255.0;
                std::uint32_t coord = ts.tex_coord_index & 0xFFFFu;
                if (coord >= kMaxTexCoords) coord = 0;
                const TextureView& tv = ctx.textures[stage];
                if (tv.present) {
                    const SamplerSnapshot& ss = ctx.samplers[stage];
                    unsigned rgba[4];
                    sampleTexture(tv, ss.address_u, ss.address_v, ss.mag_filter, ss.min_filter, u[coord], v[coord],
                                  rgba);
                    texture.c[0] = static_cast<double>(rgba[0]);
                    texture.c[1] = static_cast<double>(rgba[1]);
                    texture.c[2] = static_cast<double>(rgba[2]);
                    texture.c[3] = static_cast<double>(rgba[3]);
                }
                StageColor c_out;
                StageColor a_out;
                applyTextureOp(cpu, ctx, stage, diffuse, specular, texture, current, &c_out, &a_out);
                current.c[0] = c_out.c[0];
                current.c[1] = c_out.c[1];
                current.c[2] = c_out.c[2];
                current.c[3] = a_out.c[3];
            }
            unsigned fr = clamp255(current.c[0]);
            unsigned fg = clamp255(current.c[1]);
            unsigned fb = clamp255(current.c[2]);
            unsigned fa = clamp255(current.c[3]);
            // alpha test
            if (ctx.alpha_test) {
                const std::uint32_t ref = ctx.alpha_ref & 0xFFu;
                bool pass = false;
                switch (ctx.alpha_func) {
                    case kCmpNever: pass = false; break;
                    case kCmpLess: pass = fa < ref; break;
                    case kCmpEqual: pass = fa == ref; break;
                    case kCmpLessEqual: pass = fa <= ref; break;
                    case kCmpGreater: pass = fa > ref; break;
                    case kCmpNotEqual: pass = fa != ref; break;
                    case kCmpGreaterEqual: pass = fa >= ref; break;
                    case kCmpAlways: pass = true; break;
                    default: pass = false; break;
                }
                if (!pass) continue;
            }
            std::uint8_t* dst = ctx.color_base + static_cast<std::size_t>(py) * ctx.color_pitch +
                                static_cast<std::size_t>(px) * ctx.color_fmt.bytesPerPixel();
            std::uint32_t out_r = fr, out_g = fg, out_b = fb, out_a = fa;
            if (ctx.blend) {
                const std::uint32_t dst_raw = pixel_format::loadPixel(dst, ctx.color_fmt.bpp);
                const unsigned dr = pixel_format::expandChannel(dst_raw, ctx.color_fmt.rmask);
                const unsigned dg = pixel_format::expandChannel(dst_raw, ctx.color_fmt.gmask);
                const unsigned db = pixel_format::expandChannel(dst_raw, ctx.color_fmt.bmask);
                const unsigned da = pixel_format::expandChannel(dst_raw, ctx.color_fmt.amask);
                unsigned sr_f = 0, sg_f = 0, sb_f = 0, sa_f = 0;
                blendFactors8(ctx.src_blend, fr, fg, fb, fa, dr, dg, db, da, &sr_f, &sg_f, &sb_f, &sa_f);
                unsigned dr_f = 0, dg_f = 0, db_f = 0, da_f = 0;
                blendFactors8(ctx.dst_blend, fr, fg, fb, fa, dr, dg, db, da, &dr_f, &dg_f, &db_f, &da_f);
                out_r = applyBlendOp(ctx.blend_op, fr * sr_f, dr * dr_f);
                out_g = applyBlendOp(ctx.blend_op, fg * sg_f, dg * dg_f);
                out_b = applyBlendOp(ctx.blend_op, fb * sb_f, db * db_f);
                if (ctx.separate_alpha_blend) {
                    unsigned sa2 = 0, da2 = 0, dummy1 = 0, dummy2 = 0;
                    blendFactors8(ctx.src_blend_a, fr, fg, fb, fa, dr, dg, db, da, &sa2, &dummy1, &dummy2,
                                  &dummy1);
                    unsigned ds2 = 0;
                    blendFactors8(ctx.dst_blend_a, fr, fg, fb, fa, dr, dg, db, da, &ds2, &dummy1, &dummy2,
                                  &dummy1);
                    out_a = applyBlendOp(ctx.blend_op_a, fa * sa2, da * ds2);
                } else {
                    out_a = applyBlendOp(ctx.blend_op, fa * sa_f, da * da_f);
                }
            }
            const std::uint32_t mask = ctx.color_write & 0xFu;
            if ((mask & 0x1u) == 0) out_r = pixel_format::expandChannel(pixel_format::loadPixel(dst, ctx.color_fmt.bpp), ctx.color_fmt.rmask);
            if ((mask & 0x2u) == 0) out_g = pixel_format::expandChannel(pixel_format::loadPixel(dst, ctx.color_fmt.bpp), ctx.color_fmt.gmask);
            if ((mask & 0x4u) == 0) out_b = pixel_format::expandChannel(pixel_format::loadPixel(dst, ctx.color_fmt.bpp), ctx.color_fmt.bmask);
            if ((mask & 0x8u) == 0)
                out_a = pixel_format::expandChannel(pixel_format::loadPixel(dst, ctx.color_fmt.bpp),
                                                    ctx.color_fmt.amask);
            if (mask != 0u) {
                pixel_format::storePixel(dst, ctx.color_fmt.bpp,
                                  pixel_format::packColor(ctx.color_fmt, out_r, out_g, out_b, out_a));
            }
            if (z_enable && z_write) ctx.depth_base[depth_off] = static_cast<float>(z);
            ++ctx.pixels_written;
        }
    }
}

// =====================================================================================
// 顶点读取 / 装配
// =====================================================================================
struct VertexSource {
    const std::uint8_t* data = nullptr;
    std::uint32_t stride = 0;
    std::uint32_t base_offset = 0;  // stream offset
    std::uint32_t count = 0;        // 可用顶点数（越界检查用）
};

bool readVertex(X86& cpu, const FvfLayout& fvf, const std::uint8_t* base, std::uint32_t ea, CVertex* out,
                std::string* why) {
    (void)cpu;
    auto f32 = [&](std::uint32_t off) {
        float f = 0.0f;
        std::memcpy(&f, base + off, sizeof(f));
        return static_cast<double>(f);
    };
    out->pretransformed = fvf.xyz_rhw;
    if (fvf.xyz_rhw) {
        out->x = f32(fvf.pos_offset + 0u);
        out->y = f32(fvf.pos_offset + 4u);
        out->z = f32(fvf.pos_offset + 8u);
        const double rhw = f32(fvf.pos_offset + 12u);
        out->w = rhw != 0.0 ? 1.0 / rhw : 1.0;
    } else if (fvf.xyzw) {
        out->x = f32(fvf.pos_offset + 0u);
        out->y = f32(fvf.pos_offset + 4u);
        out->z = f32(fvf.pos_offset + 8u);
        out->w = f32(fvf.pos_offset + 12u);
    } else {
        out->x = f32(fvf.pos_offset + 0u);
        out->y = f32(fvf.pos_offset + 4u);
        out->z = f32(fvf.pos_offset + 8u);
        out->w = 1.0;
    }
    for (std::uint32_t t = 0; t < fvf.tex_count; ++t) {
        out->u[t] = f32(fvf.tex_offset[t] + 0u);
        out->v[t] = fvf.tex_size[t] >= 2u ? f32(fvf.tex_offset[t] + 4u) : 0.0;
    }
    if (fvf.has_diffuse) {
        const std::uint8_t* p = base + fvf.diffuse_offset;
        const std::uint32_t c = static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
                                (static_cast<std::uint32_t>(p[2]) << 16) |
                                (static_cast<std::uint32_t>(p[3]) << 24);
        splitColor(c, &out->a, &out->r, &out->g, &out->b);
    }
    if (fvf.has_specular) {
        const std::uint8_t* p = base + fvf.specular_offset;
        const std::uint32_t c = static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
                                (static_cast<std::uint32_t>(p[2]) << 16) |
                                (static_cast<std::uint32_t>(p[3]) << 24);
        splitColor(c, &out->sa, &out->sr, &out->sg, &out->sb);
    }
    (void)ea;
    (void)why;
    return true;
}

// =====================================================================================
// 裁剪（偏差 #5：⛔ 不再只有近平面）
//
// D3D 的**齐次裁剪空间**约定：0 <= z <= w、-w <= x <= w、-w <= y <= w。
//   · XYZ / XYZW 顶点：对 6 个半空间做 Sutherland–Hodgman 裁剪（保持绕序）：
//       近 z>=0 · 远 w-z>=0 · 左 x+w>=0 · 右 w-x>=0 · 下 y+w>=0 · 上 w-y>=0。
//     `w<0`（相机背后）的点必然落在远/左/右…之外 ⇒ 整体被裁掉（旧实现只裁近平面时
//     会拿负 w 做投影，落到错误位置）。
//   · XYZRHW 顶点：x/y **已经是屏幕坐标**，齐次面在数学上不适用（`w=1/rhw` 与屏幕 x/y 无关）；
//     真机对预变换顶点是在**屏幕空间裁到视口矩形**，这里照做；z 仍是 [0,1] 深度
//     （`toScreen` 不对预变换顶点缩放 MinZ/MaxZ，所以远平面就是 z<=1）。
//
// ⚠ 明确未做（⛔ 不静默）：用户裁剪面（D3DRS_CLIPPLANEENABLE）仍由 `recordUnsupported` 记录；
//   guard band 没有单独实现（精确裁剪已经让屏幕外几何不进光栅，效果等价）。
// =====================================================================================
CVertex lerpVertex(const CVertex& a, const CVertex& b, double t) {
    CVertex m;
    m.pretransformed = a.pretransformed;  // 同一批顶点同为预变换（FVF 是每次 Draw 一份）
    m.x = a.x + (b.x - a.x) * t;
    m.y = a.y + (b.y - a.y) * t;
    m.z = a.z + (b.z - a.z) * t;
    m.w = a.w + (b.w - a.w) * t;
    for (std::uint32_t k = 0; k < kMaxTexCoords; ++k) {
        m.u[k] = a.u[k] + (b.u[k] - a.u[k]) * t;
        m.v[k] = a.v[k] + (b.v[k] - a.v[k]) * t;
    }
    m.r = a.r + (b.r - a.r) * t;
    m.g = a.g + (b.g - a.g) * t;
    m.b = a.b + (b.b - a.b) * t;
    m.a = a.a + (b.a - a.a) * t;
    m.sr = a.sr + (b.sr - a.sr) * t;
    m.sg = a.sg + (b.sg - a.sg) * t;
    m.sb = a.sb + (b.sb - a.sb) * t;
    m.sa = a.sa + (b.sa - a.sa) * t;
    return m;
}

// 单个半空间：dist(v) >= 0 为内侧。`in` / `out` 不得别名。
template <typename Dist>
std::uint32_t clipHalfSpace(const CVertex* in, std::uint32_t n, Dist dist, CVertex* out) {
    std::uint32_t count = 0;
    for (std::uint32_t i = 0; i < n; ++i) {
        const CVertex& a = in[i];
        const CVertex& b = in[(i + 1u) % n];
        const double da = dist(a);
        const double db = dist(b);
        if (da >= 0.0) {
            if (count < kMaxClipVerts) out[count++] = a;
        }
        if ((da >= 0.0) != (db >= 0.0)) {
            const double t = da / (da - db);
            if (count < kMaxClipVerts) out[count++] = lerpVertex(a, b, t);
        }
    }
    return count;
}

// 凸多边形过一串平面。三角形最多变成 3+6=9 个顶点（< kMaxClipVerts=16）。
template <typename... Dist>
std::uint32_t clipAgainst(const CVertex* in, std::uint32_t n, CVertex* out, Dist... dists) {
    CVertex buf_a[kMaxClipVerts];
    CVertex buf_b[kMaxClipVerts];
    CVertex* cur = buf_a;
    CVertex* next = buf_b;
    const std::uint32_t start = n < kMaxClipVerts ? n : kMaxClipVerts;
    for (std::uint32_t i = 0; i < start; ++i) cur[i] = in[i];
    n = start;
    auto pass = [&](auto dist) {
        n = clipHalfSpace(cur, n, dist, next);
        CVertex* tmp = cur;
        cur = next;
        next = tmp;
    };
    (pass(dists), ...);
    for (std::uint32_t i = 0; i < n; ++i) out[i] = cur[i];
    return n;
}

// XYZ / XYZW：齐次裁剪空间 6 个面
std::uint32_t clipHomogeneous(const CVertex* in, std::uint32_t n, CVertex* out) {
    return clipAgainst(in, n, out, [](const CVertex& v) { return v.z; },        // 近
                       [](const CVertex& v) { return v.w - v.z; },              // 远
                       [](const CVertex& v) { return v.x + v.w; },              // 左
                       [](const CVertex& v) { return v.w - v.x; },              // 右
                       [](const CVertex& v) { return v.y + v.w; },              // 下
                       [](const CVertex& v) { return v.w - v.y; });             // 上
}

// XYZRHW：屏幕空间裁到**视口矩形**（x/y）+ 深度 [0,1]（z）
std::uint32_t clipScreen(const CVertex* in, std::uint32_t n, const DrawContext& ctx, CVertex* out) {
    if (ctx.viewport_w <= 0 || ctx.viewport_h <= 0) return 0;
    const double x0 = static_cast<double>(ctx.viewport_x);
    const double y0 = static_cast<double>(ctx.viewport_y);
    const double x1 = x0 + static_cast<double>(ctx.viewport_w);
    const double y1 = y0 + static_cast<double>(ctx.viewport_h);
    return clipAgainst(in, n, out, [x0](const CVertex& v) { return v.x - x0; },
                       [x1](const CVertex& v) { return x1 - v.x; },
                       [y0](const CVertex& v) { return v.y - y0; },
                       [y1](const CVertex& v) { return y1 - v.y; },
                       [](const CVertex& v) { return v.z; },        // 近
                       [](const CVertex& v) { return 1.0 - v.z; }); // 远
}

bool toScreen(const CVertex& v, const DrawContext& ctx, SVertex* out) {
    if (v.pretransformed) {
        out->x = v.x;
        out->y = v.y;
        out->z = v.z;
        const double invw = v.w != 0.0 ? 1.0 / v.w : 1.0;  // w = 1/rhw ⇒ invw = rhw
        out->invw = invw;
        for (std::uint32_t t = 0; t < kMaxTexCoords; ++t) {
            out->u[t] = v.u[t] * invw;
            out->v[t] = v.v[t] * invw;
        }
        out->r = v.r * invw;
        out->g = v.g * invw;
        out->b = v.b * invw;
        out->a = v.a * invw;
        out->sr = v.sr * invw;
        out->sg = v.sg * invw;
        out->sb = v.sb * invw;
        out->sa = v.sa * invw;
        return true;
    }
    if (!finite(v.w) || std::fabs(v.w) < 1e-12) return false;
    const double invw = 1.0 / v.w;
    out->x = (v.x * invw * 0.5 + 0.5) * static_cast<double>(ctx.viewport_w) + static_cast<double>(ctx.viewport_x);
    out->y = (-v.y * invw * 0.5 + 0.5) * static_cast<double>(ctx.viewport_h) + static_cast<double>(ctx.viewport_y);
    out->z = v.z * invw;
    out->invw = invw;
    for (std::uint32_t t = 0; t < kMaxTexCoords; ++t) {
        out->u[t] = v.u[t] * invw;
        out->v[t] = v.v[t] * invw;
    }
    out->r = v.r * invw;
    out->g = v.g * invw;
    out->b = v.b * invw;
    out->a = v.a * invw;
    out->sr = v.sr * invw;
    out->sg = v.sg * invw;
    out->sb = v.sb * invw;
    out->sa = v.sa * invw;
    return true;
}

}  // namespace

// =====================================================================================
// 状态块生命周期
// =====================================================================================
std::uint32_t createForDevice(X86& cpu, std::uint32_t device_state, int width, int height, std::string* why) {
    Machine* m = nullptr;
    if (!requireMachine(cpu, &m, why)) return 0;
    if (!cpu.heap || !cpu.heap->ready()) {
        if (why) *why = "D3D9 流水线：guest 堆未就绪（流水线状态块要放在 guest 堆上）";
        return 0;
    }
    if (width <= 0 || height <= 0) {
        if (why) *why = "D3D9 流水线：后缓冲尺寸非法";
        return 0;
    }
    const std::uint32_t pe = cpu.heap->alloc(kPlBytes, /*zero=*/true);
    if (pe == 0) {
        if (why) *why = std::string("D3D9 流水线：guest 堆分配状态块失败：") + cpu.heap->lastError();
        return 0;
    }
    initDefaults(cpu, pe, width, height);
    // 深度缓冲：SurfaceArena 里的 32bpp 表面（像素存 float 位模式）。清成 1.0f（远平面）。
    // ★ 格式走 pixel_format 的唯一一份（⛔ 不再在这里手抄 BGRA8888 的四个掩码）
    const SurfaceFormat depth_fmt = pixel_format::formatForBpp(32);
    const std::uint32_t depth = m->surfaces().create(width, height, depth_fmt);
    if (depth == 0) {
        cpu.heap->free(pe);
        if (why) *why = std::string("D3D9 流水线：深度缓冲分配失败：") + m->surfaces().lastError();
        return 0;
    }
    GuestSurface* dgs = m->surfaces().find(depth);
    if (!dgs) {
        m->surfaces().destroy(depth);
        cpu.heap->free(pe);
        if (why) *why = "D3D9 流水线：深度缓冲不在 arena 里";
        return 0;
    }
    std::uint8_t* dp = m->surfaces().pixels(*dgs);
    if (!dp) {
        m->surfaces().destroy(depth);
        cpu.heap->free(pe);
        if (why) *why = "D3D9 流水线：深度缓冲像素指针为空";
        return 0;
    }
    const float one = 1.0f;
    std::uint32_t one_bits = 0;
    std::memcpy(&one_bits, &one, sizeof(one_bits));
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            std::uint8_t* p = dp + static_cast<std::size_t>(y) * dgs->pitch + static_cast<std::size_t>(x) * 4u;
            std::memcpy(p, &one_bits, 4u);
        }
    }
    plSet(cpu, pe, kPlDepthArena, depth);
    plSet(cpu, pe, kPlDevice, device_state);
    wrU32(cpu, device_state + 56u, pe);
    pipeLog("Init",
            "流水线状态块 0x%08X（'DL01'，%u B）· 深度缓冲 0x%08X（%dx%d，float）· 视口 0..%d/0..%d",
            pe, kPlBytes, depth, width, height, width, height);
    return pe;
}

void destroyForDevice(X86& cpu, std::uint32_t device_state) {
    if (device_state == 0 || !cpu.mem || !cpu.mem->committed(device_state, 64u)) return;
    const std::uint32_t pe = pipeEaOf(cpu, device_state);
    if (pe == 0 || !cpu.mem->committed(pe, kPlBytes)) return;
    if (cpu.mem->rd32(pe + kPlMagic) != kPlMagicValue) return;
    // 绑定的纹理在 SetTexture 时 AddRef 过 ⇒ 这里对称释放；渲染目标同理。
    if (cpu.com) {
        for (std::uint32_t s = 0; s < kMaxSamplers; ++s) {
            const std::uint32_t t = plGet(cpu, pe, kPlTextures + s * 4u);
            if (t != 0) cpu.com->release(cpu, t);
        }
        const std::uint32_t rt = plGet(cpu, pe, kPlColorSurface);
        if (rt != 0) cpu.com->release(cpu, rt);
    }
    const std::uint32_t depth = plGet(cpu, pe, kPlDepthArena);
    cpu.mem->wr32(pe + kPlMagic, 0u);
    if (Machine* m = activeMachine(); m && &m->cpu() == &cpu && depth != 0) m->surfaces().destroy(depth);
    if (cpu.heap) cpu.heap->free(pe);
    cpu.mem->wr32(device_state + 56u, 0u);
}

std::uint32_t pipelineOfDevice(X86& cpu, std::uint32_t device_state) {
    std::uint32_t pe = 0;
    if (!pipelineOf(cpu, device_state, &pe, nullptr)) return 0;
    return pe;
}

bool pipelineInfoOfState(X86& cpu, std::uint32_t state, PipelineInfo* out, std::string* why) {
    if (state == 0 || !cpu.mem->committed(state, kPlBytes) || cpu.mem->rd32(state + kPlMagic) != kPlMagicValue) {
        if (why) *why = "D3D9 流水线：不是本模块的流水线状态块";
        return false;
    }
    PipelineInfo i;
    i.state = state;
    i.device = plGet(cpu, state, kPlDevice);
    i.in_scene = plGet(cpu, state, kPlInScene) != 0;
    i.fvf = plGet(cpu, state, kPlFvf);
    i.vertex_decl = plGet(cpu, state, kPlVertexDecl);
    i.depth_arena = plGet(cpu, state, kPlDepthArena);
    i.color_arena = plGet(cpu, state, kPlColorArena);
    i.color_surface = plGet(cpu, state, kPlColorSurface);
    i.begins = plGet(cpu, state, kPlBegins);
    i.ends = plGet(cpu, state, kPlEnds);
    i.draws = plGet(cpu, state, kPlDraws);
    i.draw_calls = plGet(cpu, state, kPlDrawCalls);
    i.stage_snapshots = plGet(cpu, state, kPlStageSnapshots);
    i.triangles = plGet(cpu, state, kPlTriangles);
    i.rasterized = plGet(cpu, state, kPlRasterized);
    i.pixels = (static_cast<std::uint64_t>(plGet(cpu, state, kPlPixels + 4u)) << 32) |
               plGet(cpu, state, kPlPixels);
    i.viewport_x = plGet(cpu, state, kPlViewport + 0u);
    i.viewport_y = plGet(cpu, state, kPlViewport + 4u);
    i.viewport_w = plGet(cpu, state, kPlViewport + 8u);
    i.viewport_h = plGet(cpu, state, kPlViewport + 12u);
    *out = i;
    return true;
}

bool pipelineInfoOfDevice(X86& cpu, std::uint32_t device_state, PipelineInfo* out, std::string* why) {
    std::uint32_t pe = 0;
    if (!pipelineOf(cpu, device_state, &pe, why)) return false;
    return pipelineInfoOfState(cpu, pe, out, why);
}

// =====================================================================================
// 渲染目标（SetRenderTarget）
// =====================================================================================
std::uint32_t renderTargetArena(X86& cpu, std::uint32_t pipeline_state) {
    if (pipeline_state == 0 || !cpu.mem->committed(pipeline_state, kPlBytes) ||
        cpu.mem->rd32(pipeline_state + kPlMagic) != kPlMagicValue) {
        return 0;
    }
    return plGet(cpu, pipeline_state, kPlColorArena);
}

std::uint32_t setRenderTarget(X86& cpu, std::uint32_t dev, std::uint32_t target, std::uint32_t zstencil,
                              std::string* why) {
    std::uint32_t pe = 0;
    if (!pipelineOf(cpu, dev, &pe, why)) return kInvalidCall;
    const std::uint32_t old_surface = plGet(cpu, pe, kPlColorSurface);
    if (target == 0) {
        if (old_surface != 0 && cpu.com) cpu.com->release(cpu, old_surface);
        plSet(cpu, pe, kPlColorArena, 0u);
        plSet(cpu, pe, kPlColorSurface, 0u);
        pipeLog("SetRenderTarget", "目标 = 后缓冲（0）");
        return kOk;
    }
    const char* iface = cpu.com ? cpu.com->ifaceOf(target) : nullptr;
    if (iface == nullptr || std::strcmp(iface, "IDirect3DSurface9") != 0) {
        if (why) *why = "D3D9 流水线：SetRenderTarget 的 pRenderTarget 不是本模块的 IDirect3DSurface9";
        return kInvalidCall;
    }
    const std::uint32_t tex_state = cpu.com->userOf(target);
    TextureInfo ti;
    if (!surfaceInfoOfState(cpu, tex_state, &ti, why)) return kInvalidCall;
    if (zstencil != 0) {
        // 本线只有一个设备深度缓冲；纹理与后缓冲同尺寸时共用它。不同尺寸会错，但不静默。
        pipeLog("SetRenderTarget", "pNewZStencil=0x%08X **被忽略**（本线共用设备深度缓冲；⛔ 不静默）",
                zstencil);
    }
    if (old_surface != target) {
        if (!cpu.com->addRef(cpu, target)) {
            if (why) *why = "D3D9 流水线：SetRenderTarget AddRef 失败";
            return kInvalidCall;
        }
        if (old_surface != 0) cpu.com->release(cpu, old_surface);
        plSet(cpu, pe, kPlColorSurface, target);
        plSet(cpu, pe, kPlColorArena, ti.arena);
    }
    pipeLog("SetRenderTarget", "目标 = 纹理表面 0x%08X（%ux%u，像素块 0x%08X）", target, ti.width,
            ti.height, ti.arena);
    return kOk;
}

// =====================================================================================
// 场景 / 变换 / 渲染状态
// =====================================================================================
std::uint32_t beginScene(X86& cpu, std::uint32_t dev, std::string* why) {
    std::uint32_t pe = 0;
    if (!pipelineOf(cpu, dev, &pe, why)) return kInvalidCall;
    if (plGet(cpu, pe, kPlInScene) != 0) {
        pipeLog("BeginScene", "已经在场景里 ⇒ D3DERR_INVALIDCALL");
        if (why) *why = "D3D9 流水线：BeginScene 嵌套";
        return kInvalidCall;
    }
    plSet(cpu, pe, kPlInScene, 1u);
    plSet(cpu, pe, kPlBegins, plGet(cpu, pe, kPlBegins) + 1u);
    return kOk;
}

std::uint32_t endScene(X86& cpu, std::uint32_t dev, std::string* why) {
    std::uint32_t pe = 0;
    if (!pipelineOf(cpu, dev, &pe, why)) return kInvalidCall;
    if (plGet(cpu, pe, kPlInScene) == 0) {
        pipeLog("EndScene", "不在场景里 ⇒ D3DERR_INVALIDCALL");
        if (why) *why = "D3D9 流水线：EndScene 没有配对的 BeginScene";
        return kInvalidCall;
    }
    plSet(cpu, pe, kPlInScene, 0u);
    plSet(cpu, pe, kPlEnds, plGet(cpu, pe, kPlEnds) + 1u);
    const std::uint64_t pixels = (static_cast<std::uint64_t>(plGet(cpu, pe, kPlPixels + 4u)) << 32) |
                                 plGet(cpu, pe, kPlPixels);
    pipeLog("EndScene", "场景结束（累计 Draw %u / 三角形 %u / 像素 %llu）", plGet(cpu, pe, kPlDraws),
            plGet(cpu, pe, kPlTriangles), static_cast<unsigned long long>(pixels));
    return kOk;
}

std::uint32_t setTransform(X86& cpu, std::uint32_t dev, std::uint32_t state, std::uint32_t matrix_ea,
                           std::string* why) {
    std::uint32_t pe = 0;
    if (!pipelineOf(cpu, dev, &pe, why)) return kInvalidCall;
    if (!validTransformState(state)) {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "SetTransform 的 State=%u 不是合法的 D3DTRANSFORMSTATETYPE", state);
        pipeLog("SetTransform", "%s ⇒ D3DERR_INVALIDCALL", buf);
        if (why) *why = buf;
        return kInvalidCall;
    }
    if (!committed(cpu, matrix_ea, 64u, "SetTransform 的 pMatrix", why)) return kInvalidCall;
    for (unsigned i = 0; i < 16; ++i)
        wrU32(cpu, pe + kPlTransforms + state * 64u + i * 4u, cpu.mem->rd32(matrix_ea + i * 4u));
    return kOk;
}

std::uint32_t getTransform(X86& cpu, std::uint32_t dev, std::uint32_t state, std::uint32_t matrix_ea,
                           std::string* why) {
    std::uint32_t pe = 0;
    if (!pipelineOf(cpu, dev, &pe, why)) return kInvalidCall;
    if (!validTransformState(state)) {
        if (why) *why = "D3D9 流水线：GetTransform 的 State 非法";
        return kInvalidCall;
    }
    if (!committed(cpu, matrix_ea, 64u, "GetTransform 的 pMatrix", why)) return kInvalidCall;
    for (unsigned i = 0; i < 16; ++i)
        wrU32(cpu, matrix_ea + i * 4u, cpu.mem->rd32(pe + kPlTransforms + state * 64u + i * 4u));
    return kOk;
}

std::uint32_t multiplyTransform(X86& cpu, std::uint32_t dev, std::uint32_t state, std::uint32_t matrix_ea,
                                std::string* why) {
    std::uint32_t pe = 0;
    if (!pipelineOf(cpu, dev, &pe, why)) return kInvalidCall;
    if (!validTransformState(state)) {
        if (why) *why = "D3D9 流水线：MultiplyTransform 的 State 非法";
        return kInvalidCall;
    }
    if (!committed(cpu, matrix_ea, 64u, "MultiplyTransform 的 pMatrix", why)) return kInvalidCall;
    // D3DX 语义：state = state * pMatrix（行向量 v' = v*state*pMatrix ⇒ 先 state 后 pMatrix）
    const math::Mat4d cur = readMatrix(cpu, pe + kPlTransforms + state * 64u);
    const math::Mat4d mul = readMatrix(cpu, matrix_ea);
    writeMatrix(cpu, pe + kPlTransforms + state * 64u, math::multiplyD3D(cur, mul));
    return kOk;
}

std::uint32_t setRenderState(X86& cpu, std::uint32_t dev, std::uint32_t state, std::uint32_t value,
                             std::string* why) {
    std::uint32_t pe = 0;
    if (!pipelineOf(cpu, dev, &pe, why)) return kInvalidCall;
    switch (state) {
        case kRsZEnable:
            if (value > 2u) {
                pipeLog("SetRenderState", "ZENABLE=%u 非法（D3DZB_FALSE/TRUE/USEW）⇒ D3DERR_INVALIDCALL", value);
                return kInvalidCall;
            }
            if (value == 2u) recordUnsupported(cpu, pe, 0u, "D3DRS_ZENABLE=D3DZB_USEW（w 缓冲）", value);
            break;
        case kRsFillMode:
            if (value != 1u && value != 2u && value != 3u) {
                pipeLog("SetRenderState", "FILLMODE=%u 非法 ⇒ D3DERR_INVALIDCALL", value);
                return kInvalidCall;
            }
            if (value != 3u) recordUnsupported(cpu, pe, 1u, "D3DFILL_POINT/LINE（只做 SOLID）", value);
            break;
        case kRsShadeMode:
            if (value != 1u && value != 2u) {
                pipeLog("SetRenderState", "SHADEMODE=%u 非法（只认 FLAT/GOURAUD）⇒ D3DERR_INVALIDCALL", value);
                return kInvalidCall;
            }
            break;
        case kRsCullMode:
            if (value < 1u || value > 3u) {
                pipeLog("SetRenderState", "CULLMODE=%u 非法 ⇒ D3DERR_INVALIDCALL", value);
                return kInvalidCall;
            }
            break;
        case kRsZFunc:
        case kRsAlphaFunc:
            if (value < 1u || value > 8u) {
                pipeLog("SetRenderState", "比较函数=%u 非法 ⇒ D3DERR_INVALIDCALL", value);
                return kInvalidCall;
            }
            break;
        case kRsAlphaBlendEnable:
        case kRsAlphaTestEnable:
            if (value > 1u) {
                pipeLog("SetRenderState", "布尔状态=%u 非法 ⇒ D3DERR_INVALIDCALL", value);
                return kInvalidCall;
            }
            break;
        case kRsSrcBlend:
        case kRsDestBlend:
        case kRsSrcBlendAlpha:
        case kRsDestBlendAlpha:
            if (value < 1u || value > 13u) {
                pipeLog("SetRenderState", "D3DBLEND=%u 非法 ⇒ D3DERR_INVALIDCALL", value);
                return kInvalidCall;
            }
            break;
        case kRsBlendOp:
        case kRsBlendOpAlpha:
            if (value < 1u || value > 5u) {
                pipeLog("SetRenderState", "D3DBLENDOP=%u 非法 ⇒ D3DERR_INVALIDCALL", value);
                return kInvalidCall;
            }
            break;
        case kRsFogEnable:
            if (value != 0u) recordUnsupported(cpu, pe, 2u, "D3DRS_FOGENABLE（本线不做雾）", value);
            break;
        case kRsStencilEnable:
            if (value != 0u) recordUnsupported(cpu, pe, 3u, "D3DRS_STENCILENABLE（本线没有模板缓冲）", value);
            break;
        case kRsMultiSampleAntiAlias:
            if (value != 0u) recordUnsupported(cpu, pe, 4u, "D3DRS_MULTISAMPLEANTIALIAS", value);
            break;
        case kRsVertexBlend:
            if (value != 0u) recordUnsupported(cpu, pe, 5u, "D3DRS_VERTEXBLEND（本线不做顶点混合）", value);
            break;
        case kRsIndexedVertexBlendEnable:
            if (value != 0u) recordUnsupported(cpu, pe, 6u, "D3DRS_INDEXEDVERTEXBLENDENABLE", value);
            break;
        case kRsClipPlaneEnable:
            if (value != 0u) recordUnsupported(cpu, pe, 7u, "D3DRS_CLIPPLANEENABLE（本线不做用户裁剪面）", value);
            break;
        case kRsClipping:
            // 本线**总是**做裁剪（齐次 6 面 / 预变换视口矩形）。真机 CLIPPING=0 会关掉落屏外的裁剪，
            // ⛔ 不静默：记录，行为仍是"总是裁剪"（与既有近平面裁剪的口径一致）。
            if (value == 0u) recordUnsupported(cpu, pe, 18u, "D3DRS_CLIPPING=0（本线总是裁剪）", value);
            break;
        case kRsTweenFactor:
            if (value != 0u) recordUnsupported(cpu, pe, 8u, "D3DRS_TWEENFACTOR", value);
            break;
        case kRsDitherEnable:
            if (value != 0u) recordUnsupported(cpu, pe, 9u, "D3DRS_DITHERENABLE", value);
            break;
        default:
            break;
    }
    setRs(cpu, pe, state, value);
    return kOk;
}

std::uint32_t getRenderState(X86& cpu, std::uint32_t dev, std::uint32_t state, std::uint32_t out_ea,
                             std::string* why) {
    std::uint32_t pe = 0;
    if (!pipelineOf(cpu, dev, &pe, why)) return kInvalidCall;
    if (state >= kPlRenderStateCount) {
        if (why) *why = "D3D9 流水线：GetRenderState 的 State 越界";
        return kInvalidCall;
    }
    if (!committed(cpu, out_ea, 4u, "GetRenderState 的 pValue", why)) return kInvalidCall;
    wrU32(cpu, out_ea, rsOf(cpu, pe, state));
    return kOk;
}

std::uint32_t setSamplerState(X86& cpu, std::uint32_t dev, std::uint32_t stage, std::uint32_t type,
                              std::uint32_t value, std::string* why) {
    std::uint32_t pe = 0;
    if (!pipelineOf(cpu, dev, &pe, why)) return kInvalidCall;
    if (stage >= kMaxSamplers) {
        pipeLog("SetSamplerState", "Stage=%u 越界（上限 %u）⇒ D3DERR_INVALIDCALL", stage, kMaxSamplers);
        return kInvalidCall;
    }
    if (type == 0u || type > 13u) {
        pipeLog("SetSamplerState", "Type=%u 不是合法的 D3DSAMPLERSTATETYPE ⇒ D3DERR_INVALIDCALL", type);
        return kInvalidCall;
    }
    switch (type) {
        case kSampAddressU:
        case kSampAddressV:
        case kSampAddressW:
            if (value < 1u || value > 5u) {
                pipeLog("SetSamplerState", "D3DTADDRESS=%u 非法 ⇒ D3DERR_INVALIDCALL", value);
                return kInvalidCall;
            }
            break;
        case kSampMagFilter:
        case kSampMinFilter:
            if (value > 3u) {
                pipeLog("SetSamplerState", "过滤器=%u 非法 ⇒ D3DERR_INVALIDCALL", value);
                return kInvalidCall;
            }
            if (value == kTexfAnisotropic) recordUnsupported(cpu, pe, 10u, "各向异性过滤（按 LINEAR 近似）", value);
            if (value == kTexfNone)
                recordUnsupported(cpu, pe, 17u, "D3DTEXF_NONE（MIN/MAG 无过滤；两侧都 NONE 时按透明黑）", value);
            break;
        case kSampMipFilter:
            if (value != 0u) recordUnsupported(cpu, pe, 11u, "MIPFILTER（本线没有 mip 链）", value);
            break;
        case kSampMaxMipLevel:
        case kSampMaxAnisotropy:
            break;
        default:
            recordUnsupported(cpu, pe, 12u, "其它 sampler state", type);
            break;
    }
    plSet(cpu, pe, kPlSamplers + (stage * 16u + type) * 4u, value);
    return kOk;
}

std::uint32_t getSamplerState(X86& cpu, std::uint32_t dev, std::uint32_t stage, std::uint32_t type,
                              std::uint32_t out_ea, std::string* why) {
    std::uint32_t pe = 0;
    if (!pipelineOf(cpu, dev, &pe, why)) return kInvalidCall;
    if (stage >= kMaxSamplers || type == 0u || type > 13u) {
        if (why) *why = "D3D9 流水线：GetSamplerState 的 Stage/Type 非法";
        return kInvalidCall;
    }
    if (!committed(cpu, out_ea, 4u, "GetSamplerState 的 pValue", why)) return kInvalidCall;
    wrU32(cpu, out_ea, samplerOf(cpu, pe, stage, type));
    return kOk;
}

std::uint32_t setTextureStageState(X86& cpu, std::uint32_t dev, std::uint32_t stage, std::uint32_t type,
                                   std::uint32_t value, std::string* why) {
    std::uint32_t pe = 0;
    if (!pipelineOf(cpu, dev, &pe, why)) return kInvalidCall;
    if (stage >= kMaxTextureStages) {
        pipeLog("SetTextureStageState", "Stage=%u 越界（上限 %u）⇒ D3DERR_INVALIDCALL", stage, kMaxTextureStages);
        return kInvalidCall;
    }
    if (type == 0u || type > kPlTexStageTypes - 1u) {
        pipeLog("SetTextureStageState", "Type=%u 越界 ⇒ D3DERR_INVALIDCALL", type);
        return kInvalidCall;
    }
    if (type == kTssColorOp || type == kTssAlphaOp) {
        if (value == 0u || value > 26u) {
            pipeLog("SetTextureStageState", "D3DTOP=%u 非法 ⇒ D3DERR_INVALIDCALL", value);
            return kInvalidCall;
        }
        if (value > kTopMultiplyAdd && value != kTopLerp && value != kTopModulateAlphaAddColor &&
            value != kTopModulateColorAddAlpha) {
            recordUnsupported(cpu, pe, 13u, "D3DTOP", value);
        }
    }
    if (type == kTssTextureTransformFlags && value != 0u) {
        recordUnsupported(cpu, pe, 14u, "D3DTSS_TEXTURETRANSFORMFLAGS（本线不做纹理坐标变换）", value);
    }
    if (type == kTssTexCoordIndex && (value & 0x10000u) != 0u) {
        recordUnsupported(cpu, pe, 15u, "D3DTSS_TEXCOORDINDEX 的相机空间生成标志", value);
    }
    plSet(cpu, pe, kPlTexStages + (stage * kPlTexStageTypes + type) * 4u, value);
    return kOk;
}

std::uint32_t getTextureStageState(X86& cpu, std::uint32_t dev, std::uint32_t stage, std::uint32_t type,
                                   std::uint32_t out_ea, std::string* why) {
    std::uint32_t pe = 0;
    if (!pipelineOf(cpu, dev, &pe, why)) return kInvalidCall;
    if (stage >= kMaxTextureStages || type == 0u || type >= kPlTexStageTypes) {
        if (why) *why = "D3D9 流水线：GetTextureStageState 的 Stage/Type 非法";
        return kInvalidCall;
    }
    if (!committed(cpu, out_ea, 4u, "GetTextureStageState 的 pValue", why)) return kInvalidCall;
    wrU32(cpu, out_ea, texStageOf(cpu, pe, stage, type));
    return kOk;
}

std::uint32_t setTexture(X86& cpu, std::uint32_t dev, std::uint32_t stage, std::uint32_t texture,
                         std::string* why) {
    std::uint32_t pe = 0;
    if (!pipelineOf(cpu, dev, &pe, why)) return kInvalidCall;
    if (stage >= kMaxSamplers) {
        pipeLog("SetTexture", "Stage=%u 越界（上限 %u）⇒ D3DERR_INVALIDCALL", stage, kMaxSamplers);
        return kInvalidCall;
    }
    if (texture != 0) {
        TextureInfo ti;
        if (!textureInfo(cpu, texture, &ti, why)) {
            pipeLog("SetTexture", "0x%08X 不是本模块建的 IDirect3DTexture9 ⇒ D3DERR_INVALIDCALL", texture);
            if (why) *why = "D3D9 流水线：SetTexture 的纹理不是本模块建的";
            return kInvalidCall;
        }
        if (!cpu.com || !cpu.com->addRef(cpu, texture)) {
            pipeLog("SetTexture", "纹理 0x%08X 的 AddRef 失败（COM 状态坏了）", texture);
            return kInvalidCall;
        }
    }
    const std::uint32_t old = plGet(cpu, pe, kPlTextures + stage * 4u);
    plSet(cpu, pe, kPlTextures + stage * 4u, texture);
    if (old != 0 && cpu.com) cpu.com->release(cpu, old);
    if ((stage == 0 || texture != 0) && old != texture) {
        char buf[160];
        std::snprintf(buf, sizeof(buf), "Stage=%u ⇒ 纹理 0x%08X（原 0x%08X）", stage, texture, old);
        pipeLog("SetTexture", "%s", buf);
    }
    return kOk;
}

std::uint32_t getTexture(X86& cpu, std::uint32_t dev, std::uint32_t stage, std::uint32_t out_ea,
                         std::string* why) {
    std::uint32_t pe = 0;
    if (!pipelineOf(cpu, dev, &pe, why)) return kInvalidCall;
    if (stage >= kMaxSamplers) return kInvalidCall;
    if (!committed(cpu, out_ea, 4u, "GetTexture 的 ppTexture", why)) return kInvalidCall;
    wrU32(cpu, out_ea, plGet(cpu, pe, kPlTextures + stage * 4u));
    return kOk;
}

std::uint32_t setFVF(X86& cpu, std::uint32_t dev, std::uint32_t fvf, std::string* why) {
    std::uint32_t pe = 0;
    if (!pipelineOf(cpu, dev, &pe, why)) return kInvalidCall;
    if (fvf == 0u) {
        // 真机：SetFVF(0) 合法（= 当前没有 FVF）；之后 Draw 会因缺少顶点格式失败。
        plSet(cpu, pe, kPlFvf, 0u);
        plSet(cpu, pe, kPlVertexDecl, 0u);
        pipeLog("SetFVF", "FVF=0 ⇒ 当前没有 FVF（合法的清除；之后 Draw 会 INVALIDCALL）");
        return kOk;
    }
    std::string parse_why;
    const FvfLayout l = parseFvf(fvf, &parse_why);
    if (!l.valid) {
        pipeLog("SetFVF", "%s ⇒ D3DERR_INVALIDCALL", parse_why.c_str());
        if (why) *why = parse_why;
        return kInvalidCall;
    }
    plSet(cpu, pe, kPlFvf, fvf);
    plSet(cpu, pe, kPlVertexDecl, 0u);  // 用 FVF 就清掉顶点声明（D3D 也是二选一）
    return kOk;
}

std::uint32_t getFVF(X86& cpu, std::uint32_t dev, std::uint32_t out_ea, std::string* why) {
    std::uint32_t pe = 0;
    if (!pipelineOf(cpu, dev, &pe, why)) return kInvalidCall;
    if (!committed(cpu, out_ea, 4u, "GetFVF 的 pFVF", why)) return kInvalidCall;
    wrU32(cpu, out_ea, plGet(cpu, pe, kPlFvf));
    return kOk;
}

std::uint32_t setVertexDeclaration(X86& cpu, std::uint32_t dev, std::uint32_t decl, std::string* why) {
    std::uint32_t pe = 0;
    if (!pipelineOf(cpu, dev, &pe, why)) return kInvalidCall;
    if (decl != 0) {
        // 只接受**本模块建的** IDirect3DVertexDeclaration9。用声明代替 FVF 是合法路径；本线的
        // Draw 仍只认 FVF（`drawPrecheck` 会 D3DERR_INVALIDCALL），但 Set 本身不该拦。
        const char* iface = cpu.com ? cpu.com->ifaceOf(decl) : nullptr;
        if (!cpu.com || !cpu.com->find(decl) || !iface ||
            std::strcmp(iface, "IDirect3DVertexDeclaration9") != 0) {
            pipeLog("SetVertexDeclaration", "pDecl=0x%08X 不是本模块的 IDirect3DVertexDeclaration9 ⇒ D3DERR_INVALIDCALL",
                    decl);
            if (why) *why = "D3D9 流水线：SetVertexDeclaration 收到非顶点声明对象";
            return kInvalidCall;
        }
        plSet(cpu, pe, kPlVertexDecl, decl);
        plSet(cpu, pe, kPlFvf, 0u);  // 用声明就清掉 FVF（D3D 二选一）
        return kOk;
    }
    plSet(cpu, pe, kPlVertexDecl, 0u);
    return kOk;
}

std::uint32_t getVertexDeclaration(X86& cpu, std::uint32_t dev, std::uint32_t out_ea, std::string* why) {
    std::uint32_t pe = 0;
    if (!pipelineOf(cpu, dev, &pe, why)) return kInvalidCall;
    if (!committed(cpu, out_ea, 4u, "GetVertexDeclaration 的 ppDecl", why)) return kInvalidCall;
    wrU32(cpu, out_ea, plGet(cpu, pe, kPlVertexDecl));
    return kOk;
}

std::uint32_t setViewport(X86& cpu, std::uint32_t dev, std::uint32_t viewport_ea, std::string* why) {
    std::uint32_t pe = 0;
    if (!pipelineOf(cpu, dev, &pe, why)) return kInvalidCall;
    if (!committed(cpu, viewport_ea, 24u, "SetViewport 的 pViewport", why)) return kInvalidCall;
    const std::uint32_t w = cpu.mem->rd32(viewport_ea + 8u);
    const std::uint32_t h = cpu.mem->rd32(viewport_ea + 12u);
    if (w == 0 || h == 0) {
        pipeLog("SetViewport", "Width/Height 为 0 ⇒ D3DERR_INVALIDCALL");
        return kInvalidCall;
    }
    for (unsigned i = 0; i < 4; ++i)
        plSet(cpu, pe, kPlViewport + i * 4u, cpu.mem->rd32(viewport_ea + i * 4u));
    for (unsigned i = 4; i < 6; ++i)
        wrU32(cpu, pe + kPlViewport + i * 4u, cpu.mem->rd32(viewport_ea + i * 4u));
    return kOk;
}

std::uint32_t getViewport(X86& cpu, std::uint32_t dev, std::uint32_t viewport_ea, std::string* why) {
    std::uint32_t pe = 0;
    if (!pipelineOf(cpu, dev, &pe, why)) return kInvalidCall;
    if (!committed(cpu, viewport_ea, 24u, "GetViewport 的 pViewport", why)) return kInvalidCall;
    for (unsigned i = 0; i < 6; ++i)
        wrU32(cpu, viewport_ea + i * 4u, cpu.mem->rd32(pe + kPlViewport + i * 4u));
    return kOk;
}

std::uint32_t setScissorRect(X86& cpu, std::uint32_t dev, std::uint32_t rect_ea, std::string* why) {
    std::uint32_t pe = 0;
    if (!pipelineOf(cpu, dev, &pe, why)) return kInvalidCall;
    if (!committed(cpu, rect_ea, 16u, "SetScissorRect 的 pRect", why)) return kInvalidCall;
    const std::int32_t left = static_cast<std::int32_t>(cpu.mem->rd32(rect_ea + 0u));
    const std::int32_t top = static_cast<std::int32_t>(cpu.mem->rd32(rect_ea + 4u));
    const std::int32_t right = static_cast<std::int32_t>(cpu.mem->rd32(rect_ea + 8u));
    const std::int32_t bottom = static_cast<std::int32_t>(cpu.mem->rd32(rect_ea + 12u));
    if (left < 0 || top < 0 || right < left || bottom < top) {
        pipeLog("SetScissorRect", "矩形 (%d,%d,%d,%d) 非法 ⇒ D3DERR_INVALIDCALL", left, top, right, bottom);
        return kInvalidCall;
    }
    for (unsigned i = 0; i < 4; ++i) plSet(cpu, pe, kPlScissor + i * 4u, cpu.mem->rd32(rect_ea + i * 4u));
    return kOk;
}

std::uint32_t getScissorRect(X86& cpu, std::uint32_t dev, std::uint32_t rect_ea, std::string* why) {
    std::uint32_t pe = 0;
    if (!pipelineOf(cpu, dev, &pe, why)) return kInvalidCall;
    if (!committed(cpu, rect_ea, 16u, "GetScissorRect 的 pRect", why)) return kInvalidCall;
    for (unsigned i = 0; i < 4; ++i) wrU32(cpu, rect_ea + i * 4u, cpu.mem->rd32(pe + kPlScissor + i * 4u));
    return kOk;
}

std::uint32_t setMaterial(X86& cpu, std::uint32_t dev, std::uint32_t material_ea, std::string* why) {
    std::uint32_t pe = 0;
    if (!pipelineOf(cpu, dev, &pe, why)) return kInvalidCall;
    if (!committed(cpu, material_ea, 68u, "SetMaterial 的 pMaterial", why)) return kInvalidCall;
    for (unsigned i = 0; i < 68u; ++i) cpu.mem->wr8(pe + kPlMaterial + i, cpu.mem->rd8(material_ea + i));
    return kOk;
}

std::uint32_t setLight(X86& cpu, std::uint32_t dev, std::uint32_t index, std::uint32_t light_ea,
                       std::string* why) {
    std::uint32_t pe = 0;
    if (!pipelineOf(cpu, dev, &pe, why)) return kInvalidCall;
    if (index >= kMaxLights) {
        pipeLog("SetLight", "Index=%u 越界（上限 %u）⇒ D3DERR_INVALIDCALL", index, kMaxLights);
        return kInvalidCall;
    }
    if (!committed(cpu, light_ea, 104u, "SetLight 的 pLight", why)) return kInvalidCall;
    for (unsigned i = 0; i < 104u; ++i) cpu.mem->wr8(pe + kPlLights + index * 104u + i, cpu.mem->rd8(light_ea + i));
    return kOk;
}

std::uint32_t lightEnable(X86& cpu, std::uint32_t dev, std::uint32_t index, std::uint32_t enable,
                          std::string* why) {
    std::uint32_t pe = 0;
    if (!pipelineOf(cpu, dev, &pe, why)) return kInvalidCall;
    if (index >= kMaxLights) {
        pipeLog("LightEnable", "Index=%u 越界（上限 %u）⇒ D3DERR_INVALIDCALL", index, kMaxLights);
        return kInvalidCall;
    }
    plSet(cpu, pe, kPlLightEnabled + index * 4u, enable != 0 ? 1u : 0u);
    return kOk;
}

std::uint32_t validateDevice(X86& cpu, std::uint32_t dev, std::uint32_t passes_ea, std::string* why) {
    std::uint32_t pe = 0;
    if (!pipelineOf(cpu, dev, &pe, why)) return kInvalidCall;
    if (!committed(cpu, passes_ea, 4u, "ValidateDevice 的 pNumPasses", why)) return kInvalidCall;
    wrU32(cpu, passes_ea, 1u);  // 本线的状态组合一律单 pass
    return kOk;
}

// =====================================================================================
// 顶点缓冲 / 索引缓冲
// =====================================================================================
std::uint32_t createVertexBufferState(X86& cpu, std::uint32_t device_object, std::uint32_t length,
                                      std::uint32_t usage, std::uint32_t fvf, std::uint32_t pool,
                                      std::string* why) {
    if (!cpu.heap || !cpu.heap->ready()) {
        if (why) *why = "D3D9 流水线：guest 堆未就绪（顶点缓冲要放在 guest 堆上）";
        return 0;
    }
    if (length == 0u || length > kMaxBufferBytes) {
        if (why) *why = "D3D9 流水线：创建顶点缓冲的长度非法";
        return 0;
    }
    const std::uint32_t total = kVbHeader + length;
    const std::uint32_t state = cpu.heap->alloc(total, /*zero=*/true);
    if (state == 0) {
        if (why) *why = std::string("D3D9 流水线：分配顶点缓冲失败：") + cpu.heap->lastError();
        return 0;
    }
    wrU32(cpu, state + kVbMagic, kVbMagicValue);
    wrU32(cpu, state + kVbDevice, device_object);
    wrU32(cpu, state + kVbLength, length);
    wrU32(cpu, state + kVbUsage, usage);
    wrU32(cpu, state + kVbFvf, fvf);
    wrU32(cpu, state + kVbPool, pool);
    pipeLog("CreateVertexBuffer", "%u B（Usage=0x%X FVF=0x%X Pool=%u）⇒ 状态块 0x%08X", length, usage, fvf, pool,
            state);
    return state;
}

bool vertexBufferInfoOfState(X86& cpu, std::uint32_t state, std::uint32_t* length, std::uint32_t* data_ea,
                             std::uint32_t* fvf, std::string* why) {
    if (state == 0 || !cpu.mem->committed(state, kVbHeader)) {
        if (why) *why = "D3D9 流水线：顶点缓冲状态块未提交";
        return false;
    }
    if (cpu.mem->rd32(state + kVbMagic) != kVbMagicValue) {
        if (why) *why = "D3D9 流水线：不是本模块建的顶点缓冲";
        return false;
    }
    if (length) *length = cpu.mem->rd32(state + kVbLength);
    if (data_ea) *data_ea = state + kVbHeader;
    if (fvf) *fvf = cpu.mem->rd32(state + kVbFvf);
    return true;
}

std::uint32_t vertexBufferDevice(X86& cpu, std::uint32_t state) {
    if (state == 0 || !cpu.mem->committed(state, kVbHeader)) return 0;
    if (cpu.mem->rd32(state + kVbMagic) != kVbMagicValue) return 0;
    return cpu.mem->rd32(state + kVbDevice);
}

void destroyVertexBufferState(X86& cpu, std::uint32_t state) {
    if (state == 0 || !cpu.mem || !cpu.mem->committed(state, kVbHeader)) return;
    if (cpu.mem->rd32(state + kVbMagic) != kVbMagicValue) return;
    cpu.mem->wr32(state + kVbMagic, 0u);
    if (cpu.heap) cpu.heap->free(state);
}

std::uint32_t vertexBufferLock(X86& cpu, std::uint32_t state, std::uint32_t offset, std::uint32_t size,
                               std::uint32_t out_ea, std::uint32_t flags, std::string* why) {
    std::uint32_t length = 0;
    std::uint32_t data = 0;
    if (!vertexBufferInfoOfState(cpu, state, &length, &data, nullptr, why)) return kInvalidCall;
    if (cpu.mem->rd32(state + kVbLocked) != 0) {
        if (why) *why = "D3D9 流水线：顶点缓冲已锁（真机也是 D3DERR_INVALIDCALL）";
        return kInvalidCall;
    }
    if (!committed(cpu, out_ea, 4u, "顶点缓冲 Lock 的 ppbData", why)) return kInvalidCall;
    if (offset > length) {
        if (why) *why = "D3D9 流水线：Lock 的 OffsetToLock 越界";
        return kInvalidCall;
    }
    const std::uint32_t avail = length - offset;
    const std::uint32_t n = (size == 0u || size > avail) ? avail : size;
    wrU32(cpu, out_ea, data + offset);
    wrU32(cpu, state + kVbLocked, 1u);
    if (flags != 0u) {
        // DISCARD/NOOVERWRITE 对 guest 内存里的顶点缓冲没有可观察差异 ⇒ 只记录（不静默）
        pipeLog("VertexBuffer::Lock", "Flags=0x%X 被记录（guest 内存没有 DISCARD/NOOVERWRITE 语义）", flags);
    }
    (void)n;
    return kOk;
}

std::uint32_t vertexBufferUnlock(X86& cpu, std::uint32_t state, std::string* why) {
    std::uint32_t length = 0;
    if (!vertexBufferInfoOfState(cpu, state, &length, nullptr, nullptr, why)) return kInvalidCall;
    if (cpu.mem->rd32(state + kVbLocked) == 0) {
        if (why) *why = "D3D9 流水线：Unlock 未锁的顶点缓冲（真机也是 D3DERR_INVALIDCALL）";
        return kInvalidCall;
    }
    wrU32(cpu, state + kVbLocked, 0u);
    return kOk;
}

std::uint32_t createIndexBufferState(X86& cpu, std::uint32_t device_object, std::uint32_t length,
                                     std::uint32_t usage, std::uint32_t format, std::uint32_t pool,
                                     std::string* why) {
    if (!cpu.heap || !cpu.heap->ready()) {
        if (why) *why = "D3D9 流水线：guest 堆未就绪";
        return 0;
    }
    if (length == 0u || length > kMaxBufferBytes) {
        if (why) *why = "D3D9 流水线：创建索引缓冲的长度非法";
        return 0;
    }
    if (format != 101u && format != 102u) {
        if (why) *why = "D3D9 流水线：索引缓冲格式只支持 D3DFMT_INDEX16(101) / INDEX32(102)";
        return 0;
    }
    const std::uint32_t total = kIbHeader + length;
    const std::uint32_t state = cpu.heap->alloc(total, /*zero=*/true);
    if (state == 0) {
        if (why) *why = std::string("D3D9 流水线：分配索引缓冲失败：") + cpu.heap->lastError();
        return 0;
    }
    wrU32(cpu, state + kIbMagic, kIbMagicValue);
    wrU32(cpu, state + kIbDevice, device_object);
    wrU32(cpu, state + kIbLength, length);
    wrU32(cpu, state + kIbUsage, usage);
    wrU32(cpu, state + kIbFormat, format);
    wrU32(cpu, state + kIbPool, pool);
    pipeLog("CreateIndexBuffer", "%u B（Usage=0x%X Format=%u Pool=%u）⇒ 状态块 0x%08X", length, usage, format,
            pool, state);
    return state;
}

bool indexBufferInfoOfState(X86& cpu, std::uint32_t state, std::uint32_t* length, std::uint32_t* data_ea,
                            std::uint32_t* format, std::string* why) {
    if (state == 0 || !cpu.mem->committed(state, kIbHeader)) {
        if (why) *why = "D3D9 流水线：索引缓冲状态块未提交";
        return false;
    }
    if (cpu.mem->rd32(state + kIbMagic) != kIbMagicValue) {
        if (why) *why = "D3D9 流水线：不是本模块建的索引缓冲";
        return false;
    }
    if (length) *length = cpu.mem->rd32(state + kIbLength);
    if (data_ea) *data_ea = state + kIbHeader;
    if (format) *format = cpu.mem->rd32(state + kIbFormat);
    return true;
}

std::uint32_t indexBufferDevice(X86& cpu, std::uint32_t state) {
    if (state == 0 || !cpu.mem->committed(state, kIbHeader)) return 0;
    if (cpu.mem->rd32(state + kIbMagic) != kIbMagicValue) return 0;
    return cpu.mem->rd32(state + kIbDevice);
}

void destroyIndexBufferState(X86& cpu, std::uint32_t state) {
    if (state == 0 || !cpu.mem || !cpu.mem->committed(state, kIbHeader)) return;
    if (cpu.mem->rd32(state + kIbMagic) != kIbMagicValue) return;
    cpu.mem->wr32(state + kIbMagic, 0u);
    if (cpu.heap) cpu.heap->free(state);
}

std::uint32_t indexBufferLock(X86& cpu, std::uint32_t state, std::uint32_t offset, std::uint32_t size,
                              std::uint32_t out_ea, std::uint32_t flags, std::string* why) {
    std::uint32_t length = 0;
    std::uint32_t data = 0;
    if (!indexBufferInfoOfState(cpu, state, &length, &data, nullptr, why)) return kInvalidCall;
    if (cpu.mem->rd32(state + kIbLocked) != 0) {
        if (why) *why = "D3D9 流水线：索引缓冲已锁";
        return kInvalidCall;
    }
    if (!committed(cpu, out_ea, 4u, "索引缓冲 Lock 的 ppbData", why)) return kInvalidCall;
    if (offset > length) {
        if (why) *why = "D3D9 流水线：索引缓冲 Lock 的 OffsetToLock 越界";
        return kInvalidCall;
    }
    wrU32(cpu, out_ea, data + offset);
    wrU32(cpu, state + kIbLocked, 1u);
    (void)size;
    (void)flags;
    return kOk;
}

std::uint32_t indexBufferUnlock(X86& cpu, std::uint32_t state, std::string* why) {
    std::uint32_t length = 0;
    if (!indexBufferInfoOfState(cpu, state, &length, nullptr, nullptr, why)) return kInvalidCall;
    if (cpu.mem->rd32(state + kIbLocked) == 0) {
        if (why) *why = "D3D9 流水线：Unlock 未锁的索引缓冲";
        return kInvalidCall;
    }
    wrU32(cpu, state + kIbLocked, 0u);
    return kOk;
}

std::uint32_t setStreamSource(X86& cpu, std::uint32_t dev, std::uint32_t stream, std::uint32_t vb,
                              std::uint32_t offset, std::uint32_t stride, std::string* why) {
    std::uint32_t pe = 0;
    if (!pipelineOf(cpu, dev, &pe, why)) return kInvalidCall;
    if (stream >= kMaxStreams) {
        pipeLog("SetStreamSource", "Stream=%u 越界（上限 %u）⇒ D3DERR_INVALIDCALL", stream, kMaxStreams);
        return kInvalidCall;
    }
    if (vb != 0) {
        // ★ 入参是 guest 可见的 **IDirect3DVertexBuffer9 对象**；流水线内部存的是它的状态块。
        //   旧实现把对象 EA 直接交给 `vertexBufferInfoOfState` ⇒ 每个真实 SetStreamSource 都失败，
        //   Draw 于是永远报“没有 SetStreamSource(0)”（boot 日志可见）。这里只做一次对象 → 状态换算。
        if (!cpu.com) {
            if (why) *why = "D3D9 流水线：SetStreamSource 需要 cpu.com 来解析顶点缓冲对象";
            return kInvalidCall;
        }
        const std::uint32_t vb_state = cpu.com->userOf(vb);
        std::uint32_t length = 0;
        if (vb_state == 0 ||
            !vertexBufferInfoOfState(cpu, vb_state, &length, nullptr, nullptr, why)) {
            pipeLog("SetStreamSource", "0x%08X 不是本模块建的顶点缓冲 ⇒ D3DERR_INVALIDCALL", vb);
            return kInvalidCall;
        }
        if (offset > length) {
            pipeLog("SetStreamSource", "OffsetInBytes=%u 超过缓冲长度 %u ⇒ D3DERR_INVALIDCALL", offset, length);
            return kInvalidCall;
        }
        vb = vb_state;  // 后续只存状态块（Draw 用同一个口径）
    }
    plSet(cpu, pe, kPlStreams + stream * 16u + 0u, vb);
    plSet(cpu, pe, kPlStreams + stream * 16u + 4u, offset);
    plSet(cpu, pe, kPlStreams + stream * 16u + 8u, stride);
    return kOk;
}

std::uint32_t setIndices(X86& cpu, std::uint32_t dev, std::uint32_t ib, std::string* why) {
    std::uint32_t pe = 0;
    if (!pipelineOf(cpu, dev, &pe, why)) return kInvalidCall;
    if (ib != 0) {
        // 同 SetStreamSource：入参是对象 EA，流水线内部存状态块 EA。
        if (!cpu.com) {
            if (why) *why = "D3D9 流水线：SetIndices 需要 cpu.com 来解析索引缓冲对象";
            return kInvalidCall;
        }
        const std::uint32_t ib_state = cpu.com->userOf(ib);
        std::uint32_t length = 0;
        if (ib_state == 0 ||
            !indexBufferInfoOfState(cpu, ib_state, &length, nullptr, nullptr, why)) {
            pipeLog("SetIndices", "0x%08X 不是本模块建的索引缓冲 ⇒ D3DERR_INVALIDCALL", ib);
            return kInvalidCall;
        }
        ib = ib_state;
    }
    plSet(cpu, pe, kPlIndices, ib);
    return kOk;
}

// =====================================================================================
// Draw：顶点装配 → T&L → 裁剪 → 光栅
// =====================================================================================
namespace {

// 把一次 Draw 的顶点喂进光栅；返回 false ⇒ 硬失败（why 已填）
bool submitPrimitives(X86& cpu, std::uint32_t pe, std::uint32_t prim, const std::vector<CVertex>& verts,
                      std::uint32_t prim_count, DrawContext& ctx, std::uint32_t* triangles_out,
                      std::uint32_t* rasterized_out, std::string* why);
bool prepareContext(X86& cpu, std::uint32_t dev, std::uint32_t pe, DrawContext* ctx, std::string* why) {
    Machine* m = nullptr;
    if (!requireMachine(cpu, &m, why)) return false;
    DeviceInfo dev_info;
    if (!deviceInfoOfState(cpu, dev, &dev_info, why)) return false;
    const std::uint32_t color_arena = plGet(cpu, pe, kPlColorArena);
    GuestSurface* gs = m->surfaces().find(color_arena != 0 ? color_arena : dev_info.arena);
    if (!gs) {
        if (why) *why = color_arena != 0 ? "D3D9 流水线：渲染目标像素不在 arena 里"
                                         : "D3D9 流水线：后缓冲像素不在 arena 里";
        return false;
    }
    const std::uint32_t depth = plGet(cpu, pe, kPlDepthArena);
    GuestSurface* dgs = m->surfaces().find(depth);
    if (!dgs) {
        if (why) *why = "D3D9 流水线：深度缓冲不在 arena 里";
        return false;
    }
    ctx->cpu = &cpu;
    ctx->machine = m;
    ctx->gpu = m->gpuRaster();
    ctx->pe = pe;
    ctx->color_base = m->surfaces().pixels(*gs);
    ctx->color_fmt = gs->format;
    ctx->color_width = gs->width;
    ctx->color_height = gs->height;
    ctx->color_pitch = gs->pitch;
    ctx->color_surface = plGet(cpu, pe, kPlColorSurface);
    ctx->depth_base = reinterpret_cast<float*>(m->surfaces().pixels(*dgs));
    ctx->depth_pitch_floats = dgs->pitch / 4u;
    if (!ctx->color_base || !ctx->depth_base) {
        if (why) *why = "D3D9 流水线：后缓冲 / 深度缓冲像素指针为空";
        return false;
    }
    // 视口
    ctx->viewport_x = static_cast<int>(plGet(cpu, pe, kPlViewport + 0u));
    ctx->viewport_y = static_cast<int>(plGet(cpu, pe, kPlViewport + 4u));
    ctx->viewport_w = static_cast<int>(plGet(cpu, pe, kPlViewport + 8u));
    ctx->viewport_h = static_cast<int>(plGet(cpu, pe, kPlViewport + 12u));
    // 裁剪矩形（ScissorTest 关 ⇒ 整个后缓冲）
    int sx0 = 0;
    int sy0 = 0;
    int sx1 = gs->width;
    int sy1 = gs->height;
    if (rsOf(cpu, pe, kRsScissorTestEnable) != 0u) {
        sx0 = static_cast<int>(plGet(cpu, pe, kPlScissor + 0u));
        sy0 = static_cast<int>(plGet(cpu, pe, kPlScissor + 4u));
        sx1 = static_cast<int>(plGet(cpu, pe, kPlScissor + 8u));
        sy1 = static_cast<int>(plGet(cpu, pe, kPlScissor + 12u));
        if (sx0 < 0) sx0 = 0;
        if (sy0 < 0) sy0 = 0;
        if (sx1 > gs->width) sx1 = gs->width;
        if (sy1 > gs->height) sy1 = gs->height;
    }
    ctx->scissor_x0 = sx0;
    ctx->scissor_y0 = sy0;
    ctx->scissor_x1 = sx1;
    ctx->scissor_y1 = sy1;
    // 状态快照
    ctx->z_enable = rsOf(cpu, pe, kRsZEnable) != 0u;
    ctx->z_write = rsOf(cpu, pe, kRsZWriteEnable) != 0u;
    ctx->z_func = rsOf(cpu, pe, kRsZFunc);
    ctx->cull = rsOf(cpu, pe, kRsCullMode);
    ctx->shade = rsOf(cpu, pe, kRsShadeMode);
    ctx->alpha_test = rsOf(cpu, pe, kRsAlphaTestEnable) != 0u;
    ctx->alpha_func = rsOf(cpu, pe, kRsAlphaFunc);
    ctx->alpha_ref = rsOf(cpu, pe, kRsAlphaRef);
    ctx->blend = rsOf(cpu, pe, kRsAlphaBlendEnable) != 0u;
    ctx->src_blend = rsOf(cpu, pe, kRsSrcBlend);
    ctx->dst_blend = rsOf(cpu, pe, kRsDestBlend);
    ctx->blend_op = rsOf(cpu, pe, kRsBlendOp);
    ctx->separate_alpha_blend = rsOf(cpu, pe, kRsSeparateAlphaBlendEnable) != 0u;
    ctx->src_blend_a = rsOf(cpu, pe, kRsSrcBlendAlpha);
    ctx->dst_blend_a = rsOf(cpu, pe, kRsDestBlendAlpha);
    ctx->blend_op_a = rsOf(cpu, pe, kRsBlendOpAlpha);
    ctx->color_write = rsOf(cpu, pe, kRsColorWriteEnable);
    ctx->texture_factor = rsOf(cpu, pe, kRsTextureFactor);
    { static int serial = 0; ctx->pixels_written = 0; ctx->serial = ++serial; }
    for (std::uint32_t s = 0; s < kMaxTextureStages; ++s) {
        std::string terr;
        const std::uint32_t tex = plGet(cpu, pe, kPlTextures + s * 4u);
        if (!resolveTexture(cpu, m, tex, &ctx->textures[s], &terr)) {
            char buf[256];
            std::snprintf(buf, sizeof(buf), "D3D9 流水线：纹理级 %u 的纹理无效：%s", s, terr.c_str());
            if (why) *why = buf;
            return false;
        }
    }
    // ★ 偏差 #4 的消除点：纹理级 / 采样器状态在这里**读一遍**（guest 在 Draw 期间不执行，
    //   所以这份快照就是整个光栅阶段看到的全部状态）。像素循环只读 ctx->stages / ctx->samplers。
    //   可观察量：kPlStageSnapshots 每次 Draw 只 +1（⛔ 不随三角形/像素增长）。
    for (std::uint32_t s = 0; s < kMaxTextureStages; ++s) {
        TextureStageSnapshot& ts = ctx->stages[s];
        ts.color_op = texStageOf(cpu, pe, s, kTssColorOp);
        ts.alpha_op = texStageOf(cpu, pe, s, kTssAlphaOp);
        ts.color_arg0 = texStageOf(cpu, pe, s, kTssColorArg0);
        ts.color_arg1 = texStageOf(cpu, pe, s, kTssColorArg1);
        ts.color_arg2 = texStageOf(cpu, pe, s, kTssColorArg2);
        ts.alpha_arg1 = texStageOf(cpu, pe, s, kTssAlphaArg1);
        ts.alpha_arg2 = texStageOf(cpu, pe, s, kTssAlphaArg2);
        ts.constant = texStageOf(cpu, pe, s, kTssConstant);
        ts.tex_coord_index = texStageOf(cpu, pe, s, kTssTexCoordIndex);
        SamplerSnapshot& ss = ctx->samplers[s];
        ss.address_u = samplerOf(cpu, pe, s, kSampAddressU);
        ss.address_v = samplerOf(cpu, pe, s, kSampAddressV);
        ss.mag_filter = samplerOf(cpu, pe, s, kSampMagFilter);
        ss.min_filter = samplerOf(cpu, pe, s, kSampMinFilter);
    }
    plSet(cpu, pe, kPlStageSnapshots, plGet(cpu, pe, kPlStageSnapshots) + 1u);
    return true;
}

// 顶点变换（XYZ 路：WORLD → VIEW → PROJECTION；XYZRHW 路：已经是屏幕坐标）
void transformVertex(X86& cpu, std::uint32_t pe, const FvfLayout& fvf, const std::uint8_t* base,
                     std::uint32_t vertex_index, const math::Mat4d& world, const math::Mat4d& view,
                     const math::Mat4d& proj, CVertex* out) {
    readVertex(cpu, fvf, base, 0, out, nullptr);
    if (out->pretransformed) return;
    // 光照（先做：需要世界空间位置与法线）
    if (rsOf(cpu, pe, kRsLighting) != 0u && fvf.has_normal) {
        const math::Vec3d local(out->x, out->y, out->z);
        const math::Vec3d wpos = math::transformPoint3D3D(world, local);
        const std::uint8_t* np = base + fvf.normal_offset;
        float nf[3];
        std::memcpy(nf, np, sizeof(nf));
        const math::Vec3d localn(static_cast<double>(nf[0]), static_cast<double>(nf[1]),
                                 static_cast<double>(nf[2]));
        // ⚠ 近似：真机用 world 的逆转置（非等比缩放下会偏）
        const math::Vec3d n = math::transformVector3D3D(world, localn);
        double lit[4];
        applyLighting(cpu, pe, wpos, n, view, lit);
        const std::uint32_t diffuse_src = rsOf(cpu, pe, kRsDiffuseMaterialSource);
        if (diffuse_src == 1u && fvf.has_diffuse) {
            // D3DMCS_COLOR1：顶点色参与
            lit[0] *= out->r / 255.0;
            lit[1] *= out->g / 255.0;
            lit[2] *= out->b / 255.0;
        }
        out->r = lit[0] * 255.0;
        out->g = lit[1] * 255.0;
        out->b = lit[2] * 255.0;
        out->a = lit[3] * 255.0;
    } else if (rsOf(cpu, pe, kRsLighting) != 0u && !fvf.has_diffuse) {
        // 光照开着、既没法线也没漫反射 ⇒ 用材质漫反射（真机的固定功能也是这样）
        MaterialState mat;
        readMaterial(cpu, pe, &mat);
        out->r = mat.diffuse[0] * 255.0;
        out->g = mat.diffuse[1] * 255.0;
        out->b = mat.diffuse[2] * 255.0;
        out->a = mat.diffuse[3] * 255.0;
    }
    // WORLD → VIEW → PROJECTION 走**同一个** multiplyD3D（行向量顺序在 math 里反转）
    const math::Mat4d m = math::multiplyD3D(math::multiplyD3D(world, view), proj);
    const math::Vec4d clip = math::transformVec4D3D(m, math::Vec4d(out->x, out->y, out->z, 1.0));
    out->x = clip.x;
    out->y = clip.y;
    out->z = clip.z;
    out->w = clip.w;
    (void)vertex_index;
}

bool gpuEligible(const DrawContext& ctx, std::string* why) {
    if (ctx.gpu == nullptr || !ctx.gpu->rasterActive()) {
        if (why) *why = "没有 GPU 光栅化后端";
        return false;
    }
    if (ctx.color_surface != 0 || ctx.color_fmt.bpp != 32) {
        if (why) *why = "GPU phase 1 只支持 32bpp 默认后缓冲";
        return false;
    }
    if (ctx.alpha_test) {
        if (why) *why = "GPU phase 1 不支持 alpha test";
        return false;
    }
    if (ctx.color_write != 0xF) {
        if (why) *why = "GPU phase 1 只支持完整 color write";
        return false;
    }
    if (ctx.shade != kShadeGouraud) {
        if (why) *why = "GPU phase 1 只支持 GOURAUD";
        return false;
    }
    if (ctx.separate_alpha_blend) {
        if (why) *why = "GPU phase 1 不支持 separate alpha blend";
        return false;
    }
    if (ctx.blend) {
        const bool replace = ctx.src_blend == kBlendOne && ctx.dst_blend == kBlendZero;
        const bool src_alpha = ctx.src_blend == kBlendSrcAlpha && ctx.dst_blend == kBlendInvSrcAlpha;
        if (!replace && !src_alpha) {
            if (why) *why = "GPU phase 1 只支持 ONE/ZERO 或 SRCALPHA/INVSRCALPHA 混合";
            return false;
        }
    }
    for (std::uint32_t s = 0; s < kMaxTextureStages; ++s) {
        if (ctx.textures[s].present) {
            if (why) *why = "GPU phase 1 不支持纹理";
            return false;
        }
        // 未绑定纹理 ⇒ CPU 采样是白色（identity）。stage 0 的默认 MODULATE / SELECTARG1
        // 在这里是恒等；其余运算（ADD 等）会改变结果 ⇒ 回退 CPU。
        const std::uint32_t cop = ctx.stages[s].color_op;
        const std::uint32_t aop = ctx.stages[s].alpha_op;
        const bool cop_ok = cop == kTopDisable || cop == kTopModulate || cop == kTopSelectArg1;
        const bool aop_ok = aop == kTopDisable || aop == kTopModulate || aop == kTopSelectArg1;
        if (!cop_ok || !aop_ok) {
            if (why) *why = "GPU phase 1 不支持该纹理级运算（无纹理时只允许 identity）";
            return false;
        }
    }
    return true;
}

bool submitPrimitives(X86& cpu, std::uint32_t pe, std::uint32_t prim, const std::vector<CVertex>& verts,
                      std::uint32_t prim_count, DrawContext& ctx, std::uint32_t* triangles_out,
                      std::uint32_t* rasterized_out, std::string* why) {
    // 顶点已经是**变换后**的 CVertex（由调用方喂），这里只做裁剪 + 屏幕映射 + 光栅
    std::uint32_t triangles = 0;
    std::uint32_t rasterized = 0;
    std::string gpu_why;
    const bool gpu_ok = gpuEligible(ctx, &gpu_why);
    if (!gpu_ok && ctx.gpu != nullptr && ctx.gpu->rasterDirty()) {
        // 上一批走 GPU、这一批要回 CPU：先把 GPU 像素落回 guest，CPU 才能接着画。
        std::string sync_err;
        ctx.gpu->rasterSyncToGuest(ctx.color_base, ctx.color_pitch, ctx.color_width, ctx.color_height, &sync_err);
    }
    std::vector<GpuRasterVertex> gpu_vertices;
    bool gpu_invw_ok = true;
    struct SavedTri {
        SVertex a, b, c;
    };
    std::vector<SavedTri> saved_tris;
    GpuRaster* gpu = gpu_ok ? ctx.gpu : nullptr;

    auto push_tri = [&](const SVertex& a, const SVertex& b, const SVertex& c) {
        const double area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
        if (!finite(area) || std::fabs(area) < 1e-12) return;
        if (area > 0.0) {
            if (ctx.cull == kCullCw) return;
        } else if (ctx.cull == kCullCcw) {
            return;
        }
        if (gpu == nullptr) {
            rasterTriangle(ctx, a, b, c);
            return;
        }
        // SVertex 的颜色/uv 已经乘过 invw；SDL_RenderGeometry 是线性插值，只有 invw==1 才等价。
        if (std::fabs(a.invw - 1.0) > 1e-9 || std::fabs(b.invw - 1.0) > 1e-9 ||
            std::fabs(c.invw - 1.0) > 1e-9) {
            gpu_invw_ok = false;
        }
        saved_tris.push_back({a, b, c});
        const auto add = [&](const SVertex& v) {
            GpuRasterVertex g;
            g.x = static_cast<float>(v.x);
            g.y = static_cast<float>(v.y);
            g.z = static_cast<float>(v.z);
            g.r = static_cast<float>(v.r / 255.0);
            g.g = static_cast<float>(v.g / 255.0);
            g.b = static_cast<float>(v.b / 255.0);
            g.a = static_cast<float>(v.a / 255.0);
            gpu_vertices.push_back(g);
        };
        add(a);
        add(b);
        add(c);
    };

    auto emit = [&](const CVertex& a, const CVertex& b, const CVertex& c) {
        ++triangles;
        CVertex poly[3] = {a, b, c};
        CVertex clipped[kMaxClipVerts];
        // ★ 预变换顶点裁屏幕空间（视口矩形），其余裁齐次裁剪空间 6 个面；语义见 clipHomogeneous 上方
        const std::uint32_t n = poly[0].pretransformed ? clipScreen(poly, 3, ctx, clipped)
                                                       : clipHomogeneous(poly, 3, clipped);
        if (n < 3) return;
        SVertex sv[kMaxClipVerts];
        for (std::uint32_t i = 0; i < n; ++i) {
            if (!toScreen(clipped[i], ctx, &sv[i])) return;
        }
        ++rasterized;
        for (std::uint32_t i = 1; i + 1 < n; ++i) push_tri(sv[0], sv[i], sv[i + 1]);
    };
    const std::uint32_t n = static_cast<std::uint32_t>(verts.size());
    switch (prim) {
        case kPtTriangleList:
            for (std::uint32_t i = 0; i + 2 < n; i += 3) emit(verts[i], verts[i + 1], verts[i + 2]);
            break;
        case kPtTriangleStrip:
            for (std::uint32_t i = 0; i + 2 < n; ++i) {
                if ((i & 1u) == 0) emit(verts[i], verts[i + 1], verts[i + 2]);
                else emit(verts[i + 1], verts[i], verts[i + 2]);
            }
            break;
        case kPtTriangleFan:
            for (std::uint32_t i = 1; i + 1 < n; ++i) emit(verts[0], verts[i], verts[i + 1]);
            break;
        case kPtPointList:
        case kPtLineList:
        case kPtLineStrip:
            recordUnsupported(cpu, pe, 16u, "点/线图元（只做三角形）", prim);
            break;
        default:
            if (why) *why = "D3D9 流水线：未知图元类型";
            return false;
    }

    if (gpu != nullptr && !gpu_invw_ok) {
        for (const SavedTri& t : saved_tris) rasterTriangle(ctx, t.a, t.b, t.c);
        gpu_vertices.clear();
    } else if (gpu != nullptr) {
        std::string gerr;
        bool ok = gpu->rasterPrepare(ctx.color_fmt, ctx.color_width, ctx.color_height, &gerr);
        if (ok && !gpu->rasterDirty()) {
            ok = gpu->rasterSyncFromGuest(ctx.color_base, ctx.color_pitch, ctx.color_width, ctx.color_height, &gerr);
        }
        GpuRasterState gs;
        gs.viewport_x = ctx.viewport_x;
        gs.viewport_y = ctx.viewport_y;
        gs.viewport_w = ctx.viewport_w;
        gs.viewport_h = ctx.viewport_h;
        gs.scissor_x0 = ctx.scissor_x0;
        gs.scissor_y0 = ctx.scissor_y0;
        gs.scissor_x1 = ctx.scissor_x1;
        gs.scissor_y1 = ctx.scissor_y1;
        gs.scissor_enabled = ctx.scissor_x0 != 0 || ctx.scissor_y0 != 0 || ctx.scissor_x1 != ctx.color_width ||
                             ctx.scissor_y1 != ctx.color_height;
        gs.blend = ctx.blend && ctx.src_blend == kBlendSrcAlpha && ctx.dst_blend == kBlendInvSrcAlpha;
        gs.depth_test = ctx.z_enable;
        gs.depth_write = ctx.z_write;
        gs.depth_func = ctx.z_func;
        if (ok && !gpu_vertices.empty()) ok = gpu->rasterDraw(gpu_vertices.data(), gpu_vertices.size(), gs, &gerr);
        if (ok) {
            ++g_gpu_draws;
            g_gpu_triangles += saved_tris.size();
            static bool logged = false;
            if (!logged && !saved_tris.empty()) {
                logged = true;
                const SavedTri& t = saved_tris.front();
                pipeLog("GPU", "首个 GPU 光栅 Draw：%zu 三角形（后端 %s）z=%.3f/%.3f/%.3f depth=%d/%d/%u blend=%d src=%u dst=%u",
                        saved_tris.size(), gpu->rasterName(), t.a.z, t.b.z, t.c.z, static_cast<int>(gs.depth_test),
                        static_cast<int>(gs.depth_write), gs.depth_func, static_cast<int>(gs.blend), ctx.src_blend,
                        ctx.dst_blend);
            }
        }
        if (!ok) {
            // GPU 本批失败：先让 guest 看到已经画出的部分，再用 CPU 重放这一批。
            std::string sync_err;
            gpu->rasterSyncToGuest(ctx.color_base, ctx.color_pitch, ctx.color_width, ctx.color_height, &sync_err);
            for (const SavedTri& t : saved_tris) rasterTriangle(ctx, t.a, t.b, t.c);
            gpu->rasterSyncFromGuest(ctx.color_base, ctx.color_pitch, ctx.color_width, ctx.color_height, &sync_err);
            static bool logged = false;
            if (!logged) {
                logged = true;
                pipeLog("GPU", "光栅化失败，已回退 CPU：%s", gerr.c_str());
            }
        }
    }

    (void)prim_count;
    plSet(cpu, pe, kPlTriangles, plGet(cpu, pe, kPlTriangles) + triangles);
    plSet(cpu, pe, kPlRasterized, plGet(cpu, pe, kPlRasterized) + rasterized);
    if (triangles_out) *triangles_out = triangles;
    if (rasterized_out) *rasterized_out = rasterized;
    std::uint64_t pixels = (static_cast<std::uint64_t>(plGet(cpu, pe, kPlPixels + 4u)) << 32) |
                           plGet(cpu, pe, kPlPixels);
    pixels += ctx.pixels_written;
    cpu.mem->wr32(pe + kPlPixels, static_cast<std::uint32_t>(pixels & 0xFFFFFFFFu));
    cpu.mem->wr32(pe + kPlPixels + 4u, static_cast<std::uint32_t>(pixels >> 32));
    return true;
}

// Draw 的公共前置检查（真机的失败码）
bool drawPrecheck(X86& cpu, const char* method, std::uint32_t dev, std::uint32_t* pe_out,
                  FvfLayout* fvf_out, std::string* why) {
    std::uint32_t pe = 0;
    if (!pipelineOf(cpu, dev, &pe, why)) return false;
    // ★ 进入 Draw 就算一次调用（含之后的失败）—— 计数器是引擎要求画了几次的可观察量
    plSet(cpu, pe, kPlDrawCalls, plGet(cpu, pe, kPlDrawCalls) + 1u);
    if (plGet(cpu, pe, kPlInScene) == 0) {
        pipeLog(method, "不在 BeginScene/EndScene 之间 ⇒ D3DERR_INVALIDCALL（真机语义）");
        if (why) *why = "D3D9 流水线：Draw 在 BeginScene 之外";
        return false;
    }
    const std::uint32_t decl = plGet(cpu, pe, kPlVertexDecl);
    if (decl != 0) {
        pipeLog(method, "设了顶点声明（0x%08X）但本线只支持 SetFVF ⇒ D3DERR_INVALIDCALL", decl);
        if (why) *why = "D3D9 流水线：只支持 FVF 路";
        return false;
    }
    const std::uint32_t fvf = plGet(cpu, pe, kPlFvf);
    if (fvf == 0) {
        pipeLog(method, "没有 SetFVF ⇒ D3DERR_INVALIDCALL");
        if (why) *why = "D3D9 流水线：Draw 之前没有 SetFVF";
        return false;
    }
    std::string parse_why;
    const FvfLayout l = parseFvf(fvf, &parse_why);
    if (!l.valid) {
        pipeLog(method, "FVF=0x%08X 不支持：%s ⇒ D3DERR_INVALIDCALL", fvf, parse_why.c_str());
        if (why) *why = parse_why;
        return false;
    }
    *pe_out = pe;
    *fvf_out = l;
    return true;
}

// 读一批顶点（从 guest 内存的 base 起，按 stride 走）
bool gatherVertices(X86& cpu, std::uint32_t pe, const FvfLayout& fvf, const std::uint8_t* base,
                    std::uint32_t stride, std::uint32_t first, std::uint32_t count,
                    const math::Mat4d& world, const math::Mat4d& view, const math::Mat4d& proj,
                    std::vector<CVertex>* out, std::string* why) {
    if (base == nullptr) {
        if (why) *why = "D3D9 流水线：顶点数据指针为空";
        return false;
    }
    if (stride < fvf.stride) {
        char buf[160];
        std::snprintf(buf, sizeof(buf), "D3D9 流水线：顶点步长 %u 小于 FVF 需要的最小步长 %u", stride, fvf.stride);
        if (why) *why = buf;
        return false;
    }
    out->clear();
    out->reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::uint32_t index = first + i;
        const std::uint8_t* vp = base + static_cast<std::size_t>(index) * stride;
        CVertex v;
        transformVertex(cpu, pe, fvf, vp, index, world, view, proj, &v);
        out->push_back(v);
    }
    return true;
}

}  // namespace

bool samplerUsesLinear(std::uint32_t mag_filter, std::uint32_t min_filter) {
    // 定义放匿名 namespace 外面：头文件里是公开 API（夹具直接调，证明 MIN 不再被忽略）
    auto linear = [](std::uint32_t f) { return f == kTexfLinear || f == kTexfAnisotropic; };
    return linear(mag_filter) || linear(min_filter);
}

std::uint32_t drawPrimitive(X86& cpu, std::uint32_t dev, std::uint32_t prim, std::uint32_t start_vertex,
                            std::uint32_t prim_count, std::string* why) {
    FvfLayout fvf;
    std::uint32_t pe = 0;
    if (!drawPrecheck(cpu, "DrawPrimitive", dev, &pe, &fvf, why)) return kInvalidCall;
    if (prim < kPtPointList || prim > kPtTriangleFan) {
        pipeLog("DrawPrimitive", "PrimitiveType=%u 非法 ⇒ D3DERR_INVALIDCALL", prim);
        return kInvalidCall;
    }
    if (prim_count == 0) {
        // 真机：0 个图元 ⇒ S_OK 且什么都不画
        return kOk;
    }
    const std::uint32_t vb = plGet(cpu, pe, kPlStreams);
    if (vb == 0) {
        pipeLog("DrawPrimitive", "没有 SetStreamSource(0) ⇒ D3DERR_INVALIDCALL");
        return kInvalidCall;
    }
    std::uint32_t length = 0;
    std::uint32_t data = 0;
    if (!vertexBufferInfoOfState(cpu, vb, &length, &data, nullptr, why)) return kInvalidCall;
    const std::uint32_t offset = plGet(cpu, pe, kPlStreams + 4u);
    std::uint32_t stride = plGet(cpu, pe, kPlStreams + 8u);
    if (stride == 0) stride = fvf.stride;
    std::uint32_t vertex_count = 0;
    switch (prim) {
        case kPtTriangleList: vertex_count = prim_count * 3u; break;
        case kPtTriangleStrip: vertex_count = prim_count + 2u; break;
        case kPtTriangleFan: vertex_count = prim_count + 2u; break;
        case kPtLineList: vertex_count = prim_count * 2u; break;
        case kPtLineStrip: vertex_count = prim_count + 1u; break;
        default: vertex_count = prim_count; break;
    }
    const std::uint64_t need = static_cast<std::uint64_t>(offset) +
                               static_cast<std::uint64_t>(start_vertex + vertex_count) * stride;
    if (need > length) {
        char buf[192];
        std::snprintf(buf, sizeof(buf),
                      "D3D9 流水线：DrawPrimitive 越过顶点缓冲（需要 %llu B，缓冲 %u B，stride=%u）",
                      static_cast<unsigned long long>(need), length, stride);
        pipeLog("DrawPrimitive", "%s ⇒ D3DERR_INVALIDCALL", buf);
        if (why) *why = buf;
        return kInvalidCall;
    }
    Machine* m = nullptr;
    if (!requireMachine(cpu, &m, why)) return kInvalidCall;
    DrawContext ctx;
    if (!prepareContext(cpu, dev, pe, &ctx, why)) return kInvalidCall;
    const std::uint8_t* base = nullptr;
    {
        // guest 内存里的顶点数据（不走 arena ⇒ 直接指针）
        // ⚠ 这里用 GuestMemory 的宿主映射（与 Lock 返回的 guest EA 是同一块内存）
        base = cpu.mem->host(data);
        if (!base) {
            if (why) *why = "D3D9 流水线：顶点缓冲的宿主映射为空";
            return kInvalidCall;
        }
    }
    const math::Mat4d world = readMatrix(cpu, pe + kPlTransforms + kTsWorld * 64u);
    const math::Mat4d view = readMatrix(cpu, pe + kPlTransforms + kTsView * 64u);
    const math::Mat4d proj = readMatrix(cpu, pe + kPlTransforms + kTsProjection * 64u);
    // 顶点偏移按字节：offset 是流偏移，start_vertex 是顶点序号
    std::vector<CVertex> verts;
    if (!gatherVertices(cpu, pe, fvf, base + offset, stride, start_vertex, vertex_count, world, view, proj, &verts,
                        why)) {
        return kInvalidCall;
    }
    std::uint32_t tris = 0;
    std::uint32_t rast = 0;
    if (!submitPrimitives(cpu, pe, prim, verts, prim_count, ctx, &tris, &rast, why)) return kInvalidCall;
    plSet(cpu, pe, kPlDraws, plGet(cpu, pe, kPlDraws) + 1u);
    pipeLog("DrawPrimitive",
            "PrimitiveType=%u StartVertex=%u PrimitiveCount=%u ⇒ 三角形 %u 个（光栅化 %u），写 %llu 个像素", prim,
            start_vertex, prim_count, tris, rast, static_cast<unsigned long long>(ctx.pixels_written));
    return kOk;
}

std::uint32_t drawIndexedPrimitive(X86& cpu, std::uint32_t dev, std::uint32_t prim, std::uint32_t base_vertex,
                                   std::uint32_t min_index, std::uint32_t num_vertices, std::uint32_t start_index,
                                   std::uint32_t prim_count, std::string* why) {
    FvfLayout fvf;
    std::uint32_t pe = 0;
    if (!drawPrecheck(cpu, "DrawIndexedPrimitive", dev, &pe, &fvf, why)) return kInvalidCall;
    if (prim < kPtPointList || prim > kPtTriangleFan) return kInvalidCall;
    if (prim_count == 0) return kOk;
    const std::uint32_t vb = plGet(cpu, pe, kPlStreams);
    const std::uint32_t ib = plGet(cpu, pe, kPlIndices);
    if (vb == 0 || ib == 0) {
        pipeLog("DrawIndexedPrimitive", "缺少 SetStreamSource(0) 或 SetIndices ⇒ D3DERR_INVALIDCALL");
        return kInvalidCall;
    }
    std::uint32_t vlength = 0;
    std::uint32_t vdata = 0;
    std::uint32_t ilength = 0;
    std::uint32_t idata = 0;
    std::uint32_t iformat = 0;
    if (!vertexBufferInfoOfState(cpu, vb, &vlength, &vdata, nullptr, why)) return kInvalidCall;
    if (!indexBufferInfoOfState(cpu, ib, &ilength, &idata, &iformat, why)) return kInvalidCall;
    std::uint32_t stride = plGet(cpu, pe, kPlStreams + 8u);
    if (stride == 0) stride = fvf.stride;
    const std::uint32_t index_size = iformat == 101u ? 2u : 4u;
    std::uint32_t index_count = 0;
    switch (prim) {
        case kPtTriangleList: index_count = prim_count * 3u; break;
        case kPtTriangleStrip: index_count = prim_count + 2u; break;
        case kPtTriangleFan: index_count = prim_count + 2u; break;
        case kPtLineList: index_count = prim_count * 2u; break;
        case kPtLineStrip: index_count = prim_count + 1u; break;
        default: index_count = prim_count; break;
    }
    if (start_index + index_count > ilength / index_size) {
        pipeLog("DrawIndexedPrimitive", "索引越界（start=%u count=%u，缓冲 %u 个索引）⇒ D3DERR_INVALIDCALL",
                start_index, index_count, ilength / index_size);
        return kInvalidCall;
    }
    Machine* m = nullptr;
    if (!requireMachine(cpu, &m, why)) return kInvalidCall;
    DrawContext ctx;
    if (!prepareContext(cpu, dev, pe, &ctx, why)) return kInvalidCall;
    const std::uint8_t* vbase = reinterpret_cast<const std::uint8_t*>(cpu.mem->host(vdata));
    const std::uint8_t* ibase = reinterpret_cast<const std::uint8_t*>(cpu.mem->host(idata));
    if (!vbase || !ibase) {
        if (why) *why = "D3D9 流水线：顶点 / 索引缓冲的宿主映射为空";
        return kInvalidCall;
    }
    const math::Mat4d world = readMatrix(cpu, pe + kPlTransforms + kTsWorld * 64u);
    const math::Mat4d view = readMatrix(cpu, pe + kPlTransforms + kTsView * 64u);
    const math::Mat4d proj = readMatrix(cpu, pe + kPlTransforms + kTsProjection * 64u);
    const std::uint32_t offset = plGet(cpu, pe, kPlStreams + 4u);
    std::vector<CVertex> verts;
    verts.reserve(index_count);
    for (std::uint32_t i = 0; i < index_count; ++i) {
        std::uint32_t idx = 0;
        if (index_size == 2u) {
            std::uint16_t v = 0;
            std::memcpy(&v, ibase + static_cast<std::size_t>(start_index + i) * 2u, 2u);
            idx = v;
        } else {
            std::memcpy(&idx, ibase + static_cast<std::size_t>(start_index + i) * 4u, 4u);
        }
        if (idx >= num_vertices) {
            pipeLog("DrawIndexedPrimitive", "索引 %u 超出 NumVertices=%u ⇒ D3DERR_INVALIDCALL", idx, num_vertices);
            return kInvalidCall;
        }
        const std::uint32_t vertex_index = idx + base_vertex;
        if ((static_cast<std::uint64_t>(offset) +
             static_cast<std::uint64_t>(vertex_index + 1u) * stride) > vlength) {
            pipeLog("DrawIndexedPrimitive", "顶点 %u 越过顶点缓冲 ⇒ D3DERR_INVALIDCALL", vertex_index);
            return kInvalidCall;
        }
        CVertex v;
        transformVertex(cpu, pe, fvf, vbase + offset + static_cast<std::size_t>(vertex_index) * stride,
                        vertex_index, world, view, proj, &v);
        verts.push_back(v);
    }
    (void)min_index;
    std::uint32_t tris = 0;
    std::uint32_t rast = 0;
    if (!submitPrimitives(cpu, pe, prim, verts, prim_count, ctx, &tris, &rast, why)) return kInvalidCall;
    plSet(cpu, pe, kPlDraws, plGet(cpu, pe, kPlDraws) + 1u);
    pipeLog("DrawIndexedPrimitive",
            "PrimitiveType=%u BaseVertexIndex=%d PrimitiveCount=%u ⇒ 三角形 %u（光栅化 %u），写 %llu 个像素", prim,
            static_cast<int>(base_vertex), prim_count, tris, rast,
            static_cast<unsigned long long>(ctx.pixels_written));
    return kOk;
}

std::uint32_t drawPrimitiveUP(X86& cpu, std::uint32_t dev, std::uint32_t prim, std::uint32_t prim_count,
                              std::uint32_t data_ea, std::uint32_t stride, std::string* why) {
    FvfLayout fvf;
    std::uint32_t pe = 0;
    if (!drawPrecheck(cpu, "DrawPrimitiveUP", dev, &pe, &fvf, why)) return kInvalidCall;
    if (prim < kPtPointList || prim > kPtTriangleFan) return kInvalidCall;
    if (prim_count == 0) return kOk;
    if (data_ea == 0) {
        pipeLog("DrawPrimitiveUP", "pVertexStreamZeroData=NULL ⇒ D3DERR_INVALIDCALL");
        return kInvalidCall;
    }
    if (stride == 0) stride = fvf.stride;
    if (stride < fvf.stride) {
        pipeLog("DrawPrimitiveUP", "VertexStreamZeroStride=%u < FVF 需要的最小步长 %u ⇒ D3DERR_INVALIDCALL", stride,
                fvf.stride);
        return kInvalidCall;
    }
    std::uint32_t vertex_count = 0;
    switch (prim) {
        case kPtTriangleList: vertex_count = prim_count * 3u; break;
        case kPtTriangleStrip: vertex_count = prim_count + 2u; break;
        case kPtTriangleFan: vertex_count = prim_count + 2u; break;
        case kPtLineList: vertex_count = prim_count * 2u; break;
        case kPtLineStrip: vertex_count = prim_count + 1u; break;
        default: vertex_count = prim_count; break;
    }
    const std::uint64_t need = static_cast<std::uint64_t>(vertex_count) * stride;
    if (!cpu.mem->committed(data_ea, static_cast<std::size_t>(need))) {
        pipeLog("DrawPrimitiveUP", "顶点数据 0x%08X 起 %llu 字节未提交 ⇒ D3DERR_INVALIDCALL", data_ea,
                static_cast<unsigned long long>(need));
        return kInvalidCall;
    }
    Machine* m = nullptr;
    if (!requireMachine(cpu, &m, why)) return kInvalidCall;
    DrawContext ctx;
    if (!prepareContext(cpu, dev, pe, &ctx, why)) return kInvalidCall;
    const std::uint8_t* base = reinterpret_cast<const std::uint8_t*>(cpu.mem->host(data_ea));
    if (!base) {
        if (why) *why = "D3D9 流水线：UP 顶点数据的宿主映射为空";
        return kInvalidCall;
    }
    const math::Mat4d world = readMatrix(cpu, pe + kPlTransforms + kTsWorld * 64u);
    const math::Mat4d view = readMatrix(cpu, pe + kPlTransforms + kTsView * 64u);
    const math::Mat4d proj = readMatrix(cpu, pe + kPlTransforms + kTsProjection * 64u);
    std::vector<CVertex> verts;
    if (!gatherVertices(cpu, pe, fvf, base, stride, 0u, vertex_count, world, view, proj, &verts, why)) {
        return kInvalidCall;
    }
    std::uint32_t tris = 0;
    std::uint32_t rast = 0;
    if (!submitPrimitives(cpu, pe, prim, verts, prim_count, ctx, &tris, &rast, why)) return kInvalidCall;
    plSet(cpu, pe, kPlDraws, plGet(cpu, pe, kPlDraws) + 1u);
    pipeLog("DrawPrimitiveUP",
            "PrimitiveType=%u PrimitiveCount=%u Stride=%u ⇒ 三角形 %u（光栅化 %u），写 %llu 个像素", prim,
            prim_count, stride, tris, rast, static_cast<unsigned long long>(ctx.pixels_written));
    return kOk;
}

std::uint32_t drawIndexedPrimitiveUP(X86& cpu, std::uint32_t dev, std::uint32_t prim, std::uint32_t min_index,
                                     std::uint32_t num_vertices, std::uint32_t prim_count,
                                     std::uint32_t index_ea, std::uint32_t index_format, std::uint32_t data_ea,
                                     std::uint32_t stride, std::string* why) {
    FvfLayout fvf;
    std::uint32_t pe = 0;
    if (!drawPrecheck(cpu, "DrawIndexedPrimitiveUP", dev, &pe, &fvf, why)) return kInvalidCall;
    if (prim < kPtPointList || prim > kPtTriangleFan) return kInvalidCall;
    if (prim_count == 0) return kOk;
    if (data_ea == 0 || index_ea == 0) {
        pipeLog("DrawIndexedPrimitiveUP", "pVertexStreamZeroData / pIndexData 为 NULL ⇒ D3DERR_INVALIDCALL");
        return kInvalidCall;
    }
    if (index_format != 101u && index_format != 102u) {
        pipeLog("DrawIndexedPrimitiveUP", "IndexDataFormat=%u（只认 INDEX16=101 / INDEX32=102）", index_format);
        return kInvalidCall;
    }
    if (stride == 0) stride = fvf.stride;
    if (stride < fvf.stride) return kInvalidCall;
    std::uint32_t index_count = 0;
    switch (prim) {
        case kPtTriangleList: index_count = prim_count * 3u; break;
        case kPtTriangleStrip: index_count = prim_count + 2u; break;
        case kPtTriangleFan: index_count = prim_count + 2u; break;
        case kPtLineList: index_count = prim_count * 2u; break;
        case kPtLineStrip: index_count = prim_count + 1u; break;
        default: index_count = prim_count; break;
    }
    const std::uint32_t index_size = index_format == 101u ? 2u : 4u;
    if (!cpu.mem->committed(index_ea, static_cast<std::size_t>(index_count) * index_size)) {
        pipeLog("DrawIndexedPrimitiveUP", "索引数据未提交 ⇒ D3DERR_INVALIDCALL");
        return kInvalidCall;
    }
    if (!cpu.mem->committed(data_ea, static_cast<std::size_t>(num_vertices) * stride)) {
        pipeLog("DrawIndexedPrimitiveUP", "顶点数据未提交 ⇒ D3DERR_INVALIDCALL");
        return kInvalidCall;
    }
    Machine* m = nullptr;
    if (!requireMachine(cpu, &m, why)) return kInvalidCall;
    DrawContext ctx;
    if (!prepareContext(cpu, dev, pe, &ctx, why)) return kInvalidCall;
    const std::uint8_t* ibase = reinterpret_cast<const std::uint8_t*>(cpu.mem->host(index_ea));
    const std::uint8_t* vbase = reinterpret_cast<const std::uint8_t*>(cpu.mem->host(data_ea));
    if (!ibase || !vbase) {
        if (why) *why = "D3D9 流水线：UP 顶点 / 索引数据的宿主映射为空";
        return kInvalidCall;
    }
    const math::Mat4d world = readMatrix(cpu, pe + kPlTransforms + kTsWorld * 64u);
    const math::Mat4d view = readMatrix(cpu, pe + kPlTransforms + kTsView * 64u);
    const math::Mat4d proj = readMatrix(cpu, pe + kPlTransforms + kTsProjection * 64u);
    std::vector<CVertex> verts;
    verts.reserve(index_count);
    for (std::uint32_t i = 0; i < index_count; ++i) {
        std::uint32_t idx = 0;
        if (index_size == 2u) {
            std::uint16_t v = 0;
            std::memcpy(&v, ibase + static_cast<std::size_t>(i) * 2u, 2u);
            idx = v;
        } else {
            std::memcpy(&idx, ibase + static_cast<std::size_t>(i) * 4u, 4u);
        }
        if (idx >= num_vertices) return kInvalidCall;
        CVertex v;
        transformVertex(cpu, pe, fvf, vbase + static_cast<std::size_t>(idx) * stride, idx, world, view, proj, &v);
        verts.push_back(v);
    }
    (void)min_index;
    std::uint32_t tris = 0;
    std::uint32_t rast = 0;
    if (!submitPrimitives(cpu, pe, prim, verts, prim_count, ctx, &tris, &rast, why)) return kInvalidCall;
    plSet(cpu, pe, kPlDraws, plGet(cpu, pe, kPlDraws) + 1u);
    pipeLog("DrawIndexedPrimitiveUP",
            "PrimitiveType=%u PrimitiveCount=%u Stride=%u ⇒ 三角形 %u（光栅化 %u），写 %llu 个像素", prim,
            prim_count, stride, tris, rast, static_cast<unsigned long long>(ctx.pixels_written));
    return kOk;
}

// =====================================================================================
// Present 的像素统计
// =====================================================================================
bool clearDepth(X86& cpu, std::uint32_t pipeline_ea, float z, std::string* why) {
    if (pipeline_ea == 0 || !cpu.mem->committed(pipeline_ea, kPlBytes) ||
        cpu.mem->rd32(pipeline_ea + kPlMagic) != kPlMagicValue) {
        if (why) *why = "D3D9 流水线：清深度时没有流水线状态块";
        return false;
    }
    Machine* m = nullptr;
    if (!requireMachine(cpu, &m, why)) return false;
    GuestSurface* dgs = m->surfaces().find(plGet(cpu, pipeline_ea, kPlDepthArena));
    if (!dgs) {
        if (why) *why = "D3D9 流水线：深度缓冲不在 arena 里";
        return false;
    }
    std::uint8_t* base = m->surfaces().pixels(*dgs);
    if (!base) {
        if (why) *why = "D3D9 流水线：深度缓冲像素指针为空";
        return false;
    }
    std::uint32_t bits = 0;
    std::memcpy(&bits, &z, sizeof(bits));
    for (int y = 0; y < dgs->height; ++y) {
        for (int x = 0; x < dgs->width; ++x) {
            std::memcpy(base + static_cast<std::size_t>(y) * dgs->pitch + static_cast<std::size_t>(x) * 4u, &bits,
                        4u);
        }
    }
    return true;
}
bool frameStats(const SurfaceFormat& fmt, const std::uint8_t* base, std::uint32_t pitch, int width, int height,
                FrameStats* out) {
    if (!base || width <= 0 || height <= 0) return false;
    const std::uint32_t bpp = fmt.bytesPerPixel();
    if (bpp == 0) return false;
    std::map<std::uint32_t, std::uint32_t> histogram;
    std::uint64_t hash = 1469598103934665603ull;
    for (int y = 0; y < height; ++y) {
        const std::uint8_t* row = base + static_cast<std::size_t>(y) * pitch;
        for (int x = 0; x < width; ++x) {
            const std::uint8_t* p = row + static_cast<std::size_t>(x) * bpp;
            const std::uint32_t raw = pixel_format::loadPixel(p, fmt.bpp);
            ++histogram[raw];
            hash ^= raw;
            hash *= 1099511628211ull;
        }
    }
    std::uint32_t best = 0;
    std::uint32_t best_count = 0;
    for (const auto& kv : histogram) {
        if (kv.second > best_count) {
            best_count = kv.second;
            best = kv.first;
        }
    }
    out->total_pixels = static_cast<std::uint32_t>(width) * static_cast<std::uint32_t>(height);
    out->background_color = best;
    out->background_pixels = best_count;
    out->non_background_pixels = out->total_pixels - best_count;
    out->distinct_colors = static_cast<std::uint32_t>(histogram.size());
    out->hash = hash;
    return true;
}

std::uint64_t gpuDrawCount() { return g_gpu_draws; }
std::uint64_t gpuTriangleCount() { return g_gpu_triangles; }

}  // namespace d3d9
}  // namespace lift
