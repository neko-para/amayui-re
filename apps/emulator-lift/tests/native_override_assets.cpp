// emulator-lift · assets 档：**真 guest 字节**的 native override 差分（解释器 vs override）
//
// 判据（红了意味着什么）：
//   ① 从 `AGE.EXE` 里取出 `sub_429020` / `sub_48AE90` / `sub_401000` / `sub_4CCC60` / `sub_4E0B40` 的**真字节**，放进 bare 机
//      的**同一 EA**（与 boot 里一模一样），同一份夹具跑**解释器** vs **override**；
//      目的内存 / EAX / ESP / 标号寄存器 / EIP 必须逐位一致。
//   ② 48AE90 还要比**整张纹理逐字节** + 锁记账（locks / version / locked）。
// ★ 为什么不用手写的参考 asm：手抄错时"两边都错"看不出来；真字节是唯一的外部 oracle
//   （`.lst` 只用来确定 EA，不参与执行）。
// ★ 缺资产 ⇒ SKIP 77（CMake 侧 `SKIP_RETURN_CODE 77`，注册在 `if(EXISTS ... .lst)` 里 = assets 档）。
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>

#include "check.h"
#include "fixture.h"
#include "runtime/com.h"
#include "runtime/d3d9.h"
#include "runtime/native_affine.h"
#include "runtime/native_mdct.h"
#include "runtime/native_overrides.h"
#include "shims/register.h"

namespace {

std::string envValue(const char* name) {
    const char* v = std::getenv(name);
    return v != nullptr ? std::string(v) : std::string();
}

// 用 PE 载入器把执行源映射进 guest（⛔ 不自己解析 PE），再把某段代码按字节拷出来。
struct ImageCode {
    lift::Machine img;
    std::string err;
    bool ok = false;

    bool load(const std::string& path) {
        if (!img.init(&err)) return false;
        if (!img.loadImage(path, &err)) return false;
        ok = true;
        return true;
    }
    std::vector<std::uint8_t> at(std::uint32_t ea, std::size_t n) const {
        const std::uint8_t* p = img.mem().host(ea);
        return std::vector<std::uint8_t>(p, p + n);
    }
};

// ---- sub_429020 的夹具（MSVC std::string 槽：0x10=len / 0x14=cap；cap>=16 ⇒ [slot] 是堆指针）----
struct MapCase {
    std::vector<std::string> slots;
    std::string key;
    std::uint32_t index0 = 0;
};

std::uint32_t placeString(lift_fixture::Fixture& fx, std::uint32_t ea, const std::string& s) {
    std::vector<std::uint8_t> b(s.begin(), s.end());
    b.push_back(0);
    if (!fx.m.placeData(ea, b.data(), b.size(), &fx.err)) fx.ok = false;
    return ea;
}

struct MapResult {
    bool valid = false;
    std::uint32_t eax = 0;
    std::uint32_t esp = 0;
    std::uint32_t eip = 0;
    std::uint32_t ebx = 0;
    std::uint32_t esi = 0;
    std::uint32_t edi = 0;
    std::vector<std::uint8_t> table;
};

MapResult runMap(const std::vector<std::uint8_t>& code401000, const std::vector<std::uint8_t>& code429020,
                 const MapCase& c, bool with_override) {
    MapResult out;
    lift_fixture::Fixture fx;
    if (!fx.ok) return out;
    const std::uint32_t self = 0x00420000u;
    const std::uint32_t table = 0x00450000u;
    const std::uint32_t heap = 0x00460000u;
    const std::uint32_t capacity = static_cast<std::uint32_t>(c.slots.size());
    const std::size_t table_bytes = static_cast<std::size_t>(capacity) * 28u;
    std::vector<std::uint8_t> zeros(table_bytes + 64u, 0);
    if (!fx.m.placeData(self, zeros.data(), 32u, &fx.err)) return out;
    if (!fx.m.placeData(table, zeros.data(), table_bytes, &fx.err)) return out;
    if (!fx.m.placeData(heap, zeros.data(), zeros.size(), &fx.err)) return out;
    if (!fx.m.placeCode(0x00401000u, code401000.data(), code401000.size(), &fx.err)) return out;
    if (!fx.m.placeCode(0x00429020u, code429020.data(), code429020.size(), &fx.err)) return out;
    fx.m.mem().wr32(self + 0x18u, table);
    fx.m.mem().wr32(self + 8u, capacity);
    std::uint32_t heap_cursor = heap;
    for (std::uint32_t i = 0; i < capacity; ++i) {
        const std::uint32_t slot = table + i * 28u;
        const std::string& s = c.slots[i];
        fx.m.mem().wr32(slot + 0x10u, static_cast<std::uint32_t>(s.size()));
        if (s.size() < 16u) {
            for (std::size_t k = 0; k < s.size(); ++k)
                fx.m.mem().wr8(slot + static_cast<std::uint32_t>(k), static_cast<std::uint8_t>(s[k]));
            fx.m.mem().wr32(slot + 0x14u, 15u);
        } else {
            for (std::size_t k = 0; k < s.size(); ++k)
                fx.m.mem().wr8(heap_cursor + static_cast<std::uint32_t>(k), static_cast<std::uint8_t>(s[k]));
            fx.m.mem().wr32(slot + 0x00u, heap_cursor);
            fx.m.mem().wr32(slot + 0x14u, static_cast<std::uint32_t>(s.size()));
            heap_cursor += static_cast<std::uint32_t>(s.size()) + 1u;
        }
    }
    std::vector<std::uint8_t> keybuf(64u, 0);
    if (!fx.m.placeData(0x00470000u, keybuf.data(), keybuf.size(), &fx.err)) return out;
    const std::uint32_t key_ea = placeString(fx, 0x00470000u, c.key);
    if (!fx.ok) return out;
    if (with_override) {
        std::string err;
        if (!fx.m.overrides().add(0x00429020u, "ref_find", lift::GuestConv::Thiscall, 8,
                                  lift::nativeFindIndex429020, &err))
            return out;
    }
    const std::uint32_t call_ea = 0x00402000u;
    lift_fixture::Asm a(call_ea);
    a.u8(0xB9);  // mov ecx, self
    a.u32(self);
    a.pushImm(key_ea);
    a.pushImm(c.index0);
    a.callRel(0x00429020u);
    a.ret();
    if (!fx.m.placeCode(call_ea, a.b.data(), a.b.size(), &fx.err)) return out;
    fx.rearmStack();
    fx.runCode(call_ea);
    if (!fx.ok) return out;
    out.valid = true;
    out.eax = fx.m.cpu().eax();
    out.esp = fx.m.cpu().esp();
    out.eip = fx.m.cpu().eip;
    out.ebx = fx.m.cpu().ebx();
    out.esi = fx.m.cpu().esi();
    out.edi = fx.m.cpu().edi();
    const std::uint8_t* p = fx.m.mem().host(table);
    out.table.assign(p, p + table_bytes);
    if (with_override) {
        const lift::NativeOverride* e = fx.m.overrides().at(0);
        LIFT_CHECK_EQ_U32(e->hits, 1u);
        LIFT_CHECK_EQ_U32(e->fallbacks, 0u);
    }
    return out;
}

// ---- sub_48AE90 的纹理夹具：真 COM 对象（vtable +0x4C / +0x50 必须是本线的 lock/unlock）----
void shimLockRect(lift::X86& cpu, lift::ShimCall& call) {
    const std::uint32_t self = call.u32(0);
    lift::d3d9::TextureInfo ti;
    std::string why;
    if (!lift::d3d9::textureInfo(cpu, self, &ti, &why)) {
        call.result = lift::d3d9::kInvalidCall;
        return;
    }
    call.result = lift::d3d9::lockRect(cpu, ti, call.u32(1), call.u32(2), call.u32(3), call.u32(4), &why)
                      ? lift::d3d9::kOk
                      : lift::d3d9::kInvalidCall;
}
void shimUnlockRect(lift::X86& cpu, lift::ShimCall& call) {
    const std::uint32_t self = call.u32(0);
    lift::d3d9::TextureInfo ti;
    std::string why;
    if (!lift::d3d9::textureInfo(cpu, self, &ti, &why)) {
        call.result = lift::d3d9::kInvalidCall;
        return;
    }
    call.result = lift::d3d9::unlockRect(cpu, ti, call.u32(1), &why) ? lift::d3d9::kOk
                                                                     : lift::d3d9::kInvalidCall;
}

struct FillResult {
    bool valid = false;
    std::uint32_t eax = 0;
    std::uint32_t esp = 0;
    std::uint32_t eip = 0;
    std::uint32_t locks = 0;
    std::uint32_t version = 0;
    bool locked = false;
    std::vector<std::uint8_t> arena;
};

FillResult runFill(const std::vector<std::uint8_t>& code48AE90, const std::string& err_prefix,
                   std::uint32_t tex_bit, std::uint32_t tex_w, std::uint32_t tex_h, std::uint32_t format,
                   std::int32_t left, std::int32_t top, std::int32_t right, std::int32_t bottom,
                   std::uint32_t color, std::int32_t alpha, bool with_override) {
    FillResult out;
    lift_fixture::Fixture fx;
    if (!fx.ok) return out;
    if (!fx.m.heap().init(&fx.m.mem(), fx.m.layout().heap_base, 0x00100000u)) return out;
    fx.m.shims().configureStubPage(0x7FF90000u, 0x1000u);
    lift::X86& cpu = fx.m.cpu();
    lift::d3d9::CreateRequest req;
    req.device_type = lift::d3d9::kDevTypeHal;
    req.behavior_flags = 0x44u;
    req.present.width = 64u;
    req.present.height = 64u;
    req.present.format = lift::d3d9::kFmtX8R8G8B8;
    req.present.windowed = true;
    const std::uint32_t dev_state = lift::d3d9::createDeviceState(cpu, req, &fx.err);
    if (dev_state == 0) return out;
    const std::vector<lift::ComMethod> device_slots = {{0u, "GetCaps", lift::Conv::Stdcall, 1, nullptr}};
    const std::uint32_t device = cpu.com->createObject(cpu, "IDirect3DDevice9", device_slots, dev_state);
    if (device == 0) return out;
    const std::uint32_t tex_state =
        lift::d3d9::createTextureState(cpu, device, tex_w, tex_h, 1u, 0u, format, 1u, &fx.err);
    if (tex_state == 0) return out;
    const std::vector<lift::ComMethod> tex_slots = {
        {0x4Cu, "LockRect", lift::Conv::Stdcall, 4, &shimLockRect},
        {0x50u, "UnlockRect", lift::Conv::Stdcall, 1, &shimUnlockRect},
    };
    const std::uint32_t texture = cpu.com->createObject(cpu, "IDirect3DTexture9", tex_slots, tex_state);
    if (texture == 0) return out;
    lift::d3d9::TextureInfo ti;
    if (!lift::d3d9::textureInfo(cpu, texture, &ti, &fx.err)) return out;
    // 纹理位深（= `ds:dword_5518C0`，`.lst` 0x406B14 是唯一写点）
    std::vector<std::uint8_t> bitbuf(4u, 0);
    if (!fx.m.placeData(0x005518C0u, bitbuf.data(), bitbuf.size(), &fx.err)) return out;
    fx.m.mem().wr32(0x005518C0u, tex_bit);
    // this 块 + rect
    const std::uint32_t self = 0x00420000u;
    std::vector<std::uint8_t> self_blk(0x410u, 0);
    if (!fx.m.placeData(self, self_blk.data(), self_blk.size(), &fx.err)) return out;
    fx.m.mem().wr32(self + 0x40Cu, texture);
    const std::uint32_t rect_ea = 0x00430000u;
    std::vector<std::uint8_t> rect(16u, 0);
    if (!fx.m.placeData(rect_ea, rect.data(), rect.size(), &fx.err)) return out;
    fx.m.mem().wr32(rect_ea + 0u, static_cast<std::uint32_t>(left));
    fx.m.mem().wr32(rect_ea + 4u, static_cast<std::uint32_t>(top));
    fx.m.mem().wr32(rect_ea + 8u, static_cast<std::uint32_t>(right));
    fx.m.mem().wr32(rect_ea + 12u, static_cast<std::uint32_t>(bottom));
    // arena 预填 0x5A（保留 alpha 的分支要从这里读旧值）
    const std::size_t arena_n = static_cast<std::size_t>(ti.pitch) * ti.height;
    std::memset(fx.m.mem().host(ti.arena), 0x5A, arena_n);
    if (!fx.m.placeCode(0x0048AE90u, code48AE90.data(), code48AE90.size(), &fx.err)) return out;
    if (with_override) {
        std::string err;
        if (!fx.m.overrides().add(0x0048AE90u, "ref_fill", lift::GuestConv::Thiscall, 12,
                                  lift::nativeFill48AE90, &err))
            return out;
    }
    const std::uint32_t call_ea = 0x00402000u;
    lift_fixture::Asm a(call_ea);
    a.u8(0xB9);
    a.u32(self);
    a.pushImm(static_cast<std::uint32_t>(alpha));
    a.pushImm(color);
    a.pushImm(rect_ea);
    a.callRel(0x0048AE90u);
    a.ret();
    if (!fx.m.placeCode(call_ea, a.b.data(), a.b.size(), &fx.err)) return out;
    fx.rearmStack();
    fx.runCode(call_ea);
    if (!fx.ok) {
        std::fprintf(stderr, "  [fill %s] 运行失败：%s\n", err_prefix.c_str(), fx.err.c_str());
        return out;
    }
    lift::d3d9::TextureInfo after;
    if (!lift::d3d9::textureInfo(cpu, texture, &after, nullptr)) return out;
    out.valid = true;
    out.eax = cpu.eax();
    out.esp = cpu.esp();
    out.eip = cpu.eip;
    out.locks = after.locks;
    out.version = after.version;
    out.locked = after.locked;
    const std::uint8_t* p = fx.m.mem().host(ti.arena);
    out.arena.assign(p, p + arena_n);
    if (with_override) {
        const lift::NativeOverride* e = fx.m.overrides().at(0);
        LIFT_CHECK_EQ_U32(e->hits, 1u);
        LIFT_CHECK_EQ_U32(e->fallbacks, 0u);
    }
    return out;
}

}  // namespace

// ---- sub_46D9F0 的字形夹具（Font / mgr / 纹理槽 / 裁剪对象 / a4 / 源 / 目标表面全是合成内存）----
struct GlyphCase {
    std::uint32_t tex_bit = 16u;  // ds:0x5518C0（16 / 32）
    bool level8 = false;          // [Font+0x548] != 0 ⇒ 8bpp 级数
    bool levels65 = false;        // [Font+0x35584] == 1 ⇒ 65 级
    std::uint32_t w = 2u;
    std::uint32_t h = 2u;
    std::int32_t ox = 0;
    std::int32_t oy = 0;
    std::uint32_t stride = 4u;
    std::vector<std::uint8_t> src;  // h*stride 字节（不足补 0）
    std::int32_t x = 1;
    std::int32_t y = 0;
    std::int32_t l = 0, t = 0, r = 4, b = 2;
    std::uint32_t pitch = 8u;
    std::uint32_t color = 0x00804020u;
    std::uint32_t dst_fill = 0x5A5A5A5Au;  // 按 4 字节重复
    std::uint32_t dst_bytes = 16u;
};

struct GlyphResult {
    bool valid = false;
    std::uint32_t eax = 0, esp = 0, eip = 0, ebx = 0, esi = 0, edi = 0, ebp = 0;
    std::vector<std::uint8_t> dst;
    std::uint64_t hits = 0, fallbacks = 0;
};

GlyphResult runGlyph(const std::vector<std::uint8_t>& code46CE20, const std::vector<std::uint8_t>& code46D9F0,
                     const GlyphCase& c, bool with_override) {
    GlyphResult out;
    lift_fixture::Fixture fx;
    if (!fx.ok) return out;
    const std::uint32_t font = 0x00420000u;
    const std::uint32_t mgr = 0x00460000u;
    const std::uint32_t clip = 0x00470000u;
    const std::uint32_t src_ea = 0x00480000u;
    const std::uint32_t dst_ea = 0x00490000u;
    const std::uint32_t metrics = 0x004A0000u;
    const std::uint32_t texbit = 0x005518C0u;
    std::vector<std::uint8_t> zeros(0x420, 0);
    if (!fx.m.placeData(font, zeros.data(), zeros.size(), &fx.err)) return out;
    if (!fx.m.placeData(font + 0x548u, zeros.data(), 4u, &fx.err)) return out;
    if (!fx.m.placeData(font + 0x35584u, zeros.data(), 4u, &fx.err)) return out;
    if (!fx.m.placeData(mgr + 0xA5D8u, zeros.data(), 4u, &fx.err)) return out;
    if (!fx.m.placeData(mgr + 0xB648u, zeros.data(), 4u, &fx.err)) return out;
    if (!fx.m.placeData(clip + 0x41Cu, zeros.data(), 16u, &fx.err)) return out;
    if (!fx.m.placeData(metrics, zeros.data(), 16u, &fx.err)) return out;
    if (!fx.m.placeData(texbit, zeros.data(), 4u, &fx.err)) return out;
    std::vector<std::uint8_t> dst(c.dst_bytes, 0);
    for (std::size_t i = 0; i + 4u <= dst.size(); i += 4u)
        std::memcpy(dst.data() + i, &c.dst_fill, 4);
    if (!fx.m.placeData(dst_ea, dst.data(), dst.size(), &fx.err)) return out;
    const std::size_t src_bytes = static_cast<std::size_t>(c.h) * c.stride + 8u;
    std::vector<std::uint8_t> srcpad(src_bytes, 0);
    if (!c.src.empty()) std::memcpy(srcpad.data(), c.src.data(), c.src.size() < srcpad.size() ? c.src.size() : srcpad.size());
    if (!fx.m.placeData(src_ea, srcpad.data(), srcpad.size(), &fx.err)) return out;
    if (!fx.m.placeCode(0x0046CE20u, code46CE20.data(), code46CE20.size(), &fx.err)) return out;
    if (!fx.m.placeCode(0x0046D9F0u, code46D9F0.data(), code46D9F0.size(), &fx.err)) return out;
    auto& mem = fx.m.mem();
    mem.wr32(font + 0x410u, mgr);
    mem.wr32(font + 0x548u, c.level8 ? 1u : 0u);
    mem.wr32(font + 0x35584u, c.levels65 ? 1u : 0u);
    mem.wr32(mgr + 0xB648u, 0u);    // texid = 0
    mem.wr32(mgr + 0xA5D8u, clip);  // 槽 0 = 裁剪对象
    mem.wr32(clip + 0x41Cu, static_cast<std::uint32_t>(c.l));
    mem.wr32(clip + 0x420u, static_cast<std::uint32_t>(c.t));
    mem.wr32(clip + 0x424u, static_cast<std::uint32_t>(c.r));
    mem.wr32(clip + 0x428u, static_cast<std::uint32_t>(c.b));
    mem.wr32(texbit, c.tex_bit);
    mem.wr32(metrics + 0u, c.w);
    mem.wr32(metrics + 4u, c.h);
    mem.wr32(metrics + 8u, static_cast<std::uint32_t>(c.ox));
    mem.wr32(metrics + 12u, static_cast<std::uint32_t>(c.oy));
    if (with_override) {
        std::string err;
        if (!fx.m.overrides().add(0x0046D9F0u, "ref_glyph", lift::GuestConv::Thiscall, 32,
                                  lift::nativeBlend46D9F0, &err))
            return out;
    }
    const std::uint32_t call_ea = 0x00402000u;
    lift_fixture::Asm a(call_ea);
    a.u8(0xB9);  // mov ecx, font
    a.u32(font);
    a.pushImm(c.color);
    a.pushImm(static_cast<std::uint32_t>(c.y));
    a.pushImm(static_cast<std::uint32_t>(c.x));
    a.pushImm(src_ea);
    a.pushImm(c.stride);
    a.pushImm(metrics);
    a.pushImm(dst_ea);
    a.pushImm(c.pitch);
    a.callRel(0x0046D9F0u);
    a.ret();
    if (!fx.m.placeCode(call_ea, a.b.data(), a.b.size(), &fx.err)) return out;
    fx.rearmStack();
    fx.runCode(call_ea);
    if (!fx.ok) return out;
    out.valid = true;
    out.eax = fx.m.cpu().eax();
    out.esp = fx.m.cpu().esp();
    out.eip = fx.m.cpu().eip;
    out.ebx = fx.m.cpu().ebx();
    out.esi = fx.m.cpu().esi();
    out.edi = fx.m.cpu().edi();
    out.ebp = fx.m.cpu().ebp();
    const std::uint8_t* p = fx.m.mem().host(dst_ea);
    out.dst.assign(p, p + c.dst_bytes);
    if (with_override) {
        const lift::NativeOverride* e = fx.m.overrides().at(0);
        out.hits = e->hits;
        out.fallbacks = e->fallbacks;
    }
    return out;
}

// ---- sub_4CCC60 的仿射夹具（state / inner 对象 / src / dst / flt_52F1B0 全是合成内存）----
struct AffineCase {
    float angle_deg = 30.0f;
    float angle_scale = 0.017453292f;
    float radius = 1.0f;
    std::uint8_t flip_x = 0;
    std::uint8_t flip_y = 0;
    float add4 = 0.0f;
    float add8 = 0.0f;
    std::int32_t count = 2;
    std::int32_t i0 = 0;
    std::int32_t stride = 2;
    std::vector<float> src;
    bool alias = false;       // src == dst（原地）
    bool pick_o24 = false;    // state+0x24 非 0 ⇒ 走第二个 inner 对象
    std::uint32_t dst_fill = 0xCCCCCCCCu;
    bool fpu_noise = false;   // 调用前 fld1（x87 栈非空 ⇒ override 必须 Declined）
    // ★ x87 控制字 / 入口状态字：缺省值 = x87 reset 值（`0x037F` / 0）⇒ 现有用例的行为不变。
    //   控制字是**真实输入面**（语料里 `fldcw` 94 处），状态字的 C0..C3 是 `fsin`/`fcos` 会清的位
    //   （`.lst` 0x4CCC80/0x4CCC84 之后的 `fnstsw` 能读到）⇒ 两臂都必须从同一份初值出发。
    std::uint16_t control = 0x037Fu;
    std::uint16_t entry_status = 0;
};

struct AffineResult {
    bool valid = false;
    std::uint32_t eax = 0, esp = 0, eip = 0, ebx = 0, esi = 0, edi = 0, ebp = 0;
    std::uint16_t fpu_status = 0, fpu_control = 0, fpu_tag = 0;
    std::uint8_t fpu_top = 0;
    std::vector<std::uint8_t> buf;
    std::uint64_t hits = 0, fallbacks = 0;
};

AffineResult runAffine(const std::vector<std::uint8_t>& code4CCC60, const AffineCase& c, bool with_override) {
    AffineResult out;
    lift_fixture::Fixture fx;
    if (!fx.ok) return out;
    const std::uint32_t state_ea = 0x00420000u;
    const std::uint32_t obj_ea = 0x00430000u;
    const std::uint32_t obj2_ea = 0x00440000u;
    const std::uint32_t src_ea = 0x00480000u;
    const std::uint32_t dst_ea = 0x00490000u;
    const std::uint32_t scale_ea = 0x0052F1B0u;
    constexpr std::size_t kFloats = 64u;
    auto putF32 = [](std::uint8_t* p, float v) { std::memcpy(p, &v, 4); };
    auto putU32 = [](std::uint8_t* p, std::uint32_t v) { std::memcpy(p, &v, 4); };

    std::vector<std::uint8_t> state(0x28u, 0), obj(0x20u, 0), obj2(0x20u, 0);
    putF32(state.data() + 0x10u, c.radius);
    putU32(state.data() + 0x20u, obj_ea);
    putU32(state.data() + 0x24u, c.pick_o24 ? obj2_ea : 0u);
    putF32(obj.data() + 0x04u, c.add4);
    putF32(obj.data() + 0x08u, c.add8);
    putF32(obj.data() + 0x14u, c.angle_deg);
    obj[0x18] = c.flip_x;
    obj[0x19] = c.flip_y;
    obj2 = obj;
    putF32(obj2.data() + 0x04u, c.add4 + 100.0f);
    putF32(obj2.data() + 0x08u, c.add8 + 200.0f);
    putF32(obj2.data() + 0x14u, c.angle_deg + 45.0f);

    std::vector<std::uint8_t> srcbuf(kFloats * 4u, 0), dstbuf(kFloats * 4u, 0);
    for (std::size_t i = 0; i < kFloats; ++i) {
        const float v = i < c.src.size() ? c.src[i] : 0.0f;
        std::memcpy(srcbuf.data() + i * 4u, &v, 4);
    }
    for (std::size_t i = 0; i + 4u <= dstbuf.size(); i += 4u) std::memcpy(dstbuf.data() + i, &c.dst_fill, 4);
    std::uint32_t scale_bits = 0;
    std::memcpy(&scale_bits, &c.angle_scale, 4);

    if (!fx.m.placeData(state_ea, state.data(), state.size(), &fx.err)) return out;
    if (!fx.m.placeData(obj_ea, obj.data(), obj.size(), &fx.err)) return out;
    if (!fx.m.placeData(obj2_ea, obj2.data(), obj2.size(), &fx.err)) return out;
    if (!fx.m.placeData(scale_ea, &scale_bits, 4u, &fx.err)) return out;
    if (!fx.m.placeData(src_ea, srcbuf.data(), srcbuf.size(), &fx.err)) return out;
    if (!fx.m.placeData(dst_ea, dstbuf.data(), dstbuf.size(), &fx.err)) return out;
    if (!fx.m.placeCode(0x004CCC60u, code4CCC60.data(), code4CCC60.size(), &fx.err)) return out;

    if (with_override) {
        std::string err;
        if (!fx.m.overrides().add(0x004CCC60u, "ref_affine", lift::GuestConv::Stdcall, 28,
                                  lift::nativeAffine4CCC60, &err))
            return out;
    }
    const std::uint32_t dst_ptr = c.alias ? src_ea : dst_ea;
    const std::uint32_t call_ea = 0x00402000u;
    lift_fixture::Asm a(call_ea);
    if (c.fpu_noise) {
        a.u8(0xD9);
        a.u8(0xE8);  // fld1
    }
    a.pushImm(static_cast<std::uint32_t>(c.stride));  // a7
    a.pushImm(static_cast<std::uint32_t>(c.i0));      // a6
    a.pushImm(static_cast<std::uint32_t>(c.count));   // a5
    a.pushImm(dst_ptr);                               // a4
    a.pushImm(src_ea);                                // a3
    a.pushImm(state_ea);                              // a2
    a.pushImm(0u);                                    // a1：guest 全程未读
    a.callRel(0x004CCC60u);
    a.ret();
    if (!fx.m.placeCode(call_ea, a.b.data(), a.b.size(), &fx.err)) return out;
    fx.rearmStack();
    // ★ 两臂都从同一份 x87 控制字 / 状态字出发（见 AffineCase 的字段注释）。
    // ★ 还要把**宿主全局**的 softfloat 舍入/异常位复位：softfloat 的 `roundingMode` /
    //   `roundingPrecision` 是进程级全局量，上一臂（解释器）跑完会把它留在**本用例的控制字**上
    //   ⇒ 若不复位，"override 忘了 apply 控制字"会继承同一份残值而**静默相同**，控制字用例就白写了。
    lift::x87ApplyControlWord(0x037Fu);
    lift::x87ClearExceptionFlags();
    fx.m.cpu().fpu.control = c.control;
    fx.m.cpu().fpu.status = c.entry_status;
    fx.runCode(call_ea);
    if (!fx.ok) return out;
    out.valid = true;
    out.eax = fx.m.cpu().eax();
    out.esp = fx.m.cpu().esp();
    out.eip = fx.m.cpu().eip;
    out.ebx = fx.m.cpu().ebx();
    out.esi = fx.m.cpu().esi();
    out.edi = fx.m.cpu().edi();
    out.ebp = fx.m.cpu().ebp();
    out.fpu_status = fx.m.cpu().fpu.status;
    out.fpu_control = fx.m.cpu().fpu.control;
    out.fpu_tag = fx.m.cpu().fpu.tag;
    out.fpu_top = fx.m.cpu().fpu.top;
    const std::uint8_t* p = fx.m.mem().host(c.alias ? src_ea : dst_ea);
    out.buf.assign(p, p + kFloats * 4u);
    if (with_override) {
        const lift::NativeOverride* e = fx.m.overrides().at(0);
        out.hits = e->hits;
        out.fallbacks = e->fallbacks;
    }
    return out;
}

// ---- sub_4367B0（退出写存档的 LZSS Encode）：真字节两臂差分 ----
// 判据（缺一条都不算）：同一份夹具（对象预填 0xCC + 同一输入 + 同一个新堆）跑**解释器臂**与
// **override 臂**，比 ①`EAX`（= 出缓冲 guest EA）②`ESP` ③`EIP` ④`info[0..2]`（`a3[1]` 是本单的
// `<待核>`）⑤`out[0..comp)` 逐字节 ⑥**整个 `0xD430` 对象**逐字节。
// ★ 为什么对象要预填 0xCC：guest **不写** `text_buf[4078+len .. 4095]` 的尾巴、`text_buf[4096..4112]`
//   的未写部分、`dad[4096]`、`lson[4096]`、`rson[0..4096]`、以及（`len==0` 时）`match_pos/match_len`
//   ⇒ 不预填时这些字节是**堆里上一次留下的垃圾**，两臂的堆历史不同就会"差在任何地方"。
// ★ 为什么两臂各用一个**全新** `Machine` + 同一个 `_crtheap` 预置：解释器臂的首次 `_malloc` 会走
//   `_heap_init`（`0x4EA529`）并多动堆状态 ⇒ 出缓冲指针会稳定地差一点（测试形状的错，不是实现的错）。
struct LzssCase {
    const char* name;
    std::vector<std::uint8_t> input;
};

struct LzssResult {
    bool valid = false;
    std::uint32_t eax = 0, esp = 0, eip = 0, ebx = 0, esi = 0, edi = 0, ebp = 0;
    std::uint32_t info[3] = {0, 0, 0};
    std::vector<std::uint8_t> obj;
    std::vector<std::uint8_t> out;
    std::uint64_t hits = 0, fallbacks = 0;
    // ★ `GuestHeap::allocCount()`：回退臂必须与解释器臂**同一份堆账**（证明 override 没有在旁路
    //   自己分配 / 释放 —— 它只是 decline，然后由解释器重放 `delete[]` + `new[]`）。
    std::uint32_t alloc_count = 0;
};

// `preset_out_bytes != 0` ⇒ 先在这个 guest 堆上分配一块"旧出缓冲"、写哨兵、把 EA 写进
// `[this+0xD42C]` —— 这是 **复用对象** 形状（`0x4367DA` 读到非 0 ⇒ guest 会先 `delete[]` 0x4367E6
// 再 `new[]` 0x436801）。override 的准入第 5 条只接管"新对象"⇒ 这条形状必须**回退**。
LzssResult runLzss(const std::string& exe, const std::vector<std::uint8_t>& input, bool with_override,
                   const char* tag, std::uint32_t preset_out_bytes = 0u) {
    constexpr std::uint32_t kObjBytes = 0xD430u;
    constexpr std::uint32_t kSelf = 0x08000000u;
    constexpr std::uint32_t kInfo = 0x08100000u;
    constexpr std::uint32_t kIn = 0x08200000u;
    constexpr std::uint32_t kCall = 0x08300000u;
    constexpr std::uint32_t kCrtheap = 0x0055E920u;  // `_crtheap`（`0x4EA529 cmp dword [0x55E920],0`）
    LzssResult out;
    lift::Machine m;
    std::string err;
    if (!m.initBare(&err)) return out;
    if (!m.loadImage(exe, &err)) return out;
    if (!m.prepareStack(&err)) return out;
    lift::registerKernel32Shims(m.shims());
    // ★ 让 guest 的 `new[] → _malloc → HeapAlloc` / `delete[] → _free → HeapFree` 走得通：把这两个
    //   导入的 **IAT 槽**改指到我们装的桩（不 patch guest 字节；只改 IAT 里的那两个 dword）。
    //   ★ 为什么 `HeapFree` 也要绑：`initBare + loadImage + prepareStack` 这条夹具路径**没有**跑
    //     `Machine::prepare()`（那里才会 `bind()` + `installShim`）⇒ 未绑定的 IAT 槽里还是镜像原值，
    //     guest 一调就跳进数据段（0x4367E6 的 `delete[]` 正好需要它）。
    auto bindHeapImport = [&](const char* api, std::uint32_t stub) {
        lift::ShimDef* d = m.shims().findMutable("kernel32", api);
        if (d == nullptr) {
            std::fprintf(stderr, "  [lzss %s] 没有 kernel32!%s shim\n", tag, api);
            return false;
        }
        d->stub_ea = stub;
        m.cache().installShim(stub, d->id);
        std::uint32_t slot = 0;
        for (const lift::PeImportInfo& imp : m.image().imports) {
            if (imp.name == api) slot = m.image().image_base + imp.iat_rva;
        }
        if (slot == 0) {
            std::fprintf(stderr, "  [lzss %s] 导入表里找不到 %s 的 IAT 槽\n", tag, api);
            return false;
        }
        m.mem().wr32(slot, stub);
        return true;
    };
    if (!bindHeapImport("HeapAlloc", 0x7FF90000u)) return out;
    if (!bindHeapImport("HeapFree", 0x7FF90100u)) return out;
    if (!m.heap().init(&m.mem(), m.layout().heap_base, 0x00400000u)) return out;
    if (!m.mem().committed(kCrtheap, 4u)) {
        std::fprintf(stderr, "  [lzss %s] 0x55E920 不在已提交页（_crtheap 定位失效）\n", tag);
        return out;
    }
    m.mem().wr32(kCrtheap, m.cpu().shim.heap_handle);  // 非 0 ⇒ 跳过 `_heap_init`
    // 夹具：对象（0xCC 预填）+ info（12 B）+ 输入 + 调用桩
    std::vector<std::uint8_t> obj(kObjBytes, 0xCC);
    if (!m.placeData(kSelf, obj.data(), obj.size(), &err)) return out;
    std::vector<std::uint8_t> info(12u, 0);
    if (!m.placeData(kInfo, info.data(), info.size(), &err)) return out;
    const std::uint32_t in_ea = input.empty() ? 0u : kIn;
    if (!input.empty() && !m.placeData(kIn, input.data(), input.size(), &err)) return out;
    m.mem().wr32(kInfo + 0u, static_cast<std::uint32_t>(input.size()));
    m.mem().wr32(kInfo + 4u, 0xDEADBEEFu);
    m.mem().wr32(kInfo + 8u, 0xDEADBEEFu);
    m.mem().wr32(kSelf + 0xD42Cu, 0u);  // "新对象"：out == 0（准入第 5 条；guest 也会 delete[] 它）
    if (preset_out_bytes != 0u) {
        const std::uint32_t old_out = m.heap().alloc(preset_out_bytes, /*zero=*/false);
        if (old_out == 0u) {
            std::fprintf(stderr, "  [lzss %s] 预置旧出缓冲失败（guest 堆申请不到）\n", tag);
            return out;
        }
        std::vector<std::uint8_t> sentinel(preset_out_bytes, 0x5A);
        std::memcpy(m.mem().host(old_out), sentinel.data(), sentinel.size());
        m.mem().wr32(kSelf + 0xD42Cu, old_out);
    }
    if (with_override) {
        if (!m.overrides().add(0x004367B0u, "refLzss", lift::GuestConv::Thiscall, 8, lift::nativeLzss4367B0,
                               &err)) {
            return out;
        }
    }
    lift_fixture::Asm a(kCall);
    a.u8(0xB9);
    a.u32(kSelf);
    a.pushImm(kInfo);   // a3（arg_4）
    a.pushImm(in_ea);   // a2（arg_0）
    a.callRel(0x004367B0u);
    a.ret();
    if (!m.placeCode(kCall, a.b.data(), a.b.size(), &err)) return out;
    m.cpu().esp() = m.layout().stack_top - 4u;
    m.mem().wr32(m.cpu().esp(), 0xFFFFFFFFu);
    m.cpu().eip = kCall;
    lift::RunOptions o;
    o.until_ea = 0xFFFFFFFFu;
    o.max_insns = 400000000ull;  // 150,068 B 的解释器臂要 ~1.3e8 条（实测 131,104,825）
    const lift::RunResult r = m.run(o);
    if (r.reason != lift::StopReason::UntilEa) {
        std::fprintf(stderr, "  [lzss %s] 运行停止：%s — %s\n", tag, lift::stopReasonName(r.reason),
                     r.detail.c_str());
        return out;
    }
    out.valid = true;
    out.eax = m.cpu().eax();
    out.esp = m.cpu().esp();
    out.eip = m.cpu().eip;
    out.ebx = m.cpu().ebx();
    out.esi = m.cpu().esi();
    out.edi = m.cpu().edi();
    out.ebp = m.cpu().ebp();
    for (int i = 0; i < 3; ++i) out.info[i] = m.mem().rd32(kInfo + static_cast<std::uint32_t>(i) * 4u);
    const std::uint8_t* po = m.mem().host(kSelf);
    out.obj.assign(po, po + kObjBytes);
    if (out.eax != 0u && out.info[2] != 0u) {
        const std::uint8_t* pb = m.mem().host(out.eax);
        out.out.assign(pb, pb + out.info[2]);
    }
    out.alloc_count = m.heap().allocCount();
    if (with_override) {
        const lift::NativeOverride* e = m.overrides().at(0);
        out.hits = e->hits;
        out.fallbacks = e->fallbacks;
    }
    return out;
}

// ---- `__ftol2_sse`（`0x4E82C0`）的夹具：x87 `st(0)` 由**夹具自己**用 `fld` 压进去 ----
// ★ 为什么输入走 guest 指令（`fld qword` / `fld tbyte`）而不是直接写 `fpu.regs[]`：两臂必须从
//   **同一条 guest 路径**进入被测函数（写 regs 会绕过 tag / TOP / `markValid` 的语义），
//   而且 `fld tbyte` 是唯一能造出"double 表示不出"的 64 位有效位输入的通道。
struct FtolCase {
    const char* name = "";
    bool ext80 = false;              // true ⇒ 用 `fld tbyte [kInEa]`（前 10 字节有效）
    std::uint64_t f64 = 0;           // ext80 == false 时的输入位型（`fld qword`）
    std::uint8_t t80[10] = {};       // ext80 == true 时的 10 字节
    std::uint16_t entry_status = 0;  // 入口状态字（C0..C3 必须原样保留）
    bool top_noise = false;          // 先垫一层活值 ⇒ 被测函数入口 TOP != 0
    std::uint32_t flag = 0;          // `[0x55F2E8]` 的值（≠0 ⇒ 必须回退）
    bool skip_base = false;          // 真机走 `cvttsd2si`（解释器解不出）⇒ 只跑 override 臂
    bool want_decline = false;
    std::uint32_t want_eax = 0, want_edx = 0;
};

struct FtolResult {
    bool valid = false;
    std::uint32_t eax = 0, edx = 0, esp = 0, eip = 0;
    std::uint16_t fpu_status = 0, fpu_control = 0, fpu_tag = 0;
    std::uint8_t fpu_top = 0;
    std::uint8_t scratch[0x20] = {};  // `[E, E+0x20)`（E = align16(入口 esp − 0x24)），两臂都预填 0x5A
    std::uint64_t hits = 0, fallbacks = 0;
    std::uint64_t decline_sse2 = 0;   // `declines[FtolWhy::Sse2Path]`
};

FtolResult runFtol(const std::vector<std::uint8_t>& code, const FtolCase& c, bool with_override) {
    FtolResult out;
    lift_fixture::Fixture fx;
    if (!fx.ok) return out;
    constexpr std::uint32_t kFlagEa = 0x0055F2E8u;  // CRT 的 x87→SSE 选路开关
    constexpr std::uint32_t kInEa = 0x00450000u;
    constexpr std::uint32_t kCallEa = 0x00402000u;
    std::vector<std::uint8_t> in(16u, 0);
    std::memcpy(in.data(), &c.f64, 8);
    if (c.ext80) std::memcpy(in.data(), c.t80, 10);
    if (!fx.m.placeData(kFlagEa, &c.flag, 4u, &fx.err)) return out;
    if (!fx.m.placeData(kInEa, in.data(), in.size(), &fx.err)) return out;
    if (!fx.m.placeCode(0x004E82C0u, code.data(), code.size(), &fx.err)) return out;
    if (with_override) {
        std::string err;
        if (!fx.m.overrides().add(0x004E82C0u, "ref_ftol", lift::GuestConv::Cdecl, 0, lift::nativeFtol2Sse, &err))
            return out;
    }
    lift_fixture::Asm a(kCallEa);
    if (c.top_noise) {
        // ★ 只压不弹：两次 `fld1` 之后 `st(0)` 落在**物理槽 5**（TOP=5、tag[0] 是 empty）
        //   ⇒ 若闸门写成 `empty(0)` 就会误判"st(0) 空"而回退（复审 H2）。⛔ 不用 `fstp st(i)`：
        //   Zydis 把 `D9 D8+i` 报成 `fstpnce`，本线的助记符表里没有那一条。
        a.u8(0xD9);
        a.u8(0xE8);  // fld1
        a.u8(0xD9);
        a.u8(0xE8);  // fld1
    }
    if (c.ext80) {
        a.u8(0xDB);
        a.u8(0x2D);  // fld tbyte ptr [disp32]
        a.u32(kInEa);
    } else {
        a.u8(0xDD);
        a.u8(0x05);  // fld qword ptr [disp32]
        a.u32(kInEa);
    }
    a.callRel(0x004E82C0u);
    a.ret();
    if (!fx.m.placeCode(kCallEa, a.b.data(), a.b.size(), &fx.err)) return out;
    fx.rearmStack();
    // 被测函数的栈 scratch：入口 esp = 调用前的 esp − 4（`call` 压了返回地址），
    // 然后 `push ebp / sub esp,20h / and esp,-10h` ⇒ E = align16(入口 esp − 0x24)。
    const std::uint32_t esp_entry = fx.m.cpu().esp() - 4u;
    const std::uint32_t frame = (esp_entry - 0x24u) & ~15u;
    for (std::uint32_t i = 0; i < 0x20u; ++i) fx.m.mem().wr8(frame + i, 0x5Au);
    // 宿主的 softfloat 舍入模式 / 粘性异常位是**进程级全局** ⇒ 两臂都必须从同一份缺省出发，
    // 否则"override 少 sync 一次"会继承上一臂的异常位而**静默相同**。
    lift::x87ApplyControlWord(0x037Fu);
    lift::x87ClearExceptionFlags();
    fx.m.cpu().fpu.control = 0x037Fu;
    fx.m.cpu().fpu.status = c.entry_status;
    fx.runCode(kCallEa);
    // ★ `skip_base` 档（`[0x55F2E8] != 0`）在回退之后由解释器重放 `cvttsd2si`，而本线的解码器
    //   没有那一条 ⇒ `runCode` 会响亮失败。那一档只断言"必须回退"，所以只要 override 真的被调用过
    //   就接受（计数器与 `declines[]` 仍然读得到）。
    if (!fx.ok && !c.skip_base) return out;
    out.valid = true;
    out.eax = fx.m.cpu().eax();
    out.edx = fx.m.cpu().edx();
    out.esp = fx.m.cpu().esp();
    out.eip = fx.m.cpu().eip;
    out.fpu_status = fx.m.cpu().fpu.status;
    out.fpu_control = fx.m.cpu().fpu.control;
    out.fpu_tag = fx.m.cpu().fpu.tag;
    out.fpu_top = fx.m.cpu().fpu.top;
    std::memcpy(out.scratch, fx.m.mem().host(frame), 0x20u);
    if (with_override) {
        const lift::NativeOverride* e = fx.m.overrides().at(0);
        out.hits = e->hits;
        out.fallbacks = e->fallbacks;
        out.decline_sse2 = e->declines[static_cast<std::size_t>(lift::FtolWhy::Sse2Path)];
    }
    return out;
}

// ---- `_memcpy`（`0x4E74B0`）的**重叠**差分：真字节两臂（解释器 vs override）----
//
// ★ 为什么这条只能放在 assets 档：`.lst` 把 `_memcpy` / `_memcpy_0` 记成 COLLAPSED
//   （`; [00000361 BYTES: COLLAPSED FUNCTION _memcpy]`，**没有指令文本**）⇒ "重叠时 guest 到底往
//   哪个方向搬"只有**真字节**能回答。`tests/native_override.cpp` 的 `kMemcpyRef` / `kMemcpyBackRef`
//   只是同一形状的最小复述，不是 oracle。
// ★ 真字节里的分流（EA 是锚）：
//   · `0x4E74C4 cmp edi,esi` / `0x4E74C6 jbe 0x4E74F4` ⇒ `dst <= src`：前向（`0x4E7508 F3 A5 rep movsd`）；
//   · `0x4E74C8 cmp edi,eax`（EAX = src+n）/ `0x4E74CA jb 0x4E7670` ⇒ `dst > src` 且重叠：
//     后向（`0x4E768B FD std` / `0x4E768C F3 A5 rep movsd` / `0x4E768E FC cld`）。
// ★ `[0x55F2E8]` 必须**置 0 且提交**：非 0 时 `0x4E74DF jz` 不跳，会走 `0x4E74EF jmp 0x4F1120`
//   （SSE2 支，**不在**这 0x361 字节里）；本线 `shimIsProcessorFeaturePresent` 恒 0 ⇒ 生产里同样走
//   通用支。两臂看到的是同一条 `0x55F2E8`，所以这条闸门不是"给测试开小灶"。
struct MemcpyAssetsCase {
    std::string name;
    std::uint32_t n;
    std::uint32_t dst_off;
    std::uint32_t src_off;
};

struct MemcpyAssetsResult {
    bool valid = false;
    std::uint32_t eax = 0, esp = 0, esi = 0, edi = 0, ebp = 0, eip = 0;
    std::vector<std::uint8_t> buf;
    std::uint64_t hits = 0, fallbacks = 0;
};

constexpr std::uint32_t kMemcpyBufBytes = 4096u + 64u;
constexpr std::uint32_t kMemcpyEa = 0x004E74B0u;
constexpr std::uint32_t kMemcpyBufEa = 0x00450000u;

MemcpyAssetsResult runMemcpyAssets(const std::vector<std::uint8_t>& code, const MemcpyAssetsCase& c,
                                  bool with_override) {
    MemcpyAssetsResult out;
    lift_fixture::Fixture fx;
    if (!fx.ok) return out;
    constexpr std::uint32_t kSse2FlagEa = 0x0055F2E8u;  // CRT 的 SSE 选路开关（本线恒 0）
    const std::uint32_t flag = 0u;
    if (!fx.m.placeData(kSse2FlagEa, &flag, 4u, &fx.err)) return out;
    std::vector<std::uint8_t> init(kMemcpyBufBytes);
    for (std::uint32_t i = 0; i < kMemcpyBufBytes; ++i)
        init[i] = static_cast<std::uint8_t>((i * 37u + (i >> 3) * 11u + 101u) & 0xFFu);
    if (!fx.m.placeData(kMemcpyBufEa, init.data(), init.size(), &fx.err)) return out;
    if (!fx.m.placeCode(kMemcpyEa, code.data(), code.size(), &fx.err)) return out;
    if (with_override) {
        std::string err;
        if (!fx.m.overrides().add(kMemcpyEa, "ref_memcpy", lift::GuestConv::Cdecl, 12, lift::nativeMemcpy, &err))
            return out;
    }
    lift_fixture::Asm a(0x00402000u);
    a.pushImm(c.n);                                   // 第 3 个实参
    a.pushImm(kMemcpyBufEa + c.src_off);              // 第 2 个
    a.pushImm(kMemcpyBufEa + c.dst_off);              // 第 1 个
    a.callRel(kMemcpyEa);
    a.addEspImm8(12u);  // cdecl：调用方清栈（3 个 dword）
    a.ret();
    if (!fx.m.placeCode(0x00402000u, a.b.data(), a.b.size(), &fx.err)) return out;
    fx.rearmStack();
    fx.runCode(0x00402000u);
    if (!fx.ok) return out;
    out.valid = true;
    out.eax = fx.m.cpu().eax();
    out.esp = fx.m.cpu().esp();
    out.esi = fx.m.cpu().esi();
    out.edi = fx.m.cpu().edi();
    out.ebp = fx.m.cpu().ebp();
    out.eip = fx.m.cpu().eip;
    const std::uint8_t* p = fx.m.mem().host(kMemcpyBufEa);
    out.buf.assign(p, p + kMemcpyBufBytes);
    if (with_override) {
        const lift::NativeOverride* e = fx.m.overrides().at(0);
        out.hits = e->hits;
        out.fallbacks = e->fallbacks;
    }
    return out;
}

int main() {
    const std::string exe = envValue("LIFT_AGE_EXE");
    if (exe.empty()) LIFT_SKIP("没有 LIFT_AGE_EXE（CMake 的 assets 档会设它）");
    ImageCode img;
    if (!img.load(exe)) LIFT_SKIP("载入 AGE.EXE 失败（缺资产 ⇒ 不是红灯）");
    const std::vector<std::uint8_t> code401000 = img.at(0x00401000u, 0x100u);
    const std::vector<std::uint8_t> code429020 = img.at(0x00429020u, 0x100u);
    const std::vector<std::uint8_t> code48AE90 = img.at(0x0048AE90u, 0x200u);
    // `_memcpy`：长度 0x361 取自 `.lst` 对 EA 0x4E74B0 的折叠声明（`[00000361 BYTES: COLLAPSED FUNCTION _memcpy]`）。
    const std::vector<std::uint8_t> code_memcpy = img.at(kMemcpyEa, 0x361u);

    // ---- ①b `_memcpy` 的**重叠**：真字节两臂（判据见 `runMemcpyAssets` 上方）----
    // ★ oracle 的方向由 guest 自己的判据决定（`0x4E74CA`），不在这里另写一份：两臂都是**同一份真字节**
    //   跑出来的，所以"哪一类该后向"根本不进本测试的期望值 —— 这是把结论交给真字节。
    // ★ 另一条断言（`want`）单独把**结论**钉住："重叠 ⇒ memmove 语义（等价于经临时缓冲中转）"。
    //   没有它，两臂可能一起错（例如把 `std::memmove` 当成 oracle 的唯一实现）；差分只钉"两臂一致"。
    {
        auto mk = [](const std::string& name, std::uint32_t n, std::uint32_t d, std::uint32_t s) {
            MemcpyAssetsCase c;
            c.name = name;
            c.n = n;
            c.dst_off = d;
            c.src_off = s;
            return c;
        };
        std::vector<MemcpyAssetsCase> cases;
        const std::uint32_t sizes[] = {0u, 1u, 3u, 4u, 7u, 8u, 64u, 4096u};
        for (std::uint32_t n : sizes) {
            const std::string tag = std::to_string(n);
            cases.push_back(mk("memcpy-overlap/same-n" + tag, n, 0u, 0u));
            cases.push_back(mk("memcpy-overlap/dst<src-n" + tag, n, 0u, 1u));
            cases.push_back(mk("memcpy-overlap/dst>src-n" + tag, n, 1u, 0u));
        }
        cases.push_back(mk("memcpy-overlap/dst<src-d8-n64", 64u, 0u, 8u));
        cases.push_back(mk("memcpy-overlap/dst>src-d8-n64", 64u, 8u, 0u));
        // 对照：**不重叠**（dst = src+72 > src+n）—— 这一类走原来的 `std::memcpy` 快路径，判据是"仍然对"。
        cases.push_back(mk("memcpy-overlap/no-overlap-n64", 64u, 72u, 0u));
        std::vector<std::uint8_t> init(kMemcpyBufBytes);
        for (std::uint32_t i = 0; i < kMemcpyBufBytes; ++i)
            init[i] = static_cast<std::uint8_t>((i * 37u + (i >> 3) * 11u + 101u) & 0xFFu);
        for (const MemcpyAssetsCase& c : cases) {
            lift_test::g_current_case = c.name.c_str();
            const MemcpyAssetsResult base = runMemcpyAssets(code_memcpy, c, false);
            const MemcpyAssetsResult ovr = runMemcpyAssets(code_memcpy, c, true);
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
            // ★ 本单的判据：这一类重叠**不再回退**
            LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(ovr.hits), 1u);
            LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(ovr.fallbacks), 0u);
            // ★ 结论：重叠 ⇒ memmove 语义（经临时缓冲中转的定义，与"哪个方向"无关）
            std::vector<std::uint8_t> want = init;
            if (c.n != 0u && c.dst_off != c.src_off) {
                std::vector<std::uint8_t> tmp(c.n);
                std::memcpy(tmp.data(), init.data() + c.src_off, c.n);
                std::memcpy(want.data() + c.dst_off, tmp.data(), c.n);
            }
            LIFT_CHECK(want == base.buf);
            if (want != base.buf) {
                std::size_t first = 0;
                while (first < want.size() && want[first] == base.buf[first]) ++first;
                std::fprintf(stderr, "  [%s] guest 结果不是 memmove：首个不同 off=%zu want=%02X got=%02X\n",
                             c.name.c_str(), first, want[first], base.buf[first]);
            }
        }
    }

    // ---- ① sub_429020：真字节，解释器 vs override ----
    {
        const std::vector<MapCase> cases = {
            {{"alpha", "", "", ""}, "alpha", 0u},          // 直接命中
            {{"zero", "one", "two", ""}, "two", 0u},       // 线性探测命中
            {{"zero", "one", "two", ""}, "zzz", 0u},       // 探测到空槽 ⇒ -1
            {{"0123456789abcdefghij", "", "", ""}, "0123456789abcdefghij", 0u},  // 堆串
            {{"abcd", "", "", ""}, "ab", 0u},              // 长度不等（前缀相同）⇒ -1
            {{"", "a", "", ""}, "a", 0u},                  // 第一个槽空 ⇒ -1
        };
        for (const MapCase& c : cases) {
            const MapResult base = runMap(code401000, code429020, c, false);
            const MapResult ovr = runMap(code401000, code429020, c, true);
            LIFT_CHECK(base.valid && ovr.valid);
            if (!base.valid || !ovr.valid) continue;
            LIFT_CHECK_EQ_U32(ovr.eax, base.eax);
            LIFT_CHECK_EQ_U32(ovr.esp, base.esp);
            LIFT_CHECK_EQ_U32(ovr.eip, base.eip);
            LIFT_CHECK_EQ_U32(ovr.ebx, base.ebx);
            LIFT_CHECK_EQ_U32(ovr.esi, base.esi);
            LIFT_CHECK_EQ_U32(ovr.edi, base.edi);
            LIFT_CHECK(ovr.table == base.table);
            if (ovr.eax != base.eax) {
                std::fprintf(stderr, "  [429020 key=%s] eax=%08X/%08X\n", c.key.c_str(), base.eax, ovr.eax);
            }
        }
    }

    // ---- ② sub_48AE90：真字节，解释器 vs override；整张纹理逐字节 + 锁记账 ----
    {
        struct FillCase {
            std::uint32_t tex_bit;
            std::uint32_t w, h, format;
            std::int32_t l, t, r, b;
            std::uint32_t color;
            std::int32_t alpha;
        };
        const std::vector<FillCase> cases = {
            {32u, 4u, 3u, lift::d3d9::kFmtA8R8G8B8, 1, 1, 3, 3, 0x00112233u, 0x40},   // 32bpp · alpha>=0
            {32u, 4u, 3u, lift::d3d9::kFmtA8R8G8B8, 0, 0, 4, 1, 0x00ABCDEFu, -1},     // 32bpp · 保留 alpha
            {16u, 4u, 3u, lift::d3d9::kFmtA4R4G4B4, 1, 0, 3, 3, 0x00123456u, 0xF0},   // 16bpp · alpha>=0
            {16u, 4u, 3u, lift::d3d9::kFmtA4R4G4B4, 0, 0, 3, 2, 0x000F0F0Fu, -1},     // 16bpp · 保留 alpha
            // ★ 交叉：纹理 bpp ≠ dword_5518C0（store 宽度 / pBits 的 left 偏移是两套量纲）。
            //   引擎里二者同源（同一个 display:TextureBit），这里只是把"两套量纲分开算"钉住。
            {32u, 8u, 4u, lift::d3d9::kFmtA4R4G4B4, 1, 0, 3, 2, 0x00112233u, 0x40},
            {16u, 8u, 4u, lift::d3d9::kFmtA8R8G8B8, 1, 0, 3, 2, 0x00112233u, 0x40},
        };
        for (std::size_t i = 0; i < cases.size(); ++i) {
            const FillCase& c = cases[i];
            char name[64];
            std::snprintf(name, sizeof(name), "case%zu", i);
            const FillResult base =
                runFill(code48AE90, name, c.tex_bit, c.w, c.h, c.format, c.l, c.t, c.r, c.b, c.color, c.alpha,
                        false);
            const FillResult ovr =
                runFill(code48AE90, name, c.tex_bit, c.w, c.h, c.format, c.l, c.t, c.r, c.b, c.color, c.alpha,
                        true);
            LIFT_CHECK(base.valid && ovr.valid);
            if (!base.valid || !ovr.valid) continue;
            LIFT_CHECK_EQ_U32(ovr.eax, base.eax);
            LIFT_CHECK_EQ_U32(ovr.esp, base.esp);
            LIFT_CHECK_EQ_U32(ovr.eip, base.eip);
            LIFT_CHECK_EQ_U32(ovr.locks, base.locks);
            LIFT_CHECK_EQ_U32(ovr.version, base.version);
            LIFT_CHECK(ovr.locked == base.locked);
            LIFT_CHECK(ovr.arena == base.arena);
            if (ovr.arena != base.arena) {
                std::size_t diff = 0;
                for (std::size_t k = 0; k < ovr.arena.size() && k < base.arena.size(); ++k)
                    if (ovr.arena[k] != base.arena[k]) {
                        diff = k;
                        break;
                    }
                std::fprintf(stderr, "  [48AE90 %s] arena 首个差异 @%zu: %02X vs %02X\n", name, diff,
                             base.arena[diff], ovr.arena[diff]);
            }
        }
    }

    // ---- ③ sub_46D9F0：真字节，解释器 vs override；目标表面逐字节 + EAX/ESP/寄存器 ----
    // ★ 这是 G1（纯算术）之外的**外部 oracle**：同一份合成 guest 内存，让**真机器码**跑一遍，
    //   再让 native 实现跑一遍 —— 覆盖 16/32 位 × 1bpp/8bpp × 65/17 级 × 覆写/加权/裁剪。
    //   纯核自己算错（例如整数取整顺序、半字节落位、字节反序）在这里会当场红。
    {
        const std::vector<std::uint8_t> code46CE20 = img.at(0x0046CE20u, 0x80u);
        const std::vector<std::uint8_t> code46D9F0 = img.at(0x0046D9F0u, 0x4E0u);
        struct NamedGlyphCase {
            const char* name;
            GlyphCase c;
        };
        std::vector<NamedGlyphCase> cases;
        auto add = [&](const char* name, const GlyphCase& c) { cases.push_back({name, c}); };
        // 16 位 × 1bpp 掩码（每行只有 col0 置位）
        {
            GlyphCase c;
            c.src = {0x80, 0, 0, 0, 0x80, 0, 0, 0};
            add("16/mask1", c);
        }
        // 16 位 × 8bpp 65 级（0 / 1 / 32 / 64 四档，含"最低一档被 >>4 吃掉"）
        {
            GlyphCase c;
            c.level8 = true;
            c.levels65 = true;
            c.src = {0, 1, 32, 64, 0, 1, 32, 64};
            add("16/gray8-l65", c);
        }
        // 16 位 × 8bpp 17 级（0 / 1 / 3 / 16）
        {
            GlyphCase c;
            c.level8 = true;
            c.levels65 = false;
            c.src = {0, 1, 3, 16, 0, 1, 3, 16};
            add("16/gray4-l17", c);
        }
        // 16 位 × 目标 A = 0（覆写）与 A > 覆盖值（max）
        {
            GlyphCase c;
            c.level8 = true;
            c.levels65 = true;
            c.src = {32, 32, 32, 32, 32, 32, 32, 32};
            c.dst_fill = 0x0A5C0A5Cu;  // A = 0
            add("16/dstA0", c);
            c.dst_fill = 0xDA5CDA5Cu;  // A = 0xD
            add("16/dstAmax", c);
        }
        // 16 位 × 裁剪（左 / 右 / 下 越界；原点偏移）
        {
            GlyphCase c;
            c.src = {0x80, 0, 0, 0, 0x80, 0, 0, 0};
            c.x = -3;
            add("16/clip-left", c);
            c.x = 4;
            add("16/clip-right", c);
            c.x = 1;
            c.y = 2;
            add("16/clip-bottom", c);
            c.y = -1;
            add("16/clip-top", c);
            c.x = 0;
            c.y = 0;
            c.ox = 2;
            c.oy = 1;
            add("16/origin", c);
        }
        // 16 位 × 空字形（w=0 / h=0）与空裁剪矩形
        {
            GlyphCase c;
            c.src = {0, 0, 0, 0};
            c.w = 0;
            add("16/w0", c);
            c.w = 2;
            c.h = 0;
            add("16/h0", c);
            c.h = 2;
            c.src = {0x80, 0, 0, 0, 0x80, 0, 0, 0};
            c.r = 0;
            c.b = 0;
            add("16/empty-clip", c);
        }
        // 32 位 × 1bpp / 8bpp 65 级 / 17 级（2 px/行）
        {
            GlyphCase c;
            c.tex_bit = 32u;
            c.stride = 4u;
            c.dst_bytes = 32u;
            c.src = {0xC0, 0, 0, 0, 0xC0, 0, 0, 0};
            add("32/mask1", c);
            c.level8 = true;
            c.levels65 = true;
            c.src = {0, 32, 64, 64, 0, 32, 64, 64};
            add("32/gray8-l65", c);
            c.levels65 = false;
            c.src = {0, 9, 16, 16, 0, 9, 16, 16};
            add("32/gray4-l17", c);
            c.levels65 = true;         // 下面是"中覆盖"档：raw=32 只在 65 级里合法（17 级满值 = 16）
            c.dst_fill = 0x40102030u;  // A = 0x40 ⇒ 加权 + max
            c.src = {32, 32, 32, 32, 32, 32, 32, 32};
            add("32/dstAmax", c);
            c.dst_fill = 0x00102030u;  // A = 0 ⇒ 覆写
            add("32/dstA0", c);
            c.dst_fill = 0xC0102030u;  // A = 0xC0 > 覆盖值 ⇒ max 才是 0xC0
            add("32/dstAover", c);
        }
        // 级数越界（raw = 200 > 65 级的满值 64）⇒ override 必须 **Declined**（回退解释器，结果仍一致）
        {
            GlyphCase c;
            c.level8 = true;
            c.levels65 = true;
            c.src = {200, 200, 200, 200, 200, 200, 200, 200};
            add("16/level-oob", c);
        }
        for (const NamedGlyphCase& nc : cases) {
            lift_test::g_current_case = nc.name;
            const GlyphResult base = runGlyph(code46CE20, code46D9F0, nc.c, false);
            const GlyphResult ovr = runGlyph(code46CE20, code46D9F0, nc.c, true);
            LIFT_CHECK(base.valid && ovr.valid);
            if (!base.valid || !ovr.valid) continue;
            LIFT_CHECK_EQ_U32(ovr.eax, base.eax);
            LIFT_CHECK_EQ_U32(ovr.esp, base.esp);
            LIFT_CHECK_EQ_U32(ovr.eip, base.eip);
            LIFT_CHECK_EQ_U32(ovr.ebx, base.ebx);
            LIFT_CHECK_EQ_U32(ovr.esi, base.esi);
            LIFT_CHECK_EQ_U32(ovr.edi, base.edi);
            LIFT_CHECK_EQ_U32(ovr.ebp, base.ebp);
            LIFT_CHECK(ovr.dst == base.dst);
            const bool oob = std::strcmp(nc.name, "16/level-oob") == 0;
            LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(ovr.hits), oob ? 0u : 1u);
            LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(ovr.fallbacks), oob ? 1u : 0u);
            if (ovr.dst != base.dst) {
                std::size_t diff = 0;
                for (std::size_t k = 0; k < ovr.dst.size() && k < base.dst.size(); ++k)
                    if (ovr.dst[k] != base.dst[k]) {
                        diff = k;
                        break;
                    }
                std::fprintf(stderr, "  [46D9F0 %s] dst 首个差异 @%zu: %02X vs %02X（eax %08X/%08X）\n",
                             nc.name, diff, base.dst[diff], ovr.dst[diff], base.eax, ovr.eax);
            }
        }
    }

    // ---- ④ sub_4CCC60：真字节，解释器 vs override；dst 逐字节 + 寄存器 + **x87 状态字** ----
    // ★ 这是本线**第一项 x87 override**：除目的内存/寄存器外，还要比 `fpu.status` / `tag` / `top` /
    //   `control` —— 算术顺序或 `fsin`/`fcos` 路径一偏，status 的 PE 位或顶层槽就会露馅。
    // ★ 纯核的静态判据在 pure 档（tests/native_override.cpp）；这里只做"同一份真字节两遍"。
    {
        const std::vector<std::uint8_t> code4CCC60 = img.at(0x004CCC60u, 0xD4u);
        struct NamedAffineCase {
            const char* name;
            AffineCase c;
            bool decline;
        };
        std::vector<NamedAffineCase> cases;
        auto addCase = [&](const char* name, const AffineCase& c, bool decline = false) {
            cases.push_back({name, c, decline});
        };

        AffineCase base;
        base.src = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
        addCase("30/stride2", base);

        AffineCase offset = base;
        offset.angle_deg = -45.5f;
        offset.radius = 0.75f;
        offset.flip_x = 1;
        offset.add4 = -3.25f;
        offset.add8 = 7.5f;
        offset.i0 = 1;
        offset.count = 4;
        offset.stride = 3;
        offset.src = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
        addCase("-45.5/stride3/i0=1", offset);

        AffineCase stride1 = base;
        stride1.angle_deg = 90.0f;
        stride1.radius = 2.0f;
        stride1.flip_y = 1;
        stride1.count = 5;
        stride1.stride = 1;  // 相邻两点重叠：后写赢
        addCase("90/stride1", stride1);

        AffineCase alias1 = stride1;
        alias1.alias = true;
        addCase("alias/stride1", alias1);

        AffineCase alias2 = base;
        alias2.alias = true;
        addCase("alias/stride2", alias2);

        AffineCase flags = base;
        flags.flip_x = 1;
        flags.flip_y = 1;
        flags.radius = -1.5f;
        flags.count = 3;
        flags.stride = 2;
        flags.src = {-1, 2.5f, -3.25f, 4.5f, -5, 6};
        addCase("flags-both/r<0", flags);

        AffineCase empty = base;
        empty.count = 2;
        empty.stride = 2;
        empty.i0 = 4;  // bound = 4 <= i0 ⇒ 空循环（EAX = inner 对象）
        empty.src = {1, 2, 3, 4};
        // ★ 空循环这一支**没有**快路径（序言仍要跑，见下一段的 `.lst` 依据）⇒ 必须交解释器重放。
        //   参数保持原样（`r=1`、30° = **精确序言**档）：它现在与下面两条一起验"回退"，数值上仍是
        //   一个独立数据点（这条的回退前状态字恰好是 0）。
        addCase("empty-loop", empty, true);

        // ★ 空循环**仍然执行序言**：`.lst` 的 `0x4CCC71 fld [eax+14h]` / `0x4CCC75 fmul flt_52F1B0` /
        //   `0x4CCC80 fsin` / `0x4CCC84 fcos` / `0x4CCCC0..0x4CCCCE` 的四系数乘加 /
        //   `0x4CCCD2..0x4CCCDB` 的取平移**全在 `0x4CCCDE jge` 之前** ⇒ 即使不进循环，guest 也会置
        //   `fpu.status` 的粘性异常位（PE/ES）并清 C0..C3。override 一旦在这条分支上"直接成功返回"，
        //   两者都会漏 ⇒ 这两条用例就是钉它的。
        //   · `empty-loop-inexact`：`radius = 1.234567f` ⇒ `(sx*r)*cos` 的 77 位积放不进 PC=80 ⇒ inexact；
        //   · `empty-loop-preset-cc`：入口 C3=1（`0x4000`，`fucomp` 之后的状态）⇒ `fsin`/`fcos` 会清 C0..C3。
        AffineCase emptyInexact = empty;
        emptyInexact.radius = 1.234567f;
        addCase("empty-loop-inexact", emptyInexact, true);  // bound<=i0 ⇒ 必须交解释器重放序言

        AffineCase emptyPresetCC = emptyInexact;
        emptyPresetCC.entry_status = 0x4000u;  // C3 = 1
        addCase("empty-loop-preset-cc", emptyPresetCC, true);

        AffineCase pick = base;
        pick.pick_o24 = true;  // `[state+0x24] ?: [state+0x20]` 的非零分支
        addCase("pick-o24", pick);

        // ★ 非默认控制字：`fldcw` 在语料里 94 处（例 `0x4046B2` / `0x46236A`）⇒ "按 `cpu.fpu.control`
        //   算"是真实不变量，且解释器对**每条** FP 指令都 `x87ApplyControlWord(cpu.fpu.control)`
        //   （`exec_fpu.cpp`）⇒ override 必须在第一次算术之前做同一件事。
        //   输入选**不可精确**的值：`radius = 1/3`、`angle_deg = 17` ⇒ 舍入模式 / 尾数位数一换，
        //   dst 的最后一位就变（默认控制字下这两条仍然绿）。
        //   ★ 0x077F：RC 位 [11:10] = 01 ⇒ round-down（向 −∞）；PC 位 [9:8] = 11 ⇒ 80 位（缺省）。
        //   ★ 0x027F：RC 位 = 00（最近偶数）；PC 位 = 10 ⇒ 双精度（53 位尾数）。SoftFloat 侧这条映射
        //     落到 `extF80_roundingPrecision = 64`（与解释器同一条 `x87ApplyControlWord`）⇒ 两臂一致。
        AffineCase roundDown = base;
        roundDown.control = 0x077Fu;  // RC = round-down
        roundDown.radius = 1.0f / 3.0f;
        roundDown.angle_deg = 17.0f;
        roundDown.src = {0.25f, -1.5f, 3.75f, -0.1f, 7.0f, 2.0f};
        addCase("rc/round-down", roundDown);

        AffineCase pc53 = roundDown;
        // ★ PC 位 [9:8] = 10 ⇒ 双精度（53 位尾数）；`x87ApplyControlWord` 把它映射到
        //   `extF80_roundingPrecision = 64`（= 53 位 significand，与解释器同一条映射）。
        pc53.control = 0x027Fu;
        // ★ 让 PC 这一维**真的可见**（否则它只验"两臂都用同一个控制字"，删掉 apply 也不会红）：
        //   `r = 1 + 2^-10`（11 位 significand）⇒ 四个系数 `cos*(1+2^-10)` 是 **63 位** significand
        //   （53 位的 sin/cos × (2^10+1)）⇒ 在 PC=53 下 inexact、PC=80（64 位尾数）下精确；
        //   而 `src` 全 0 使点积只剩平移项（f32 原值，精确）⇒ 两档的 `fpu.status`（PE 位）不同。
        pc53.radius = 1.0009765625f;  // 1 + 2^-10
        pc53.src = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
        pc53.add4 = 1.0f;
        pc53.add8 = 2.0f;
        addCase("pc/53-bit", pc53);

        AffineCase zero = base;
        zero.angle_deg = 0.0f;
        zero.radius = 2.0f;
        zero.add4 = 10.0f;
        zero.add8 = 20.0f;
        zero.count = 2;
        zero.stride = 2;
        zero.src = {2, 1, 3, 4};
        addCase("angle0", zero);

        AffineCase huge = base;
        huge.angle_deg = 1.0e30f;  // |θ| >= 2^63 ⇒ fsin 的 C2 分支 ⇒ override 必须 Declined
        addCase("huge-angle", huge, true);

        AffineCase noise = base;
        noise.fpu_noise = true;  // x87 栈非空 ⇒ guest 会走 push 溢出 ⇒ override 必须 Declined
        addCase("fpu-nonempty", noise, true);

        for (const NamedAffineCase& nc : cases) {
            lift_test::g_current_case = nc.name;
            const AffineResult b = runAffine(code4CCC60, nc.c, false);
            const AffineResult o = runAffine(code4CCC60, nc.c, true);
            LIFT_CHECK(b.valid && o.valid);
            if (!b.valid || !o.valid) continue;
            LIFT_CHECK_EQ_U32(o.eax, b.eax);
            LIFT_CHECK_EQ_U32(o.esp, b.esp);
            LIFT_CHECK_EQ_U32(o.eip, b.eip);
            LIFT_CHECK_EQ_U32(o.ebx, b.ebx);
            LIFT_CHECK_EQ_U32(o.esi, b.esi);
            LIFT_CHECK_EQ_U32(o.edi, b.edi);
            LIFT_CHECK_EQ_U32(o.ebp, b.ebp);
            LIFT_CHECK(o.fpu_control == b.fpu_control);
            LIFT_CHECK(o.fpu_tag == b.fpu_tag);
            LIFT_CHECK(o.fpu_top == b.fpu_top);
            LIFT_CHECK(o.fpu_status == b.fpu_status);
            LIFT_CHECK(o.buf == b.buf);
            LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(o.hits), nc.decline ? 0u : 1u);
            LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(o.fallbacks), nc.decline ? 1u : 0u);
            if (o.buf != b.buf) {
                std::size_t diff = 0;
                for (std::size_t k = 0; k < o.buf.size() && k < b.buf.size(); ++k)
                    if (o.buf[k] != b.buf[k]) {
                        diff = k;
                        break;
                    }
                std::fprintf(stderr, "  [4CCC60 %s] dst 首个差异 @%zu: %02X vs %02X（eax %08X/%08X）\n",
                             nc.name, diff, b.buf[diff], o.buf[diff], b.eax, o.eax);
            }
            if (o.fpu_status != b.fpu_status) {
                std::fprintf(stderr, "  [4CCC60 %s] fpu.status %04X vs %04X（tag %04X/%04X top %u/%u）\n",
                             nc.name, b.fpu_status, o.fpu_status, b.fpu_tag, o.fpu_tag,
                             static_cast<unsigned>(b.fpu_top), static_cast<unsigned>(o.fpu_top));
            }
        }
    }

    // ---- ⑤ sub_4367B0：真字节，解释器 vs override；整个 0xD430 + info[0..2] + EAX/ESP/EIP + out ----
    // ★ 输入档：4 KB / 150,068 B（真存档 payload 量级）各一档"可压"与一档"几乎不可压"（逼 give_up），
    //   外加平局定点向量（8 B）、空输入、17/18 的边界。⛔ 不断言具体 comp 数字（会随输入变）。
    {
        auto mkPattern = [](std::uint32_t n) {
            std::vector<std::uint8_t> v(n);
            for (std::uint32_t i = 0; i < n; ++i) v[i] = static_cast<std::uint8_t>("ABRACADABRA"[i % 11u]);
            return v;
        };
        auto mkNoise = [](std::uint32_t n) {
            std::vector<std::uint8_t> v(n);
            std::uint32_t x = 0x9E3779B9u;
            for (std::uint32_t i = 0; i < n; ++i) {
                x ^= x << 13;
                x ^= x >> 17;
                x ^= x << 5;
                v[i] = static_cast<std::uint8_t>(x & 0xFFu);
            }
            return v;
        };
        std::vector<LzssCase> cases;
        cases.push_back({"tie-8", {0x42, 0x41, 0x42, 0x41, 0x42, 0x42, 0x41, 0x42}});
        cases.push_back({"empty-0", {}});
        cases.push_back({"short-1", {0x7F}});
        cases.push_back({"edge-17", mkPattern(17u)});
        cases.push_back({"edge-18", mkPattern(18u)});
        cases.push_back({"4k-pattern", mkPattern(4096u)});
        cases.push_back({"4k-noise", mkNoise(4096u)});
        cases.push_back({"150068-pattern", mkPattern(150068u)});
        cases.push_back({"150068-noise", mkNoise(150068u)});
        for (const LzssCase& c : cases) {
            lift_test::g_current_case = c.name;
            const LzssResult base = runLzss(exe, c.input, false, c.name);
            const LzssResult ovr = runLzss(exe, c.input, true, c.name);
            LIFT_CHECK(base.valid && ovr.valid);
            if (!base.valid || !ovr.valid) continue;
            const bool gave_up = base.info[0] != 0u && base.info[2] == base.info[0];
            std::printf("  [lzss %-16s] in=%6zu comp=%6u gave_up=%d eax=%08X\n", c.name, c.input.size(),
                        base.info[2], gave_up ? 1 : 0, base.eax);
            LIFT_CHECK_EQ_U32(ovr.eax, base.eax);
            LIFT_CHECK_EQ_U32(ovr.esp, base.esp);
            LIFT_CHECK_EQ_U32(ovr.eip, base.eip);
            LIFT_CHECK_EQ_U32(ovr.ebx, base.ebx);
            LIFT_CHECK_EQ_U32(ovr.esi, base.esi);
            LIFT_CHECK_EQ_U32(ovr.edi, base.edi);
            LIFT_CHECK_EQ_U32(ovr.ebp, base.ebp);
            for (int i = 0; i < 3; ++i) LIFT_CHECK_EQ_U32(ovr.info[i], base.info[i]);
            LIFT_CHECK(ovr.obj == base.obj);
            LIFT_CHECK(ovr.out == base.out);
            // ★ 主判据：这一项**真的被接管了**，且没有偷偷回退（排除"没启用""每次回退"两种假绿）。
            LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(ovr.hits), 1u);
            LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(ovr.fallbacks), 0u);
            if (ovr.obj != base.obj) {
                std::size_t diff = 0;
                for (std::size_t k = 0; k < ovr.obj.size() && k < base.obj.size(); ++k) {
                    if (ovr.obj[k] != base.obj[k]) {
                        diff = k;
                        break;
                    }
                }
                std::fprintf(stderr, "  [lzss %s] 对象首个差异 @0x%zX: ovr=%02X base=%02X\n", c.name, diff,
                             ovr.obj[diff], base.obj[diff]);
            }
            if (ovr.out != base.out) {
                std::size_t diff = 0;
                for (std::size_t k = 0; k < ovr.out.size() && k < base.out.size(); ++k) {
                    if (ovr.out[k] != base.out[k]) {
                        diff = k;
                        break;
                    }
                }
                std::fprintf(stderr, "  [lzss %s] out 首个差异 @%zu: ovr=%02X base=%02X（comp %u/%u）\n",
                             c.name, diff, ovr.out[diff], base.out[diff], ovr.info[2], base.info[2]);
            }
        }

        // ---- ⑤b `ReusedOut` 回退 ⇒ **解释器真的重放**（`delete[]` 0x4367E6 + `new[]` 0x436801）----
        // ★ 为什么单列：现有的 9 档全是"新对象"（`[this+0xD42C] == 0`），只验了"接管 + 逐字节等价"；
        //   而 `ReusedOut` 只验过"零写 + declines 桶"（pure 的 `lzss/admission-no-write`），
        //   **没有**验"回退之后 guest 自己走完对象生命周期"。进 `all` 后这是会被真实存档 / 对象
        //   复用踩到的路径 ⇒ 这里用**堆上预置的旧出缓冲**把那条形状造出来，两臂各跑一次。
        // 判据：①override 臂 `hits == 0 && fallbacks == 1`（真的回退，不是偷偷接管）；
        //   ②两臂的 `eax / esp / eip / 寄存器 / info[0..2] / obj(0xD430) / out` 全同
        //   （`obj` 里含 `[this+0xD42C]` = 重放后**新分配**的指针，所以这条同时钉住"旧块被 free、
        //   新块从同一份堆状态里拿到同一个地址"）；③两臂 `heap.allocCount()` 相同
        //   （override 没有在旁路自己分配 / 释放）。
        {
            lift_test::g_current_case = "lzss/decline-replay";
            const std::vector<std::uint8_t> input = {0x42, 0x41, 0x42, 0x41, 0x42, 0x42, 0x41, 0x42};
            const std::uint32_t kOldOutBytes = 16u;  // 旧出缓冲（内容不被读，只被 delete[]）
            const LzssResult base = runLzss(exe, input, false, "decline-replay", kOldOutBytes);
            const LzssResult ovr = runLzss(exe, input, true, "decline-replay", kOldOutBytes);
            LIFT_CHECK(base.valid && ovr.valid);
            if (base.valid && ovr.valid) {
                std::printf("  [lzss %-16s] in=%6zu comp=%6u eax=%08X allocs=%u\n", "decline-replay",
                            input.size(), base.info[2], base.eax, base.alloc_count);
                LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(ovr.hits), 0u);
                LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(ovr.fallbacks), 1u);
                LIFT_CHECK_EQ_U32(ovr.eax, base.eax);
                LIFT_CHECK_EQ_U32(ovr.esp, base.esp);
                LIFT_CHECK_EQ_U32(ovr.eip, base.eip);
                LIFT_CHECK_EQ_U32(ovr.ebx, base.ebx);
                LIFT_CHECK_EQ_U32(ovr.esi, base.esi);
                LIFT_CHECK_EQ_U32(ovr.edi, base.edi);
                LIFT_CHECK_EQ_U32(ovr.ebp, base.ebp);
                for (int i = 0; i < 3; ++i) LIFT_CHECK_EQ_U32(ovr.info[i], base.info[i]);
                LIFT_CHECK(ovr.obj == base.obj);
                LIFT_CHECK(ovr.out == base.out);
                LIFT_CHECK_EQ_U32(ovr.alloc_count, base.alloc_count);
                // 两臂都**真的做了事**（排除"两边都空跑"的假绿）
                LIFT_CHECK(ovr.eax != 0u);
                LIFT_CHECK_EQ_U32(ovr.info[0], static_cast<std::uint32_t>(input.size()));
                LIFT_CHECK(ovr.info[2] != 0u);
            }
        }
    }

    // ---- ④ sub_4E0B40：libvorbis `mdct_backward`（**数值容忍档**；解释器臂 = 同一 EA 的 oracle）----
    //
    // 判据（红了意味着什么）：
    //   ① 同一份 `mdct_lookup`（n / log2n / trig / bitrev）与同一份输入，解释器臂 vs override 臂的
    //      `out[0..n)` **逐值比**，`max|Δ| <= 1e-6 * peak`（容差 = 这一档的"契约"；写坏了是 O(1) 的差）；
    //   ② override 臂 `hits==1 && fallbacks==0`（排除"没接管"与"每次回退"两种假绿）；
    //   ③ 两臂都真的算出了东西（peak > 1e-3，排除"两边全零"）。
    // ★ 为什么 oracle 是解释器而不是手写参考：真字节是唯一外部真值；`.lst` 只用来定 EA。
    // ★ `in == out`（原地）是**真实调用点** 0x4E2EAA 的形状，必须单列；`n` 覆盖 64..2048。
    {
        // 0x4E0230..0x4E0DC0 是自足的一整块（`mdct_backward` + `butterflies` + `bitreverse` +
        // 四个蝶形）。唯一的对外调用是 `mdct_clear` 里的 `_free`，本用例不走那条。
        const std::vector<std::uint8_t> code_mdct = img.at(0x004E0230u, 0x004E0DC0u - 0x004E0230u);

        // 表 = `lift_fixture::mdctFixtureTables`（**不复刻** guest 的 `mdct_init`：见 fixture.h 的说明）
        struct MdctOutcome {
            bool valid = false;
            std::uint32_t hits = 0;
            std::uint32_t fallbacks = 0;
            std::vector<float> out;
        };

        auto runMdct = [&](int n, bool aliased, bool with_override) -> MdctOutcome {
            MdctOutcome res;
            lift_fixture::Fixture fx;
            if (!fx.ok) return res;
            const int n2 = n / 2;
            std::vector<float> trig;
            std::vector<std::int32_t> bitrev;
            lift_fixture::mdctFixtureTables(n, &trig, &bitrev);

            constexpr std::uint32_t kInitEa = 0x00420000u;
            constexpr std::uint32_t kTrigEa = 0x00430000u;
            constexpr std::uint32_t kBitrevEa = 0x00450000u;
            constexpr std::uint32_t kInEa = 0x00470000u;
            constexpr std::uint32_t kOutEa = 0x00490000u;
            const std::uint32_t in_arg = aliased ? kOutEa : kInEa;

            int log2n = 0;
            while ((1 << log2n) < n) ++log2n;
            std::vector<std::uint8_t> init(16u, 0);
            std::memcpy(init.data() + 0, &n, 4);
            std::memcpy(init.data() + 4, &log2n, 4);
            std::memcpy(init.data() + 8, &kTrigEa, 4);
            std::memcpy(init.data() + 12, &kBitrevEa, 4);
            std::vector<std::uint8_t> trig_b(trig.size() * 4u);
            std::memcpy(trig_b.data(), trig.data(), trig_b.size());
            std::vector<std::uint8_t> bitrev_b(bitrev.size() * 4u);
            std::memcpy(bitrev_b.data(), bitrev.data(), bitrev_b.size());

            // 输入：确定性 LCG ⇒ [-1, 1)（不依赖 rand()/时间，两臂同一份）
            std::vector<float> in(static_cast<std::size_t>(n2));
            std::uint32_t x = 0x12345678u;
            for (float& v : in) {
                x = x * 1664525u + 1013904223u;
                v = static_cast<float>((x >> 8) & 0xFFFFu) / 32768.0f - 1.0f;
            }
            std::vector<std::uint8_t> in_b(in.size() * 4u);
            std::memcpy(in_b.data(), in.data(), in_b.size());
            std::vector<std::uint8_t> out_b(static_cast<std::size_t>(n) * 4u, 0x5A);

            // guest 的蝶形从 `.data` 读 4 个常量（`dbl_51D7F8`=0.5 / `dbl_52FA00`=cPI2_8 /
            // `dbl_52FA08`=cPI3_8 / `dbl_52FA10`=cPI1_8）—— bare 夹具里得把这几页放进去，
            // 否则解释器臂会 SIGSEGV（⛔ 这不是 false positive：真 boot 里它们本来就在）。
            const std::vector<std::uint8_t> const_half = img.at(0x0051D7F8u, 8u);
            const std::vector<std::uint8_t> const_trig = img.at(0x0052FA00u, 24u);
            if (!fx.m.placeData(0x0051D7F8u, const_half.data(), const_half.size(), &fx.err)) return res;
            if (!fx.m.placeData(0x0052FA00u, const_trig.data(), const_trig.size(), &fx.err)) return res;
            if (!fx.m.placeData(kInitEa, init.data(), init.size(), &fx.err)) return res;
            if (!fx.m.placeData(kTrigEa, trig_b.data(), trig_b.size(), &fx.err)) return res;
            if (!fx.m.placeData(kBitrevEa, bitrev_b.data(), bitrev_b.size(), &fx.err)) return res;
            // ⚠ 顺序有意：先铺整个 `out`（n 个 float，`out` 段必须整段已提交），再把输入写到
            // `in_arg`。`in == out` 的那条里 `in_arg == kOutEa` ⇒ 前半段被输入覆盖，正是原地调用的形状。
            if (!fx.m.placeData(kOutEa, out_b.data(), out_b.size(), &fx.err)) return res;
            if (!fx.m.placeData(in_arg, in_b.data(), in_b.size(), &fx.err)) return res;
            if (!fx.m.placeCode(0x004E0230u, code_mdct.data(), code_mdct.size(), &fx.err)) return res;
            if (with_override) {
                std::string err;
                if (!fx.m.overrides().add(0x004E0B40u, "ref_mdct", lift::GuestConv::Cdecl, 12,
                                          lift::nativeMdct4E0B40, &err))
                    return res;
            }

            constexpr std::uint32_t kCallEa = 0x00402000u;
            lift_fixture::Asm a(kCallEa);
            a.pushImm(kOutEa);   // out
            a.pushImm(in_arg);   // in
            a.pushImm(kInitEa);  // init
            a.callRel(0x004E0B40u);
            a.addEspImm8(12);  // cdecl：调用方清栈
            a.ret();
            if (!fx.m.placeCode(kCallEa, a.b.data(), a.b.size(), &fx.err)) return res;
            fx.rearmStack();
            fx.runCode(kCallEa);
            if (!fx.ok) return res;

            res.valid = true;
            res.out.resize(static_cast<std::size_t>(n));
            std::memcpy(res.out.data(), fx.m.mem().host(kOutEa), res.out.size() * 4u);
            if (with_override) {
                const lift::NativeOverride* e = fx.m.overrides().at(0);
                res.hits = static_cast<std::uint32_t>(e->hits);
                res.fallbacks = static_cast<std::uint32_t>(e->fallbacks);
            }
            return res;
        };

        struct MdctCase {
            const char* name;
            int n;
            bool aliased;
        };
        const MdctCase mdct_cases[] = {
            {"mdct/n=64", 64, false},     {"mdct/n=256", 256, false},   {"mdct/n=256/alias", 256, true},
            {"mdct/n=1024", 1024, false}, {"mdct/n=2048", 2048, false}, {"mdct/n=2048/alias", 2048, true},
            {"mdct/n=8192", 8192, false},  // 上界（`kMaxN`）
        };
        for (const MdctCase& c : mdct_cases) {
            lift_test::g_current_case = c.name;
            const MdctOutcome base = runMdct(c.n, c.aliased, false);
            const MdctOutcome ovr = runMdct(c.n, c.aliased, true);
            LIFT_CHECK(base.valid && ovr.valid);
            if (!base.valid || !ovr.valid) continue;
            float peak = 0.0f;
            float maxdiff = 0.0f;
            std::size_t at = 0;
            std::size_t nonfinite = 0;
            for (std::size_t i = 0; i < base.out.size(); ++i) {
                // ★ NaN/Inf 必须**单独数**：`d > maxdiff` 对 NaN 恒 false ⇒ 一个 NaN 输出会让整条
                //   数值判据静默通过（实测：在纯核末尾写 `out[0] = NaN`，旧的 `max|d|` 一位不变、749 checks 全绿）。
                if (!std::isfinite(base.out[i]) || !std::isfinite(ovr.out[i])) {
                    ++nonfinite;
                    continue;
                }
                peak = std::fmax(peak, std::fabs(base.out[i]));
                const float d = std::fabs(ovr.out[i] - base.out[i]);
                if (d > maxdiff) {
                    maxdiff = d;
                    at = i;
                }
            }
            std::printf("  [mdct %-12s] peak=%9.5f max|d|=%.3e @%4zu rel=%.2e\n", c.name, peak, maxdiff, at,
                        peak > 0.0f ? maxdiff / peak : 0.0f);
            LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(nonfinite), 0u);  // ⛔ 不许有 NaN/Inf（见上面的注释）
            LIFT_CHECK(peak > 1e-3f);  // ③ 两臂都真的算出了东西
            // ② 这一项**真的被接管**了，且没有偷偷回退
            LIFT_CHECK_EQ_U32(ovr.hits, 1u);
            LIFT_CHECK_EQ_U32(ovr.fallbacks, 0u);
            // ① 数值档容差（这一档的契约；实测见上面那行打印）
            LIFT_CHECK(maxdiff <= 1e-6f * peak);
        }
    }

    // ---- ⑥ `__ftol2_sse`（`0x4E82C0`）：真字节两臂差分（边界表 + `fpu.status` 逐位 + scratch 4 B）----
    //
    // 判据（红了意味着什么）：
    //   ① 边界表每一格的 `EDX:EAX`：解释器臂 vs native 臂逐值相同，**并且**等于表里的 spec 值
    //      （`want_eax` / `want_edx` 由 `.lst` 字节推出，⛔ 不是"跑一遍把 actual 抄成 expected"）；
    //   ② `fpu.status` **逐位**相同（PE/UE/OE/IE 粘性位 + ES + TOP 字段），且**入口的 C0..C3
    //      原样保留**（那一段 guest 里没有任何指令改 CC ⇒ 抄 `native_affine` 的清 CC 会当场红）；
    //      `fpu.control` / `fpu.tag` / `fpu.top` 相同；
    //   ③ guest scratch 的 `[E+18h,E+1Ch)`（= `0x4E8301 fst dword` 那 4 字节）逐字节相同；
    //      另两段（`[E,E+4)` = `f32(x−n)`、`[E+10h,E+18h)` = 就近取整的 int64）是**登记的不复刻段**
    //      ⇒ 只断言 native 臂里它们仍是预填哨兵（native 不许往那里写别的）；
    //   ④ `hits == 1` / `fallbacks == 0`（排除"没接管"与"每次回退"两种假绿）。
    // ★ 为什么边界表里**必须有非整数 / 非 2 的幂**：`±1.5` / `±2.7` 是"截断 vs 四舍五入"的唯一判别
    //   点（`0.4` / `-0.4` 那种两边都给 0 ⇒ 恒绿）；`1.3`（f32 不可精确表示）与 `1e39`（f32 上溢）
    //   才是"漏掉 `fst dword`"的判别点 —— 全是 2 的幂时 PE/OE 一位都不会置。
    // ★ 为什么还要 `2^53+1` / `2^62+1.5` / `2^63−1` 这三个 **10 字节 ext80** 输入：宿主
    //   `(int64)(double)` 在 `|x| ≥ 2^53` 上差一位（double 只有 53 位）⇒ 它们钉住"必须走 softfloat"。
    // ★ 入口 `fpu.status` 带 C0..C3（`0x4700`）与 `top_noise`（TOP != 0）两档分别钉住
    //   "清 CC" 与 `empty(0)`/`empty(phys(0))` 这两类硬错（复审 H2 / M4）。
    // ★ 入口 EA 必须是 `0x4E82C0`（`.lst` 381874 的 `COLLAPSED FUNCTION __ftol2_sse`，28 B），
    //   ⛔ 不是 `0x4E82F6`（`__ftol2`）：后者只是 `0x4E82E3 jz` 的**函数内**目标 ⇒ 挂它 hits 恒 0。
    // ★ 已知不覆盖（登记，不修）：`0x4B5545 jmp __ftol2_sse`（`sub_4B5500` 的 qsort 比较器尾跳）——
    //   dispatcher 只在上一条是 `call` 时拦 ⇒ 这条既不 `+hits` 也不 `+fallbacks`；另有
    //   `0x4BC539` / `0x4BC680` 直连 `__ftol2`（不经 `0x4E82C0`）。⇒ hits 只是**下界**（386 个入口）。
    {
        const std::vector<std::uint8_t> code_ftol = img.at(0x004E82C0u, 0x004E836Bu - 0x004E82C0u);

        auto f64bits = [](double d) {
            std::uint64_t v = 0;
            std::memcpy(&v, &d, 8);
            return v;
        };
        // ext80 正规值：value = (−1)^neg × sig × 2^(e−63)，sig ∈ [2^63, 2^64)（e 是二进制指数）
        auto mk80 = [](bool neg, int e, std::uint64_t sig, FtolCase* c) {
            for (int i = 0; i < 8; ++i) c->t80[i] = static_cast<std::uint8_t>((sig >> (8 * i)) & 0xFFu);
            const std::uint16_t se = static_cast<std::uint16_t>((e + 16383) | (neg ? 0x8000 : 0));
            c->t80[8] = static_cast<std::uint8_t>(se & 0xFFu);
            c->t80[9] = static_cast<std::uint8_t>(se >> 8);
            c->ext80 = true;
        };

        std::vector<FtolCase> cases;
        auto add = [&](const FtolCase& c) { cases.push_back(c); };

        // ── 基本 / 符号 / 零 ──
        {
            FtolCase c;
            c.name = "ftol/+0";
            c.f64 = f64bits(0.0);
            add(c);
            c.name = "ftol/-0";
            c.f64 = f64bits(-0.0);
            add(c);
            c.name = "ftol/+1";
            c.f64 = f64bits(1.0);
            c.want_eax = 1u;
            add(c);
            // -1 ⇒ EDX 也是 0xFFFFFFFF（高位是符号扩展的一部分，不是 0）
            c.name = "ftol/-1";
            c.f64 = f64bits(-1.0);
            c.want_eax = 0xFFFFFFFFu;
            c.want_edx = 0xFFFFFFFFu;
            add(c);
        }
        // ── ★ 截断判别档：小数部分 ≥ 0.5（四舍五入会给另一个整数）──
        {
            FtolCase c;
            c.name = "ftol/+1.5";
            c.f64 = f64bits(1.5);
            c.want_eax = 1u;
            add(c);
            c.name = "ftol/-1.5";
            c.f64 = f64bits(-1.5);
            c.want_eax = 0xFFFFFFFFu;  // -1
            c.want_edx = 0xFFFFFFFFu;
            add(c);
            c.name = "ftol/+3.5";
            c.f64 = f64bits(3.5);
            c.want_eax = 3u;
            c.want_edx = 0u;
            add(c);
            c.name = "ftol/+2.7";
            c.f64 = f64bits(2.7);
            c.want_eax = 2u;
            add(c);
            c.name = "ftol/-2.7";
            c.f64 = f64bits(-2.7);
            c.want_eax = 0xFFFFFFFEu;  // -2
            c.want_edx = 0xFFFFFFFFu;
            add(c);
        }
        // ── ±0.5 邻域（截断给 0；就近给 0 或 ±1）──
        {
            FtolCase c;
            c.name = "ftol/+0.5";
            c.f64 = f64bits(0.5);
            add(c);
            c.name = "ftol/-0.5";
            c.f64 = f64bits(-0.5);
            add(c);
            c.name = "ftol/+0.6";
            c.f64 = f64bits(0.6);
            add(c);
            c.name = "ftol/-0.6";
            c.f64 = f64bits(-0.6);
            add(c);
        }
        // ── ★ "漏掉 `fst dword`（f32 转换）" 的判别档：PE（不可精确表示）/ OE（溢出）──
        {
            FtolCase c;
            c.name = "ftol/1.3-PE";
            c.f64 = f64bits(1.3);
            c.want_eax = 1u;
            add(c);
            c.name = "ftol/1e39-OE";
            c.f64 = f64bits(1.0e39);  // > FLT_MAX ⇒ f32 上溢（OE+PE），同时 > 2^63 ⇒ IE
            c.want_eax = 0u;
            c.want_edx = 0x80000000u;
            add(c);
            c.name = "ftol/1e-42-UE";
            c.f64 = f64bits(1.0e-42);  // f32 次正规/下溢 ⇒ UE+PE；截断 0
            c.want_eax = 0u;
            c.want_edx = 0u;
            add(c);
        }
        // ── 32 位边界与 2^31..2^63 的"回绕"（⛔ 不是 clamp）──
        {
            FtolCase c;
            c.name = "ftol/2^31-1";
            c.f64 = f64bits(2147483647.0);
            c.want_eax = 0x7FFFFFFFu;
            add(c);
            c.name = "ftol/2^31";
            c.f64 = f64bits(2147483648.0);
            c.want_eax = 0x80000000u;
            add(c);
            c.name = "ftol/3e9-wrap";
            c.f64 = f64bits(3.0e9);
            c.want_eax = 0xB2D05E00u;  // 3000000000 的低 32 位（符号变负）
            c.want_edx = 0u;
            add(c);
        }
        // ── 2^53 / 2^62 / 2^63 ──
        {
            FtolCase c;
            c.name = "ftol/2^53";
            c.f64 = f64bits(9007199254740992.0);
            c.want_eax = 0u;         // 低 32 位为 0 ⇒ 走 `0x4E8355` 那一支
            c.want_edx = 0x00200000u;
            add(c);
            c.name = "ftol/2^62";
            c.f64 = f64bits(4611686018427387904.0);
            c.want_eax = 0u;
            c.want_edx = 0x40000000u;
            add(c);
            c.name = "ftol/2^63";
            c.f64 = f64bits(9223372036854775808.0);
            c.want_eax = 0u;
            c.want_edx = 0x80000000u;  // fistp 越界 ⇒ int64 不定值
            add(c);
            c.name = "ftol/-2^63";
            c.f64 = f64bits(-9223372036854775808.0);
            c.want_eax = 0u;
            c.want_edx = 0x80000000u;  // **恰好** −2^63：位型与不定值相同，但**不置 IE**
            add(c);
        }
        // ── ★ 10 字节 ext80：宿主 double 截断会差一位的三档 ──
        {
            FtolCase c;  // 2^53 + 1（double 表示不出 ⇒ `(int64)(double)` 会给 2^53）
            c.name = "ftol/2^53+1-ext80";
            mk80(false, 53, 0x8000000000000400ull, &c);  // sig = (2^53+1) × 2^10 ⇒ 值 = sig × 2^-10
            c.want_eax = 1u;
            c.want_edx = 0x00200000u;
            add(c);
            FtolCase d;  // 2^62 + 1.5 ⇒ 截断 = 2^62+1；就近(-even) = 2^62+2 ⇒ 再修正 −1
            d.name = "ftol/2^62+1.5-ext80";
            mk80(false, 62, 0x8000000000000003ull, &d);
            d.want_eax = 1u;
            d.want_edx = 0x40000000u;
            add(d);
            FtolCase e;  // 2^63 − 1（64 位有效位全 1）⇒ INT64_MAX，**不置 IE**
            e.name = "ftol/2^63-1-ext80";
            mk80(false, 62, 0xFFFFFFFFFFFFFFFEull, &e);
            e.want_eax = 0xFFFFFFFFu;
            e.want_edx = 0x7FFFFFFFu;
            add(e);
        }
        // ── ±Inf / NaN（都是 `EAX=0, EDX=0x80000000` + IE；⛔ 不是 `0x80000000/0`）──
        {
            FtolCase c;
            c.name = "ftol/+Inf";
            c.f64 = 0x7FF0000000000000ull;
            c.want_edx = 0x80000000u;
            add(c);
            c.name = "ftol/-Inf";
            c.f64 = 0xFFF0000000000000ull;
            c.want_edx = 0x80000000u;
            add(c);
            c.name = "ftol/NaN";
            c.f64 = 0x7FF8000000000000ull;
            c.want_edx = 0x80000000u;
            add(c);
        }
        // ── 入口 C0..C3 = 0x4700：那一段 guest 不改 CC ⇒ 两臂都必须**原样保留**（抓 M4）──
        {
            FtolCase c;
            c.name = "ftol/status-cc-preserved";
            c.f64 = f64bits(2.7);
            c.entry_status = 0x4700u;
            c.want_eax = 2u;
            add(c);
        }
        // ── TOP != 0（栈里先垫一层活值）：抓 `empty(0)` vs `empty(phys(0))`（复审 H2）──
        {
            FtolCase c;
            c.name = "ftol/top-nonzero";
            c.f64 = f64bits(-2.7);
            c.top_noise = true;
            c.want_eax = 0xFFFFFFFEu;
            c.want_edx = 0xFFFFFFFFu;
            add(c);
        }
        // ── 闸门：`[0x55F2E8] != 0` ⇒ 必须回退（真机走 `cvttsd2si`，解释器解不出那一条 ⇒ 只跑 override 臂）
        {
            FtolCase c;
            c.name = "ftol/decline-sse2-flag";
            c.f64 = f64bits(2.7);
            c.flag = 1u;
            c.skip_base = true;
            c.want_decline = true;
            add(c);
        }

        for (const FtolCase& c : cases) {
            lift_test::g_current_case = c.name;
            const FtolResult b = c.skip_base ? FtolResult{} : runFtol(code_ftol, c, false);
            const FtolResult o = runFtol(code_ftol, c, true);
            if (!c.skip_base) {
                LIFT_CHECK(b.valid && o.valid);
                if (!b.valid || !o.valid) continue;
            } else {
                LIFT_CHECK(o.valid);
                if (!o.valid) continue;
            }
            LIFT_CHECK_EQ_U32(o.eax, b.eax);
            LIFT_CHECK_EQ_U32(o.edx, b.edx);
            if (c.want_decline) {
                LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(o.hits), 0u);
                LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(o.fallbacks), 1u);
                LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(o.decline_sse2), 1u);
            } else {
                // ① spec 值（解释器臂与 native 臂都必须命中它）
                LIFT_CHECK_EQ_U32(b.eax, c.want_eax);
                LIFT_CHECK_EQ_U32(b.edx, c.want_edx);
                LIFT_CHECK_EQ_U32(o.eax, c.want_eax);
                LIFT_CHECK_EQ_U32(o.edx, c.want_edx);
                // ② x87 状态字逐位 + 控制字 / tag / top
                LIFT_CHECK_EQ_U32(o.fpu_status, b.fpu_status);
                LIFT_CHECK_EQ_U32(o.fpu_control, b.fpu_control);
                LIFT_CHECK_EQ_U32(o.fpu_tag, b.fpu_tag);
                LIFT_CHECK_EQ_U32(o.fpu_top, b.fpu_top);
                // ③ scratch 的 4 字节（`fst dword [E+18h]`）+ 另两段必须是预填哨兵
                LIFT_CHECK(std::memcmp(o.scratch + 0x18u, b.scratch + 0x18u, 4u) == 0);
                bool sentinel_ok = true;
                for (std::uint32_t i = 0u; i < 4u; ++i) sentinel_ok = sentinel_ok && o.scratch[i] == 0x5Au;
                for (std::uint32_t i = 0x10u; i < 0x18u; ++i)
                    sentinel_ok = sentinel_ok && o.scratch[i] == 0x5Au;
                LIFT_CHECK(sentinel_ok);
                // ④ 真的被接管了
                LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(o.hits), 1u);
                LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(o.fallbacks), 0u);
            }
            if (o.eax != b.eax || o.edx != b.edx) {
                std::fprintf(stderr, "  [ftol %s] EDX:EAX base=%08X:%08X ovr=%08X:%08X\n", c.name, b.edx,
                             b.eax, o.edx, o.eax);
            }
            if (o.fpu_status != b.fpu_status) {
                std::fprintf(stderr, "  [ftol %s] fpu.status %04X vs %04X（tag %04X/%04X top %u/%u）\n", c.name,
                             b.fpu_status, o.fpu_status, b.fpu_tag, o.fpu_tag,
                             static_cast<unsigned>(b.fpu_top), static_cast<unsigned>(o.fpu_top));
            }
            if (std::memcmp(o.scratch + 0x18u, b.scratch + 0x18u, 4u) != 0) {
                std::fprintf(stderr, "  [ftol %s] scratch[18h] %02X%02X%02X%02X vs %02X%02X%02X%02X\n", c.name,
                             b.scratch[0x18], b.scratch[0x19], b.scratch[0x1A], b.scratch[0x1B],
                             o.scratch[0x18], o.scratch[0x19], o.scratch[0x1A], o.scratch[0x1B]);
            }
        }
    }

    return lift_test::finish("native-override-assets");
}
