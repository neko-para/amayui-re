// emulator-lift · GDI32 启动最小件（P0）
//
// ★ 启动路径只要 `GetStockObject(BLACK_BRUSH)` 填 WNDCLASSEX.hbrBackground；
//   它不需要真的画笔 / 位图。返回一个确定性、非零的 guest 对象句柄（不透明的 token）。
// ★ P2 已接管：`GetStockObject` 与全部 GDI 对象表迁到 `gdi32_object.cpp`（令牌值不变：
//   0x00013000 + index）。本文件的注册函数保留为空壳，只为不改 `register.cpp` 的装配顺序。
#include <cstdint>

#include "shims/register.h"

namespace lift {

void registerGdiBootShims(ShimRegistry& s) {
    (void)s;  // P2：见 gdi32_object.cpp（registerGdiObjectShims）
}

}  // namespace lift

