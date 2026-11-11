// emulator-lift · guest 函数级 native override 的 pure 夹具
//
// 判据（红了意味着什么）：
//   ① **差分**：同一段 guest 参考实现（cdecl / `rep movsb` / `rep stosb`），解释器跑 vs override 跑，
//      目的内存 / EAX / ESP / 被调用方保存寄存器 / EIP 必须一致；不一致 ⇒ override 的 ABI 或语义错了。
//   ② **回退**：native 实现返回 false 时，dispatcher 必须继续解释原字节（结果仍与基线一致），
//      并把 fallback 记到账上 —— ⛔ 不许静默跳过、也不许直接 trap。
//   ③ **ABI**：cdecl 只弹返回地址；stdcall / thiscall 由被调用方再弹 arg_bytes；ECX = this。
//   ④ **纯核不变量**（`sub_4E0B40` `/` sub_4CCC60` 的纯核）：原地等价、不越界、输出不依赖旧内容。
// ★ 这里不测 profile / 首帧性能：那些用 `--native-overrides` 的真实 boot A/B（见 requirement 正文）。
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>

#include "check.h"
#include "fixture.h"
#include "runtime/com.h"
#include "runtime/d3d9.h"
#include "runtime/ddraw_surface.h"
#include "runtime/native_affine.h"
#include "runtime/native_mdct.h"
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

// ---------------------------------------------------------------------------------------
// ①（补）`_memcpy` 的**重叠**差分：解释器 vs native
//
// ★ 为什么参考实现要**两条**而不是一条：真 `_memcpy` 在重叠时**分流**
//   （EA 0x4E74B0，字节取自 PE —— `.lst` 里它是 COLLAPSED，没有指令文本）：
//   · `dst <= src` ⇒ 前向（`0x4E74C6 jbe 0x4E74F4` 那支：`rep movsd` + 字节尾表）⇒ 参考用 `kMemcpyRef`；
//   · `dst >  src` 且重叠 ⇒ 后向（`0x4E74CA jb 0x4E7670`，那里是 `0x4E768B FD std` /
//     `0x4E768C F3 A5 rep movsd` / `0x4E768E FC cld`）⇒ 参考用 `kMemcpyBackRef`。
//   ⇒ **oracle 是解释器**：每一类重叠都必须与"guest 在该类上真的会走的那条指令序列"逐字节相同。
//   ⛔ 若把三类都拿前向参考去比，`dst > src` 的重叠会被判成"前向才对"——那是把 C 标准当 oracle。
// ★ 真函数的**整体**（含它自己的分流）由 assets 档用真字节差分（`tests/native_override_assets.cpp`
//   的 `memcpy-overlap/*`）；这里用等价的最小参考把同一批形状压进 pure 档。
// ---------------------------------------------------------------------------------------

// dst > src 且**重叠**时 guest 走的那支：从高地址往低地址搬（`std; rep movsb; cld`）。
//   int __cdecl ref_memcpy_back(void *dst, const void *src, size_t n)
//     push ebp; mov ebp,esp; push esi; push edi; mov edi,[ebp+8]; mov esi,[ebp+0Ch]; mov ecx,[ebp+10h];
//     lea esi,[esi+ecx-1]; lea edi,[edi+ecx-1]; std; rep movsb; cld;
//     mov eax,[ebp+8]; pop edi; pop esi; pop ebp; ret
const std::uint8_t kMemcpyBackRef[] = {0x55, 0x8B, 0xEC, 0x56, 0x57, 0x8B, 0x7D, 0x08, 0x8B, 0x75,
                                       0x0C, 0x8B, 0x4D, 0x10, 0x8D, 0x74, 0x0E, 0xFF, 0x8D,
                                       0x7C, 0x0F, 0xFF, 0xFD, 0xF3, 0xA4, 0xFC, 0x8B, 0x45,
                                       0x08, 0x5F, 0x5E, 0x5D, 0xC3};

struct MemcpyOverlapCase {
    std::string name;       // ★ `std::string`（不是 `const char*`）：用例名要在循环外存活
    std::uint32_t n;
    std::uint32_t dst_off;  // 相对缓冲区基址
    std::uint32_t src_off;
    bool backward_ref;      // true ⇒ 该类重叠上 guest 走后向支（oracle = `kMemcpyBackRef`）
    bool straddle_page;     // true ⇒ 缓冲区基址让 [dst, dst+n) **跨 4 KiB 页**
};

struct MemcpyOverlapResult {
    bool valid = false;
    std::uint32_t eax = 0;
    std::uint32_t esp = 0;
    std::uint32_t esi = 0;
    std::uint32_t edi = 0;
    std::uint32_t ebp = 0;
    std::uint32_t eip = 0;
    std::vector<std::uint8_t> buf;
    std::uint64_t hits = 0;
    std::uint64_t fallbacks = 0;
};

constexpr std::uint32_t kOverlapBufBytes = 4096u + 64u;

// 缓冲区初始图样：**非周期**（否则"前向搬成重复图样"与"后向搬对"可能碰巧相等）。
std::vector<std::uint8_t> overlapPattern() {
    std::vector<std::uint8_t> v(kOverlapBufBytes);
    for (std::uint32_t i = 0; i < kOverlapBufBytes; ++i)
        v[i] = static_cast<std::uint8_t>((i * 37u + (i >> 3) * 11u + 101u) & 0xFFu);
    return v;
}

MemcpyOverlapResult runMemcpyOverlap(const MemcpyOverlapCase& c, bool with_override) {
    MemcpyOverlapResult out;
    lift_fixture::Fixture fx;
    if (!fx.ok) return out;
    const std::vector<std::uint8_t> init = overlapPattern();
    // ★ `0x00450000 - 32`：让 [dst, dst+64) 跨过页边界 0x00450000（含两端都在同一块连续已提交区）
    const std::uint32_t buf = c.straddle_page ? 0x0044FFE0u : 0x00450000u;
    if (!fx.m.placeData(buf, init.data(), init.size(), &fx.err)) return out;
    const std::uint8_t* ref = c.backward_ref ? kMemcpyBackRef : kMemcpyRef;
    const std::size_t ref_len = c.backward_ref ? sizeof(kMemcpyBackRef) : sizeof(kMemcpyRef);
    if (!fx.m.placeCode(kFnEa, ref, ref_len, &fx.err)) return out;
    if (with_override) {
        std::string err;
        if (!fx.m.overrides().add(kFnEa, "ref_memcpy", lift::GuestConv::Cdecl, 12, lift::nativeMemcpy, &err))
            return out;
    }
    const std::uint32_t dst = buf + c.dst_off;
    const std::uint32_t src = buf + c.src_off;
    const std::uint32_t ret = callCdecl(fx, kFnEa, {dst, src, c.n});
    if (!fx.ok) return out;
    out.valid = true;
    out.eax = ret;
    out.esp = fx.m.cpu().esp();
    out.esi = fx.m.cpu().esi();
    out.edi = fx.m.cpu().edi();
    out.ebp = fx.m.cpu().ebp();
    out.eip = fx.m.cpu().eip;
    const std::uint8_t* p = fx.m.mem().host(buf);
    out.buf.assign(p, p + kOverlapBufBytes);
    if (with_override) {
        const lift::NativeOverride* e = fx.m.overrides().at(0);
        out.hits = e->hits;
        out.fallbacks = e->fallbacks;
    }
    return out;
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

// ---------------------------------------------------------------------------------------
// `sub_4367B0`（退出写存档的 LZSS Encode）的**独立转写** —— 只给 pure 档当**回归网**。
//
// ★ 与生产核分开写：生产核是"逐条照 `.lst` 转写"（保留寄存器/变量的一一对应），这里是
//   "按经典 Okumura LZSS 的结构重写 + 本 guest 的六条怪癖"（`text_buf` 只清前 N 字节、
//   `s < F-1` 才写 `text_buf[s+N]`、**平局严格 `>`**、`size > 1` 才收尾 flush、放弃时从
//   输入起点整体重拷）。两份独立写出来的东西对同一输入给出同一串字节，才说明"照 asm 转写"
//   没抄错一行。
// ★ 诚实项：它**不是独立 oracle**（可能是同一份误读）；真正的 oracle 是 assets 档的**真字节两臂**
//   （解释器跑 `AGE.EXE` 的同一 EA）。这里只当"改一处转写就红"的回归网。
// ★ 不覆盖 `a2 + a3[0]` 的 32 位回绕（这里没有 guest 地址）；那条由生产核的 `in_ea` 用例覆盖。
struct NaiveEncoded {
    std::uint32_t consumed = 0;
    std::uint32_t comp = 0;
    bool gave_up = false;
    std::vector<std::uint8_t> out;
};

NaiveEncoded naiveLzssEncode(const std::uint8_t* input, std::uint32_t n) {
    constexpr int N = 4096;
    constexpr int F = 18;
    constexpr int NIL = N;
    NaiveEncoded res;
    std::vector<int> dad(N + 1, NIL);
    // ★ 尺寸 = N+1+256（不是 N+1）：根索引 p = 0x1001+byte ∈ [4097,4352] 落在 `rson[]` 的前 256 项上
    //   （guest 布局里 `lson[4097+i]` 与 `rson[i]` **同址**）。**根左支不可达**：首轮方向恒为 cmp=1
    //   （走 rson）、且挂在根上的节点必是根的右孩子 ⇒ 这段下标只可能是"越界写"而不是语义。
    //   补齐只是让这份转写在 ASan / 别的分配器下也**不会**靠运气不炸（现在它落在 vector 尾外 4 字节）。
    std::vector<int> lson(N + 1 + 256, NIL);
    std::vector<int> rson(N + 1 + 256, NIL);  // 根索引 p = 0x1001+byte ∈ [4097,4352]
    // ★ 预填必须与夹具/guest 对象同形（0xCC）：`text_buf` 只清前 0xFEE，`[4078..4112]` 里
    //   首灌写 [4078..4095] 的前 len 个，**其余保留对象原值**——而比较循环会读到它们
    //   （`text_buf[p+i]`，p ≤ 4095、i ≤ 17 ⇒ 最远 4112）。用 0 填会把一条真实依赖抹掉。
    std::vector<std::uint8_t> text(N + F - 1, 0xCC);
    std::memset(text.data(), 0, 0xFEEu);  // `0x43683D`：只清 0xFEE = 4078 字节
    int match_pos = 0;
    int match_len = 0;

    auto insert = [&](int r) {
        int p = N + 1 + static_cast<int>(text[r]);
        lson[r] = rson[r] = NIL;
        match_len = 0;
        int cmp = 1;
        for (;;) {
            if (cmp >= 0) {
                if (rson[p] != NIL) {
                    p = rson[p];
                } else {
                    rson[p] = r;
                    dad[r] = p;
                    return;
                }
            } else {
                if (lson[p] != NIL) {
                    p = lson[p];
                } else {
                    // 根左支不可达（见上面 lson 的尺寸注释）：这里 p 只该是普通节点
                    // ⇒ `lson[p]` 的 p 与 `rson[0..255]` 同址的那一段**不会**被这里写到。
                    lson[p] = r;
                    dad[r] = p;
                    return;
                }
            }
            int i = 1;
            for (; i < F; ++i) {
                cmp = static_cast<int>(text[r + i]) - static_cast<int>(text[p + i]);
                if (cmp != 0) break;
            }
            if (i > match_len) {  // ★ 平局严格大于：等长保留先入树者
                match_pos = p;
                match_len = i;
                if (i >= F) break;
            }
        }
        // i == F：把 p 换成 r
        dad[r] = dad[p];
        lson[r] = lson[p];
        rson[r] = rson[p];
        dad[lson[p]] = r;
        dad[rson[p]] = r;
        if (rson[dad[p]] == p) {
            rson[dad[p]] = r;
        } else {
            lson[dad[p]] = r;
        }
        dad[p] = NIL;
    };

    auto remove = [&](int p) {
        if (dad[p] == NIL) return;
        int q = 0;
        if (rson[p] == NIL) {
            q = lson[p];
        } else if (lson[p] == NIL) {
            q = rson[p];
        } else {
            q = lson[p];
            if (rson[q] != NIL) {
                do {
                    q = rson[q];
                } while (rson[q] != NIL);
                rson[dad[q]] = lson[q];
                dad[lson[q]] = dad[q];
                lson[q] = lson[p];
                dad[lson[p]] = q;
            }
            rson[q] = rson[p];
            dad[rson[p]] = q;
        }
        dad[q] = dad[p];
        if (rson[dad[p]] == p) {
            rson[dad[p]] = q;
        } else {
            lson[dad[p]] = q;
        }
        dad[p] = NIL;
    };

    std::uint8_t code[20] = {};
    int mask = 1;
    int size = 1;
    code[0] = 0;
    int r = N - F;
    int len = 0;
    while (len < F && static_cast<std::uint32_t>(len) < n) text[r + len++] = input[len];
    res.consumed = static_cast<std::uint32_t>(len);
    if (len == 0) return res;
    for (int j = r - 1; j >= r - F; --j) insert(j);
    insert(r);
    int s = 0;
    std::uint32_t in_pos = static_cast<std::uint32_t>(len);
    std::vector<std::uint8_t> outbuf(static_cast<std::size_t>(n) + 0x1000u, 0);
    std::uint32_t out_pos = 0;
    bool gave_up = false;
    for (;;) {
        if (match_len > len) match_len = len;
        if (match_len >= 3) {
            code[size] = static_cast<std::uint8_t>(match_pos & 0xFF);
            code[size + 1] = static_cast<std::uint8_t>(((match_pos >> 4) & 0xF0) | (match_len - 3));
            size += 2;
        } else {
            code[0] = static_cast<std::uint8_t>(code[0] | mask);
            match_len = 1;
            code[size] = text[r];
            ++size;
        }
        mask = (mask << 1) & 0xFF;
        if (mask == 0) {
            std::memcpy(outbuf.data() + out_pos, code, static_cast<std::size_t>(size));
            out_pos += static_cast<std::uint32_t>(size);
            res.comp += static_cast<std::uint32_t>(size);
            if (res.comp >= n) {
                gave_up = true;
                break;
            }
            code[0] = 0;
            mask = 1;
            size = 1;
        }
        const int last_match_length = match_len;
        int i = 0;
        while (i < last_match_length && in_pos < n) {
            const std::uint8_t ch = input[in_pos++];
            remove(s);
            text[s] = ch;
            if (s < F - 1) text[s + N] = ch;
            s = (s + 1) & (N - 1);
            r = (r + 1) & (N - 1);
            insert(r);
            ++i;
        }
        res.consumed += static_cast<std::uint32_t>(i);
        while (i++ < last_match_length) {
            remove(s);
            s = (s + 1) & (N - 1);
            r = (r + 1) & (N - 1);
            if (--len) insert(r);
        }
        if (len <= 0) break;
    }
    if (!gave_up && size > 1) {
        std::memcpy(outbuf.data() + out_pos, code, static_cast<std::size_t>(size));
        out_pos += static_cast<std::uint32_t>(size);
        res.comp += static_cast<std::uint32_t>(size);
        if (res.comp >= n) gave_up = true;
    }
    if (gave_up) {
        if (n != 0) std::memcpy(outbuf.data(), input, n);
        res.consumed = n;
        res.comp = n;
    }
    res.gave_up = gave_up;
    res.out.assign(outbuf.begin(), outbuf.begin() + res.comp);
    return res;
}

}  // namespace

int main() {
    // ---- ① memcpy：解释器（rep movsb） vs native ----
    {
        const Snapshot base = runMemcpy(false);
        const Snapshot ovr = runMemcpy(true);
        compare(base, ovr, "memcpy");
    }
    // ---- ① memcpy **重叠**：三类（dst<src / dst>src / dst==src）× n 表 ----
    // 判据：解释器臂与 native 臂的**整块内存**（含 dst/src 之外的两端哨兵）+ EAX/ESP/ESI/EDI/EBP/EIP
    // 逐字节/逐位相同，且 native 臂 **hits==1 / fallbacks==0**（= 这一类重叠**不再回退**）。
    // ★ oracle 的方向按 guest 自己的判据选（`mk()` 里那一行）；两类重叠用**不同**的参考实现 ——
    //   见 `kMemcpyRef` / `kMemcpyBackRef` 上方的注释。
    {
        auto mk = [](const std::string& name, std::uint32_t n, std::uint32_t d, std::uint32_t s,
                     bool straddle = false) {
            MemcpyOverlapCase c;
            c.name = name;
            c.n = n;
            c.dst_off = d;
            c.src_off = s;
            c.straddle_page = straddle;
            // ★ 与 guest 的 `0x4E74CA jb 0x4E7670` 同一条判据：`dst > src` **且** `dst < src+n`。
            const std::uint64_t dd = d;
            const std::uint64_t ss = s;
            c.backward_ref = dd > ss && dd < ss + static_cast<std::uint64_t>(n);
            return c;
        };
        std::vector<MemcpyOverlapCase> cases;
        const std::uint32_t sizes[] = {0u, 1u, 3u, 4u, 7u, 8u, 64u, 4096u};
        for (std::uint32_t n : sizes) {
            const std::string tag = std::to_string(n);
            cases.push_back(mk("memcpy-ovl/same-n" + tag, n, 0u, 0u));
            cases.push_back(mk("memcpy-ovl/dst<src-n" + tag, n, 0u, 1u));
            cases.push_back(mk("memcpy-ovl/dst>src-n" + tag, n, 1u, 0u));
        }
        // 距离 8（不是只测"错开 1 字节"）+ **跨 4 KiB 页**的一条
        cases.push_back(mk("memcpy-ovl/dst<src-d8-n64", 64u, 0u, 8u));
        cases.push_back(mk("memcpy-ovl/dst>src-d8-n64", 64u, 8u, 0u));
        cases.push_back(mk("memcpy-ovl/straddle-page-n64", 64u, 0u, 1u, true));
        for (const MemcpyOverlapCase& c : cases) {
            lift_test::g_current_case = c.name.c_str();
            const MemcpyOverlapResult base = runMemcpyOverlap(c, false);
            const MemcpyOverlapResult ovr = runMemcpyOverlap(c, true);
            LIFT_CHECK(base.valid && ovr.valid);
            if (!base.valid || !ovr.valid) continue;
            LIFT_CHECK_EQ_U32(ovr.eax, base.eax);
            LIFT_CHECK_EQ_U32(ovr.esp, base.esp);
            LIFT_CHECK_EQ_U32(ovr.esi, base.esi);
            LIFT_CHECK_EQ_U32(ovr.edi, base.edi);
            LIFT_CHECK_EQ_U32(ovr.ebp, base.ebp);
            LIFT_CHECK_EQ_U32(ovr.eip, base.eip);
            LIFT_CHECK(base.buf.size() == ovr.buf.size());
            bool same = base.buf.size() == ovr.buf.size() &&
                        std::memcmp(base.buf.data(), ovr.buf.data(), base.buf.size()) == 0;
            LIFT_CHECK(same);
            if (!same && base.buf.size() == ovr.buf.size()) {
                std::size_t first = 0;
                while (first < base.buf.size() && base.buf[first] == ovr.buf[first]) ++first;
                std::fprintf(stderr, "  [%s] 首个不同字节 off=%zu base=%02X ovr=%02X\n", c.name.c_str(), first,
                             base.buf[first], ovr.buf[first]);
            }
            // ★ 这一类重叠**不再回退**（本单的判据）
            LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(ovr.hits), 1u);
            LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(ovr.fallbacks), 0u);
        }
    }
    // ---- ① memcpy 仍然回退的两条（本期**不动**；只是把原因钉住，别让它们被"顺手放宽"）----
    {
        // (a) 跨 4 GiB：`dst + n > 4 GiB` ⇒ 拒绝（宿主平坦映射只有 4 GiB，搬它就是读出界）
        lift::Machine m;
        std::string err;
        LIFT_CHECK(m.initBare(&err));
        LIFT_CHECK(m.prepareStack(&err));
        lift::X86& cpu = m.cpu();
        const std::uint32_t esp = m.layout().stack_top - 32u;
        cpu.esp() = esp;
        m.mem().wr32(esp, 0xFFFFFFFFu);   // 返回地址
        m.mem().wr32(esp + 4, 0xFFFFF000u);  // dst
        m.mem().wr32(esp + 8, 0x00410000u);  // src
        m.mem().wr32(esp + 12, 0x00002000u); // n（dst+n 越过 4 GiB）
        lift::NativeOverrideTable table;
        LIFT_CHECK(table.add(0x00403000u, "ref_memcpy", lift::GuestConv::Cdecl, 12, lift::nativeMemcpy, &err));
        const char* why = nullptr;
        LIFT_CHECK(!table.invoke(*table.find(0x00403000u), cpu, m.mem(), &why));
        LIFT_CHECK_EQ_U32(table.find(0x00403000u)->hits, 0u);
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(table.find(0x00403000u)->fallbacks), 1u);
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(
                              table.find(0x00403000u)->declines[static_cast<std::size_t>(lift::MemcpyWhy::CrossBoundary)]),
                          1u);
    }
    {
        // (b) safe mode + 跨度不在已提交页 ⇒ 拒绝（guest 会 fault，替它搬就是掩盖 fault）
        lift::Machine m;
        std::string err;
        LIFT_CHECK(m.initBare(&err));
        LIFT_CHECK(m.prepareStack(&err));
        m.mem().setSafeMode(true);
        lift::X86& cpu = m.cpu();
        const std::uint32_t esp = m.layout().stack_top - 32u;
        cpu.esp() = esp;
        m.mem().wr32(esp, 0xFFFFFFFFu);
        m.mem().wr32(esp + 4, 0x00001000u);  // dst：未提交
        m.mem().wr32(esp + 8, 0x00410000u);
        m.mem().wr32(esp + 12, 16u);
        lift::NativeOverrideTable table;
        LIFT_CHECK(table.add(0x00403000u, "ref_memcpy", lift::GuestConv::Cdecl, 12, lift::nativeMemcpy, &err));
        const char* why = nullptr;
        LIFT_CHECK(!table.invoke(*table.find(0x00403000u), cpu, m.mem(), &why));
        LIFT_CHECK_EQ_U32(table.find(0x00403000u)->hits, 0u);
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(table.find(0x00403000u)->fallbacks), 1u);
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(
                              table.find(0x00403000u)->declines[static_cast<std::size_t>(lift::MemcpyWhy::Uncommitted)]),
                          1u);
        m.mem().setSafeMode(false);
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
                // ★ 内容版本语义（2026-11 改）：0 = 未知；建纹理 = 1（像素全 0，已知）；
                //   每次 UnlockRect 赋**下一个单调序列号**（序列从 2 起），不再等于 "UnlockRect 次数"。
                //   所以一次 Lock/Unlock 之后是 2，而不是旧的 1。
                LIFT_CHECK_EQ_U32(after.version, 2u);
                LIFT_CHECK(!after.locked);
                LIFT_CHECK_EQ_U32(rig.m.overrides().at(0)->hits, 1u);
                LIFT_CHECK_EQ_U32(rig.m.overrides().at(0)->fallbacks, 0u);
            }
        }
        // ---- 纹理内容版本语义（GPU 纹理上传缓存"跳过重传"的判据；纯 CPU 侧，不建窗口）----
        // ★ 判据：建纹理 = 1（像素块 zero=true ⇒ 全 0、内容已知）；Lock = 0（未知）；
        //   Unlock 发**单调序列号**（第一次 = 2，⛔ 不是 `version+1`）；RT 降级 = 0（未知）；
        //   再 Unlock = 3 ⇒ 序列**不复用**（回到 1/2 会让缓存命中旧 generation ⇒ 静默旧纹理）。
        {
            lift_test::g_current_case = "texture-content-version";
            TextureRig rig;
            LIFT_CHECK(rig.boot(8, 8, lift::d3d9::kFmtA8R8G8B8));
            if (rig.ok) {
                lift::d3d9::TextureInfo ti;
                LIFT_CHECK(lift::d3d9::textureInfo(rig.m.cpu(), rig.texture, &ti, nullptr));
                LIFT_CHECK_EQ_U32(ti.version, 1u);
                const std::uint32_t lr_ea = 0x00430000u;
                std::vector<std::uint8_t> lr(8, 0);
                LIFT_CHECK(rig.m.placeData(lr_ea, lr.data(), lr.size(), &rig.err));
                LIFT_CHECK(lift::d3d9::lockRect(rig.m.cpu(), ti, 0u, lr_ea, 0u, 0u, nullptr));
                LIFT_CHECK(lift::d3d9::textureInfo(rig.m.cpu(), rig.texture, &ti, nullptr));
                LIFT_CHECK_EQ_U32(ti.version, 0u);
                LIFT_CHECK(lift::d3d9::unlockRect(rig.m.cpu(), ti, 0u, nullptr));
                LIFT_CHECK(lift::d3d9::textureInfo(rig.m.cpu(), rig.texture, &ti, nullptr));
                LIFT_CHECK_EQ_U32(ti.version, 2u);
                // RT 降级（SetRenderTarget 绑到纹理表面时走同一个函数）⇒ 未知。
                lift::d3d9::markTextureContentUnknown(rig.m.cpu(), ti.state);
                LIFT_CHECK(lift::d3d9::textureInfo(rig.m.cpu(), rig.texture, &ti, nullptr));
                LIFT_CHECK_EQ_U32(ti.version, 0u);
                LIFT_CHECK(lift::d3d9::lockRect(rig.m.cpu(), ti, 0u, lr_ea, 0u, 0u, nullptr));
                // ★ 必须重新读状态块：`ti.locked` 是**调用前**的快照，unlockRect 会按它判 D3DERR_INVALIDCALL。
                LIFT_CHECK(lift::d3d9::textureInfo(rig.m.cpu(), rig.texture, &ti, nullptr));
                LIFT_CHECK(lift::d3d9::unlockRect(rig.m.cpu(), ti, 0u, nullptr));
                LIFT_CHECK(lift::d3d9::textureInfo(rig.m.cpu(), rig.texture, &ti, nullptr));
                LIFT_CHECK_EQ_U32(ti.version, 3u);
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

    // =====================================================================================
    // ⑯ `sub_46D9F0` 的字形混合核（**G1：纯算术**；不建机器、不碰游戏 / LFS）
    //
    // 判据（红了意味着什么）：
    //   ① 覆盖值 = 0 ⇒ 一个字节都不写；覆盖值 = 满值 ⇒ 整像素覆写；目标自身 A = 0 ⇒ 覆写；
    //      其余 ⇒ 逐通道 `(目标×(满-覆盖) + 源×覆盖) ÷ 满`，输出 A = `max(目标, 覆盖)`。
    //   ② 两条支路（16 位 满=15 / 32 位 满=255）与两种源格式（1bpp 掩码 / 8bpp 级数）都走同一套判据。
    //   ③ 覆盖值量表：65 级的满覆盖 = **251**、17 级 = **240**（⛔ 不是 255）—— 32 位支能直接观察 A。
    //   ④ 裁剪：left 含、right/bottom 不含；被裁掉的像素一个字节都不写。
    //   ⑤ EAX = `(a4[1] > 0) ? a5 : 0`。
    // ★ 变异自检（需求单 G1，另在构建上手工施加过一次）：`sar 4`→`sar 3` · ÷17→÷16 · `max`→`+`
    //   三条都必须让本节的断言当场红（见报告）。
    // ★ 这里**不测**准入层（那是宿主侧判据）；准入在下面 ⑳ 用合成 guest 内存逐条钉。
    // =====================================================================================
    {
        lift_test::g_current_case = "glyph-blend-46D9F0";
        struct BlitResult {
            std::vector<std::uint8_t> pixels;
            std::uint32_t eax = 0;
        };
        // 宿主缓冲上一次跑纯核：4 px/行 × 2 行（16 位 = 16 B、32 位 = 32 B），初值 0x5A。
        auto runBlit = [](const lift::GlyphBlit& b, std::size_t bytes) {
            std::vector<std::uint8_t> buf(bytes, 0x5A);
            lift::GlyphBlit args = b;
            args.dst = buf.data();
            BlitResult r;
            r.eax = lift::blendGlyph46D9F0(args);
            r.pixels = buf;
            return r;
        };
        auto px16 = [](const BlitResult& r, int x, int y) {
            std::uint16_t v = 0;
            std::memcpy(&v, r.pixels.data() + static_cast<std::size_t>(y) * 8u + static_cast<std::size_t>(x) * 2u, 2);
            return static_cast<std::uint32_t>(v);
        };
        auto px32 = [](const BlitResult& r, int x, int y) {
            std::uint32_t v = 0;
            std::memcpy(&v, r.pixels.data() + static_cast<std::size_t>(y) * 8u + static_cast<std::size_t>(x) * 4u, 4);
            return v;
        };
        // color = 0x00804020（COLORREF：R=0x20 / G=0x40 / B=0x80）
        //   32 位支：flat = (R<<16)|(G<<8)|B = 0x00204080
        //   16 位支：R4=2 / G4=4 / B4=8 ⇒ flat = 0x0248（A4R4G4B4 的 R/G/B）
        const std::uint32_t kColor = 0x00804020u;
        const std::uint32_t kFlat16 = 0x0248u;
        const std::uint32_t kFlat32 = 0x00204080u;
        const std::uint16_t kUnchanged16 = 0x5A5Au;
        const std::uint32_t kUnchanged32 = 0x5A5A5A5Au;
        lift::GlyphBlit base;
        base.dst_pitch = 8;  // 16 位：4 px/行；32 位：2 px/行
        base.width = 2;
        base.height = 2;
        base.origin_x = 0;
        base.origin_y = 0;
        base.src_stride = 4;
        base.dst_x = 1;
        base.dst_y = 0;
        base.color = kColor;
        base.clip_left = 0;
        base.clip_top = 0;
        base.clip_right = 4;
        base.clip_bottom = 2;
        base.sixteen_bpp = true;
        base.source = lift::GlyphMaskSource::BitMask1;
        base.levels = lift::GlyphLevels::Levels17;

        // ---- ① 16 位 × 1bpp 掩码：置位 ⇒ 满覆盖 15 ⇒ 整像素覆写；位 = 0 ⇒ 一个字节都不写 ----
        {
            const std::uint8_t src[8] = {0x80, 0, 0, 0, 0x80, 0, 0, 0};  // 每行只有 col0 置位
            lift::GlyphBlit b = base;
            b.src = src;
            const BlitResult r = runBlit(b, 16);
            LIFT_CHECK_EQ_U32(r.eax, 4u);  // EAX = a5（0x46DC55）
            LIFT_CHECK_EQ_U32(px16(r, 1, 0), (0xFu << 12) | kFlat16);
            LIFT_CHECK_EQ_U32(px16(r, 1, 1), (0xFu << 12) | kFlat16);
            LIFT_CHECK_EQ_U32(px16(r, 2, 0), kUnchanged16);  // 位 = 0 ⇒ 不写
            LIFT_CHECK_EQ_U32(px16(r, 2, 1), kUnchanged16);
            LIFT_CHECK_EQ_U32(px16(r, 0, 0), kUnchanged16);  // 裁剪外
            LIFT_CHECK_EQ_U32(px16(r, 3, 0), kUnchanged16);
        }
        // ---- ② 16 位 × 65 级：raw=64 ⇒ 覆盖值 251 ⇒ `sar 4` = 15 ⇒ 整像素覆写 ----
        //   ★ `sar 4` → `sar 3` 的变异在这里红（251>>3 = 31 ≠ 15 ⇒ 走加权而不是覆写）。
        {
            const std::uint8_t src[8] = {64, 64, 64, 64, 64, 64, 64, 64};
            lift::GlyphBlit b = base;
            b.source = lift::GlyphMaskSource::Level8;
            b.levels = lift::GlyphLevels::Levels65;
            b.src = src;
            const BlitResult r = runBlit(b, 16);
            LIFT_CHECK_EQ_U32(px16(r, 1, 0), (0xFu << 12) | kFlat16);
            LIFT_CHECK_EQ_U32(px16(r, 2, 0), (0xFu << 12) | kFlat16);
            LIFT_CHECK_EQ_U32(px16(r, 1, 1), (0xFu << 12) | kFlat16);
            LIFT_CHECK_EQ_U32(px16(r, 2, 1), (0xFu << 12) | kFlat16);
            LIFT_CHECK_EQ_U32(px16(r, 0, 0), kUnchanged16);
            LIFT_CHECK_EQ_U32(r.eax, 4u);
        }
        // ---- ③ 16 位 × 17 级：raw=16 ⇒ 覆盖值 240 ⇒ `sar 4` = 15 ⇒ 同样是覆写 ----
        {
            const std::uint8_t src[8] = {16, 16, 16, 16, 16, 16, 16, 16};
            lift::GlyphBlit b = base;
            b.source = lift::GlyphMaskSource::Level8;
            b.levels = lift::GlyphLevels::Levels17;
            b.src = src;
            const BlitResult r = runBlit(b, 16);
            LIFT_CHECK_EQ_U32(px16(r, 1, 0), (0xFu << 12) | kFlat16);
            LIFT_CHECK_EQ_U32(px16(r, 2, 1), (0xFu << 12) | kFlat16);
        }
        // ---- ④ 16 位 × 中覆盖（raw=32 ⇒ 覆盖值 125 ⇒ `sar 4` = 7）：逐通道加权 + A = max ----
        //   dst = 0xFA5C（A=0xF / ch8=0xA / ch4=0x5 / ch0=0xC），cov=7、k=8：
        //     ch8: (0xA*8 + 7*2)/15 = 94/15 = 6   ch4: (0x5*8 + 7*4)/15 = 68/15 = 4
        //     ch0: (0xC*8 + 7*8)/15 = 152/15 = 10  ⇒ 0xF64A
        //   ★ `sar 4` → `sar 3` 会变成 125>>3 = 15 ⇒ 覆写 0xF248（与 0xF64A 不同 ⇒ 红）。
        {
            const std::uint8_t src[8] = {32, 32, 32, 32, 32, 32, 32, 32};
            lift::GlyphBlit b = base;
            b.source = lift::GlyphMaskSource::Level8;
            b.levels = lift::GlyphLevels::Levels65;
            b.src = src;
            std::vector<std::uint8_t> buf(16, 0);
            for (int y = 0; y < 2; ++y) {
                for (int x = 0; x < 4; ++x) {
                    const std::uint16_t v = 0xFA5Cu;
                    std::memcpy(buf.data() + static_cast<std::size_t>(y) * 8u + static_cast<std::size_t>(x) * 2u, &v, 2);
                }
            }
            lift::GlyphBlit args = b;
            args.dst = buf.data();
            LIFT_CHECK_EQ_U32(lift::blendGlyph46D9F0(args), 4u);
            BlitResult r;
            r.pixels = buf;
            LIFT_CHECK_EQ_U32(px16(r, 1, 0), 0xF64Au);
            LIFT_CHECK_EQ_U32(px16(r, 2, 1), 0xF64Au);
            LIFT_CHECK_EQ_U32(px16(r, 0, 0), 0xFA5Cu);
        }
        // ---- ⑤ 16 位 × 目标 A = 0 ⇒ 整像素覆写（值 = 覆盖值<<12 | 源 RGB，**不读目标**）----
        {
            const std::uint8_t src[8] = {32, 32, 32, 32, 32, 32, 32, 32};
            lift::GlyphBlit b = base;
            b.source = lift::GlyphMaskSource::Level8;
            b.levels = lift::GlyphLevels::Levels65;
            b.src = src;
            std::vector<std::uint8_t> buf(16, 0);
            for (std::size_t i = 0; i < buf.size(); i += 2) {
                const std::uint16_t v = 0x0A5Cu;  // A = 0
                std::memcpy(buf.data() + i, &v, 2);
            }
            lift::GlyphBlit args = b;
            args.dst = buf.data();
            (void)lift::blendGlyph46D9F0(args);
            BlitResult r;
            r.pixels = buf;
            LIFT_CHECK_EQ_U32(px16(r, 1, 0), (7u << 12) | kFlat16);  // 0x7248
        }
        // ---- ⑥ 16 位 × 目标 A > 覆盖值：输出 A = max（`max` → `+` 的变异在这里红）----
        //   dst = 0xDA5C（A=0xD）、cov=7 ⇒ A 应为 0xD；`+` 会给出 (0xD+7)&0xF = 4。
        {
            const std::uint8_t src[8] = {32, 32, 32, 32, 32, 32, 32, 32};
            lift::GlyphBlit b = base;
            b.source = lift::GlyphMaskSource::Level8;
            b.levels = lift::GlyphLevels::Levels65;
            b.src = src;
            std::vector<std::uint8_t> buf(16, 0);
            for (std::size_t i = 0; i < buf.size(); i += 2) {
                const std::uint16_t v = 0xDA5Cu;
                std::memcpy(buf.data() + i, &v, 2);
            }
            lift::GlyphBlit args = b;
            args.dst = buf.data();
            (void)lift::blendGlyph46D9F0(args);
            BlitResult r;
            r.pixels = buf;
            LIFT_CHECK_EQ_U32(px16(r, 1, 0), 0xD64Au);
        }
        // ---- ⑦ 覆盖值 = 0（raw = 0）⇒ 一个字节都不写 ----
        {
            const std::uint8_t src[8] = {0, 0, 0, 0, 0, 0, 0, 0};
            lift::GlyphBlit b = base;
            b.source = lift::GlyphMaskSource::Level8;
            b.levels = lift::GlyphLevels::Levels65;
            b.src = src;
            const BlitResult r = runBlit(b, 16);
            for (int y = 0; y < 2; ++y)
                for (int x = 0; x < 4; ++x) LIFT_CHECK_EQ_U32(px16(r, x, y), kUnchanged16);
            LIFT_CHECK_EQ_U32(r.eax, 4u);
        }
        // ---- ⑧ 裁剪：左越界 / 右越界 / 下越界 ⇒ 一个字节都不写；上越界只写回来那行 ----
        {
            const std::uint8_t src[8] = {0x80, 0, 0, 0, 0x80, 0, 0, 0};
            lift::GlyphBlit b = base;
            b.src = src;
            b.dst_x = -3;  // x = -3, -2 ⇒ 全在 left=0 之外
            BlitResult r = runBlit(b, 16);
            for (int y = 0; y < 2; ++y)
                for (int x = 0; x < 4; ++x) LIFT_CHECK_EQ_U32(px16(r, x, y), kUnchanged16);
            b = base;
            b.src = src;
            b.dst_x = 4;  // x = 4, 5 ⇒ 全在 right=4 之外（right 不含）
            r = runBlit(b, 16);
            for (int y = 0; y < 2; ++y)
                for (int x = 0; x < 4; ++x) LIFT_CHECK_EQ_U32(px16(r, x, y), kUnchanged16);
            b = base;
            b.src = src;
            b.dst_y = 2;  // y = 2, 3 ⇒ 全在 bottom=2 之外
            r = runBlit(b, 16);
            for (int y = 0; y < 2; ++y)
                for (int x = 0; x < 4; ++x) LIFT_CHECK_EQ_U32(px16(r, x, y), kUnchanged16);
            b = base;
            b.src = src;
            b.dst_y = -1;  // y = -1（裁掉）, 0（写）
            r = runBlit(b, 16);
            LIFT_CHECK_EQ_U32(px16(r, 1, 0), (0xFu << 12) | kFlat16);
            LIFT_CHECK_EQ_U32(px16(r, 1, 1), kUnchanged16);
        }
        // ---- ⑨ 32 位 × 1bpp 掩码：置位 ⇒ 覆盖值 255 ⇒ 整像素覆写 ----
        {
            const std::uint8_t src[8] = {0xC0, 0, 0, 0, 0xC0, 0, 0, 0};
            lift::GlyphBlit b = base;
            b.dst_x = 0;  // 32 位支只有 2 px/行
            b.sixteen_bpp = false;
            b.src = src;
            const BlitResult r = runBlit(b, 32);
            LIFT_CHECK_EQ_U32(px32(r, 0, 0), 0xFF000000u | kFlat32);
            LIFT_CHECK_EQ_U32(px32(r, 1, 0), 0xFF000000u | kFlat32);
            LIFT_CHECK_EQ_U32(px32(r, 0, 1), 0xFF000000u | kFlat32);
            LIFT_CHECK_EQ_U32(r.eax, 4u);
            LIFT_CHECK_EQ_U32(px32(r, 0, 0), 0xFF204080u);  // 字节反序口径落位
        }
        // ---- ⑩ 32 位 × 65 级满（raw=64 ⇒ 251）：**不**等于 255 ⇒ 走加权（目标 A=0 ⇒ 覆写，A=251）----
        //   ★ 这条同时钉住"满覆盖 251 不是 255"（spec 侧的口径）；÷65 → ÷17 的变异不影响它。
        {
            const std::uint8_t src[8] = {64, 0, 0, 0, 0, 0, 0, 0};
            lift::GlyphBlit b = base;
            b.dst_x = 0;
            b.sixteen_bpp = false;
            b.source = lift::GlyphMaskSource::Level8;
            b.levels = lift::GlyphLevels::Levels65;
            b.src = src;
            std::vector<std::uint8_t> buf(32, 0);  // 目标 A = 0 ⇒ 覆写
            lift::GlyphBlit args = b;
            args.dst = buf.data();
            (void)lift::blendGlyph46D9F0(args);
            BlitResult r;
            r.pixels = buf;
            LIFT_CHECK_EQ_U32(px32(r, 0, 0), 0xFB204080u);  // A = 251
        }
        // ---- ⑪ 32 位 × 17 级满（raw=16 ⇒ 240）⇒ A = 240；÷17 → ÷16 的变异在这里红 ----
        //   目标 A = 0x40 ⇒ 走加权：outA = max(0x40,240) = 240
        //     R: (0x10*15 + 240*0x20)/255 = 7920/255  = 31  (0x1F)
        //     G: (0x20*15 + 240*0x40)/255 = 15840/255 = 62  (0x3E)
        //     B: (0x30*15 + 240*0x80)/255 = 31440/255 = 123 (0x7B)
        //   ★ ÷17 → ÷16 会让覆盖值变 255 ⇒ 整像素覆写 0xFF204080（不同 ⇒ 红）。
        {
            const std::uint8_t src[8] = {16, 0, 0, 0, 0, 0, 0, 0};
            lift::GlyphBlit b = base;
            b.dst_x = 0;
            b.sixteen_bpp = false;
            b.source = lift::GlyphMaskSource::Level8;
            b.levels = lift::GlyphLevels::Levels17;
            b.src = src;
            std::vector<std::uint8_t> buf(32, 0);
            for (std::size_t i = 0; i < buf.size(); i += 4) {
                const std::uint32_t v = 0x40102030u;  // A=0x40 R=0x10 G=0x20 B=0x30
                std::memcpy(buf.data() + i, &v, 4);
            }
            lift::GlyphBlit args = b;
            args.dst = buf.data();
            (void)lift::blendGlyph46D9F0(args);
            BlitResult r;
            r.pixels = buf;
            LIFT_CHECK_EQ_U32(px32(r, 0, 0), 0xF01F3E7Bu);
            LIFT_CHECK_EQ_U32(px32(r, 1, 0), 0x40102030u);  // 覆盖值 0 的列不写
        }
        // ---- ⑫ 32 位 × 中覆盖（raw=32 ⇒ 125）：加权 + A = max（`max` → `+` 的变异在这里红）----
        //   dst A=0xC0 > 125 ⇒ A 应为 0xC0；`+` 会给出 (0xC0+125) = 0xFD。
        //     R: (0x10*130 + 125*0x20)/255 = 6080/255  = 23  (0x17)
        //     G: (0x20*130 + 125*0x40)/255 = 12160/255 = 47  (0x2F)
        //     B: (0x30*130 + 125*0x80)/255 = 22240/255 = 87  (0x57)
        {
            const std::uint8_t src[8] = {32, 0, 0, 0, 0, 0, 0, 0};
            lift::GlyphBlit b = base;
            b.dst_x = 0;
            b.sixteen_bpp = false;
            b.source = lift::GlyphMaskSource::Level8;
            b.levels = lift::GlyphLevels::Levels65;
            b.src = src;
            std::vector<std::uint8_t> buf(32, 0);
            for (std::size_t i = 0; i < buf.size(); i += 4) {
                const std::uint32_t v = 0xC0102030u;
                std::memcpy(buf.data() + i, &v, 4);
            }
            lift::GlyphBlit args = b;
            args.dst = buf.data();
            (void)lift::blendGlyph46D9F0(args);
            BlitResult r;
            r.pixels = buf;
            LIFT_CHECK_EQ_U32(px32(r, 0, 0), 0xC0172F57u);
        }
        // ---- ⑬ 32 位 × 目标 A = 0 ⇒ 覆写（A = 覆盖值）----
        {
            const std::uint8_t src[8] = {32, 0, 0, 0, 0, 0, 0, 0};
            lift::GlyphBlit b = base;
            b.dst_x = 0;
            b.sixteen_bpp = false;
            b.source = lift::GlyphMaskSource::Level8;
            b.levels = lift::GlyphLevels::Levels65;
            b.src = src;
            std::vector<std::uint8_t> buf(32, 0);
            for (std::size_t i = 0; i < buf.size(); i += 4) {
                const std::uint32_t v = 0x00102030u;  // A = 0
                std::memcpy(buf.data() + i, &v, 4);
            }
            lift::GlyphBlit args = b;
            args.dst = buf.data();
            (void)lift::blendGlyph46D9F0(args);
            BlitResult r;
            r.pixels = buf;
            LIFT_CHECK_EQ_U32(px32(r, 0, 0), 0x7D204080u);  // A = 125
        }
        // ---- ⑭ 返回值：h <= 0 的出口 EAX = 0（0x46DA50 / 0x46DC9D）；w <= 0 不影响返回值 ----
        {
            const std::uint8_t src[8] = {0x80, 0, 0, 0, 0x80, 0, 0, 0};
            lift::GlyphBlit b = base;
            b.src = src;
            b.height = 0;
            LIFT_CHECK_EQ_U32(runBlit(b, 16).eax, 0u);
            b = base;
            b.src = src;
            b.width = 0;
            const BlitResult r = runBlit(b, 16);
            LIFT_CHECK_EQ_U32(r.eax, 4u);  // h > 0 ⇒ EAX = a5
            for (int y = 0; y < 2; ++y)
                for (int x = 0; x < 4; ++x) LIFT_CHECK_EQ_U32(px16(r, x, y), kUnchanged16);
            // 32 位支同样：w = 0 / h = 0 一个字节都不写，EAX 分别是 a5 / 0
            b = base;
            b.sixteen_bpp = false;
            b.dst_x = 0;
            b.src = src;
            b.width = 0;
            const BlitResult r32 = runBlit(b, 32);
            LIFT_CHECK_EQ_U32(r32.eax, 4u);
            for (int y = 0; y < 2; ++y)
                for (int x = 0; x < 2; ++x) LIFT_CHECK_EQ_U32(px32(r32, x, y), kUnchanged32);
            b.height = 0;
            LIFT_CHECK_EQ_U32(runBlit(b, 32).eax, 0u);
        }
        // ---- ⑮ 覆盖值 = 1 档（级数 = 1）：16 位被 `sar 4` 吃成 0 ⇒ **不写**；32 位得 3 ⇒ 写 ----
        //   65 级：1×255 = 255，÷65 = 3（255/65 = 3.92 ⇒ 3）⇒ 16 位 `>>4` = 0；32 位 = 3。
        //   ★ 这就是 spec 侧记的"最低一档灰度被降位吃掉"（`docs/02-engine/spec-audit-2026-10.md` §94）。
        //   ⚠ 覆盖值**恰好 = 1** 在本函数里不可达（除数是 65 / 17 ⇒ 最小非零档是 3 / 15）——
        //     需求单写的"0 / 1 / 中 / 满"里的"1"按"最低非零档"理解，这里钉的是它。
        {
            const std::uint8_t src[8] = {1, 1, 1, 1, 1, 1, 1, 1};
            lift::GlyphBlit b = base;
            b.source = lift::GlyphMaskSource::Level8;
            b.levels = lift::GlyphLevels::Levels65;
            b.src = src;
            const BlitResult r16 = runBlit(b, 16);
            for (int y = 0; y < 2; ++y)
                for (int x = 0; x < 4; ++x) LIFT_CHECK_EQ_U32(px16(r16, x, y), kUnchanged16);
            lift::GlyphBlit b32 = b;
            b32.sixteen_bpp = false;
            b32.dst_x = 0;
            std::vector<std::uint8_t> buf(32, 0);  // 目标 A = 0 ⇒ 覆写，A = 覆盖值 3
            lift::GlyphBlit args = b32;
            args.dst = buf.data();
            (void)lift::blendGlyph46D9F0(args);
            BlitResult r;
            r.pixels = buf;
            LIFT_CHECK_EQ_U32(px32(r, 0, 0), 0x03204080u);  // (3<<24) | flat
            LIFT_CHECK_EQ_U32(px32(r, 1, 0), 0x03204080u);
        }
        // ---- ⑯ 16 位 × 覆盖值(7) > 目标 A(5) ⇒ 输出 A = 覆盖值（cov>dstA 的 max 侧）----
        //   dst = 0x5A5C（A=5）⇒ A 应为 7；`max` → `+` 会给出 (5+7)&0xF = 0xC。
        {
            const std::uint8_t src[8] = {32, 32, 32, 32, 32, 32, 32, 32};
            lift::GlyphBlit b = base;
            b.source = lift::GlyphMaskSource::Level8;
            b.levels = lift::GlyphLevels::Levels65;
            b.src = src;
            std::vector<std::uint8_t> buf(16, 0);
            for (std::size_t i = 0; i < buf.size(); i += 2) {
                const std::uint16_t v = 0x5A5Cu;
                std::memcpy(buf.data() + i, &v, 2);
            }
            lift::GlyphBlit args = b;
            args.dst = buf.data();
            (void)lift::blendGlyph46D9F0(args);
            BlitResult r;
            r.pixels = buf;
            LIFT_CHECK_EQ_U32(px16(r, 1, 0), 0x764Au);
        }
        // ---- ⑰ 32 位 × 裁剪（左 / 右 / 下越界）⇒ 一个字节都不写 ----
        {
            const std::uint8_t src[8] = {0xC0, 0, 0, 0, 0xC0, 0, 0, 0};
            lift::GlyphBlit b = base;
            b.sixteen_bpp = false;
            b.src = src;
            b.dst_x = -3;
            BlitResult r = runBlit(b, 32);
            for (int y = 0; y < 2; ++y)
                for (int x = 0; x < 2; ++x) LIFT_CHECK_EQ_U32(px32(r, x, y), kUnchanged32);
            b = base;
            b.sixteen_bpp = false;
            b.src = src;
            b.dst_x = 4;
            r = runBlit(b, 32);
            for (int y = 0; y < 2; ++y)
                for (int x = 0; x < 2; ++x) LIFT_CHECK_EQ_U32(px32(r, x, y), kUnchanged32);
            b = base;
            b.sixteen_bpp = false;
            b.src = src;
            b.dst_y = 2;
            r = runBlit(b, 32);
            for (int y = 0; y < 2; ++y)
                for (int x = 0; x < 2; ++x) LIFT_CHECK_EQ_U32(px32(r, x, y), kUnchanged32);
            b = base;
            b.sixteen_bpp = false;
            b.src = src;
            b.dst_y = -1;  // y = -1（裁掉）, 0（写）
            r = runBlit(b, 32);
            LIFT_CHECK_EQ_U32(px32(r, 1, 0), 0xFF000000u | kFlat32);
            LIFT_CHECK_EQ_U32(px32(r, 1, 1), kUnchanged32);
        }
    }

    // =====================================================================================
    // ⑳ `sub_46D9F0` 的**准入层**：合成 guest 内存（Font / mgr / 纹理槽 / 裁剪对象 / 参数 / 表面）
    //
    // 判据：① 正常路径写出来的字节与纯核一致、EAX 对；② 每一条 Declined 判据都**明确回退**，
    //       且一个字节都不改（失败不留半状态）；③ 这些判据全部在写第一个像素之前判完。
    // =====================================================================================
    {
        lift_test::g_current_case = "glyph-blend-admit-46D9F0";
        constexpr std::uint32_t kFontEa = 0x00420000u;
        constexpr std::uint32_t kMgrEa = 0x00460000u;
        constexpr std::uint32_t kClipEa = 0x00470000u;
        constexpr std::uint32_t kSrcEa = 0x00480000u;
        constexpr std::uint32_t kDstEa = 0x00490000u;
        constexpr std::uint32_t kMetricsEa = 0x004A0000u;
        constexpr std::uint32_t kTextureBit = 0x005518C0u;
        auto bootRig = [&](lift::Machine& m, std::string& err, std::uint32_t texid, std::uint32_t display_bpp) {
            if (!m.initBare(&err) || !m.prepareStack(&err)) return false;
            const std::vector<std::uint8_t> zero(0x420, 0);
            if (!m.placeData(kFontEa, zero.data(), zero.size(), &err)) return false;
            if (!m.placeData(kFontEa + 0x548u, zero.data(), 4u, &err)) return false;
            if (!m.placeData(kFontEa + 0x35584u, zero.data(), 4u, &err)) return false;
            const std::vector<std::uint8_t> mgr_zero(0x10, 0);
            if (!m.placeData(kMgrEa + 0xA5D8u + 4u * texid, mgr_zero.data(), 4u, &err)) return false;
            if (!m.placeData(kMgrEa + 0xB648u, mgr_zero.data(), 4u, &err)) return false;
            if (!m.placeData(kClipEa + 0x41Cu, mgr_zero.data(), 16u, &err)) return false;
            if (!m.placeData(kMetricsEa, mgr_zero.data(), 16u, &err)) return false;
            if (!m.placeData(kTextureBit, mgr_zero.data(), 4u, &err)) return false;
            const std::vector<std::uint8_t> dst(16, 0x5A);
            if (!m.placeData(kDstEa, dst.data(), dst.size(), &err)) return false;
            const std::uint8_t src_bytes[8] = {0x80, 0, 0, 0, 0x80, 0, 0, 0};
            if (!m.placeData(kSrcEa, src_bytes, sizeof(src_bytes), &err)) return false;
            auto& mem = m.mem();
            mem.wr32(kFontEa + 0x410u, kMgrEa);
            mem.wr32(kFontEa + 0x548u, 0u);    // 1bpp 掩码
            mem.wr32(kFontEa + 0x35584u, 0u);  // 17 级
            mem.wr32(kMgrEa + 0xB648u, texid);
            mem.wr32(kMgrEa + 0xA5D8u + 4u * texid, kClipEa);
            mem.wr32(kClipEa + 0x41Cu, 0u);
            mem.wr32(kClipEa + 0x420u, 0u);
            mem.wr32(kClipEa + 0x424u, 4u);
            mem.wr32(kClipEa + 0x428u, 2u);
            mem.wr32(kMetricsEa + 0u, 2u);   // 宽
            mem.wr32(kMetricsEa + 4u, 2u);   // 高
            mem.wr32(kMetricsEa + 8u, 0u);   // 原点 x
            mem.wr32(kMetricsEa + 12u, 0u);  // 原点 y
            mem.wr32(kTextureBit, display_bpp);
            return true;
        };
        // 8 个栈参数（pitch / bits / a4 / stride / src / x / y / color）+ 哨兵返回地址
        auto arm = [&](lift::Machine& m, std::uint32_t pitch, std::uint32_t bits, std::uint32_t src,
                       std::uint32_t x, std::uint32_t y) {
            auto& cpu = m.cpu();
            auto& mem = m.mem();
            const std::uint32_t esp = m.layout().stack_top - 0x40u;
            cpu.esp() = esp;
            cpu.ecx() = kFontEa;
            mem.wr32(esp, lift_fixture::kSentinel);
            mem.wr32(esp + 4u, pitch);
            mem.wr32(esp + 8u, bits);
            mem.wr32(esp + 12u, kMetricsEa);
            mem.wr32(esp + 16u, 4u);
            mem.wr32(esp + 20u, src);
            mem.wr32(esp + 24u, x);
            mem.wr32(esp + 28u, y);
            mem.wr32(esp + 32u, 0x00804020u);
        };
        auto dstBytes = [&](lift::Machine& m) {
            std::vector<std::uint8_t> v(16);
            std::memcpy(v.data(), m.mem().host(kDstEa), v.size());
            return v;
        };
        {
            lift::Machine m;
            std::string err;
            LIFT_CHECK(bootRig(m, err, 0u, 16u));
            arm(m, 8u, kDstEa, kSrcEa, 1u, 0u);
            std::uint32_t r = 0xDEADBEEFu;
            LIFT_CHECK(lift::nativeBlend46D9F0(m.cpu(), m.mem(), &r));
            LIFT_CHECK_EQ_U32(r, 4u);  // EAX = a5
            const std::vector<std::uint8_t> dst = dstBytes(m);
            auto word = [&](int x, int y) {
                std::uint16_t v = 0;
                std::memcpy(&v, dst.data() + static_cast<std::size_t>(y) * 8u + static_cast<std::size_t>(x) * 2u, 2);
                return static_cast<std::uint32_t>(v);
            };
            LIFT_CHECK_EQ_U32(word(1, 0), (0xFu << 12) | 0x0248u);
            LIFT_CHECK_EQ_U32(word(1, 1), (0xFu << 12) | 0x0248u);
            LIFT_CHECK_EQ_U32(word(2, 0), 0x5A5Au);
        }
        // 逐条 Declined：返回值 false + 目标缓冲一个字节都没改
        {
            const std::vector<std::uint8_t> frozen(16, 0x5A);
            // ① this = 0
            {
                lift::Machine m;
                std::string err;
                LIFT_CHECK(bootRig(m, err, 0u, 16u));
                arm(m, 8u, kDstEa, kSrcEa, 1u, 0u);
                m.cpu().ecx() = 0u;
                std::uint32_t r = 0xDEADBEEFu;
                LIFT_CHECK(!lift::nativeBlend46D9F0(m.cpu(), m.mem(), &r));
                LIFT_CHECK(dstBytes(m) == frozen);
            }
            // ② mgr = 0
            {
                lift::Machine m;
                std::string err;
                LIFT_CHECK(bootRig(m, err, 0u, 16u));
                arm(m, 8u, kDstEa, kSrcEa, 1u, 0u);
                m.mem().wr32(kFontEa + 0x410u, 0u);
                std::uint32_t r = 0xDEADBEEFu;
                LIFT_CHECK(!lift::nativeBlend46D9F0(m.cpu(), m.mem(), &r));
                LIFT_CHECK(dstBytes(m) == frozen);
            }
            // ③ texid 越界（1000 = 槽表容量）
            {
                lift::Machine m;
                std::string err;
                LIFT_CHECK(bootRig(m, err, 0u, 16u));
                arm(m, 8u, kDstEa, kSrcEa, 1u, 0u);
                m.mem().wr32(kMgrEa + 0xB648u, 1000u);
                std::uint32_t r = 0xDEADBEEFu;
                LIFT_CHECK(!lift::nativeBlend46D9F0(m.cpu(), m.mem(), &r));
                LIFT_CHECK(dstBytes(m) == frozen);
            }
            // ④ 纹理槽指针 = 0（guest 会走 sprintf_s 错误路径 ⇒ 不许接手）
            {
                lift::Machine m;
                std::string err;
                LIFT_CHECK(bootRig(m, err, 0u, 16u));
                arm(m, 8u, kDstEa, kSrcEa, 1u, 0u);
                m.mem().wr32(kMgrEa + 0xA5D8u, 0u);
                std::uint32_t r = 0xDEADBEEFu;
                LIFT_CHECK(!lift::nativeBlend46D9F0(m.cpu(), m.mem(), &r));
                LIFT_CHECK(dstBytes(m) == frozen);
            }
            // ⑤ a4（GLYPHMETRICS）未提交
            {
                lift::Machine m;
                std::string err;
                LIFT_CHECK(bootRig(m, err, 0u, 16u));
                auto& mem = m.mem();
                const std::uint32_t esp = m.layout().stack_top - 0x40u;
                arm(m, 8u, kDstEa, kSrcEa, 1u, 0u);
                mem.wr32(esp + 12u, 0x00A00000u);  // 没 placeData 过 ⇒ 未提交
                std::uint32_t r = 0xDEADBEEFu;
                LIFT_CHECK(!lift::nativeBlend46D9F0(m.cpu(), m.mem(), &r));
                LIFT_CHECK(dstBytes(m) == frozen);
            }
            // ⑥ pitch 的元素数 <= 0（16 位 pitch=1 ⇒ pe=0）
            {
                lift::Machine m;
                std::string err;
                LIFT_CHECK(bootRig(m, err, 0u, 16u));
                arm(m, 1u, kDstEa, kSrcEa, 1u, 0u);
                std::uint32_t r = 0xDEADBEEFu;
                LIFT_CHECK(!lift::nativeBlend46D9F0(m.cpu(), m.mem(), &r));
                LIFT_CHECK(dstBytes(m) == frozen);
            }
            // ⑦ 源基址 = 0
            {
                lift::Machine m;
                std::string err;
                LIFT_CHECK(bootRig(m, err, 0u, 16u));
                arm(m, 8u, kDstEa, 0u, 1u, 0u);
                std::uint32_t r = 0xDEADBEEFu;
                LIFT_CHECK(!lift::nativeBlend46D9F0(m.cpu(), m.mem(), &r));
                LIFT_CHECK(dstBytes(m) == frozen);
            }
            // ⑧ 目标基址 = 0
            {
                lift::Machine m;
                std::string err;
                LIFT_CHECK(bootRig(m, err, 0u, 16u));
                arm(m, 8u, 0u, kSrcEa, 1u, 0u);
                std::uint32_t r = 0xDEADBEEFu;
                LIFT_CHECK(!lift::nativeBlend46D9F0(m.cpu(), m.mem(), &r));
                LIFT_CHECK(dstBytes(m) == frozen);
            }
            // ⑨ 显示位深不是 16/32
            {
                lift::Machine m;
                std::string err;
                LIFT_CHECK(bootRig(m, err, 0u, 24u));
                arm(m, 8u, kDstEa, kSrcEa, 1u, 0u);
                std::uint32_t r = 0xDEADBEEFu;
                LIFT_CHECK(!lift::nativeBlend46D9F0(m.cpu(), m.mem(), &r));
                LIFT_CHECK(dstBytes(m) == frozen);
            }
            // ⑩ 源跨距越过 4 GiB（src = 0xFFFFFFF0）
            {
                lift::Machine m;
                std::string err;
                LIFT_CHECK(bootRig(m, err, 0u, 16u));
                arm(m, 8u, kDstEa, 0xFFFFFFF0u, 1u, 0u);
                std::uint32_t r = 0xDEADBEEFu;
                LIFT_CHECK(!lift::nativeBlend46D9F0(m.cpu(), m.mem(), &r));
                LIFT_CHECK(dstBytes(m) == frozen);
            }
            // ⑪ 8bpp 级数越界（raw = 200 > 65 级的满值 64）
            {
                lift::Machine m;
                std::string err;
                LIFT_CHECK(bootRig(m, err, 0u, 16u));
                m.mem().wr32(kFontEa + 0x548u, 1u);  // 8bpp
                m.mem().wr32(kFontEa + 0x35584u, 1u);  // 65 级
                for (std::uint32_t i = 0; i < 8u; ++i) m.mem().wr8(kSrcEa + i, 200u);
                arm(m, 8u, kDstEa, kSrcEa, 1u, 0u);
                std::uint32_t r = 0xDEADBEEFu;
                LIFT_CHECK(!lift::nativeBlend46D9F0(m.cpu(), m.mem(), &r));
                LIFT_CHECK(dstBytes(m) == frozen);
            }
            // ⑫ 目标跨度未提交（bits 指向没 placeData 过的页）⇒ **先判后碰**，不许撞宿主指针
            {
                lift::Machine m;
                std::string err;
                LIFT_CHECK(bootRig(m, err, 0u, 16u));
                arm(m, 8u, 0x00B00000u, kSrcEa, 1u, 0u);
                std::uint32_t r = 0xDEADBEEFu;
                LIFT_CHECK(!lift::nativeBlend46D9F0(m.cpu(), m.mem(), &r));
                LIFT_CHECK(dstBytes(m) == frozen);
            }
            // ⑬ 32 位 × pitch 元素 <= 0（pitch = 3 ⇒ truncDiv4 = 0）
            {
                lift::Machine m;
                std::string err;
                LIFT_CHECK(bootRig(m, err, 0u, 32u));
                arm(m, 3u, kDstEa, kSrcEa, 1u, 0u);
                std::uint32_t r = 0xDEADBEEFu;
                LIFT_CHECK(!lift::nativeBlend46D9F0(m.cpu(), m.mem(), &r));
                LIFT_CHECK(dstBytes(m) == frozen);
            }
            // ⑭ 源跨度未提交（src 指向没 placeData 过的页）：1bpp 与 8bpp（后者要预扫级数）都必须干净回退
            //   ★ 8bpp 这一条钉的是"先算跨度 + 提交检查，**再**碰源字节"（否则会撞宿主 fault 而不是回退）。
            {
                lift::Machine m;
                std::string err;
                LIFT_CHECK(bootRig(m, err, 0u, 16u));
                arm(m, 8u, kDstEa, 0x00C00000u, 1u, 0u);
                std::uint32_t r = 0xDEADBEEFu;
                LIFT_CHECK(!lift::nativeBlend46D9F0(m.cpu(), m.mem(), &r));
                LIFT_CHECK(dstBytes(m) == frozen);
                lift::Machine m8;
                LIFT_CHECK(bootRig(m8, err, 0u, 16u));
                m8.mem().wr32(kFontEa + 0x548u, 1u);  // 8bpp ⇒ 预扫会碰源字节
                arm(m8, 8u, kDstEa, 0x00C00000u, 1u, 0u);
                r = 0xDEADBEEFu;
                LIFT_CHECK(!lift::nativeBlend46D9F0(m8.cpu(), m8.mem(), &r));
                LIFT_CHECK(dstBytes(m8) == frozen);
            }
        }
    }

    // ---- ⑰b sub_4CCC60 的仿射纯核：符号 / 零的符号位 / 单点的 faddp 顺序 ----
    //   ★ 逐位判据在 assets 档（真机器码 vs override）；这里钉住能静态判死的部分：
    //     θ=0 ⇒ sin=+0 / cos=1（精确）⇒ A=r*sx、B=-(r*sy*+0)、C=r*sy、D=sx*r*+0；
    //     以及 `out0 = iy*B + ix*A + add4` / `out1 = iy*C + ix*D + add8` 的乘加顺序。
    {
        lift::x87ApplyControlWord(0x037Fu);  // 默认控制字（64 位尾数 / 最近偶数）
        auto extFromFloat = [](float f, lift::Ext80 out) {
            std::uint8_t b[4];
            std::memcpy(b, &f, 4);
            lift::x87MemToReg(b, out, 4);
        };

        LIFT_CHECK(lift::affineAngleInRange(0.0f, 0.017453292f));
        LIFT_CHECK(lift::affineAngleInRange(359.0f, 0.017453292f));
        LIFT_CHECK(!lift::affineAngleInRange(1.0e30f, 0.017453292f));  // |θ| >= 2^63 ⇒ 交解释器
        LIFT_CHECK(!lift::affineAngleInRange(std::nanf(""), 1.0f));

        lift::Ext80 angle, scale, radius;
        extFromFloat(0.0f, angle);
        extFromFloat(1.0f, scale);
        extFromFloat(2.0f, radius);
        lift::AffineCoeffs k;
        LIFT_CHECK(lift::affineCoeffs(angle, scale, radius, false, false, &k));
        lift::Ext80 two, minusTwo;
        lift::x87FromI32(2, two);
        lift::x87FromI32(-2, minusTwo);
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(lift::x87Compare(k.a, two)), 0u);  // A = +2
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(lift::x87Compare(k.c, two)), 0u);  // C = +2
        LIFT_CHECK(lift::x87IsZero(k.b) && lift::x87SignBit(k.b));    // B = -(r*1*+0) = -0
        LIFT_CHECK(lift::x87IsZero(k.d) && !lift::x87SignBit(k.d));   // D = +0*sx*r = +0

        // flip_x ⇒ sx=-1：A/D 反号；B 的零符号只由 sy 决定（θ=0 时 sin=+0）⇒ 仍是 -0。
        LIFT_CHECK(lift::affineCoeffs(angle, scale, radius, true, false, &k));
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(lift::x87Compare(k.a, minusTwo)), 0u);
        LIFT_CHECK(lift::x87IsZero(k.b) && lift::x87SignBit(k.b));  // -(r*1*+0) = -0
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(lift::x87Compare(k.c, two)), 0u);
        LIFT_CHECK(lift::x87IsZero(k.d) && lift::x87SignBit(k.d));  // +0 * (sx*r) = -0
        // flip_y ⇒ sy=-1：C 反号；`-(r*(-1)*+0)` 先得 -0、fchs 后是 **+0**。
        LIFT_CHECK(lift::affineCoeffs(angle, scale, radius, false, true, &k));
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(lift::x87Compare(k.a, two)), 0u);
        LIFT_CHECK(lift::x87IsZero(k.b) && !lift::x87SignBit(k.b));  // +0
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(lift::x87Compare(k.c, minusTwo)), 0u);
        LIFT_CHECK(lift::x87IsZero(k.d) && !lift::x87SignBit(k.d));  // +0

        // 单点：A=1 B=10 C=100 D=1000（整数在 ext80 精确）⇒
        //   out0 = 3*10 + 2*1 + 7 = 39；out1 = 3*100 + 2*1000 + 8 = 2308。
        //   ⛔ 把 ix/iy 或两个平移对调，这两条会当场红。
        lift::AffineCoeffs m;
        lift::x87FromI32(1, m.a);
        lift::x87FromI32(10, m.b);
        lift::x87FromI32(100, m.c);
        lift::x87FromI32(1000, m.d);
        lift::Ext80 ix, iy, add4, add8, o0, o1;
        lift::x87FromI32(2, ix);
        lift::x87FromI32(3, iy);
        lift::x87FromI32(7, add4);
        lift::x87FromI32(8, add8);
        lift::affinePoint(m, ix, iy, add4, add8, o0, o1);
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(lift::x87ToI32(o0, false)), 39u);
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(lift::x87ToI32(o1, false)), 2308u);

        // 进入 fsin 的 C2 区（|θ| >= 2^63）⇒ 核本身也要防御性返回 false。
        lift::Ext80 huge, one;
        extFromFloat(1.0e30f, huge);
        extFromFloat(1.0f, one);
        LIFT_CHECK(!lift::affineCoeffs(huge, one, radius, false, false, &k));
    }

    // ---- ⑰c sub_4CCC60 的**准入层**：Declined 必须**零写**（照 ⑳/sub_46D9F0 的 ⑫⑬⑭ 形制）----
    // ★ 为什么必须有这一组：assets 档的两臂差分在回退之后由解释器**重放整个函数**，会把
    //   "回退前写了 dst / 动了 x87" 修回去 ⇒ 那类错在差分里**结构上不可见**。
    //   这里**直接调** `lift::nativeAffine4CCC60`（不经 dispatcher）+ 冻结快照。
    // ★ 判据（每一条代表一个 `return false` 的**类别**）：返回 false；`dst` 逐字节不变；
    //   `fpu.status / control / tag / top` 不变；`*result` 里不留非 0 的"半成品 EAX"。
    {
        lift_test::g_current_case = "affine/decline-no-write";
        constexpr std::uint32_t kStateEa = 0x00420000u;
        constexpr std::uint32_t kObjEa = 0x00430000u;
        constexpr std::uint32_t kSrcEa = 0x00480000u;
        constexpr std::uint32_t kDstEa = 0x00490000u;
        constexpr std::uint32_t kScaleEa = 0x0052F1B0u;  // `flt_52F1B0`（guest `.data` 常量）
        constexpr std::uint32_t kDstBytes = 64u;
        auto bootRig = [&](lift::Machine& m, std::string& err) {
            if (!m.initBare(&err) || !m.prepareStack(&err)) return false;
            auto& mem = m.mem();
            std::vector<std::uint8_t> state(0x28u, 0), obj(0x20u, 0);
            const float radius = 1.234567f, angle = 17.0f, scale = 0.017453292f;
            std::memcpy(state.data() + 0x10u, &radius, 4);
            const std::uint32_t inner = kObjEa;
            std::memcpy(state.data() + 0x20u, &inner, 4);  // `[state+0x24] ?: [state+0x20]`
            const std::uint32_t angle_bits = [&] {
                std::uint32_t v = 0;
                std::memcpy(&v, &angle, 4);
                return v;
            }();
            std::memcpy(obj.data() + 0x14u, &angle_bits, 4);
            const std::uint32_t scale_bits = [&] {
                std::uint32_t v = 0;
                std::memcpy(&v, &scale, 4);
                return v;
            }();
            if (!m.placeData(kStateEa, state.data(), state.size(), &err)) return false;
            if (!m.placeData(kObjEa, obj.data(), obj.size(), &err)) return false;
            if (!m.placeData(kScaleEa, &scale_bits, 4u, &err)) return false;
            const std::vector<std::uint8_t> src(kDstBytes / 4u * 4u, 0);
            std::vector<std::uint8_t> dst(kDstBytes, 0x5A);
            if (!m.placeData(kSrcEa, src.data(), src.size(), &err)) return false;
            if (!m.placeData(kDstEa, dst.data(), dst.size(), &err)) return false;
            return true;
        };
        // 7 个栈参数（`__stdcall retn 1Ch` = 28 B）+ 哨兵返回地址；a1（`[ebp+8]`）guest 全程不读。
        auto arm = [&](lift::Machine& m, std::uint32_t dst, std::uint32_t count, std::uint32_t i0,
                       std::uint32_t stride) {
            auto& cpu = m.cpu();
            auto& mem = m.mem();
            const std::uint32_t esp = m.layout().stack_top - 0x40u;
            cpu.esp() = esp;
            mem.wr32(esp, lift_fixture::kSentinel);
            mem.wr32(esp + 4u, 0u);         // a1：guest 不读
            mem.wr32(esp + 8u, kStateEa);   // a2 = state
            mem.wr32(esp + 12u, kSrcEa);    // a3 = src
            mem.wr32(esp + 16u, dst);       // a4 = dst
            mem.wr32(esp + 20u, count);     // a5
            mem.wr32(esp + 24u, i0);        // a6
            mem.wr32(esp + 28u, stride);    // a7
        };
        auto dstBytes = [&](lift::Machine& m) {
            std::vector<std::uint8_t> v(kDstBytes);
            std::memcpy(v.data(), m.mem().host(kDstEa), v.size());
            return v;
        };
        struct FpuSnap {
            std::uint16_t status = 0, control = 0, tag = 0;
            std::uint8_t top = 0;
        };
        auto fpuSnap = [&](lift::Machine& m) {
            FpuSnap s;
            s.status = m.cpu().fpu.status;
            s.control = m.cpu().fpu.control;
            s.tag = m.cpu().fpu.tag;
            s.top = m.cpu().fpu.top;
            return s;
        };
        const std::vector<std::uint8_t> frozen(kDstBytes, 0x5A);
        auto expectDecline = [&](const char* what, std::uint32_t dst, std::uint32_t count, std::uint32_t i0,
                                std::uint32_t stride) {
            (void)what;  // 分支名只给读代码的人（断言消息里已经有表达式本身）
            lift::Machine m;
            std::string err;
            const bool rig_ok = bootRig(m, err);
            LIFT_CHECK(rig_ok);
            if (!rig_ok) return;
            arm(m, dst, count, i0, stride);
            const FpuSnap before = fpuSnap(m);
            std::uint32_t r = 0xDEADBEEFu;
            LIFT_CHECK(!lift::nativeAffine4CCC60(m.cpu(), m.mem(), &r));
            LIFT_CHECK(dstBytes(m) == frozen);
            const FpuSnap after = fpuSnap(m);
            LIFT_CHECK_EQ_U32(after.status, before.status);
            LIFT_CHECK_EQ_U32(after.control, before.control);
            LIFT_CHECK_EQ_U32(after.tag, before.tag);
            LIFT_CHECK_EQ_U32(after.top, before.top);
        };
        // ① 负起点（`i0 < 0`，非空循环 ⇒ guest 会读写 src/dst 的负偏移）
        expectDecline("i0<0", kDstEa, 2u, 0xFFFFFFFFu, 1u);
        // ② `dst` 跨度未提交（那一段从没 `placeData` 过）
        expectDecline("dst-uncommitted", 0x00B00000u, 2u, 0u, 2u);
        // ③ x87 栈非空（guest 会走 push 上溢路径 ⇒ 无法复刻）
        {
            lift::Machine m;
            std::string err;
            const bool rig_ok = bootRig(m, err);
            LIFT_CHECK(rig_ok);
            if (rig_ok) {
                arm(m, kDstEa, 2u, 0u, 2u);
                m.cpu().fpu.setValid(0);  // 标记 ST(0) 非空（`fpuStackEmpty` 的判据是 tag）
                const FpuSnap before = fpuSnap(m);
                std::uint32_t r = 0xDEADBEEFu;
                LIFT_CHECK(!lift::nativeAffine4CCC60(m.cpu(), m.mem(), &r));
                LIFT_CHECK(dstBytes(m) == frozen);
                const FpuSnap after = fpuSnap(m);
                LIFT_CHECK_EQ_U32(after.status, before.status);
                LIFT_CHECK_EQ_U32(after.control, before.control);
                LIFT_CHECK_EQ_U32(after.tag, before.tag);
                LIFT_CHECK_EQ_U32(after.top, before.top);
            }
        }
    }

    // ---- ⑰d `__ftol2_sse`（`0x4E82C0`）：准入**零写** + ABI（`EDX:EAX`）+ 栈平衡 ----
    //
    // 判据（红了意味着什么）：
    //   ① **准入零写**：每条闸门不成立时 `nativeFtol2Sse` 返回 false，且**直接调 fn**前后的冻结快照
    //      逐项相同（`esp` / 8 个 x87 寄存器槽（16 B × 8）/ `status` / `control` / `tag` / `top` /
    //      宿主 softfloat 的粘性位 / 栈 scratch 的 0x20 B / `[0x55F2E8]`），`*result` 不留半成品；
    //      回退原因落进**对应**的 `declines[]` 桶（⛔ 不是"静默跳过"）。
    //   ② ABI：成功路径 `invoke()` 之后 EIP = 返回地址、ESP = esp + 4（cdecl 只弹返回地址）、
    //      EAX = 低 32 位、**EDX = 高 32 位**（两项都写才算 64 位返回；`-1` 那档 EDX 是 0xFFFFFFFF）。
    //   ③ 栈平衡：连续 6 次调用各消费 1 层（TOP 每次 +1、被消费的物理槽置 empty、返回值逐次正确）。
    // ★ 为什么这一组**不能**用 assets 档的两臂差分代替：回退之后解释器会把整个函数重放，
    //   把"回退前写过的东西"修回去 ⇒ 那一类错在差分里**结构上不可见**（`affine/decline-no-write`
    //   同口径）。
    // ★ 宿主粘性位在快照前**人为置上**（`x87RaiseInvalid`）：这样"回退路径有没有偷偷 take 掉
    //   异常位"也是可判的（`x87TakeExceptionFlags` 是**取走并清空**）。
    {
        lift_test::g_current_case = "ftol/decline-no-write";
        constexpr std::uint32_t kFlagEa = 0x0055F2E8u;
        constexpr std::uint32_t kUncommitted = 0x00B00000u;  // 从不 placeData ⇒ 未提交页
        auto extFromF64 = [](double d, lift::Ext80 out) {
            std::uint8_t b[8];
            std::memcpy(b, &d, 8);
            lift::x87MemToReg(b, out, 8);
        };
        // 夹具：bare 机 + `[0x55F2E8]`（可故意不铺）+ 表里那一项
        auto bootFtol = [&](lift::Machine& m, std::string& err, bool with_flag) {
            if (!m.initBare(&err) || !m.prepareStack(&err)) return false;
            if (with_flag) {
                const std::uint32_t zero = 0;
                if (!m.placeData(kFlagEa, &zero, 4u, &err)) return false;
            }
            std::string e;
            return m.overrides().add(0x004E82C0u, "ref_ftol", lift::GuestConv::Cdecl, 0, lift::nativeFtol2Sse, &e);
        };
        struct FtolSnap {
            std::uint32_t esp = 0;
            std::uint16_t status = 0, control = 0, tag = 0;
            std::uint8_t top = 0;
            std::uint8_t regs[8][16] = {};
            std::uint8_t scratch[0x20] = {};
            std::uint8_t exc = 0;
            std::uint32_t flag = 0;
        };
        auto ftolSnap = [&](lift::Machine& m, std::uint32_t frame) {
            FtolSnap s;
            s.esp = m.cpu().esp();
            s.status = m.cpu().fpu.status;
            s.control = m.cpu().fpu.control;
            s.tag = m.cpu().fpu.tag;
            s.top = m.cpu().fpu.top;
            std::memcpy(s.regs, m.cpu().fpu.regs, sizeof(s.regs));
            // ★ 未提交页**不能读**（`host()` 返回的是保留地址空间里的裸指针，读它会 AV）⇒ 那一档
            //   的 scratch 快照留全 0，两臂比较仍然是"相等"（真正要判的是 fn 没写 guest 内存）。
            if (m.mem().committed(frame, 0x20u)) std::memcpy(s.scratch, m.mem().host(frame), 0x20u);
            s.exc = lift::x87PeekExceptionFlags();
            s.flag = m.mem().committed(kFlagEa, 4u) ? m.mem().rd32(kFlagEa) : 0xFFFFFFFFu;
            return s;
        };
        auto sameFtolSnap = [](const FtolSnap& a, const FtolSnap& b) {
            return a.esp == b.esp && a.status == b.status && a.control == b.control && a.tag == b.tag &&
                   a.top == b.top && a.exc == b.exc && a.flag == b.flag &&
                   std::memcmp(a.regs, b.regs, sizeof(a.regs)) == 0 &&
                   std::memcmp(a.scratch, b.scratch, sizeof(a.scratch)) == 0;
        };
        auto checkDecline = [&](const char* what, lift::FtolWhy why, auto&& setup, bool with_flag) {
            (void)what;  // 分支名只给读代码的人（断言消息里已有表达式本身）
            lift::Machine m;
            std::string err;
            const bool rig = bootFtol(m, err, with_flag);
            LIFT_CHECK(rig);
            if (!rig) return;
            // 缺省夹具：`st(0)` = 2.7（已置 valid）、esp 落在已提交栈上
            lift::Ext80 v;
            extFromF64(2.7, v);
            std::memcpy(m.cpu().fpu.regs[m.cpu().fpu.phys(0)], v, 16);
            m.cpu().fpu.setValid(m.cpu().fpu.phys(0));
            m.cpu().esp() = m.layout().stack_top - 0x40u;
            setup(m);
            const std::uint32_t esp_before = m.cpu().esp();
            const std::uint32_t frame = (esp_before - 0x24u) & ~15u;
            // scratch 哨兵（只在已提交时铺：未提交页写不得 —— 那一档要的正是"没铺"）
            if (m.mem().committed(frame, 0x20u))
                for (std::uint32_t i = 0; i < 0x20u; ++i) m.mem().wr8(frame + i, 0x5Au);
            lift::x87ApplyControlWord(0x037Fu);
            lift::x87ClearExceptionFlags();
            lift::x87RaiseInvalid();  // 宿主粘性位预置：回退路径不许把它 take 走
            const FtolSnap before = ftolSnap(m, frame);
            lift::NativeOverride* ov = m.overrides().at(0);
            LIFT_CHECK(ov != nullptr);
            if (ov == nullptr) return;
            std::uint32_t r = 0xDEADBEEFu;
            m.cpu().current_override = ov;
            const bool took = lift::nativeFtol2Sse(m.cpu(), m.mem(), &r);
            m.cpu().current_override = nullptr;
            LIFT_CHECK(!took);
            LIFT_CHECK_EQ_U32(r, 0u);  // ⛔ 不留"半成品 EAX"
            LIFT_CHECK(sameFtolSnap(before, ftolSnap(m, frame)));
            LIFT_CHECK_EQ_U32(m.cpu().esp(), esp_before);
            LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(ov->declines[static_cast<std::size_t>(why)]), 1u);
            LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(ov->fallbacks), 0u);  // fallbacks 是 invoke 记的
        };
        // ① `[0x55F2E8]` 未提交 ⇒ 读不出"走哪条路"（非 safe mode 的 rd32 会静默给 0）
        checkDecline("flag-unreadable", lift::FtolWhy::FlagUnreadable, [](lift::Machine&) {}, false);
        // ② `[0x55F2E8] != 0` ⇒ 真机走 `cvttsd2si`（只写 EAX、越界语义不同）⇒ 必须回退
        checkDecline(
            "sse2-path", lift::FtolWhy::Sse2Path, [&](lift::Machine& m) { m.mem().wr32(kFlagEa, 1u); }, true);
        // ③ `st(0)` 空（tag 全 empty）
        checkDecline(
            "stack-empty", lift::FtolWhy::StackEmpty, [](lift::Machine& m) { m.cpu().fpu.tag = 0xFFFFu; }, true);
        // ④ 空闲槽 < 2（8 个槽全 valid）
        checkDecline(
            "stack-full", lift::FtolWhy::StackFull,
            [](lift::Machine& m) {
                for (int i = 0; i < 8; ++i) m.cpu().fpu.setValid(i);
            },
            true);
        // ⑤ 栈 scratch 未提交（esp 落在一个只提交了 4 B 的页首 ⇒ `[E,E+20h)` 跨进未提交页）
        checkDecline(
            "scratch-uncommitted", lift::FtolWhy::ScratchUnreadable,
            [](lift::Machine& m) {
                const std::uint32_t page = 0x00B00000u;
                std::uint32_t zero = 0;
                std::string e;
                if (!m.placeData(page, &zero, 4u, &e)) return;
                m.cpu().esp() = page;  // E = align16(page − 0x24) = page − 0x30 ⇒ 未提交
            },
            true);
        // ⑥ `x ∈ (2^63−1, 2^63)`：唯一可表示点是 `2^63−0.5`（sig = 2^64−1、E = 62）
        //   ⇒ guest 的 `fistp`（就近）给不定值，trunc 给 INT64_MAX ⇒ 两条语义在这一格分叉，必须交解释器
        checkDecline(
            "boundary-2^63-minus-half", lift::FtolWhy::TruncRoundDiverge,
            [](lift::Machine& m) {
                std::uint8_t t80[10] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x3D, 0x40};
                std::memcpy(m.cpu().fpu.regs[m.cpu().fpu.phys(0)], t80, 10);
            },
            true);
        // ⑦ 同一条边界但 `x = 2^63−1`（可表示、**不越界**）⇒ **不许**回退（否则 G-5 就宽到把正常值也挡了）
        {
            lift::Machine m;
            std::string err;
            const bool rig = bootFtol(m, err, true);
            LIFT_CHECK(rig);
            if (rig) {
                std::uint8_t t80[10] = {0xFE, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x3D, 0x40};
                std::memcpy(m.cpu().fpu.regs[m.cpu().fpu.phys(0)], t80, 10);
                m.cpu().fpu.setValid(m.cpu().fpu.phys(0));
                m.cpu().esp() = m.layout().stack_top - 0x40u;
                lift::x87ApplyControlWord(0x037Fu);
                lift::x87ClearExceptionFlags();
                std::uint32_t r = 0;
                lift_test::g_current_case = "ftol/boundary-2^63-minus-1";
                LIFT_CHECK(lift::nativeFtol2Sse(m.cpu(), m.mem(), &r));
                LIFT_CHECK_EQ_U32(r, 0xFFFFFFFFu);
                LIFT_CHECK_EQ_U32(m.cpu().edx(), 0x7FFFFFFFu);
            }
        }

        // ② ABI：`invoke()` 成功路径的 EIP / ESP / EAX / **EDX**
        {
            lift_test::g_current_case = "ftol/abi";
            lift::Machine m;
            std::string err;
            const bool rig = bootFtol(m, err, true);
            LIFT_CHECK(rig);
            if (rig) {
                lift::Ext80 v;
                extFromF64(-2.7, v);
                std::memcpy(m.cpu().fpu.regs[m.cpu().fpu.phys(0)], v, 16);
                m.cpu().fpu.setValid(m.cpu().fpu.phys(0));
                const std::uint32_t esp = m.layout().stack_top - 0x40u;
                m.cpu().esp() = esp;
                const std::uint32_t ret_ea = 0x00401000u;
                m.mem().wr32(esp, ret_ea);
                lift::x87ApplyControlWord(0x037Fu);
                lift::x87ClearExceptionFlags();
                lift::NativeOverride* ov = m.overrides().at(0);
                LIFT_CHECK(ov != nullptr);
                if (ov != nullptr) {
                    const char* why = "";
                    LIFT_CHECK(m.overrides().invoke(*ov, m.cpu(), m.mem(), &why));
                    LIFT_CHECK_EQ_U32(m.cpu().eax(), 0xFFFFFFFEu);  // -2
                    LIFT_CHECK_EQ_U32(m.cpu().edx(), 0xFFFFFFFFu);  // 高 32 位 = 符号扩展
                    LIFT_CHECK_EQ_U32(m.cpu().eip, ret_ea);
                    LIFT_CHECK_EQ_U32(m.cpu().esp(), esp + 4u);     // cdecl：只弹返回地址
                    LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(ov->hits), 1u);
                    LIFT_CHECK_EQ_U32(m.cpu().fpu.top, 1u);
                    LIFT_CHECK(m.cpu().fpu.empty(0));               // 被消费的物理槽
                }
            }
        }

        // ③ 栈平衡：6 个活值各被消费 1 层（TOP 逐次 +1、槽逐次 empty、返回值逐次正确）
        {
            lift_test::g_current_case = "ftol/stack-balance";
            lift::Machine m;
            std::string err;
            const bool rig = bootFtol(m, err, true);
            LIFT_CHECK(rig);
            if (rig) {
                // 槽 0..5 = st(0)..st(5)（TOP = 0 ⇒ 前 6 次调用的 st(0) 都落在同一批槽上）
                const double vals[6] = {2.7, -2.7, 1.5, -1.5, 0.5, -0.5};
                const std::uint32_t want[6] = {2u, 0xFFFFFFFEu, 1u, 0xFFFFFFFFu, 0u, 0u};
                for (int i = 0; i < 6; ++i) {
                    lift::Ext80 v;
                    extFromF64(vals[i], v);
                    std::memcpy(m.cpu().fpu.regs[i], v, 16);
                    m.cpu().fpu.setValid(i);
                }
                m.cpu().esp() = m.layout().stack_top - 0x40u;
                for (int k = 0; k < 6; ++k) {
                    lift::x87ApplyControlWord(0x037Fu);
                    lift::x87ClearExceptionFlags();
                    std::uint32_t r = 0;
                    LIFT_CHECK(lift::nativeFtol2Sse(m.cpu(), m.mem(), &r));
                    LIFT_CHECK_EQ_U32(r, want[k]);
                    LIFT_CHECK_EQ_U32(m.cpu().fpu.top, static_cast<std::uint32_t>(k + 1));
                    LIFT_CHECK(m.cpu().fpu.empty(k));
                }
                LIFT_CHECK_EQ_U32(m.cpu().fpu.tag, 0xFFFFu);  // 6 层全消费完 ⇒ 8 槽皆空
            }
        }
    }

    // ---- sub_4E0B40（libvorbis `mdct_backward`，**数值容忍档**）：准入零写 + 纯核不变量 ----
    //
    // 判据（红了意味着什么）：
    //   ① **准入零写**：`trig` 指向未提交页 ⇒ 必须在任何写之前回退，`out` 一字节不变，
    //      且原因落进 `declines[MdctWhy::TrigUnreadable]`（⛔ 不是"静默跳过"）；
    //   ② 纯核 **in == out 与 in != out 逐字节相同**（真实调用点 0x4E2EAA 就是原地）；
    //   ③ 纯核不越界：`out` 前后的哨兵不被写、`in` / `trig` / `bitrev` 不被改；
    //   ④ 成功路径的 ABI：`invoke` 后 EIP = 返回地址、ESP = esp + 4 + 12（cdecl 只弹返回地址）。
    // ★ 数学正确性**不在**这里判（本档没有 git 里的 oracle）：那是 native_override_assets 的
    //   `mdct/*` 两臂差分（真 guest 字节当 oracle，容差 `1e-6 * peak`）。
    {
        constexpr std::uint32_t kInitEa = 0x00420000u;
        constexpr std::uint32_t kTrigEa = 0x00430000u;
        constexpr std::uint32_t kInEa = 0x00470000u;
        constexpr std::uint32_t kOutEa = 0x00490000u;
        constexpr std::uint32_t kBadEa = 0x00B00000u;       // 从不 placeData ⇒ 未提交（init 用）
        constexpr std::uint32_t kBadPartial = 0x00B00010u;  // 同上（表驱动里"这一段不铺"）
        constexpr std::uint32_t kBitrevEa = 0x00450000u;
        constexpr int kN = 256;
        constexpr int kLog2N = 8;
        const int n2 = kN / 2;
        const int n4 = kN / 4;

        // 表 = `lift_fixture::mdctFixtureTables`（任意但有界的确定性数据；只测不变量，不测数值）
        std::vector<float> trig;
        std::vector<std::int32_t> bitrev;
        lift_fixture::mdctFixtureTables(kN, &trig, &bitrev);
        std::vector<float> src(static_cast<std::size_t>(n2));
        for (int i = 0; i < n2; ++i) src[static_cast<std::size_t>(i)] = static_cast<float>(0.125 * (i % 17) - 1.0);

        // ② / ③ 纯核：`out` 前后各留 4 个哨兵。
        {
            lift_test::g_current_case = "mdct/host-invariants";
            constexpr int kGuard = 4;
            std::vector<float> out_a(static_cast<std::size_t>(kN) + 2 * kGuard, 777.0f);
            std::vector<float> out_b = out_a;
            const std::vector<float> src_before = src;
            float* pa = out_a.data() + kGuard;
            float* pb = out_b.data() + kGuard;
            lift::mdctBackwardHost(kN, kLog2N, trig.data(), bitrev.data(), src.data(), pa);
            lift::mdctBackwardHost(kN, kLog2N, trig.data(), bitrev.data(), src.data(), pb);  // in != out
            LIFT_CHECK(std::memcmp(pa, pb, static_cast<std::size_t>(kN) * 4u) == 0);
            // ② **真正的原地**：同一缓冲既当 `in` 又当 `out`（真实调用点 0x4E2EAA 就是这个形状）。
            //   ⛔ 上面那两次都不是原地（`in`/`out` 不同缓冲）—— 只比它俩等于没测 alias。
            {
                std::vector<float> buf(static_cast<std::size_t>(kN) + 2 * kGuard, 555.0f);
                std::memcpy(buf.data() + kGuard, src.data(), static_cast<std::size_t>(n2) * 4u);
                float* same = buf.data() + kGuard;
                lift::mdctBackwardHost(kN, kLog2N, trig.data(), bitrev.data(), same, same);
                LIFT_CHECK(std::memcmp(same, pb, static_cast<std::size_t>(kN) * 4u) == 0);
            }
            bool guards_ok = true;
            for (int i = 0; i < kGuard; ++i) {
                guards_ok = guards_ok && out_a[static_cast<std::size_t>(i)] == 777.0f;
                guards_ok = guards_ok && out_a[static_cast<std::size_t>(kN + kGuard + i)] == 777.0f;
            }
            LIFT_CHECK(guards_ok);                            // ③ 不越界
            LIFT_CHECK(src == src_before);                    // ③ 不改输入
            // 纯核不依赖 `out` 的旧内容（⇒ `in == out` 的原地调用安全，且输出可复现）：
            // 这次把 `out` 预填成**另一个**哨兵值，结果必须逐字节相同。
            std::vector<float> out_c(static_cast<std::size_t>(kN) + 2 * kGuard, -999.0f);
            lift::mdctBackwardHost(kN, kLog2N, trig.data(), bitrev.data(), src.data(), out_c.data() + kGuard);
            LIFT_CHECK(std::memcmp(out_c.data() + kGuard, pb, static_cast<std::size_t>(kN) * 4u) == 0);
        }

        // ④ ABI + ① 回退：同一套 guest 夹具，只把 `trig` 换成未提交页。
        auto armMdct = [&](lift::Machine& m, int n_arg = kN) {
            auto& cpu = m.cpu();
            auto& mem = m.mem();
            const std::uint32_t esp = m.layout().stack_top - 0x40u;
            cpu.esp() = esp;
            mem.wr32(esp, lift_fixture::kSentinel);
            (void)n_arg;
            mem.wr32(esp + 4u, kInitEa);   // init
            mem.wr32(esp + 8u, kInEa);     // in
            mem.wr32(esp + 12u, kOutEa);   // out
            return esp;
        };
        // 表驱动用：按给定 EA 铺区段（不铺的那一段就是"未提交页"，用来触发对应回退原因）。
        auto bootMdctVariant = [&](lift::Machine& m, int n, int log2n, std::uint32_t trig_ea, std::uint32_t in_ea,
                                  std::uint32_t out_ea, std::uint32_t bitrev_ea, bool init_unreadable) {
            std::string err;
            if (!m.initBare(&err) || !m.prepareStack(&err)) return false;
            const int n2v = n / 2;
            std::vector<float> tv;
            std::vector<std::int32_t> bv;
            lift_fixture::mdctFixtureTables(n, &tv, &bv);
            std::vector<std::uint8_t> init(16u, 0), in_b(static_cast<std::size_t>(n2v) * 4u);
            std::memcpy(init.data() + 0, &n, 4);
            std::memcpy(init.data() + 4, &log2n, 4);
            std::memcpy(init.data() + 8, &trig_ea, 4);
            std::memcpy(init.data() + 12, &bitrev_ea, 4);
            std::memcpy(in_b.data(), src.data(), in_b.size());
            std::vector<std::uint8_t> tv_b(tv.size() * 4u), bv_b(bv.size() * 4u);
            std::memcpy(tv_b.data(), tv.data(), tv_b.size());
            std::memcpy(bv_b.data(), bv.data(), bv_b.size());
            std::vector<std::uint8_t> out_b(static_cast<std::size_t>(n) * 4u, 0x5A);
            if (!init_unreadable && !m.placeData(kInitEa, init.data(), init.size(), &err)) return false;
            if (!m.placeData(in_ea, in_b.data(), in_b.size(), &err)) return false;
            if (!m.placeData(out_ea, out_b.data(), out_b.size(), &err)) return false;
            if (trig_ea != kBadPartial && !m.placeData(trig_ea, tv_b.data(), tv_b.size(), &err)) return false;
            if (bitrev_ea != kBadPartial && !m.placeData(bitrev_ea, bv_b.data(), bv_b.size(), &err)) return false;
            return true;
        };
        auto bootMdct = [&](lift::Machine& m, std::uint32_t trig_ea, int bad_bitrev = 0) {
            std::string err;
            if (!m.initBare(&err) || !m.prepareStack(&err)) return false;
            auto& mem = m.mem();
            std::vector<std::uint8_t> init(16u, 0), in_b(static_cast<std::size_t>(n2) * 4u);
            std::memcpy(init.data() + 0, &kN, 4);
            std::memcpy(init.data() + 4, &kLog2N, 4);
            std::memcpy(init.data() + 8, &trig_ea, 4);
            std::memcpy(init.data() + 12, &kBitrevEa, 4);
            std::memcpy(in_b.data(), src.data(), in_b.size());
            std::vector<std::uint8_t> trig_b(trig.size() * 4u), bitrev_b(bitrev.size() * 4u);
            std::memcpy(trig_b.data(), trig.data(), trig_b.size());
            std::memcpy(bitrev_b.data(), bitrev.data(), bitrev_b.size());
            if (bad_bitrev != 0) {
                // 坏表：`bitrev[0]` 越界。准入取**真表的范围** `[0, n/2-2]` ⇒ 下界用例用 `-1`
                //   （负值会让 `mdct_bitreverse` 读 `out[n/2-1]`，即**还没写过**的半段），
                //   上界用例用 `n/2-1`（`x0[1]` 落到 `out+n`）。
                // 宿主裸指针没有页表兜底 ⇒ 必须在**运行纯核之前**被值域判据挡掉（MdctWhy::BitrevOutOfRange）。
                std::memcpy(bitrev_b.data(), &bad_bitrev, 4u);
            }
            std::vector<std::uint8_t> out_b(static_cast<std::size_t>(kN) * 4u, 0x5A);
            if (!m.placeData(kInitEa, init.data(), init.size(), &err)) return false;
            if (!m.placeData(kInEa, in_b.data(), in_b.size(), &err)) return false;
            if (!m.placeData(kOutEa, out_b.data(), out_b.size(), &err)) return false;
            if (trig_ea != kBadEa) {
                if (!m.placeData(kTrigEa, trig_b.data(), trig_b.size(), &err)) return false;
                if (!m.placeData(kBitrevEa, bitrev_b.data(), bitrev_b.size(), &err)) return false;
            }
            return true;
        };
        auto outBytes = [&](lift::Machine& m) {
            std::vector<std::uint8_t> v(static_cast<std::size_t>(kN) * 4u);
            std::memcpy(v.data(), m.mem().host(kOutEa), v.size());
            return v;
        };

        // ④ 成功路径：所有区段都提交 ⇒ 接管一次，ABI 对得上
        {
            lift_test::g_current_case = "mdct/abi";
            lift::Machine m;
            LIFT_CHECK(bootMdct(m, kTrigEa));
            const std::uint32_t esp = armMdct(m);
            std::string err;
            LIFT_CHECK(m.overrides().add(0x004E0B40u, "ref_mdct", lift::GuestConv::Cdecl, 12,
                                        lift::nativeMdct4E0B40, &err));
            lift::NativeOverride* e = m.overrides().at(0);
            const char* why = "";
            LIFT_CHECK(e != nullptr && m.overrides().invoke(*e, m.cpu(), m.mem(), &why));
            LIFT_CHECK_EQ_U32(e->hits, 1u);
            LIFT_CHECK_EQ_U32(e->fallbacks, 0u);
            LIFT_CHECK_EQ_U32(m.cpu().eip, lift_fixture::kSentinel);  // eip = 返回地址
            LIFT_CHECK_EQ_U32(m.cpu().esp(), esp + 4u);               // cdecl：只弹返回地址（调用方清栈）
            LIFT_CHECK_EQ_U32(m.cpu().eax(), 0u);                     // `void` 函数 ⇒ 登记为 0
            // 真的写了 out（排除"空跑"）
            const std::vector<std::uint8_t> got = outBytes(m);
            bool changed = false;
            for (std::uint8_t b : got) changed = changed || b != 0x5A;
            LIFT_CHECK(changed);
        }
        // ① 回退零写：trig 未提交 ⇒ 回退、out 一字节不变、原因落桶
        {
            lift_test::g_current_case = "mdct/decline-trig-unreadable";
            lift::Machine m;
            LIFT_CHECK(bootMdct(m, kBadEa));
            const std::uint32_t esp = armMdct(m);
            std::string err;
            LIFT_CHECK(m.overrides().add(0x004E0B40u, "ref_mdct", lift::GuestConv::Cdecl, 12,
                                        lift::nativeMdct4E0B40, &err));
            lift::NativeOverride* e = m.overrides().at(0);
            const std::vector<std::uint8_t> frozen = outBytes(m);
            const char* why = "";
            LIFT_CHECK(!m.overrides().invoke(*e, m.cpu(), m.mem(), &why));
            LIFT_CHECK_EQ_U32(e->hits, 0u);
            LIFT_CHECK_EQ_U32(e->fallbacks, 1u);
            LIFT_CHECK_EQ_U32(e->declines[static_cast<std::size_t>(lift::MdctWhy::TrigUnreadable)], 1u);
            LIFT_CHECK_EQ_U32(m.cpu().eip, 0u);          // 回退：EIP 没被改（dispatcher 自己会解释原字节）
            LIFT_CHECK_EQ_U32(m.cpu().esp(), esp);       // 回退：栈没动
            LIFT_CHECK(outBytes(m) == frozen);           // ① 零写
        }
        // ①' `bitrev` 值越界 ⇒ 同样必须在写之前回退（⛔ 否则 `x0 = x + bit[k]` 是宿主野指针：
        //    不是 guest fault，没有页表兜底，会读/写任意宿主内存）
        {
            lift_test::g_current_case = "mdct/decline-bitrev-low";
            lift::Machine m;
            LIFT_CHECK(bootMdct(m, kTrigEa, /*bad_bitrev=*/-1));
            const std::uint32_t esp = armMdct(m);
            std::string err;
            LIFT_CHECK(m.overrides().add(0x004E0B40u, "ref_mdct", lift::GuestConv::Cdecl, 12,
                                        lift::nativeMdct4E0B40, &err));
            lift::NativeOverride* e = m.overrides().at(0);
            const std::vector<std::uint8_t> frozen = outBytes(m);
            const char* why = "";
            LIFT_CHECK(!m.overrides().invoke(*e, m.cpu(), m.mem(), &why));
            LIFT_CHECK_EQ_U32(e->hits, 0u);
            LIFT_CHECK_EQ_U32(e->fallbacks, 1u);
            LIFT_CHECK_EQ_U32(e->declines[static_cast<std::size_t>(lift::MdctWhy::BitrevOutOfRange)], 1u);
            LIFT_CHECK_EQ_U32(m.cpu().esp(), esp);
            LIFT_CHECK(outBytes(m) == frozen);
        }
        // ①2 其余 6 个原因逐个触发一次（表驱动：只铺"该铺的"区段，其余指向未提交页）
        //   ⇒ 保证 9 个桶每一个都有一次真实触发（否则"分桶"只是扇没被敲过的门）。
        {
            struct DeclineCase {
                const char* name;
                int n;
                int log2n;
                std::uint32_t trig_ea;
                std::uint32_t in_ea;
                std::uint32_t out_ea;
                std::uint32_t bitrev_ea;
                lift::MdctWhy why;
            };
            const DeclineCase cases[] = {
                {"init-unreadable", kN, kLog2N, kTrigEa, kInEa, kOutEa, kBitrevEa, lift::MdctWhy::InitUnreadable},
                {"size-out-of-range", 48, 6, kTrigEa, kInEa, kOutEa, kBitrevEa, lift::MdctWhy::SizeOutOfRange},
                {"not-power-of-two", 200, 8, kTrigEa, kInEa, kOutEa, kBitrevEa, lift::MdctWhy::NotPowerOfTwo},
                {"log2-mismatch", kN, 7, kTrigEa, kInEa, kOutEa, kBitrevEa, lift::MdctWhy::Log2Mismatch},
                {"in-unreadable", kN, kLog2N, kTrigEa, kBadPartial, kOutEa, kBitrevEa, lift::MdctWhy::InUnreadable},
                {"out-unreadable", kN, kLog2N, kTrigEa, kInEa, kBadPartial, kBitrevEa, lift::MdctWhy::OutUnreadable},
                {"trig-unreadable", kN, kLog2N, kBadPartial, kInEa, kOutEa, kBitrevEa, lift::MdctWhy::TrigUnreadable},
                {"bitrev-unreadable", kN, kLog2N, kTrigEa, kInEa, kOutEa, kBadPartial, lift::MdctWhy::BitrevUnreadable},
            };
            for (const DeclineCase& c : cases) {
                lift_test::g_current_case = (std::string("mdct/decline-") + c.name).c_str();
                lift::Machine m;
                LIFT_CHECK(bootMdctVariant(m, c.n, c.log2n, c.trig_ea, c.in_ea, c.out_ea, c.bitrev_ea,
                                          c.why == lift::MdctWhy::InitUnreadable));
                const std::uint32_t esp = armMdct(m, c.n);
                std::string err;
                LIFT_CHECK(m.overrides().add(0x004E0B40u, "ref_mdct", lift::GuestConv::Cdecl, 12,
                                            lift::nativeMdct4E0B40, &err));
                lift::NativeOverride* e = m.overrides().at(0);
                const char* why = "";
                LIFT_CHECK(!m.overrides().invoke(*e, m.cpu(), m.mem(), &why));
                LIFT_CHECK_EQ_U32(e->hits, 0u);
                LIFT_CHECK_EQ_U32(e->fallbacks, 1u);
                LIFT_CHECK_EQ_U32(e->declines[static_cast<std::size_t>(c.why)], 1u);
                LIFT_CHECK_EQ_U32(m.cpu().esp(), esp);
            }
        }
        // ①'' 上界：`bitrev[0] = n/2-1`（真表最大值 `n/2-2` 的下一格）也必须回退
        {
            lift_test::g_current_case = "mdct/decline-bitrev-high";
            lift::Machine m;
            LIFT_CHECK(bootMdct(m, kTrigEa, /*bad_bitrev=*/n2 - 1));
            const std::uint32_t esp = armMdct(m);
            std::string err;
            LIFT_CHECK(m.overrides().add(0x004E0B40u, "ref_mdct", lift::GuestConv::Cdecl, 12,
                                        lift::nativeMdct4E0B40, &err));
            lift::NativeOverride* e = m.overrides().at(0);
            const char* why = "";
            LIFT_CHECK(!m.overrides().invoke(*e, m.cpu(), m.mem(), &why));
            LIFT_CHECK_EQ_U32(e->declines[static_cast<std::size_t>(lift::MdctWhy::BitrevOutOfRange)], 1u);
            LIFT_CHECK_EQ_U32(m.cpu().esp(), esp);
        }
    }

    // ---- ⑱ known 表：新项的默认分层（全部 debug=false ⇒ 都在 all 里）+ kMax 容得下 ----
    {
        lift_test::g_current_case = "known-table";  // 失败归因（下面几条都是"这一项在不在 all 里"）
        LIFT_CHECK(lift::findKnownNative("sub_4E0B40") != nullptr);
        LIFT_CHECK(!lift::findKnownNative("sub_4E0B40")->debug);
        LIFT_CHECK(lift::findKnownNative("sub_429020") != nullptr);
        LIFT_CHECK(lift::findKnownNative("sub_48AE90") != nullptr);
        LIFT_CHECK(!lift::findKnownNative("sub_429020")->debug);
        LIFT_CHECK(!lift::findKnownNative("sub_48AE90")->debug);
        // ★ sub_46D9F0：thiscall `retn 20h` = 8 个栈参数 = 32 B；debug 层不进（`all` 必须带上它）。
        const lift::KnownNative* glyph = lift::findKnownNative("sub_46D9F0");
        LIFT_CHECK(glyph != nullptr);
        if (glyph != nullptr) {
            LIFT_CHECK(!glyph->debug);
            LIFT_CHECK(glyph->conv == lift::GuestConv::Thiscall);
            LIFT_CHECK_EQ_U32(glyph->arg_bytes, 32u);
        }
        const lift::KnownBlockNative* pool = lift::findKnownBlockNative("sub_414AC0");
        LIFT_CHECK(pool != nullptr && !pool->debug);
        LIFT_CHECK_EQ_U32(pool->entry_ea, 0x00415384u);
        LIFT_CHECK_EQ_U32(pool->resume_ea, 0x0041539Fu);
        // ★ sub_4CCC60：stdcall `retn 1Ch` = 28 B；**稳定层**（2026-11-11 进 `all`）。
        const lift::KnownNative* affine = lift::findKnownNative("sub_4CCC60");
        LIFT_CHECK(affine != nullptr);
        if (affine != nullptr) {
            LIFT_CHECK(!affine->debug);
            LIFT_CHECK(affine->conv == lift::GuestConv::Stdcall);
            LIFT_CHECK_EQ_U32(affine->arg_bytes, 28u);
        }
        // ★ `__ftol2_sse`：cdecl、**0 个栈参**（两条 x87 路径都是裸 `retn`）、**稳定层**。
        //   ★ 这条就是"它在 `all` 里"的判据：`all` 的定义是"每个 `debug == false` 的已知项都 add 一遍"
        //     （`machine.cpp` 的 configureNativeOverrides）⇒ `debug == false` + 名字在 `kKnown[]` 里
        //     等价于"`--native-overrides all` 会启用它"。
        //   ★ 隔离臂（`all,-__ftol2_sse` 回到解释器）在 `tests/loader.cpp` 的
        //     `native-override-name-resolution`（assets 档，有 `.lst`）：那里断言 `all` 装进
        //     `0x4E82C0`、而 `all,-__ftol2_sse` 把它排掉。这里只能断言"名字表里那一项" ——
        //     ⛔ 名字 → EA 的解析要 `.lst` 索引，pure 档（bare 机器）够不着。
        //     ★ 为什么要单列这条：`all` 里"`.lst` 找不到函数入口"是**静默跳过**的
        //     ⇒ 挂错名字（如 `__ftol2`）的表现是 hits 恒 0 而差分类测试全绿。
        const lift::KnownNative* ftol = lift::findKnownNative("__ftol2_sse");
        LIFT_CHECK(ftol != nullptr);
        if (ftol != nullptr) {
            LIFT_CHECK(!ftol->debug);
            LIFT_CHECK(ftol->conv == lift::GuestConv::Cdecl);
            LIFT_CHECK_EQ_U32(ftol->arg_bytes, 0u);
            LIFT_CHECK(ftol->fn == &lift::nativeFtol2Sse);
        }
        // ★ 表容量：`all` 会把每个已知项都 add() 一遍 ⇒ kMax 必须装得下**所有**已知项，
        //   否则 configureNativeOverrides 会硬失败（模拟器拒绝启动）。⛔ 不写死数字。
        LIFT_CHECK(lift::NativeOverrideTable::kMax >= lift::knownNativeCount());
        LIFT_CHECK(lift::NativeBlockTable::kMax >= lift::knownBlockNativeCount());
        // ★ sub_4367B0：thiscall `retn 8` = 8 B；**稳定层**（2026-11-11 进 `all`）。
        const lift::KnownNative* lzss = lift::findKnownNative("sub_4367B0");
        LIFT_CHECK(lzss != nullptr);
        if (lzss != nullptr) {
            LIFT_CHECK(!lzss->debug);
            LIFT_CHECK(lzss->conv == lift::GuestConv::Thiscall);
            LIFT_CHECK_EQ_U32(lzss->arg_bytes, 8u);
            LIFT_CHECK(lzss->fn == &lift::nativeLzss4367B0);
        }
    }

    // ---- ⑲ sub_4367B0（退出写存档的 LZSS Encode）的纯核 ----
    // 判据（红了意味着什么；缺一条都不算）：
    //   ① **树不变量**：`dad/lson/rson` 互反、无环、根索引域（`dad[r]` 永不为 NIL 假根 4096）；
    //   ② **平局定点**：`InsertNode` 的 `i > match_len` 放宽成 `>=` ⇒ 固定的 7 字节输出当场变；
    //   ③ **独立转写对拍**：同一输入下生产核与 `naiveLzssEncode` 的 `consumed/comp/gave_up/out` 全同；
    //   ④ `lzssEncodeObject` ↔ `lzssDecode` **往返**（含 give_up 的 `comp == in_size` 直通；
    //      0 / 1 / 17 / 18 / 4078 / 4096 / 4097 / 150068 档 × 5 种数据形态）；
    //   ⑤ **准入面**：`nativeLzss4367B0` 每条回退路径都**零写**（对象 0xD430 + info 12 B 快照 memcmp），
    //      且 declines 桶点名；happy path 的出参与独立算出的核结果逐字节相同。
    // ★ 修前红：`lift::lzssEncodeObject` / `lift::nativeLzss4367B0` 不存在 ⇒ **编译期红**（新符号）。
    {
        constexpr std::uint32_t kObjBytes = 0xD430u;
        constexpr std::uint32_t kDad = 0x1018u;
        constexpr std::uint32_t kLson = 0x501Cu;
        constexpr std::uint32_t kRson = 0x9020u;
        constexpr std::uint32_t kOutField = 0xD42Cu;
        constexpr std::uint32_t kNil = 4096u;
        auto makeObj = [] { return std::vector<std::uint8_t>(kObjBytes, 0xCC); };
        auto getU32 = [](const std::vector<std::uint8_t>& o, std::uint32_t off) {
            std::uint32_t v = 0;
            std::memcpy(&v, o.data() + off, 4);
            return v;
        };
        // 5 种数据形态：全 0 / 全同 / 有 LZSS 结构 / 伪随机（可压） / xorshift（不可压 ⇒ give_up）
        auto mkInput = [](std::uint32_t n, std::uint32_t kind) {
            std::vector<std::uint8_t> v(n, 0);
            std::uint32_t x = 0x12345678u + kind;
            for (std::uint32_t i = 0; i < n; ++i) {
                switch (kind) {
                    case 0: v[i] = 0; break;
                    case 1: v[i] = 0x41; break;
                    case 2: v[i] = static_cast<std::uint8_t>("ABRACADABRA"[i % 11u]); break;
                    case 3: v[i] = static_cast<std::uint8_t>((i * 37u + 11u) % 251u); break;
                    default:
                        x ^= x << 13;
                        x ^= x >> 17;
                        x ^= x << 5;
                        v[i] = static_cast<std::uint8_t>(x & 0xFFu);
                        break;
                }
            }
            return v;
        };
        auto runKernel = [&](const std::vector<std::uint8_t>& in, std::uint32_t in_ea, std::vector<std::uint8_t>* obj,
                             std::vector<std::uint8_t>* out) {
            out->assign(in.size() + 0x1000u, 0x5A);
            return lift::lzssEncodeObject(obj->data(), in_ea, in.empty() ? nullptr : in.data(),
                                         static_cast<std::uint32_t>(in.size()), out->data());
        };
        // ①③④ 的树检查：对**结果对象**直接查 dad/lson/rson。
        auto checkTree = [&](const std::vector<std::uint8_t>& obj, const std::vector<std::uint8_t>& input,
                             std::uint32_t consumed, bool gave_up) {
            // ① `dad[r]` ∈ {NIL} ∪ [0,4095] ∪ [4097,4352]；**非 NIL 的父索引永不为 4096**
            //   （根不会挂在 NIL 下 —— 4096 只是"没有父"的哨兵，不是任何节点的编号）。
            for (std::uint32_t r = 0; r < 4096u; ++r) {
                const std::uint32_t d = getU32(obj, kDad + 4u * r);
                LIFT_CHECK(d == kNil || d < 4096u || (d >= 4097u && d <= 4352u));
            }
            // ② 互反：树里每个节点的孩子都指回它（已删除节点 `dad==NIL`，其 lson/rson 是残值 ⇒ 跳过）
            for (std::uint32_t p = 0; p < 4096u; ++p) {
                if (getU32(obj, kDad + 4u * p) == kNil) continue;
                const std::uint32_t l = getU32(obj, kLson + 4u * p);
                const std::uint32_t rs = getU32(obj, kRson + 4u * p);
                if (l != kNil) LIFT_CHECK(l < 4096u && getU32(obj, kDad + 4u * l) == p);
                if (rs != kNil) LIFT_CHECK(rs < 4096u && getU32(obj, kDad + 4u * rs) == p);
            }
            // ②b 根的孩子只在 `rson[]` 里（`lson[4097..4352]` 不在这份对象的数组域内）
            for (std::uint32_t root = 4097u; root <= 4352u; ++root) {
                const std::uint32_t rs = getU32(obj, kRson + 4u * root);
                if (rs != kNil) LIFT_CHECK(rs < 4096u && getU32(obj, kDad + 4u * rs) == root);
            }
            // ③ 无环：从所有根出发，每个节点最多被访问一次、步数 ≤ 节点总数
            {
                std::vector<std::uint8_t> seen(4096u, 0);
                std::vector<std::uint32_t> stack;
                for (std::uint32_t root = 4097u; root <= 4352u; ++root) {
                    const std::uint32_t rs = getU32(obj, kRson + 4u * root);
                    if (rs != kNil) stack.push_back(rs);
                }
                std::uint32_t steps = 0;
                bool bad = false;
                while (!stack.empty() && !bad) {
                    const std::uint32_t p = stack.back();
                    stack.pop_back();
                    ++steps;
                    if (p >= 4096u || seen[p] != 0u || steps > 4096u) {
                        bad = true;
                        break;
                    }
                    seen[p] = 1;
                    const std::uint32_t l = getU32(obj, kLson + 4u * p);
                    const std::uint32_t rs = getU32(obj, kRson + 4u * p);
                    if (l != kNil) stack.push_back(l);
                    if (rs != kNil) stack.push_back(rs);
                }
                LIFT_CHECK(!bad);
            }
            // ④ `text_buf` 与输入逐字节对得上。★ 两段的**下标不同**（这不是笔误）：
            //   首灌把 `input[0..len0)` 写进 `text_buf[4078..]`；拷回阶段从 **s = 0** 起把
            //   `input[len0 + s]` 写进 `text_buf[s]`。★ 只在**没放弃压缩**时查：give_up 会把 `a3[1]`
            //   改成输入长度，而 ring 只装了"放弃前"那一段 ⇒ 用 consumed 查会冤枉它（`a3[1]` 的双语义）。
            if (!gave_up) {
                const std::uint32_t len0 =
                    input.size() < 18u ? static_cast<std::uint32_t>(input.size()) : 18u;
                LIFT_CHECK(consumed >= len0);
                const std::uint32_t copied = consumed - len0;
                // 首灌段（s 还没推进到 4078 之前才没被覆盖）
                if (copied <= 4078u) {
                    for (std::uint32_t i = 0; i < len0; ++i) LIFT_CHECK(obj[0x004u + 4078u + i] == input[i]);
                }
                // 拷贝段：`text_buf[j & 0xFFF] == input[len0 + j]`
                const std::uint32_t lo = copied > 4096u ? copied - 4096u : 0u;
                for (std::uint32_t j = lo; j < copied && (len0 + j) < input.size(); ++j) {
                    LIFT_CHECK(obj[0x004u + (j & 0xFFFu)] == input[len0 + j]);
                }
            }
        };

        // ---- ① 树不变量（多个输入档；含 give_up 与空输入）----
        lift_test::g_current_case = "lzss/tree-invariants";
        for (std::uint32_t n : {0u, 1u, 17u, 18u, 19u, 64u, 300u, 4096u, 4097u, 9000u}) {
            for (std::uint32_t kind = 0; kind < 5u; ++kind) {
                const std::vector<std::uint8_t> in = mkInput(n, kind);
                std::vector<std::uint8_t> obj = makeObj();
                std::vector<std::uint8_t> out;
                const lift::LzssEncodeOutcome r = runKernel(in, 0x00400000u, &obj, &out);
                LIFT_CHECK_EQ_U32(r.consumed, n);  // 非回绕：每条返回路径 a3[1] == a3[0]
                checkTree(obj, in, r.consumed, r.gave_up);
                if (n == 0u) {
                    // ★ `len == 0` 的早退（0x43687E `jz 0x436A59`）只做 InitTree + text_buf 清零：
                    //   `match_pos` / `match_len` **保持预填值**（guest 一个字节都没往那儿写）。
                    //   这条能抓"InitTree 多清一项"这类越界（把 rson[4353] 也写掉 = 踩 match_pos）。
                    LIFT_CHECK_EQ_U32(getU32(obj, 0xD424u), 0xCCCCCCCCu);
                    LIFT_CHECK_EQ_U32(getU32(obj, 0xD428u), 0xCCCCCCCCu);
                    LIFT_CHECK_EQ_U32(getU32(obj, 0xD42Cu), 0xCCCCCCCCu);  // out 由包装写，纯核不碰
                }
            }
        }

        // ---- ② 平局定点：`i > match_len` 放宽成 `>=` ⇒ 末尾两个字节从 `EE F0` 变 `F0 F0` ----
        // 输入 "BABABBAB"（对象预填 0xCC ⇒ `text_buf[4086..4112]` 的尾巴参与比较）：两个候选
        // （4078 与 4080）对最后一段的匹配**等长 3** ⇒ 严格 `>` 保留**先入树**的 4078（offset 0xFEE），
        // `>=` 会改成 4080（offset 0xFF0）。输出字节就是判据（实测：把生产核那一行改成 `>=` 后本用例红）。
        lift_test::g_current_case = "lzss/tie-strict-greater";
        {
            const std::uint8_t tie_in[] = {0x42, 0x41, 0x42, 0x41, 0x42, 0x42, 0x41, 0x42};
            const std::uint8_t tie_expected[] = {0x03, 0x42, 0x41, 0xEE, 0xF0, 0xEE, 0xF0};
            std::vector<std::uint8_t> in(tie_in, tie_in + sizeof(tie_in));
            std::vector<std::uint8_t> obj = makeObj();
            std::vector<std::uint8_t> out;
            const lift::LzssEncodeOutcome r = runKernel(in, 0x00400000u, &obj, &out);
            LIFT_CHECK_EQ_U32(r.consumed, 8u);
            LIFT_CHECK_EQ_U32(r.comp, 7u);
            LIFT_CHECK(!r.gave_up);
            LIFT_CHECK_EQ_U32(out[3], 0xEEu);  // match_pos & 0xFF = 0xFEE 的低字节（= 4078，先入树者）
            LIFT_CHECK(std::memcmp(out.data(), tie_expected, sizeof(tie_expected)) == 0);
            checkTree(obj, in, r.consumed, r.gave_up);
        }

        // ---- ③ 独立转写对拍 ----
        lift_test::g_current_case = "lzss/independent-transcription";
        for (std::uint32_t n : {0u, 1u, 5u, 17u, 18u, 19u, 64u, 300u, 4096u, 4097u, 9000u}) {
            for (std::uint32_t kind = 0; kind < 5u; ++kind) {
                const std::vector<std::uint8_t> in = mkInput(n, kind);
                std::vector<std::uint8_t> obj = makeObj();
                std::vector<std::uint8_t> out;
                const lift::LzssEncodeOutcome r = runKernel(in, 0x00400000u, &obj, &out);
                const NaiveEncoded nv = naiveLzssEncode(in.empty() ? nullptr : in.data(), n);
                LIFT_CHECK_EQ_U32(r.consumed, nv.consumed);
                LIFT_CHECK_EQ_U32(r.comp, nv.comp);
                LIFT_CHECK(r.gave_up == nv.gave_up);
                if (r.comp != 0u) {
                    const bool same = std::memcmp(out.data(), nv.out.data(), r.comp) == 0;
                    LIFT_CHECK(same);
                    if (!same) {
                        std::size_t at = 0;
                        for (; at < r.comp && out[at] == nv.out[at]; ++at) {
                        }
                        std::fprintf(stderr,
                                     "  [naive n=%u kind=%u] comp=%u/%u 首个差异 @%zu: 生产=%02X 转写=%02X\n",
                                     n, kind, r.comp, nv.comp, at, at < r.comp ? out[at] : 0,
                                     at < nv.out.size() ? nv.out[at] : 0);
                    }
                }
            }
        }
        {
            // 真存档 payload 量级（150,068 B）× 可压 / 不可压 两档
            for (std::uint32_t kind : {2u, 4u}) {
                const std::vector<std::uint8_t> in = mkInput(150068u, kind);
                std::vector<std::uint8_t> obj = makeObj();
                std::vector<std::uint8_t> out;
                const lift::LzssEncodeOutcome r = runKernel(in, 0x00400000u, &obj, &out);
                const NaiveEncoded nv = naiveLzssEncode(in.data(), 150068u);
                LIFT_CHECK_EQ_U32(r.consumed, nv.consumed);
                LIFT_CHECK_EQ_U32(r.comp, nv.comp);
                LIFT_CHECK(r.gave_up == nv.gave_up);
                if (r.comp != 0u) LIFT_CHECK(std::memcmp(out.data(), nv.out.data(), r.comp) == 0);
            }
        }

        // ---- ④ `lzssEncodeObject` ↔ `lzssDecode` 往返 ----
        // ★ give_up 分支（`comp == in_size`）**不能只靠解码往返**：`lzssDecode` 在
        //   `comp_size == out_size` 时直接 `memcpy`（native_lzss.cpp:18-21）⇒ "解码 = 原文"恒真、
        //   等于没测。所以 give_up 要**单独**断言三件事：`consumed == in_size`、`comp == in_size`、
        //   `out[0..in_size)` 逐字节是原文（这才是"放弃压缩 = 原样拷贝"的判据）。
        lift_test::g_current_case = "lzss/roundtrip-decode";
        for (std::uint32_t n : {0u, 1u, 17u, 18u, 4078u, 4096u, 4097u, 150068u}) {
            for (std::uint32_t kind = 0; kind < 5u; ++kind) {
                const std::vector<std::uint8_t> in = mkInput(n, kind);
                std::vector<std::uint8_t> obj = makeObj();
                std::vector<std::uint8_t> out;
                const lift::LzssEncodeOutcome r = runKernel(in, 0x00400000u, &obj, &out);
                LIFT_CHECK(r.comp <= n);
                if (r.gave_up) {
                    LIFT_CHECK_EQ_U32(r.comp, n);
                    LIFT_CHECK_EQ_U32(r.consumed, n);
                    if (n != 0u) LIFT_CHECK(std::memcmp(out.data(), in.data(), n) == 0);
                }
                std::vector<std::uint8_t> back(n + 1u, 0);  // n == 0 也要给非空指针（lzssDecode 拒 nullptr）
                std::uint8_t ring[4096] = {};
                LIFT_CHECK(lift::lzssDecode(out.data(), r.comp, back.data(), n, ring));
                LIFT_CHECK(std::memcmp(back.data(), in.data(), n) == 0);
            }
        }

        // ---- ⑤ 32 位回绕：`a2 + a3[0]` 回绕 ⇒ 首灌 0 次 ⇒ consumed == 0、comp == 0、只 InitTree ----
        // （guest `0x43685D jnb`：地址比较是**无符号**；这里 in_ea 只参与那个比较，⛔ 不读 in。）
        lift_test::g_current_case = "lzss/wrap-address";
        {
            std::vector<std::uint8_t> in(0x2000u, 0x42u);
            std::vector<std::uint8_t> obj = makeObj();
            std::vector<std::uint8_t> out;
            const lift::LzssEncodeOutcome r = runKernel(in, 0xFFFFF000u, &obj, &out);
            LIFT_CHECK_EQ_U32(r.consumed, 0u);  // **按计数**写：不是 a3[0]
            LIFT_CHECK_EQ_U32(r.comp, 0u);
            LIFT_CHECK(!r.gave_up);
            LIFT_CHECK_EQ_U32(getU32(obj, kDad + 0u), 4096u);  // InitTree 写了 NIL（不是 0）
            for (std::uint32_t i = 0; i < 4078u; ++i) LIFT_CHECK(obj[0x004u + i] == 0u);
        }

        // ---- ⑥ 准入面：每条回退路径都零写 + declines 桶；happy path 出参与核结果一致 ----
        lift_test::g_current_case = "lzss/admission-no-write";
        {
            lift_fixture::Fixture fx;
            LIFT_CHECK(fx.ok);
            if (fx.ok) {
                const std::uint32_t self = 0x00420000u;
                const std::uint32_t info = 0x00430000u;
                const std::uint32_t in_ea = 0x00440000u;
                std::vector<std::uint8_t> obj0 = makeObj();
                std::vector<std::uint8_t> info0(12u, 0);
                std::vector<std::uint8_t> input(64u, 0x41u);
                LIFT_CHECK(fx.m.placeData(self, obj0.data(), obj0.size(), &fx.err));
                LIFT_CHECK(fx.m.placeData(info, info0.data(), info0.size(), &fx.err));
                LIFT_CHECK(fx.m.placeData(in_ea, input.data(), input.size(), &fx.err));
                fx.m.mem().wr32(self + kOutField, 0u);  // "新对象"形态（其余字节保持 0xCC）
                fx.m.mem().wr32(info + 0u, 64u);
                fx.m.mem().wr32(info + 4u, 0xAAAA5555u);
                fx.m.mem().wr32(info + 8u, 0x5A5AA5A5u);

                lift::NativeOverride ov;
                auto armCall = [&](std::uint32_t ecx_self, std::uint32_t a0, std::uint32_t a1) {
                    const std::uint32_t esp = fx.m.layout().stack_top - 0x100u;
                    fx.m.cpu().esp() = esp;
                    fx.m.mem().wr32(esp, 0xFFFFFFFFu);  // 返回地址（不真的返回）
                    fx.m.mem().wr32(esp + 4u, a0);
                    fx.m.mem().wr32(esp + 8u, a1);
                    fx.m.cpu().ecx() = ecx_self;
                    std::memset(ov.declines, 0, sizeof(ov.declines));
                    fx.m.cpu().current_override = &ov;
                };
                auto invoke = [&](std::uint32_t ecx_self, std::uint32_t a0, std::uint32_t a1, std::uint32_t* eax) {
                    armCall(ecx_self, a0, a1);
                    const bool ok = lift::nativeLzss4367B0(fx.m.cpu(), fx.m.mem(), eax);
                    fx.m.cpu().current_override = nullptr;
                    return ok;
                };
                auto snapshot = [&](std::vector<std::uint8_t>* o, std::vector<std::uint8_t>* i) {
                    const std::uint8_t* po = fx.m.mem().host(self);
                    o->assign(po, po + kObjBytes);
                    const std::uint8_t* pi = fx.m.mem().host(info);
                    i->assign(pi, pi + 12u);
                };
                auto expectDecline = [&](lift::LzssWhy why, std::uint32_t ecx_self, std::uint32_t a0,
                                         std::uint32_t a1) {
                    std::vector<std::uint8_t> before_obj;
                    std::vector<std::uint8_t> before_info;
                    snapshot(&before_obj, &before_info);
                    std::uint32_t eax = 0xDEADBEEFu;
                    LIFT_CHECK(!invoke(ecx_self, a0, a1, &eax));
                    std::vector<std::uint8_t> after_obj;
                    std::vector<std::uint8_t> after_info;
                    snapshot(&after_obj, &after_info);
                    LIFT_CHECK(std::memcmp(before_obj.data(), after_obj.data(), kObjBytes) == 0);
                    LIFT_CHECK(std::memcmp(before_info.data(), after_info.data(), 12u) == 0);
                    LIFT_CHECK_EQ_U32(ov.declines[static_cast<std::size_t>(why)], 1u);
                };

                // 1) 没有堆（这个 Fixture 还没 init 堆）
                expectDecline(lift::LzssWhy::NoHeap, self, in_ea, info);
                // 2) info == 0 / info 不在已提交页
                expectDecline(lift::LzssWhy::NoInfo, self, in_ea, 0u);
                expectDecline(lift::LzssWhy::NoInfo, self, in_ea, 0x00001000u);
                // 3) this == 0 / 不在已提交页
                expectDecline(lift::LzssWhy::BadSelf, 0u, in_ea, info);
                expectDecline(lift::LzssWhy::BadSelf, 0x00002000u, in_ea, info);
                // 4) 输入跨度不在已提交页
                expectDecline(lift::LzssWhy::BadInput, self, 0x00003000u, info);
                // ★ `GuestHeap::init` 的最小尺寸是 0x10000（heap.cpp:27）⇒ 用一个 **0x10000** 的堆：
                //   happy path 只要 64+0x1000，而下面的"分配失败"档要 0x10000+0x1000 > 0x10000。
                LIFT_CHECK(fx.m.heap().init(&fx.m.mem(), fx.m.layout().heap_base, 0x10000u));
                // 5) `a3[0] + 0x1000` 溢出（`0xFFFFF000` 是**边界**：它回绕成 0 ⇒ 必须拒）
                fx.m.mem().wr32(info + 0u, 0xFFFFF000u);
                expectDecline(lift::LzssWhy::Overflow, self, in_ea, info);
                fx.m.mem().wr32(info + 0u, 64u);
                // 6) `out != 0`（对象被复用 / 未构造）⇒ 拒，且不碰那个旧指针
                fx.m.mem().wr32(self + kOutField, 0x1234u);
                expectDecline(lift::LzssWhy::ReusedOut, self, in_ea, info);
                fx.m.mem().wr32(self + kOutField, 0u);
                // 7) 分配失败（堆只有 0x10000，请求 0x10000+0x1000）⇒ 回退、零写
                {
                    const std::uint32_t big_in = 0x00450000u;
                    std::vector<std::uint8_t> big(0x10000u, 0x37u);
                    LIFT_CHECK(fx.m.placeData(big_in, big.data(), big.size(), &fx.err));
                    fx.m.mem().wr32(info + 0u, 0x10000u);
                    expectDecline(lift::LzssWhy::AllocFailed, self, big_in, info);
                    fx.m.mem().wr32(info + 0u, 64u);
                }
                // 8) happy path：出参 = 独立算出的核结果；只有 out 指针 / a3[1] / a3[2] 被写
                {
                    std::vector<std::uint8_t> before_obj;
                    std::vector<std::uint8_t> before_info;
                    snapshot(&before_obj, &before_info);
                    std::uint32_t eax = 0u;
                    LIFT_CHECK(invoke(self, in_ea, info, &eax));
                    LIFT_CHECK(eax != 0u);
                    std::vector<std::uint8_t> ref_obj = makeObj();
                    std::vector<std::uint8_t> ref_out;
                    const lift::LzssEncodeOutcome ref = runKernel(input, in_ea, &ref_obj, &ref_out);
                    LIFT_CHECK_MEM_U32(fx.m.mem(), self + kOutField, eax);
                    LIFT_CHECK_MEM_U32(fx.m.mem(), info + 0u, 64u);
                    LIFT_CHECK_MEM_U32(fx.m.mem(), info + 4u, ref.consumed);
                    LIFT_CHECK_MEM_U32(fx.m.mem(), info + 8u, ref.comp);
                    // ★ 对象要从 `[0, 0xD42C)` 比：`+0xD42C`（= out 字段）由**包装**写（纯核不碰），
                    //   单独逐字比对更紧（下一行）。
                    LIFT_CHECK(std::memcmp(fx.m.mem().host(self), ref_obj.data(), kOutField) == 0);
                    if (eax != 0u && ref.comp != 0u) {
                        LIFT_CHECK(std::memcmp(fx.m.mem().host(eax), ref_out.data(), ref.comp) == 0);
                    }
                    for (std::size_t i = 0; i < lift::NativeOverride::kDeclineSlots; ++i)
                        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(ov.declines[i]), 0u);
                }
            }
        }

    }

    return lift_test::finish("native-override");
}
