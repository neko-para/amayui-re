// emulator-lift · 派发 / 块缓存（pure：自造机器码字节）
//
// ★ 它盯的是五件事：
//   ① 块缓存真的**缓存**（同一块第二次不重新解码）；
//   ② `jcc / call / ret` 只改 eip 并回 dispatcher（块的边界正确，循环能跑回来）；
//   ③ 没实现的指令**响亮失败**（返回 Unimplemented，而不是静默 no-op）；
//   ④ SMC（guest 写已解码的代码页）**只丢 guest 指令块**，⛔ 不连带丢掉 `installShim` 的 shim 块；
//   ⑤ **SSE 对齐违例必须响亮失败**（`movaps` / `movdqa` 的 m128 未 16 字节对齐 ⇒ Unimplemented，
//      一条都不执行、目标一个字节都不写 —— ⛔ 不许静默按 movups / movdqu 处理）。
#include <cstdint>
#include <string>
#include <vector>

#include "check.h"
#include "fixture.h"
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

// ⑧ 用的 shim：返回一个**可辨认的哨兵值**，于是"桩页字节被当成代码跑"变成一次可读的失败
//   （返回 0xBEEF0001 的调用真的发生过，还是根本没进 shim）。
std::uint32_t g_bump_calls = 0;
void shimBump(lift::X86&, lift::ShimCall& call) {
    ++g_bump_calls;
    call.result = 0xBEEF0001u;
}

// ⑧ 的公共装配：一台 bare 机器 + 一条 shim（`addRuntime` 发唯一桩地址 → `installShim` 装块）。
//   返回桩地址；0 = 装配失败（断言已经在里面）。
std::uint32_t bootWithShim(lift::Machine& m, std::uint32_t stub_page, std::string* err) {
    g_bump_calls = 0;
    if (!m.initBare(err)) return 0;
    if (!m.prepareStack(err)) return 0;
    m.shims().configureStubPage(stub_page, 0x100u);
    lift::ShimDef* def = m.shims().addRuntime("dispatch_test", "Bump", lift::Conv::Stdcall, 0, &shimBump);
    if (!def || def->stub_ea == 0) return 0;
    m.cache().installShim(def->stub_ea, def->id);
    return def->stub_ea;
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

    // ⑧ SMC 只该丢 **guest 指令块**：`dropBlocks()` 把 `blocks_` 整张清掉时，`installShim` 装进去的
    //   shim 块也一起没了 ⇒ 下一次 `call 桩地址` 会去**解码桩页的字节**：
    //     桩页没提交 ⇒ decode-error（看起来像"解码器不认识这条指令"，归因误导）；
    //     桩页有字节 ⇒ 静默把数据当代码跑（走错路）。
    //   布局（主代码 0x401000；shim 桩 0x7FFE0000，桩页**不提交** —— 与真实夹具/ddraw 的桩页同形）：
    //     0x401000: call helper          ; 先解码 helper（把 0x401000 这页标成代码页）
    //     0x401005: mov byte [0x401021], 0x63   ; ★ 写**已解码的代码页** ⇒ 触发 SMC 失效
    //     0x40100C: call shim            ; ★ 失效之后第一次调 shim：修好前这条丢块 ⇒ decode-error
    //     0x401011: mov [0x410000], eax  ; 记下 shim 的返回值（否则随后被 helper 覆盖）
    //     0x401016: call helper          ; helper 的立即数已被改 ⇒ eax=99 证明 guest 块**确实**失效了
    //     0x40101B: ret
    //     helper(0x401020): mov eax,42 ; ret
    {
        g_current_case = "smc-preserves-shim-blocks";
        lift::Machine m;
        std::string err;
        const std::uint32_t stub_ea = bootWithShim(m, 0x7FFE0000u, &err);
        LIFT_CHECK_NE_U32(stub_ea, 0u);
        if (stub_ea != 0) {
            lift_fixture::Asm body(0x00401000u);
            body.callRel(0x00401020u);                                              // 0x401000 call helper
            body.u8(0xC6); body.u8(0x05); body.u32(0x00401021u); body.u8(0x63);     // 0x401005 mov byte [0x401021],0x63
            body.callRel(stub_ea);                                                  // 0x40100C call shim
            body.u8(0xA3); body.u32(0x00410000u);                                   // 0x401011 mov [0x410000],eax
            body.callRel(0x00401020u);                                              // 0x401016 call helper
            body.ret();                                                             // 0x40101B ret
            lift_fixture::Asm helper(0x00401020u);
            helper.movEaxImm(42);  // 0x401020 mov eax,42（0x401021 是它的立即数低字节）
            helper.ret();          // 0x401025 ret
            const std::uint8_t zero[4] = {};
            LIFT_CHECK(m.placeCode(0x00401000u, body.b.data(), body.b.size(), &err));
            LIFT_CHECK(m.placeCode(0x00401020u, helper.b.data(), helper.b.size(), &err));
            LIFT_CHECK(m.placeData(0x00410000u, zero, sizeof(zero), &err));
            m.cpu().eip = 0x00401000u;

            const lift::RunResult r = run(m);
            LIFT_CHECK(r.reason == lift::StopReason::UntilEa);  // 修好前：DecodeError（桩页当代码解）
            LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(g_bump_calls), 1);  // shim 真的被调到了
            LIFT_CHECK_EQ_U32(m.mem().rd32(0x00410000u), 0xBEEF0001u);
            LIFT_CHECK_EQ_U32(m.cpu().eax(), 99u);  // guest 块仍被失效：第二次 helper 看到新立即数
            lift::Block* sb = m.cache().peek(stub_ea);
            LIFT_CHECK(sb != nullptr && sb->is_shim);  // 失效之后 shim 块还在（而且还是 shim 块）
        }
    }

    // ⑨ 同一个根因的**同一页**形态：guest 指令与 shim 桩共处一页（4 KiB 粒度 ⇒ 失效"整页一刀切"时
    //   最容易被误丢的那一形）。这里桩槽的 4 个字节是页里的数据 `90 90 90 C3`：
    //   修好前，桩块被丢 ⇒ 解出来变成 `nop;nop;nop;ret` ⇒ 静默跳过 shim（g_bump_calls==0、结果是 42）。
    {
        g_current_case = "smc-preserves-shim-blocks-same-page";
        lift::Machine m;
        std::string err;
        const std::uint32_t stub_ea = bootWithShim(m, 0x7FFE0000u, &err);
        LIFT_CHECK_EQ_U32(stub_ea, 0x7FFE0000u);  // 桩在页首 = 与下面的 guest 代码同页
        if (stub_ea != 0) {
            const std::vector<std::uint8_t> slot = {0x90, 0x90, 0x90, 0xC3};  // 桩页里的"数据"
            lift_fixture::Asm body(0x7FFE0100u);
            body.callRel(0x7FFE0140u);                                              // 0x7FFE0100 call helper（同页）
            body.u8(0xC6); body.u8(0x05); body.u32(0x7FFE0141u); body.u8(0x63);     // 0x7FFE0105 mov byte [0x7FFE0141],0x63
            body.callRel(stub_ea);                                                  // 0x7FFE010C call shim（同页）
            body.u8(0xA3); body.u32(0x00410000u);                                   // 0x7FFE0111 mov [0x410000],eax
            body.callRel(0x7FFE0140u);                                              // 0x7FFE0116 call helper
            body.ret();                                                             // 0x7FFE011B ret
            lift_fixture::Asm helper(0x7FFE0140u);
            helper.movEaxImm(42);  // 0x7FFE0140 mov eax,42（0x7FFE0141 是它的立即数低字节）
            helper.ret();          // 0x7FFE0145 ret
            const std::uint8_t zero[4] = {};
            LIFT_CHECK(m.placeCode(0x7FFE0000u, slot.data(), slot.size(), &err));
            LIFT_CHECK(m.placeCode(0x7FFE0100u, body.b.data(), body.b.size(), &err));
            LIFT_CHECK(m.placeCode(0x7FFE0140u, helper.b.data(), helper.b.size(), &err));
            LIFT_CHECK(m.placeData(0x00410000u, zero, sizeof(zero), &err));
            m.cpu().eip = 0x7FFE0100u;

            const lift::RunResult r = run(m);
            LIFT_CHECK(r.reason == lift::StopReason::UntilEa);  // 修好前也是 UntilEa —— 但走错路（见下）
            LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(g_bump_calls), 1);  // shim 被跳过的话这里是 0
            LIFT_CHECK_EQ_U32(m.mem().rd32(0x00410000u), 0xBEEF0001u);      // 修好前：42（桩字节当代码跑）
            LIFT_CHECK_EQ_U32(m.cpu().eax(), 99u);  // 同页的 guest 块仍被失效（没有连 guest 块一起"保住"）
            lift::Block* sb = m.cache().peek(stub_ea);
            LIFT_CHECK(sb != nullptr && sb->is_shim);
        }
    }

    // ⑩ 同根因的另一面：**同一个 EA 上先解出了一条 guest 块，再声明它是 shim**。
    //   旧实现 `installShim` 见 `blocks_` 里已有条目就原样返回 ⇒ 返回的是一条 `is_shim == false`
    //   的 guest 块，dispatcher 于是把桩当 guest 代码跑（而且这张块还会被 SMC 反复丢掉）。
    //   "这个 EA 是 shim" 必须以 installShim 为准。
    {
        g_current_case = "install-shim-replaces-guest-block";
        g_bump_calls = 0;
        lift::Machine m;
        std::string err;
        LIFT_CHECK(m.initBare(&err));
        LIFT_CHECK(m.prepareStack(&err));
        const std::vector<std::uint8_t> bytes = {0x90, 0x90, 0x90, 0xC3};  // nop;nop;nop;ret
        LIFT_CHECK(m.placeCode(0x7FFE0200u, bytes.data(), bytes.size(), &err));
        lift::Block* guest = m.cache().get(0x7FFE0200u);  // 先按 guest 代码解出来（= 桩页字节被当代码）
        LIFT_CHECK(guest != nullptr && !guest->is_shim);
        m.shims().configureStubPage(0x7FFE0200u, 0x10u);
        lift::ShimDef* def = m.shims().addRuntime("dispatch_test", "Bump", lift::Conv::Stdcall, 0, &shimBump);
        LIFT_CHECK(def != nullptr && def->stub_ea == 0x7FFE0200u);
        if (def) {
            lift::Block* installed = m.cache().installShim(def->stub_ea, def->id);
            LIFT_CHECK(installed != nullptr && installed->is_shim);
            LIFT_CHECK_EQ_U32(installed ? installed->shim_id : 0xFFFFFFFFu, def->id);
            LIFT_CHECK(m.cache().peek(def->stub_ea) == installed);
            // 幂等：同一条 shim 再装一次 ⇒ 同一个块，⛔ 不新建、不改 id
            LIFT_CHECK(m.cache().installShim(def->stub_ea, def->id) == installed);
        }
    }

    // ⑪ 未对齐 `movaps xmm0, m128` ⇒ **响亮失败**（SDM：对齐违例 #GP；本仓没有 guest 异常投递，
    //   所以落在 `unimplemented()`）。修掉守卫（`sseRequireAligned` 恒 true）时这条会红：
    //   它会静默按 movups 搬 —— 把"程序有 bug"变成静默的数据搬运。
    {
        g_current_case = "unaligned-movaps-is-loud";
        lift::Machine m;
        // 0F 28 /r = movaps xmm0, m128；绝对地址 0x480003 **不是** 16 的倍数。
        const std::vector<std::uint8_t> code = {0x0F, 0x28, 0x05, 0x03, 0x00, 0x48, 0x00,  // movaps xmm0,[0x480003]
                                                0xC3};                                      // ret
        LIFT_CHECK(boot(m, code));
        std::string err;
        // ★ 源区域**先提交**（16 字节可辨认的字节）：守卫若被改成恒 true，这条会"静默搬成功"然后
        //   死在断言上（reason/insns/eip/xmm），而不是死在一次访存 fault 上 —— 那才看得清红的原因。
        const std::vector<std::uint8_t> pattern(16, 0x5A);
        LIFT_CHECK(m.placeData(0x00480003u, pattern.data(), pattern.size(), &err));
        m.cpu().xmm[0][0] = 0x11111111u;
        m.cpu().xmm[0][1] = 0x22222222u;
        m.cpu().xmm[0][2] = 0x33333333u;
        m.cpu().xmm[0][3] = 0x44444444u;
        const lift::RunResult r = run(m);
        LIFT_CHECK(r.reason == lift::StopReason::Unimplemented);
        LIFT_CHECK_EQ_I64(r.insns, 0);  // 一条都没执行（⛔ 不是"先搬了再报错"）
        LIFT_CHECK_EQ_U32(m.cpu().eip, 0x00401000u);
        LIFT_CHECK_EQ_U32(m.cpu().xmm[0][0], 0x11111111u);  // 目标 xmm 四个 lane 全未动
        LIFT_CHECK_EQ_U32(m.cpu().xmm[0][1], 0x22222222u);
        LIFT_CHECK_EQ_U32(m.cpu().xmm[0][2], 0x33333333u);
        LIFT_CHECK_EQ_U32(m.cpu().xmm[0][3], 0x44444444u);
    }

    // ⑫ 未对齐 `movdqa m128, xmm0`（**store** 路径）⇒ 同样响亮失败，且目标 16 字节**一个都没写**。
    //   store 与 load 是两条独立路径（各查一次 `sseRequireAligned`）⇒ 只测 load 会漏掉一条。
    {
        g_current_case = "unaligned-movdqa-store-is-loud";
        lift::Machine m;
        // 66 0F 7F /r = movdqa m128, xmm0；目标 0x480003 未对齐。
        const std::vector<std::uint8_t> code = {0x66, 0x0F, 0x7F, 0x05, 0x03, 0x00, 0x48, 0x00,  // movdqa [0x480003],xmm0
                                                0xC3};                                            // ret
        LIFT_CHECK(boot(m, code));
        std::string err;
        const std::vector<std::uint8_t> pattern(16, 0xCC);  // 目标区域先铺可辨认的字节
        LIFT_CHECK(m.placeData(0x00480003u, pattern.data(), pattern.size(), &err));
        m.cpu().xmm[0][0] = 0x11111111u;
        m.cpu().xmm[0][1] = 0x22222222u;
        m.cpu().xmm[0][2] = 0x33333333u;
        m.cpu().xmm[0][3] = 0x44444444u;
        const lift::RunResult r = run(m);
        LIFT_CHECK(r.reason == lift::StopReason::Unimplemented);
        LIFT_CHECK_EQ_I64(r.insns, 0);
        LIFT_CHECK_EQ_U32(m.cpu().eip, 0x00401000u);
        for (std::uint32_t i = 0; i < 16; ++i) {
            LIFT_CHECK_EQ_U32(m.mem().rd8(0x00480003u + i), 0xCCu);  // 目标 16 字节逐字节未写
        }
        LIFT_CHECK_EQ_U32(m.cpu().xmm[0][0], 0x11111111u);  // 源也没被动（本来就不该写源）
    }

    return finish("dispatch");
}
