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

struct Asm {
    std::vector<std::uint8_t> b;
    std::uint32_t ea() const { return kCode + static_cast<std::uint32_t>(b.size()); }
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

}  // namespace

int main() {
    using namespace lift_test;
    lift::Machine m;
    std::string err;
    g_current_case = "setup";
    LIFT_CHECK(m.initBare(&err));
    LIFT_CHECK(m.prepareStack(&err));
    lift::registerUser32WindowShims(m.shims());

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

    return finish("user32-window");
}
