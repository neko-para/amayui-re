#include "runtime/dshow/guest_invoke.h"

#include <cstdio>
#include <utility>
#include <vector>

#include "runtime/memory.h"
#include "runtime/x86.h"

#include "runtime/dshow/dshow.h"

namespace lift::dshow {
namespace {

std::string hexEa(std::uint32_t v) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "0x%08X", v);
    return buf;
}

}  // namespace

std::uint32_t vtableSlot(X86& cpu, std::uint32_t obj, std::uint32_t off, std::string* err) {
    if (!cpu.mem) {
        if (err) *err = "cpu.mem 为空";
        return 0;
    }
    if ((off & 3u) != 0) {
        if (err) *err = "vtable 偏移 " + hexEa(off) + " 不是 4 的倍数";
        return 0;
    }
    if (obj == 0) {
        if (err) *err = "对象 EA 为 0（没取到接口指针？）";
        return 0;
    }
    if (!cpu.mem->committed(obj, 4u)) {
        if (err) *err = "对象头未提交";
        return 0;
    }
    const std::uint32_t vtable = cpu.mem->rd32(obj);
    if (vtable == 0 || !cpu.mem->committed(vtable, off + 4u)) {
        if (err) *err = "vtable 指针为 0 或槽未提交";
        return 0;
    }
    const std::uint32_t fn = cpu.mem->rd32(vtable + off);
    if (fn == 0) {
        if (err) *err = "vtable 槽为 0（纯虚 / 没填）";
        return 0;
    }
    return fn;
}

bool invokeGuest(X86& cpu, std::uint32_t this_ea, std::uint32_t vtbl_off,
                 std::initializer_list<std::uint32_t> args, std::uint32_t cleanup_bytes, const char* api,
                 std::uint32_t* result, GuestContinuation tail, std::string* err) {
    const char* name = api ? api : "dshow guest call";
    if (!cpu.calls) {
        if (err) *err = std::string(name) + ": cpu.calls 为空（Machine 没装配 GuestCallStack）";
        return false;
    }
    std::string slot_err;
    const std::uint32_t fn = vtableSlot(cpu, this_ea, vtbl_off, &slot_err);
    if (fn == 0) {
        if (err) {
            *err = std::string(name) + ": this=" + hexEa(this_ea) + " vtbl+" + hexEa(vtbl_off) +
                   " 读不到实现（" + slot_err + "）";
        }
        return false;
    }
    // ★ 结果 / 收尾都在续延里：`resume()` 会把 callee 的 EAX 交到这里（doc §5-3）。
    GuestContinuation cont;
    if (result != nullptr || tail) {
        cont = [result, tail](X86& c, std::uint32_t callback_result, std::uint32_t* shim_result) -> bool {
            if (result != nullptr) *result = callback_result;  // ←「在续延里取 EAX」
            if (tail) return tail(c, callback_result, shim_result);
            *shim_result = kSOk;
            return true;
        };
    } else {
        cont = [](X86&, std::uint32_t, std::uint32_t* shim_result) -> bool {
            *shim_result = kSOk;
            return true;
        };
    }
    const std::vector<std::uint32_t> stack_args(args);
    // `callee_ecx = this_ea`：COM 的 this 在**栈上**（见头注释），ECX 只是顺手对上 thiscall。
    return cpu.calls->start(cpu, fn, stack_args, cleanup_bytes, name, std::move(cont), err, this_ea);
}

}  // namespace lift::dshow
