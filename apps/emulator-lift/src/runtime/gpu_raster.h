// emulator-lift · GPU 光栅化后端接口（SDL-free）
//
// ★ 只做 **光栅化**：顶点已经由 CPU 做完 T&L / 裁剪 / 屏幕映射（d3d9_pipeline.cpp），
//   这里接收屏幕空间三角形 + 状态，由宿主 GPU 画到 guest 后缓冲的镜像纹理。
// ★ 缺省 CPU 后端完全不变；`--raster gpu` 才启用本接口。
// ★ 后端实现负责：镜像纹理、绘制、把像素读回 guest（Present / 回退时）。
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
    bool blend = false;  // SRCALPHA / INVSRCALPHA
    bool depth_test = true;
    bool depth_write = true;
    std::uint32_t depth_func = 4;  // D3DCMP_LESSEQUAL
};

struct GpuRasterTexture {
    const std::uint8_t* pixels = nullptr;
    std::uint32_t pitch = 0;
    int width = 0;
    int height = 0;
    SurfaceFormat format;
    const void* key = nullptr;  // guest 纹理像素指针（缓存键）
};

class GpuRaster {
public:
    virtual ~GpuRaster() = default;

    virtual bool rasterActive() const = 0;
    virtual const char* rasterName() const = 0;
    // D3DXSprite 纹理四边形是否也走 GPU（phase 1 可单独关掉做 A/B）。
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
    // 带纹理的四边形（D3DXSprite：NEAREST + 顶点色调制）。
    virtual bool rasterDrawTextured(const GpuRasterVertex* vertices, std::size_t count,
                                    const GpuRasterTexture& texture, const GpuRasterState& state,
                                    std::string* err) = 0;
    // GPU 镜像里是否有尚未读回 guest 的绘制。
    virtual bool rasterDirty() const = 0;
};

}  // namespace lift
