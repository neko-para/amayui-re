// emulator-lift · 嵌套 guest 回调栈（P0 / G3）
//
// ★ 为什么需要：shim 是宿主函数，不能在自己的 C++ 栈里直接调 guest 代码。
//   消息泵（DispatchMessage / SendMessage / CreateWindowEx 的 WM_CREATE）以及
//   DDraw EnumDisplayModes / GDI EnumFontFamiliesExA 都需要“shim → guest 回调 → 回到 shim 继续”。
// ★ 机制：
//   ① shim 把 guest 栈铺成回调帧（[esp] = 续延桩 EA，[esp+4..] = 参数），eip 指向 guest 回调，
//      置 `ShimCall::take_control` ⇒ invoke() 不再改写 esp/eip；
//   ② 回调 `ret n` 落到续延桩（`kContinuationEa`，不是普通块）；dispatcher 认出它，
//      调 `resume()`：取 EAX、恢复 shim 的 esp/eip、执行 shim 的宿主续延；
//   ③ 续延可以再次 `start()`（嵌套 SendMessage / 多回调枚举），形成 LIFO 栈。
// ★ **多回调枚举**（P1 的 DDraw `EnumDisplayModes`）：续延在 `resume()` 里**再调一次 `start()`**
//   并返回 false ⇒ 控制权又交给下一个 guest 回调；该回调返回后会**再进同一个续延**
//   （收到的 `callback_result` 是这一次的 EAX），直到续延返回 true 收尾。
//   ⛔ 返回 false 却**没有** start() 新回调 = 编程错误（resume 会明确报错，⛔ 不静默）。
//   子回调帧（`nested`）的返回点是**父帧的续延**，不是 guest 的调用点。
// ★ 只支持 **stdcall 回调**（Win32 的 WndProc / COM 方法 / 枚举回调都是 stdcall）；
//   回调的 `ret n` 必须由 guest 自己弹参，`resume()` 只负责回到 shim 的调用点。
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace lift {

struct X86;

// 返回值：true = 这个 shim 调用已经完成（*shim_result 是它的 EAX）；
//         false = 续延自己又 `start()` 了一个 guest 回调（多回调枚举），dispatcher 继续跑 guest。
using GuestContinuation = std::function<bool(X86&, std::uint32_t callback_result, std::uint32_t* shim_result)>;

class GuestCallStack {
public:
    // 续延桩地址：与 winmm 定时器（0x7FFEF004）错开，且不在 shim 桩页里。
    static constexpr std::uint32_t kContinuationEa = 0x7FFEF008u;

    // 开始一次 guest 回调。
    // `cleanup_bytes` = 原 shim 返回时要抬的栈字节（stdcall 的 4 + argc*4；由调用方从 ShimCall 取）。
    // `callee_ecx` = 进入 callee 之前要写进 ECX 的值（thiscall 的 `this`）；0 = 不动 ECX。
    // 返回 false + err ⇒ 栈不够 / 已有帧状态坏。
    // ★ 在**续延里**调用 = 多回调枚举的下一次回调：压一个 `nested` 子帧（它的返回点是父帧续延）。
    bool start(X86& cpu, std::uint32_t fn, const std::vector<std::uint32_t>& args,
               std::uint32_t cleanup_bytes, const char* api, GuestContinuation cont, std::string* err,
               std::uint32_t callee_ecx = 0);

    // dispatcher 在 eip == kContinuationEa 时调用。
    // 返回 false + err ⇒ 没有活动帧 / 栈状态坏（调用方应停下并报错）。
    bool resume(X86& cpu, std::string* err);

    bool active() const { return !frames_.empty(); }
    std::size_t depth() const { return frames_.size(); }
    void reset() { frames_.clear(); }

    // 诊断：当前帧的 API 名（空 = 没有）
    const char* currentApi() const { return frames_.empty() ? nullptr : frames_.back().api; }
    bool inContinuation() const { return in_continuation_; }

private:
    struct Frame {
        std::uint32_t ret_ea = 0;       // 原 shim 的返回地址（`nested` 帧不用它）
        std::uint32_t saved_esp = 0;    // 原 shim 入口的 esp（指向 ret_ea）
        std::uint32_t cleanup_bytes = 0;// 原 shim 完成后要抬的栈字节
        std::uint32_t fn = 0;           // guest 回调
        const char* api = nullptr;      // 诊断（字符串字面量；保持生命周期）
        GuestContinuation cont;         // 宿主续延
        // ★ true = 这个帧是在**续延里**压的（多回调枚举的第 2..N 次回调）：
        //   它返回时先把结果交回父帧的同一份续延，而不是恢复 guest 的调用点。
        bool nested = false;
        // ★ thiscall 的 `this`（ECX）**是帧的一部分**，不是"铺完帧顺手写一下"：
        //   铺帧之后本类立刻把控制权交回 dispatcher（native 实现返回 false ⇒ suspended），
        //   下一个 block 边界上可能插入一个 **winmm 定时器回调**（真机上它跑在另一个线程）。
        //   只写一次 ECX 就会被那个回调改坏 ⇒ 这里把"callee 该看到的 ECX"记在帧上，
        //   `start()` 在交控制权之前写进去；`resume()` 再把**调用方**的 ECX 还给续延。
        std::uint32_t caller_ecx = 0;  // start() 时的 ECX（宿主 / native 调用的上下文）
        std::uint32_t callee_ecx = 0;  // 0 = 不动 ECX；否则 = 进入 callee 前的 ECX（thiscall 的 this）
    };
    std::vector<Frame> frames_;
    bool in_continuation_ = false;
};

}  // namespace lift
