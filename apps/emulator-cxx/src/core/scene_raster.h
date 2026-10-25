#pragma once

#include "core/vm.h"

#include <cstdint>
#include <string>

namespace core {

// 本步派生色（`CORE-TIMING-WINDOW` 的四档）：无窗 / 延迟内 / 未推进 ⇒ 当前颜色；
// 推进中 ⇒ 当前颜色与终色之间插值；到期（或「强制完成」）⇒ 终色。
// 这是**派生量**，不写回记录字段；到期写回由 `Machine::advance_scene` 做。
std::uint32_t derive_color(const DrawItem& item, std::uint32_t now_ms, bool forced);
std::uint32_t derive_color(const MeshRecord& mesh, std::uint32_t now_ms, bool forced);

// 软件合成：把当前场景的绘制项（按 key 升序）画到 RGBA8 画布。
// 这是“core 状态 -> 可验证像素”的确定层；平台上传 / 窗口显示在宿主侧。
Bytes rasterize_scene(Machine& machine, std::uint32_t width, std::uint32_t height);

// 32 位 BMP（top-down），供无窗口路径导出像素。
void save_bmp_rgba(const std::string& path, const Bytes& rgba, std::uint32_t width, std::uint32_t height);

}  // namespace core
