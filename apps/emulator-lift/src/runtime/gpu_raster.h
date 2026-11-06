// emulator-lift · GPU 光栅化后端接口（SDL-free）
//
// ★ 只做 **光栅化**：顶点已经由 CPU 做完 T&L / 裁剪 / 屏幕映射（d3d9_pipeline.cpp），
//   这里接收屏幕空间三角形 + 状态，由宿主 GPU 画到 guest 后缓冲的镜像纹理。
// ★ 缺省是 CPU 后端；`--raster auto` 在窗口模式下选 GPU，`--raster gpu` 强制 GPU。
// ★ 后端实现负责：镜像纹理、绘制、把像素读回 guest（Present / 回退时）。
//
// ★ 状态口径（`GpuRasterState`）：只表达**后端能画**的 D3D9 固定功能状态子集。
//   准入判定在 `d3d9::gpuEligible()` / `d3d9::gpuBlendStateFor()`（纯函数，夹具可测）；
//   ⛔ 这里不猜"近似等价"——映射不了的状态由调用方回退 CPU 并记账。
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "runtime/surface.h"

namespace lift {

struct GpuRasterVertex {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float r = 1.0f;
    float g = 1.0f;
    float b = 1.0f;
    float a = 1.0f;
    float u = 0.0f;
    float v = 0.0f;
};

// D3DBLEND 的可表达子集（0 = 不支持；映射失败由 gpuBlendStateFor 拒绝）。
enum class GpuBlendFactor : std::uint8_t {
    Zero = 0,
    One,
    SrcColor,
    InvSrcColor,
    DstColor,
    InvDstColor,
    SrcAlpha,
    InvSrcAlpha,
    DstAlpha,
    InvDstAlpha,
    SrcAlphaSaturate,
};

// D3DBLENDOP 的可表达子集
enum class GpuBlendOp : std::uint8_t {
    Add = 0,
    Subtract,
    ReverseSubtract,
    Min,
    Max,
};

// 纹理 stage 0 的"颜色/alpha 怎么从 texture 与 diffuse 得来"（片元着色器置换）。
//   ModulateBoth            : tex * diffuse（颜色与 alpha 都是）
//   SelectTextureBoth       : tex（颜色与 alpha 都忽略 diffuse）
//   ModulateColorSelectAlpha: 颜色 tex*diffuse，alpha 取 tex.a
//   SelectColorModulateAlpha: 颜色取 tex，alpha tex.a*diffuse.a
//   ModulateColorDiffuseAlpha: 颜色 tex*diffuse，alpha 取 diffuse.a
//   SelectColorDiffuseAlpha : 颜色取 tex，alpha 取 diffuse.a
enum class GpuTexMode : std::uint8_t {
    ModulateBoth = 0,
    SelectTextureBoth,
    ModulateColorSelectAlpha,
    SelectColorModulateAlpha,
    ModulateColorDiffuseAlpha,
    SelectColorDiffuseAlpha,
};

enum class GpuFilter : std::uint8_t { Point = 0, Linear };
enum class GpuAddress : std::uint8_t { Wrap = 0, Clamp, Mirror };

struct GpuSampler {
    GpuAddress address_u = GpuAddress::Clamp;
    GpuAddress address_v = GpuAddress::Clamp;
    GpuFilter mag_filter = GpuFilter::Point;
    GpuFilter min_filter = GpuFilter::Point;
};

struct GpuRasterState {
    int viewport_x = 0;
    int viewport_y = 0;
    int viewport_w = 0;
    int viewport_h = 0;
    int scissor_x0 = 0;
    int scissor_y0 = 0;
    int scissor_x1 = 0;
    int scissor_y1 = 0;
    bool scissor_enabled = false;
    bool depth_test = true;
    bool depth_write = true;
    std::uint32_t depth_func = 4;  // D3DCMP_LESSEQUAL
    // ---- 混合（D3D9 设备状态 → SDL_GPUColorTargetBlendState 的 color/alpha 两段）----
    bool blend = false;
    GpuBlendFactor src_blend = GpuBlendFactor::SrcAlpha;
    GpuBlendFactor dst_blend = GpuBlendFactor::InvSrcAlpha;
    GpuBlendOp blend_op = GpuBlendOp::Add;
    bool separate_alpha_blend = false;
    GpuBlendFactor src_blend_alpha = GpuBlendFactor::SrcAlpha;
    GpuBlendFactor dst_blend_alpha = GpuBlendFactor::InvSrcAlpha;
    GpuBlendOp blend_op_alpha = GpuBlendOp::Add;
    // D3DCOLORWRITEENABLE_*：R=1 G=2 B=4 A=8（⛔ 与 SDL 的 RGBA 位序一致，但语义各自有注释）
    std::uint8_t color_write = 0xFu;
    // 纹理 stage 0 的片元置换（只在 rasterDrawTextured 时使用）
    GpuTexMode tex_mode = GpuTexMode::ModulateBoth;
};

struct GpuRasterTexture {
    const std::uint8_t* pixels = nullptr;
    std::uint32_t pitch = 0;
    int width = 0;
    int height = 0;
    SurfaceFormat format;
    const void* key = nullptr;  // guest 纹理像素指针（缓存键）
    // guest 纹理内容版本（D3D9 的 UnlockRect 次数）。非 0 且与缓存一致 ⇒ 不重传、也不做内容哈希；
    // 0 = 未知（还没 Unlock 过）⇒ 后端退回内容哈希比较。
    std::uint32_t generation = 0;
    // guest 纹理身份（进程级单调 serial）。★ 缓存键用它：guest 指针 / 尺寸 / alpha / generation
    // 都可能撞车（arena 复用 + 新对象 generation 也从 0/1 起）。0 = 夹具/白纹理 ⇒ 退回指针键。
    std::uint32_t serial = 0;
    GpuSampler sampler;         // 缺省 NEAREST + CLAMP（D3DXSprite 的现状，⛔ 不变）
};

// GPU 纹理缓存身份键（纯函数；夹具可直接测）。
//   · serial != 0：串行号唯一 ⇒ 键的高位标记 + serial×常量 ⊕ 尺寸 ⊕ alpha；⛔ 不复用指针/EA。
//   · serial == 0：退回 guest 像素指针键（D3DXSprite 的 1×1 白纹理、夹具纹理）。
//   · 两条路径用 bit63 区分，避免 serial 键与指针键碰撞。
inline std::uint64_t gpuTextureCacheKey(const GpuRasterTexture& t, int width, int height) {
    const std::uint64_t dims = (static_cast<std::uint64_t>(static_cast<std::uint32_t>(width)) << 32) |
                               static_cast<std::uint64_t>(static_cast<std::uint32_t>(height));
    const std::uint64_t alpha = t.format.amask != 0 ? 1ull : 0ull;
    std::uint64_t k = 0;
    if (t.serial != 0) {
        k = static_cast<std::uint64_t>(t.serial) * 0x9E3779B97F4A7C15ull;
        k ^= dims * 0xBF58476D1CE4E5B9ull;
        k ^= alpha * 0x94D049BB133111EBull;
        return k | (1ull << 63);
    }
    const std::uint64_t ptr = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(t.key));
    k = ptr * 0x9E3779B97F4A7C15ull;
    k ^= dims * 0xBF58476D1CE4E5B9ull;
    k ^= alpha * 0x94D049BB133111EBull;
    return k & ~(1ull << 63);
}

class GpuRaster {
public:
    virtual ~GpuRaster() = default;

    virtual bool rasterActive() const = 0;
    virtual const char* rasterName() const = 0;
    // D3DXSprite 纹理四边形是否也走 GPU（可用 `--raster-sprites 0` 做 A/B）。
    virtual bool rasterSpritesEnabled() const { return true; }

    // 目标纹理（尺寸/格式变化时重建）。
    virtual bool rasterPrepare(const SurfaceFormat& format, int width, int height, std::string* err) = 0;
    // 用 guest 后缓冲的当前内容初始化/同步 GPU 镜像（CPU Clear / CPU 回退之后）。
    virtual bool rasterSyncFromGuest(const std::uint8_t* pixels, std::uint32_t pitch, int width, int height,
                                     std::string* err) = 0;
    // 把 GPU 镜像读回 guest（Present / CPU 回退之前）。
    virtual bool rasterSyncToGuest(std::uint8_t* pixels, std::uint32_t pitch, int width, int height,
                                   std::string* err) = 0;
    // 画一批屏幕空间三角形（triangle list）。
    virtual bool rasterDraw(const GpuRasterVertex* vertices, std::size_t count, const GpuRasterState& state,
                            std::string* err) = 0;
    // 带纹理的三角形（D3DXSprite 与 D3D9 stage 0 纹理都走这里）。
    virtual bool rasterDrawTextured(const GpuRasterVertex* vertices, std::size_t count,
                                    const GpuRasterTexture& texture, const GpuRasterState& state,
                                    std::string* err) = 0;
    // GPU 镜像里是否有尚未读回 guest 的绘制。
    virtual bool rasterDirty() const = 0;
};

}  // namespace lift
