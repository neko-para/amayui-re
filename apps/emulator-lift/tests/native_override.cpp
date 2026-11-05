// emulator-lift · guest 函数级 native override 的 pure 夹具
//
// 判据（红了意味着什么）：
//   ① **差分**：同一段 guest 参考实现（cdecl / `rep movsb` / `rep stosb`），解释器跑 vs override 跑，
//      目的内存 / EAX / ESP / 被调用方保存寄存器 / EIP 必须一致；不一致 ⇒ override 的 ABI 或语义错了。
//   ② **回退**：native 实现返回 false 时，dispatcher 必须继续解释原字节（结果仍与基线一致），
//      并把 fallback 记到账上 —— ⛔ 不许静默跳过、也不许直接 trap。
//   ③ **ABI**：cdecl 只弹返回地址；stdcall / thiscall 由被调用方再弹 arg_bytes；ECX = this。
// ★ 这里不测 profile / 首帧性能：那些用 `--native-overrides` 的真实 boot A/B（见 requirement 正文）。
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "check.h"
#include "fixture.h"
#include "runtime/native_overrides.h"

namespace {

constexpr std::uint32_t kFnEa = 0x00403000u;
constexpr std::uint32_t kOtherFnEa = 0x00404000u;

// int __cdecl ref_memcpy(void *dst, const void *src, size_t n)
//   push ebp; mov ebp, esp; push esi; push edi; mov edi,[ebp+8]; mov esi,[ebp+0Ch];
//   mov ecx,[ebp+10h]; rep movsb; mov eax,[ebp+8]; pop edi; pop esi; pop ebp; ret
const std::uint8_t kMemcpyRef[] = {0x55, 0x8B, 0xEC, 0x56, 0x57, 0x8B, 0x7D, 0x08, 0x8B, 0x75, 0x0C, 0x8B,
                                   0x4D, 0x10, 0xF3, 0xA4, 0x8B, 0x45, 0x08, 0x5F, 0x5E, 0x5D, 0xC3};

// void *__cdecl ref_memset(void *dst, int value, size_t n)
//   push ebp; mov ebp, esp; push edi; mov edi,[ebp+8]; mov eax,[ebp+0Ch];
//   mov ecx,[ebp+10h]; rep stosb; mov eax,[ebp+8]; pop edi; pop ebp; ret
const std::uint8_t kMemsetRef[] = {0x55, 0x8B, 0xEC, 0x57, 0x8B, 0x7D, 0x08, 0x8B, 0x45, 0x0C,
                                   0x8B, 0x4D, 0x10, 0xF3, 0xAA, 0x8B, 0x45, 0x08, 0x5F, 0x5D, 0xC3};

bool rejectNative(lift::X86&, lift::GuestMemory&, std::uint32_t*) { return false; }

// 调一个 **guest cdecl 函数**：push 参数 → call → add esp,N → ret（哨兵）。
// ★ 不能直接用 Fixture::call：那个是给 shim 用的，shim 框架自己清栈；guest cdecl 要调用方清。
std::uint32_t callCdecl(lift_fixture::Fixture& fx, std::uint32_t fn_ea, std::initializer_list<std::uint32_t> args) {
    const std::uint32_t code_ea = fx.code_cursor;
    fx.code_cursor += 0x100u;
    const std::vector<std::uint32_t> v(args);
    lift_fixture::Asm a(code_ea);
    for (auto it = v.rbegin(); it != v.rend(); ++it) a.pushImm(*it);
    a.callRel(fn_ea);
    a.addEspImm8(static_cast<std::uint8_t>(v.size() * 4u));
    a.ret();
    if (!fx.m.placeCode(code_ea, a.b.data(), a.b.size(), &fx.err)) {
        fx.ok = false;
        return 0;
    }
    fx.rearmStack();
    fx.runCode(code_ea);
    return fx.m.cpu().eax();
}

bool addThisNative(lift::X86& cpu, lift::GuestMemory&, std::uint32_t* result) {
    *result = cpu.ecx() + cpu.arg(0) + cpu.arg(1) + cpu.arg(2);
    return true;
}

struct Snapshot {
    std::uint32_t eax = 0;
    std::uint32_t esp = 0;
    std::uint32_t ebx = 0;
    std::uint32_t esi = 0;
    std::uint32_t edi = 0;
    std::uint32_t ebp = 0;
    std::uint32_t eip = 0;
    std::vector<std::uint8_t> dst;
};

Snapshot runMemcpy(bool with_override) {
    lift_fixture::Fixture fx;
    LIFT_CHECK(fx.ok);
    std::vector<std::uint8_t> src(64);
    for (std::size_t i = 0; i < src.size(); ++i) src[i] = static_cast<std::uint8_t>(0x30u + i * 7u);
    const std::uint32_t src_ea = fx.data(src.data(), src.size());
    const std::uint32_t dst_ea = fx.zeroData(src.size());
    LIFT_CHECK(fx.m.placeCode(kFnEa, kMemcpyRef, sizeof(kMemcpyRef), &fx.err));
    if (with_override) {
        std::string err;
        LIFT_CHECK(fx.m.overrides().add(kFnEa, "ref_memcpy", lift::GuestConv::Cdecl, 12, lift::nativeMemcpy, &err));
    }
    const std::uint32_t ret = callCdecl(fx, kFnEa, {dst_ea, src_ea, static_cast<std::uint32_t>(src.size())});
    LIFT_CHECK(fx.ok);
    if (with_override) {
        LIFT_CHECK_EQ_U32(fx.m.overrides().at(0)->hits, 1u);
        LIFT_CHECK_EQ_U32(fx.m.overrides().at(0)->fallbacks, 0u);
    }
    Snapshot s;
    s.eax = ret;
    s.esp = fx.m.cpu().esp();
    s.ebx = fx.m.cpu().ebx();
    s.esi = fx.m.cpu().esi();
    s.edi = fx.m.cpu().edi();
    s.ebp = fx.m.cpu().ebp();
    s.eip = fx.m.cpu().eip;
    const std::uint8_t* p = fx.m.mem().host(dst_ea);
    s.dst.assign(p, p + src.size());
    return s;
}

Snapshot runMemset(bool with_override) {
    lift_fixture::Fixture fx;
    LIFT_CHECK(fx.ok);
    const std::uint32_t dst_ea = fx.zeroData(48);
    // 先填非零，确认 memset 真的覆盖了
    for (std::uint32_t i = 0; i < 48; ++i) fx.m.mem().wr8(dst_ea + i, 0xEE);
    LIFT_CHECK(fx.m.placeCode(kFnEa, kMemsetRef, sizeof(kMemsetRef), &fx.err));
    if (with_override) {
        std::string err;
        LIFT_CHECK(fx.m.overrides().add(kFnEa, "ref_memset", lift::GuestConv::Cdecl, 12, lift::nativeMemset, &err));
    }
    const std::uint32_t ret = callCdecl(fx, kFnEa, {dst_ea, 0xA5u, 48u});
    LIFT_CHECK(fx.ok);
    if (with_override) {
        LIFT_CHECK_EQ_U32(fx.m.overrides().at(0)->hits, 1u);
        LIFT_CHECK_EQ_U32(fx.m.overrides().at(0)->fallbacks, 0u);
    }
    Snapshot s;
    s.eax = ret;
    s.esp = fx.m.cpu().esp();
    s.ebx = fx.m.cpu().ebx();
    s.esi = fx.m.cpu().esi();
    s.edi = fx.m.cpu().edi();
    s.ebp = fx.m.cpu().ebp();
    s.eip = fx.m.cpu().eip;
    const std::uint8_t* p = fx.m.mem().host(dst_ea);
    s.dst.assign(p, p + 48);
    return s;
}

void compare(const Snapshot& a, const Snapshot& b, const char* what) {
    LIFT_CHECK(a.eax == b.eax);
    LIFT_CHECK(a.esp == b.esp);
    LIFT_CHECK(a.ebx == b.ebx);
    LIFT_CHECK(a.esi == b.esi);
    LIFT_CHECK(a.edi == b.edi);
    LIFT_CHECK(a.ebp == b.ebp);
    LIFT_CHECK(a.eip == b.eip);
    LIFT_CHECK(a.dst.size() == b.dst.size());
    if (a.dst.size() == b.dst.size()) {
        LIFT_CHECK(std::memcmp(a.dst.data(), b.dst.data(), a.dst.size()) == 0);
    }
    if (a.eax != b.eax || a.esp != b.esp || a.eip != b.eip || a.dst != b.dst) {
        std::fprintf(stderr, "  [compare failed: %s]\n", what);
    }
}

}  // namespace

int main() {
    // ---- ① memcpy：解释器（rep movsb） vs native ----
    {
        const Snapshot base = runMemcpy(false);
        const Snapshot ovr = runMemcpy(true);
        compare(base, ovr, "memcpy");
    }
    // ---- ① memset：解释器（rep stosb） vs native ----
    {
        const Snapshot base = runMemset(false);
        const Snapshot ovr = runMemset(true);
        compare(base, ovr, "memset");
    }
    // ---- ② 回退：native 返回 false ⇒ 解释原字节，结果与基线一致 ----
    {
        lift_fixture::Fixture fx;
        LIFT_CHECK(fx.ok);
        std::vector<std::uint8_t> src(32, 0x5A);
        const std::uint32_t src_ea = fx.data(src.data(), src.size());
        const std::uint32_t dst_ea = fx.zeroData(src.size());
        LIFT_CHECK(fx.m.placeCode(kFnEa, kMemcpyRef, sizeof(kMemcpyRef), &fx.err));
        std::string err;
        LIFT_CHECK(fx.m.overrides().add(kFnEa, "reject", lift::GuestConv::Cdecl, 12, rejectNative, &err));
        callCdecl(fx, kFnEa, {dst_ea, src_ea, static_cast<std::uint32_t>(src.size())});
        LIFT_CHECK(fx.ok);
        LIFT_CHECK_EQ_U32(fx.m.overrides().at(0)->hits, 0u);
        LIFT_CHECK_EQ_U32(fx.m.overrides().at(0)->fallbacks, 1u);
        LIFT_CHECK(std::memcmp(fx.m.mem().host(dst_ea), src.data(), src.size()) == 0);
    }
    // ---- ① 未覆盖的 EA：照旧解释（这条同时钉住"表里没有的 EA 不许被拦"）----
    {
        lift_fixture::Fixture fx;
        LIFT_CHECK(fx.ok);
        const std::uint32_t dst_ea = fx.zeroData(16);
        LIFT_CHECK(fx.m.placeCode(kOtherFnEa, kMemsetRef, sizeof(kMemsetRef), &fx.err));
        const std::uint32_t ret = callCdecl(fx, kOtherFnEa, {dst_ea, 0x11u, 16u});
        LIFT_CHECK(fx.ok);
        LIFT_CHECK_EQ_U32(ret, dst_ea);
        for (int i = 0; i < 16; ++i) LIFT_CHECK_EQ_U32(fx.m.mem().rd8(dst_ea + static_cast<std::uint32_t>(i)), 0x11u);
    }
    // ---- ① **jmp 进入** override EA 必须回退解释器（那里没有本次 call 的返回地址）----
    {
        lift_fixture::Fixture fx;
        LIFT_CHECK(fx.ok);
        std::vector<std::uint8_t> src(32, 0x77);
        const std::uint32_t src_ea = fx.data(src.data(), src.size());
        const std::uint32_t dst_ea = fx.zeroData(src.size());
        LIFT_CHECK(fx.m.placeCode(kFnEa, kMemcpyRef, sizeof(kMemcpyRef), &fx.err));
        std::string err;
        LIFT_CHECK(fx.m.overrides().add(kFnEa, "ref_memcpy", lift::GuestConv::Cdecl, 12, lift::nativeMemcpy, &err));
        const std::uint32_t esp = fx.m.layout().stack_top - 32u;
        fx.m.cpu().esp() = esp;
        fx.m.mem().wr32(esp, lift_fixture::kSentinel);
        fx.m.mem().wr32(esp + 4, dst_ea);
        fx.m.mem().wr32(esp + 8, src_ea);
        fx.m.mem().wr32(esp + 12, static_cast<std::uint32_t>(src.size()));
        fx.runCode(kFnEa);  // 直接设 eip（不是 call）⇒ 不许命中 override
        LIFT_CHECK(fx.ok);
        LIFT_CHECK_EQ_U32(fx.m.overrides().at(0)->hits, 0u);
        LIFT_CHECK(std::memcmp(fx.m.mem().host(dst_ea), src.data(), src.size()) == 0);
    }
    // ---- ① `sub_401000` 的返回值是**符号**（`.lst` 401066: sar eax,1Fh; or eax,1），不是字节差 ----
    {
        lift::Machine m;
        std::string err;
        LIFT_CHECK(m.initBare(&err));
        LIFT_CHECK(m.prepareStack(&err));
        const std::uint8_t a[] = {1, 2, 3, 4};
        const std::uint8_t b[] = {1, 2, 4, 4};
        const std::uint8_t c[] = {1, 2, 3, 4};
        LIFT_CHECK(m.placeData(0x00410000u, a, sizeof(a), &err));
        LIFT_CHECK(m.placeData(0x00410010u, b, sizeof(b), &err));
        LIFT_CHECK(m.placeData(0x00410020u, c, sizeof(c), &err));
        auto& cpu = m.cpu();
        auto& mem = m.mem();
        const std::uint32_t esp = m.layout().stack_top - 32u;
        cpu.esp() = esp;
        mem.wr32(esp, lift_fixture::kSentinel);
        auto sign_of = [&](std::uint32_t left, std::uint32_t right) {
            mem.wr32(esp + 4, left);
            mem.wr32(esp + 8, right);
            mem.wr32(esp + 12, 4u);
            std::uint32_t r = 0xDEADBEEFu;
            LIFT_CHECK(lift::nativeSignMemcmp(cpu, mem, &r));
            return r;
        };
        LIFT_CHECK_EQ_U32(sign_of(0x00410000u, 0x00410010u), 0xFFFFFFFFu);  // 3 < 4
        LIFT_CHECK_EQ_U32(sign_of(0x00410010u, 0x00410000u), 1u);           // 4 > 3
        LIFT_CHECK_EQ_U32(sign_of(0x00410000u, 0x00410020u), 0u);           // equal
    }
    // ---- ① `_strlen`：直接跑 native 实现（边界字节也测；safe mode 边界不越过 4 GiB）----
    {
        lift::Machine m;
        std::string err;
        LIFT_CHECK(m.initBare(&err));
        LIFT_CHECK(m.prepareStack(&err));
        const char s[] = "abc\0def";
        LIFT_CHECK(m.placeData(0x00410000u, s, sizeof(s), &err));
        auto& cpu = m.cpu();
        auto& mem = m.mem();
        const std::uint32_t esp = m.layout().stack_top - 32u;
        cpu.esp() = esp;
        mem.wr32(esp, lift_fixture::kSentinel);
        mem.wr32(esp + 4, 0x00410000u);
        std::uint32_t r = 0xDEADBEEFu;
        LIFT_CHECK(lift::nativeStrlen(cpu, mem, &r));
        LIFT_CHECK_EQ_U32(r, 3u);
    }
    // ---- ③ ABI：cdecl 只弹返回地址；stdcall / thiscall 由被调用方弹 arg_bytes ----
    {
        lift::Machine m;
        std::string err;
        LIFT_CHECK(m.initBare(&err));
        LIFT_CHECK(m.prepareStack(&err));
        auto& cpu = m.cpu();
        auto& mem = m.mem();
        const std::uint32_t top = m.layout().stack_top;
        const std::uint32_t saved_esp = top - 0x80u;
        auto setup_frame = [&]() {
            cpu.esp() = saved_esp;
            mem.wr32(saved_esp, 0xFFFFFFFFu);
            mem.wr32(saved_esp + 4, 0x11u);
            mem.wr32(saved_esp + 8, 0x22u);
            mem.wr32(saved_esp + 12, 0x33u);
            cpu.ecx() = 0x44u;
            cpu.eax() = 0;
        };
        lift::NativeOverrideTable table;
        LIFT_CHECK(table.add(0x00403000u, "this_add", lift::GuestConv::Thiscall, 12, addThisNative, &err));
        setup_frame();
        const char* why = nullptr;
        LIFT_CHECK(table.invoke(*table.find(0x00403000u), cpu, mem, &why));
        LIFT_CHECK_EQ_U32(cpu.eax(), 0x44u + 0x11u + 0x22u + 0x33u);
        LIFT_CHECK_EQ_U32(cpu.esp(), saved_esp + 16u);  // 4（返回地址）+ 12（callee 清栈）
        LIFT_CHECK_EQ_U32(cpu.eip, 0xFFFFFFFFu);
        LIFT_CHECK_EQ_U32(table.find(0x00403000u)->hits, 1u);

        LIFT_CHECK(table.add(0x00403010u, "cdecl_add", lift::GuestConv::Cdecl, 12, addThisNative, &err));
        setup_frame();
        LIFT_CHECK(table.invoke(*table.find(0x00403010u), cpu, mem, &why));
        LIFT_CHECK_EQ_U32(cpu.esp(), saved_esp + 4u);  // 只弹返回地址
        LIFT_CHECK_EQ_U32(cpu.eax(), 0x44u + 0x11u + 0x22u + 0x33u);

        LIFT_CHECK(table.add(0x00403020u, "stdcall_add", lift::GuestConv::Stdcall, 12, addThisNative, &err));
        setup_frame();
        LIFT_CHECK(table.invoke(*table.find(0x00403020u), cpu, mem, &why));
        LIFT_CHECK_EQ_U32(cpu.esp(), saved_esp + 16u);
    }
    // ---- ③ 未提交的栈帧 ⇒ 拒绝执行（回退），不许在宿主侧越界读 ----
    {
        lift::Machine m;
        std::string err;
        LIFT_CHECK(m.initBare(&err));
        auto& cpu = m.cpu();
        cpu.esp() = 0x00001000u;  // 未提交
        lift::NativeOverrideTable table;
        LIFT_CHECK(table.add(0x00403000u, "this_add", lift::GuestConv::Thiscall, 12, addThisNative, &err));
        const char* why = nullptr;
        LIFT_CHECK(!table.invoke(*table.find(0x00403000u), cpu, m.mem(), &why));
        LIFT_CHECK(why != nullptr && *why != 0);
        LIFT_CHECK_EQ_U32(table.find(0x00403000u)->fallbacks, 1u);
    }

    // ---- ④ sub_436A80 的 LZSS 核：literal / match / 非压缩拷贝 ----
    {
        std::uint8_t ring[4096] = {};
        const std::uint8_t lit[] = {0x07, 'A', 'B', 'C'};  // 3 个 literal 位（LSB first）
        std::uint8_t out3[3] = {};
        LIFT_CHECK(lift::lzssDecode(lit, sizeof(lit), out3, sizeof(out3), ring));
        LIFT_CHECK(std::memcmp(out3, "ABC", 3) == 0);

        const std::uint8_t match[] = {0x01, 'A', 0x00, 0x00, 0x00};  // literal A，然后 offset=0 count=3
        std::uint8_t out4[4] = {};
        std::uint8_t ring2[4096] = {};
        LIFT_CHECK(lift::lzssDecode(match, sizeof(match), out4, sizeof(out4), ring2));
        LIFT_CHECK_EQ_U32(out4[0], 'A');
        LIFT_CHECK_EQ_U32(out4[1], 0u);
        LIFT_CHECK_EQ_U32(out4[2], 0u);
        LIFT_CHECK_EQ_U32(out4[3], 0u);

        const std::uint8_t raw[] = {'W', 'X', 'Y', 'Z'};
        std::uint8_t copy[4] = {};
        LIFT_CHECK(lift::lzssDecode(raw, sizeof(raw), copy, sizeof(copy), ring));
        LIFT_CHECK(std::memcmp(copy, raw, 4) == 0);
    }

    return lift_test::finish("native-override");
}
