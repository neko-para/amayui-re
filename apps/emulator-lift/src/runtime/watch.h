// emulator-lift · 调试观察口：观察一个 guest DWORD 何时被改写（默认关、零成本）
//
// ★ 为什么要有它：真机里"某个宿主 COM 对象的对象头在 guest 侧被改写"这种问题，靠日志无法定位写者
//   —— 因为写者可能根本不在我们的 shim 里。这里只做**观察**：`HostObjects` 把样本对象的 guest EA
//   登记进来，`Machine::run` 在环境变量 `LIFT_WATCH_SAMPLE=1` 时**每个 block 入口**读一次该 DWORD，
//   值变了就打印 `[watch] ea old new at block eip (prev block)`，然后继续跑。
//   语义零改动（不读环境变量时连调用都不会发生 —— 调用方先查 env）。
#pragma once

#include <cstdint>

namespace lift {

inline std::uint32_t& watchSampleEaRef() {
    static std::uint32_t ea = 0;
    return ea;
}

// 登记要观察的 guest DWORD（默认是 IMediaSample 对象头 `[0]` = vtable 指针；
// `GuestCallStack::start` 在 `LIFT_WATCH_SAMPLE=1` 时会把它改成**本次回调帧的返回地址槽**）。
inline void setWatchSampleEa(std::uint32_t ea) { watchSampleEaRef() = ea; }
inline std::uint32_t watchSampleEa() { return watchSampleEaRef(); }

// 别名：语义就是"当前观察的 EA"，不限于样本对象（watch 的落点由调用方决定）。
inline void setWatchEa(std::uint32_t ea) { watchSampleEaRef() = ea; }
inline std::uint32_t watchEa() { return watchSampleEaRef(); }

}  // namespace lift
