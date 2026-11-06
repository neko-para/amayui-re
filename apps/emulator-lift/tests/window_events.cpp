// emulator-lift · GetMessageA + 宿主事件源（P3；pure）
//
// 判据（红了意味着什么）：
//   ① 未安装事件源 ⇒ `WindowManager::get` 仍是旧的"空队列返回 false"；
//   ② 安装假事件源后，guest 的 `GetMessageA` 不再 abort；事件源依次注入
//      WM_KEYDOWN / WM_MOUSEMOVE / WM_CLOSE，guest 按顺序收到；最后 postQuit ⇒ GetMessageA 返回 0。
//   ③ WM_KEYDOWN/UP 与 `GetAsyncKeyState` 共用 WindowManager 的影子键状态（⛔ 两套状态）。
//   ④ 关窗消息的 hwnd = primaryWindow()（本作单窗口）。
#include <cstdint>
#include <cstdio>
#include <deque>
#include <string>
#include <vector>

#include "check.h"
#include "runtime/host_window.h"
#include "runtime/machine.h"
#include "shims/register.h"

namespace {

constexpr std::uint32_t kCode = 0x00401000u;
constexpr std::uint32_t kMsgBase = 0x00402000u;
constexpr std::uint32_t kResBase = 0x00403000u;
constexpr std::uint32_t kStubBase = 0x7FF80000u;
constexpr std::uint32_t kSentinel = 0xFFFFFFFFu;

struct Asm {
    std::uint32_t base = kCode;
    std::vector<std::uint8_t> b;
    std::uint32_t ea() const { return base + static_cast<std::uint32_t>(b.size()); }
    void u8(std::uint8_t v) { b.push_back(v); }
    void u32(std::uint32_t v) {
        for (int i = 0; i < 4; ++i) u8(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF));
    }
    void pushImm(std::uint32_t v) { u8(0x68); u32(v); }
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
    for (std::uint32_t id = 0; id < m.shims().all().size(); ++id) {
        lift::ShimDef* d = const_cast<lift::ShimDef*>(m.shims().byId(id));
        if (!d || !d->fn) continue;
        d->stub_ea = kStubBase + id * 4u;
        m.cache().installShim(d->stub_ea, d->id);
    }
    const std::uint32_t get_message = stubOf(m, "GetMessageA");
    const std::uint32_t get_async = stubOf(m, "GetAsyncKeyState");
    LIFT_CHECK_NE_U32(get_message, 0u);
    LIFT_CHECK_NE_U32(get_async, 0u);
    if (get_message == 0 || get_async == 0) return lift_test::finish("window-events");

    std::vector<std::uint8_t> zeros(28u * 3u, 0u);
    LIFT_CHECK(m.placeData(kMsgBase, zeros.data(), zeros.size(), &err));
    LIFT_CHECK(m.placeData(kResBase, zeros.data(), 20u, &err));

    Asm a;
    for (int i = 0; i < 4; ++i) {
        a.pushImm(0);  // wMsgFilterMax
        a.pushImm(0);  // wMsgFilterMin
        a.pushImm(0);  // hWnd
        a.pushImm(kMsgBase + static_cast<std::uint32_t>(i) * 28u);
        a.callRel(get_message);
        a.storeEax(kResBase + static_cast<std::uint32_t>(i) * 4u);
    }
    a.pushImm(0x41u);  // VK 'A'（影子键状态由下面 setKeyDown 置位）
    a.callRel(get_async);
    a.storeEax(kResBase + 16u);
    a.ret();
    LIFT_CHECK(m.placeCode(kCode, a.b.data(), a.b.size(), &err));

    lift::WindowManager* wm = m.cpu().windows;
    LIFT_CHECK(wm != nullptr);
    if (wm == nullptr) return lift_test::finish("window-events");
    const std::uint32_t hwnd = wm->createWindow("WindowEvents", 0x10000000u, 0, 0, 0, 1280, 720, 0);
    LIFT_CHECK_EQ_U32(wm->primaryWindow(), hwnd);

    std::deque<lift::WinMessage> seq;
    lift::WinMessage key;
    key.hwnd = hwnd;
    key.message = 0x0100u;  // WM_KEYDOWN
    key.wparam = 0x41u;     // 'A'
    key.lparam = 0x001E0001u;
    seq.push_back(key);
    lift::WinMessage move;
    move.hwnd = hwnd;
    move.message = 0x0200u;  // WM_MOUSEMOVE
    move.lparam = (360u << 16) | 640u;
    seq.push_back(move);
    lift::WinMessage close;
    close.hwnd = hwnd;
    close.message = 0x0010u;  // WM_CLOSE
    seq.push_back(close);

    std::size_t pumped = 0;
    wm->setHostEventPump([&](lift::X86&) {
        if (pumped < seq.size()) {
            wm->post(seq[pumped++]);
        } else {
            wm->postQuit(0);
        }
    });
    LIFT_CHECK(wm->hasHostEventPump());

    // ③ 影子键状态：WM_KEYDOWN 的设置端与 GetAsyncKeyState 的读取端必须同源。
    wm->setKeyDown(0x41u, true);
    LIFT_CHECK(wm->keyDown(0x41u));

    m.cpu().eip = kCode;
    lift::RunOptions opt;
    opt.until_ea = kSentinel;
    opt.max_insns = 10000;
    const lift::RunResult r = m.run(opt);
    g_current_case = "run";
    LIFT_CHECK(r.reason == lift::StopReason::UntilEa);
    LIFT_CHECK_EQ_U32(r.ea, kSentinel);
    LIFT_CHECK_EQ_U32(m.mem().rd32(kResBase + 0u), 1u);
    LIFT_CHECK_EQ_U32(m.mem().rd32(kResBase + 4u), 1u);
    LIFT_CHECK_EQ_U32(m.mem().rd32(kResBase + 8u), 1u);
    LIFT_CHECK_EQ_U32(m.mem().rd32(kResBase + 12u), 0u);  // WM_QUIT ⇒ GetMessageA 返回 0
    LIFT_CHECK_EQ_U32(m.mem().rd32(kResBase + 16u), 0x8000u);  // GetAsyncKeyState('A') = 按下

    g_current_case = "messages";
    LIFT_CHECK_EQ_U32(m.mem().rd32(kMsgBase + 0u + 0u), hwnd);
    LIFT_CHECK_EQ_U32(m.mem().rd32(kMsgBase + 0u + 4u), 0x0100u);
    LIFT_CHECK_EQ_U32(m.mem().rd32(kMsgBase + 0u + 8u), 0x41u);
    LIFT_CHECK_EQ_U32(m.mem().rd32(kMsgBase + 28u + 4u), 0x0200u);
    LIFT_CHECK_EQ_U32(m.mem().rd32(kMsgBase + 28u + 12u), (360u << 16) | 640u);
    LIFT_CHECK_EQ_U32(m.mem().rd32(kMsgBase + 56u + 4u), 0x0010u);  // WM_CLOSE
    LIFT_CHECK_EQ_U32(pumped, seq.size());
    wm->clearKeys();
    LIFT_CHECK(!wm->keyDown(0x41u));

    // ① 未安装事件源：get 保持旧语义（false；调用方仍可 abort）。
    lift::WindowManager plain;
    lift::WinMessage out;
    LIFT_CHECK(!plain.get(m.cpu(), &out));
    LIFT_CHECK(!plain.hasHostEventPump());

    return lift_test::finish("window-events");
}
