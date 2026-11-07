// emulator-lift · host→guest 调用的薄封装（vtable 槽 + thiscall/stdcall + 续延）
//
// ★ 为什么不是「一个函数同步拿 EAX」：本模拟器的 guest 调用是**协作式**的
//   （`src/runtime/guest_call.h`）：`GuestCallStack::start()` 只铺帧、把 eip 交给 guest；
//   控制权回到宿主是在 guest `ret n` 落到续延桩（`kContinuationEa`）之后。
//   ⇒ 结果只能**在续延里**取（doc §5-3：「必须在续延里取 EAX 并恢复」）。
//   ⛔ 不要在续延里等定时器 / 再跑一个宿主循环（§5-3 的另一半）。
//
// ★ 调用约定：这些是 COM 接口方法 = **stdcall，并且 `this` 是第 0 个栈参数**
//   （`.lst` 逐字：`CBasePin::ReceiveConnection` = `sub_4D0D00` `retn 0Ch`、
//    `IMemInputPin::Receive` = `sub_4D4730` `int __stdcall(int,int)`、
//    `CBaseFilter::Run` = `sub_4D5980` `retn 0Ch`、`CBaseFilter::Pause` = `sub_4D5860` `retn 4`）。
//   ⇒ `args[0] = this`，其余按**声明顺序**（不是压栈顺序）。
//   `callee_ecx` 仍设成 `this`：无害（stdcall 的 callee 不依赖 ECX），且万一某个槽是
//   C++ 内部虚函数（`__thiscall`）也刚好对上；`resume()` 会把调用方的 ECX 还回去。
#pragma once

#include <cstdint>
#include <initializer_list>
#include <string>

#include "runtime/guest_call.h"

namespace lift {
struct X86;
}

namespace lift::dshow {

// 读 guest COM 对象第 `off` 个 vtable 槽的函数地址（`obj+0` = vtable EA，见 `runtime/com.h`）。
// 返回 0 + `*err` 非空 ⇒ 读不到（对象头 / vtable / 槽未提交，或槽为 0）。
std::uint32_t vtableSlot(X86 &cpu, std::uint32_t obj, std::uint32_t off, std::string *err);

// 发起一次 guest 虚函数调用。
//   `cleanup_bytes` = 调用点原本要清理的栈字节（= 4 + argc*4；见 guest_call.h）。
//     ⛔ 不能猜：它来自发起这次调用的 shim（`4 + call.stdcall_bytes`）。
//   `api` 必须是**字符串字面量**（`GuestCallStack` 只存指针，不复制）。
//   `result` 非空 ⇒ **在续延里**把 callee 的 EAX 写进去（调用返回时它还没被写）。
//   `tail` 非空 ⇒ 摘到结果之后再跑调用方的续延（可 `return false` 再铺下一次调用）。
// 返回 false + `*err` ⇒ 没发起（槽读不到 / 栈不够 / 没有 GuestCallStack）。
bool invokeGuest(X86 &cpu, std::uint32_t this_ea, std::uint32_t vtbl_off,
                 std::initializer_list<std::uint32_t> args, std::uint32_t cleanup_bytes, const char *api,
                 std::uint32_t *result, GuestContinuation tail, std::string *err);

} // namespace lift::dshow
