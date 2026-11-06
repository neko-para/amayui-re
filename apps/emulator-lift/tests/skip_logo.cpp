// emulator-lift · `--skip-logo` 的字段写入（pure）
//
// 判据（红了意味着什么）：
//   ① 钩子点 = 0x004BAA37（`sub_415640` 构造返回之后、`mov ds:dword_55E1BC,eax` 之前）：
//      到达该 EA 时 EAX = T_Command，字段 +0x5EB5C 初值 1；
//   ② 打开 `setSkipLogo(true)` ⇒ 只把 +0x5EB5C 写 0，前后相邻字段一个字节都不动；
//   ③ 关闭 ⇒ 字段保持 1（⛔ 不是"默认就跳过 LOGO"）；
//   ④ EAX = 0（分配失败路径）⇒ 不写、不崩、不谎报 applied。
//
// ★ 这里刻意**不引用实现里的常量**：0x4BAA37 / 0x5EB5C 是引擎锚（`.lst` + shape），
//   实现如果把钩子/字段挪走，本用例必须红。
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "check.h"
#include "runtime/machine.h"

namespace {

constexpr std::uint32_t kHookEa = 0x004BAA37u;
constexpr std::uint32_t kShowLogoOffset = 0x0005EB5Cu;
constexpr std::uint32_t kObjectEa = 0x00410000u;
constexpr std::uint32_t kObjectBytes = kShowLogoOffset + 0x10u;  // 覆盖字段后的哨兵
constexpr std::uint32_t kSentinelEa = 0xFFFFFFFFu;
constexpr std::uint32_t kBefore = 0xDEADBEEFu;
constexpr std::uint32_t kAfter = 0xCAFEBABEu;

bool setup(lift::Machine& m, std::uint32_t object, bool skip, std::string* err) {
    if (!m.initBare(err)) return false;
    if (!m.prepareStack(err)) return false;
    std::vector<std::uint8_t> zeros(kObjectBytes, 0u);
    if (!m.placeData(kObjectEa, zeros.data(), zeros.size(), err)) return false;
    m.mem().wr32(kObjectEa + kShowLogoOffset - 4u, kBefore);
    m.mem().wr32(kObjectEa + kShowLogoOffset, 1u);
    m.mem().wr32(kObjectEa + kShowLogoOffset + 4u, kAfter);
    // 钩子点放 `nop; ret`：ret 弹到栈顶哨兵，dispatcher 在 until_ea 停。
    const std::uint8_t code[] = {0x90u, 0xC3u};
    if (!m.placeCode(kHookEa, code, sizeof(code), err)) return false;
    m.cpu().eip = kHookEa;
    m.cpu().eax() = object;
    m.setSkipLogo(skip);
    return true;
}

bool runToSentinel(lift::Machine& m) {
    lift::RunOptions opt;
    opt.until_ea = kSentinelEa;
    opt.max_insns = 32;
    const lift::RunResult r = m.run(opt);
    return r.reason == lift::StopReason::UntilEa;
}

}  // namespace

int main() {
    using namespace lift_test;
    std::string err;

    // ① + ②：开启 ⇒ +0x5EB5C = 0，邻居不变。
    {
        g_current_case = "skip-on";
        lift::Machine m;
        const bool ok = setup(m, kObjectEa, true, &err);
        LIFT_CHECK(ok);
        if (!ok) {
            std::fprintf(stderr, "setup failed: %s\n", err.c_str());
        } else {
            LIFT_CHECK(runToSentinel(m));
            LIFT_CHECK_EQ_U32(m.mem().rd32(kObjectEa + kShowLogoOffset), 0u);
            LIFT_CHECK_EQ_U32(m.mem().rd32(kObjectEa + kShowLogoOffset - 4u), kBefore);
            LIFT_CHECK_EQ_U32(m.mem().rd32(kObjectEa + kShowLogoOffset + 4u), kAfter);
            LIFT_CHECK(m.skipLogoApplied());
            LIFT_CHECK_EQ_U32(m.cpu().eax(), kObjectEa);
        }
    }

    // ③：关闭 ⇒ 字段保持初值 1，且不谎报 applied。
    {
        g_current_case = "skip-off";
        lift::Machine m;
        const bool ok = setup(m, kObjectEa, false, &err);
        LIFT_CHECK(ok);
        if (!ok) {
            std::fprintf(stderr, "setup failed: %s\n", err.c_str());
        } else {
            LIFT_CHECK(runToSentinel(m));
            LIFT_CHECK_EQ_U32(m.mem().rd32(kObjectEa + kShowLogoOffset), 1u);
            LIFT_CHECK(!m.skipLogoApplied());
        }
    }

    // ④：对象指针为 0 ⇒ 不写、不崩、不谎报。
    {
        g_current_case = "null-object";
        lift::Machine m;
        const bool ok = setup(m, 0u, true, &err);
        LIFT_CHECK(ok);
        if (!ok) {
            std::fprintf(stderr, "setup failed: %s\n", err.c_str());
        } else {
            LIFT_CHECK(runToSentinel(m));
            LIFT_CHECK_EQ_U32(m.mem().rd32(kObjectEa + kShowLogoOffset), 1u);
            LIFT_CHECK(!m.skipLogoApplied());
        }
    }

    return finish("skip-logo");
}
