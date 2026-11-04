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

#include "check.h"
#include "runtime/machine.h"
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

struct Asm {
    // ★ `base` = 这段机器码的装载地址（第一段是 kCode；第二阶段是 kCode2）。
    //   `callRel` 的位移必须相对**本段**的地址算 —— 早先写死 kCode 时，第二段的 call 全偏了 0x300。
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

int main() {
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

    return finish("user32-window");
}
