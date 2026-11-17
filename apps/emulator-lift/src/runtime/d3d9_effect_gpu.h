// emulator-lift · **每个 effect draw 一个 shader + 常量表 + 最多 2 张纹理**的后端侧表达
//（GPU 路线 P2；需求单 `REQ-01M7MSGRRHCA23RAX1QGDAYJ3X`）
//
// ★ 为什么单开一个接口、而不是改 `GpuRaster` / `GpuTexMode`：
//   · `GpuRasterState` / `GpuTexMode` 表达的是 **D3D9 固定功能**的片元置换（6 档，语义已冻结，
//     `gpu_raster.h` 的那张表逐条有守卫）；effect 的 draw 走的是**guest 的 shader**，
//     它与固定功能**互斥**（stage 状态根本不参与）⇒ 塞进同一个枚举会让"哪档是谁"变得说不清。
//   · 这套后端能力**不是每个 GpuRaster 实现都得有**（Mock / 夹具 / 未来别的后端可以不支持）：
//     用一个可以 `dynamic_cast` 到的**可选接口**表达，谁支持谁实现；不支持 ⇒ 调用方**具名拒绝**
//     （⛔ 不静默降级成固定功能画，那会写出错误像素而账上一个位都没有）。
//
// ★ 生命周期契约与 `rasterQueue` **逐条一致**（`gpu_raster.h` 的 `rasterQueue` 注释）：
//   · 顶点 / 第二套 UV / 纹理像素都由后端在**入队那一刻**拷走，调用方返回后可以释放；
//   · 状态（pipeline / blend / ztest / viewport / scissor / sampler / 纹理）逐条保留、顺序不变。
//
// ★ 顶点仍是**CPU 做完 T&L / 裁剪 / 屏幕映射**的屏幕空间三角形（`docs/03-emulator/design-notes.md`
//   的 `L3-tl-01`）：这里只多带一套纹理坐标（`uv1`），⛔ 不搬顶点变换、⛔ 没有 vertex shader 的语义。
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "runtime/gpu_raster.h"

namespace lift {

// FX 文本里**已经实现**的 shader 程序（每个 = 后端源码里的一个 entry point）。
//   ★ 这张表是"实现集"的**唯一**声明面：不在表里的程序 ⇒ 调用方**具名拒绝**（bit 44）。
//   · `FadePs20` = 资源 201 `technique Fade` 的 `compile ps_2_0 FadePS()`（alpha 归一化）
//   · `FadePs10` = 资源 200 同一个 technique 的 `compile ps_1_0 FadePS()`（逐通道直接 lerp）
//   · `SlideBlurPs20` = 资源 203 `technique SlideBlur` 的 `compile ps_2_0 SlideBlurPS()`
//     （**21 个 tap**：源文本 `for( float i = -10.0f; i <= 10.0f; i++ )`，每 tap 权重 `0.0476f`）
//   · `ZoomBlurPs20` = 资源 203 `technique ZoomBlur` 的 `compile ps_2_0 ZoomBlurPS()`
//     （**10 个 tap**：`for( float i = 0.0f; i < 10.0f; i++ )`，权重是文本里的 `ratio[10]` 表）
//   · `SnowPs10` = 资源 202 `technique Snow` 的 `compile ps_1_0 SnowPS()`
//     （`return tex2D(TexSmp, UV) * Color;`；`Color` 是 **uniform float4**，⛔ 不是顶点色；
//     同 pass 的 `vs_1_0` 由 **CPU** 顶点程序兑现 —— 后端仍然只收屏幕空间三角形）
//   ★ "10-tap" 只属于 ZoomBlur —— SlideBlur 的循环上下界是 **-10..+10（含）⇒ 21 个 tap**。
//     ⛔ 不要按"两个都是 10 tap"写（那是把 P3 任务描述当源文本用；源文本是唯一真源）。
enum class GpuEffectProgram : std::uint8_t {
    FadePs20 = 0,
    FadePs10 = 1,
    SlideBlurPs20 = 2,
    ZoomBlurPs20 = 3,
    SnowPs10 = 4,
};

// 常量表容量：一张 32 B 的 uniform（= 2 × float4）。
//   · Fade 只用 [0]（Alpha）；
//   · 203 用满 [0..5]：`Length / Width / Height / Angle / CenterU / CenterV`
//     （按 FX 源文本的**声明顺序** ⇒ `c[0] = (Length, Width, Height, Angle)`、`c[1] = (CenterU, CenterV, 0, 0)`）。
//   · 202 只用 [0..3]（`float4 Color` ⇒ 4 个 float；`Proj` 是 `float4x4`，在 CPU 顶点程序里用掉，
//     ⛔ **不进 uniform** —— push constant 只有 32 B）。
// ⛔ 上限写死是因为后端 push constant 的大小必须是常量。
constexpr std::uint32_t kGpuEffectMaxConstants = 8;
constexpr std::uint32_t kGpuEffectMaxTextures = 2;

// 一张 effect 纹理 + 它采哪一套纹理坐标。
struct GpuEffectTexture {
    GpuRasterTexture texture;
    // 0 = `GpuRasterVertex` 的 (u,v)（= FVF 的 TEXCOORD0）；1 = `rasterQueueEffect` 的 `uv1`
    // （= TEXCOORD1）。★ 两套坐标是**独立输入**（源文本里 `Tex0UV : TEXCOORD0` /
    // `Tex1UV : TEXCOORD1`）⇒ 即使引擎把两套填成同一对值也**不在这里合并**。
    std::uint32_t uv_set = 0;
};

// 一条 effect draw 的全部后端输入。
struct GpuEffectDraw {
    GpuEffectProgram program = GpuEffectProgram::FadePs20;
    // 按 FX 源文本的**声明顺序**排的常量（`float Alpha;` ⇒ [0]）；没被 `SetFloat` 设过 ⇒ 0
    // （D3D 的 uniform 初值就是 0）。
    float constants[kGpuEffectMaxConstants] = {};
    std::uint32_t constant_count = 0;
    GpuEffectTexture textures[kGpuEffectMaxTextures];
    std::uint32_t texture_count = 0;
};

// 可选后端能力："每 draw 一个 shader + 常量表 + 最多 2 张纹理"。
class GpuEffectRaster {
public:
    virtual ~GpuEffectRaster() = default;
    // `uv1` = 第二套纹理坐标（与 `vertices` 同序，`count * 2` 个 float；nullptr = 没有第二套）。
    // ⛔ 与 `rasterQueue` 一样：**只入队**，不在这里提交；flush 才录命令。
    virtual bool rasterQueueEffect(const GpuRasterVertex* vertices, std::size_t count, const float* uv1,
                                  const GpuEffectDraw& effect, const GpuRasterState& state,
                                  std::string* err) = 0;
};

// ---------------------------------------------------------------------------------------
// 交付入口（**唯一**给 d3d9_pipeline 用的两个函数；实现见 `d3d9_gpu.cpp`）
// ---------------------------------------------------------------------------------------
// ★ 为什么要两个自由函数、而不是直接 `dynamic_cast`：
//   生产路径上 `Machine::gpuRaster()` 拿到的是 **`SdlWindow`（一个 GpuRaster 转发包装）**，
//   不是 `SdlGpuRaster` 本身（见 `present_sdl.cpp` 的 `SdlWindow::gpuRaster()`）⇒ 对包装做
//   `dynamic_cast<GpuEffectRaster*>` 永远失败。这两个函数负责"从包装找到真正的 effect 后端"。
//   ★ 后端的生命周期：本线**同一时刻只有一个** SDL_GPU 后端（`d3d9_gpu.cpp` 的 `g_stats` 同一口径）
//     ⇒ 实现里用"当前活着的那个"作为包装的后备；`SdlWindow` 以外的包装**不**走后备（⛔ 不乱投）。
bool gpuSupportsEffectDraw(lift::GpuRaster* gpu);
bool gpuQueueEffectDraw(lift::GpuRaster* gpu, const GpuRasterVertex* vertices, std::size_t count,
                        const float* uv1, const GpuEffectDraw& effect, const GpuRasterState& state,
                        std::string* err);

}  // namespace lift
