// emulator-lift · GPU 光栅化后端接口（SDL-free）
//
// ★ 只做 **光栅化**：顶点已经由 CPU 做完 T&L / 裁剪 / 屏幕映射（d3d9_pipeline.cpp），
//   这里接收屏幕空间三角形 + 状态，由宿主 GPU 画到 guest 后缓冲的镜像纹理。
// ★ 缺省是 CPU 后端；`--raster auto` 在窗口模式下选 GPU，`--raster gpu` 强制 GPU。
// ★ 后端实现负责：镜像纹理、绘制、把像素读回 guest（Present / 回退时）。
// ★ 另外可选地暴露 `GpuDirectPresenter`（`--present window` + `--raster gpu`）：
//   把**同一份** target blit 到窗口 swapchain，呈现不再经过 `SDL_UpdateTexture`。
//   光栅化与呈现仍是两件事：`directPresenter()==nullptr` 时呈现走 SDL_Renderer / headless。
//
// ★ 合批口径（2026-11）：一次 draw 不再各自提交一次 —— `rasterQueue` 入队，`rasterFlush` 把
//   一帧的 draw 合成**一个** command buffer。★ 方案 C（先脏矩形判、再切段）：纹理内容在队列里
//   被引用期间发生变化时，**先判**写入矩形与本段已入队 draw 的采样矩形是否相交；相交才在边界处
//   切段（`[copy][render 1..k][copy][render k+1..n]`），不相交就不切。切段 ≠ 提交：整帧只在
//   "要 CPU 看见"（读回 / 换 target / Present）时 `Submit` 一次。
//   ⛔ 只合并提交，不合并 draw：每条 draw 的 pipeline/blend/ztest/viewport/scissor/sampler/纹理
//   都逐条保留、顺序不变。屏障清单见 `d3d9_gpu.cpp` 文件头。
// ★ 状态口径（`GpuRasterState`）：只表达**后端能画**的 D3D9 固定功能状态子集。
//   准入判定在 `d3d9::gpuEligible()` / `d3d9::gpuBlendStateFor()`（纯函数，夹具可测）；
//   ⛔ 这里不猜"近似等价"——映射不了的状态由调用方回退 CPU 并记账。
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

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

// ---------------------------------------------------------------------------------------
// texel 矩形 + 方案 C（先脏矩形判、再切段）的**纯函数**判据
// ---------------------------------------------------------------------------------------
// ★ 坐标口径：texel 坐标、**右/下开区间**（`x + w` = right，`y + h` = bottom），单位 = 纹理像素。
// ★ 为什么放进这个 SDL-free 的头文件：真后端与 Mock 后端必须用**同一份**判据；夹具可以直接测它。
struct GpuTexelRect {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
    int right() const { return x + w; }
    int bottom() const { return y + h; }
    bool empty() const { return w <= 0 || h <= 0; }
};

inline bool gpuTexelRectsIntersect(const GpuTexelRect& a, const GpuTexelRect& b) {
    if (a.empty() || b.empty()) return false;
    return a.x < b.right() && b.x < a.right() && a.y < b.bottom() && b.y < a.bottom();
}

// 一条 draw 的采样区域 = 顶点 u/v 的 bbox × 纹理尺寸，**外扩 1 texel**（过滤/纯量化的保守外扩）。
// ★ 保守到底的四条（做不到就返回整幅，宁可多切一段，⛔ 不许少切）：
//   · u/v 有任何一项落在 [0,1] 之外（WRAP 寻址会把整幅都采到）、或非有限值 ⇒ 整幅；
//   · **寻址是 WRAP/MIRROR 且 bbox 贴到 0/1 边界** ⇒ 整幅（贴边采样会把对边 texel 也采进来：
//     Linear 必采、Nearest 在 u==1.0 处正好落到对边）；
//   · 顶点数为 0 / 尺寸非法 ⇒ 空矩形（这条 draw 不可能采到任何 texel）；
//   · 外扩后与纹理求交为空（完全采样不到）⇒ 空矩形。
inline GpuTexelRect gpuDrawSampleRect(const GpuRasterVertex* v, std::size_t n, int width, int height,
                                      bool wrap_u = false, bool wrap_v = false) {
    GpuTexelRect whole{0, 0, width, height};
    if (v == nullptr || n == 0) return GpuTexelRect{};
    if (width <= 0 || height <= 0) return GpuTexelRect{};
    float umin = v[0].u;
    float umax = v[0].u;
    float vmin = v[0].v;
    float vmax = v[0].v;
    for (std::size_t i = 1; i < n; ++i) {
        if (!std::isfinite(v[i].u) || !std::isfinite(v[i].v)) return whole;
        if (v[i].u < umin) umin = v[i].u;
        if (v[i].u > umax) umax = v[i].u;
        if (v[i].v < vmin) vmin = v[i].v;
        if (v[i].v > vmax) vmax = v[i].v;
    }
    if (!std::isfinite(umin) || !std::isfinite(umax) || !std::isfinite(vmin) || !std::isfinite(vmax)) return whole;
    if (umin < 0.0f || vmin < 0.0f || umax > 1.0f || vmax > 1.0f) return whole;
    if ((wrap_u && (umin <= 0.0f || umax >= 1.0f)) || (wrap_v && (vmin <= 0.0f || vmax >= 1.0f))) return whole;
    int x0 = static_cast<int>(std::floor(static_cast<double>(umin) * static_cast<double>(width))) - 1;
    int y0 = static_cast<int>(std::floor(static_cast<double>(vmin) * static_cast<double>(height))) - 1;
    int x1 = static_cast<int>(std::ceil(static_cast<double>(umax) * static_cast<double>(width))) + 1;
    int y1 = static_cast<int>(std::ceil(static_cast<double>(vmax) * static_cast<double>(height))) + 1;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > width) x1 = width;
    if (y1 > height) y1 = height;
    if (x1 <= x0 || y1 <= y0) return GpuTexelRect{};
    return GpuTexelRect{x0, y0, x1 - x0, y1 - y0};
}

// 方案 C 的**切段判据**（纯函数）：脏区与本段已入队、采样**同一纹理**的 draw 的采样矩形相交 ⇒
// 必须在内容变更边界处切段；全不相交 ⇒ 不用切（上传并进本段开头那个 copy pass，字节不变、
// 那些 draw 也采不到新内容）。
inline bool gpuDirtyRectNeedsCut(const GpuTexelRect& dirty, const GpuTexelRect* samples, std::size_t count) {
    if (dirty.empty()) return false;
    for (std::size_t i = 0; i < count; ++i) {
        if (gpuTexelRectsIntersect(dirty, samples[i])) return true;
    }
    return false;
}

// 这次内容变更**必须上传的区域**（纯函数；真后端与 Mock 共用，⛔ 不要各写一份）。
//   · 写区未知 / 版本对不上 / 后端已录内容是"未知" ⇒ `whole = true`（整幅，必然要切段）；
//   · 后端已录内容正好是"上一次写入的那一版" ⇒ 只脏了最近一次 LockRect 的矩形。
// ★ 定义放在 `GpuRasterTexture` 之后（它要读那个结构）。

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
    // guest 纹理内容版本（`TextureInfo::version`：0 = 未知，非 0 = 已知内容的单调序列号）。
    // 非 0 且与缓存一致 ⇒ 不重传、也不做内容哈希；0 = 未知（刚 Lock / 当过 render target）
    // ⇒ 后端退回内容哈希比较。
    std::uint32_t generation = 0;
    // guest 纹理身份（进程级单调 serial）。★ 缓存键用它：guest 指针 / 尺寸 / alpha / generation
    // 都可能撞车（arena 复用 + 新对象 generation 也从 0/1 起）。0 = 夹具/白纹理 ⇒ 退回指针键。
    std::uint32_t serial = 0;
    GpuSampler sampler;         // 缺省 NEAREST + CLAMP（D3DXSprite 的现状，⛔ 不变）
    // ---- 方案 C 的"内容变更边界"数据（由 guest 侧的 LockRect/UnlockRect 记录，生产者填）----
    // ★ 语义：guest 只在 `LockRect` 允许的矩形内写像素（D3D9 API 契约；矩形外的直写是未定义行为）。
    //   `content_change_version` = 最近一次 UnlockRect 发的内容版本；`content_change_prev_version`
    //   = 它之前的版本。后端据此判断"缓存里的内容是哪一版"，只上传真正脏的那块矩形。
    // ★ `content_change_known == false` ⇒ 写区未知 ⇒ 后端按**整幅**处理（也必然切段，⛔ 不猜）。
    bool content_change_known = false;
    GpuTexelRect content_change_rect;               // 最近一次内容变更的写入区域（texel，右/下开）
    std::uint32_t content_change_version = 0;       // 该变更对应的内容版本（必须 == generation 才算数）
    std::uint32_t content_change_prev_version = 0;  // 变更前的版本（0 = 没有 ⇒ 不能声称缓存越过了它）
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

// 这次内容变更**必须上传的区域**（纯函数；真后端与 Mock 共用，⛔ 不要各写一份）。
//   · 写区未知 / 版本对不上 / 后端已录内容是"未知" ⇒ `whole = true`（整幅，必然要切段）；
//   · 后端已录内容正好是"上一次写入的那一版" ⇒ 只脏了最近一次 LockRect 的矩形。
struct GpuDirtyRegion {
    bool whole = true;  // true = 按整幅处理；false = `rect` 就是脏区（已钳制到纹理范围、非空）
    GpuTexelRect rect;
};

inline GpuDirtyRegion gpuContentDirtyRegion(const GpuRasterTexture& t, std::uint32_t recorded_generation,
                                            int width, int height) {
    GpuDirtyRegion out;
    out.whole = true;
    out.rect = GpuTexelRect{0, 0, width, height};
    if (width <= 0 || height <= 0) return out;
    if (!t.content_change_known || t.generation == 0 || t.content_change_version != t.generation) return out;
    if (recorded_generation == 0 || recorded_generation != t.content_change_prev_version) return out;
    int x0 = t.content_change_rect.x;
    int y0 = t.content_change_rect.y;
    int x1 = t.content_change_rect.right();
    int y1 = t.content_change_rect.bottom();
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > width) x1 = width;
    if (y1 > height) y1 = height;
    if (x1 <= x0 || y1 <= y0) return out;  // 钳完是空 ⇒ 什么也证明不了 ⇒ 整幅（保守）
    out.whole = false;
    out.rect = GpuTexelRect{x0, y0, x1 - x0, y1 - y0};
    return out;
}

// ---------------------------------------------------------------------------------------
// GPU 直出（`--present window --raster gpu`）：把 GPU target 直接 blit 到窗口 swapchain
// ---------------------------------------------------------------------------------------
// ★ 只有真的拿到 `SDL_ClaimWindowForGPUDevice` 的后端才提供本接口；CPU 后端 / headless /
//   直出不可用而回退 SDL_Renderer 上传路径时，`GpuRaster::directPresenter()` 返回 nullptr
//   ⇒ 呈现语义与今天逐字节一致。
// ★ 为什么单列一个接口：claim / present mode / 窗口回读都**不属于光栅化**；塞进 `GpuRaster`
//   会让 CPU 后端也被迫实现一堆空函数，也会让"有没有直出"变成隐式状态。
// ★ 本头文件仍然 **SDL-free**：窗口用 `void*` 传（真类型只有 present_sdl.cpp / d3d9_gpu.cpp 知道）。
class GpuDirectPresenter {
public:
    virtual ~GpuDirectPresenter() = default;

    // 窗口创建后 claim swapchain（`SDL_ClaimWindowForGPUDevice`）+ 设 present mode。
    // 失败 ⇒ 调用方回退 SDL_Renderer 上传路径（⛔ 不假装直出）；`err` 必须带 SDL 的错误串。
    virtual bool directAttachWindow(void* sdl_window, bool vsync, std::string* err) = 0;
    // 释放 claim（**必须**在 `SDL_DestroyWindow` 之前调用）。
    virtual void directDetachWindow() = 0;
    // 已 claim 且 swapchain 格式可 blit ⇒ 呈现可以走直出。
    virtual bool directActive() const = 0;
    virtual const char* directDriverName() const = 0;   // 例 "direct3d12"
    virtual const char* directPresentMode() const = 0;  // 实际生效的 present mode（回退后也算实际值）

    // 本帧没有 GPU 绘制（guest 才是最新的）⇒ 呈现前把 guest 后缓冲上传到 target。
    // ⛔ 不会在读回之后调用（那时 guest 与 target 已经一致）。
    // 内部按需 `rasterPrepare(format, w, h)`（首帧可能还没有 target）。
    virtual bool directUploadGuest(const SurfaceFormat& format, const std::uint8_t* pixels, std::uint32_t pitch,
                                   int width, int height, std::string* err) = 0;
    // 「本帧的最新像素已经在 target 里」——由 `d3d9::Present` 在读回/同步之后置 true（它保证
    // target == guest）。缺省 false ⇒ 呈现前 sink 必须把 guest 上传，⛔ 不会把上一帧当这一帧
    // （DDraw Flip / 测试里直接 submitFrame 都走这条）。
    virtual void directSetTargetCurrent(bool current) = 0;
    virtual bool directTargetCurrent() const = 0;
    // target → swapchain（letterbox 缩放；窗口尺寸与 target 不同也支持）。
    // 最小化 / 零尺寸的帧返回 true 但什么都不呈现（⛔ 不当失败）。
    virtual bool directPresent(std::string* err) = 0;
    // 窗口像素回读（`--present-screenshot` / 捕获测试）：XRGB8888 字节序（B,G,R,X），含 letterbox 黑边。
    virtual bool directReadback(std::vector<std::uint8_t>* xrgb, int* width, int* height,
                               std::string* err) = 0;
};

class GpuRaster {
public:
    virtual ~GpuRaster() = default;

    virtual bool rasterActive() const = 0;
    virtual const char* rasterName() const = 0;
    // D3DXSprite 纹理四边形是否也走 GPU（可用 `--raster-sprites 0` 做 A/B）。
    virtual bool rasterSpritesEnabled() const { return true; }

    // 目标纹理（尺寸/格式变化时重建）。
    // ★ `color_base` = 这块 GPU 镜像对应的 guest 像素基址（后缓冲 / 渲染目标 surface 的像素）。
    //   D3D9 允许「换成另一张同尺寸 surface」⇒ 只看尺寸/格式**判不出来**；基址变化必须视为
    //   换 target（先 flush + 回读旧目标，再重建镜像），见 d3d9_gpu.cpp 的 rasterPrepare。
    //   nullptr = 调用方不提供身份（夹具 / 离屏上传）⇒ 退回旧的"只看尺寸/格式"口径。
    virtual bool rasterPrepare(const SurfaceFormat& format, int width, int height, std::string* err,
                               const std::uint8_t* color_base = nullptr) = 0;
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

    // -----------------------------------------------------------------------------------
    // 合批：入队 + 一次 flush（「一次 draw = 一次提交」→「一帧的 draw 合到一次提交」）
    // -----------------------------------------------------------------------------------
    // ★ 旧接口（rasterDraw / rasterDrawTextured）语义**不变** = 入队 + **立即** flush：
    //   夹具、`--gpu-verify`（判据要求"画完立刻能读"）都还走它。
    // ★ `rasterQueue` 的契约：
    //   · 顶点由后端**拷贝**（调用方返回后可以释放自己的数组）；
    //   · 纹理必须在**入队时**就交给后端：`GpuRasterTexture::pixels` 是 guest 指针，
    //     D3DXSprite 在 End 末尾就 Release 那条纹理（AddRef 只保到 End）⇒ ⛔ 后端不许保存
    //     这个指针、也不许拖到 flush 再读；
    //   · 状态（pipeline/blend/ztest/viewport/scissor/sampler/纹理）逐条保留，⛔ 不合并两条不同的 draw。
    // ★ 内容变更边界（方案 C）：`GpuRasterTexture` 带着"最近一次写入的矩形/版本"进来。后端据此
    //   ①按脏矩形判要不要切段（`gpuDirtyRectNeedsCut`）；②只上传脏区。切段 ≠ 提交：一个 command
    //   buffer 内可以 `copy → render → copy → render …`，只有"要 CPU 看见"（读回/换 target）才 Submit。
    // ★ 默认实现 = 旧的整批路径（入队 + 立即 flush）⇒ CPU 后端 / 测试替身不用改。
    virtual bool rasterQueue(const GpuRasterVertex* vertices, std::size_t count,
                             const GpuRasterTexture* texture,  // nullptr = 无纹理
                             const GpuRasterState& state, std::string* err) {
        if (texture != nullptr) return rasterDrawTextured(vertices, count, *texture, state, err);
        return rasterDraw(vertices, count, state, err);
    }
    // 把队列里的 draw（以及尚未上传的纹理脏区）一次提交（SDL_GPU 后端 = 一个 command buffer，
    // 内部按需 `copy → render → copy → render …` 多段，但只 `SDL_SubmitGPUCommandBuffer` 一次）。
    // ★ 队列为空且没有待上传内容 ⇒ 立刻返回 true（零成本，⛔ 不提交空 command buffer）。
    // ★ 屏障：读 target 前必须先 flush（`rasterSyncToGuest` 内部会做）；换 target / Clear /
    //   LockRect / Present / directPresent 也要先 flush（清单见 d3d9_gpu.cpp 文件头）。
    // 默认实现 true = 「没有队列的后端」。
    virtual bool rasterFlush(std::string* err) {
        (void)err;
        return true;
    }
    // 当前 GPU 镜像对应的 guest 像素基址（nullptr = 未准备 / 不参与判断）。
    // Clear / LockRect / Present 用它判「这块 guest 像素是不是镜像的目标」。
    virtual const void* rasterTargetBase() const { return nullptr; }
    // 诊断：尚未 flush 的 draw 条数（默认 0 = 没有队列）。测试用它证明合批真的生效。
    virtual std::size_t rasterQueuedDraws() const { return 0; }
    // ★ 请后端在**下一个** render pass 把深度清成 1.0（D3D9 `Clear(...,D3DCLEAR_ZBUFFER)` 的语义）。
    //   只由 `d3d9::clear` 在 ZBUFFER 位上调用；颜色定向的同步 / 整块上传**不许**顺带清深度
    //   （D3D9 里 Clear(TARGET) 不动深度，而"每帧清一次"会掩盖这个差异）。默认 no-op。
    virtual void rasterRequestDepthClear() {}
    // 设备 / target 销毁前丢掉队列与镜像（⛔ 不要往即将释放的 guest 像素回读）。默认 no-op。
    virtual void rasterDiscardTarget() {}

    // GPU 镜像里是否有尚未读回 guest 的绘制。
    // ★ 语义（合批 + 分段后）= **队列非空 或 已提交但尚未读回 或 还有未提交的纹理上传** ——
    //   即「镜像/纹理缓存 != guest」。缺了最后一项，屏障会漏判（已录未提交的上传被当成已落地）。
    //   调用方的 `if (!rasterDirty()) rasterSyncFromGuest(...)` 因此不会在下一个 sprite 时
    //   把 guest 后缓冲重新上传、擦掉已入队但还没提交的上一条 draw。
    virtual bool rasterDirty() const = 0;

    // 可选：GPU 直出接口。nullptr = 没有 swapchain（CPU 后端 / 直出不适用 / 已回退 renderer）。
    virtual GpuDirectPresenter* directPresenter() { return nullptr; }
};

// ---------------------------------------------------------------------------------------
// 诊断计数（提交 / 切段 / 上传）—— 「提交次数」是"每条 draw 一次提交"那个回归的判据
// ---------------------------------------------------------------------------------------
// ★ 为什么不是 `GpuRaster` 的虚函数：`SdlWindow`（present_sdl）是 `GpuRaster` 的**转发壳**，
//   每加一个虚函数那一层就要再写一遍转发；而计数是**纯诊断**，也不该进生产者/后端的接口契约。
//   ⇒ 进程内一份（同一时刻只有一个 SDL_GPU 后端），测试与 `LIFT_GPU_SUBMITS` 都读它。
// ★ 判据口径：**同一次 flush 里切 N 段只算 1 次提交**。测试用前后差值断言（不重置）。
struct GpuRasterStats {
    std::uint64_t submits = 0;       // `SDL_SubmitGPUCommandBuffer*` 的总次数（含读回/直出）
    std::uint64_t submits_flush = 0;     // 其中：把队列画进 target 的那一次（切 N 段也只算 1 次）
    std::uint64_t submits_readback = 0;  // 其中：把 target 读回 CPU（带 fence 等待）
    std::uint64_t submits_upload = 0;    // 其中：整块 guest 像素上传（呈现/同步路径）
    std::uint64_t submits_present = 0;   // 其中：窗口 swapchain 的 blit
    std::uint64_t cuts = 0;          // 内容变更边界切段次数（切段 ≠ 提交）
    std::uint64_t uploads = 0;       // 纹理上传块数（方案 C 后 = 脏矩形块数）
    std::uint64_t full_uploads = 0;  // 其中"整幅"上传数（写区未知 ⇒ 退化）
};
GpuRasterStats gpuRasterStats();

}  // namespace lift
