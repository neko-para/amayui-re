// emulator-lift · PNG 解码的实现（libpng 简化 API：`png_image` + `*_from_memory` / `_finish_read`）
//
// ★ 为什么用简化 API 而不是 `png_create_read_struct`：本线只需要"一份 RGB(A) 字节"，
//   不需要逐块回调 / 自定义 chunk / 渐进读。简化 API 把这四件事一次做完：
//   ① 读头（宽高 / 色型）；② 把任意合法色型·位深·隔行统一转成请求的格式；
//   ③ 解压 + 反滤波；④ 一次性写进调用方的缓冲。
// ★ 通道序：请求 `PNG_FORMAT_RGBA` ⇒ 输出是 **R,G,B,A**（PNG 原生序）。
//   guest 的 A8R8G8B8 在内存里是 **B,G,R,A** ⇒ 换序写在 `shims/d3dx9.cpp` 的上传步骤里，
//   ⛔ 本模块不负责 D3D 的像素布局（否则同一件事会有两份换算）。
// ★ 失败面（返回 false + why）：不是 PNG / 头坏 / 截断 / CRC 坏 / 宽高 0 / 超上限 / 缓冲分配失败。
//   ⛔ 不抛（`std::bad_alloc` 也接住）、⛔ 不 trap、⛔ 不碰 guest 内存。
// ★ 生命周期：`png_image_begin_read_from_memory` **不拷输入字节**（只存指针），
//   直到 `png_image_finish_read` 返回之前都可能在读它 ⇒ 调用方必须保证 `bytes` 活到本函数返回。
//   调用方**不需要**自己先拷一份：`shims/d3dx9.cpp` 直接传 guest 的平坦映射（先整段 `committed`，
//   解码同步、期间不跑 guest 代码、`base_` 进程内不变）。`png_image_free` 在每条出口都调一次。
#include "runtime/png_decode.h"

#include <cstdio>
#include <cstring>
#include <exception>
#include <limits>
#include <utility>

#include <png.h>

namespace lift {
namespace png {
namespace {

// libpng 的失败文本（`png_image::message` 是它自己填的 64 B；空 ⇒ 明说"无消息"，⛔ 不猜）。
std::string libpngFailure(const png_image& image, const char* stage) {
    char buf[192];
    std::snprintf(buf, sizeof(buf), "%s失败（libpng: %s）", stage,
                  image.message[0] != '\0' ? image.message : "无消息");
    return std::string(buf);
}

}  // namespace

bool decodeRgba8(const std::uint8_t* bytes, std::size_t size, std::uint32_t max_side, RgbaImage* out,
                 std::string* why) {
    if (out != nullptr) *out = RgbaImage{};
    auto bad = [why](std::string text) {
        if (why != nullptr) *why = std::move(text);
        return false;
    };
    if (bytes == nullptr || size == 0) return bad("输入为空（空指针 / 0 字节）");

    png_image image;
    std::memset(&image, 0, sizeof(image));
    // ★ 文档的 REQUIRED 两条：`version` = PNG_IMAGE_VERSION、`opaque` = NULL（memset 已给）。
    //   漏了 version 的真实后果是"行为未定义 / 可能崩"，不是"返回失败"。
    image.version = PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_memory(&image, bytes, size)) {
        std::string text = libpngFailure(image, "读 PNG 头");
        png_image_free(&image);  // 失败路径也调：opaque 非空时它就是唯一的释放口
        return bad(std::move(text));
    }
    // ★ begin_read 成功之后 libpng 已持有 opaque ⇒ 后面每条出口都必须 png_image_free。
    //   请求输出格式：8-bit RGBA（不带 PNG_FORMAT_FLAG_LINEAR ⇒ 每通道 1 字节）。
    image.format = PNG_FORMAT_RGBA;

    const std::uint32_t width = image.width;
    const std::uint32_t height = image.height;
    if (width == 0 || height == 0) {
        png_image_free(&image);
        return bad("PNG 的宽/高为 0");
    }
    // ★ 宽高来自**不可信输入**：超上限在这里就拒（返回失败 = 调用方 E_FAIL 可继续），
    //   ⛔ 不要交给 `createTextureState` —— 它把"尺寸非法"当**状态崩坏**（loud trap），
    //   而一张 5000×5000 的坏 PNG 不该让机器停。
    if (max_side != 0 && (width > max_side || height > max_side)) {
        char buf[192];
        std::snprintf(buf, sizeof(buf), "PNG %ux%u 超过本线上限 %u", width, height, max_side);
        png_image_free(&image);
        return bad(buf);
    }
    const std::uint64_t pixels = static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height);
    if (pixels > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max() / 4u)) {
        png_image_free(&image);
        return bad("PNG 尺寸的字节数溢出宿主 size_t");
    }

    RgbaImage result;
    result.width = width;
    result.height = height;
    try {
        result.rgba.resize(static_cast<std::size_t>(pixels) * 4u, 0u);
    } catch (const std::exception& e) {
        // ⛔ shim 调用链没有 catch ⇒ 这里不接住就是整个进程 terminate。
        png_image_free(&image);
        return bad(std::string("分配 RGBA 缓冲失败：") + e.what());
    }
    // `background = NULL` ⇒ **不**与背景色合成（保留 alpha 通道，Live2D 图集就是 RGBA）；
    // `row_stride = 0` ⇒ 用 PNG_IMAGE_ROW_STRIDE(image)（自然行距，正好 width*4）。
    if (!png_image_finish_read(&image, nullptr, result.rgba.data(), 0, nullptr)) {
        std::string text = libpngFailure(image, "解码 PNG 像素");
        png_image_free(&image);
        return bad(std::move(text));
    }
    png_image_free(&image);
    if (out != nullptr) *out = std::move(result);
    return true;
}

}  // namespace png
}  // namespace lift
