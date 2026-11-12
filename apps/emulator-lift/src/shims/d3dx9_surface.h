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
// ★ `owner` = 这个表面**代表**的对象（纹理表面 ⇒ 父纹理对象 EA；后缓冲表面 ⇒ 0）。
//   它被 `ComRegistry::ownerOf` 读走：`SetRenderTarget` 用它做语义 addRef/release
//   （REQ-01M6X4N7C0PAF6XT7EAC172V85）。
// ★ **`owner != 0` ⇒ 本函数替父纹理持一份引用**（REQ-01M6ZZZ6PEVSFB93C6VJPKC04C，A 方案）：
//   建对象**之前**先 `addRef(owner)`，surface 的 refcount 到 0 时由 `ComRegistry::releaseImpl`
//   （`owns_owner`）成对还回去。为什么：像素块 + `'TEX1'` 状态块的唯一回收入口是**纹理对象**
//   refcount 到 0 时的 `destroyTextureState` ⇒ surface 不持纹理的话，guest 先 Release 纹理、
//   再用 surface 的 `LockRect` / `GetDesc` 就会读到已 free / 被复用的状态块。
//   ★ 还引用的地点是 `releaseImpl` 这一个收敛点（内部裸 release 也覆盖），⛔ 不是影子回收。
//   `owner == 0`（后缓冲表面）⇒ 不加引用、对象布局与 refcount 一字不改。
// 返回对象 EA；0 = 失败（已打 com / d3dx 日志；失败时刚加的那份引用已还回去）。
// ★ `on_zero` = 对象 refcount 到 0 时的回收钩子（`ComRegistry::createObject` 的 on_zero）：
//   纹理表面 / 后缓冲表面传 nullptr（状态块 + 像素块的寿命挂在父纹理 / 设备上）；
//   **独立的 offscreen 表面**（`CreateOffscreenPlainSurface`）必须传 `d3d9::destroyTextureState`
//   —— 它没有父对象，状态块 + arena 只能随自己的 refcount 回收。
std::uint32_t createD3dxSurfaceObject(X86& cpu, std::uint32_t user, std::uint32_t owner = 0,
                                      void (*on_zero)(X86&, std::uint32_t) = nullptr);

}  // namespace lift
