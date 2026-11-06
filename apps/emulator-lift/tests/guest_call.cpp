// emulator-lift · 嵌套 guest 回调栈（P0 / G3；pure）
//
// 判据：shim → guest 回调 → 回到 shim 续延，EAX/ESP 正确；续延里再启动一次回调（嵌套）也成立。
// ★ ② 钉住"帧把 this（ECX）也当状态"：铺帧之后、callee 第一条指令之前插入一个 **winmm 定时器
//   回调**（真机上它在另一个线程里跑），callee 仍然要拿到 `start()` 指定的 this；续延看到的
//   则是**调用方**的 ECX。同时钉住"定时器回调打断的通用寄存器整份还回去"——只还 EIP/ESP 时，
//   回调改掉的 EAX/ECX/EDX/EBX/ESI/EDI/EBP/EFLAGS 会漏进被插入点。
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "check.h"
#include "runtime/guest_call.h"
#include "runtime/machine.h"

namespace {
constexpr std::uint32_t kCode = 0x00401000u;
constexpr std::uint32_t kTimerCb = 0x00401100u;
constexpr std::uint32_t kSeenEa = 0x00410000u;
constexpr std::uint32_t kFakeRet = 0x00402000u;
constexpr std::uint32_t kStack = 0x6FFFF000u;

std::uint32_t g_cont_ecx = 0xDEADBEEFu;
std::uint32_t g_cb_result = 0xDEADBEEFu;

void put(std::vector<std::uint8_t>& b, std::uint8_t v) { b.push_back(v); }
void put32(std::vector<std::uint8_t>& b, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) b.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF));
}

// mov eax, imm32; ret n
std::vector<std::uint8_t> callback(std::uint32_t value, std::uint16_t pop) {
    std::vector<std::uint8_t> b;
    put(b, 0xB8);
    put32(b, value);
    put(b, 0xC2);
    put(b, static_cast<std::uint8_t>(pop & 0xFF));
    put(b, static_cast<std::uint8_t>((pop >> 8) & 0xFF));
    return b;
}

// mov <reg32>, imm32
void movRegImm(std::vector<std::uint8_t>& b, std::uint8_t opcode, std::uint32_t value) {
    put(b, opcode);
    put32(b, value);
}
}  // namespace

int main() {
    using namespace lift_test;
    lift::Machine m;
    std::string err;
    g_current_case = "setup";
    LIFT_CHECK(m.initBare(&err));
    LIFT_CHECK(m.prepareStack(&err));

    const auto cb1 = callback(0x11111111u, 8);  // 两个参数
    LIFT_CHECK(m.placeCode(kCode, cb1.data(), cb1.size(), &err));

    lift::X86& cpu = m.cpu();
    cpu.esp() = kStack;
    m.mem().wr32(kStack, kFakeRet);  // 假装是某个 shim 的返回地址

    g_current_case = "single-callback";
    lift::GuestContinuation cont = [](lift::X86&, std::uint32_t result, std::uint32_t* out) {
        LIFT_CHECK_EQ_U32(result, 0x11111111u);
        *out = 0xABCDEF01u;
        return true;
    };
    LIFT_CHECK(cpu.calls->start(cpu, kCode, {0x1u, 0x2u}, 4u + 4u * 2u, "test1", cont, &err));

    lift::RunOptions o;
    o.until_ea = kFakeRet;
    o.max_insns = 1000;
    const lift::RunResult r = m.run(o);
    g_current_case = "result";
    LIFT_CHECK_EQ_U32(r.ea, kFakeRet);
    LIFT_CHECK_EQ_U32(cpu.eax(), 0xABCDEF01u);
    // ★ 旧的续延 lambda 里对 result 的断言仍成立；这里只补 ②。
    LIFT_CHECK_EQ_U32(cpu.esp(), kStack + 4u + 4u * 2u);
    // ---- ② this（ECX）跨"定时器插入"仍是 callee 该看到的那个值；寄存器整份还回去 ----
    {
        g_current_case = "nested-this-vs-timer";
        lift::Machine m2;
        std::string err;
        LIFT_CHECK(m2.initBare(&err));
        LIFT_CHECK(m2.prepareStack(&err));
        lift::X86& cpu2 = m2.cpu();
        const std::uint8_t zeros[4] = {};
        LIFT_CHECK(m2.placeData(kSeenEa, zeros, sizeof(zeros), &err));

        // callee：把 ECX 记到 guest 内存，然后 `ret 8`（两个参数）。
        //   mov [kSeenEa], ecx  = 89 0D <ea>；ret 8 = C2 08 00
        {
            std::vector<std::uint8_t> b;
            put(b, 0x89);
            put(b, 0x0D);
            put32(b, kSeenEa);
            put(b, 0xC2);
            put(b, 0x08);
            put(b, 0x00);
            LIFT_CHECK(m2.placeCode(kCode, b.data(), b.size(), &err));
        }
        // 定时器回调：把所有通用寄存器改成垃圾（EAX 还顺手 xor 改 EFLAGS），然后 ret。
        // ★ 真机上这个回调跑在 winmm 的线程里 ⇒ 被打断点的寄存器组必须原样回来。
        {
            std::vector<std::uint8_t> b;
            movRegImm(b, 0xB8, 0xDEADBEEFu);  // mov eax, 0xDEADBEEF
            put(b, 0x31);
            put(b, 0xC0);  // xor eax, eax（改 EFLAGS）
            movRegImm(b, 0xB9, 0xAAAAAAAAu);  // mov ecx, ...
            movRegImm(b, 0xBA, 0xBBBBBBBBu);  // mov edx, ...
            movRegImm(b, 0xBB, 0xCCCCCCCCu);  // mov ebx, ...
            movRegImm(b, 0xBE, 0xDDDDDDDDu);  // mov esi, ...
            movRegImm(b, 0xBF, 0xEEEEEEEEu);  // mov edi, ...
            movRegImm(b, 0xBD, 0x99999999u);  // mov ebp, ...
            put(b, 0xC3);                     // ret（续延点自己恢复 esp/eip）
            LIFT_CHECK(m2.placeCode(kTimerCb, b.data(), b.size(), &err));
        }
        lift::GuestTimer t;
        t.id = 1;
        t.due_100ns = 0;  // 立刻到期 ⇒ 在 callee 第一条指令之前触发
        t.callback = kTimerCb;
        t.oneshot = true;
        cpu2.shim.timers.push_back(t);

        cpu2.esp() = kStack;
        m2.mem().wr32(kStack, kFakeRet);
        cpu2.ecx() = 0x00000099u;  // 调用方（native / shim）的 ECX
        cpu2.eax() = 0x11111111u;
        cpu2.ebx() = 0x22222222u;
        cpu2.edx() = 0x33333333u;
        cpu2.esi() = 0x44444444u;
        cpu2.edi() = 0x55555555u;
        cpu2.ebp() = 0x66666666u;
        cpu2.eflags = 0x202u;

        lift::GuestContinuation cont2 = [](lift::X86& c, std::uint32_t result, std::uint32_t* out) {
            g_cont_ecx = c.ecx();     // 续延是宿主侧 ⇒ 看到的是**调用方**的 ECX
            g_cb_result = result;     // = callee 的 EAX（= 定时器回调之前的 EAX）
            *out = 0x55667788u;
            return true;
        };
        // callee_ecx = 0x44 ⇒ 即使中间插了定时器回调，callee 也要看到 0x44
        LIFT_CHECK(cpu2.calls->start(cpu2, kCode, {0x1u, 0x2u}, 4u + 8u, "test2", cont2, &err, 0x44u));

        lift::RunOptions o2;
        o2.until_ea = kFakeRet;
        o2.max_insns = 1000;
        const lift::RunResult r2 = m2.run(o2);
        LIFT_CHECK_EQ_U32(r2.ea, kFakeRet);
        LIFT_CHECK_EQ_U32(m2.mem().rd32(kSeenEa), 0x44u);   // callee 的 this 没被定时器改掉
        LIFT_CHECK_EQ_U32(g_cont_ecx, 0x99u);               // 续延看到调用方的 ECX
        LIFT_CHECK_EQ_U32(g_cb_result, 0x11111111u);        // 回调前的 EAX 原样交给 callee
        LIFT_CHECK_EQ_U32(cpu2.eax(), 0x55667788u);         // 续延的返回值 = 本次调用的 EAX
        LIFT_CHECK_EQ_U32(cpu2.ecx(), 0x99u);               // 返回调用点后 = 调用方的 ECX
        LIFT_CHECK_EQ_U32(cpu2.ebx(), 0x22222222u);         // ↓ 定时器回调没漏出来
        LIFT_CHECK_EQ_U32(cpu2.edx(), 0x33333333u);
        LIFT_CHECK_EQ_U32(cpu2.esi(), 0x44444444u);
        LIFT_CHECK_EQ_U32(cpu2.edi(), 0x55555555u);
        LIFT_CHECK_EQ_U32(cpu2.ebp(), 0x66666666u);
        LIFT_CHECK_EQ_U32(cpu2.eflags, 0x202u);             // 回调里的 xor 不许改到被打断点
        LIFT_CHECK_EQ_U32(cpu2.esp(), kStack + 4u + 8u);
    }

    return finish("guest-call");
}
