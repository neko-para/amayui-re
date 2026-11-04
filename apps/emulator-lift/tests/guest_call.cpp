// emulator-lift · 嵌套 guest 回调栈（P0 / G3；pure）
//
// 判据：shim → guest 回调 → 回到 shim 续延，EAX/ESP 正确；续延里再启动一次回调（嵌套）也成立。
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "check.h"
#include "runtime/guest_call.h"
#include "runtime/machine.h"

namespace {
constexpr std::uint32_t kCode = 0x00401000u;
constexpr std::uint32_t kFakeRet = 0x00402000u;
constexpr std::uint32_t kStack = 0x6FFFF000u;

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
    LIFT_CHECK_EQ_U32(cpu.esp(), kStack + 4u + 4u * 2u);

    return finish("guest-call");
}
