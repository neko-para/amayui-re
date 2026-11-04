// emulator-lift · shim ABI（pure：guest 侧自造调用序列）
//
// ★ 判据（plan §6 S5）：guest 侧造栈 ⇒ 断言**弹参与返回值**、以及**栈平衡**。
//   三条覆盖三种约定：stdcall 被调方弹、cdecl 调用方弹、varargs 按 cdecl（调用方弹）。
// ⛔ 只要"统一按 stdcall 弹"，cdecl 那条就会红（esp 每次多抬 4 字节）—— 这就是它的意义。
// ★ 机器码用**小型汇编器**生成：远地址的 call 位移手算太容易错，让代码算。
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "check.h"
#include "runtime/machine.h"
#include "shims/register.h"

namespace {

constexpr std::uint32_t kCodeEa = 0x00401000u;

// 只做本文件要用的几条编码；位移由 `currentEa()` 算，⛔ 不手填
struct Asm {
    std::vector<std::uint8_t> b;

    std::uint32_t currentEa() const { return kCodeEa + static_cast<std::uint32_t>(b.size()); }
    void u8(std::uint8_t v) { b.push_back(v); }
    void u32(std::uint32_t v) {
        for (int i = 0; i < 4; ++i) b.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFFu));
    }
    void pushImm(std::uint32_t v) {
        u8(0x68);
        u32(v);
    }
    void callRel(std::uint32_t target) {
        const std::uint32_t at = currentEa();
        const std::int32_t rel = static_cast<std::int32_t>(target - (at + 5));
        u8(0xE8);
        u32(static_cast<std::uint32_t>(rel));
    }
    void addEspImm8(std::uint8_t n) {
        u8(0x83);
        u8(0xC4);
        u8(n);
    }
    void ret() { u8(0xC3); }
};

std::uint32_t g_arg0 = 0;
std::uint32_t g_arg1 = 0;
std::uint32_t g_varargs_seen = 0;

void shimTwoArgs(lift::X86&, lift::ShimCall& call) {
    g_arg0 = call.u32(0);
    g_arg1 = call.u32(1);
    call.result = 0x00001234u;
}

void shimVarargs(lift::X86&, lift::ShimCall& call) {
    g_varargs_seen = 0;
    for (int i = 0; i < 3; ++i) g_varargs_seen += call.u32(i);
    call.result = 0x00005678u;
}

bool attach(lift::Machine& m, const char* name, lift::Conv conv, int argc, lift::ShimFn fn,
            std::uint32_t stub_ea) {
    m.shims().add("test", name, conv, argc, fn);
    lift::ShimDef* def = m.shims().findMutable("test", name);
    if (!def) return false;
    def->stub_ea = stub_ea;
    m.cache().installShim(stub_ea, def->id);
    return true;
}

bool boot(lift::Machine& m, const Asm& code) {
    std::string err;
    if (!m.initBare(&err)) return false;
    if (!m.prepareStack(&err)) return false;
    if (!m.placeCode(kCodeEa, code.b.data(), code.b.size(), &err)) return false;
    m.cpu().eip = kCodeEa;
    return true;
}

lift::RunResult run(lift::Machine& m) {
    lift::RunOptions o;
    o.until_ea = 0xFFFFFFFFu;
    o.max_insns = 10000;
    return m.run(o);
}

}  // namespace

int main() {
    using namespace lift_test;

    // ① stdcall（2 个参数）：esp 结算 = 初始（被调方弹了 8 字节 + ret 的 4 字节）
    {
        g_current_case = "stdcall-two-args";
        Asm a;
        a.pushImm(2);
        a.pushImm(1);
        a.callRel(0x7FFE0010u);
        a.ret();
        lift::Machine m;
        LIFT_CHECK(boot(m, a));
        const std::uint32_t esp_expected = m.layout().stack_top;  // 最后的 ret 把哨兵也弹掉了
        LIFT_CHECK(attach(m, "two", lift::Conv::Stdcall, 2, &shimTwoArgs, 0x7FFE0010u));
        const lift::RunResult r = run(m);
        LIFT_CHECK(r.reason == lift::StopReason::UntilEa);
        LIFT_CHECK_EQ_U32(g_arg0, 1u);
        LIFT_CHECK_EQ_U32(g_arg1, 2u);
        LIFT_CHECK_EQ_U32(m.cpu().eax(), 0x00001234u);
        LIFT_CHECK_EQ_U32(m.cpu().esp(), esp_expected);
    }

    // ② cdecl（2 个参数）：被调方**不**弹参 ⇒ 调用方 `add esp,8` 之后才平衡
    {
        g_current_case = "cdecl-two-args";
        Asm a;
        a.pushImm(2);
        a.pushImm(1);
        a.callRel(0x7FFE0020u);
        a.addEspImm8(8);
        a.ret();
        lift::Machine m;
        LIFT_CHECK(boot(m, a));
        const std::uint32_t esp_expected = m.layout().stack_top;  // 最后的 ret 把哨兵也弹掉了
        LIFT_CHECK(attach(m, "cd", lift::Conv::Cdecl, 2, &shimTwoArgs, 0x7FFE0020u));
        const lift::RunResult r = run(m);
        LIFT_CHECK(r.reason == lift::StopReason::UntilEa);
        LIFT_CHECK_EQ_U32(g_arg0, 1u);
        LIFT_CHECK_EQ_U32(g_arg1, 2u);
        LIFT_CHECK_EQ_U32(m.cpu().eax(), 0x00001234u);
        LIFT_CHECK_EQ_U32(m.cpu().esp(), esp_expected);
    }

    // ③ varargs：声明 argc=0 ⇒ 调用方弹参；实现读 3 个
    {
        g_current_case = "varargs";
        Asm a;
        a.pushImm(3);
        a.pushImm(2);
        a.pushImm(1);
        a.callRel(0x7FFE0030u);
        a.addEspImm8(12);
        a.ret();
        lift::Machine m;
        LIFT_CHECK(boot(m, a));
        const std::uint32_t esp_expected = m.layout().stack_top;  // 最后的 ret 把哨兵也弹掉了
        LIFT_CHECK(attach(m, "va", lift::Conv::Varargs, 0, &shimVarargs, 0x7FFE0030u));
        const lift::RunResult r = run(m);
        LIFT_CHECK(r.reason == lift::StopReason::UntilEa);
        LIFT_CHECK_EQ_U32(g_varargs_seen, 6u);  // 1+2+3
        LIFT_CHECK_EQ_U32(m.cpu().eax(), 0x00005678u);
        LIFT_CHECK_EQ_U32(m.cpu().esp(), esp_expected);
    }

    // ④ 未实现的导入（桩存在、实现为空）⇒ 响亮失败 + 记进缺口
    {
        g_current_case = "unimplemented-import-traps";
        Asm a;
        a.callRel(0x7FFE0040u);
        a.ret();
        lift::Machine m;
        LIFT_CHECK(boot(m, a));
        m.shims().addUnimplemented("test", "nope", "单元测试用的未实现导入");
        lift::ShimDef* def = m.shims().findMutable("test", "nope");
        LIFT_CHECK(def != nullptr);
        if (def) {
            def->stub_ea = 0x7FFE0040u;
            m.cache().installShim(def->stub_ea, def->id);
        }
        const lift::RunResult r = run(m);
        LIFT_CHECK(r.reason == lift::StopReason::Unimplemented);
        LIFT_CHECK(m.shims().calledUnimplemented().size() == 1);
    }

    // ⑤ 顺带覆盖"实参顺序"：第 0 个参数在 `[esp+4]`（返回地址之上）—— 如果顺序反了，
    //    上面 stdcall 用例的 arg0/arg1 会互换，这里再显式钉一次。
    {
        g_current_case = "stack-arg-order";
        Asm a;
        a.pushImm(0xAAAA5555u);  // 先 push ⇒ 更高的地址 ⇒ arg1
        a.pushImm(0xBBBB6666u);  // 后 push ⇒ 更低的地址 ⇒ arg0
        a.callRel(0x7FFE0050u);
        a.ret();
        lift::Machine m;
        LIFT_CHECK(boot(m, a));
        LIFT_CHECK(attach(m, "order", lift::Conv::Stdcall, 2, &shimTwoArgs, 0x7FFE0050u));
        const lift::RunResult r = run(m);
        LIFT_CHECK(r.reason == lift::StopReason::UntilEa);
        LIFT_CHECK_EQ_U32(g_arg0, 0xBBBB6666u);
        LIFT_CHECK_EQ_U32(g_arg1, 0xAAAA5555u);
    }

    // ⑥ SCOPE §1 菜单 / 模态 no-op：返回合理值 + stdcall 栈平衡，⛔ 不是硬 trap。
    //   判据：真跑注册表里的 user32 stub（不是本文件自造的），所以 `find` 必须拿到 `fn` 非空的那条。
    {
        g_current_case = "menu-modal-noop";
        auto runStub = [&](const char* mod, const char* name, const std::vector<std::uint32_t>& args,
                           std::uint32_t expected, bool nonzero) {
            lift::Machine m;
            std::string err;
            LIFT_CHECK(m.initBare(&err));
            LIFT_CHECK(m.prepareStack(&err));
            // 真跑注册表里的 user32 stub（fixture 的 initBare 不装配 shim）
            lift::registerUser32MenuShims(m.shims());
            lift::ShimDef* d = m.shims().findMutable(mod, name);
            LIFT_CHECK(d != nullptr);
            LIFT_CHECK(d && d->fn != nullptr);  // 不是 addUnimplemented（那会是硬 trap）
            if (!d) return;
            d->stub_ea = 0x7FFE0100u;
            m.cache().installShim(d->stub_ea, d->id);
            Asm a;
            for (auto it = args.rbegin(); it != args.rend(); ++it) a.pushImm(*it);
            a.callRel(d->stub_ea);
            a.ret();
            LIFT_CHECK(m.placeCode(kCodeEa, a.b.data(), a.b.size(), &err));
            m.cpu().eip = kCodeEa;
            const lift::RunResult r = run(m);
            LIFT_CHECK(r.reason == lift::StopReason::UntilEa);
            if (nonzero) {
                LIFT_CHECK_NE_U32(m.cpu().eax(), 0u);
            } else {
                LIFT_CHECK_EQ_U32(m.cpu().eax(), expected);
            }
            LIFT_CHECK_EQ_U32(m.cpu().esp(), m.layout().stack_top);  // 被调方弹参 + ret 弹哨兵
        };
        // LoadMenuA(hInstance, lpMenuName) ⇒ 非空假句柄
        runStub("user32", "LoadMenuA", {0u, 0u}, 0u, /*nonzero=*/true);
        // SetMenu(hWnd, hMenu) ⇒ TRUE
        runStub("user32", "SetMenu", {1u, 0x00010001u}, 1u, false);
        // TrackPopupMenu(7 参数) ⇒ 0
        runStub("user32", "TrackPopupMenu", {0u, 0u, 0u, 0u, 0u, 0u, 0u}, 0u, false);
        // MessageBoxA(hWnd, text, caption, type) ⇒ IDOK(1)
        runStub("user32", "MessageBoxA", {0u, 0u, 0u, 0u}, 1u, false);
    }

    return finish("shim-abi");
}
