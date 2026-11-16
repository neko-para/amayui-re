// emulator-lift · 夹具：`d3dx9_43!D3DXLoadSurfaceFromSurface`（2D surface 拷贝）
//
// ★ 这条导出的判据 = **微软文档**（D3DXLoadSurfaceFromSurface），⛔ 不是"游戏这次跑通了"：
//   ① **半开矩形** `[left,right) × [top,bottom)`：源矩形 3×2、目标偏移 (2,2) ⇒ 目标上**只有**那 3×2 变，
//      相邻像素（含右/下边界）逐字节不变。把矩形当闭区间 ⇒ 读到 4×3 ⇒ 逐像素红。
//   ② **不隐式裁剪**：越界 / 负坐标 / 空矩形 / rect 指针不可读 ⇒ `D3DERR_INVALIDCALL` + 机器继续 +
//      目标**一个字节都不变**。⛔ 静默夹取 / 部分拷贝都会红。
//   ③ **只做 1:1**：源矩形 2×2 → 目标矩形 3×3 ⇒ **响亮停**（文档说那会按 Filter 缩放；本线不实现）。
//   ④ **不支持 colorKey**：`ColorKey != 0` ⇒ 响亮停。★ 拒绝的**理由**是"本期未实现"，⛔ 不是
//      "文档没写死"——文档明写 `ColorKey` 是 always a 32-bit ARGB color 且 Alpha is significant
//      （`https://learn.microsoft.com/en-us/windows/win32/direct3d9/d3dxloadsurfacefromsurface`）。
//      `caseColorKey` 连**理由文本**一起判（改成"文档没写死"⇒ 红）。
//   ⑤ **格式**（`d3d9::surfaceCopyPlan`）：同 D3DFORMAT ⇒ 恒等映射；32bpp 同序的 **A8→X8**
//      （丢 alpha）⇒ 恒等；**X8→A8**（`22→21`，把不携带信息的顶字节当 alpha）⇒ **拒绝**
//      （父级裁决：响亮停，⛔ 不猜 0xFF）；RGBA↔BGRA ⇒ 只换 R/B，其余位照抄；其它组合 ⇒ 拒绝。
//      ★ 16bpp 的同格式拷贝也走真 surface：它证明逐字节搬运**按 bpp 走**（不是硬拷 4 字节）。
//      ★ `22→21` 另有一条**真 surface** 用例（顶字节 0 ⇒ Trap + 目标一字节不变）。
//   ⑥ `rect == NULL` ⇒ **整幅**该 surface（不是"左上角"、也不是"0×0"）。
//
// ★ 全部经 guest 代码 → IAT 桩 → shim 调（8 个参数按 stdcall 压栈），测的是 guest 可见的导出。
// ★ 纯档：自造 guest 字节 + `d3d9::createTextureState` / `createD3dxSurfaceObject` 建两块真 surface
//   （像素在 guest `SurfaceArena` 里）⇒ 不碰游戏 / LFS。
// ★ `3D 不做` 那条裁决管的是 3D；本导出按微软文档是 **surface→surface 的 2D blit** ⇒ 真实现。
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "check.h"
#include "runtime/com.h"
#include "runtime/d3d9.h"
#include "runtime/machine.h"
#include "runtime/surface.h"
#include "shims/d3dx9_surface.h"
#include "shims/register.h"

namespace {

using lift::d3d9::SurfaceCopyPlan;
using lift::d3d9::SurfaceCopyResult;

constexpr std::uint32_t kStubPage = 0x7FF80000u;
constexpr std::uint32_t kStubSlots = 0x1000u;
constexpr std::uint32_t kProcLoadSurface = 0x7FF81000u;
constexpr std::uint32_t kCodeBase = 0x00401000u;
constexpr std::uint32_t kCodeStride = 0x1000u;
constexpr std::uint32_t kDataBase = 0x00420000u;
constexpr std::uint32_t kDataStride = 0x1000u;
constexpr std::uint32_t kFixtureHeapBytes = 0x00100000u;
constexpr std::uint32_t kSentinel = 0xFFFFFFFFu;
// D3DX_DEFAULT（调用点 0x4A3E4E 实测 push 0FFFFFFFFh；1:1 拷贝里它不参与采样）
constexpr std::uint32_t kFilterDefault = 0xFFFFFFFFu;

// 最小机器码装配器（push imm32 / call rel32 / ret）
struct Asm {
    std::uint32_t base = 0;
    std::vector<std::uint8_t> bytes;

    explicit Asm(std::uint32_t code_ea) : base(code_ea) {}
    std::uint32_t currentEa() const { return base + static_cast<std::uint32_t>(bytes.size()); }
    void u8(std::uint8_t v) { bytes.push_back(v); }
    void u32(std::uint32_t v) {
        for (int i = 0; i < 4; ++i) u8(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFFu));
    }
    void pushImm(std::uint32_t v) {
        u8(0x68);
        u32(v);
    }
    void callRel(std::uint32_t target) {
        const std::int32_t rel = static_cast<std::int32_t>(target - (currentEa() + 5u));
        u8(0xE8);
        u32(static_cast<std::uint32_t>(rel));
    }
    void ret() { u8(0xC3); }
};

struct Surf {
    std::uint32_t object = 0;
    std::uint32_t state = 0;
    std::uint32_t ea = 0;  // 像素块在 guest 里的首字节
    std::uint32_t pitch = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t format = 0;
    std::uint32_t bpp = 0;  // 位/像素（与 TextureInfo::bpp 同口径）
    std::uint32_t bytes() const { return bpp / 8u; }
};

struct Rig {
    lift::Machine m;
    std::string err;
    bool ok = false;
    std::uint32_t device = 0;
    std::uint32_t device_state = 0;
    std::uint32_t stub = 0;
    std::uint32_t code_ea = kCodeBase;
    std::uint32_t data_ea = kDataBase;

    bool boot() {
        if (!m.initBare(&err)) return false;
        if (!m.prepareStack(&err)) return false;
        if (!m.heap().init(&m.mem(), m.layout().heap_base, kFixtureHeapBytes)) return false;
        m.shims().configureStubPage(kStubPage, kStubSlots);
        lift::registerD3D9Shims(m.shims());
        lift::registerD3DX9Shims(m.shims());
        // ★ 与生产装配同形：派生缺口表也接进来（本步实现前，这条导出就在那张表里 = addUnimplemented）。
        lift::registerImportGaps(m.shims());
        lift::d3d9::CreateRequest req;
        req.device_type = lift::d3d9::kDevTypeHal;
        req.behavior_flags = 0x44u;
        req.present.width = 8u;
        req.present.height = 8u;
        req.present.format = lift::d3d9::kFmtX8R8G8B8;
        req.present.windowed = true;
        device_state = lift::d3d9::createDeviceState(m.cpu(), req, &err);
        if (device_state == 0) return false;
        // 只要一个非空方法表（createObject 拒绝空 vtable）；本夹具从不按 vtable 调设备。
        const std::vector<lift::ComMethod> slots = {{0u, "GetCaps", lift::Conv::Stdcall, 1, nullptr}};
        device = m.cpu().com->createObject(m.cpu(), "IDirect3DDevice9", slots, device_state);
        if (device == 0) return false;
        lift::ShimDef* def = m.shims().findMutable("d3dx9_43", "D3DXLoadSurfaceFromSurface");
        if (def == nullptr) {
            err = "shim 没注册：d3dx9_43!D3DXLoadSurfaceFromSurface";
            return false;
        }
        def->stub_ea = kProcLoadSurface;
        m.cache().installShim(kProcLoadSurface, def->id);
        stub = kProcLoadSurface;
        ok = true;
        return true;
    }

    std::uint32_t placeData(const void* p, std::size_t n) {
        const std::uint32_t ea = data_ea;
        const std::uint32_t aligned = (static_cast<std::uint32_t>(n) + 15u) & ~15u;
        if (!m.placeData(ea, p, n, &err)) {
            ok = false;
            return 0;
        }
        data_ea += aligned < kDataStride ? aligned : kDataStride;
        return ea;
    }
    std::uint32_t placeRect(std::int32_t l, std::int32_t t, std::int32_t r, std::int32_t b) {
        const std::int32_t v[4] = {l, t, r, b};
        return placeData(v, sizeof(v));
    }

    // 建一块真 surface（'TEX1' 状态块 + SurfaceArena 像素）。
    // ★ pool = SYSTEMMEM(2)：与 `CreateOffscreenPlainSurface` 同一池（永不作为纹理被采样）。
    bool makeSurface(std::uint32_t w, std::uint32_t h, std::uint32_t format, Surf* out) {
        std::string why;
        const std::uint32_t state =
            lift::d3d9::createTextureState(m.cpu(), device, w, h, 1u, 0u, format, 2u, &why);
        if (state == 0) {
            err = why;
            return false;
        }
        const std::uint32_t object = lift::createD3dxSurfaceObject(m.cpu(), state, 0, nullptr);
        if (object == 0) {
            err = "createD3dxSurfaceObject 失败";
            return false;
        }
        lift::d3d9::TextureInfo info;
        if (!lift::d3d9::surfaceInfoOfState(m.cpu(), state, &info, &why)) {
            err = why;
            return false;
        }
        const lift::GuestSurface* gs = m.surfaces().find(info.arena);
        if (gs == nullptr) {
            err = "SurfaceArena 里找不到像素块";
            return false;
        }
        out->object = object;
        out->state = state;
        out->ea = gs->ea;
        out->pitch = info.pitch;
        out->width = info.width;
        out->height = info.height;
        out->format = info.format;
        out->bpp = info.bpp;
        return true;
    }

    struct Call {
        std::uint32_t hr = 0;
        lift::RunResult run;
    };

    // 从 guest 代码调 8 个参数的导出（stdcall；参数逆序压栈）。
    Call loadSurface(const Surf& src, std::uint32_t src_rect_ea, const Surf& dst, std::uint32_t dst_rect_ea,
                     std::uint32_t filter, std::uint32_t color_key) {
        Call out;
        const std::uint32_t code = code_ea;
        code_ea += kCodeStride;
        Asm a(code);
        const std::uint32_t args[8] = {dst.object, 0u /*destPalette*/, dst_rect_ea,
                                       src.object, 0u /*srcPalette*/,  src_rect_ea,
                                       filter,      color_key};
        for (int i = 7; i >= 0; --i) a.pushImm(args[i]);
        a.callRel(stub);
        a.ret();
        if (!m.placeCode(code, a.bytes.data(), a.bytes.size(), &err)) {
            ok = false;
            return out;
        }
        m.cpu().eip = code;
        m.cpu().esp() = m.layout().stack_top - 4u;
        m.mem().wr32(m.cpu().esp(), kSentinel);
        m.cpu().running = true;
        lift::RunOptions o;
        o.until_ea = kSentinel;
        o.max_insns = 200000;
        out.run = m.run(o);
        out.hr = m.cpu().eax();
        return out;
    }

    void fill32(const Surf& s, std::uint32_t value) {
        for (std::uint32_t y = 0; y < s.height; ++y)
            for (std::uint32_t x = 0; x < s.width; ++x)
                m.mem().wr32(s.ea + y * s.pitch + x * 4u, value);
    }
    void fill16(const Surf& s, std::uint16_t value) {
        for (std::uint32_t y = 0; y < s.height; ++y)
            for (std::uint32_t x = 0; x < s.width; ++x)
                m.mem().wr16(s.ea + y * s.pitch + x * 2u, value);
    }
    std::uint32_t pixel32(const Surf& s, std::uint32_t x, std::uint32_t y) const {
        return m.mem().rd32(s.ea + y * s.pitch + x * 4u);
    }
    std::uint16_t pixel16(const Surf& s, std::uint32_t x, std::uint32_t y) const {
        return m.mem().rd16(s.ea + y * s.pitch + x * 2u);
    }
};

bool allPixel32(const Rig& r, const Surf& s, std::uint32_t value) {
    for (std::uint32_t y = 0; y < s.height; ++y)
        for (std::uint32_t x = 0; x < s.width; ++x)
            if (r.pixel32(s, x, y) != value) return false;
    return true;
}

constexpr std::uint32_t kDstSentinel = 0xDEADBEEFu;

// ---------------------------------------------------------------------------------------
// ① 半开矩形 + 偏移 + NULL = 整幅
// ---------------------------------------------------------------------------------------
void caseRectSemantics() {
    lift_test::g_current_case = "d3dx9/load-surface+rect-semantics";
    Rig r;
    LIFT_CHECK(r.boot());
    if (!r.ok) return;
    Surf src;
    Surf dst;
    LIFT_CHECK(r.makeSurface(6u, 5u, lift::d3d9::kFmtA8R8G8B8, &src));
    LIFT_CHECK(r.makeSurface(6u, 5u, lift::d3d9::kFmtA8R8G8B8, &dst));
    if (!r.ok) return;
    // 源：每像素都不同（R/B 位置给可辨认的值；含 alpha ⇒ 通道换序也能被抓到）。
    for (std::uint32_t y = 0; y < src.height; ++y)
        for (std::uint32_t x = 0; x < src.width; ++x)
            r.m.mem().wr32(src.ea + y * src.pitch + x * 4u, 0xA1000000u | (y << 8) | (x + 1u));
    r.fill32(dst, kDstSentinel);

    // 源 3×2（半开 [1,4)×[1,3)）→ 目标 (2,2)。
    const std::uint32_t src_rect = r.placeRect(1, 1, 4, 3);
    const std::uint32_t dst_rect = r.placeRect(2, 2, 5, 4);
    const Rig::Call c = r.loadSurface(src, src_rect, dst, dst_rect, kFilterDefault, 0u);
    LIFT_CHECK_EQ_U32(c.hr, lift::d3d9::kOk);
    LIFT_CHECK(c.run.reason == lift::StopReason::UntilEa);
    bool rect_ok = true;
    for (std::uint32_t y = 0; y < dst.height; ++y) {
        for (std::uint32_t x = 0; x < dst.width; ++x) {
            const bool inside = x >= 2u && x < 5u && y >= 2u && y < 4u;
            const std::uint32_t want =
                inside ? (0xA1000000u | ((y - 2u + 1u) << 8) | (x - 2u + 1u + 1u)) : kDstSentinel;
            if (r.pixel32(dst, x, y) != want) {
                lift_test::failf(__FILE__, __LINE__,
                                 "dst(%u,%u) = 0x%08X, expected 0x%08X（半开矩形 / 偏移 / 不裁剪）", x, y,
                                 r.pixel32(dst, x, y), want);
                rect_ok = false;
            }
        }
    }
    LIFT_CHECK(rect_ok);

    // 两个 rect 都是 NULL ⇒ 整幅（文档口径），不是"左上角"。
    r.fill32(dst, kDstSentinel);
    const Rig::Call full = r.loadSurface(src, 0u, dst, 0u, kFilterDefault, 0u);
    LIFT_CHECK_EQ_U32(full.hr, lift::d3d9::kOk);
    LIFT_CHECK(full.run.reason == lift::StopReason::UntilEa);
    bool full_ok = true;
    for (std::uint32_t y = 0; y < src.height; ++y) {
        for (std::uint32_t x = 0; x < src.width; ++x) {
            const std::uint32_t want = 0xA1000000u | (y << 8) | (x + 1u);
            if (r.pixel32(dst, x, y) != want) {
                lift_test::failf(__FILE__, __LINE__, "整幅拷贝 dst(%u,%u) = 0x%08X, expected 0x%08X", x, y,
                                 r.pixel32(dst, x, y), want);
                full_ok = false;
            }
        }
    }
    LIFT_CHECK(full_ok);

    // 只有 src 是 NULL（目标给全幅 rect）：等价于整幅。
    r.fill32(dst, kDstSentinel);
    const std::uint32_t dst_full = r.placeRect(0, 0, 6, 5);
    const Rig::Call src_null = r.loadSurface(src, 0u, dst, dst_full, kFilterDefault, 0u);
    LIFT_CHECK_EQ_U32(src_null.hr, lift::d3d9::kOk);
    LIFT_CHECK(src_null.run.reason == lift::StopReason::UntilEa);
    bool src_null_ok = true;
    for (std::uint32_t y = 0; y < src.height; ++y) {
        for (std::uint32_t x = 0; x < src.width; ++x) {
            if (r.pixel32(dst, x, y) != (0xA1000000u | (y << 8) | (x + 1u))) {
                lift_test::failf(__FILE__, __LINE__, "src=NULL 整幅拷贝 dst(%u,%u) 不对", x, y);
                src_null_ok = false;
            }
        }
    }
    LIFT_CHECK(src_null_ok);
}

// ---------------------------------------------------------------------------------------
// ② 不隐式裁剪：越界 / 负坐标 / 空矩形 / rect 指针不可读 ⇒ INVALIDCALL，目标一字节不变
// ---------------------------------------------------------------------------------------
void caseContract() {
    lift_test::g_current_case = "d3dx9/load-surface+contract";
    Rig r;
    LIFT_CHECK(r.boot());
    if (!r.ok) return;
    Surf src;
    Surf dst;
    LIFT_CHECK(r.makeSurface(6u, 5u, lift::d3d9::kFmtA8R8G8B8, &src));
    LIFT_CHECK(r.makeSurface(6u, 5u, lift::d3d9::kFmtA8R8G8B8, &dst));
    if (!r.ok) return;
    r.fill32(src, 0xA1B2C3D4u);
    r.fill32(dst, kDstSentinel);
    const std::uint32_t dst_full = r.placeRect(0, 0, 6, 5);
    const std::uint32_t traps_before = r.m.cpu().shim.unimplemented_calls;

    struct Bad {
        const char* what;
        std::int32_t l, t, rr, b;  // 源矩形
        bool dst_bad;              // 目标矩形越界
    };
    const Bad bads[] = {
        {"源右边界越界", 4, 0, 7, 2, false},
        {"源下边界越界", 0, 3, 2, 6, false},
        {"源负坐标", -1, 0, 2, 2, false},
        {"源空宽度", 2, 2, 2, 4, false},
        {"源空高度", 0, 3, 2, 3, false},
        {"目标越界", 0, 0, 6, 5, true},  // 目标矩形 [3,9)×[3,7)：9>6 / 7>5
    };
    for (const Bad& b : bads) {
        const std::uint32_t src_rect = r.placeRect(b.l, b.t, b.rr, b.b);
        const std::uint32_t dst_rect = b.dst_bad ? r.placeRect(3, 3, 9, 7) : dst_full;
        const Rig::Call c = r.loadSurface(src, src_rect, dst, dst_rect, kFilterDefault, 0u);
        if (c.hr != lift::d3d9::kInvalidCall) {
            lift_test::failf(__FILE__, __LINE__, "%s：hr=0x%08X，应为 D3DERR_INVALIDCALL", b.what, c.hr);
            continue;
        }
        if (c.run.reason != lift::StopReason::UntilEa) {
            lift_test::failf(__FILE__, __LINE__, "%s：机器停了（契约错必须可继续）", b.what);
            continue;
        }
        if (!allPixel32(r, dst, kDstSentinel)) {
            lift_test::failf(__FILE__, __LINE__, "%s：目标被改了（⛔ 不许隐式裁剪 / 部分拷贝）", b.what);
            continue;
        }
        LIFT_CHECK(true);
    }
    // rect 指针不可读 ⇒ 同样是契约错（读不到就没有"整幅"这个缺省）。
    const Rig::Call bad_ptr = r.loadSurface(src, 0x00000010u, dst, dst_full, kFilterDefault, 0u);
    LIFT_CHECK_EQ_U32(bad_ptr.hr, lift::d3d9::kInvalidCall);
    LIFT_CHECK(bad_ptr.run.reason == lift::StopReason::UntilEa);
    LIFT_CHECK(allPixel32(r, dst, kDstSentinel));

    // ---- 洞 2：`src_surface == dst_surface` ⇒ D3DERR_INVALIDCALL ----
    //   ★ 为什么不是"支持自拷贝"：矩形错位时行区间重叠 ⇒ `memcpy` UB，且会把**尚未读到的源行**
    //     覆盖掉（像素搬花）。真机行为未取证 ⇒ 父级裁决 = 一律拒绝（⛔ 不猜"应该支持"）。
    //   ★ 这里用**同一块** surface + 目标偏移 (1,1)：宽度 < 面宽 ⇒ 行区间一定重叠。
    const std::uint32_t self_src = r.placeRect(0, 0, 5, 4);
    const std::uint32_t self_dst = r.placeRect(1, 1, 6, 5);
    const Rig::Call self = r.loadSurface(dst, self_src, dst, self_dst, kFilterDefault, 0u);
    LIFT_CHECK_EQ_U32(self.hr, lift::d3d9::kInvalidCall);
    LIFT_CHECK(self.run.reason == lift::StopReason::UntilEa);  // 契约错必须可继续
    LIFT_CHECK(allPixel32(r, dst, kDstSentinel));              // 目标一字节不变
    // 这些全是"可继续的失败"：不许记成 unimplemented / 不许 trap。
    LIFT_CHECK_EQ_U32(r.m.cpu().shim.unimplemented_calls, traps_before);
    LIFT_CHECK(r.m.cpu().stop_why == nullptr || std::strlen(r.m.cpu().stop_why) == 0);
}

// ---------------------------------------------------------------------------------------
// ⑤ 格式判据（纯函数）+ 16bpp 真 surface 的同格式拷贝 / 16↔32 拒绝
// ---------------------------------------------------------------------------------------
SurfaceCopyPlan planOf(std::uint32_t src, std::uint32_t dst) { return lift::d3d9::surfaceCopyPlan(src, dst); }

bool identityPlan(const SurfaceCopyPlan& p, std::uint32_t bpp) {
    return p.supported && p.bytes_per_pixel == bpp && p.byte_map[0] == 0u && p.byte_map[1] == 1u &&
           p.byte_map[2] == 2u && p.byte_map[3] == 3u;
}
bool swapRbPlan(const SurfaceCopyPlan& p) {
    return p.supported && p.bytes_per_pixel == 4u && p.byte_map[0] == 2u && p.byte_map[1] == 1u &&
           p.byte_map[2] == 0u && p.byte_map[3] == 3u;
}

void caseFormatPairs() {
    lift_test::g_current_case = "d3dx9/load-surface+format-pairs";
    using namespace lift::d3d9;
    // 同一个 D3DFORMAT ⇒ 恒等映射（32bpp / 16bpp 都是逐字节直拷）。
    LIFT_CHECK(identityPlan(planOf(kFmtA8R8G8B8, kFmtA8R8G8B8), 4u));
    LIFT_CHECK(identityPlan(planOf(kFmtX8R8G8B8, kFmtX8R8G8B8), 4u));
    LIFT_CHECK(identityPlan(planOf(kFmtR5G6B5, kFmtR5G6B5), 2u));
    LIFT_CHECK(identityPlan(planOf(kFmtA4R4G4B4, kFmtA4R4G4B4), 2u));
    // 32bpp 同序：布局相同，只差"顶字节算不算 alpha"。
    // ★ **丢 alpha 允许**（A8→X8 = 21→22）：X8 的顶字节不携带信息 ⇒ 丢了不造信息。
    LIFT_CHECK(identityPlan(planOf(kFmtA8R8G8B8, kFmtX8R8G8B8), 4u));
    // ★ **造 alpha 拒绝**（X8→A8 = 22→21，父级裁决）：X8 的顶字节不携带信息（`f.amask = 0`），
    //   照抄会把它写成目标的 alpha ⇒ 目标被采样 / 混色时像素变透明。⛔ 不猜 0xFF / 0x00。
    LIFT_CHECK(!planOf(kFmtX8R8G8B8, kFmtA8R8G8B8).supported);
    LIFT_CHECK(identityPlan(planOf(kFmtA8B8G8R8, kFmtX8B8G8R8), 4u));
    // ⚠ 邻臂（33→32 = X8B8G8R8→A8B8G8R8）是**同一个形状**，但 32/33 建不出真 surface、
    //   父级裁决只点名了 `22→21` ⇒ 这一格**保持原状**（⛔ 不越权改裁决；见需求单"判不了"）。
    LIFT_CHECK(identityPlan(planOf(kFmtX8B8G8R8, kFmtA8B8G8R8), 4u));
    // RGBA↔BGRA（{21,22} ↔ {32,33}）⇒ 只换 R/B（目标字节 0/2 取源字节 2/0），其余照抄。
    for (const std::uint32_t a : {kFmtA8R8G8B8, kFmtX8R8G8B8}) {
        for (const std::uint32_t b : {kFmtA8B8G8R8, kFmtX8B8G8R8}) {
            LIFT_CHECK(swapRbPlan(planOf(a, b)));
            LIFT_CHECK(swapRbPlan(planOf(b, a)));
        }
    }
    // 其它组合 ⇒ 拒绝（16bpp ↔ 32bpp、不同 16bpp、未知格式、深度格式）。
    LIFT_CHECK(!planOf(kFmtA8R8G8B8, kFmtR5G6B5).supported);
    LIFT_CHECK(!planOf(kFmtR5G6B5, kFmtA8R8G8B8).supported);
    LIFT_CHECK(!planOf(kFmtR5G6B5, kFmtA1R5G5B5).supported);
    LIFT_CHECK(!planOf(kFmtUnknown, kFmtA8R8G8B8).supported);
    LIFT_CHECK(!planOf(kFmtA8R8G8B8, kFmtUnknown).supported);
    LIFT_CHECK(!planOf(kFmtD24S8, kFmtD24S8).supported);

    // ---- 16bpp 真 surface：同格式逐字节搬运（按 bpp 走，不是硬拷 4 字节）----
    Rig r;
    LIFT_CHECK(r.boot());
    if (!r.ok) return;
    Surf src;
    Surf dst;
    LIFT_CHECK(r.makeSurface(4u, 4u, lift::d3d9::kFmtR5G6B5, &src));
    LIFT_CHECK(r.makeSurface(4u, 4u, lift::d3d9::kFmtR5G6B5, &dst));
    if (!r.ok) return;
    for (std::uint32_t y = 0; y < src.height; ++y)
        for (std::uint32_t x = 0; x < src.width; ++x)
            r.m.mem().wr16(src.ea + y * src.pitch + x * 2u,
                           static_cast<std::uint16_t>(0x8000u | (y << 8) | (x + 1u)));
    r.fill16(dst, 0xBEEFu);
    // 半开 [1,3)×[1,3)（2×2）→ 目标 (2,0)
    const std::uint32_t c = r.loadSurface(src, r.placeRect(1, 1, 3, 3), dst, r.placeRect(2, 0, 4, 2),
                                          kFilterDefault, 0u)
                                .hr;
    LIFT_CHECK_EQ_U32(c, lift::d3d9::kOk);
    bool bpp16_ok = true;
    for (std::uint32_t y = 0; y < dst.height; ++y) {
        for (std::uint32_t x = 0; x < dst.width; ++x) {
            const bool inside = x >= 2u && y < 2u;
            const std::uint16_t want = inside
                                           ? static_cast<std::uint16_t>(0x8000u | ((y + 1u) << 8) | (x - 2u + 1u + 1u))
                                           : static_cast<std::uint16_t>(0xBEEFu);
            if (r.pixel16(dst, x, y) != want) {
                lift_test::failf(__FILE__, __LINE__, "16bpp dst(%u,%u)=0x%04X, expected 0x%04X", x, y,
                                 r.pixel16(dst, x, y), want);
                bpp16_ok = false;
            }
        }
    }
    LIFT_CHECK(bpp16_ok);

    // ---- 16bpp ↔ 32bpp：真 surface ⇒ 响亮停（不实现位深变换），目标不变 ----
    Rig r2;
    LIFT_CHECK(r2.boot());
    if (!r2.ok) return;
    Surf src16;
    Surf dst32;
    LIFT_CHECK(r2.makeSurface(4u, 4u, lift::d3d9::kFmtR5G6B5, &src16));
    LIFT_CHECK(r2.makeSurface(4u, 4u, lift::d3d9::kFmtA8R8G8B8, &dst32));
    if (!r2.ok) return;
    r2.fill32(dst32, kDstSentinel);
    const Rig::Call bad = r2.loadSurface(src16, 0u, dst32, 0u, kFilterDefault, 0u);
    LIFT_CHECK(bad.run.reason == lift::StopReason::Trap);
    LIFT_CHECK(bad.hr != lift::d3d9::kOk);
    LIFT_CHECK(allPixel32(r2, dst32, kDstSentinel));

    // ---- 洞 1：**真 surface** 的 X8(22) → A8(21)，顶字节 0 ⇒ 响亮停 + 目标一字节不变 ----
    //   ★ 为什么必须是真 surface：纯函数只判"计划"，判不了"目标有没有被写"。这一格从 guest 代码
    //     经 IAT 桩进 shim，落到的就是"像素会不会被悄悄写成透明"。
    Rig r3;
    LIFT_CHECK(r3.boot());
    if (!r3.ok) return;
    Surf x8;
    Surf a8;
    LIFT_CHECK(r3.makeSurface(4u, 4u, kFmtX8R8G8B8, &x8));
    LIFT_CHECK(r3.makeSurface(4u, 4u, kFmtA8R8G8B8, &a8));
    if (!r3.ok) return;
    // 源顶字节 = 0（X8 的合法读数：这一字节不携带信息），RGB 给可辨认值。
    for (std::uint32_t y = 0; y < x8.height; ++y)
        for (std::uint32_t x = 0; x < x8.width; ++x)
            r3.m.mem().wr32(x8.ea + y * x8.pitch + x * 4u, 0x00A1B2C3u);
    r3.fill32(a8, kDstSentinel);
    const Rig::Call x8_to_a8 = r3.loadSurface(x8, 0u, a8, 0u, kFilterDefault, 0u);
    LIFT_CHECK(x8_to_a8.run.reason == lift::StopReason::Trap);
    LIFT_CHECK(x8_to_a8.hr != lift::d3d9::kOk);
    LIFT_CHECK(allPixel32(r3, a8, kDstSentinel));  // ⛔ 不许"填一个 0xFF 继续跑"
    // 反方向（A8 → X8 = 丢 alpha）**仍然允许**：逐字节直拷，顶字节原样照抄（丢信息 ≠ 造信息）。
    Surf a8_src;
    Surf x8_dst;
    LIFT_CHECK(r3.makeSurface(4u, 4u, kFmtA8R8G8B8, &a8_src));
    LIFT_CHECK(r3.makeSurface(4u, 4u, kFmtX8R8G8B8, &x8_dst));
    if (!r3.ok) return;
    r3.fill32(a8_src, 0xA1B2C3D4u);
    r3.fill32(x8_dst, kDstSentinel);
    const Rig::Call a8_to_x8 = r3.loadSurface(a8_src, 0u, x8_dst, 0u, kFilterDefault, 0u);
    LIFT_CHECK_EQ_U32(a8_to_x8.hr, lift::d3d9::kOk);
    LIFT_CHECK(a8_to_x8.run.reason == lift::StopReason::UntilEa);
    LIFT_CHECK(allPixel32(r3, x8_dst, 0xA1B2C3D4u));  // 顶字节照抄（X8 一侧不携带信息）
}

// ---------------------------------------------------------------------------------------
// ③ 只做 1:1：尺寸不等 ⇒ 响亮停（不是按左上角硬拷）
// ---------------------------------------------------------------------------------------
void caseOneToOne() {
    lift_test::g_current_case = "d3dx9/load-surface+one-to-one";
    Rig r;
    LIFT_CHECK(r.boot());
    if (!r.ok) return;
    Surf src;
    Surf dst;
    LIFT_CHECK(r.makeSurface(6u, 5u, lift::d3d9::kFmtA8R8G8B8, &src));
    LIFT_CHECK(r.makeSurface(6u, 5u, lift::d3d9::kFmtA8R8G8B8, &dst));
    if (!r.ok) return;
    r.fill32(src, 0xA1B2C3D4u);
    r.fill32(dst, kDstSentinel);
    const Rig::Call c = r.loadSurface(src, r.placeRect(1, 1, 3, 3), dst, r.placeRect(0, 0, 3, 3),
                                      kFilterDefault, 0u);
    LIFT_CHECK(c.run.reason == lift::StopReason::Trap);
    LIFT_CHECK(c.hr != lift::d3d9::kOk);
    LIFT_CHECK(allPixel32(r, dst, kDstSentinel));
    LIFT_CHECK(r.m.cpu().stop_why != nullptr);
    if (r.m.cpu().stop_why != nullptr) {
        // 停机原因必须点名这条导出（否则"停在哪"只能靠翻 stderr）。
        LIFT_CHECK(std::string(r.m.cpu().stop_why) == "unimplemented D3DX9 call");
    }
}

// ---------------------------------------------------------------------------------------
// ④ colorKey != 0 ⇒ 响亮停。★ 判据包含**拒绝的理由**：文档明写 ColorKey 是 "always a 32-bit
//   ARGB color" 且 "Alpha is significant" ⇒ 不是"无法定义"，是**本期没做**。
//   ⛔ 旧理由（"文档没写死比较口径"）是错的 ⇒ 谁把它改回去，这条用例就红。
// ---------------------------------------------------------------------------------------
void caseColorKey() {
    lift_test::g_current_case = "d3dx9/load-surface+color-key";
    Rig r;
    LIFT_CHECK(r.boot());
    if (!r.ok) return;
    Surf src;
    Surf dst;
    LIFT_CHECK(r.makeSurface(4u, 4u, lift::d3d9::kFmtA8R8G8B8, &src));
    LIFT_CHECK(r.makeSurface(4u, 4u, lift::d3d9::kFmtA8R8G8B8, &dst));
    if (!r.ok) return;
    r.fill32(src, 0xA1B2C3D4u);
    r.fill32(dst, kDstSentinel);
    const Rig::Call c = r.loadSurface(src, 0u, dst, 0u, kFilterDefault, 0xFF00FF00u);
    LIFT_CHECK(c.run.reason == lift::StopReason::Trap);
    LIFT_CHECK(c.hr != lift::d3d9::kOk);
    LIFT_CHECK(allPixel32(r, dst, kDstSentinel));

    // ---- 洞 3：理由文本是"本期未实现（文档已定义：32-bit ARGB、alpha 参与比较）"+ 文档链接 ----
    //   ★ 直接调语义层读 `why`：guest 侧的 `stop_why` 只有 "unimplemented D3DX9 call"（不含理由）。
    std::string why;
    const lift::d3d9::SurfaceCopyResult res =
        lift::d3d9::copySurfaceRect(r.m.cpu(), src.object, 0u, dst.object, 0u, 0xFF00FF00u, &why);
    LIFT_CHECK(res == lift::d3d9::SurfaceCopyResult::Unsupported);
    LIFT_CHECK(why.find("本期未实现") != std::string::npos);
    LIFT_CHECK(why.find("ARGB") != std::string::npos);
    LIFT_CHECK(why.find("alpha") != std::string::npos);
    LIFT_CHECK(why.find("learn.microsoft.com") != std::string::npos);
    // ⛔ 那条错误理由（"文档没写死"）不许再出现 —— 它是洞 3 的原文。
    LIFT_CHECK(why.find("没写死") == std::string::npos);
}

}  // namespace

int main() {
    using namespace lift_test;
    caseRectSemantics();
    caseContract();
    caseFormatPairs();
    caseOneToOne();
    caseColorKey();
    return finish("d3dx9-load-surface");
}
