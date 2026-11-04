// emulator-lift · GDI32 启动最小件（P0）
//
// ★ 启动路径只要 `GetStockObject(BLACK_BRUSH)` 填 WNDCLASSEX.hbrBackground；
//   它不需要真的画笔 / 位图。返回一个确定性、非零的 guest 对象句柄（不透明的 token）。
// ★ P2 的 GDI 对象表会接管这些句柄；届时本文件的 stock object 可以迁进对象表。
#include <cstdint>

#include "runtime/x86.h"
#include "shims/register.h"

namespace lift {
namespace {

void shimGetStockObject(X86& cpu, ShimCall& call) {
    (void)cpu;
    const std::int32_t index = call.i32(0);
    // 0..17 是 Win32 的 stock object 索引；本作启动只用 4 (BLACK_BRUSH)。
    call.result = 0x00013000u + static_cast<std::uint32_t>(index & 0x1F);
    shimLog("gdi32", "GetStockObject", "index=%d ⇒ 假 stock object 0x%08X（P0；P2 接对象表）", index,
            call.result);
}

}  // namespace

void registerGdiBootShims(ShimRegistry& s) {
    s.add("gdi32", "GetStockObject", Conv::Stdcall, 1, &shimGetStockObject);
}

}  // namespace lift
