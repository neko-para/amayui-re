// emulator-lift · IDirect3DSurface9 对象的唯一建法（纹理表面 / 后缓冲共用）
//
// ★ 为什么放在 shims/ 而不是 runtime/：surface 的 COM 方法表是 d3dx9.cpp 的静态表
//   （它与 IDirect3DTexture9 同属纹理簇）；device 的 GetBackBuffer 也需要建同一类对象
//   ⇒ 两边都调这一个小入口，⛔ 不复制方法表、不各自 createObject。
#pragma once

#include <cstdint>

namespace lift {

struct X86;

// 在 guest 内存里建 IDirect3DSurface9；`user` = 表面状态块（纹理的 'TEX1' 或后缓冲的 'D9D1'）。
// 返回对象 EA；0 = 失败（已打 com 日志）。
std::uint32_t createD3dxSurfaceObject(X86& cpu, std::uint32_t user);

}  // namespace lift
