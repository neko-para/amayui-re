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
//         false = 又启动了一次 guest 回调（栈上多了一帧），dispatcher 继续跑 guest。
using GuestContinuation = std::function<bool(X86&, std::uint32_t callback_result, std::uint32_t* shim_result)>;

class GuestCallStack {
public:
    // 续延桩地址：与 winmm 定时器（0x7FFEF004）错开，且不在 shim 桩页里。
    static constexpr std::uint32_t kContinuationEa = 0x7FFEF008u;

    // 开始一次 guest 回调。
    // `cleanup_bytes` = 原 shim 返回时要抬的栈字节（stdcall 的 4 + argc*4；由调用方从 ShimCall 取）。
    // 返回 false + err ⇒ 栈不够 / 已有帧状态坏。
    bool start(X86& cpu, std::uint32_t fn, const std::vector<std::uint32_t>& args,
               std::uint32_t cleanup_bytes, const char* api, GuestContinuation cont, std::string* err);

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
        std::uint32_t ret_ea = 0;       // 原 shim 的返回地址
        std::uint32_t saved_esp = 0;    // 原 shim 入口的 esp（指向 ret_ea）
        std::uint32_t cleanup_bytes = 0;// 原 shim 完成后要抬的栈字节
        std::uint32_t fn = 0;           // guest 回调
        const char* api = nullptr;      // 诊断（字符串字面量；保持生命周期）
        GuestContinuation cont;         // 宿主续延
    };
    std::vector<Frame> frames_;
    // ★ P0 限制：continuation 里**不能**再 start 一次回调（多回调枚举要用 P1 的
    //   “从 continuation 继续”机制；见 plan P1 EnumDisplayModes）。这里显式拒绝，
    //   ⛔ 不让它静默压一个 ret 指向 guest 的错帧。
    bool in_continuation_ = false;
};

}  // namespace lift
