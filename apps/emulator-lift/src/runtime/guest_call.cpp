#include "runtime/guest_call.h"

#include <cstdio>
#include <utility>

#include "runtime/memory.h"
#include "runtime/x86.h"

namespace lift {

bool GuestCallStack::start(X86& cpu, std::uint32_t fn, const std::vector<std::uint32_t>& args,
                           std::uint32_t cleanup_bytes, const char* api, GuestContinuation cont,
                           std::string* err) {
    if (!cpu.mem) {
        if (err) *err = std::string(api ? api : "guest-call") + ": cpu.mem 为空";
        return false;
    }
    if (fn == 0) {
        if (err) *err = std::string(api ? api : "guest-call") + ": 回调地址为 0";
        return false;
    }
    // 回调帧：[esp] = 续延桩，[esp+4..] = args。先确认整段在已提交栈里。
    const std::uint32_t need = 4u + static_cast<std::uint32_t>(args.size()) * 4u;
    if (cpu.esp() < need) {
        if (err) *err = std::string(api ? api : "guest-call") + ": esp 太小，铺不下回调帧";
        return false;
    }
    const std::uint32_t frame_esp = cpu.esp() - need;
    if (!cpu.mem->committed(frame_esp, need)) {
        if (err) *err = std::string(api ? api : "guest-call") + ": 回调帧落在未提交栈上";
        return false;
    }

    Frame f;
    f.ret_ea = cpu.mem->rd32(cpu.esp());
    f.saved_esp = cpu.esp();
    f.cleanup_bytes = cleanup_bytes;
    f.fn = fn;
    f.api = api;
    f.cont = std::move(cont);
    // ★ 在续延里 start = 多回调枚举的第 2..N 次回调：子帧的“返回点”是父帧的续延，
    //   不是 guest 的调用点 ⇒ 打上 nested（resume() 会先把结果交回父帧续延）。
    f.nested = in_continuation_;

    frames_.push_back(std::move(f));

    cpu.mem->wr32(frame_esp, kContinuationEa);
    for (std::size_t i = 0; i < args.size(); ++i)
        cpu.mem->wr32(frame_esp + 4u + static_cast<std::uint32_t>(i) * 4u, args[i]);
    cpu.esp() = frame_esp;
    cpu.eip = fn;
    return true;
}

bool GuestCallStack::resume(X86& cpu, std::string* err) {
    if (frames_.empty()) {
        if (err) *err = "guest-call 续延桩被调到，但没有活动帧";
        return false;
    }
    const std::uint32_t callback_result = cpu.eax();
    // ★ 子帧（nested）返回：弹出它，把这次结果交回**父帧的同一份续延**（多回调枚举）。
    //   子帧的 saved_esp == 父帧的 saved_esp，所以恢复到那里就够。
    if (frames_.back().nested) {
        const Frame child = frames_.back();
        frames_.pop_back();
        if (frames_.empty()) {
            if (err) *err = std::string(child.api ? child.api : "guest-call") +
                            ": 子回调帧没有父帧（帧栈被破坏）";
            return false;
        }
        cpu.esp() = child.saved_esp;
    }

    // ★ 复制一份再调 cont：续延里可能 `start()`（它会 push_back ⇒ vector 扩容会让引用失效）。
    const Frame f = frames_.back();
    const std::size_t depth_before = frames_.size();
    // 回调按 stdcall 弹完参后应回到 saved_esp；这里强制归位，防止 guest 畸形 ret 把栈留歪。
    cpu.esp() = f.saved_esp;

    std::uint32_t shim_result = 0;
    in_continuation_ = true;
    const bool done = f.cont(cpu, callback_result, &shim_result);
    in_continuation_ = false;
    if (!done) {
        // 多回调枚举：续延必须自己 start() 了下一个回调；否则就是编程错误。
        if (frames_.size() <= depth_before) {
            if (err) *err = std::string(f.api ? f.api : "guest-call") +
                            ": continuation 返回 false，但没有 start() 下一个 guest 回调";
            return false;
        }
        return true;  // 控制权已经交给下一个 guest 回调（start 改了 eip/esp）
    }
    if (frames_.size() != depth_before) {
        // 续延既 start() 了新回调又说“完成了” ⇒ 状态自相矛盾
        if (err) *err = std::string(f.api ? f.api : "guest-call") +
                        ": continuation 既 start() 了新回调又返回 true";
        return false;
    }

    frames_.pop_back();
    cpu.esp() = f.saved_esp + f.cleanup_bytes;
    cpu.eip = f.ret_ea;
    cpu.eax() = shim_result;
    return true;
}

}  // namespace lift
