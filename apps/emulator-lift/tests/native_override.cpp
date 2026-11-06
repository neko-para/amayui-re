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
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "check.h"
#include "fixture.h"
#include "runtime/com.h"
#include "runtime/d3d9.h"
#include "runtime/ddraw_surface.h"
#include "runtime/native_overrides.h"
#include "runtime/surface.h"

namespace {

// 按字节读一个 little-endian u32（目标缓冲不保证 4 字节对齐；⛔ 不用 reinterpret_cast 去解引用）
std::uint32_t readU32(const std::uint8_t* p) {
    std::uint32_t v = 0;
    for (int i = 0; i < 4; ++i) v |= static_cast<std::uint32_t>(p[i]) << (8 * i);
    return v;
}

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

std::uint32_t g_nested_fn_ea = 0;

bool nestedCallNative(lift::X86& cpu, lift::GuestMemory&, std::uint32_t* result) {
    *result = 0;
    lift::startNestedGuestCall(
        cpu, g_nested_fn_ea, {0x11u, 0x22u}, 4u, "test:nested",
        [](lift::X86&, std::uint32_t callback_result, std::uint32_t* out) {
            *out = callback_result + 1u;
            return true;
        },
        nullptr);
    return false;  // invoke() 通过 g_nested_started 识别“已铺帧”
}

bool nestedThisNative(lift::X86& cpu, lift::GuestMemory&, std::uint32_t* result) {
    *result = 0;
    lift::startNestedGuestCall(
        cpu, g_nested_fn_ea, {0x11u, 0x22u}, 16u, "test:nested-thiscall",
        [](lift::X86&, std::uint32_t callback_result, std::uint32_t* out) {
            *out = callback_result;
            return true;
        },
        nullptr);
    return false;
}

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

// 真纹理夹具：用 d3d9 自己的工厂建设备 / 纹理状态（⛔ 不在测试里复制 kTx* 偏移），
// 再把状态块挂到 COM 对象上（`obj+8` = 状态块，与 boot 里 guest 看到的是同一条路径）。
struct TextureRig {
    lift::Machine m;
    std::string err;
    bool ok = false;
    std::uint32_t device = 0;
    std::uint32_t texture = 0;

    bool boot(std::uint32_t w, std::uint32_t h, std::uint32_t format) {
        if (!m.initBare(&err)) return false;
        if (!m.prepareStack(&err)) return false;
        if (!m.heap().init(&m.mem(), m.layout().heap_base, 0x00100000u)) return false;
        m.shims().configureStubPage(0x7FF90000u, 0x1000u);
        lift::X86& cpu = m.cpu();
        lift::d3d9::CreateRequest req;
        req.device_type = lift::d3d9::kDevTypeHal;
        req.behavior_flags = 0x44u;
        req.present.width = 64u;
        req.present.height = 64u;
        req.present.format = lift::d3d9::kFmtX8R8G8B8;
        req.present.windowed = true;
        const std::uint32_t dev_state = lift::d3d9::createDeviceState(cpu, req, &err);
        if (dev_state == 0) return false;
        // 只要一个非空的方法表（createObject 拒绝空 vtable）；本夹具从不按 vtable 调它。
        const std::vector<lift::ComMethod> slots = {{0u, "GetCaps", lift::Conv::Stdcall, 1, nullptr}};
        device = cpu.com->createObject(cpu, "IDirect3DDevice9", slots, dev_state);
        if (device == 0) return false;
        const std::uint32_t tex_state =
            lift::d3d9::createTextureState(cpu, device, w, h, 1u, 0u, format, 1u, &err);
        if (tex_state == 0) return false;
        texture = cpu.com->createObject(cpu, "IDirect3DTexture9", slots, tex_state);
        ok = texture != 0;
        return ok;
    }
};

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

    // ---- ⑤ sub_48B090 的像素核：bottom-up 源 / top-down 目标 / alpha 按**目标行序** ----
    // ★ alpha 的判据来自 `.lst`：0x48B49F `movzx eax, byte ptr [edi]` + 0x48B4BD `inc edi`，
    //   行循环（0x48B470）**不重置** ei/edi ⇒ 第 row 行第 col 列的 alpha 下标 = row*w+col，
    //   与源行序（bottom-up）**无关**。旧实现用 `alpha[srow*w+col]`，这里把它钉死。
    {
        std::uint8_t pal[8] = {0, 0, 0, 0, 0x10, 0x20, 0x30, 0};
        std::uint8_t src[2][8] = {};  // stride 8，2x2；内存 row0 是目标最后一行
        src[0][0] = 0;
        src[0][1] = 1;
        src[1][0] = 1;
        src[1][1] = 0;
        std::uint32_t dst[2][2] = {};
        lift::convertPal8To32(&src[0][0], 8, 2, 2, pal, reinterpret_cast<std::uint8_t*>(&dst[0][0]), 8, nullptr);
        LIFT_CHECK_EQ_U32(dst[0][0], 0xFF302010u);
        LIFT_CHECK_EQ_U32(dst[0][1], 0xFF000000u);
        LIFT_CHECK_EQ_U32(dst[1][0], 0xFF000000u);
        LIFT_CHECK_EQ_U32(dst[1][1], 0xFF302010u);

        const std::uint8_t alpha[4] = {0x00, 0x7F, 0xFF, 0xAA};
        std::uint32_t dst2[2][2] = {};
        lift::convertPal8To32(&src[0][0], 8, 2, 2, pal, reinterpret_cast<std::uint8_t*>(&dst2[0][0]), 8, alpha);
        // 目标 row0 = 源 row1（alpha[0], alpha[1]）；目标 row1 = 源 row0（alpha[2], alpha[3]）
        LIFT_CHECK_EQ_U32(dst2[0][0], 0x00302010u);  // 源 row1 col0（索引 1）⇒ alpha[0]
        LIFT_CHECK_EQ_U32(dst2[0][1], 0x7F000000u);  // 源 row1 col1（索引 0）⇒ alpha[1]
        LIFT_CHECK_EQ_U32(dst2[1][0], 0xFF000000u);  // 源 row0 col0（索引 0）⇒ alpha[2]
        LIFT_CHECK_EQ_U32(dst2[1][1], 0xAA302010u);  // 源 row0 col1（索引 1）⇒ alpha[3]
    }

    // ---- ⑤b 24bpp / 32bpp 源（`.lst` 的 default 分支：`(_WORD)bpp == 32` 走逐字节搬运）----
    {
        // 24bpp：每像素 3 字节，目标 4 字节（A=0xFF；有 alpha 时按目标行序）
        std::uint8_t src24[2][8] = {};  // stride 8（2 像素 × 3B，DIB 4 字节对齐）
        src24[0][0] = 0x01;
        src24[0][1] = 0x02;
        src24[0][2] = 0x03;
        src24[0][3] = 0x04;
        src24[0][4] = 0x05;
        src24[0][5] = 0x06;
        src24[1][0] = 0x11;
        src24[1][1] = 0x12;
        src24[1][2] = 0x13;
        src24[1][3] = 0x14;
        src24[1][4] = 0x15;
        src24[1][5] = 0x16;
        std::uint8_t dst24[2][8] = {};
        lift::convert24To32(&src24[0][0], 8, 2, 2, &dst24[0][0], 8, nullptr);
        LIFT_CHECK_EQ_U32(readU32(dst24[0]), 0xFF131211u);      // 目标 row0 = 源 row1 的第一个像素
        LIFT_CHECK_EQ_U32(readU32(dst24[0] + 4), 0xFF161514u);
        LIFT_CHECK_EQ_U32(readU32(dst24[1]), 0xFF030201u);
        LIFT_CHECK_EQ_U32(readU32(dst24[1] + 4), 0xFF060504u);
        const std::uint8_t alpha24[4] = {0x31, 0x32, 0x33, 0x34};  // 目标行序
        std::uint8_t dst24b[2][8] = {};
        lift::convert24To32(&src24[0][0], 8, 2, 2, &dst24b[0][0], 8, alpha24);
        LIFT_CHECK_EQ_U32(readU32(dst24b[0]), 0x31131211u);
        LIFT_CHECK_EQ_U32(readU32(dst24b[0] + 4), 0x32161514u);
        LIFT_CHECK_EQ_U32(readU32(dst24b[1]), 0x33030201u);
        LIFT_CHECK_EQ_U32(readU32(dst24b[1] + 4), 0x34060504u);

        // 32bpp：4 字节原样搬运（源第 4 字节 = 目标 alpha）；alpha 参数**被忽略**
        std::uint8_t src32[2][8] = {};
        for (int i = 0; i < 8; ++i) src32[0][i] = static_cast<std::uint8_t>(0xA0 + i);
        for (int i = 0; i < 8; ++i) src32[1][i] = static_cast<std::uint8_t>(0x10 + i);
        std::uint8_t dst32[2][8] = {};
        lift::convert32To32(&src32[0][0], 8, 2, 2, &dst32[0][0], 8);
        LIFT_CHECK_EQ_U32(readU32(dst32[0]), 0x13121110u);
        LIFT_CHECK_EQ_U32(readU32(dst32[0] + 4), 0x17161514u);
        LIFT_CHECK_EQ_U32(readU32(dst32[1]), 0xA3A2A1A0u);
        LIFT_CHECK_EQ_U32(readU32(dst32[1] + 4), 0xA7A6A5A4u);
        // convertImageTo32：不支持的源 bpp 显式拒绝（不是"走 default 当 24bpp"）
        lift::ImageSource bad;
        bad.bpp = 4;
        bad.pixels = &src32[0][0];
        bad.stride = 8;
        bad.width = 2;
        bad.height = 2;
        std::uint8_t scratch[16] = {};
        std::string bad_why;
        LIFT_CHECK(!lift::convertImageTo32(bad, scratch, 8, &bad_why));
        LIFT_CHECK(!bad_why.empty());
    }

    // ---- ⑥ native override 的嵌套 guest 调用：native → guest → native → 原返回 ----
    {
        lift_fixture::Fixture fx;
        LIFT_CHECK(fx.ok);
        // guest 回调：[esp+4] 是第一个参数；retn 8 弹掉两个参数，EAX=0x11。
        const std::uint8_t nested_ref[] = {0x8B, 0x44, 0x24, 0x04, 0xC2, 0x08, 0x00};
        LIFT_CHECK(fx.m.placeCode(kOtherFnEa, nested_ref, sizeof(nested_ref), &fx.err));
        g_nested_fn_ea = kOtherFnEa;
        std::string err;
        LIFT_CHECK(fx.m.overrides().add(kFnEa, "nested_call", lift::GuestConv::Cdecl, 0, nestedCallNative, &err));
        const std::uint32_t ret = callCdecl(fx, kFnEa, {});
        LIFT_CHECK(fx.ok);
        LIFT_CHECK_EQ_U32(ret, 0x12u);
        LIFT_CHECK_EQ_U32(fx.m.overrides().at(0)->hits, 1u);
        LIFT_CHECK_EQ_U32(fx.m.overrides().at(0)->fallbacks, 0u);
    }

    // ---- ⑦ thiscall 的嵌套调用：native 帧 cleanup = 4 + 12，模拟 sub_48B090 ----
    {
        lift_fixture::Fixture fx;
        LIFT_CHECK(fx.ok);
        // guest 回调: mov eax,[esp+4]; add eax,[esp+8]; retn 8
        const std::uint8_t sum_ref[] = {0x8B, 0x44, 0x24, 0x04, 0x03, 0x44, 0x24, 0x08, 0xC2, 0x08, 0x00};
        LIFT_CHECK(fx.m.placeCode(kOtherFnEa, sum_ref, sizeof(sum_ref), &fx.err));
        g_nested_fn_ea = kOtherFnEa;
        std::string err;
        LIFT_CHECK(fx.m.overrides().add(kFnEa, "nested_this", lift::GuestConv::Thiscall, 12, nestedThisNative, &err));
        const std::uint32_t code_ea = fx.code_cursor;
        fx.code_cursor += 0x100u;
        lift_fixture::Asm a(code_ea);
        a.u8(0xB9);
        a.u32(0x44u);  // mov ecx, 0x44
        a.pushImm(0x33u);
        a.pushImm(0x22u);
        a.pushImm(0x11u);
        a.callRel(kFnEa);
        // thiscall：被调用方（override 的 cleanup=16）弹返回地址 + 12 参数；调用方不再 add esp。
        a.ret();
        LIFT_CHECK(fx.m.placeCode(code_ea, a.b.data(), a.b.size(), &fx.err));
        fx.rearmStack();
        fx.runCode(code_ea);
        LIFT_CHECK_EQ_U32(fx.m.cpu().eax(), 0x33u);
        LIFT_CHECK_EQ_U32(fx.m.overrides().at(0)->hits, 1u);
        LIFT_CHECK_EQ_U32(fx.m.overrides().at(0)->fallbacks, 0u);
    }

    // ---- ⑧ known 表分层：stable 项可被 all 启用；debug 项必须显式 ----
    {
        const lift::KnownNative* stable = lift::findKnownNative("_memcpy");
        const lift::KnownNative* upload = lift::findKnownNative("sub_48B090");
        LIFT_CHECK(stable != nullptr && !stable->debug);
        LIFT_CHECK(upload != nullptr && !upload->debug);
    }

    // ---- ⑨ sub_48B090 的**准入闸**：不支持的东西在铺 guest 帧之前回退（无副作用、无半状态）----
    //   ★ 为什么这条重要：override 一旦 startNestedGuestCall() 就**不能**回退（EIP/ESP 已交给
    //     GuestCallStack），所以"源格式不支持 / 色键未实现 / 目标函数认不出"必须在 Create **之前**
    //     判掉 —— 否则屏幕上是"新纹理已建、像素一个字没写"。
    {
        lift::Machine m;
        std::string err;
        LIFT_CHECK(m.initBare(&err));
        LIFT_CHECK(m.prepareStack(&err));
        auto& cpu = m.cpu();
        auto& mem = m.mem();
        const std::uint32_t img = 0x00430000u;
        const std::uint32_t self = 0x00420000u;
        std::vector<std::uint8_t> img_blk(0x440, 0);
        std::vector<std::uint8_t> self_blk(0x410, 0);
        LIFT_CHECK(m.placeData(img, img_blk.data(), img_blk.size(), &err));
        LIFT_CHECK(m.placeData(self, self_blk.data(), self_blk.size(), &err));
        const std::uint32_t src_ea = 0x00440000u;
        std::vector<std::uint8_t> src_blk(64, 0x5A);
        LIFT_CHECK(m.placeData(src_ea, src_blk.data(), src_blk.size(), &err));
        mem.wr32(img + 0x18, 3u);  // width
        mem.wr32(img + 0x1C, 2u);  // height
        mem.wr32(img + 0x43C, src_ea);
        const std::uint32_t esp = m.layout().stack_top - 0x40u;
        cpu.esp() = esp;
        mem.wr32(esp, lift_fixture::kSentinel);
        mem.wr32(esp + 4u, img);
        mem.wr32(esp + 8u, 0u);   // alpha = NULL
        mem.wr32(esp + 12u, 0u);  // key = 0
        cpu.ecx() = self;
        std::uint32_t r = 0xDEADBEEFu;
        auto expect_fallback = [&](const char* what) {
            LIFT_CHECK(!lift::nativeUpload48B090(cpu, mem, &r));
            LIFT_CHECK_EQ_U32(r, 0u);
            // 关键：**没有**铺 guest 回调帧（= 回退是干净的，dispatcher 会照旧解释原字节）
            LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(cpu.calls->depth()), 0u);
            if (cpu.calls->depth() != 0) std::fprintf(stderr, "  [%s] 铺了帧\n", what);
        };
        // ① 源 bpp = 4（guest 有分支，本线显式不支持）
        mem.wr16(img + 0x22, 4u);
        expect_fallback("bpp=4");
        // ② 源 bpp = 1
        mem.wr16(img + 0x22, 1u);
        expect_fallback("bpp=1");
        // ③ 源 bpp = 16（guest 的 switch 里走 default=24 分支；本线不猜）
        mem.wr16(img + 0x22, 16u);
        expect_fallback("bpp=16");
        // ④ 色键 ≠ 0（guest 的色键语义按源格式/alpha 组合，本线未实现 ⇒ 回退）
        mem.wr16(img + 0x22, 8u);
        mem.wr32(esp + 12u, 0x00FF00FFu);
        expect_fallback("color-key");
        mem.wr32(esp + 12u, 0u);
        // ⑤ 8bpp、参数都合法，但**没有 .lst**（认不出目标函数）⇒ 仍然回退
        expect_fallback("no-lst");
        // ⑥ 宽高越界
        mem.wr32(img + 0x18, 0u);
        expect_fallback("width=0");
    }

    // ---- ⑩ 真纹理（d3d9 的工厂 + COM 对象）：3×2 非方阵的 arena **逐字节** + 已锁拒绝 ----
    {
        TextureRig rig;
        LIFT_CHECK(rig.boot(3, 2, lift::d3d9::kFmtA8R8G8B8));
        if (rig.ok) {
            lift::X86& cpu = rig.m.cpu();
            lift::d3d9::TextureInfo ti;
            LIFT_CHECK(lift::d3d9::textureInfo(cpu, rig.texture, &ti, nullptr));
            LIFT_CHECK_EQ_U32(ti.bpp, 32u);
            LIFT_CHECK_EQ_U32(ti.pitch, 12u);  // 3 像素 × 4 B

            std::uint8_t pal[256 * 4] = {};
            pal[4] = 0x11;   // 索引 1：R
            pal[5] = 0x22;   //         G
            pal[6] = 0x33;   //         B
            pal[8] = 0x44;   // 索引 2：R
            pal[9] = 0x55;   //         G
            pal[10] = 0x66;  //         B
            // 源：stride = ((3*8+31)/32)*4 = 4；row0 是**目标最后一行**（bottom-up），
            // 每行尾 1 字节是 4 字节对齐的 padding（⛔ 不该被读）。
            const std::uint8_t pixels[8] = {0, 1, 2, 0xEE, 2, 1, 0, 0xEE};
            const std::uint8_t alpha[6] = {2, 3, 4, 5, 6, 7};  // 目标行序（不是源行序）
            lift::ImageSource s;
            s.bpp = 8;
            s.pixels = pixels;
            s.stride = 4;
            s.palette = pal;
            s.alpha = alpha;
            s.width = 3;
            s.height = 2;
            std::string why;
            LIFT_CHECK(lift::uploadPixelsToTexture(cpu, rig.texture, s, &why));
            if (!why.empty()) std::fprintf(stderr, "  upload why=%s\n", why.c_str());
            // 期望值**手算**（每像素 = A<<24 | B<<16 | G<<8 | R；下标见上面两张常量表）
            const std::uint8_t expected[24] = {
                0x44, 0x55, 0x66, 0x02,  // 目标 row0 col0 = 源 row1 col0（索引 2）· alpha[0]
                0x11, 0x22, 0x33, 0x03,  // 目标 row0 col1 = 源 row1 col1（索引 1）· alpha[1]
                0x00, 0x00, 0x00, 0x04,  // 目标 row0 col2 = 源 row1 col2（索引 0）· alpha[2]
                0x00, 0x00, 0x00, 0x05,  // 目标 row1 col0 = 源 row0 col0（索引 0）· alpha[3]
                0x11, 0x22, 0x33, 0x06,  // 目标 row1 col1 = 源 row0 col1（索引 1）· alpha[4]
                0x44, 0x55, 0x66, 0x07,  // 目标 row1 col2 = 源 row0 col2（索引 2）· alpha[5]
            };
            const std::uint8_t* arena = rig.m.mem().host(ti.arena);
            if (std::memcmp(arena, expected, sizeof(expected)) != 0) {
                std::fprintf(stderr, "  arena:");
                for (std::size_t i = 0; i < sizeof(expected); ++i) std::fprintf(stderr, " %02X", arena[i]);
                std::fprintf(stderr, "\n");
            }
            LIFT_CHECK(std::memcmp(arena, expected, sizeof(expected)) == 0);

            // 锁的记账：走的是 lockRect / unlockRect ⇒ locks 加一、结束时**不留锁**
            lift::d3d9::TextureInfo after;
            LIFT_CHECK(lift::d3d9::textureInfo(cpu, rig.texture, &after, nullptr));
            LIFT_CHECK_EQ_U32(after.locks, 1u);
            LIFT_CHECK(!after.locked);

            // 已锁状态：guest 先锁住 ⇒ 上传必须拒绝（不写像素、也不抢别人的锁）
            const std::uint32_t lr_ea = 0x00430000u;
            std::uint8_t zeros[8] = {};
            LIFT_CHECK(rig.m.placeData(lr_ea, zeros, sizeof(zeros), &rig.err));
            lift::d3d9::TextureInfo fresh;
            LIFT_CHECK(lift::d3d9::textureInfo(cpu, rig.texture, &fresh, nullptr));
            LIFT_CHECK(lift::d3d9::lockRect(cpu, fresh, 0u, lr_ea, 0u, 0u, &why));
            std::vector<std::uint8_t> frozen(arena, arena + sizeof(expected));
            std::string locked_why;
            LIFT_CHECK(!lift::uploadPixelsToTexture(cpu, rig.texture, s, &locked_why));
            LIFT_CHECK(locked_why.find("锁定") != std::string::npos);
            LIFT_CHECK(std::memcmp(arena, frozen.data(), frozen.size()) == 0);
            lift::d3d9::TextureInfo held;
            LIFT_CHECK(lift::d3d9::textureInfo(cpu, rig.texture, &held, nullptr));
            LIFT_CHECK(held.locked);
            LIFT_CHECK(lift::d3d9::unlockRect(cpu, held, 0u, &why));

            // 非 32bpp 目标（display:TextureBit=16 的形状）⇒ 显式失败，不按 32bpp 乱写
            TextureRig wide;
            LIFT_CHECK(wide.boot(3, 2, lift::d3d9::kFmtA4R4G4B4));
            if (wide.ok) {
                std::string wide_why;
                LIFT_CHECK(!lift::uploadPixelsToTexture(wide.m.cpu(), wide.texture, s, &wide_why));
                LIFT_CHECK(!wide_why.empty());
            }
        }
    }

    // =====================================================================================
    // ⑪ sub_429020（Map::findIndex）：**golden model 差分**
    //
    // 判据：同一份 guest 表夹具，native 实现 vs 一个独立写的 C++ 参考模型（按 `.lst` 的
    // 控制流重写：空槽 -1 / 先比 min(len) 字节再比长度 / 线性探测 / index=(index+1)%cap）。
    // ★ 全表没有空槽时 guest 会**死循环**（不是返回 -1）⇒ 期望 native 回退（返回 false）。
    // ★ 每个用例都比对"guest 内存未被写"（native 只读）。
    // ★ 这里不经过解释器：真 guest 字节的差分在 `tests/native_override_assets.cpp`
    //   （assets 档，从 AGE.EXE 取 sub_429020 的真字节，解释器 vs override）。
    // =====================================================================================
    {
        // guest 表夹具：28 B/槽，SSO 与堆串两种都覆盖。
        struct MapFixture {
            lift::Machine m;
            std::string err;
            std::uint32_t self = 0x00420000u;
            std::uint32_t table = 0x00450000u;
            std::uint32_t heap_str = 0x00460000u;
            std::vector<std::string> slots;
            std::uint32_t capacity = 0;

            bool build(const std::vector<std::string>& want) {
                if (!m.initBare(&err) || !m.prepareStack(&err)) return false;
                slots = want;
                capacity = static_cast<std::uint32_t>(want.size());
                const std::size_t table_bytes = static_cast<std::size_t>(capacity) * 28u;
                std::vector<std::uint8_t> zeros(table_bytes + 32u, 0);
                if (!m.placeData(self, zeros.data(), 32u, &err)) return false;
                if (!m.placeData(table, zeros.data(), table_bytes, &err)) return false;
                if (!m.placeData(heap_str, zeros.data(), zeros.size(), &err)) return false;
                m.mem().wr32(self + 0x18u, table);
                m.mem().wr32(self + 8u, capacity);
                std::uint32_t heap_cursor = heap_str;
                for (std::uint32_t i = 0; i < capacity; ++i) {
                    const std::uint32_t slot = table + i * 28u;
                    const std::string& s = want[i];
                    m.mem().wr32(slot + 0x10u, static_cast<std::uint32_t>(s.size()));
                    if (s.size() < 16u) {
                        for (std::size_t k = 0; k < s.size(); ++k)
                            m.mem().wr8(slot + static_cast<std::uint32_t>(k), static_cast<std::uint8_t>(s[k]));
                        m.mem().wr32(slot + 0x14u, 15u);  // MSVC SSO 的 cap
                    } else {
                        for (std::size_t k = 0; k < s.size(); ++k)
                            m.mem().wr8(heap_cursor + static_cast<std::uint32_t>(k), static_cast<std::uint8_t>(s[k]));
                        m.mem().wr32(slot + 0x00u, heap_cursor);
                        m.mem().wr32(slot + 0x14u, static_cast<std::uint32_t>(s.size()));
                        heap_cursor += static_cast<std::uint32_t>(s.size()) + 1u;
                    }
                }
                return true;
            }
            // 调用 native 实现（直接铺栈帧：返回地址 + 2 个参数；thiscall 的 ECX = this）
            bool call(std::uint32_t index, std::uint32_t key_ea, std::uint32_t* out) {
                auto& cpu = m.cpu();
                auto& mem = m.mem();
                const std::uint32_t esp = m.layout().stack_top - 0x40u;
                cpu.esp() = esp;
                cpu.ecx() = self;
                mem.wr32(esp, lift_fixture::kSentinel);
                mem.wr32(esp + 4u, index);
                mem.wr32(esp + 8u, key_ea);
                return lift::nativeFindIndex429020(cpu, mem, out);
            }
            std::uint32_t placeKey(const std::string& s) {
                static std::uint32_t cursor = 0x00470000u;
                const std::uint32_t ea = cursor;
                cursor += 0x40u;
                std::vector<std::uint8_t> b(s.begin(), s.end());
                b.push_back(0);
                m.placeData(ea, b.data(), b.size(), &err);
                return ea;
            }
            std::vector<std::uint8_t> snapshot(std::size_t n) const {
                const std::uint8_t* p = m.mem().host(table);
                return std::vector<std::uint8_t>(p, p + n);
            }
        };
        auto refFind = [](const std::vector<std::string>& slots, std::uint32_t index0,
                          const std::string& key) -> int {
            const std::uint32_t cap = static_cast<std::uint32_t>(slots.size());
            if (index0 >= cap) return -2;
            if (slots[index0].empty()) return -1;  // 0x429039：**先**判空（不读 key）
            std::uint32_t idx = index0;
            for (std::uint32_t probe = 0; probe < cap; ++probe) {
                const std::string& s = slots[idx];
                if (s.empty()) return -1;
                const std::size_t n = s.size() < key.size() ? s.size() : key.size();
                const bool eq = n == 0 || std::memcmp(s.data(), key.data(), n) == 0;
                if (eq && s.size() == key.size()) return static_cast<int>(idx);
                idx = (idx + 1u) % cap;
            }
            return -2;  // 整表没有空槽 ⇒ guest 死循环（期望 native 回退）
        };

        struct Case {
            std::vector<std::string> slots;
            std::vector<std::string> keys;
        };
        const std::vector<Case> cases = {
            // 无冲突：index 0 直接命中 / 空槽 miss
            {{"alpha", "", "", ""}, {"alpha", "beta", ""}},
            // 冲突链：index0 冲突后线性探测到 2
            {{"zero", "one", "two", ""}, {"two", "zero", "one"}},
            // 长度不同但前缀相同（memcmp(min) 相等也不算命中）
            {{"abcd", "", "", ""}, {"ab", "abcd", "abcde"}},
            // 20 字节堆串（cap>=16 ⇒ 数据在 [slot] 指针处）
            {{"0123456789abcdefghij", "", "", ""}, {"0123456789abcdefghij", "0123456789abcdefghi"}},
            // 空 key：任何非空槽都不命中 ⇒ 探测到空槽 -1
            {{"x", "y", ""}, {""}},
            // 整表满（4/4 非空且 key 不存在）⇒ guest 死循环 ⇒ 期望回退
            {{"a", "b", "c", "d"}, {"zzz"}},
        };
        for (std::size_t ci = 0; ci < cases.size(); ++ci) {
            for (const std::string& key : cases[ci].keys) {
                for (std::uint32_t index0 = 0; index0 < cases[ci].slots.size(); ++index0) {
                    MapFixture fx;
                    LIFT_CHECK(fx.build(cases[ci].slots));
                    const std::uint32_t key_ea = fx.placeKey(key);
                    const std::size_t table_bytes = cases[ci].slots.size() * 28u;
                    const std::vector<std::uint8_t> before = fx.snapshot(table_bytes);
                    std::uint32_t r = 0xDEADBEEFu;
                    const bool ok = fx.call(index0, key_ea, &r);
                    const int expect = refFind(cases[ci].slots, index0, key);
                    if (expect == -2) {
                        LIFT_CHECK(!ok);  // 回退（⛔ 不编造 -1）
                    } else {
                        LIFT_CHECK(ok);
                        LIFT_CHECK_EQ_U32(r, static_cast<std::uint32_t>(expect));
                    }
                    // native 只读：表字节一字未改
                    LIFT_CHECK(fx.snapshot(table_bytes) == before);
                }
            }
        }
        // 第一个槽空 ⇒ 返回 -1 且**根本不读 key**（guest 0x429039 在 strlen 之前就返回）
        {
            MapFixture fx;
            LIFT_CHECK(fx.build({"", "a", "", ""}));
            std::uint32_t r = 0;
            LIFT_CHECK(fx.call(0, 0xFFFFFFF0u, &r));  // key 指针在 4 GiB 附近 / 未提交
            LIFT_CHECK_EQ_U32(r, 0xFFFFFFFFu);
        }
        // 回退闸：this / 表 / 容量 / index / 提交状态
        {
            MapFixture fx;
            LIFT_CHECK(fx.build({"a", "b", "", ""}));
            std::uint32_t r = 0;
            const std::uint32_t key_ea = fx.placeKey("a");
            LIFT_CHECK(!fx.call(0, 0u, &r));  // key=0
            fx.m.mem().wr32(fx.self + 8u, 0u);
            LIFT_CHECK(!fx.call(0, key_ea, &r));  // capacity=0
            fx.m.mem().wr32(fx.self + 8u, fx.capacity);
            LIFT_CHECK(!fx.call(fx.capacity, key_ea, &r));  // index >= capacity（调用方本应先 %）
            fx.m.mem().wr32(fx.self + 0x18u, 0u);
            LIFT_CHECK(!fx.call(0, key_ea, &r));  // table=0
            // ★ "未提交的表"只在 safe mode 下判（非 safe 模式与 guest 一样走平台 fault 路径，
            //   不会"静默读到 0"——native 实现不替它兜底，见 nativeFindIndex429020 的注释）。
            // safe mode：未提交的表 ⇒ 明确拒绝（而不是宿主越界读）
            lift::Machine sm;
            std::string serr;
            LIFT_CHECK(sm.initBare(&serr));
            LIFT_CHECK(sm.prepareStack(&serr));
            sm.mem().setSafeMode(true);
            std::vector<std::uint8_t> blk(0x40, 0);
            LIFT_CHECK(sm.placeData(0x00420000u, blk.data(), blk.size(), &serr));
            sm.mem().wr32(0x00420000u + 0x18u, 0x00001000u);  // 表指针未提交
            sm.mem().wr32(0x00420000u + 8u, 4u);
            auto& scpu = sm.cpu();
            const std::uint32_t sesp = sm.layout().stack_top - 0x40u;
            scpu.esp() = sesp;
            scpu.ecx() = 0x00420000u;
            sm.mem().wr32(sesp, lift_fixture::kSentinel);
            sm.mem().wr32(sesp + 4u, 0u);
            sm.mem().wr32(sesp + 8u, 0x00420010u);
            std::uint32_t sr = 0;
            LIFT_CHECK(!lift::nativeFindIndex429020(scpu, sm.mem(), &sr));
        }
    }

    // =====================================================================================
    // ⑫ sub_48AE90 的像素核：四个分支的**逐字节**期望值（手算）
    //   ★ 16bpp 的 `alpha<0` 分支行步进是 `2*((pitch*row)/4)`（= pitch/2，不是 pitch）——
    //     这是 guest 的真实写法（`.lst` 0x48AF16），这条用例专门把它钉住。
    // =====================================================================================
    {
        // 16bpp · alpha>=0：rgb(0x123456) ⇒ R4=1/G4=3/B4=5 ⇒ 0x135；alpha=0x80 ⇒ 0x8000
        std::uint8_t a16[32];
        std::memset(a16, 0xCC, sizeof(a16));
        lift::fillRect16(a16, /*pitch=*/8, /*w=*/2, /*h=*/2, 0x00123456u, 0x80);
        const std::uint16_t w16 = 0x8135u;
        for (int row = 0; row < 2; ++row) {
            for (int col = 0; col < 2; ++col) {
                std::uint16_t v = 0;
                std::memcpy(&v, a16 + row * 8 + col * 2, 2);
                LIFT_CHECK_EQ_U32(v, w16);
            }
        }
        LIFT_CHECK_EQ_U32(a16[4], 0xCCu);  // row0 只写 2 像素（4 B），pitch 8 ⇒ 中间不碰
        // 16bpp · alpha<0：保留目标 A4（`& 0xF000`）；行步进 = 2*((8*row)/4) = 4*row（**不是** 8）
        std::uint8_t b16[32];
        std::memset(b16, 0x00, sizeof(b16));
        for (int i = 0; i < 32; i += 2) std::memcpy(b16 + i, "\x23\xF1", 2);  // A4=0xF, 其余 0x123
        lift::fillRect16(b16, /*pitch=*/8, /*w=*/2, /*h=*/2, 0x00123456u, -1);
        for (int row = 0; row < 2; ++row) {
            for (int col = 0; col < 2; ++col) {
                std::uint16_t v = 0;
                std::memcpy(&v, b16 + row * 4 + col * 2, 2);  // ★ 4*row
                LIFT_CHECK_EQ_U32(v, 0xF135u);
            }
        }
        LIFT_CHECK_EQ_U32(b16[8], 0x23u);  // 8.. 没被写（行步进是 4 而不是 8）

        // 32bpp · alpha>=0：0x40<<24 | 0x112233；行步进 4*((12*row)/4)=12*row
        std::uint8_t a32[48];
        std::memset(a32, 0xCC, sizeof(a32));
        lift::fillRect32(a32, /*pitch=*/12, 2, 2, 0x00112233u, 0x40);
        for (int row = 0; row < 2; ++row) {
            for (int col = 0; col < 2; ++col) {
                std::uint32_t v = 0;
                std::memcpy(&v, a32 + row * 12 + col * 4, 4);
                LIFT_CHECK_EQ_U32(v, 0x40112233u);
            }
        }
        LIFT_CHECK_EQ_U32(a32[8], 0xCCu);
        // 32bpp · alpha<0：保留旧 A 字节
        std::uint8_t b32[48];
        std::memset(b32, 0x00, sizeof(b32));
        for (int i = 0; i < 48; i += 4) std::memcpy(b32 + i, "\x11\x22\x33\xAB", 4);
        lift::fillRect32(b32, 12, 2, 2, 0x00112233u, -1);
        for (int row = 0; row < 2; ++row) {
            for (int col = 0; col < 2; ++col) {
                std::uint32_t v = 0;
                std::memcpy(&v, b32 + row * 12 + col * 4, 4);
                LIFT_CHECK_EQ_U32(v, 0xAB112233u);
            }
        }
        // 越界/空矩形：核不写（调用方已拒绝，这里钉住"核自己不乱写"）
        std::uint8_t guard[16];
        std::memset(guard, 0x77, sizeof(guard));
        lift::fillRect16(guard, 4, 0, 2, 0, 0);
        lift::fillRect32(guard, 4, 2, 0, 0, 0);
        for (int i = 0; i < 16; ++i) LIFT_CHECK_EQ_U32(guard[i], 0x77u);
    }

    // =====================================================================================
    // ⑬ sub_48AE90 的**整函数 override**（真纹理夹具）：整张纹理逐字节 + 锁记账 + 回退无半状态
    //   ★ 判据不是"返回 0"，而是 arena **逐字节**等于手算期望（抓"整张纹理错一位/错一行"）。
    // =====================================================================================
    {
        // 铺一个 this 块（self+0x40C = 纹理对象）；函数体不需要存在：call 目标被 override 拦下。
        // ★ `ds:dword_5518C0`（`display:TextureBit` 的值；`.lst` 0x406B14 是唯一写点）必须在
        //   guest 内存里且是 16 / 32 —— 本线**不**拿 ddraw 的显示 bpp 冒充它。
        auto armTextureBit = [](TextureRig& rig, std::uint32_t bits) {
            std::vector<std::uint8_t> b(4, 0);
            LIFT_CHECK(rig.m.placeData(0x005518C0u, b.data(), b.size(), &rig.err));
            rig.m.mem().wr32(0x005518C0u, bits);
        };
        auto runFill = [](TextureRig& rig, std::uint32_t rect_ea, std::uint32_t color, std::int32_t alpha,
                          std::vector<std::uint8_t>* before) {
            lift::X86& cpu = rig.m.cpu();
            std::string err;
            LIFT_CHECK(rig.m.overrides().add(0x0048AE90u, "ref_fill", lift::GuestConv::Thiscall, 12,
                                             lift::nativeFill48AE90, &err));
            const std::uint32_t self = 0x00420000u;
            std::vector<std::uint8_t> self_blk(0x410, 0);
            LIFT_CHECK(rig.m.placeData(self, self_blk.data(), self_blk.size(), &err));
            rig.m.mem().wr32(self + 0x40Cu, rig.texture);
            if (before != nullptr) *before = std::vector<std::uint8_t>(0, 0);
            const std::uint32_t code_ea = 0x00401000u;
            lift_fixture::Asm a(code_ea);
            a.u8(0xB9);  // mov ecx, self
            a.u32(self);
            a.pushImm(static_cast<std::uint32_t>(alpha));
            a.pushImm(color);
            a.pushImm(rect_ea);
            a.callRel(0x0048AE90u);
            a.ret();
            LIFT_CHECK(rig.m.placeCode(code_ea, a.b.data(), a.b.size(), &err));
            rig.m.cpu().esp() = rig.m.layout().stack_top - 4u;
            rig.m.mem().wr32(rig.m.cpu().esp(), lift_fixture::kSentinel);
            lift::RunOptions o;
            o.until_ea = lift_fixture::kSentinel;
            const lift::RunResult r = rig.m.run(o);
            LIFT_CHECK(r.reason == lift::StopReason::UntilEa);
        };
        auto fillArena = [](TextureRig& rig, std::uint8_t v) {
            lift::d3d9::TextureInfo ti;
            LIFT_CHECK(lift::d3d9::textureInfo(rig.m.cpu(), rig.texture, &ti, nullptr));
            const std::size_t n = static_cast<std::size_t>(ti.pitch) * ti.height;
            std::memset(rig.m.mem().host(ti.arena), v, n);
            return ti;
        };

        // ---- 32bpp × alpha>=0（A8R8G8B8，4×3；rect=(1,1,3,3)）----
        {
            TextureRig rig;
            LIFT_CHECK(rig.boot(4, 3, lift::d3d9::kFmtA8R8G8B8));
            if (rig.ok) {
                armTextureBit(rig, 32u);
                const lift::d3d9::TextureInfo ti = fillArena(rig, 0xEE);
                const std::uint32_t rect_ea = 0x00430000u;
                std::vector<std::uint8_t> rect(16, 0);
                LIFT_CHECK(rig.m.placeData(rect_ea, rect.data(), rect.size(), &rig.err));
                rig.m.mem().wr32(rect_ea + 0, 1u);
                rig.m.mem().wr32(rect_ea + 4, 1u);
                rig.m.mem().wr32(rect_ea + 8, 3u);
                rig.m.mem().wr32(rect_ea + 12, 3u);
                // 期望：先复制原 arena，再把 2×2 矩形逐字节改成 0x40112233
                const std::size_t n = static_cast<std::size_t>(ti.pitch) * ti.height;
                std::vector<std::uint8_t> want(rig.m.mem().host(ti.arena), rig.m.mem().host(ti.arena) + n);
                for (std::uint32_t y = 1; y < 3; ++y) {
                    for (std::uint32_t x = 1; x < 3; ++x) {
                        const std::uint32_t v = 0x40112233u;
                        std::memcpy(want.data() + y * ti.pitch + x * 4u, &v, 4);
                    }
                }
                runFill(rig, rect_ea, 0x00112233u, 0x40, nullptr);
                LIFT_CHECK(std::memcmp(rig.m.mem().host(ti.arena), want.data(), n) == 0);
                lift::d3d9::TextureInfo after;
                LIFT_CHECK(lift::d3d9::textureInfo(rig.m.cpu(), rig.texture, &after, nullptr));
                LIFT_CHECK_EQ_U32(after.locks, 1u);
                LIFT_CHECK_EQ_U32(after.version, 1u);
                LIFT_CHECK(!after.locked);
                LIFT_CHECK_EQ_U32(rig.m.overrides().at(0)->hits, 1u);
                LIFT_CHECK_EQ_U32(rig.m.overrides().at(0)->fallbacks, 0u);
            }
        }
        // ---- 16bpp × alpha>=0（A4R4G4B4，4×3；pitch=8；rect=(1,0,3,3)）----
        {
            TextureRig rig;
            LIFT_CHECK(rig.boot(4, 3, lift::d3d9::kFmtA4R4G4B4));
            if (rig.ok) {
                armTextureBit(rig, 16u);
                const lift::d3d9::TextureInfo ti = fillArena(rig, 0x5A);
                LIFT_CHECK_EQ_U32(ti.bpp, 16u);
                const std::uint32_t rect_ea = 0x00430000u;
                std::vector<std::uint8_t> rect(16, 0);
                LIFT_CHECK(rig.m.placeData(rect_ea, rect.data(), rect.size(), &rig.err));
                rig.m.mem().wr32(rect_ea + 0, 1u);
                rig.m.mem().wr32(rect_ea + 4, 0u);
                rig.m.mem().wr32(rect_ea + 8, 3u);
                rig.m.mem().wr32(rect_ea + 12, 3u);
                const std::size_t n = static_cast<std::size_t>(ti.pitch) * ti.height;
                std::vector<std::uint8_t> want(rig.m.mem().host(ti.arena), rig.m.mem().host(ti.arena) + n);
                // color=0x123456 ⇒ 0x135；alpha=0xF0 ⇒ A4=0xF ⇒ 0xF135
                const std::uint16_t word = 0xF135u;
                for (std::uint32_t y = 0; y < 3; ++y)
                    for (std::uint32_t x = 1; x < 3; ++x)
                        std::memcpy(want.data() + y * ti.pitch + x * 2u, &word, 2);
                runFill(rig, rect_ea, 0x00123456u, 0xF0, nullptr);
                LIFT_CHECK(std::memcmp(rig.m.mem().host(ti.arena), want.data(), n) == 0);
                lift::d3d9::TextureInfo after;
                LIFT_CHECK(lift::d3d9::textureInfo(rig.m.cpu(), rig.texture, &after, nullptr));
                LIFT_CHECK_EQ_U32(after.locks, 1u);
                LIFT_CHECK(!after.locked);
            }
        }
        // ---- 回退闸：不合法矩形 / 已锁 / 纹理对象为 0 ⇒ 明确回退，且**无半状态** ----
        {
            TextureRig rig;
            LIFT_CHECK(rig.boot(4, 3, lift::d3d9::kFmtA8R8G8B8));
            if (rig.ok) {
                armTextureBit(rig, 32u);
                const lift::d3d9::TextureInfo ti = fillArena(rig, 0xEE);
                const std::size_t n = static_cast<std::size_t>(ti.pitch) * ti.height;
                std::vector<std::uint8_t> frozen(rig.m.mem().host(ti.arena), rig.m.mem().host(ti.arena) + n);
                const std::uint32_t self = 0x00420000u;
                std::vector<std::uint8_t> self_blk(0x410, 0);
                LIFT_CHECK(rig.m.placeData(self, self_blk.data(), self_blk.size(), &rig.err));
                rig.m.mem().wr32(self + 0x40Cu, rig.texture);
                const std::uint32_t rect_ea = 0x00430000u;
                std::vector<std::uint8_t> rect(16, 0);
                LIFT_CHECK(rig.m.placeData(rect_ea, rect.data(), rect.size(), &rig.err));
                auto& cpu = rig.m.cpu();
                auto& mem = rig.m.mem();
                const std::uint32_t esp = rig.m.layout().stack_top - 0x40u;
                cpu.esp() = esp;
                cpu.ecx() = self;
                mem.wr32(esp, lift_fixture::kSentinel);
                auto call_native = [&](std::uint32_t r_ea, std::uint32_t a, std::int32_t al) {
                    mem.wr32(esp + 4u, r_ea);
                    mem.wr32(esp + 8u, a);
                    mem.wr32(esp + 12u, static_cast<std::uint32_t>(al));
                    std::uint32_t r = 0xDEADBEEFu;
                    return lift::nativeFill48AE90(cpu, mem, &r);
                };
                auto expect_fallback = [&](const char* what) {
                    const lift::d3d9::TextureInfo before = [&] {
                        lift::d3d9::TextureInfo t;
                        lift::d3d9::textureInfo(cpu, rig.texture, &t, nullptr);
                        return t;
                    }();
                    LIFT_CHECK(!call_native(rect_ea, 0x00112233u, 0x40));
                    const lift::d3d9::TextureInfo after = [&] {
                        lift::d3d9::TextureInfo t;
                        lift::d3d9::textureInfo(cpu, rig.texture, &t, nullptr);
                        return t;
                    }();
                    LIFT_CHECK_EQ_U32(after.locks, before.locks);
                    LIFT_CHECK_EQ_U32(after.version, before.version);
                    LIFT_CHECK(after.locked == before.locked);
                    LIFT_CHECK(std::memcmp(rig.m.mem().host(ti.arena), frozen.data(), n) == 0);
                    (void)what;
                };
                // ① 空矩形 / 越界矩形（LockRect 会拒绝；native 在加锁前回退）
                expect_fallback("empty-rect");
                mem.wr32(rect_ea + 0, 3u);
                mem.wr32(rect_ea + 4, 3u);
                mem.wr32(rect_ea + 8, 1u);
                mem.wr32(rect_ea + 12, 1u);
                expect_fallback("inverted-rect");
                mem.wr32(rect_ea + 0, 0u);
                mem.wr32(rect_ea + 4, 0u);
                mem.wr32(rect_ea + 8, 5u);  // > 纹理宽
                mem.wr32(rect_ea + 12, 1u);
                expect_fallback("oob-rect");
                // ② 纹理对象为 0
                mem.wr32(rect_ea + 0, 0u);
                mem.wr32(rect_ea + 4, 0u);
                mem.wr32(rect_ea + 8, 1u);
                mem.wr32(rect_ea + 12, 1u);
                mem.wr32(self + 0x40Cu, 0u);
                expect_fallback("texture=0");
                mem.wr32(self + 0x40Cu, rig.texture);
                // ③ 已锁：guest 先锁住 ⇒ native 必须拒绝（不抢锁、不写像素）
                const std::uint32_t lr = 0x00440000u;
                std::vector<std::uint8_t> lrb(8, 0);
                LIFT_CHECK(rig.m.placeData(lr, lrb.data(), lrb.size(), &rig.err));
                lift::d3d9::TextureInfo fresh;
                LIFT_CHECK(lift::d3d9::textureInfo(cpu, rig.texture, &fresh, nullptr));
                std::string why;
                LIFT_CHECK(lift::d3d9::lockRect(cpu, fresh, 0u, lr, 0u, 0u, &why));
                expect_fallback("already-locked");
                lift::d3d9::TextureInfo held;
                LIFT_CHECK(lift::d3d9::textureInfo(cpu, rig.texture, &held, nullptr));
                LIFT_CHECK(held.locked);  // 别人的锁还在
                LIFT_CHECK(lift::d3d9::unlockRect(cpu, held, 0u, &why));
            }
        }
    }

    // =====================================================================================
    // ⑭ sub_414AC0 的**填池循环段**（段级 override）：合成最小循环夹具，解释器 vs override
    //
    // 夹具 = `.lst` 0x41537E..0x41539F 的**同一段指令**（手写编码，逐条与反汇编对齐）：
    //   xor eax,eax / test edx,edx / jle 0x41539F
    //   loc_415384: mov ecx,[esi-10h] / mov edx,[ebx+5EC90h] / mov [ecx+eax*4],edx /
    //               mov ecx,[ebx+5D7E8h] / inc eax / lea edx,[ecx+edi+1] / cmp eax,edx /
    //               jl loc_415384
    //   loc_41539F: mov eax,[ebx+5D7ECh] / mov edx,[esi-8] / add eax,edi /
    //               mov [0x004F0000],eax / ret      ← 用来看循环后的寄存器/内存，并回到哨兵
    // ★ 判据：目标数组 / 结果 dword / EAX,EBX,ECX,EDX,ESI,EDI,EBP,ESP,EIP,EFLAGS / **insns**
    //   全部逐位相同（insns 相同 ⇒ `replaced_insns` 的补偿数对了）。
    // =====================================================================================
    {
        const std::uint8_t kPoolLoop[] = {
            0x31, 0xC0,                          // 0x41537E xor eax,eax
            0x85, 0xD2,                          // 0x415380 test edx,edx
            0x7E, 0x1B,                          // 0x415382 jle 0x41539F
            0x8B, 0x4E, 0xF0,                    // 0x415384 mov ecx,[esi-10h]
            0x8B, 0x93, 0x90, 0xEC, 0x05, 0x00,  // 0x415387 mov edx,[ebx+5EC90h]
            0x89, 0x14, 0x81,                    // 0x41538D mov [ecx+eax*4],edx
            0x8B, 0x8B, 0xE8, 0xD7, 0x05, 0x00,  // 0x415390 mov ecx,[ebx+5D7E8h]
            0x40,                                // 0x415396 inc eax
            0x8D, 0x54, 0x39, 0x01,              // 0x415397 lea edx,[ecx+edi+1]
            0x3B, 0xC2,                          // 0x41539B cmp eax,edx
            0x7C, 0xE5,                          // 0x41539D jl 0x415384
            0x8B, 0x83, 0xEC, 0xD7, 0x05, 0x00,  // 0x41539F mov eax,[ebx+5D7ECh]
            0x8B, 0x56, 0xF8,                    // 0x4153A5 mov edx,[esi-8]
            0x03, 0xC7,                          // 0x4153A8 add eax,edi
            0xA3, 0x00, 0x00, 0x4F, 0x00,        // 0x4153AA mov [0x004F0000],eax
            0xC3,                                // 0x4153AF ret
        };
        const std::uint32_t kContinuationResultEa = 0x004F0000u;
        struct PoolSnapshot {
            std::vector<std::uint8_t> target;
            std::uint32_t result = 0;
            std::uint32_t eax = 0, ebx = 0, ecx = 0, edx = 0, esi = 0, edi = 0, ebp = 0, esp = 0, eip = 0,
                          eflags = 0;
            std::uint64_t insns = 0;
            std::uint64_t hits = 0, declines = 0, fallbacks = 0;
        };
        // `start_ea` = 0x41537E（正常入口）或 0x415384（用于测"EAX!=0 ⇒ Declined"）
        // `stop_at_resume` = true ⇒ 在**段出口 0x41539F** 就停（不跑续延）⇒ 比较的 EFLAGS 就是
        //   循环尾 `cmp eax,edx` 留下的那组（否则续延里的 `add` 会把标志覆盖掉，等于没比）。
        auto runPool = [&](bool with_override, std::uint32_t count, std::uint32_t start_ea,
                           std::uint32_t eax0, bool stop_at_resume) {
            lift_fixture::Fixture fx;
            LIFT_CHECK(fx.ok);
            const std::uint32_t ebx = 0x00410000u;
            const std::uint32_t esi = 0x00470100u;
            const std::uint32_t target = 0x00480000u;
            std::vector<std::uint8_t> zeros(static_cast<std::size_t>(count) * 4u + 64u, 0);
            LIFT_CHECK(fx.m.placeData(ebx + 0x5D7E8u, zeros.data(), 16u, &fx.err));
            LIFT_CHECK(fx.m.placeData(ebx + 0x5EC90u, zeros.data(), 8u, &fx.err));
            LIFT_CHECK(fx.m.placeData(esi - 0x10u, zeros.data(), 16u, &fx.err));
            LIFT_CHECK(fx.m.placeData(target, zeros.data(), static_cast<std::size_t>(count) * 4u, &fx.err));
            LIFT_CHECK(fx.m.placeData(kContinuationResultEa, zeros.data(), 4u, &fx.err));
            fx.m.mem().wr32(ebx + 0x5D7E8u, count - 1u);  // count = [ebx+5D7E8] + edi + 1（edi=0）
            fx.m.mem().wr32(ebx + 0x5D7ECu, 0x1234u);
            fx.m.mem().wr32(ebx + 0x5EC90u, 0xDEADBEEFu);
            fx.m.mem().wr32(esi - 0x10u, target);
            fx.m.mem().wr32(esi - 8u, 0x99u);
            LIFT_CHECK(fx.m.placeCode(0x0041537Eu, kPoolLoop, sizeof(kPoolLoop), &fx.err));
            if (with_override) {
                std::string err;
                LIFT_CHECK(fx.m.blockOverrides().add(0x00415384u, 0x0041539Fu, "ref_pool",
                                                     lift::nativeFillPool414AC0, &err));
            }
            lift::X86& cpu = fx.m.cpu();
            fx.rearmStack();
            cpu.eax() = eax0;
            cpu.ebx() = ebx;
            cpu.esi() = esi;
            cpu.edi() = 0;
            cpu.edx() = count;
            cpu.eflags = 0x202u;
            if (stop_at_resume) {
                cpu.eip = start_ea;
                lift::RunOptions o;
                o.until_ea = 0x0041539Fu;
                const lift::RunResult r = fx.m.run(o);
                if (r.reason != lift::StopReason::UntilEa) {
                    fx.ok = false;
                    fx.err = std::string("意外停机：") + lift::stopReasonName(r.reason) + " — " + r.detail;
                }
            } else {
                fx.runCode(start_ea);
            }
            LIFT_CHECK(fx.ok);
            PoolSnapshot s;
            s.eax = cpu.eax();
            s.ebx = cpu.ebx();
            s.ecx = cpu.ecx();
            s.edx = cpu.edx();
            s.esi = cpu.esi();
            s.edi = cpu.edi();
            s.ebp = cpu.ebp();
            s.esp = cpu.esp();
            s.eip = cpu.eip;
            s.eflags = cpu.eflags;
            s.insns = cpu.insns;
            s.result = fx.m.mem().rd32(kContinuationResultEa);
            const std::uint8_t* p = fx.m.mem().host(target);
            s.target.assign(p, p + static_cast<std::size_t>(count) * 4u);
            if (with_override) {
                const lift::NativeBlockOverride* e = fx.m.blockOverrides().at(0);
                s.hits = e->hits;
                s.declines = e->declines;
                s.fallbacks = e->fallbacks;
            }
            return s;
        };
        auto comparePool = [&](const PoolSnapshot& base, const PoolSnapshot& ovr, const char* what) {
            LIFT_CHECK(base.eax == ovr.eax);
            LIFT_CHECK(base.ebx == ovr.ebx);
            LIFT_CHECK(base.ecx == ovr.ecx);
            LIFT_CHECK(base.edx == ovr.edx);
            LIFT_CHECK(base.esi == ovr.esi);
            LIFT_CHECK(base.edi == ovr.edi);
            LIFT_CHECK(base.ebp == ovr.ebp);
            LIFT_CHECK(base.esp == ovr.esp);
            LIFT_CHECK(base.eip == ovr.eip);
            LIFT_CHECK(base.eflags == ovr.eflags);
            LIFT_CHECK(base.insns == ovr.insns);
            LIFT_CHECK(base.result == ovr.result);
            LIFT_CHECK(base.target == ovr.target);
            if (base.eax != ovr.eax || base.eflags != ovr.eflags || base.insns != ovr.insns ||
                base.target != ovr.target) {
                std::fprintf(stderr, "  [pool compare failed: %s] eax=%08X/%08X eflags=%08X/%08X insns=%llu/%llu\n",
                             what, base.eax, ovr.eax, base.eflags, ovr.eflags,
                             (unsigned long long)base.insns, (unsigned long long)ovr.insns);
            }
        };
        // ★ 段出口的 EFLAGS 逐位比：在 0x41539F **就停**（不跑续延）⇒ 比的是 `cmp eax,edx` 留下的
        //   那一组。`cmp` 相等时**差 = 0** ⇒ PF 恒 1（parity of 0x00 = even），与 count 的奇偶无关；
        //   这里用奇/偶/跨字节的 count 把这条钉住。
        for (std::uint32_t count : {1u, 2u, 3u, 5u, 17u, 255u, 256u, 257u, 4097u}) {
            const PoolSnapshot base = runPool(false, count, 0x0041537Eu, 0u, true);
            const PoolSnapshot ovr = runPool(true, count, 0x0041537Eu, 0u, true);
            comparePool(base, ovr, "flags@resume");
            LIFT_CHECK_EQ_U32(ovr.hits, 1u);
            LIFT_CHECK_EQ_U32(ovr.eip, 0x0041539Fu);
            // 显式点名 ZF/PF（比较失败时 comparePool 只报整数，这里给出语义）
            LIFT_CHECK((ovr.eflags & 0x40u) != 0u);  // ZF
            LIFT_CHECK((ovr.eflags & 0x04u) != 0u);  // PF
            LIFT_CHECK((ovr.eflags & (0x01u | 0x10u | 0x80u | 0x800u)) == 0u);  // CF/AF/SF/OF
        }
        for (std::uint32_t count : {1u, 5u, 17u, 4097u}) {
            const PoolSnapshot base = runPool(false, count, 0x0041537Eu, 0u, false);
            const PoolSnapshot ovr = runPool(true, count, 0x0041537Eu, 0u, false);
            comparePool(base, ovr, "loop");
            LIFT_CHECK_EQ_U32(ovr.hits, 1u);
            LIFT_CHECK_EQ_U32(ovr.declines, 0u);
            LIFT_CHECK_EQ_U32(ovr.fallbacks, 0u);
            LIFT_CHECK_EQ_U32(ovr.result, 0x1234u);  // [ebx+5D7EC] + edi
            LIFT_CHECK_EQ_U32(ovr.ecx, count - 1u);  // 循环尾 ECX = [ebx+5D7E8]
        }
        // 回边进入（EAX!=0）⇒ **Declined**，回退解释器；结果仍与解释器逐位一致，且之后该项停用。
        {
            const PoolSnapshot base = runPool(false, 9u, 0x00415384u, 3u, false);
            const PoolSnapshot ovr = runPool(true, 9u, 0x00415384u, 3u, false);
            comparePool(base, ovr, "declined");
            LIFT_CHECK_EQ_U32(ovr.hits, 0u);
            LIFT_CHECK_EQ_U32(ovr.declines, 1u);
            LIFT_CHECK_EQ_U32(ovr.fallbacks, 0u);
        }
        // 直接调 fn：失败路径（目标未提交 ⇒ Failed；EAX!=0 / count<=0 ⇒ Declined）
        {
            lift::Machine m;
            std::string err;
            LIFT_CHECK(m.initBare(&err));
            LIFT_CHECK(m.prepareStack(&err));
            std::vector<std::uint8_t> blk(0x100, 0);
            LIFT_CHECK(m.placeData(0x00410000u, blk.data(), blk.size(), &err));
            LIFT_CHECK(m.placeData(0x00410000u + 0x5D7E8u, blk.data(), 16u, &err));
            LIFT_CHECK(m.placeData(0x00410000u + 0x5EC90u, blk.data(), 8u, &err));
            auto& cpu = m.cpu();
            auto& mem = m.mem();
            cpu.ebx() = 0x00410000u;
            cpu.esi() = 0x00410080u;
            cpu.edi() = 0u;
            mem.wr32(0x00410080u - 0x10u, 0x00001000u);  // 未提交的目标
            mem.wr32(0x00410000u + 0x5D7E8u, 3u);
            mem.wr32(0x00410000u + 0x5EC90u, 0xAAu);
            cpu.eax() = 0u;
            std::uint64_t replaced = 99;
            LIFT_CHECK(lift::nativeFillPool414AC0(cpu, mem, &replaced) ==
                       lift::BlockOverrideOutcome::Failed);
            LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(replaced), 0u);
            cpu.eax() = 7u;
            LIFT_CHECK(lift::nativeFillPool414AC0(cpu, mem, &replaced) ==
                       lift::BlockOverrideOutcome::Declined);
            cpu.eax() = 0u;
            mem.wr32(0x00410000u + 0x5D7E8u, 0xFFFFFFFFu);  // count = 0（回绕）
            LIFT_CHECK(lift::nativeFillPool414AC0(cpu, mem, &replaced) ==
                       lift::BlockOverrideOutcome::Declined);
        }
    }

    // ---- ⑮ known 表：新项的默认分层（全部 debug=false ⇒ 都在 all 里）+ kMax 容得下 ----
    {
        LIFT_CHECK(lift::findKnownNative("sub_429020") != nullptr);
        LIFT_CHECK(lift::findKnownNative("sub_48AE90") != nullptr);
        LIFT_CHECK(!lift::findKnownNative("sub_429020")->debug);
        LIFT_CHECK(!lift::findKnownNative("sub_48AE90")->debug);
        const lift::KnownBlockNative* pool = lift::findKnownBlockNative("sub_414AC0");
        LIFT_CHECK(pool != nullptr && !pool->debug);
        LIFT_CHECK_EQ_U32(pool->entry_ea, 0x00415384u);
        LIFT_CHECK_EQ_U32(pool->resume_ea, 0x0041539Fu);
        LIFT_CHECK(lift::NativeOverrideTable::kMax >= 10);
        LIFT_CHECK(lift::NativeBlockTable::kMax >= 1);
    }

    return lift_test::finish("native-override");
}
