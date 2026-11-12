// emulator-lift · MessageBox（uType→返回值表 / headless 不阻塞 / 模态状态机 / Yield 单令牌）
//
// 判据（红了意味着什么）：
//   ① **返回值表**：`uType & 0x0F` 的每一档都给出**合法**的 Win32 返回值；`MB_YESNO` 的默认键
//      **必须是 IDNO(7)**（硬要求：boot 的 `uType=0x34`，答 YES(`0x4BA82B cmp eax,6`) 会删 marker
//      + 返回 1 ⇒ 弹启动设置然后退出）。未知低 4 位 ⇒ 按 MB_OK + `unknown_type`（⛔ 不静默）。
//   ② **headless 不阻塞**：`Fixture`（`initBare`、没有 sink）直接调 shim ⇒ 立即返回上述默认值、
//      `call_count` 恰好 +1、进程不挂（本用例跑完就是证据）。⛔ 窗口路径不在这里测。
//   ③ **模态状态机**（纯）：按下沿命中 / Enter / Esc / X / DEFBUTTON2 / YESNO 覆盖 / 焦点循环 /
//      quit 收口 / 空白处不提交 / 布局不出界。⛔ 不需要 SDL / 线程 / 安装。
//   ④ **Yield 是真的**：假 sink 在 `waitHostEvents` 里起第二条执行流去 `tryAcquire` 令牌 ——
//      模态循环若不 Yield，它永远拿不到 ⇒ `yield_observed=false` 当场红；拿到时
//      `concurrent()==1` ⇒ `maxConcurrent()` 不得 > 1。**不靠墙钟**。
//   ⑤ **合成帧**：整幅不透明 RGBA8（尺寸 = guest 帧），角落是纯色底、面板内是面板底色。
//
// ★ 全部 pure（顶层 CTest 注册）：不需要游戏安装 / 窗口 / LFS。
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "check.h"
#include "fixture.h"
#include "runtime/font.h"
#include "runtime/guest_token.h"
#include "runtime/host_window.h"
#include "runtime/input.h"
#include "runtime/machine.h"
#include "runtime/message_box.h"
#include "runtime/present.h"
#include "shims/register.h"

namespace {

constexpr std::uint32_t kIdOk = 1;
constexpr std::uint32_t kIdCancel = 2;
constexpr std::uint32_t kIdAbort = 3;
constexpr std::uint32_t kIdRetry = 4;
constexpr std::uint32_t kIdYes = 6;
constexpr std::uint32_t kIdNo = 7;
constexpr std::uint32_t kIdTryAgain = 10;
constexpr std::uint32_t kIdContinue = 11;

// 每个 uType 的**期望默认值**（= Win32 的默认按钮；表在 `message_box.h` / 设计报告 §4）。
struct DefaultCase {
    std::uint32_t u_type;
    std::uint32_t expected;
    const char* name;
};

const DefaultCase kDefaultCases[] = {
    {0x00000000u, kIdOk, "MB_OK"},                       // 默认键 = 唯一按钮
    {0x00000001u, kIdOk, "MB_OKCANCEL"},                 // DEFBUTTON1 = OK
    {0x00000002u, kIdAbort, "MB_ABORTRETRYIGNORE"},      // DEFBUTTON1 = Abort
    {0x00000003u, kIdYes, "MB_YESNOCANCEL"},             // DEFBUTTON1 = Yes
    {0x00000034u, kIdNo, "MB_YESNO|ICONWARNING"},        // ★ 硬要求：IDNO(7)，不是 IDYES
    {0x00000005u, kIdRetry, "MB_RETRYCANCEL"},           // DEFBUTTON1 = Retry
    {0x00000006u, kIdCancel, "MB_CANCELTRYCONTINUE"},    // DEFBUTTON1 = Cancel
    {0x000003F0u, kIdOk, "MB_OK|DEFBUTTON4(夹到唯一按钮)"},
};

// 找某个返回值的按钮矩形（测试用它算点击点；⛔ 不抄坐标）。
lift::ModalRect rectForId(const lift::ModalSpec& spec, const lift::ModalLayout& layout, std::uint32_t id) {
    for (std::size_t i = 0; i < spec.buttons.size() && i < layout.button_rects.size(); ++i) {
        if (spec.buttons[i].id == id) return layout.button_rects[i];
    }
    return lift::ModalRect{};
}

lift::ModalInput pointAt(const lift::ModalRect& r, bool pressed) {
    lift::ModalInput in;
    in.mouse_x = r.x + r.w / 2;
    in.mouse_y = r.y + r.h / 2;
    in.lbutton = pressed;
    return in;
}

// =======================================================================================
// ② headless：Fixture 直接调 shim（没有 sink ⇒ runModalDialog 立即默认值，⛔ 不阻塞）
// =======================================================================================
void caseHeadlessDefaults() {
    lift_test::g_current_case = "messagebox/headless-defaults";
    lift_fixture::Fixture f;
    LIFT_CHECK(f.ok);
    if (!f.ok) return;
    // bare 机器没有 bind()：夹具自己装注册表（与 tests/user32_window.cpp 同一口径）。
    lift::registerUser32MenuShims(f.m.shims());
    const std::uint32_t stub = f.use("user32", "MessageBoxA");
    LIFT_CHECK(stub != 0);
    const std::uint32_t text = f.cstr("hello");
    const std::uint32_t cap = f.cstr("lift");

    std::uint64_t calls = 0;
    for (const DefaultCase& c : kDefaultCases) {
        const std::uint32_t rc = f.call(stub, {0u, text, cap, c.u_type});
        ++calls;
        if (rc != c.expected) {
            lift_test::failf(__FILE__, __LINE__, "%s: uType=0x%08X expected %u, got %u", c.name, c.u_type,
                             c.expected, rc);
        }
        ++lift_test::g_checks;
    }
    // 未知低 4 位（0x3F & 0x0F = 0xF）⇒ MB_OK，且必须**记下来**（⛔ 不静默）。
    const std::uint32_t unknown = f.call(stub, {0u, text, cap, 0x0000003Fu});
    LIFT_CHECK_EQ_U32(unknown, kIdOk);
    ++calls;

    LIFT_CHECK(!f.m.presenter().hasSink());  // 这就是"headless 分支"的判据
    const lift::ShimDef* def = f.m.shims().find("user32", "MessageBoxA");
    LIFT_CHECK(def != nullptr);
    if (def != nullptr) LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(def->call_count), static_cast<std::int64_t>(calls));
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(lift::messageBoxCalls()), static_cast<std::int64_t>(calls));
    LIFT_CHECK_EQ_U32(lift::messageBoxLastType(), 0x0000003Fu);
    LIFT_CHECK_EQ_U32(lift::messageBoxLastResult(), kIdOk);
    // ★ 进程没挂 / 没建窗口：跑到这里就是判据（⛔ 不靠墙钟）。
}

// =======================================================================================
// ③ 模态状态机（纯）
// =======================================================================================
void caseStateYesNo() {
    lift_test::g_current_case = "messagebox/yesno";
    lift::ModalSpec spec = lift::modalSpecFromUType(0x34u);
    LIFT_CHECK_EQ_U32(spec.buttons.size() == 2 ? 1u : 0u, 1u);
    LIFT_CHECK_EQ_U32(lift::modalDefaultResult(spec), kIdNo);
    LIFT_CHECK_EQ_U32(spec.esc_result, kIdNo);
    LIFT_CHECK_EQ_U32(spec.close_result, kIdNo);

    // Enter ⇒ 默认键（No）
    {
        lift::ModalState st(spec);
        lift::ModalInput in;
        in.enter = true;
        const auto r = st.step(in);
        LIFT_CHECK(r.has_value());
        if (r) LIFT_CHECK_EQ_U32(*r, kIdNo);
    }
    // Esc ⇒ IDNO
    {
        lift::ModalState st(spec);
        lift::ModalInput in;
        in.escape = true;
        const auto r = st.step(in);
        LIFT_CHECK(r.has_value());
        if (r) LIFT_CHECK_EQ_U32(*r, kIdNo);
    }
    // X（关窗）⇒ IDNO
    {
        lift::ModalState st(spec);
        lift::ModalInput in;
        in.close_requested = true;
        const auto r = st.step(in);
        LIFT_CHECK(r.has_value());
        if (r) LIFT_CHECK_EQ_U32(*r, kIdNo);
    }
    // 点击 No：**按下沿**提交（注入的抬起可能永远不来）；没按下时只 hover ⇒ 继续等。
    {
        lift::ModalState st(spec);
        const lift::ModalRect no = rectForId(spec, st.layout(), kIdNo);
        LIFT_CHECK(!no.empty());
        lift::ModalInput hover = pointAt(no, false);
        LIFT_CHECK(!st.step(hover).has_value());
        lift::ModalInput down = pointAt(no, true);
        const auto r = st.step(down);
        LIFT_CHECK(r.has_value());
        if (r) LIFT_CHECK_EQ_U32(*r, kIdNo);
    }
    // 点击 Yes ⇒ IDYES（只有**用户真的点**才给 YES；默认路径永不）
    {
        lift::ModalState st(spec);
        const lift::ModalRect yes = rectForId(spec, st.layout(), kIdYes);
        LIFT_CHECK(!yes.empty());
        const auto r = st.step(pointAt(yes, true));
        LIFT_CHECK(r.has_value());
        if (r) LIFT_CHECK_EQ_U32(*r, kIdYes);
    }
    // 点在面板空白处 ⇒ 不提交
    {
        lift::ModalState st(spec);
        lift::ModalInput in;
        in.mouse_x = st.layout().panel.x + 3;
        in.mouse_y = st.layout().panel.y + 3;
        in.lbutton = true;
        LIFT_CHECK(!st.step(in).has_value());
    }
    // quit（脚本 quit / marker 超时）⇒ 默认值收口（⛔ 不挂死）
    {
        lift::ModalState st(spec);
        lift::ModalInput in;
        in.quit_requested = true;
        const auto r = st.step(in);
        LIFT_CHECK(r.has_value());
        if (r) LIFT_CHECK_EQ_U32(*r, kIdNo);
        LIFT_CHECK(st.quitRequested());
    }
}

void caseStateGroups() {
    lift_test::g_current_case = "messagebox/groups";
    struct Case {
        std::uint32_t u_type;
        std::uint32_t click_id;
        std::uint32_t expected;
    };
    const Case cases[] = {
        {0x00000000u, kIdOk, kIdOk},
        {0x00000001u, kIdCancel, kIdCancel},
        {0x00000002u, kIdRetry, kIdRetry},
        {0x00000003u, kIdCancel, kIdCancel},
        {0x00000005u, kIdCancel, kIdCancel},
        {0x00000006u, kIdTryAgain, kIdTryAgain},
        {0x00000006u, kIdContinue, kIdContinue},
    };
    for (const Case& c : cases) {
        const lift::ModalSpec spec = lift::modalSpecFromUType(c.u_type);
        lift::ModalState st(spec);
        const lift::ModalRect r = rectForId(spec, st.layout(), c.click_id);
        if (r.empty()) {
            lift_test::failf(__FILE__, __LINE__, "uType=0x%08X 没有 id=%u 的按钮", c.u_type, c.click_id);
            ++lift_test::g_checks;
            continue;
        }
        const auto res = st.step(pointAt(r, true));
        ++lift_test::g_checks;
        if (!res || *res != c.expected) {
            lift_test::failf(__FILE__, __LINE__, "uType=0x%08X click %u expected %u, got %s", c.u_type,
                             c.click_id, c.expected, res ? std::to_string(*res).c_str() : "(none)");
        }
    }
    // MB_ABORTRETRYIGNORE 的 Esc/X 偏差：Win32 没有 Cancel ⇒ 取默认键 Abort。
    {
        const lift::ModalSpec spec = lift::modalSpecFromUType(0x2u);
        LIFT_CHECK_EQ_U32(spec.esc_result, kIdAbort);
        LIFT_CHECK_EQ_U32(spec.close_result, kIdAbort);
    }
    // YesNoCancel 的 Esc/X = Cancel
    {
        const lift::ModalSpec spec = lift::modalSpecFromUType(0x3u);
        LIFT_CHECK_EQ_U32(spec.esc_result, kIdCancel);
        LIFT_CHECK_EQ_U32(spec.close_result, kIdCancel);
    }
}

void caseStateDefbutton() {
    lift_test::g_current_case = "messagebox/defbutton";
    // 0x101 = MB_OKCANCEL | MB_DEFBUTTON2 ⇒ Enter = Cancel(2)
    {
        const lift::ModalSpec spec = lift::modalSpecFromUType(0x101u);
        LIFT_CHECK_EQ_U32(lift::modalDefaultResult(spec), kIdCancel);
        lift::ModalState st(spec);
        lift::ModalInput in;
        in.enter = true;
        const auto r = st.step(in);
        LIFT_CHECK(r.has_value());
        if (r) LIFT_CHECK_EQ_U32(*r, kIdCancel);
    }
    // 0x104 = MB_YESNO | MB_DEFBUTTON2 ⇒ **仍** 7（Yes 永远不做默认键），且记 defbutton_overridden
    {
        const lift::ModalSpec spec = lift::modalSpecFromUType(0x104u);
        LIFT_CHECK_EQ_U32(lift::modalDefaultResult(spec), kIdNo);
        LIFT_CHECK(spec.defbutton_overridden);
        lift::ModalState st(spec);
        lift::ModalInput in;
        in.enter = true;
        const auto r = st.step(in);
        LIFT_CHECK(r.has_value());
        if (r) LIFT_CHECK_EQ_U32(*r, kIdNo);
    }
    // 越界的 DEFBUTTON4（只有 2 个按钮）⇒ 夹到最后一个按钮，不崩。
    {
        const lift::ModalSpec spec = lift::modalSpecFromUType(0x301u);  // OKCANCEL + DEFBUTTON4
        LIFT_CHECK_EQ_U32(lift::modalDefaultResult(spec), kIdCancel);
    }
}

void caseStateUnknownAndFocus() {
    lift_test::g_current_case = "messagebox/unknown-focus";
    // 未知低 4 位 ⇒ MB_OK + unknown_type（调用方据此打 warn）
    {
        const lift::ModalSpec spec = lift::modalSpecFromUType(0x3Fu);
        LIFT_CHECK(spec.unknown_type);
        LIFT_CHECK_EQ_U32(spec.buttons.size() == 1 ? 1u : 0u, 1u);
        LIFT_CHECK_EQ_U32(lift::modalDefaultResult(spec), kIdOk);
    }
    // Tab 焦点循环（OK / Cancel；默认焦点 = 0）
    {
        const lift::ModalSpec spec = lift::modalSpecFromUType(0x1u);
        lift::ModalState st(spec);
        LIFT_CHECK_EQ_I64(st.layout().focus, 0);
        lift::ModalInput in;
        in.tab = true;
        LIFT_CHECK(!st.step(in).has_value());
        LIFT_CHECK_EQ_I64(st.layout().focus, 1);
        in.tab = false;
        LIFT_CHECK(!st.step(in).has_value());
        in.tab = true;
        LIFT_CHECK(!st.step(in).has_value());
        LIFT_CHECK_EQ_I64(st.layout().focus, 0);
        // Enter 走**焦点**按钮（Win32 的真语义：Enter 激活当前焦点）
        in.tab = false;
        st.step(in);
        in.tab = true;
        st.step(in);  // 焦点 → Cancel
        LIFT_CHECK_EQ_I64(st.layout().focus, 1);
        lift::ModalInput enter;
        enter.enter = true;
        const auto r = st.step(enter);
        LIFT_CHECK(r.has_value());
        if (r) LIFT_CHECK_EQ_U32(*r, kIdCancel);
    }
    // 布局：面板/按钮都在屏幕内、按钮互不重叠、面板居中
    {
        const lift::ModalSpec spec = lift::modalSpecFromUType(0x3u);
        const lift::ModalLayout l = lift::modalLayout(spec, 1280, 720);
        LIFT_CHECK(l.panel.x >= 0 && l.panel.y >= 0);
        LIFT_CHECK(l.panel.x + l.panel.w <= 1280 && l.panel.y + l.panel.h <= 720);
        LIFT_CHECK_EQ_I64(l.panel.x, (1280 - l.panel.w) / 2);
        LIFT_CHECK_EQ_I64(l.panel.y, (720 - l.panel.h) / 2);
        for (std::size_t i = 0; i < l.button_rects.size(); ++i) {
            const lift::ModalRect& a = l.button_rects[i];
            LIFT_CHECK(a.x >= l.panel.x && a.y >= l.panel.y);
            LIFT_CHECK(a.x + a.w <= l.panel.x + l.panel.w);
            LIFT_CHECK(a.y + a.h <= l.panel.y + l.panel.h);
            for (std::size_t j = i + 1; j < l.button_rects.size(); ++j) {
                const lift::ModalRect& b = l.button_rects[j];
                const bool overlap = a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
                LIFT_CHECK(!overlap);
            }
        }
    }
}

void caseCompose() {
    lift_test::g_current_case = "messagebox/compose";
    lift::FontEngine fonts;
    fonts.setSyntheticForTest(true);
    lift::ModalSpec spec = lift::makeMessageBoxSpec(0x34u, "lift", "line one\r\nline two");
    LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(spec.lines.size()), 2u);
    lift::ModalLayout layout = lift::modalLayout(spec, 1280, 720);
    lift::ModalInput in;
    in.mouse_x = layout.panel.x + 4;
    in.mouse_y = layout.panel.y + 4;
    std::vector<std::uint8_t> img;
    int w = 0;
    int h = 0;
    bool font_missing = true;
    const bool ok = lift::composeModalFrame(spec, layout, in, &fonts, nullptr, 0, 0, 0, &img, &w, &h,
                                            &font_missing);
    LIFT_CHECK(ok);
    LIFT_CHECK_EQ_I64(w, 1280);
    LIFT_CHECK_EQ_I64(h, 720);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(img.size()), 1280ll * 720ll * 4ll);
    LIFT_CHECK(!font_missing);  // synthetic 字形必须可用（⛔ 不许"没字体就当画过了"）
    bool opaque = true;
    for (std::size_t i = 3; i < img.size(); i += 4)
        if (img[i] != 255) {
            opaque = false;
            break;
        }
    LIFT_CHECK(opaque);
    // (0,0) = 纯色底（boot 没有末帧 ⇒ #202020）；面板内 = 面板底色。
    LIFT_CHECK_EQ_I64(img[0], 32);
    LIFT_CHECK_EQ_I64(img[1], 32);
    LIFT_CHECK_EQ_I64(img[2], 32);
    const std::size_t panel_off = (static_cast<std::size_t>(layout.panel.y + 2) * 1280u +
                                   static_cast<std::size_t>(layout.panel.x + 2)) * 4u;
    LIFT_CHECK_EQ_I64(img[panel_off + 0], 226);
    LIFT_CHECK_EQ_I64(img[panel_off + 1], 226);
    LIFT_CHECK_EQ_I64(img[panel_off + 2], 230);
}

// =======================================================================================
// ④ 驱动层：假 sink + 第二条执行流抢令牌（Yield 真的发生了）
// =======================================================================================
class FakeModalSink final : public lift::PresentSink, public lift::ModalPresenter {
public:
    bool submitFrame(const lift::SurfaceArena&, const lift::GuestSurface&, std::string*) override {
        return true;
    }
    lift::ModalPresenter* modalPresenter() override { return this; }

    bool presentModal(const std::uint8_t* rgba, int w, int h, std::string* err) override {
        ++present_calls;
        active = true;
        last_w = w;
        last_h = h;
        frame_opaque = rgba != nullptr && w > 0 && h > 0;
        const std::size_t bytes = static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4u;
        frame_sized = rgba != nullptr && bytes > 0;
        for (std::size_t i = 3; rgba != nullptr && i < bytes; i += 4) {
            if (rgba[i] != 255) {
                frame_opaque = false;
                break;
            }
        }
        if (err) err->clear();
        return true;
    }
    void waitHostEvents(int) override {
        ++wait_calls;
        if (wait_calls == 1) {
            // ★ 第二条执行流：模态循环若**不** Yield，它永远拿不到令牌 ⇒ yield_observed=false。
            //   用 tryAcquire + 有限轮询（⛔ 不阻塞等锁：红的时候也要能退出）。
            std::thread t([this]() {
                for (int i = 0; i < 500; ++i) {
                    if (lift::guestToken().tryAcquire()) {
                        observed_concurrent = lift::guestToken().concurrent();
                        lift::guestToken().release();
                        yield_observed = true;
                        return;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            });
            t.join();
            // ★ 重入：对话框里再弹一个 ⇒ 立即默认值（⛔ 不嵌套模态循环）
            nested_result = lift::runModalDialog(machine, machine.cpu(), spec);
        } else if (wait_calls == 2) {
            // 第二次等待时注入 Enter（默认键 = No）⇒ 下一次 step 收口。
            if (machine.cpu().windows != nullptr)
                machine.cpu().windows->setKeyDown(lift::InputSource::Os, 0x0Du, true);
        }
    }
    bool closeRequested() const override { return false; }
    bool modalActive() const override { return active; }
    void endModal() override { active = false; }

    lift::Machine& machine;
    const lift::ModalSpec& spec;
    int present_calls = 0;
    int wait_calls = 0;
    int last_w = 0;
    int last_h = 0;
    bool frame_opaque = false;
    bool frame_sized = false;
    bool yield_observed = false;
    unsigned observed_concurrent = 0;
    std::uint32_t nested_result = 0;
    bool active = false;

    FakeModalSink(lift::Machine& m, const lift::ModalSpec& s) : machine(m), spec(s) {}
};

void caseDriverYield() {
    lift_test::g_current_case = "messagebox/driver-yield";
    lift_fixture::Fixture f;
    LIFT_CHECK(f.ok);
    if (!f.ok) return;
    f.m.fonts().setSyntheticForTest(true);
    lift::registerUser32MenuShims(f.m.shims());
    const lift::ModalSpec spec = lift::makeMessageBoxSpec(0x34u, "lift", "modal body");
    auto* sink = new FakeModalSink(f.m, spec);
    f.m.presenter().setSink(std::unique_ptr<lift::PresentSink>(sink));
    const std::uint32_t stub = f.use("user32", "MessageBoxA");
    const std::uint32_t text = f.cstr("modal body");
    const std::uint32_t cap = f.cstr("lift");
    const std::uint32_t rc = f.call(stub, {0u, text, cap, 0x34u});
    LIFT_CHECK_EQ_U32(rc, kIdNo);  // ★ MB_YESNO 默认 IDNO（硬要求）
    LIFT_CHECK_EQ_U32(sink->nested_result, kIdNo);  // 重入 ⇒ 立即默认值
    LIFT_CHECK(sink->yield_observed);               // Yield 真的发生了（第二条执行流拿到了令牌）
    LIFT_CHECK_EQ_I64(sink->observed_concurrent, 1);  // 同时只有一条执行流持令牌
    LIFT_CHECK_EQ_I64(lift::guestToken().maxConcurrent(), 1);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(lift::guestToken().violations()), 0);
    LIFT_CHECK(sink->present_calls >= 1);  // 至少画了一帧（不是"不显示直接返回"）
    LIFT_CHECK_EQ_I64(sink->last_w, 1280);
    LIFT_CHECK_EQ_I64(sink->last_h, 720);
    LIFT_CHECK(sink->frame_sized);
    LIFT_CHECK(sink->frame_opaque);  // 整幅不透明 RGBA8
    LIFT_CHECK(!sink->modalActive());  // endModal 已收口
    const lift::ShimDef* def = f.m.shims().find("user32", "MessageBoxA");
    LIFT_CHECK(def != nullptr);
    if (def != nullptr) LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(def->call_count), 1);
}

// ★ 注入通道进模态：`runModalDialog` 每帧 `tickInputScript`（模态期间没有块边界，这是唯一
//   能让 `--input-script` 动作落地的点）。这里在**第一次等待**里追加脚本动作，确定性验证
//   "脚本按下的键真的被模态读到"（⛔ 不靠墙钟 / 不靠真实窗口）。
class ScriptSink final : public lift::PresentSink, public lift::ModalPresenter {
public:
    bool submitFrame(const lift::SurfaceArena&, const lift::GuestSurface&, std::string*) override {
        return true;
    }
    lift::ModalPresenter* modalPresenter() override { return this; }
    bool presentModal(const std::uint8_t*, int, int, std::string* err) override {
        ++present_calls;
        if (err) err->clear();
        return true;
    }
    void waitHostEvents(int) override {
        ++wait_calls;
        if (wait_calls == 1) {
            // `at insn 0` 在下一个 tick 必到期（模态里 insns 冻住但已经 > 0）；
            // 第二条远期动作让脚本保持 active ⇒ 键**不**在同 tick 被"结束时抬起"。
            std::string err;
            appended = machine.inputScript().appendText(
                "at insn 0\nkey_down 0x0D\nat insn 2000000000\nmouse_move 640 360\n", "messagebox-test",
                &err);
        }
    }
    bool closeRequested() const override { return false; }
    bool modalActive() const override { return active; }
    void endModal() override { active = false; }

    lift::Machine& machine;
    int present_calls = 0;
    int wait_calls = 0;
    bool appended = false;
    bool active = false;

    explicit ScriptSink(lift::Machine& m) : machine(m) {}
};

void caseDriverScript() {
    lift_test::g_current_case = "messagebox/driver-script";
    lift_fixture::Fixture f;
    LIFT_CHECK(f.ok);
    if (!f.ok) return;
    f.m.fonts().setSyntheticForTest(true);
    lift::registerUser32MenuShims(f.m.shims());
    const lift::ModalSpec spec = lift::makeMessageBoxSpec(0x34u, "lift", "modal body");
    auto* sink = new ScriptSink(f.m);
    f.m.presenter().setSink(std::unique_ptr<lift::PresentSink>(sink));
    const std::uint32_t stub = f.use("user32", "MessageBoxA");
    const std::uint32_t text = f.cstr("modal body");
    const std::uint32_t cap = f.cstr("lift");
    const std::uint32_t rc = f.call(stub, {0u, text, cap, 0x34u});
    LIFT_CHECK_EQ_U32(rc, kIdNo);
    LIFT_CHECK(sink->appended);  // 脚本确实追加过（否则下面这两条是空转）
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.m.inputScript().appliedCount()), 1);
    LIFT_CHECK(!sink->modalActive());
}

}  // namespace

int main() {
    caseHeadlessDefaults();
    caseStateYesNo();
    caseStateGroups();
    caseStateDefbutton();
    caseStateUnknownAndFocus();
    caseCompose();
    caseDriverYield();
    caseDriverScript();
    return lift_test::finish("messagebox");
}
