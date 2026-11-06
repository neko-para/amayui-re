// emulator-lift · user32 窗口 / 消息泵端到端（P0 / B1+B6；pure）
//
// 判据：guest 调 RegisterClassExA → CreateWindowExA → SendMessageA / PostMessageA / PeekMessageA /
//       DispatchMessageA，WndProc 真的被调、消息真的被记录、返回值/栈平衡正确。
// ★ 这是 P0 的"完整消息泵"夹具；不需要游戏 / 资产。
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

#include "check.h"
#include "runtime/cp932.h"
#include "runtime/machine.h"
#include "runtime/region.h"
#include "shims/register.h"

namespace {

constexpr std::uint32_t kCode = 0x00401000u;
constexpr std::uint32_t kProc = 0x00401100u;
constexpr std::uint32_t kClass = 0x00402000u;
constexpr std::uint32_t kClassName = 0x00402100u;
constexpr std::uint32_t kTitle = 0x00402140u;
constexpr std::uint32_t kMsgSlot = 0x00403000u;
constexpr std::uint32_t kCreateSeen = 0x00403004u;
constexpr std::uint32_t kShowSeen = 0x00403008u;
constexpr std::uint32_t kShowResult = 0x0040300Cu;
constexpr std::uint32_t kResult = 0x00403010u;
constexpr std::uint32_t kPeekResult = 0x00403018u;
constexpr std::uint32_t kHwndSlot = 0x00403014u;
constexpr std::uint32_t kMsg = 0x00403020u;
constexpr std::uint32_t kStubBase = 0x7FF80000u;
constexpr std::uint32_t kSentinel = 0xFFFFFFFFu;

// 第二阶段（M2 输入 / 光标 / 监控器 / 系统参数 / 菜单族）用的落点
constexpr std::uint32_t kCode2 = 0x00401300u;
constexpr std::uint32_t kPoint = 0x00403100u;        // POINT (8)
constexpr std::uint32_t kMonitorInfo = 0x00403140u;  // MONITORINFO (40)
constexpr std::uint32_t kSpiOut = 0x00403180u;       // BOOL (4)
constexpr std::uint32_t kRes2 = 0x00403200u;         // 结果槽（0x80 = 32 个 dword）
constexpr std::uint32_t kCursorFile = 0x00402180u;   // "mouse.cur"

// 第三阶段（M2 · P4：GDI region + 窗口无效区）用的落点。
// ★ 刻意放在 0x405000+：与上面两段代码 / 数据（0x401000–0x403300）隔开，⛔ 不与它们重叠。
constexpr std::uint32_t kCode3 = 0x00405000u;       // 第三段 guest 代码
constexpr std::uint32_t kRes3 = 0x00406000u;        // 结果槽（0x100 = 64 个 dword）
constexpr std::uint32_t kRect1 = 0x00406100u;       // RECT 输出（GetRgnBox）
constexpr std::uint32_t kRect2 = 0x00406110u;       // RECT 输入（InvalidateRect）
constexpr std::uint32_t kRect3 = 0x00406120u;       // RECT 输入（椭圆包围盒）
constexpr std::uint32_t kRect4 = 0x00406130u;       // RECT 输出（无效区包围盒 #1）
constexpr std::uint32_t kRect5 = 0x00406140u;       // RECT 输出（空区域包围盒）
constexpr std::uint32_t kRect6 = 0x00406150u;       // RECT 输出（椭圆包围盒）
constexpr std::uint32_t kRect7 = 0x00406160u;       // RECT 输出（多边形包围盒）
constexpr std::uint32_t kRect8 = 0x00406170u;       // RECT 输出（Validate 之后的无效区）
constexpr std::uint32_t kRect9 = 0x00406180u;       // RECT 输出（整客户区）
constexpr std::uint32_t kPts = 0x004061A0u;         // POINT[6]（L 形多边形）
constexpr std::uint32_t kRgnData = 0x00406200u;     // RGNDATA 缓冲（矩形区域）
constexpr std::uint32_t kRgnData2 = 0x00406300u;    // RGNDATA 缓冲（多边形区域：2 个 RECT）
constexpr std::uint32_t kHrgnA = 0x00406400u;       // HRGN 槽（矩形 0,0,10,10）
constexpr std::uint32_t kHrgnB = 0x00406404u;       // HRGN 槽（矩形 5,0,15,10）
constexpr std::uint32_t kHrgnDst = 0x00406408u;     // HRGN 槽（CombineRgn 的目标）
constexpr std::uint32_t kHrgnUpd = 0x0040640Cu;     // HRGN 槽（GetUpdateRgn 的目标）
constexpr std::uint32_t kHrgnUpd2 = 0x00406410u;    // HRGN 槽（第二次 GetUpdateRgn）
constexpr std::uint32_t kHrgnEmpty = 0x00406414u;   // HRGN 槽（空矩形）
constexpr std::uint32_t kHrgnEllipse = 0x00406418u; // HRGN 槽（椭圆）
constexpr std::uint32_t kHrgnPoly = 0x0040641Cu;    // HRGN 槽（多边形）

// 第四阶段（2026-11-05 追加）：MessageBoxA 的 caption / text 必须按 **CP932 → UTF-8** 进日志。
// ★ 落点同样隔开（0x407000+），⛔ 不与前三段重叠。
constexpr std::uint32_t kCode4 = 0x00407000u;        // 第四段 guest 代码（只有一条 MessageBoxA 调用）
constexpr std::uint32_t kMBCaption = 0x00407100u;    // CP932 字节的 caption（NUL 结尾）
constexpr std::uint32_t kMBText = 0x00407200u;       // CP932 字节的 text（NUL 结尾）
constexpr std::uint32_t kMBResult = 0x00407300u;     // 返回值槽

struct Asm {
    // ★ `base` = 这段机器码的装载地址（第一段是 kCode；第二阶段是 kCode2）。    //   `callRel` 的位移必须相对**本段**的地址算 —— 早先写死 kCode 时，第二段的 call 全偏了 0x300。
    explicit Asm(std::uint32_t base = kCode) : base_(base) {}
    std::uint32_t base_;
    std::vector<std::uint8_t> b;
    std::uint32_t ea() const { return base_ + static_cast<std::uint32_t>(b.size()); }
    void u8(std::uint8_t v) { b.push_back(v); }
    void u32(std::uint32_t v) {
        for (int i = 0; i < 4; ++i) b.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF));
    }
    void pushImm(std::uint32_t v) { u8(0x68); u32(v); }
    void pushMem(std::uint32_t ea) { u8(0xFF); u8(0x35); u32(ea); }
    void callRel(std::uint32_t target) {
        const std::uint32_t at = ea();
        u8(0xE8);
        u32(static_cast<std::uint32_t>(static_cast<std::int32_t>(target - (at + 5))));
    }
    void storeEax(std::uint32_t ea) { u8(0xA3); u32(ea); }
    void ret() { u8(0xC3); }
};

std::uint32_t stubOf(lift::Machine& m, const char* name) {
    const lift::ShimDef* d = m.shims().find("user32", name);
    if (!d || !d->stub_ea) {
        std::fprintf(stderr, "stub missing: %s\n", name);
        return 0;
    }
    return d->stub_ea;
}

// 别的模块的桩地址（P4 的 imm32 也在这个夹具里验：它改的是窗口的输入上下文状态）
std::uint32_t stubOfIn(lift::Machine& m, const char* module, const char* name) {
    const lift::ShimDef* d = m.shims().find(module, name);
    if (!d || !d->stub_ea) {
        std::fprintf(stderr, "stub missing: %s!%s\n", module, name);
        return 0;
    }
    return d->stub_ea;
}

}  // namespace

// ---- stderr 捕获（只给 `MessageBoxA` 的日志判据用；Windows 走 `_dup/_dup2`，POSIX 走 `dup/dup2`）----
// ★ 为什么要捕获：`MessageBoxA` 的行为里**唯一**新增的、可验收的东西就是那行日志（caption / text
//   的 CP932 → UTF-8 转码）。返回值仍是 IDOK(1)，不捕获就没法把"转码对"变成会红的断言。
namespace {

#ifdef _WIN32
int dupFd(int fd) { return _dup(fd); }
int dup2Fd(int src, int dst) { return _dup2(src, dst); }
int filenoOf(std::FILE* f) { return _fileno(f); }
void closeFd(int fd) { _close(fd); }
#else
int dupFd(int fd) { return ::dup(fd); }
int dup2Fd(int src, int dst) { return ::dup2(src, dst); }
int filenoOf(std::FILE* f) { return ::fileno(f); }
void closeFd(int fd) { ::close(fd); }
#endif

struct StderrCapture {
    std::FILE* tmp = nullptr;
    int saved = -1;
    bool active = false;

    bool begin() {
        tmp = std::tmpfile();
        if (!tmp) return false;
        std::fflush(stderr);
        saved = dupFd(2);
        if (saved < 0) return false;
        if (dup2Fd(filenoOf(tmp), 2) < 0) return false;  // POSIX dup2 成功返回目标 fd（2），不是 0
        active = true;
        return true;
    }
    std::string end() {
        if (!active) return std::string();
        std::fflush(stderr);
        dup2Fd(saved, 2);
        closeFd(saved);
        active = false;
        std::fseek(tmp, 0, SEEK_END);
        const long n = std::ftell(tmp);
        std::fseek(tmp, 0, SEEK_SET);
        std::string out(n > 0 ? static_cast<std::size_t>(n) : 0u, '\0');
        if (n > 0) {
            const std::size_t got = std::fread(&out[0], 1, static_cast<std::size_t>(n), tmp);
            out.resize(got);
        }
        std::fclose(tmp);
        tmp = nullptr;
        return out;
    }
};

}  // namespace

int main() {
    lift::logSetLevel("info");  // 本夹具断言日志行；默认 warn 会隐藏 Info
    using namespace lift_test;
    lift::Machine m;
    std::string err;
    g_current_case = "setup";
    LIFT_CHECK(m.initBare(&err));
    LIFT_CHECK(m.prepareStack(&err));
    lift::registerUser32WindowShims(m.shims());
    // ★ M2 输入 / 光标 / 监控器 / 系统参数族 + 菜单族也在本夹具里（各自的断言见第二阶段）。
    lift::registerUser32InputShims(m.shims());
    lift::registerUser32MenuShims(m.shims());
    // ★ P4：imm32!ImmAssociateContext（SCOPE §1「完整 IME 不做」⇒ 没有 IME）走 shell32.cpp 的注册入口
    lift::registerShell32Shims(m.shims());
    // ★ P4：GDI region 七条（CreateRectRgn / CreateEllipticRgnIndirect / CreatePolygonRgn / CombineRgn /
    //   GetRegionData / GetRgnBox / PtInRegion）。它们与 GetUpdateRgn / ValidateRgn 共用 `GdiRegion` 类型。
    lift::registerGdiRegionShims(m.shims());
    // ★ DeleteObject 在 gdi32_object.cpp（P2）：本夹具要验"DeleteObject 能删 region"，所以把它也装上。
    lift::registerGdiObjectShims(m.shims());

    // 给所有 user32 窗口 shim 分配 deterministic 桩地址（bare 机器没有 bind）。
    for (std::uint32_t id = 0; id < m.shims().all().size(); ++id) {
        lift::ShimDef* d = const_cast<lift::ShimDef*>(m.shims().byId(id));
        if (!d || !d->fn) continue;
        d->stub_ea = kStubBase + id * 4u;
        m.cache().installShim(d->stub_ea, d->id);
    }

    // WNDCLASSEXA：style=0, proc=kProc, lpszClassName=kClassName
    std::vector<std::uint8_t> cls(0x30, 0);
    auto put32 = [&](std::size_t off, std::uint32_t v) {
        for (int i = 0; i < 4; ++i) cls[off + i] = static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF);
    };
    put32(8, kProc);
    put32(40, kClassName);
    LIFT_CHECK(m.placeData(kClass, cls.data(), cls.size(), &err));
    const char cls_name[] = "ARCGameEngine";
    const char title[] = "test";
    LIFT_CHECK(m.placeData(kClassName, cls_name, sizeof(cls_name), &err));
    LIFT_CHECK(m.placeData(kTitle, title, sizeof(title), &err));
    const std::uint8_t zeros[0x30] = {};
    LIFT_CHECK(m.placeData(kResult, zeros, sizeof(zeros), &err));
    LIFT_CHECK(m.placeData(kMsgSlot, zeros, 4, &err));
    LIFT_CHECK(m.placeData(kCreateSeen, zeros, 4, &err));
    LIFT_CHECK(m.placeData(kShowSeen, zeros, 4, &err));
    LIFT_CHECK(m.placeData(kShowResult, zeros, 4, &err));
    LIFT_CHECK(m.placeData(kHwndSlot, zeros, 4, &err));
    LIFT_CHECK(m.placeData(kMsg, zeros, 28, &err));

    // WndProc: mov eax,[esp+8]; mov [kMsgSlot],eax; mov eax,0x1234; ret 16
    // mov eax,[esp+8]; mov [kMsgSlot],eax; cmp eax,1; jne +9;
    // mov dword [kCreateSeen],1; mov eax,0x1234; ret 16
    const std::vector<std::uint8_t> proc = {
        0x8B, 0x44, 0x24, 0x08, 0xA3, 0x00, 0x30, 0x40, 0x00, 0x83, 0xF8, 0x01, 0x75, 0x0A,
        0xC7, 0x05, 0x04, 0x30, 0x40, 0x00, 0x01, 0x00, 0x00, 0x00, 0xB8, 0x34, 0x12, 0x00,
        0x00, 0xC2, 0x10, 0x00};
    LIFT_CHECK(m.placeCode(kProc, proc.data(), proc.size(), &err));

    Asm a;
    // RegisterClassExA(&class)
    a.pushImm(kClass);
    a.callRel(stubOf(m, "RegisterClassExA"));
    a.storeEax(kResult);
    // CreateWindowExA: push param, hInstance, hMenu, parent, h, w, y, x, style, name, class, exstyle
    a.pushImm(0);            // lpParam
    a.pushImm(0);            // hInstance
    a.pushImm(0);            // hMenu
    a.pushImm(0);            // parent
    a.pushImm(764);          // height
    a.pushImm(1286);         // width
    a.pushImm(0);            // y
    a.pushImm(0);            // x
    a.pushImm(0x90CB0000u);  // style
    a.pushImm(kTitle);       // window name
    a.pushImm(kClassName);   // class
    a.pushImm(0);            // exstyle
    a.callRel(stubOf(m, "CreateWindowExA"));
    a.storeEax(kHwndSlot);
    // ShowWindowA(hwnd, SW_SHOWNORMAL) -> WM_SIZE
    a.pushImm(1);
    a.pushMem(kHwndSlot);
    a.callRel(stubOf(m, "ShowWindow"));
    a.storeEax(kShowResult);
    // 记录 ShowWindow 之后的最近消息（应为 WM_SIZE=5）
    a.u8(0xA1); a.u32(kMsgSlot);  // mov eax,[kMsgSlot]
    a.storeEax(kShowSeen);
    // SendMessageA(hwnd, 0x0400, 7, 0)
    a.pushImm(0);
    a.pushImm(7);
    a.pushImm(0x0400);
    a.pushMem(kHwndSlot);
    a.callRel(stubOf(m, "SendMessageA"));
    a.storeEax(kResult);
    // PostMessageA(hwnd, 0x0401, 9, 0)
    a.pushImm(0);
    a.pushImm(9);
    a.pushImm(0x0401);
    a.pushMem(kHwndSlot);
    a.callRel(stubOf(m, "PostMessageA"));
    // PeekMessageA(&msg, 0, 0, 0, PM_REMOVE)
    a.pushImm(1);
    a.pushImm(0);
    a.pushImm(0);
    a.pushImm(0);
    a.pushImm(kMsg);
    a.callRel(stubOf(m, "PeekMessageA"));
    a.storeEax(kPeekResult);
    // DispatchMessageA(&msg)
    a.pushImm(kMsg);
    a.callRel(stubOf(m, "DispatchMessageA"));
    a.ret();
    LIFT_CHECK(m.placeCode(kCode, a.b.data(), a.b.size(), &err));
    m.cpu().eip = kCode;

    lift::RunOptions o;
    o.until_ea = kSentinel;
    o.max_insns = 10000;
    const lift::RunResult r = m.run(o);
    g_current_case = "run";
    LIFT_CHECK_EQ_U32(r.ea, kSentinel);

    g_current_case = "window";
    const std::uint32_t hwnd = m.mem().rd32(kHwndSlot);
    LIFT_CHECK(hwnd != 0);
    lift::HostWindow* w = m.cpu().windows->find(hwnd);
    LIFT_CHECK(w != nullptr);
    if (w) {
        LIFT_CHECK_EQ_I64(w->client_w, 1280);
        LIFT_CHECK_EQ_I64(w->client_h, 720);
    }

    g_current_case = "create-message";
    LIFT_CHECK_EQ_U32(m.mem().rd32(kCreateSeen), 1u);  // WM_CREATE 真的走了一次 WndProc

    g_current_case = "show-size";
    LIFT_CHECK_EQ_U32(m.mem().rd32(kShowSeen), 5u);  // WM_SIZE
    LIFT_CHECK_EQ_U32(m.mem().rd32(kShowResult), 1u);  // WS_VISIBLE 已在 style 里

    g_current_case = "send-message";
    LIFT_CHECK_EQ_U32(m.mem().rd32(kResult), 0x1234u);

    g_current_case = "post-peek-dispatch";
    LIFT_CHECK_EQ_U32(m.mem().rd32(kMsg + 4), 0x0401u);
    LIFT_CHECK_EQ_U32(m.mem().rd32(kMsg + 8), 9u);
    LIFT_CHECK_EQ_U32(m.mem().rd32(kPeekResult), 1u);
    LIFT_CHECK_EQ_U32(m.mem().rd32(kMsgSlot), 0x0401u);

    // =================================================================================
    // 第二阶段：M2 输入 / 光标 / 监控器 / 系统参数 / 菜单族（headless 语义）
    // ★ 每条实现至少一个断言；判据 = "状态真的记了 / 返回值是 Win32 的真值"。
    //   结果槽偏移见下面的 R_* 常量（读 `m.mem()` 而不是复述代码逻辑）。
    // =================================================================================
    constexpr std::uint32_t R_SETCUR_OLD1 = 0;
    constexpr std::uint32_t R_SETCUR_OLD2 = 4;
    constexpr std::uint32_t R_SETCURPOS = 8;
    constexpr std::uint32_t R_GETCURPOS = 12;
    constexpr std::uint32_t R_SHOW1 = 16;
    constexpr std::uint32_t R_SHOW2 = 20;
    constexpr std::uint32_t R_SHOW3 = 24;
    constexpr std::uint32_t R_LOADCUR = 28;
    constexpr std::uint32_t R_MON1 = 32;
    constexpr std::uint32_t R_MON2 = 36;
    constexpr std::uint32_t R_MONINFO = 40;
    constexpr std::uint32_t R_SPI_GETFADE = 44;
    constexpr std::uint32_t R_SPI_SETFADE = 48;
    constexpr std::uint32_t R_SPI_EXT = 52;
    constexpr std::uint32_t R_SPI_UNKNOWN = 56;
    constexpr std::uint32_t R_MSGW_EMPTY = 60;
    constexpr std::uint32_t R_MSGW_MSG = 64;
    constexpr std::uint32_t R_MSGW_TIMEOUT = 68;
    constexpr std::uint32_t R_UNHOOK = 72;
    constexpr std::uint32_t R_GETMENU0 = 76;
    constexpr std::uint32_t R_LOADMENU = 80;
    constexpr std::uint32_t R_SETMENU = 84;
    constexpr std::uint32_t R_GETMENU1 = 88;
    constexpr std::uint32_t R_GETSUBMENU = 92;
    constexpr std::uint32_t R_GETSYSMENU = 96;
    constexpr std::uint32_t R_REMOVEMENU = 100;
    constexpr std::uint32_t R_DRAWMENUBAR = 104;
    constexpr std::uint32_t R_DESTROYMENU = 108;
    constexpr std::uint32_t R_SM23 = 112;
    constexpr std::uint32_t R_SM0 = 116;
    constexpr std::uint32_t R_MSGW_QUIT = 120;
    constexpr std::uint32_t R_IMM = 124;

    g_current_case = "families-setup";
    std::vector<std::uint8_t> zero_buf(0x80, 0);
    std::vector<std::uint8_t> mi(40, 0);
    mi[0] = 40;  // MONITORINFO.cbSize = sizeof(MONITORINFO)
    const char cursor_file[] = "mouse.cur";
    LIFT_CHECK(m.placeData(kPoint, zero_buf.data(), 8, &err));
    LIFT_CHECK(m.placeData(kMonitorInfo, mi.data(), mi.size(), &err));
    LIFT_CHECK(m.placeData(kSpiOut, zero_buf.data(), 4, &err));
    LIFT_CHECK(m.placeData(kRes2, zero_buf.data(), zero_buf.size(), &err));
    LIFT_CHECK(m.placeData(kCursorFile, cursor_file, sizeof(cursor_file), &err));

    Asm b(kCode2);
    // ---- 光标：SetCursor 返回旧光标（第一次是 0，第二次是刚才设的伪句柄）----
    b.pushImm(0x00020002u);
    b.callRel(stubOf(m, "SetCursor"));
    b.storeEax(kRes2 + R_SETCUR_OLD1);
    b.pushImm(0);
    b.callRel(stubOf(m, "SetCursor"));
    b.storeEax(kRes2 + R_SETCUR_OLD2);
    // ---- SetCursorPos / GetCursorPos：虚拟位置真的被改（(320,240)）----
    b.pushImm(240);
    b.pushImm(320);
    b.callRel(stubOf(m, "SetCursorPos"));
    b.storeEax(kRes2 + R_SETCURPOS);
    b.pushImm(kPoint);
    b.callRel(stubOf(m, "GetCursorPos"));
    b.storeEax(kRes2 + R_GETCURPOS);
    // ---- ShowCursor：显示计数 1 → 2 → 1（返回值是**新**计数，不是布尔）----
    b.pushImm(1);
    b.callRel(stubOf(m, "ShowCursor"));
    b.storeEax(kRes2 + R_SHOW1);
    b.pushImm(1);
    b.callRel(stubOf(m, "ShowCursor"));
    b.storeEax(kRes2 + R_SHOW2);
    b.pushImm(0);
    b.callRel(stubOf(m, "ShowCursor"));
    b.storeEax(kRes2 + R_SHOW3);
    // ---- LoadCursorFromFileA：确定性伪句柄；⛔ 不读 .cur ----
    b.pushImm(kCursorFile);
    b.callRel(stubOf(m, "LoadCursorFromFileA"));
    b.storeEax(kRes2 + R_LOADCUR);
    // ---- 监控器：有效窗口 ⇒ 单显示器句柄；DEFAULTTONULL + 无效窗口 ⇒ NULL ----
    b.pushImm(2);  // MONITOR_DEFAULTTONEAREST
    b.pushMem(kHwndSlot);
    b.callRel(stubOf(m, "MonitorFromWindow"));
    b.storeEax(kRes2 + R_MON1);
    b.pushImm(0);  // MONITOR_DEFAULTTONULL
    b.pushImm(0x0000DEADu);
    b.callRel(stubOf(m, "MonitorFromWindow"));
    b.storeEax(kRes2 + R_MON2);
    b.pushImm(kMonitorInfo);
    b.pushMem(kRes2 + R_MON1);
    b.callRel(stubOf(m, "GetMonitorInfoA"));
    b.storeEax(kRes2 + R_MONINFO);
    // ---- SystemParametersInfoA：三条引擎 action + 一条未建模 ----
    b.pushImm(0);
    b.pushImm(kSpiOut);
    b.pushImm(0);
    b.pushImm(0x1014u);  // SPI_GETSELECTIONFADE
    b.callRel(stubOf(m, "SystemParametersInfoA"));
    b.storeEax(kRes2 + R_SPI_GETFADE);
    b.pushImm(0);
    b.pushImm(0);
    b.pushImm(0);
    b.pushImm(0x1015u);  // SPI_SETSELECTIONFADE
    b.callRel(stubOf(m, "SystemParametersInfoA"));
    b.storeEax(kRes2 + R_SPI_SETFADE);
    b.pushImm(0);
    b.pushImm(0);
    b.pushImm(0);
    b.pushImm(0x005Cu);  // SPI_GETWINDOWSEXTENSION（NT 家族 ⇒ FALSE）
    b.callRel(stubOf(m, "SystemParametersInfoA"));
    b.storeEax(kRes2 + R_SPI_EXT);
    b.pushImm(0);
    b.pushImm(0);
    b.pushImm(0);
    b.pushImm(0x1234u);  // 未建模 action ⇒ FALSE
    b.callRel(stubOf(m, "SystemParametersInfoA"));
    b.storeEax(kRes2 + R_SPI_UNKNOWN);
    // ---- MsgWaitForMultipleObjects：空队列 + 0ms ⇒ WAIT_TIMEOUT；有消息 ⇒ 0+nCount ----
    b.pushImm(0x0048u);
    b.pushImm(0);
    b.pushImm(0);
    b.pushImm(0);
    b.pushImm(0);
    b.callRel(stubOf(m, "MsgWaitForMultipleObjects"));
    b.storeEax(kRes2 + R_MSGW_EMPTY);
    b.pushImm(0);
    b.pushImm(0);
    b.pushImm(0x0401u);
    b.pushMem(kHwndSlot);
    b.callRel(stubOf(m, "PostMessageA"));
    b.pushImm(0x0048u);
    b.pushImm(0);
    b.pushImm(0);
    b.pushImm(0);
    b.pushImm(0);
    b.callRel(stubOf(m, "MsgWaitForMultipleObjects"));
    b.storeEax(kRes2 + R_MSGW_MSG);
    b.pushImm(1);  // 把刚才那条消息取走（PM_REMOVE）
    b.pushImm(0);
    b.pushImm(0);
    b.pushImm(0);
    b.pushImm(kMsg);
    b.callRel(stubOf(m, "PeekMessageA"));
    b.pushImm(0x0048u);
    b.pushImm(0);
    b.pushImm(0);
    b.pushImm(5);  // 有限超时 ⇒ 推进虚拟时钟后 WAIT_TIMEOUT
    b.pushImm(0);
    b.callRel(stubOf(m, "MsgWaitForMultipleObjects"));
    b.storeEax(kRes2 + R_MSGW_TIMEOUT);
    // ---- WM_QUIT 也是队列里的一条消息（peekAt 的队尾虚拟项）⇒ 0ms 等待应立刻返回 0 ----
    b.pushImm(0);
    b.callRel(stubOf(m, "PostQuitMessage"));
    b.pushImm(0x0048u);
    b.pushImm(0);
    b.pushImm(0);
    b.pushImm(0);
    b.pushImm(0);
    b.callRel(stubOf(m, "MsgWaitForMultipleObjects"));
    b.storeEax(kRes2 + R_MSGW_QUIT);
    // ---- UnhookWindowsHookEx：本线没有装过钩子 ⇒ TRUE + 日志 ----
    b.pushImm(0x00001234u);
    b.callRel(stubOf(m, "UnhookWindowsHookEx"));
    b.storeEax(kRes2 + R_UNHOOK);
    // ---- 菜单族（SCOPE §1 永久 no-op）：GetMenu 反映窗口状态，其余是假句柄 / TRUE ----
    b.pushMem(kHwndSlot);
    b.callRel(stubOf(m, "GetMenu"));
    b.storeEax(kRes2 + R_GETMENU0);
    b.pushImm(0);
    b.pushImm(0);
    b.callRel(stubOf(m, "LoadMenuA"));
    b.storeEax(kRes2 + R_LOADMENU);
    b.pushMem(kRes2 + R_LOADMENU);
    b.pushMem(kHwndSlot);
    b.callRel(stubOf(m, "SetMenu"));
    b.storeEax(kRes2 + R_SETMENU);
    b.pushMem(kHwndSlot);
    b.callRel(stubOf(m, "GetMenu"));
    b.storeEax(kRes2 + R_GETMENU1);
    b.pushImm(2);
    b.pushImm(0x00010001u);
    b.callRel(stubOf(m, "GetSubMenu"));
    b.storeEax(kRes2 + R_GETSUBMENU);
    b.pushImm(0);
    b.pushMem(kHwndSlot);
    b.callRel(stubOf(m, "GetSystemMenu"));
    b.storeEax(kRes2 + R_GETSYSMENU);
    b.pushImm(0);
    b.pushImm(0xF030u);
    b.pushImm(0x00010003u);
    b.callRel(stubOf(m, "RemoveMenu"));
    b.storeEax(kRes2 + R_REMOVEMENU);
    b.pushMem(kHwndSlot);
    b.callRel(stubOf(m, "DrawMenuBar"));
    b.storeEax(kRes2 + R_DRAWMENUBAR);
    b.pushImm(0x00010001u);
    b.callRel(stubOf(m, "DestroyMenu"));
    b.storeEax(kRes2 + R_DESTROYMENU);
    // ---- GetSystemMetrics（引擎用到的 index：0/1/7/8/23）----
    b.pushImm(23);  // SM_SWAPBUTTON ⇒ 0
    b.callRel(stubOf(m, "GetSystemMetrics"));
    b.storeEax(kRes2 + R_SM23);
    b.pushImm(0);  // SM_CXSCREEN
    b.callRel(stubOf(m, "GetSystemMetrics"));
    b.storeEax(kRes2 + R_SM0);
    // ---- imm32!ImmAssociateContext：没有 IME ⇒ 返回 NULL（⛔ 不造假 HIMC） ----
    b.pushImm(0x0000ABCDu);  // hIMC（任意值：没有 IME 时真机也返回 NULL）
    b.pushMem(kHwndSlot);
    b.callRel(stubOfIn(m, "imm32", "ImmAssociateContext"));
    b.storeEax(kRes2 + R_IMM);
    b.ret();
    LIFT_CHECK(m.placeCode(kCode2, b.b.data(), b.b.size(), &err));
    // ★ 第二阶段是**另一段 guest 代码**：重新摆一次栈（`esp = stack_top-4`、写哨兵）——
    //   第一阶段的 `ret` 已经把哨兵弹掉了，不重置的话第二段的 `ret` 会弹到垃圾地址。
    LIFT_CHECK(m.prepareStack(&err));
    m.cpu().eip = kCode2;

    lift::RunOptions o2;
    o2.until_ea = kSentinel;
    o2.max_insns = 20000;
    const lift::RunResult r2 = m.run(o2);
    g_current_case = "families-run";
    LIFT_CHECK_EQ_U32(r2.ea, kSentinel);

    auto res = [&](std::uint32_t off) { return m.mem().rd32(kRes2 + off); };

    g_current_case = "cursor";
    LIFT_CHECK_EQ_U32(res(R_SETCUR_OLD1), 0u);          // 初值：没有光标
    LIFT_CHECK_EQ_U32(res(R_SETCUR_OLD2), 0x00020002u);  // 返回的是**旧**光标（Win32 语义）
    LIFT_CHECK_EQ_U32(m.cpu().windows->cursor(), 0u);    // 第二次 SetCursor(0) 之后当前光标是 0

    g_current_case = "cursor-pos";
    LIFT_CHECK_EQ_U32(res(R_SETCURPOS), 1u);
    LIFT_CHECK_EQ_U32(res(R_GETCURPOS), 1u);
    LIFT_CHECK_EQ_I64(m.mem().rd32(kPoint), 320);
    LIFT_CHECK_EQ_I64(m.mem().rd32(kPoint + 4), 240);

    g_current_case = "show-cursor";
    LIFT_CHECK_EQ_U32(res(R_SHOW1), 1u);  // 0 + 1
    LIFT_CHECK_EQ_U32(res(R_SHOW2), 2u);  // +1
    LIFT_CHECK_EQ_U32(res(R_SHOW3), 1u);  // -1 ⇒ 返回新计数 1（⛔ 不是 TRUE/FALSE）

    g_current_case = "load-cursor-from-file";
    LIFT_CHECK_EQ_U32(res(R_LOADCUR), 0x00020002u);  // 确定性伪句柄（⛔ 不读 .cur）

    g_current_case = "monitor";
    LIFT_CHECK_EQ_U32(res(R_MON1), 0x00030001u);  // 单显示器句柄
    LIFT_CHECK_EQ_U32(res(R_MON2), 0u);           // DEFAULTTONULL + 无效窗口 ⇒ NULL
    LIFT_CHECK_EQ_U32(res(R_MONINFO), 1u);
    LIFT_CHECK_EQ_I64(m.mem().rd32(kMonitorInfo), 40);  // cbSize 原样保留
    LIFT_CHECK_EQ_I64(m.mem().rd32(kMonitorInfo + 4), 0);
    LIFT_CHECK_EQ_I64(m.mem().rd32(kMonitorInfo + 8), 0);
    LIFT_CHECK_EQ_I64(m.mem().rd32(kMonitorInfo + 12), 1280);
    LIFT_CHECK_EQ_I64(m.mem().rd32(kMonitorInfo + 16), 720);
    LIFT_CHECK_EQ_I64(m.mem().rd32(kMonitorInfo + 20), 0);  // rcWork = 屏幕（没有任务栏）
    LIFT_CHECK_EQ_I64(m.mem().rd32(kMonitorInfo + 28), 1280);
    LIFT_CHECK_EQ_I64(m.mem().rd32(kMonitorInfo + 32), 720);
    LIFT_CHECK_EQ_U32(m.mem().rd32(kMonitorInfo + 36), 1u);  // MONITORINFOF_PRIMARY

    g_current_case = "system-parameters";
    LIFT_CHECK_EQ_U32(res(R_SPI_GETFADE), 1u);
    LIFT_CHECK_EQ_U32(m.mem().rd32(kSpiOut), 0u);  // headless：没有选择淡入动画
    LIFT_CHECK_EQ_U32(res(R_SPI_SETFADE), 1u);
    LIFT_CHECK_EQ_U32(res(R_SPI_EXT), 0u);       // Win9x 专属 ⇒ NT 口径 FALSE
    LIFT_CHECK_EQ_U32(res(R_SPI_UNKNOWN), 0u);   // 未建模 ⇒ Win32 失败值（⛔ 不返回成功）

    g_current_case = "msg-wait";
    LIFT_CHECK_EQ_U32(res(R_MSGW_EMPTY), 0x102u);    // WAIT_TIMEOUT（轮询，无消息）
    LIFT_CHECK_EQ_U32(res(R_MSGW_MSG), 0u);          // WAIT_OBJECT_0 + nCount(0)：有消息
    LIFT_CHECK_EQ_U32(res(R_MSGW_TIMEOUT), 0x102u);  // 有限超时 ⇒ 无事件源 ⇒ WAIT_TIMEOUT
    LIFT_CHECK_EQ_U32(res(R_MSGW_QUIT), 0u);         // WM_QUIT 在队列里 ⇒ 0ms 就可用

    g_current_case = "unhook";
    LIFT_CHECK_EQ_U32(res(R_UNHOOK), 1u);

    g_current_case = "menu";
    LIFT_CHECK_EQ_U32(res(R_GETMENU0), 0u);              // 窗口没有菜单 ⇒ Win32 真值 0
    LIFT_CHECK_EQ_U32(res(R_LOADMENU), 0x00010001u);     // 非空假句柄
    LIFT_CHECK_EQ_U32(res(R_SETMENU), 1u);               // TRUE
    LIFT_CHECK_EQ_U32(res(R_GETMENU1), 0x00010001u);     // 反映 SetMenu 记下的窗口状态
    LIFT_CHECK_EQ_U32(res(R_GETSUBMENU), 0u);            // 假菜单没有子菜单项
    LIFT_CHECK_EQ_U32(res(R_GETSYSMENU), 0x00010003u);   // 非空假系统菜单句柄
    LIFT_CHECK_EQ_U32(res(R_REMOVEMENU), 1u);            // TRUE
    LIFT_CHECK_EQ_U32(res(R_DRAWMENUBAR), 1u);           // TRUE
    LIFT_CHECK_EQ_U32(res(R_DESTROYMENU), 1u);           // TRUE

    g_current_case = "system-metrics";
    LIFT_CHECK_EQ_U32(res(R_SM23), 0u);  // SM_SWAPBUTTON：0 = 没交换
    LIFT_CHECK_EQ_U32(res(R_SM0), 1280u);

    // ★ P4 / SCOPE §1：完整 IME 不做 ⇒ 按"没有 IME"回答（NULL），⛔ 不是假成功
    g_current_case = "imm-associate-context";
    LIFT_CHECK_EQ_U32(res(R_IMM), 0u);

    // =================================================================================
    // 第三阶段：M2 · P4 —— GDI region 七条 + 窗口无效区（GetUpdateRgn / ValidateRgn）
    // ★ 判据（plan §5 B7 / §6 P4）：
    //   ① 数据类型真做：矩形 / 椭圆 / 多边形（扫描线）都能 PtInRegion 与 GetRgnBox；
    //   ② CombineRgn 的 AND / OR / DIFF / COPY（+ XOR）各自**语义不同**（DIFF 实现成 AND 会红）；
    //   ③ GetRegionData 的 RGNDATA（头 + 矩形数组）能被解析回**同一个区域**（往返一致）；
    //   ④ PtInRegion 的右 / 下边界**不含**（改成 <= 会红）；
    //   ⑤ `CreateRectRgn` 的空矩形 ⇒ **合法空区域**（句柄非 0、NULLREGION、点永远不在里面）；
    //   ⑥ InvalidateRect 之后 GetUpdateRgn 给出同一块；ValidateRgn / ValidateRect 之后为空。
    // =================================================================================
    constexpr std::uint32_t R_RECT_IN = 0;
    constexpr std::uint32_t R_RECT_IN_MAX = 4;
    constexpr std::uint32_t R_RECT_RIGHT = 8;
    constexpr std::uint32_t R_RECT_BOTTOM = 12;
    constexpr std::uint32_t R_RECT_NEG = 16;
    constexpr std::uint32_t R_RECT_BOXTYPE = 20;
    constexpr std::uint32_t R_EMPTY_BOXTYPE = 28;
    constexpr std::uint32_t R_EMPTY_PT = 32;
    constexpr std::uint32_t R_ELLIPSE_BOXTYPE = 40;
    constexpr std::uint32_t R_ELLIPSE_CENTER = 44;
    constexpr std::uint32_t R_ELLIPSE_CORNER = 48;
    constexpr std::uint32_t R_POLY_BOXTYPE = 56;
    constexpr std::uint32_t R_POLY_IN1 = 60;
    constexpr std::uint32_t R_POLY_IN2 = 64;
    constexpr std::uint32_t R_POLY_OUT = 68;
    constexpr std::uint32_t R_POLY_IN3 = 72;
    constexpr std::uint32_t R_POLY_WINDING = 76;
    constexpr std::uint32_t R_AND_TYPE = 80;
    constexpr std::uint32_t R_AND_IN = 84;
    constexpr std::uint32_t R_AND_OUT = 88;
    constexpr std::uint32_t R_OR_TYPE = 92;
    constexpr std::uint32_t R_OR_LEFT = 96;
    constexpr std::uint32_t R_OR_RIGHT = 100;
    constexpr std::uint32_t R_OR_OUT = 104;
    constexpr std::uint32_t R_DIFF_TYPE = 108;
    constexpr std::uint32_t R_DIFF_IN = 112;
    constexpr std::uint32_t R_DIFF_OUT = 116;
    constexpr std::uint32_t R_COPY_TYPE = 120;
    constexpr std::uint32_t R_COPY_IN = 124;
    constexpr std::uint32_t R_COPY_OUT = 128;
    constexpr std::uint32_t R_XOR_TYPE = 132;
    constexpr std::uint32_t R_XOR_L = 136;
    constexpr std::uint32_t R_XOR_M = 140;
    constexpr std::uint32_t R_XOR_R = 144;
    constexpr std::uint32_t R_DATA_PROBE = 148;
    constexpr std::uint32_t R_DATA_SMALL = 152;
    constexpr std::uint32_t R_DATA_FILL = 156;
    constexpr std::uint32_t R_DATA_POLY_PROBE = 160;
    constexpr std::uint32_t R_DATA_POLY_FILL = 164;
    constexpr std::uint32_t R_UPD_INV = 168;
    constexpr std::uint32_t R_UPD_GET = 172;
    constexpr std::uint32_t R_UPD_BOX = 176;
    constexpr std::uint32_t R_UPD_VALIDATE = 180;
    constexpr std::uint32_t R_UPD_AFTER_GET = 184;
    constexpr std::uint32_t R_UPD_AFTER_BOX = 188;
    constexpr std::uint32_t R_UPD_FULL = 192;
    constexpr std::uint32_t R_UPD_FULL_GET = 196;
    constexpr std::uint32_t R_UPD_FULL_BOX = 200;
    constexpr std::uint32_t R_UPD_CLEAR = 204;
    constexpr std::uint32_t R_UPD_CLEARED = 208;
    constexpr std::uint32_t R_DEL_OK = 212;
    constexpr std::uint32_t R_DEL_PT = 216;

    g_current_case = "region-setup";
    {
        std::vector<std::uint8_t> zero(0x100, 0);
        LIFT_CHECK(m.placeData(kRes3, zero.data(), zero.size(), &err));
        // 每个 GetRgnBox 的 LPRECT 落点都必须先提交（否则 shim 按不可写返回 ERROR）
        const std::uint32_t rect_slots[] = {kRect1, kRect4, kRect5, kRect6, kRect7, kRect8, kRect9};
        for (const std::uint32_t slot : rect_slots) {
            LIFT_CHECK(m.placeData(slot, zero.data(), 16, &err));
        }
        // InvalidateRect 用的矩形：(10,20)-(30,40)（客户区坐标）
        std::vector<std::uint8_t> rc(16, 0);
        const std::int32_t rcv[4] = {10, 20, 30, 40};
        for (int i = 0; i < 4; ++i) {
            for (int k = 0; k < 4; ++k) {
                rc[static_cast<std::size_t>(i) * 4 + static_cast<std::size_t>(k)] =
                    static_cast<std::uint8_t>((static_cast<std::uint32_t>(rcv[i]) >> (8 * k)) & 0xFFu);
            }
        }
        LIFT_CHECK(m.placeData(kRect2, rc.data(), rc.size(), &err));
        // 椭圆包围盒 (0,0)-(20,20)
        std::vector<std::uint8_t> eb(16, 0);
        eb[8] = 20;
        eb[12] = 20;
        LIFT_CHECK(m.placeData(kRect3, eb.data(), eb.size(), &err));
        // L 形多边形：(0,0) (10,0) (10,5) (5,5) (5,10) (0,10)
        const std::int32_t pts[12] = {0, 0, 10, 0, 10, 5, 5, 5, 5, 10, 0, 10};
        std::vector<std::uint8_t> pb(48, 0);
        for (int i = 0; i < 12; ++i) {
            for (int k = 0; k < 4; ++k) {
                pb[static_cast<std::size_t>(i) * 4 + static_cast<std::size_t>(k)] =
                    static_cast<std::uint8_t>((static_cast<std::uint32_t>(pts[i]) >> (8 * k)) & 0xFFu);
            }
        }
        LIFT_CHECK(m.placeData(kPts, pb.data(), pb.size(), &err));
        std::vector<std::uint8_t> nulldata(0x100, 0);
        LIFT_CHECK(m.placeData(kRgnData, nulldata.data(), nulldata.size(), &err));
        LIFT_CHECK(m.placeData(kRgnData2, nulldata.data(), nulldata.size(), &err));
        std::vector<std::uint8_t> handles(0x20, 0);
        LIFT_CHECK(m.placeData(kHrgnA, handles.data(), handles.size(), &err));
    }

    Asm c(kCode3);
    auto gdi = [&](const char* name) { return stubOfIn(m, "gdi32", name); };
    auto ptIn = [&](std::uint32_t hrgn_slot, std::int32_t x, std::int32_t y, std::uint32_t out) {
        c.pushImm(static_cast<std::uint32_t>(y));
        c.pushImm(static_cast<std::uint32_t>(x));
        c.pushMem(hrgn_slot);
        c.callRel(gdi("PtInRegion"));
        c.storeEax(kRes3 + out);
    };
    auto createRect = [&](std::int32_t l, std::int32_t t, std::int32_t r, std::int32_t b,
                          std::uint32_t slot) {
        c.pushImm(static_cast<std::uint32_t>(b));
        c.pushImm(static_cast<std::uint32_t>(r));
        c.pushImm(static_cast<std::uint32_t>(t));
        c.pushImm(static_cast<std::uint32_t>(l));
        c.callRel(gdi("CreateRectRgn"));
        c.storeEax(slot);
    };
    auto getBox = [&](std::uint32_t hrgn_slot, std::uint32_t rect_ea, std::uint32_t out) {
        c.pushImm(rect_ea);
        c.pushMem(hrgn_slot);
        c.callRel(gdi("GetRgnBox"));
        c.storeEax(kRes3 + out);
    };

    // ---- ① 矩形 (0,0)-(10,10)：内部 / 最大内部点 / 右边界 / 下边界 / 负坐标 ----
    createRect(0, 0, 10, 10, kHrgnA);
    ptIn(kHrgnA, 0, 0, R_RECT_IN);
    ptIn(kHrgnA, 9, 9, R_RECT_IN_MAX);
    ptIn(kHrgnA, 10, 5, R_RECT_RIGHT);
    ptIn(kHrgnA, 5, 10, R_RECT_BOTTOM);
    ptIn(kHrgnA, -1, 5, R_RECT_NEG);
    getBox(kHrgnA, kRect1, R_RECT_BOXTYPE);

    // ---- ② 空矩形（x1 == x2）：仍然是**合法区域**（句柄非 0、NULLREGION）----
    createRect(5, 5, 5, 9, kHrgnEmpty);
    getBox(kHrgnEmpty, kRect5, R_EMPTY_BOXTYPE);
    ptIn(kHrgnEmpty, 5, 6, R_EMPTY_PT);

    // ---- ③ 椭圆：包围盒 (0,0)-(20,20) ----
    c.pushImm(kRect3);
    c.callRel(gdi("CreateEllipticRgnIndirect"));
    c.storeEax(kHrgnEllipse);
    getBox(kHrgnEllipse, kRect6, R_ELLIPSE_BOXTYPE);
    ptIn(kHrgnEllipse, 10, 10, R_ELLIPSE_CENTER);
    ptIn(kHrgnEllipse, 0, 0, R_ELLIPSE_CORNER);

    // ---- ④ 多边形（L 形，ALTERNATE 奇偶扫描线）：内点 / 缺口 / WINDING 明确失败 ----
    c.pushImm(1);  // ALTERNATE
    c.pushImm(6);
    c.pushImm(kPts);
    c.callRel(gdi("CreatePolygonRgn"));
    c.storeEax(kHrgnPoly);
    getBox(kHrgnPoly, kRect7, R_POLY_BOXTYPE);
    ptIn(kHrgnPoly, 2, 2, R_POLY_IN1);
    ptIn(kHrgnPoly, 7, 2, R_POLY_IN2);
    ptIn(kHrgnPoly, 7, 7, R_POLY_OUT);
    ptIn(kHrgnPoly, 2, 7, R_POLY_IN3);
    c.pushImm(2);  // WINDING（未实现 ⇒ NULL）
    c.pushImm(6);
    c.pushImm(kPts);
    c.callRel(gdi("CreatePolygonRgn"));
    c.storeEax(kRes3 + R_POLY_WINDING);

    // ---- ⑤ CombineRgn：A=(0,0,10,10)（① 已建）· B=(5,0,15,10) ----
    createRect(5, 0, 15, 10, kHrgnB);
    struct CombineCase {
        std::int32_t mode;
        std::uint32_t type_out;
        std::int32_t in_x, in_y;
        std::uint32_t in_out;
        std::int32_t out_x, out_y;
        std::uint32_t out_out;
        std::int32_t extra_x, extra_y;  // < 0 ⇒ 不用
        std::uint32_t extra_out;
    };
    const CombineCase cases[] = {
        {1, R_AND_TYPE, 7, 5, R_AND_IN, 2, 5, R_AND_OUT, -1, -1, 0},      // RGN_AND
        {2, R_OR_TYPE, 2, 5, R_OR_LEFT, 20, 5, R_OR_OUT, 12, 5, R_OR_RIGHT},  // RGN_OR
        {4, R_DIFF_TYPE, 2, 5, R_DIFF_IN, 7, 5, R_DIFF_OUT, -1, -1, 0},   // RGN_DIFF
        {5, R_COPY_TYPE, 5, 5, R_COPY_IN, 15, 5, R_COPY_OUT, -1, -1, 0},  // RGN_COPY
        {3, R_XOR_TYPE, 2, 5, R_XOR_L, 7, 5, R_XOR_M, 12, 5, R_XOR_R},    // RGN_XOR
    };
    for (const CombineCase& cc : cases) {
        createRect(100, 100, 101, 101, kHrgnDst);  // 目标区域（内容会被 combine 覆盖）
        c.pushImm(static_cast<std::uint32_t>(cc.mode));
        c.pushMem(kHrgnB);
        c.pushMem(kHrgnA);
        c.pushMem(kHrgnDst);
        c.callRel(gdi("CombineRgn"));
        c.storeEax(kRes3 + cc.type_out);
        ptIn(kHrgnDst, cc.in_x, cc.in_y, cc.in_out);
        ptIn(kHrgnDst, cc.out_x, cc.out_y, cc.out_out);
        if (cc.extra_x >= 0) ptIn(kHrgnDst, cc.extra_x, cc.extra_y, cc.extra_out);
    }

    // ---- ⑥ GetRegionData：需要字节数 / 缓冲不足 / 写成功（矩形与多边形各一次）----
    c.pushImm(0);
    c.pushImm(0);
    c.pushMem(kHrgnA);
    c.callRel(gdi("GetRegionData"));
    c.storeEax(kRes3 + R_DATA_PROBE);
    c.pushImm(kRgnData);
    c.pushImm(16);
    c.pushMem(kHrgnA);
    c.callRel(gdi("GetRegionData"));
    c.storeEax(kRes3 + R_DATA_SMALL);
    c.pushImm(kRgnData);
    c.pushImm(80);
    c.pushMem(kHrgnA);
    c.callRel(gdi("GetRegionData"));
    c.storeEax(kRes3 + R_DATA_FILL);
    c.pushImm(0);
    c.pushImm(0);
    c.pushMem(kHrgnPoly);
    c.callRel(gdi("GetRegionData"));
    c.storeEax(kRes3 + R_DATA_POLY_PROBE);
    c.pushImm(kRgnData2);
    c.pushImm(96);
    c.pushMem(kHrgnPoly);
    c.callRel(gdi("GetRegionData"));
    c.storeEax(kRes3 + R_DATA_POLY_FILL);

    // ---- ⑦ 窗口无效区：InvalidateRect → GetUpdateRgn 是同一块；ValidateRgn 之后为空 ----
    createRect(0, 0, 0, 0, kHrgnUpd);
    createRect(0, 0, 0, 0, kHrgnUpd2);
    c.pushImm(1);  // bErase
    c.pushImm(kRect2);
    c.pushMem(kHwndSlot);
    c.callRel(stubOf(m, "InvalidateRect"));
    c.storeEax(kRes3 + R_UPD_INV);
    c.pushImm(0);
    c.pushMem(kHrgnUpd);
    c.pushMem(kHwndSlot);
    c.callRel(stubOf(m, "GetUpdateRgn"));
    c.storeEax(kRes3 + R_UPD_GET);
    getBox(kHrgnUpd, kRect4, R_UPD_BOX);
    c.pushMem(kHrgnUpd);
    c.pushMem(kHwndSlot);
    c.callRel(stubOf(m, "ValidateRgn"));
    c.storeEax(kRes3 + R_UPD_VALIDATE);
    c.pushImm(0);
    c.pushMem(kHrgnUpd2);
    c.pushMem(kHwndSlot);
    c.callRel(stubOf(m, "GetUpdateRgn"));
    c.storeEax(kRes3 + R_UPD_AFTER_GET);
    getBox(kHrgnUpd2, kRect8, R_UPD_AFTER_BOX);
    // NULL 矩形 ⇒ 整个客户区（1280×720）
    c.pushImm(0);
    c.pushImm(0);
    c.pushMem(kHwndSlot);
    c.callRel(stubOf(m, "InvalidateRect"));
    c.storeEax(kRes3 + R_UPD_FULL);
    c.pushImm(0);
    c.pushMem(kHrgnUpd);
    c.pushMem(kHwndSlot);
    c.callRel(stubOf(m, "GetUpdateRgn"));
    c.storeEax(kRes3 + R_UPD_FULL_GET);
    getBox(kHrgnUpd, kRect9, R_UPD_FULL_BOX);
    // ValidateRect(NULL) ⇒ 整窗有效
    c.pushImm(0);
    c.pushMem(kHwndSlot);
    c.callRel(stubOf(m, "ValidateRect"));
    c.storeEax(kRes3 + R_UPD_CLEAR);
    c.pushImm(0);
    c.pushMem(kHrgnUpd2);
    c.pushMem(kHwndSlot);
    c.callRel(stubOf(m, "GetUpdateRgn"));
    c.storeEax(kRes3 + R_UPD_CLEARED);

    // ---- ⑧ DeleteObject 能删 region；删完 PtInRegion 按无效句柄返回 FALSE ----
    c.pushMem(kHrgnA);
    c.callRel(gdi("DeleteObject"));
    c.storeEax(kRes3 + R_DEL_OK);
    ptIn(kHrgnA, 5, 5, R_DEL_PT);
    c.ret();

    g_current_case = "region-code-fit";
    LIFT_CHECK(c.b.size() < 0x1000);  // 第三段代码必须落在 kCode3..kRes3 之间
    LIFT_CHECK(m.placeCode(kCode3, c.b.data(), c.b.size(), &err));
    LIFT_CHECK(m.prepareStack(&err));  // 第二阶段的 ret 已经用掉了哨兵 ⇒ 重新铺栈
    m.cpu().eip = kCode3;

    lift::RunOptions o3;
    o3.until_ea = kSentinel;
    o3.max_insns = 40000;
    const lift::RunResult r3 = m.run(o3);
    g_current_case = "region-run";
    LIFT_CHECK_EQ_U32(r3.ea, kSentinel);

    auto res3 = [&](std::uint32_t off) { return m.mem().rd32(kRes3 + off); };
    auto rectField = [&](std::uint32_t ea, int index) {
        return static_cast<std::int32_t>(m.mem().rd32(ea + static_cast<std::uint32_t>(index) * 4u));
    };

    g_current_case = "region-rect";
    const std::uint32_t hrgn_a = m.mem().rd32(kHrgnA);
    LIFT_CHECK_EQ_U32(res3(R_RECT_IN), 1u);
    LIFT_CHECK_EQ_U32(res3(R_RECT_IN_MAX), 1u);  // (9,9) 是**最后一个**内部像素
    LIFT_CHECK_EQ_U32(res3(R_RECT_RIGHT), 0u);   // ★ 右边界不含：(10,5) 不在区域里
    LIFT_CHECK_EQ_U32(res3(R_RECT_BOTTOM), 0u);  // ★ 下边界不含：(5,10) 不在区域里
    LIFT_CHECK_EQ_U32(res3(R_RECT_NEG), 0u);
    LIFT_CHECK_EQ_U32(res3(R_RECT_BOXTYPE), 2u);  // SIMPLEREGION
    LIFT_CHECK_EQ_I64(rectField(kRect1, 0), 0);   // 包围盒 = 创建时的矩形
    LIFT_CHECK_EQ_I64(rectField(kRect1, 1), 0);
    LIFT_CHECK_EQ_I64(rectField(kRect1, 2), 10);
    LIFT_CHECK_EQ_I64(rectField(kRect1, 3), 10);

    g_current_case = "region-empty-rect";
    LIFT_CHECK(m.mem().rd32(kHrgnEmpty) != 0u);   // ★ 空矩形**也是**一个句柄（不是"没创建"）
    LIFT_CHECK_EQ_U32(res3(R_EMPTY_BOXTYPE), 1u);  // NULLREGION
    LIFT_CHECK_EQ_U32(res3(R_EMPTY_PT), 0u);       // 空区域里没有点
    LIFT_CHECK_EQ_I64(rectField(kRect5, 0), 0);    // 空区域包围盒 (0,0,0,0)
    LIFT_CHECK_EQ_I64(rectField(kRect5, 2), 0);
    LIFT_CHECK_EQ_I64(rectField(kRect5, 3), 0);

    g_current_case = "region-ellipse";
    LIFT_CHECK(m.mem().rd32(kHrgnEllipse) != 0u);
    LIFT_CHECK_EQ_U32(res3(R_ELLIPSE_BOXTYPE), 3u);  // COMPLEXREGION（多行）
    LIFT_CHECK_EQ_U32(res3(R_ELLIPSE_CENTER), 1u);   // 圆心在区域内
    LIFT_CHECK_EQ_U32(res3(R_ELLIPSE_CORNER), 0u);   // 包围盒角不在内切椭圆里
    LIFT_CHECK_EQ_I64(rectField(kRect6, 2), 20);     // 内切椭圆正好撑满包围盒
    LIFT_CHECK_EQ_I64(rectField(kRect6, 3), 20);

    g_current_case = "region-polygon";
    LIFT_CHECK(m.mem().rd32(kHrgnPoly) != 0u);
    LIFT_CHECK_EQ_U32(res3(R_POLY_BOXTYPE), 3u);  // L 形 = 两个矩形 ⇒ COMPLEXREGION
    LIFT_CHECK_EQ_U32(res3(R_POLY_IN1), 1u);      // (2,2) 左竖条
    LIFT_CHECK_EQ_U32(res3(R_POLY_IN2), 1u);      // (7,2) 上横条
    LIFT_CHECK_EQ_U32(res3(R_POLY_OUT), 0u);      // (7,7) 缺口
    LIFT_CHECK_EQ_U32(res3(R_POLY_IN3), 1u);      // (2,7) 左竖条
    LIFT_CHECK_EQ_I64(rectField(kRect7, 2), 10);  // 顶点范围 = (0,0)-(10,10)
    LIFT_CHECK_EQ_I64(rectField(kRect7, 3), 10);

    g_current_case = "region-polygon-winding";
    // ★ 未实现形态必须**响亮**：WINDING 返回 NULL（日志在 stderr），⛔ 不拿 ALTERNATE 冒充。
    LIFT_CHECK_EQ_U32(res3(R_POLY_WINDING), 0u);

    g_current_case = "combine-and";
    LIFT_CHECK_EQ_U32(res3(R_AND_TYPE), 2u);  // [5,10)×[0,10) 一个矩形
    LIFT_CHECK_EQ_U32(res3(R_AND_IN), 1u);
    LIFT_CHECK_EQ_U32(res3(R_AND_OUT), 0u);

    g_current_case = "combine-or";
    LIFT_CHECK_EQ_U32(res3(R_OR_TYPE), 2u);  // [0,15)×[0,10) 一个矩形
    LIFT_CHECK_EQ_U32(res3(R_OR_LEFT), 1u);
    LIFT_CHECK_EQ_U32(res3(R_OR_RIGHT), 1u);
    LIFT_CHECK_EQ_U32(res3(R_OR_OUT), 0u);

    g_current_case = "combine-diff";
    // ★ RGN_DIFF = src1 − src2：只有 A 的左半。把 DIFF 实现成 AND 时 R_DIFF_IN 会变 0（红）。
    LIFT_CHECK_EQ_U32(res3(R_DIFF_TYPE), 2u);
    LIFT_CHECK_EQ_U32(res3(R_DIFF_IN), 1u);
    LIFT_CHECK_EQ_U32(res3(R_DIFF_OUT), 0u);

    g_current_case = "combine-copy";
    LIFT_CHECK_EQ_U32(res3(R_COPY_TYPE), 2u);  // 目标被 src1 整体替换（原来那块 100,100 没了）
    LIFT_CHECK_EQ_U32(res3(R_COPY_IN), 1u);
    LIFT_CHECK_EQ_U32(res3(R_COPY_OUT), 0u);

    g_current_case = "combine-xor";
    // ★ RGN_XOR = 对称差：A∪B − A∩B ⇒ 两块（[0,5) 与 [10,15)）⇒ COMPLEXREGION
    LIFT_CHECK_EQ_U32(res3(R_XOR_TYPE), 3u);
    LIFT_CHECK_EQ_U32(res3(R_XOR_L), 1u);
    LIFT_CHECK_EQ_U32(res3(R_XOR_M), 0u);  // 交集里 ⇒ 不属于对称差
    LIFT_CHECK_EQ_U32(res3(R_XOR_R), 1u);

    g_current_case = "region-data";
    LIFT_CHECK_EQ_U32(res3(R_DATA_PROBE), 48u);  // 32 字节头 + 1 个 RECT
    LIFT_CHECK_EQ_U32(res3(R_DATA_SMALL), 48u);  // 缓冲不足 ⇒ 返回所需字节数（不是 1）
    LIFT_CHECK_EQ_U32(res3(R_DATA_FILL), 1u);    // 缓冲够 ⇒ 返回非零
    LIFT_CHECK_EQ_U32(m.mem().rd32(kRgnData), 48u);      // dwSize
    LIFT_CHECK_EQ_U32(m.mem().rd32(kRgnData + 4), 1u);   // iType = RDH_RECTANGLES
    LIFT_CHECK_EQ_I64(static_cast<std::int32_t>(m.mem().rd32(kRgnData + 8)), 0);   // rcBound
    LIFT_CHECK_EQ_I64(static_cast<std::int32_t>(m.mem().rd32(kRgnData + 12)), 0);
    LIFT_CHECK_EQ_I64(static_cast<std::int32_t>(m.mem().rd32(kRgnData + 16)), 10);
    LIFT_CHECK_EQ_I64(static_cast<std::int32_t>(m.mem().rd32(kRgnData + 20)), 10);
    LIFT_CHECK_EQ_I64(static_cast<std::int32_t>(m.mem().rd32(kRgnData + 32)), 0);  // RECT[0]
    LIFT_CHECK_EQ_I64(static_cast<std::int32_t>(m.mem().rd32(kRgnData + 40)), 10);
    LIFT_CHECK_EQ_I64(static_cast<std::int32_t>(m.mem().rd32(kRgnData + 44)), 10);

    g_current_case = "region-data-roundtrip";
    {
        // ① 矩形区域：GetRegionData 的字节 → 解析回区域，必须**逐矩形相同**
        std::uint8_t buf[48] = {};
        m.mem().readBytes(kRgnData, buf, sizeof(buf));
        lift::GdiRegion back;
        LIFT_CHECK(lift::GdiRegion::parseRegionData(buf, sizeof(buf), &back));
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(back.size()), 1u);
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(back.type()), 2u);
        LIFT_CHECK(back.contains(0, 0));
        LIFT_CHECK(back.contains(9, 9));
        LIFT_CHECK(!back.contains(10, 5));
        LIFT_CHECK(!back.contains(5, 10));
        // ② 多边形区域：往返后与原对象**完全相等**（同一份规范化矩形表）
        const std::uint32_t hpoly = m.mem().rd32(kHrgnPoly);
        const lift::HandleEntry* entry = m.handles().get(hpoly);
        LIFT_CHECK(entry != nullptr && entry->kind == lift::HandleKind::GdiRegion);
        const std::uint32_t poly_bytes = res3(R_DATA_POLY_PROBE);
        LIFT_CHECK_EQ_U32(poly_bytes, 64u);  // 32 + 2×16
        LIFT_CHECK_EQ_U32(res3(R_DATA_POLY_FILL), 1u);
        std::uint8_t pbuf[64] = {};
        m.mem().readBytes(kRgnData2, pbuf, sizeof(pbuf));
        lift::GdiRegion pback;
        LIFT_CHECK(lift::GdiRegion::parseRegionData(pbuf, sizeof(pbuf), &pback));
        if (entry != nullptr && entry->payload != nullptr) {
            const auto* orig = static_cast<const lift::GdiRegion*>(entry->payload);
            LIFT_CHECK(pback == *orig);  // ★ 往返一致（矩形表逐项相等）
            LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(pback.size()), 2u);
        }
        // ③ 不合规的头必须被拒绝（⛔ 不"大概能读就当真"）
        std::uint8_t bad[48] = {};
        m.mem().readBytes(kRgnData, bad, sizeof(bad));
        bad[4] = 0;  // iType = 0（不是 RDH_RECTANGLES）
        lift::GdiRegion junk;
        LIFT_CHECK(!lift::GdiRegion::parseRegionData(bad, sizeof(bad), &junk));
    }

    g_current_case = "update-region";
    LIFT_CHECK_EQ_U32(res3(R_UPD_INV), 1u);
    LIFT_CHECK_EQ_U32(res3(R_UPD_GET), 2u);  // SIMPLEREGION：InvalidateRect 的那一块
    LIFT_CHECK_EQ_U32(res3(R_UPD_BOX), 2u);
    LIFT_CHECK_EQ_I64(rectField(kRect4, 0), 10);  // 与 InvalidateRect 的矩形**完全相同**
    LIFT_CHECK_EQ_I64(rectField(kRect4, 1), 20);
    LIFT_CHECK_EQ_I64(rectField(kRect4, 2), 30);
    LIFT_CHECK_EQ_I64(rectField(kRect4, 3), 40);

    g_current_case = "update-region-validated";
    LIFT_CHECK_EQ_U32(res3(R_UPD_VALIDATE), 1u);
    LIFT_CHECK_EQ_U32(res3(R_UPD_AFTER_GET), 1u);  // ★ ValidateRgn 之后 ⇒ NULLREGION
    LIFT_CHECK_EQ_U32(res3(R_UPD_AFTER_BOX), 1u);
    LIFT_CHECK_EQ_I64(rectField(kRect8, 0), 0);  // 空区域包围盒 (0,0,0,0)
    LIFT_CHECK_EQ_I64(rectField(kRect8, 2), 0);
    LIFT_CHECK_EQ_I64(rectField(kRect8, 3), 0);

    g_current_case = "update-region-full-client";
    LIFT_CHECK_EQ_U32(res3(R_UPD_FULL), 1u);
    LIFT_CHECK_EQ_U32(res3(R_UPD_FULL_GET), 2u);
    LIFT_CHECK_EQ_U32(res3(R_UPD_FULL_BOX), 2u);
    LIFT_CHECK_EQ_I64(rectField(kRect9, 0), 0);
    LIFT_CHECK_EQ_I64(rectField(kRect9, 2), 1280);  // 整个客户区
    LIFT_CHECK_EQ_I64(rectField(kRect9, 3), 720);
    LIFT_CHECK_EQ_U32(res3(R_UPD_CLEAR), 1u);
    LIFT_CHECK_EQ_U32(res3(R_UPD_CLEARED), 1u);  // ValidateRect(NULL) 之后 ⇒ NULLREGION

    g_current_case = "region-delete";
    LIFT_CHECK_EQ_U32(res3(R_DEL_OK), 1u);        // DeleteObject 认 region
    LIFT_CHECK(m.handles().get(hrgn_a) == nullptr);  // 句柄项已释放
    LIFT_CHECK_EQ_U32(res3(R_DEL_PT), 0u);        // 之后 PtInRegion ⇒ FALSE（无效句柄）

    // =================================================================================
    // 第四阶段（2026-11-05 追加）：`MessageBoxA` 的 caption / text 必须按 CP932 → UTF-8 进日志。
    // 判据（会红）：
    //   ① 返回值**一字未改**：仍是 IDOK(1)（SCOPE §1 模态 no-op）；
    //   ② 日志里出现**转码后**的真实日文（不是 `?`、不是 CP932 原始字节）；
    //   ③ 正文里的 CRLF 被转义成 `\r\n` ⇒ 一行日志（否则多行日志没法 grep）。
    // ★ CP932 字节由**本仓的码表**编出来（⛔ 不手写编码事实；码表本身由 tests/codepage.cpp 守）。
    // =================================================================================
    g_current_case = "message-box-cp932-log";
    {
        const std::u16string cap_w = u"エラー：DirectSound";
        const std::u16string txt_w = u"DirectSoundが使用できません\r\n音を出力出来ません";
        const std::string cap_bytes = lift::wideToCp932(cap_w);
        const std::string txt_bytes = lift::wideToCp932(txt_w);
        // 含非 ASCII ⇒ 这条用例真的在走 CP932（纯 ASCII 会让"没转码"也蒙混过关）
        auto hasHighByte = [](const std::string& s) {
            for (const char ch : s)
                if ((static_cast<unsigned char>(ch) & 0x80u) != 0) return true;
            return false;
        };
        LIFT_CHECK(hasHighByte(cap_bytes));
        LIFT_CHECK(hasHighByte(txt_bytes));
        // 而且 CP932 是**双字节**：解码后字节数必须与原 UTF-16 码元数不同（窄字符 → 宽字节）
        LIFT_CHECK_NE_U32(static_cast<std::uint32_t>(txt_bytes.size()),
                          static_cast<std::uint32_t>(txt_w.size()));

        std::vector<std::uint8_t> capb(cap_bytes.begin(), cap_bytes.end());
        capb.push_back(0);
        std::vector<std::uint8_t> txtb(txt_bytes.begin(), txt_bytes.end());
        txtb.push_back(0);
        LIFT_CHECK(m.placeData(kMBCaption, capb.data(), capb.size(), &err));
        LIFT_CHECK(m.placeData(kMBText, txtb.data(), txtb.size(), &err));
        const std::uint8_t zeros4[4] = {};
        LIFT_CHECK(m.placeData(kMBResult, zeros4, sizeof(zeros4), &err));

        // 引擎 0x00406D90 的压栈次序：uType, lpCaption, lpText, hWnd
        Asm d(kCode4);
        d.pushImm(0x41u);  // uType（MB_OK | MB_ICONINFORMATION —— 引擎那里就是 0x41）
        d.pushImm(kMBCaption);
        d.pushImm(kMBText);
        d.pushImm(0x00020004u);  // hWnd（headless 里没有窗口；MessageBoxA 不会去查它）
        d.callRel(stubOf(m, "MessageBoxA"));
        d.storeEax(kMBResult);
        d.ret();
        LIFT_CHECK(m.placeCode(kCode4, d.b.data(), d.b.size(), &err));
        LIFT_CHECK(m.prepareStack(&err));

        StderrCapture cc;
        LIFT_CHECK(cc.begin());
        m.cpu().eip = kCode4;
        lift::RunOptions o4;
        o4.until_ea = kSentinel;
        o4.max_insns = 2000;
        const lift::RunResult r4 = m.run(o4);
        const std::string log = cc.end();

        g_current_case = "message-box-cp932-log-run";
        // ★ 把捕获到的那一行**回显**出来：它本身就是这条用例的证据（真机上它是弹给人看的），
        //   ctest 只在失败时显示它，直接跑二进制时能当场看见转码结果。
        std::fputs(log.c_str(), stderr);
        LIFT_CHECK_EQ_U32(r4.ea, kSentinel);
        // ① 语义不变
        LIFT_CHECK_EQ_U32(m.mem().rd32(kMBResult), 1u);
        // ② 转码后的真实文本（UTF-8）+ 原始 type
        const std::string want_caption = std::string("caption=\"") + "エラー：DirectSound" + "\"";
        LIFT_CHECK(log.find(want_caption) != std::string::npos);
        const std::string want_text =
            std::string("text=\"") + "DirectSoundが使用できません\\r\\n音を出力出来ません" + "\"";
        LIFT_CHECK(log.find(want_text) != std::string::npos);
        LIFT_CHECK(log.find("type=0x00000041") != std::string::npos);
        LIFT_CHECK(log.find("⇒ 返回 IDOK(1)（headless 不建窗口）") != std::string::npos);
        // ③ 一行一条：正文里的 CRLF 被转义 ⇒ 日志里没有裸的 CRLF 跑进那行
        LIFT_CHECK(log.find("DirectSoundが使用できません\r\n") == std::string::npos);
    }

    return finish("user32-window");
}
