// emulator-lift · assets 档：**真 guest 字节**的 native override 差分（解释器 vs override）
//
// 判据（红了意味着什么）：
//   ① 从 `AGE.EXE` 里取出 `sub_429020` / `sub_48AE90` / `sub_401000` 的**真字节**，放进 bare 机
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
#include <string>
#include <vector>

#include "check.h"
#include "fixture.h"
#include "runtime/com.h"
#include "runtime/d3d9.h"
#include "runtime/native_overrides.h"

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

int main() {
    const std::string exe = envValue("LIFT_AGE_EXE");
    if (exe.empty()) LIFT_SKIP("没有 LIFT_AGE_EXE（CMake 的 assets 档会设它）");
    ImageCode img;
    if (!img.load(exe)) LIFT_SKIP("载入 AGE.EXE 失败（缺资产 ⇒ 不是红灯）");
    const std::vector<std::uint8_t> code401000 = img.at(0x00401000u, 0x100u);
    const std::vector<std::uint8_t> code429020 = img.at(0x00429020u, 0x100u);
    const std::vector<std::uint8_t> code48AE90 = img.at(0x0048AE90u, 0x200u);

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

    return lift_test::finish("native-override-assets");
}
