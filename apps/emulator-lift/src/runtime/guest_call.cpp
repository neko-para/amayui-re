#include "runtime/guest_call.h"

#include <cstdio>

#include "runtime/memory.h"
#include "runtime/x86.h"

namespace lift {

bool GuestCallStack::start(X86& cpu, std::uint32_t fn, const std::vector<std::uint32_t>& args,
                           std::uint32_t cleanup_bytes, const char* api, GuestContinuation cont,
                           std::string* err) {
    if (in_continuation_) {
        if (err) *err = std::string(api ? api : "guest-call") +
                        ": continuation 里再次 start 未实现（P0 只支持单回调；多回调等 P1）";
        return false;
    }
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
    Frame& f = frames_.back();
    const std::uint32_t callback_result = cpu.eax();
    // 回调按 stdcall 弹完参后应回到 saved_esp；这里强制归位，防止 guest 畸形 ret 把栈留歪。
    cpu.esp() = f.saved_esp;

    std::uint32_t shim_result = 0;
    in_continuation_ = true;
    const bool done = f.cont(cpu, callback_result, &shim_result);
    in_continuation_ = false;
    if (!done) {
        // P0 的 start() 会拒绝在 continuation 里再 start；走到这里说明续延自己违反了约定。
        if (err) *err = std::string(f.api ? f.api : "guest-call") + ": continuation 返回 false（未实现多回调）";
        return false;
    }

    const Frame done_frame = f;
    frames_.pop_back();
    cpu.esp() = done_frame.saved_esp + done_frame.cleanup_bytes;
    cpu.eip = done_frame.ret_ea;
    cpu.eax() = shim_result;
    return true;
}

}  // namespace lift
