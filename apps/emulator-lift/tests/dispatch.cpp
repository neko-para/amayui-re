// emulator-lift · 派发 / 块缓存（pure：自造机器码字节）
//
// ★ 它盯的是三件事：
//   ① 块缓存真的**缓存**（同一块第二次不重新解码）；
//   ② `jcc / call / ret` 只改 eip 并回 dispatcher（块的边界正确，循环能跑回来）；
//   ③ 没实现的指令**响亮失败**（返回 Unimplemented，而不是静默 no-op）。
#include <string>
#include <vector>

#include "check.h"
#include "runtime/machine.h"

namespace {

bool boot(lift::Machine& m, const std::vector<std::uint8_t>& code) {
    std::string err;
    if (!m.initBare(&err)) {
        std::fprintf(stderr, "initBare: %s\n", err.c_str());
        return false;
    }
    if (!m.prepareStack(&err)) {
        std::fprintf(stderr, "prepareStack: %s\n", err.c_str());
        return false;
    }
    if (!m.placeCode(0x00401000u, code.data(), code.size(), &err)) {
        std::fprintf(stderr, "placeCode: %s\n", err.c_str());
        return false;
    }
    m.cpu().eip = 0x00401000u;
    return true;
}

lift::RunResult run(lift::Machine& m, std::uint64_t max_insns = 100000) {
    lift::RunOptions o;
    o.until_ea = 0xFFFFFFFFu;  // 栈顶哨兵
    o.max_insns = max_insns;
    return m.run(o);
}

}  // namespace

int main() {
    using namespace lift_test;

    // ① 直路：mov eax,1; add eax,2; ret
    {
        g_current_case = "straight-line";
        lift::Machine m;
        const std::vector<std::uint8_t> code = {0xB8, 0x01, 0x00, 0x00, 0x00,  // mov eax,1
                                                0x83, 0xC0, 0x02,              // add eax,2
                                                0xC3};                          // ret
        LIFT_CHECK(boot(m, code));
        const lift::RunResult r = run(m);
        LIFT_CHECK_EQ_U32(m.cpu().eax(), 3u);
        LIFT_CHECK(r.reason == lift::StopReason::UntilEa);
        LIFT_CHECK_EQ_I64(m.cache().blockCount(), 1);
        LIFT_CHECK_EQ_U32(m.cpu().eip, 0xFFFFFFFFu);
    }

    // ② 循环 + jcc：mov ecx,3; xor eax,eax; L: inc eax; dec ecx; jnz L; ret
    {
        g_current_case = "loop-jcc";
        lift::Machine m;
        const std::vector<std::uint8_t> code = {0xB9, 0x03, 0x00, 0x00, 0x00,  // mov ecx,3
                                                0x31, 0xC0,                    // xor eax,eax
                                                0x40,                          // L: inc eax
                                                0x49,                          // dec ecx
                                                0x75, 0xFC,                    // jnz L（-4）
                                                0xC3};                         // ret
        LIFT_CHECK(boot(m, code));
        const lift::RunResult r = run(m);
        LIFT_CHECK_EQ_U32(m.cpu().eax(), 3u);
        LIFT_CHECK_EQ_U32(m.cpu().ecx(), 0u);
        LIFT_CHECK(r.reason == lift::StopReason::UntilEa);
        // 块：第一条块（到 jnz 结束）+ 循环体块 ⇒ 至少 2 个；且命中过缓存
        LIFT_CHECK(m.cache().blockCount() >= 2);
        LIFT_CHECK(m.cache().hits() > 0);
    }

    // ③ call / ret：call f; ret; f: mov eax,7; ret
    {
        g_current_case = "call-ret";
        lift::Machine m;
        const std::vector<std::uint8_t> code = {0xE8, 0x01, 0x00, 0x00, 0x00,  // call 0x401006
                                                0xC3,                          // ret
                                                0xB8, 0x07, 0x00, 0x00, 0x00,  // mov eax,7
                                                0xC3};                         // ret
        LIFT_CHECK(boot(m, code));
        const std::uint32_t esp_expected = m.layout().stack_top;  // 最后的 ret 把哨兵也弹掉了
        const lift::RunResult r = run(m);
        LIFT_CHECK_EQ_U32(m.cpu().eax(), 7u);
        LIFT_CHECK(r.reason == lift::StopReason::UntilEa);
        LIFT_CHECK_EQ_U32(m.cpu().esp(), esp_expected);  // 栈平衡
    }

    // ④ 未实现指令（ADDPS xmm0,xmm0 = 0F 58 C0；浮点 SSE 语料为 0 ⇒ 仍未实现）⇒ 响亮失败
    {
        g_current_case = "unimplemented-is-loud";
        lift::Machine m;
        const std::vector<std::uint8_t> code = {0x0F, 0x58, 0xC0, 0xC3};  // addps xmm0,xmm0; ret
        LIFT_CHECK(boot(m, code));
        const lift::RunResult r = run(m);
        LIFT_CHECK(r.reason == lift::StopReason::Unimplemented);
        LIFT_CHECK_EQ_I64(r.insns, 0);  // 一条都没执行（不是先执行再静默）
        LIFT_CHECK_EQ_U32(m.cpu().eip, 0x00401000u);
    }

    // ⑤ 块缓存：同一段跑两遍，第二次全命中
    {
        g_current_case = "block-cache-reuse";
        lift::Machine m;
        const std::vector<std::uint8_t> code = {0xB8, 0x09, 0x00, 0x00, 0x00, 0xC3};
        LIFT_CHECK(boot(m, code));
        (void)run(m);
        const std::size_t misses1 = m.cache().misses();
        m.cpu().eip = 0x00401000u;
        m.cpu().esp() = m.layout().stack_top - 4u;
        m.mem().wr32(m.cpu().esp(), 0xFFFFFFFFu);
        (void)run(m);
        LIFT_CHECK_EQ_I64(m.cache().misses(), static_cast<std::int64_t>(misses1));  // 没再解码
        LIFT_CHECK(m.cache().hits() >= 1);
    }

    // ⑥ 长块分片：>256 条无控制流的直路（语料里有 554 条的 vtable 初始化块）
    //   ⇒ 解码返回**部分块**，run 从下一片继续，⛔ 不是 DecodeError。
    //   旧实现到第 256 条就 return false ⇒ 这条会红在 reason/insns。
    {
        g_current_case = "long-block-splits";
        lift::Machine m;
        std::vector<std::uint8_t> code(300, 0x90);  // 300 × nop
        code.push_back(0xC3);                        // ret
        LIFT_CHECK(boot(m, code));
        const lift::RunResult r = run(m, 100000);
        LIFT_CHECK(r.reason == lift::StopReason::UntilEa);
        LIFT_CHECK_EQ_I64(r.insns, 301);
        LIFT_CHECK(m.cache().blockCount() >= 2);  // 256 + 45
    }

    // ⑦ SMC：先缓存 target，再由 guest **写** target 的立即数，第二次调用必须看到新字节。
    //   布局（0x401000）：
    //     call target        ; 第一次调用把 target 块缓存下来（eax=0）
    //     mov byte [0x401013], 7
    //     call target        ; 第二次调用：有失效 ⇒ eax=7；无失效 ⇒ 旧 Insn 里 imm=0 ⇒ eax=0
    //     ret
    //   target（0x401012）: mov eax, 0 ; ret
    {
        g_current_case = "smc-invalidates-block";
        lift::Machine m;
        const std::vector<std::uint8_t> code = {
            0xE8, 0x0D, 0x00, 0x00, 0x00,              // call 0x401012
            0xC6, 0x05, 0x13, 0x10, 0x40, 0x00, 0x07,  // mov byte [0x401013], 7
            0xE8, 0x01, 0x00, 0x00, 0x00,              // call 0x401012
            0xC3,                                      // ret
            0xB8, 0x00, 0x00, 0x00, 0x00,              // mov eax, 0
            0xC3};                                     // ret
        LIFT_CHECK(boot(m, code));
        const lift::RunResult r = run(m);
        LIFT_CHECK(r.reason == lift::StopReason::UntilEa);
        LIFT_CHECK_EQ_U32(m.cpu().eax(), 7u);  // 旧实现会是 0（缓存的 imm 没更新）
    }

    return finish("dispatch");
}
