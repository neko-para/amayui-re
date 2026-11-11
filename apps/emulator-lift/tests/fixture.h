// emulator-lift · 夹具的公共小工具（pure 档；只给 tests/*.cpp 用，不进 runtime）
//
// ★ 为什么要有它：本轮新增的 4 个族夹具都要"guest 侧自造一段机器码 → 调到 shim 桩地址 →
//   看返回值 / 看 guest 内存"，这段样板在 file_io.cpp / strfmt.cpp 里已经手抄了两遍。
//   再抄四遍的话，"恢复夹具栈的哨兵"这类细节迟早有一份写错 ⇒ 抽成一处。
// ★ 它**不是**测试框架：没有用例注册、没有断言（断言仍在 check.h）。
// ★ 每次 `call()` / `runCode()` 都重新铺一次栈底的哨兵 0xFFFFFFFF：
//   `prepareStack()` 只写一次，而每次 ret 都会把哨兵弹掉；不重铺的话第二次调用会 ret 到 0。
#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <string>
#include <vector>

#include "runtime/machine.h"

namespace lift_fixture {

constexpr std::uint32_t kCodeBase = 0x00401000u;
constexpr std::uint32_t kDataBase = 0x00410000u;
constexpr std::uint32_t kStubBase = 0x7FFE2000u;
constexpr std::uint32_t kSentinel = 0xFFFFFFFFu;

// 最小机器码装配器（位移一律由 currentEa() 算，⛔ 不手填）
struct Asm {
    std::uint32_t base = 0;
    std::vector<std::uint8_t> b;

    explicit Asm(std::uint32_t code_ea = kCodeBase) : base(code_ea) {}
    std::uint32_t currentEa() const { return base + static_cast<std::uint32_t>(b.size()); }
    void u8(std::uint8_t v) { b.push_back(v); }
    void u16(std::uint16_t v) {
        u8(static_cast<std::uint8_t>(v & 0xFFu));
        u8(static_cast<std::uint8_t>(v >> 8));
    }
    void u32(std::uint32_t v) {
        for (int i = 0; i < 4; ++i) b.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFFu));
    }
    void pushImm(std::uint32_t v) {
        u8(0x68);
        u32(v);
    }
    void callRel(std::uint32_t target) {
        const std::int32_t rel = static_cast<std::int32_t>(target - (currentEa() + 5u));
        u8(0xE8);
        u32(static_cast<std::uint32_t>(rel));
    }
    void movEaxImm(std::uint32_t v) {
        u8(0xB8);
        u32(v);
    }
    // inc dword ptr [ea]
    void incDword(std::uint32_t ea) {
        u8(0xFF);
        u8(0x05);
        u32(ea);
    }
    // mov dword ptr [ea], value
    void movMemImm(std::uint32_t ea, std::uint32_t v) {
        u8(0xC7);
        u8(0x05);
        u32(ea);
        u32(v);
    }
    // mov eax, dword ptr [ea]
    void movEaxMem(std::uint32_t ea) {
        u8(0xA1);
        u32(ea);
    }
    void ret() { u8(0xC3); }
    void retImm16(std::uint16_t n) {
        u8(0xC2);
        u16(n);
    }
    // add esp, imm8（cdecl 调用方清栈；只用于 ≤127 字节）
    void addEspImm8(std::uint8_t n) {
        u8(0x83);
        u8(0xC4);
        u8(n);
    }
};

// `mdct_backward`（`sub_4E0B40`）差分夹具用的两张表。
//
// ★ 为什么不照抄 `sub_4E0030` 的三段 trig 公式：那会把 guest 的逻辑在测试里再写一遍，变成第二份
//   "没人验证的真源"（改一处就静默漂）；而 `mdct_backward` 是**无分支纯算术**，`trig` 取什么值
//   不改变它走哪条路。判据只需要"两臂拿到**同一份**表"。
// ★ `bitrev` 不一样：它**不是纯算术的输入**，而是**下标**（`x0 = out + n/2 + bit[k]`，读 `x0[0]/x0[1]`）。
//   所以取值域必须与真表一致 —— `mdct_init` 的公式 `((~acc)&mask)-1` 给出 **`[0, n/2-2]`**，且
//   **`bitrev[0]` 恰好就是最大值 `n/2-2`**。这里刻意复现这个范围（并把最大值放在 `[0]`）：
//   ⚠ 实测踩过 —— 准入判据一度写成 `[0, n/4)`，真表一半以上越界 ⇒ 生产里 `all` 那一项永远回退，
//   而夹具表落在窄区间内 ⇒ 两臂差分照样全绿。**窄夹具会让上界判据永远不被触发。**
// ★ `trig` 用 `sin` 的有界序列（⛔ 不用随下标线性增长的值：n=2048 时 11 级蝶形会把它放大到接近
//   float 上限，两臂都溢出就看不出差异了）。
inline void mdctFixtureTables(int n, std::vector<float>* trig, std::vector<std::int32_t>* bitrev) {
    const int n4 = n / 4;
    const int n2 = n / 2;
    const int span = n2 - 2;  // 真表的最大值（也是 `bitrev[0]`）
    trig->assign(static_cast<std::size_t>(n + n4), 0.0f);
    for (std::size_t i = 0; i < trig->size(); ++i)
        (*trig)[i] = static_cast<float>(std::sin(0.37 * static_cast<double>(i) + 0.11));
    bitrev->assign(static_cast<std::size_t>(n4), 0);
    for (int k = 0; k < n4; ++k) {
        if (k == 0) {
            (*bitrev)[0] = span;  // 真表的最大值：判据的上界必须接受它
            continue;
        }
        (*bitrev)[static_cast<std::size_t>(k)] = (k * 37 + 11) % (span + 1);  // 覆盖 [0, n/2-2]
    }
}

struct Fixture {
    lift::Machine m;
    std::string err;
    bool ok = true;
    std::uint32_t stub_cursor = kStubBase;
    std::uint32_t data_cursor = kDataBase;
    std::uint32_t code_cursor = kCodeBase;

    Fixture() {
        ok = m.initBare(&err) && m.prepareStack(&err);
        if (!ok) return;
    }

    // 给一条**已注册**的 shim 分配唯一桩地址并装块（返回 0 = 没注册）
    std::uint32_t use(const char* module, const char* name) {
        lift::ShimDef* def = m.shims().findMutable(module, name);
        if (!def) {
            ok = false;
            err = std::string("shim 没注册：") + module + "!" + name;
            return 0;
        }
        const std::uint32_t ea = stub_cursor;
        stub_cursor += 4;
        def->stub_ea = ea;
        m.cache().installShim(ea, def->id);
        return ea;
    }
    // kernel32 的简写（大多数夹具只碰 kernel32）
    std::uint32_t use(const char* name) { return use("kernel32", name); }

    std::uint32_t data(const void* p, std::size_t n) {
        const std::uint32_t ea = data_cursor;
        data_cursor += ((static_cast<std::uint32_t>(n) + 15u) & ~15u);
        if (data_cursor > 0x004F0000u) {
            ok = false;
            err = "夹具数据区用尽";
            return 0;
        }
        if (!m.placeData(ea, p, n, &err)) ok = false;
        return ea;
    }
    std::uint32_t cstr(const char* s) { return data(s, std::strlen(s) + 1u); }
    std::uint32_t u32data(std::uint32_t v) { return data(&v, 4u); }
    std::uint32_t zeroData(std::size_t n) {
        const std::vector<std::uint8_t> z(n, 0);
        return data(z.data(), z.size());
    }
    std::uint32_t wideCstr(const char16_t* s, std::size_t n_chars) { return data(s, n_chars * 2u); }

    // 重新铺一次夹具栈（每次调用前都要做；见文件头）
    void rearmStack() {
        m.cpu().esp() = m.layout().stack_top - 4u;
        m.mem().wr32(m.cpu().esp(), kSentinel);
    }

    lift::RunResult runCode(std::uint32_t code_ea, std::uint64_t max_insns = 2000000ull) {
        m.cpu().eip = code_ea;
        lift::RunOptions o;
        o.until_ea = kSentinel;
        o.max_insns = max_insns;
        const lift::RunResult r = m.run(o);
        if (r.reason != lift::StopReason::UntilEa) {
            ok = false;
            err = std::string("意外停机：") + lift::stopReasonName(r.reason) + " — " + r.detail;
        }
        return r;
    }

    // 一次性调用：push 参数（逆序）→ call 桩 → ret（哨兵）
    std::uint32_t call(std::uint32_t shim_ea, std::initializer_list<std::uint32_t> args) {
        const std::uint32_t code_ea = code_cursor;
        code_cursor += 0x100u;
        const std::vector<std::uint32_t> v(args);
        Asm a(code_ea);
        for (auto it = v.rbegin(); it != v.rend(); ++it) a.pushImm(*it);
        a.callRel(shim_ea);
        a.ret();
        if (!m.placeCode(code_ea, a.b.data(), a.b.size(), &err)) {
            ok = false;
            return 0;
        }
        rearmStack();
        runCode(code_ea);
        return m.cpu().eax();
    }
};

}  // namespace lift_fixture
