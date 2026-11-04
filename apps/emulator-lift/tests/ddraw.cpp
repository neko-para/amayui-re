// emulator-lift · P1 / B2 夹具：DirectDraw 2D 核心（pure 档：不走游戏）
//
// 判据（plan §5 B2）：
//   ① 动态导出：`LoadLibraryA("DDRAW.DLL")` → `GetProcAddress("DirectDrawCreate")` → 调到**我们的 thunk**
//      → 得到 guest 可见对象；`QueryInterface(IID_IDirectDraw2)` 成功、`Release` 后 refcount 归零；
//   ② `CreateSurface` → surface 对象存在、`QueryInterface(IID_IDirectDrawSurface3)` 成功；
//   ③ `SetColorKey` + `Blt`：键色像素**不拷贝**（目标保持原值），非键色被覆盖；不带 KEYSRC 时键色也拷；
//   ④ `BltFast` 越界裁剪：源/目标都超出时夹紧、⛔ 不越界写（用**后一块 surface** 当 canary 断言）；
//   ⑤ `Lock` + `Unlock`：lPitch/lpSurface 往返、嵌套锁计数、多余的 Unlock 返回真实 DDERR_*；
//   ⑥ `GetDC` / `ReleaseDC` 配对（引用计数），且 HDC 能折算回**同一块** surface 像素；
//   ⑦ `Flip`：frameCount +1；同一串操作在**两台新机器**上 lastHash 相同（确定性）；
//   ⑧ `EnumDisplayModes`：guest 回调真的被调到、收到 ≥1 条模式（含 1280×720×32 / ×16）；
//      回调返回 DDENUMRET_CANCEL(0) ⇒ 提前终止。
//
// ★ 这些用例**会红**（变异验证见交付）：键色改成"永远复制" ⇒ ③ 红；裁剪切掉 ⇒ ④ 红。
//
// ★ 所有方法调用都从 **guest 代码** 经 vtable 调（`mov eax,[vtbl]; push …; call [eax+off]`），
//   这样测的是"guest 可见的对象"，不是宿主直接调函数。
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "check.h"
#include "runtime/ddraw_surface.h"
#include "runtime/machine.h"
#include "shims/register.h"

namespace {

constexpr std::uint32_t kCodeBase = 0x00401000u;
constexpr std::uint32_t kCodeStride = 0x1000u;
constexpr std::uint32_t kDataBase = 0x00420000u;
constexpr std::uint32_t kDataStride = 0x1000u;
constexpr std::uint32_t kStubPage = 0x7FF80000u;
constexpr std::uint32_t kStubSlots = 0x1000u;
constexpr std::uint32_t kProcLoadLibrary = 0x7FF81000u;
constexpr std::uint32_t kProcGetProcAddress = 0x7FF81004u;
constexpr std::uint32_t kProcDirectDrawCreate = 0x7FF81008u;
constexpr std::uint32_t kSentinel = 0xFFFFFFFFu;
constexpr std::uint32_t kFixtureHeapBytes = 0x00100000u;

// 32bpp 掩码（与 ddraw_surface 的 8888 一致）
constexpr std::uint32_t kGreen = 0x0000FF00u;
constexpr std::uint32_t kBlue = 0x000000FFu;

struct Asm {
    std::vector<std::uint8_t> b;

    std::uint32_t ea(std::uint32_t base) const {
        return base + static_cast<std::uint32_t>(b.size());
    }
    void u8(std::uint8_t v) { b.push_back(v); }
    void u16(std::uint16_t v) {
        u8(static_cast<std::uint8_t>(v & 0xFF));
        u8(static_cast<std::uint8_t>(v >> 8));
    }
    void u32(std::uint32_t v) {
        for (int i = 0; i < 4; ++i) u8(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF));
    }
    void pushImm(std::uint32_t v) {
        u8(0x68);
        u32(v);
    }
    void pushEax() { u8(0x50); }
    void movEaxImm(std::uint32_t v) {
        u8(0xB8);
        u32(v);
    }
    void movEaxMem(std::uint32_t ea) {  // mov eax, [imm32]
        u8(0xA1);
        u32(ea);
    }
    void movMemEax(std::uint32_t ea) {  // mov [imm32], eax
        u8(0xA3);
        u32(ea);
    }
    void movEaxMemEax() {  // mov eax, [eax]
        u8(0x8B);
        u8(0x00);
    }
    void movEcxMemEsp4() {  // mov ecx, [esp+4]
        u8(0x8B);
        u8(0x4C);
        u8(0x24);
        u8(0x04);
    }
    void movEaxMemEcxDisp8(std::uint8_t d) {  // mov eax, [ecx+d]
        u8(0x8B);
        u8(0x41);
        u8(d);
    }
    void movEbxMemEcxDisp8(std::uint8_t d) {  // mov ebx, [ecx+d]
        u8(0x8B);
        u8(0x59);
        u8(d);
    }
    void movMemEdxEbx() {  // mov [edx], ebx
        u8(0x89);
        u8(0x1A);
    }
    void movMemEdxDisp8Ebx(std::uint8_t d) {  // mov [edx+d], ebx
        u8(0x89);
        u8(0x5A);
        u8(d);
    }
    void movEdxEax() {  // mov edx, eax
        u8(0x8B);
        u8(0xD0);
    }
    void shlEdx4() {  // shl edx, 4
        u8(0xC1);
        u8(0xE2);
        u8(0x04);
    }
    void addEdxImm(std::uint32_t v) {  // add edx, imm32
        u8(0x81);
        u8(0xC2);
        u32(v);
    }
    void incEax() { u8(0x40); }
    void callRel(std::uint32_t base, std::uint32_t target) {
        const std::uint32_t at = ea(base);
        u8(0xE8);
        u32(static_cast<std::uint32_t>(static_cast<std::int32_t>(target - (at + 5))));
    }
    void callEax() {
        u8(0xFF);
        u8(0xD0);
    }
    void callMemEaxDisp8(std::uint8_t d) {
        u8(0xFF);
        u8(0x50);
        u8(d);
    }
    void callMemEaxDisp32(std::uint32_t d) {
        u8(0xFF);
        u8(0x90);
        u32(d);
    }
    void ret() { u8(0xC3); }
    void retImm(std::uint16_t n) {
        u8(0xC2);
        u16(n);
    }
};

struct Fixture {
    lift::Machine m;
    std::string err;
    std::uint32_t code_ea = kCodeBase;
    std::uint32_t code_next = 0x00406000u;  // 独立代码页（回调 / 不走 run() 的常驻代码）
    std::uint32_t data_ea = kDataBase;
    std::uint32_t thunk = 0;   // DirectDrawCreate 的 thunk（GetProcAddress 的返回值）
    std::uint32_t device = 0;  // IDirectDraw2 对象
    std::uint32_t vtable = 0;  // device 的 vtable EA

    bool boot() {
        if (!m.initBare(&err)) return false;
        if (!m.prepareStack(&err)) return false;
        if (!m.heap().init(&m.mem(), m.layout().heap_base, kFixtureHeapBytes)) return false;
        m.shims().configureStubPage(kStubPage, kStubSlots);
        lift::registerProcessShims(m.shims());
        lift::registerDdrawShims(m.shims());
        if (!hook("kernel32", "LoadLibraryA", kProcLoadLibrary)) return false;
        if (!hook("kernel32", "GetProcAddress", kProcGetProcAddress)) return false;
        if (!hook("ddraw", "DirectDrawCreate", kProcDirectDrawCreate)) return false;
        return true;
    }

    bool hook(const char* module, const char* name, std::uint32_t ea) {
        lift::ShimDef* d = m.shims().findMutable(module, name);
        if (!d) return false;
        d->stub_ea = ea;
        m.cache().installShim(ea, d->id);
        return true;
    }

    std::uint32_t placeData(const void* p, std::size_t n) {
        const std::uint32_t ea = data_ea;
        const std::uint32_t aligned = (static_cast<std::uint32_t>(n) + 15u) & ~15u;
        if (!m.placeData(ea, p, n, &err)) return 0;
        data_ea += (aligned < kDataStride) ? aligned : kDataStride;
        return ea;
    }
    std::uint32_t placeU32(std::uint32_t v) { return placeData(&v, 4); }
    std::uint32_t placeString(const char* s) { return placeData(s, std::string(s).size() + 1); }
    std::uint32_t placeBytes(const std::vector<std::uint8_t>& v) { return placeData(v.data(), v.size()); }
    std::uint32_t placeRect(int l, int t, int r, int b) {
        const std::uint32_t v[4] = {static_cast<std::uint32_t>(l), static_cast<std::uint32_t>(t),
                                    static_cast<std::uint32_t>(r), static_cast<std::uint32_t>(b)};
        return placeData(v, sizeof(v));
    }
    // 锁住一块 surface 的像素窗口（用于 canary 快照 / 断言）
    bool readPixels(std::uint32_t surf, std::uint32_t* ptr, std::uint32_t* pitch, int* w, int* h) {
        std::uint32_t bpp = 0;
        return lockPixels(surf, ptr, pitch, w, h, &bpp);
    }
    // 把 surface 的整块像素（含行尾 padding）做 FNV-1a（确定性；只读 guest 内存）
    bool hashSurface(std::uint32_t surf, std::uint64_t* out) {
        std::uint32_t ptr = 0;
        std::uint32_t pitch = 0;
        int w = 0;
        int h = 0;
        if (!readPixels(surf, &ptr, &pitch, &w, &h)) return false;
        std::uint64_t hash = 0xCBF29CE484222325ull;
        for (std::uint32_t i = 0; i < pitch * static_cast<std::uint32_t>(h); ++i) {
            hash ^= m.mem().rd8(ptr + i);
            hash *= 0x100000001B3ull;
        }
        *out = hash;
        return unlock(surf, ptr) == 0;
    }

    // 放一段 guest 代码并执行到 0xFFFFFFFF 哨兵；返回 EAX
    std::uint32_t run(Asm& a) {
        const std::uint32_t ea = code_ea;
        if (!m.placeCode(ea, a.b.data(), a.b.size(), &err)) return 0;
        code_ea += kCodeStride;
        m.cpu().eip = ea;
        m.cpu().esp() = m.layout().stack_top - 4u;
        m.mem().wr32(m.cpu().esp(), kSentinel);
        m.cpu().running = true;
        lift::RunOptions o;
        o.until_ea = kSentinel;
        o.max_insns = 200000;
        const lift::RunResult r = m.run(o);
        if (r.reason != lift::StopReason::UntilEa) {
            lift_test::failf(__FILE__, __LINE__, "guest 代码没有走到哨兵（reason=%s: %s）",
                             lift::stopReasonName(r.reason), r.detail.c_str());
        }
        return m.cpu().eax();
    }

    // 一段**代码**（放进独立的代码页；⛔ 不要和数据混在同一页 —— P0 的 SMC 会因"写到了已解码
    // 的代码页"清空整块缓存，连带丢掉 shim 块，下一次 shim 调用就会 decode-error）
    std::uint32_t placeCodeBlob(const std::vector<std::uint8_t>& v) {
        const std::uint32_t ea = code_next;
        if (!m.placeCode(ea, v.data(), v.size(), &err)) return 0;
        code_next += kCodeStride;
        return ea;
    }

    // ①②③… 调用点：guest 侧的 COM 调用
    std::uint32_t method(std::uint32_t obj, std::uint32_t offset,
                         const std::vector<std::uint32_t>& args) {
        const std::uint32_t vtbl = m.mem().rd32(obj);
        Asm a;
        a.movEaxImm(vtbl);
        for (std::size_t i = args.size(); i > 0; --i) a.pushImm(args[i - 1]);
        a.pushImm(obj);
        if (offset <= 0x7Fu) a.callMemEaxDisp8(static_cast<std::uint8_t>(offset));
        else a.callMemEaxDisp32(offset);
        a.ret();
        return run(a);
    }

    // ① LoadLibraryA + GetProcAddress + DirectDrawCreate
    bool createDirectDraw() {
        const std::uint32_t mod = placeString("DDRAW.DLL");
        const std::uint32_t proc = placeString("DirectDrawCreate");
        const std::uint32_t hmod = placeU32(0);
        const std::uint32_t thunk_slot = placeU32(0);
        const std::uint32_t out = placeU32(0);
        Asm a;
        a.pushImm(mod);
        a.callRel(kCodeBase, kProcLoadLibrary);
        a.movMemEax(hmod);
        a.pushImm(proc);
        a.pushEax();
        a.callRel(kCodeBase, kProcGetProcAddress);
        a.movMemEax(thunk_slot);
        a.pushImm(0);       // pUnkOuter
        a.pushImm(out);     // lplpDD
        a.pushImm(0);       // lpGUID（引擎传 0 或 2；这里用 0）
        a.callEax();        // ★ 动态导出：call eax
        a.ret();
        const std::uint32_t hr = run(a);
        LIFT_CHECK_EQ_U32(hr, 0u);
        LIFT_CHECK_NE_U32(m.mem().rd32(hmod), 0u);
        LIFT_CHECK_NE_U32(m.mem().rd32(thunk_slot), 0u);
        thunk = m.mem().rd32(thunk_slot);
        device = m.mem().rd32(out);
        LIFT_CHECK_NE_U32(device, 0u);
        if (device) vtable = m.mem().rd32(device);
        LIFT_CHECK_NE_U32(vtable, 0u);
        return device != 0 && thunk != 0;
    }

    // ② CreateSurface：desc 在 guest 里填（dwSize=108 / dwFlags=7 / 宽高 / caps）
    std::uint32_t createSurface(int w, int h, std::uint32_t caps, std::uint32_t* out_obj = nullptr) {
        std::vector<std::uint8_t> desc(lift::ddraw::kDescBytes, 0);
        auto wr = [&desc](std::uint32_t off, std::uint32_t v) {
            for (int i = 0; i < 4; ++i)
                desc[off + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF);
        };
        wr(lift::ddraw::kOffSize, lift::ddraw::kDescBytes);
        wr(lift::ddraw::kOffFlags, lift::ddraw::kDdsdCaps | lift::ddraw::kDdsdWidth | lift::ddraw::kDdsdHeight);
        wr(lift::ddraw::kOffWidth, static_cast<std::uint32_t>(w));
        wr(lift::ddraw::kOffHeight, static_cast<std::uint32_t>(h));
        wr(lift::ddraw::kOffCaps, caps);
        const std::uint32_t desc_ea = placeBytes(desc);
        const std::uint32_t out = placeU32(0);
        const std::uint32_t hr = method(device, 24u, {desc_ea, out, 0u});
        const std::uint32_t obj = m.mem().rd32(out);
        if (out_obj) *out_obj = obj;
        if (hr != 0) return hr;
        LIFT_CHECK_NE_U32(obj, 0u);
        return hr;
    }

    // 一块零初始化、dwSize=108 的 DDSURFACEDESC（真机要求调用方先填 dwSize）
    std::uint32_t newDesc() {
        std::vector<std::uint8_t> desc(lift::ddraw::kDescBytes, 0);
        desc[lift::ddraw::kOffSize + 0] = static_cast<std::uint8_t>(lift::ddraw::kDescBytes & 0xFF);
        desc[lift::ddraw::kOffSize + 1] = static_cast<std::uint8_t>((lift::ddraw::kDescBytes >> 8) & 0xFF);
        return placeBytes(desc);
    }

    // Lock 一次拿像素视图（desc 在 guest 里）
    bool lockPixels(std::uint32_t surf, std::uint32_t* ptr, std::uint32_t* pitch, int* w, int* h,
                    std::uint32_t* bpp) {
        const std::uint32_t desc_ea = newDesc();
        const std::uint32_t hr = method(surf, 100u, {0u, desc_ea, lift::ddraw::kLockWait, 0u});
        LIFT_CHECK_EQ_U32(hr, 0u);
        *ptr = m.mem().rd32(desc_ea + lift::ddraw::kOffLpSurface);
        *pitch = m.mem().rd32(desc_ea + lift::ddraw::kOffPitch);
        *w = static_cast<int>(m.mem().rd32(desc_ea + lift::ddraw::kOffWidth));
        *h = static_cast<int>(m.mem().rd32(desc_ea + lift::ddraw::kOffHeight));
        *bpp = m.mem().rd32(desc_ea + lift::ddraw::kOffPixelFormat + lift::ddraw::kPfBitCount);
        return hr == 0;
    }

    std::uint32_t unlock(std::uint32_t surf, std::uint32_t ptr) {
        return method(surf, 128u, {ptr});
    }

    void fillSurface(std::uint32_t surf, std::uint32_t value, int count) {
        std::uint32_t ptr = 0;
        std::uint32_t pitch = 0;
        int w = 0;
        int h = 0;
        std::uint32_t bpp = 0;
        LIFT_CHECK(lockPixels(surf, &ptr, &pitch, &w, &h, &bpp));
        const int total = (count > 0 && count < w * h) ? count : w * h;
        for (int i = 0; i < total; ++i) {
            const int x = i % w;
            const int y = i / w;
            m.mem().wr32(ptr + static_cast<std::uint32_t>(y) * pitch + static_cast<std::uint32_t>(x) * 4u,
                         value);
        }
        LIFT_CHECK_EQ_U32(unlock(surf, ptr), 0u);
    }
};

// ---------------------------------------------------------------------------------------
// ① 动态导出 + DirectDrawCreate + QI + Release（refcount）
// ---------------------------------------------------------------------------------------
void caseCreateAndQueryInterface() {
    lift_test::g_current_case = "ddraw/create+qi+release";
    Fixture f;
    LIFT_CHECK(f.boot());
    LIFT_CHECK(f.createDirectDraw());
    if (f.device == 0) return;
    LIFT_CHECK_EQ_U32(f.m.cpu().com->refCount(f.device), 1u);

    // IID_IDirectDraw2（与 AGE.EXE 在 0x52E268 的 16 字节一致；SDK ddraw.h 交叉核对）
    const std::uint8_t iid[16] = {0xE0, 0xF3, 0xA6, 0xB3, 0x43, 0x2B, 0xCF, 0x11,
                                  0xA2, 0xDE, 0x00, 0xAA, 0x00, 0xB9, 0x33, 0x56};
    const std::uint32_t iid_ea = f.placeBytes(std::vector<std::uint8_t>(iid, iid + 16));
    const std::uint32_t ppv = f.placeU32(0xDEADBEEF);

    // QI(IID_IDirectDraw2) 必须成功（引擎的「IDirectDraw2の作成に失敗しました」）
    LIFT_CHECK_EQ_U32(f.method(f.device, 0u, {iid_ea, ppv}), 0u);
    LIFT_CHECK_EQ_U32(f.m.mem().rd32(ppv), f.device);
    LIFT_CHECK_EQ_U32(f.m.cpu().com->refCount(f.device), 2u);

    // 引擎随后 Release 原对象 ⇒ 回到 1
    LIFT_CHECK_EQ_U32(f.method(f.device, 8u, {}), 1u);
    LIFT_CHECK_EQ_U32(f.m.cpu().com->refCount(f.device), 1u);
    LIFT_CHECK(f.m.cpu().com->find(f.device));

    // 未知 IID ⇒ E_NOINTERFACE（0x80004002），且不写 ppv
    std::uint8_t bogus[16] = {0};
    bogus[0] = 0x42;
    const std::uint32_t bogus_ea = f.placeBytes(std::vector<std::uint8_t>(bogus, bogus + 16));
    const std::uint32_t ppv2 = f.placeU32(0xDEADBEEF);
    LIFT_CHECK_EQ_U32(f.method(f.device, 0u, {bogus_ea, ppv2}), 0x80004002u);
    LIFT_CHECK_EQ_U32(f.m.mem().rd32(ppv2), 0u);

    // 再 Release 一次 ⇒ refcount 到 0，对象被释放（guest 堆归还）
    LIFT_CHECK_EQ_U32(f.method(f.device, 8u, {}), 0u);
    LIFT_CHECK(!f.m.cpu().com->find(f.device));
    LIFT_CHECK_EQ_U32(f.m.cpu().com->objectCount(), 0u);
}

// ---------------------------------------------------------------------------------------
// ② CreateSurface + QI(IID_IDirectDrawSurface3)
// ---------------------------------------------------------------------------------------
void caseCreateSurface() {
    lift_test::g_current_case = "ddraw/create-surface+qi";
    Fixture f;
    LIFT_CHECK(f.boot());
    LIFT_CHECK(f.createDirectDraw());
    if (f.device == 0) return;
    std::uint32_t surf = 0;
    LIFT_CHECK_EQ_U32(f.createSurface(64, 48, 0x00000040u /* OFFSCREENPLAIN */, &surf), 0u);
    if (surf == 0) return;
    const std::uint8_t iid[16] = {0x00, 0x4E, 0x04, 0xDA, 0xB2, 0x69, 0xD0, 0x11,
                                  0xA1, 0xD5, 0x00, 0xAA, 0x00, 0xB8, 0xDF, 0xBB};
    const std::uint32_t iid_ea = f.placeBytes(std::vector<std::uint8_t>(iid, iid + 16));
    const std::uint32_t ppv = f.placeU32(0);
    LIFT_CHECK_EQ_U32(f.method(surf, 0u, {iid_ea, ppv}), 0u);
    LIFT_CHECK_EQ_U32(f.m.mem().rd32(ppv), surf);
    LIFT_CHECK_EQ_U32(f.m.cpu().com->refCount(surf), 2u);
    // 引擎随后 Release 原对象 ⇒ 1
    LIFT_CHECK_EQ_U32(f.method(surf, 8u, {}), 1u);
    // GetCaps(+56) 回填的是创建时请求的 caps（引擎 0x43CB10 用它判 0x4000/0x800）
    const std::uint32_t caps_ea = f.placeU32(0);
    LIFT_CHECK_EQ_U32(f.method(surf, 56u, {caps_ea}), 0u);
    LIFT_CHECK_EQ_U32(f.m.mem().rd32(caps_ea), 0x00000040u);
    // 像素在 guest 内存：Lock 的 lpSurface 指向 arena（非宿主指针），pitch = 64*4
    std::uint32_t ptr = 0;
    std::uint32_t pitch = 0;
    int w = 0;
    int h = 0;
    std::uint32_t bpp = 0;
    LIFT_CHECK(f.lockPixels(surf, &ptr, &pitch, &w, &h, &bpp));
    LIFT_CHECK_EQ_U32(pitch, 256u);
    LIFT_CHECK_EQ_I64(w, 64);
    LIFT_CHECK_EQ_I64(h, 48);
    LIFT_CHECK_EQ_U32(bpp, 32u);
    LIFT_CHECK(ptr >= 0x28000000u && ptr < 0x30000000u);  // SurfaceArena 区间
    LIFT_CHECK_EQ_U32(f.unlock(surf, ptr), 0u);
}

// ---------------------------------------------------------------------------------------
// ③ SetColorKey + Blt：键色不拷贝
// ---------------------------------------------------------------------------------------
void caseBltColorKey() {
    lift_test::g_current_case = "ddraw/blt-colorkey";
    Fixture f;
    LIFT_CHECK(f.boot());
    LIFT_CHECK(f.createDirectDraw());
    if (f.device == 0) return;
    std::uint32_t dst = 0;
    std::uint32_t src = 0;
    LIFT_CHECK_EQ_U32(f.createSurface(4, 4, 0x40u, &dst), 0u);
    LIFT_CHECK_EQ_U32(f.createSurface(4, 4, 0x40u, &src), 0u);
    if (!dst || !src) return;
    // 目标先填已知色；源：第 0 个像素是键色，其余是另一色
    f.fillSurface(dst, kBlue, 16);
    {
        std::uint32_t ptr = 0;
        std::uint32_t pitch = 0;
        int w = 0;
        int h = 0;
        std::uint32_t bpp = 0;
        LIFT_CHECK(f.lockPixels(src, &ptr, &pitch, &w, &h, &bpp));
        for (int i = 0; i < 16; ++i) {
            const std::uint32_t v = (i == 0) ? kGreen : 0x55667788u;
            f.m.mem().wr32(ptr + static_cast<std::uint32_t>(i / 4) * pitch +
                               static_cast<std::uint32_t>(i % 4) * 4u,
                           v);
        }
        LIFT_CHECK_EQ_U32(f.unlock(src, ptr), 0u);
    }
    // SetColorKey(+116) flags=8（DDCKEY_SRCBLT），键色 = 0x0000FF00
    // DDCOLORKEY 是**两个连续的 DWORD**（dwColorSpaceLowValue / HighValue），所以一次放 8 字节
    const std::uint32_t key_ea = f.placeBytes([&] {
        std::vector<std::uint8_t> k(8, 0);
        for (int i = 0; i < 4; ++i) {
            k[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>((kGreen >> (8 * i)) & 0xFF);
            k[4 + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>((kGreen >> (8 * i)) & 0xFF);
        }
        return k;
    }());
    LIFT_CHECK_EQ_U32(f.method(src, 116u, {8u, key_ea}), 0u);
    // Blt(+20)：dest, src, srcRect=NULL, flags=DDBLT_KEYSRC(0x8000), fx=NULL
    LIFT_CHECK_EQ_U32(f.method(dst, 20u, {0u, src, 0u, 0x00008000u, 0u}), 0u);
    {
        std::uint32_t ptr = 0;
        std::uint32_t pitch = 0;
        int w = 0;
        int h = 0;
        std::uint32_t bpp = 0;
        LIFT_CHECK(f.lockPixels(dst, &ptr, &pitch, &w, &h, &bpp));
        LIFT_CHECK_EQ_U32(f.m.mem().rd32(ptr), kBlue);          // ★ 键色位置**不变**
        LIFT_CHECK_EQ_U32(f.m.mem().rd32(ptr + 4u), 0x55667788u);
        LIFT_CHECK_EQ_U32(f.m.mem().rd32(ptr + 3u * pitch + 12u), 0x55667788u);
        LIFT_CHECK_EQ_U32(f.unlock(dst, ptr), 0u);
    }
    // 不带 KEYSRC 再 Blt 一次 ⇒ 键色**也要拷**（证明上面的"不变"来自键，不是"什么都没拷"）
    LIFT_CHECK_EQ_U32(f.method(dst, 20u, {0u, src, 0u, 0u, 0u}), 0u);
    {
        std::uint32_t ptr = 0;
        std::uint32_t pitch = 0;
        int w = 0;
        int h = 0;
        std::uint32_t bpp = 0;
        LIFT_CHECK(f.lockPixels(dst, &ptr, &pitch, &w, &h, &bpp));
        LIFT_CHECK_EQ_U32(f.m.mem().rd32(ptr), kGreen);
        LIFT_CHECK_EQ_U32(f.unlock(dst, ptr), 0u);
    }
}

// ---------------------------------------------------------------------------------------
// ④ BltFast 越界裁剪 + canary（不越界写）
// ---------------------------------------------------------------------------------------
void caseBltFastClip() {
    lift_test::g_current_case = "ddraw/bltfast-clip";
    Fixture f;
    LIFT_CHECK(f.boot());
    LIFT_CHECK(f.createDirectDraw());
    if (f.device == 0) return;
    std::uint32_t dst = 0;
    std::uint32_t src = 0;
    std::uint32_t canary = 0;
    LIFT_CHECK_EQ_U32(f.createSurface(8, 8, 0x40u, &dst), 0u);
    LIFT_CHECK_EQ_U32(f.createSurface(4, 4, 0x40u, &src), 0u);
    LIFT_CHECK_EQ_U32(f.createSurface(8, 8, 0x40u, &canary), 0u);  // 后一块：越界写会踩到它
    if (!dst || !src || !canary) return;
    f.fillSurface(dst, 0xAAAAAAAAu, 64);
    f.fillSurface(canary, 0xCAFEBABEu, 64);
    {
        std::uint32_t ptr = 0;
        std::uint32_t pitch = 0;
        int w = 0;
        int h = 0;
        std::uint32_t bpp = 0;
        LIFT_CHECK(f.lockPixels(src, &ptr, &pitch, &w, &h, &bpp));
        for (int i = 0; i < 16; ++i)
            f.m.mem().wr32(ptr + static_cast<std::uint32_t>(i / 4) * pitch +
                               static_cast<std::uint32_t>(i % 4) * 4u,
                           0x100u + static_cast<std::uint32_t>(i));
        LIFT_CHECK_EQ_U32(f.unlock(src, ptr), 0u);
    }
    std::uint64_t canary_before = 0;
    LIFT_CHECK(f.hashSurface(canary, &canary_before));

    // 情形 A：源矩形 {2,2,6,6} 超出 4x4 源 ⇒ 夹到 {2,2,4,4}；目标起点 (7,7) 只写 (7,7)
    const std::uint32_t sr = f.placeRect(2, 2, 6, 6);
    LIFT_CHECK_EQ_U32(f.method(dst, 28u, {7u, 7u, src, sr, 0u}), 0u);
    // 情形 B：整块拷贝到 (0,0)，源矩形完全在内 ⇒ dest(0..3,0..3) 全部覆盖
    LIFT_CHECK_EQ_U32(f.method(dst, 28u, {0u, 0u, src, 0u, 0u}), 0u);
    {
        std::uint32_t ptr = 0;
        std::uint32_t pitch = 0;
        int w = 0;
        int h = 0;
        std::uint32_t bpp = 0;
        LIFT_CHECK(f.lockPixels(dst, &ptr, &pitch, &w, &h, &bpp));
        LIFT_CHECK_EQ_U32(f.m.mem().rd32(ptr + 7u * pitch + 7u * 4u), 0x100u + 10u);  // src(2,2)
        for (int y = 0; y < 4; ++y)
            for (int x = 0; x < 4; ++x)
                LIFT_CHECK_EQ_U32(f.m.mem().rd32(ptr + static_cast<std::uint32_t>(y) * pitch +
                                                 static_cast<std::uint32_t>(x) * 4u),
                                  0x100u + static_cast<std::uint32_t>(y * 4 + x));
        // (4,0) 没被写（情形 B 只覆盖 0..3）
        LIFT_CHECK_EQ_U32(f.m.mem().rd32(ptr + 4u * 4u), 0xAAAAAAAAu);
        LIFT_CHECK_EQ_U32(f.unlock(dst, ptr), 0u);
    }
    std::uint64_t canary_after = 0;
    LIFT_CHECK(f.hashSurface(canary, &canary_after));
    LIFT_CHECK(canary_before == canary_after);  // ★ 不越界写（后一块 surface 逐字节未变）
}

// ---------------------------------------------------------------------------------------
// ⑤ Lock / Unlock：desc 往返、嵌套锁、多余 Unlock 的真实 DDERR
// ---------------------------------------------------------------------------------------
void caseLockUnlock() {
    lift_test::g_current_case = "ddraw/lock-unlock";
    Fixture f;
    LIFT_CHECK(f.boot());
    LIFT_CHECK(f.createDirectDraw());
    if (f.device == 0) return;
    std::uint32_t surf = 0;
    LIFT_CHECK_EQ_U32(f.createSurface(5, 3, 0x40u, &surf), 0u);
    if (surf == 0) return;
    std::uint32_t ptr = 0;
    std::uint32_t pitch = 0;
    int w = 0;
    int h = 0;
    std::uint32_t bpp = 0;
    LIFT_CHECK(f.lockPixels(surf, &ptr, &pitch, &w, &h, &bpp));
    LIFT_CHECK_EQ_U32(pitch, 20u);  // 5 * 4
    LIFT_CHECK_EQ_I64(w, 5);
    LIFT_CHECK_EQ_I64(h, 3);
    LIFT_CHECK_EQ_U32(bpp, 32u);

    // 嵌套锁：第二次 Lock 返回同一个 lpSurface；两次 Unlock 都成功，第三次失败
    const std::uint32_t desc_ea = f.newDesc();
    LIFT_CHECK_EQ_U32(f.method(surf, 100u, {0u, desc_ea, 1u, 0u}), 0u);
    LIFT_CHECK_EQ_U32(f.m.mem().rd32(desc_ea + lift::ddraw::kOffLpSurface), ptr);
    LIFT_CHECK_EQ_U32(f.unlock(surf, ptr), 0u);
    LIFT_CHECK_EQ_U32(f.unlock(surf, ptr), 0u);
    LIFT_CHECK_EQ_U32(f.unlock(surf, ptr), 0x88760248u);  // DDERR_NOTLOCKED

    // 像素写入后重新 Lock 能读回（lpSurface 指向 guest 内存）
    LIFT_CHECK(f.lockPixels(surf, &ptr, &pitch, &w, &h, &bpp));
    f.m.mem().wr32(ptr, 0x12345678u);
    LIFT_CHECK_EQ_U32(f.unlock(surf, ptr), 0u);
    LIFT_CHECK(f.lockPixels(surf, &ptr, &pitch, &w, &h, &bpp));
    LIFT_CHECK_EQ_U32(f.m.mem().rd32(ptr), 0x12345678u);
    LIFT_CHECK_EQ_U32(f.unlock(surf, ptr), 0u);

    // 越界矩形 ⇒ 真实 DDERR_INVALIDRECT（0x88760096）
    const std::uint32_t bad = f.placeRect(100, 100, 120, 120);
    const std::uint32_t desc2 = f.newDesc();
    LIFT_CHECK_EQ_U32(f.method(surf, 100u, {bad, desc2, 1u, 0u}), 0x88760096u);
    // 参数非法（desc 为 NULL）⇒ DDERR_INVALIDPARAMS
    LIFT_CHECK_EQ_U32(f.method(surf, 100u, {0u, 0u, 1u, 0u}), 0x80070057u);
}

// ---------------------------------------------------------------------------------------
// ⑥ GetDC / ReleaseDC：配对 + HDC 能折算回同一块像素
// ---------------------------------------------------------------------------------------
void caseGetDc() {
    lift_test::g_current_case = "ddraw/getdc-release";
    Fixture f;
    LIFT_CHECK(f.boot());
    LIFT_CHECK(f.createDirectDraw());
    if (f.device == 0) return;
    std::uint32_t surf = 0;
    LIFT_CHECK_EQ_U32(f.createSurface(8, 4, 0x40u, &surf), 0u);
    if (surf == 0) return;
    const std::uint32_t out = f.placeU32(0);
    LIFT_CHECK_EQ_U32(f.method(surf, 68u, {out}), 0u);
    const std::uint32_t hdc = f.m.mem().rd32(out);
    LIFT_CHECK_NE_U32(hdc, 0u);
    // 第二次 GetDC ⇒ 同一个 HDC（引用计数 +1）
    const std::uint32_t out2 = f.placeU32(0);
    LIFT_CHECK_EQ_U32(f.method(surf, 68u, {out2}), 0u);
    LIFT_CHECK_EQ_U32(f.m.mem().rd32(out2), hdc);
    // 有 DC 时 Lock 必须失败（DDERR_SURFACEBUSY = 0x887601AE）
    const std::uint32_t desc = f.newDesc();
    LIFT_CHECK_EQ_U32(f.method(surf, 100u, {0u, desc, 1u, 0u}), 0x887601AEu);
    // P1→P2 接缝：HDC 能折算回**同一块** surface 像素
    lift::ddraw::DcInfo info;
    LIFT_CHECK(lift::ddraw::dcInfo(f.m.cpu(), hdc, &info));
    LIFT_CHECK_EQ_U32(info.pitch, 32u);  // 8*4
    LIFT_CHECK_EQ_I64(info.width, 8);
    LIFT_CHECK_EQ_I64(info.height, 4);
    LIFT_CHECK_EQ_U32(info.format.bpp, 32u);
    // 往那块像素写一个值，ReleaseDC 后 Lock 能读到（DC 与 surface 是同一块内存）
    f.m.mem().wr32(info.pixels_ea + 3u * info.pitch + 4u, 0x0BADF00Du);
    LIFT_CHECK_EQ_U32(f.method(surf, 104u, {hdc}), 0u);
    LIFT_CHECK_EQ_U32(f.method(surf, 104u, {hdc}), 0u);
    LIFT_CHECK_EQ_U32(f.method(surf, 104u, {hdc}), 0x88760082u);  // 多于 GetDC ⇒ DDERR_INVALIDOBJECT
    std::uint32_t ptr = 0;
    std::uint32_t pitch = 0;
    int w = 0;
    int h = 0;
    std::uint32_t bpp = 0;
    LIFT_CHECK(f.lockPixels(surf, &ptr, &pitch, &w, &h, &bpp));
    LIFT_CHECK_EQ_U32(f.m.mem().rd32(ptr + 3u * pitch + 4u), 0x0BADF00Du);
    LIFT_CHECK_EQ_U32(f.unlock(surf, ptr), 0u);
}

// ---------------------------------------------------------------------------------------
// ⑦ Flip：frameCount +1 / 两次运行 lastHash 相同
// ---------------------------------------------------------------------------------------
struct FlipResult {
    bool ok = false;
    std::uint64_t frames = 0;
    std::uint64_t hash = 0;
};

FlipResult runFlipFixture() {
    FlipResult r;
    Fixture f;
    if (!f.boot()) return r;
    if (!f.createDirectDraw()) return r;
    if (f.device == 0) return r;
    std::uint32_t primary = 0;
    if (f.createSurface(4, 4, 0x00000200u /* PRIMARYSURFACE */, &primary) != 0) return r;
    if (primary == 0) return r;
    // 确定性地填 primary（Lock → 写像素 → Unlock）
    std::uint32_t ptr = 0;
    std::uint32_t pitch = 0;
    int w = 0;
    int h = 0;
    std::uint32_t bpp = 0;
    if (!f.lockPixels(primary, &ptr, &pitch, &w, &h, &bpp)) return r;
    for (int i = 0; i < w * h; ++i)
        f.m.mem().wr32(ptr + static_cast<std::uint32_t>(i / w) * pitch +
                           static_cast<std::uint32_t>(i % w) * 4u,
                       0x300u + static_cast<std::uint32_t>(i));
    if (f.unlock(primary, ptr) != 0) return r;
    // Flip(+44)：back=NULL, flags=DDFLIP_WAIT(1)
    if (f.method(primary, 44u, {0u, 1u}) != 0) return r;
    r.frames = f.m.presenter().frameCount();
    r.hash = f.m.presenter().lastHash();
    // 第二次 Flip（内容不变）⇒ 同一个 hash
    if (f.method(primary, 44u, {0u, 1u}) != 0) return r;
    r.frames = f.m.presenter().frameCount();
    if (f.m.presenter().lastHash() != r.hash) return r;
    r.ok = true;

    // 非 primary 的 Flip ⇒ DDERR_NOTFLIPPABLE
    std::uint32_t off = 0;
    if (f.createSurface(4, 4, 0x40u, &off) == 0 && off != 0) {
        if (f.method(off, 44u, {0u, 1u}) != 0x88760246u) r.ok = false;
    }
    return r;
}

void caseFlip() {
    lift_test::g_current_case = "ddraw/flip";
    const FlipResult a = runFlipFixture();
    LIFT_CHECK(a.ok);
    LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(a.frames), 2u);
    LIFT_CHECK_NE_U32(static_cast<std::uint32_t>(a.hash), 0u);
    const FlipResult b = runFlipFixture();
    LIFT_CHECK(b.ok);
    LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(b.frames), static_cast<std::uint32_t>(a.frames));
    LIFT_CHECK(a.hash == b.hash);  // ★ 确定性：两台新机器同输入 ⇒ 同帧哈希
}

// ---------------------------------------------------------------------------------------
// ⑧ EnumDisplayModes：guest 回调真的被调到；CANCEL(0) 提前终止
// ---------------------------------------------------------------------------------------
void caseEnumDisplayModes() {
    lift_test::g_current_case = "ddraw/enum-display-modes";
    Fixture f;
    LIFT_CHECK(f.boot());
    LIFT_CHECK(f.createDirectDraw());
    if (f.device == 0) return;
    const std::uint32_t count_ea = f.placeU32(0);
    const std::uint32_t ret_ea = f.placeU32(1);  // 1 = DDENUMRET_OK（继续）
    const std::uint32_t arr = f.placeBytes(std::vector<std::uint8_t>(64 * 16, 0));

    // guest 回调 (lpDesc, lpContext)：
    //   记录 (width,height,bpp) 到 arr[count]（每项 16 字节），++count，返回 *ret_ea
    Asm cb;
    cb.movEcxMemEsp4();                            // ecx = lpDesc
    cb.movEaxMem(count_ea);                        // eax = count
    cb.movEdxEax();                                // edx = count
    cb.shlEdx4();                                  // *16
    cb.addEdxImm(arr);
    cb.movEbxMemEcxDisp8(lift::ddraw::kOffWidth);
    cb.movMemEdxEbx();
    cb.movEbxMemEcxDisp8(lift::ddraw::kOffHeight);
    cb.movMemEdxDisp8Ebx(4);
    cb.movEbxMemEcxDisp8(static_cast<std::uint8_t>(lift::ddraw::kOffPixelFormat + lift::ddraw::kPfBitCount));
    cb.movMemEdxDisp8Ebx(8);
    cb.incEax();
    cb.movMemEax(count_ea);
    cb.movEaxMem(ret_ea);
    cb.retImm(8);  // stdcall：两个参数
    const std::uint32_t cb_ea = f.placeCodeBlob(cb.b);

    auto enum_call = [&f, cb_ea](std::uint32_t ctx) {
        Asm a;
        a.movEaxImm(f.m.mem().rd32(f.device));
        a.pushImm(cb_ea);  // 回调
        a.pushImm(ctx);    // lpContext
        a.pushImm(0);      // lpDesc = NULL
        a.pushImm(1);      // DDEDM_REFRESHRATES
        a.pushImm(f.device);
        a.callMemEaxDisp8(0x20u);
        a.ret();
        return f.run(a);
    };

    // 第一轮：一直返回 1 ⇒ 30 条（固定模式表）；含 1280x720x32 与 1280x720x16
    LIFT_CHECK_EQ_U32(enum_call(0u), 0u);
    const std::uint32_t n = f.m.mem().rd32(count_ea);
    LIFT_CHECK_EQ_U32(n, 30u);
    bool saw32 = false;
    bool saw16 = false;
    for (std::uint32_t i = 0; i < n; ++i) {
        const std::uint32_t base = arr + i * 16u;
        const std::uint32_t w = f.m.mem().rd32(base);
        const std::uint32_t h = f.m.mem().rd32(base + 4u);
        const std::uint32_t b = f.m.mem().rd32(base + 8u);
        LIFT_CHECK(w != 0 && h != 0);
        if (w == 1280 && h == 720 && b == 32) saw32 = true;
        if (w == 1280 && h == 720 && b == 16) saw16 = true;
    }
    LIFT_CHECK(saw32);
    LIFT_CHECK(saw16);

    // 第二轮：回调返回 0 = DDENUMRET_CANCEL ⇒ 只回调一次就终止
    f.m.mem().wr32(count_ea, 0u);
    f.m.mem().wr32(ret_ea, 0u);
    LIFT_CHECK_EQ_U32(enum_call(0u), 0u);
    LIFT_CHECK_EQ_U32(f.m.mem().rd32(count_ea), 1u);
}

}  // namespace

int main() {
    using namespace lift_test;
    try {
        caseCreateAndQueryInterface();
        caseCreateSurface();
        caseBltColorKey();
        caseBltFastClip();
        caseLockUnlock();
        caseGetDc();
        caseFlip();
        caseEnumDisplayModes();
        return finish("ddraw");
    } catch (const std::exception& e) {
        std::fprintf(stderr, "DIAG uncaught std::exception: %s\n", e.what());
        return 1;
    } catch (...) {
        std::fprintf(stderr, "DIAG uncaught non-std exception\n");
        return 1;
    }
}
