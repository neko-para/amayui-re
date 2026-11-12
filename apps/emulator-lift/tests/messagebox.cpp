// emulator-lift · MessageBox / AGERC「終了」框的**纯映射 + 系统框缝**守卫（pure）
//
// 判据（红了意味着什么）：
//   ① **返回值表**：`uType & 0x0F` 的每一档都给出合法 Win32 返回值；`MB_YESNO` 的默认键
//      **必须是 IDNO(7)**（硬要求：boot 的 `uType=0x34`，答 YES(`0x4BA82B cmp eax,6`) 会删 marker
//      + 返回 1 ⇒ 弹启动设置然后退出）。未知低 4 位 ⇒ MB_OK + `unknown_type`。
//   ② **headless 不阻塞**：`Fixture`（initBare、没有 sink）直接调 `MessageBoxA` ⇒ 立即默认值、
//      `call_count` 恰好 +1、进程不挂。
//   ③ **系统框缝**：`PresentSink::showSystemMessageBox` 返回的 id 原样成为 shim 的返回值；
//      返回 false（系统框起不来 / 测试替身没有）⇒ 默认值 + 不挂死。
//   ④ **AGERC `_ShowDialog@12`**：只对 `id==3` 出手；结果 1/2/0 才是三臂；2/0 必须调
//      `cancelCloseRequest()`（窗口留着），1 不调（guest 自己关）；其它 id ⇒ 1 + 不弹框；
//      `--close-confirm exit/title/cancel` 策略短路 1/2/0 且同样在 2/0 取消关窗。
//   ⑤ **`close_window` 宿主动作**：脚本到期后经 `PresentSink::requestClose()` 交付（一次）。
//
// ★ 全部 pure（顶层 CTest 注册）：不需要游戏安装 / 窗口 / LFS。系统框本身由
//   `tests/close_confirm.cpp`（assets）用 `--present window` 跑真闭窗链；这里只钉映射与缝。
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "check.h"
#include "fixture.h"
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

// 系统框的**测试替身**：记录 spec / 调用次数，按预设回答（或失败）。
class FakeSystemBox final : public lift::PresentSink {
public:
    bool submitFrame(const lift::SurfaceArena&, const lift::GuestSurface&, std::string*) override {
        return true;
    }
    bool showSystemMessageBox(const lift::ModalSpec& spec, std::uint32_t* result) override {
        ++show_calls;
        last_spec = spec;
        if (!answer) return false;
        *result = answer_id;
        return true;
    }
    void requestClose() override { ++close_requests; }
    void cancelCloseRequest() override { ++cancel_calls; }

    int show_calls = 0;
    int close_requests = 0;
    int cancel_calls = 0;
    bool answer = false;
    std::uint32_t answer_id = 0;
    lift::ModalSpec last_spec;
};

void caseHeadlessDefaults() {
    lift_test::g_current_case = "messagebox/headless-defaults";
    lift_fixture::Fixture f;
    LIFT_CHECK(f.ok);
    if (!f.ok) return;
    lift::registerUser32MenuShims(f.m.shims());
    const std::uint32_t stub = f.use("user32", "MessageBoxA");
    LIFT_CHECK(stub != 0);
    const std::uint32_t text = f.cstr("hello");
    const std::uint32_t cap = f.cstr("lift");
    struct Case {
        std::uint32_t u_type;
        std::uint32_t expected;
        const char* name;
    };
    const Case cases[] = {
        {0x00000000u, kIdOk, "MB_OK"},
        {0x00000001u, kIdOk, "MB_OKCANCEL"},
        {0x00000002u, kIdAbort, "MB_ABORTRETRYIGNORE"},
        {0x00000003u, kIdYes, "MB_YESNOCANCEL"},
        {0x00000034u, kIdNo, "MB_YESNO|ICONWARNING"},  // ★ 硬要求 IDNO
        {0x00000005u, kIdRetry, "MB_RETRYCANCEL"},
        {0x00000006u, kIdCancel, "MB_CANCELTRYCONTINUE"},
        {0x0000003Fu, kIdOk, "unknown-low-nibble"},
        {0x000003F0u, kIdOk, "defbutton-clamped"},
    };
    std::uint64_t calls = 0;
    for (const Case& c : cases) {
        const std::uint32_t rc = f.call(stub, {0u, text, cap, c.u_type});
        ++calls;
        ++lift_test::g_checks;
        if (rc != c.expected) {
            lift_test::failf(__FILE__, __LINE__, "%s: uType=0x%08X expected %u, got %u", c.name, c.u_type,
                             c.expected, rc);
        }
    }
    LIFT_CHECK(!f.m.presenter().hasSink());  // 这就是"headless 分支"的判据
    const lift::ShimDef* def = f.m.shims().find("user32", "MessageBoxA");
    LIFT_CHECK(def != nullptr);
    if (def != nullptr)
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(def->call_count), static_cast<std::int64_t>(calls));
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(lift::messageBoxCalls()), static_cast<std::int64_t>(calls));
    LIFT_CHECK_EQ_U32(lift::messageBoxLastType(), 0x000003F0u);
    LIFT_CHECK_EQ_U32(lift::messageBoxLastResult(), kIdOk);
}

void caseSpecTable() {
    lift_test::g_current_case = "messagebox/spec-table";
    struct Case {
        std::uint32_t u_type;
        std::uint32_t expected_default;
        std::uint32_t expected_esc;
        std::uint32_t expected_close;
        int expected_buttons;
    };
    const Case cases[] = {
        {0x0u, kIdOk, kIdOk, kIdOk, 1},
        {0x1u, kIdOk, kIdCancel, kIdCancel, 2},
        {0x2u, kIdAbort, kIdAbort, kIdAbort, 3},
        {0x3u, kIdYes, kIdCancel, kIdCancel, 3},
        {0x4u, kIdNo, kIdNo, kIdNo, 2},
        {0x5u, kIdRetry, kIdCancel, kIdCancel, 2},
        {0x6u, kIdCancel, kIdCancel, kIdCancel, 3},
    };
    for (const Case& c : cases) {
        const lift::ModalSpec spec = lift::modalSpecFromUType(c.u_type);
        ++lift_test::g_checks;
        if (static_cast<int>(spec.buttons.size()) != c.expected_buttons) {
            lift_test::failf(__FILE__, __LINE__, "uType=0x%X expected %d buttons, got %zu", c.u_type,
                             c.expected_buttons, spec.buttons.size());
            continue;
        }
        LIFT_CHECK_EQ_U32(lift::modalDefaultResult(spec), c.expected_default);
        LIFT_CHECK_EQ_U32(spec.default_result, c.expected_default);
        LIFT_CHECK_EQ_U32(spec.esc_result, c.expected_esc);
        LIFT_CHECK_EQ_U32(spec.close_result, c.expected_close);
        int defaults = 0;
        for (const lift::ModalButton& b : spec.buttons)
            if (b.is_default) ++defaults;
        LIFT_CHECK_EQ_I64(defaults, 1);
    }
    // 未知低 4 位：MB_OK + unknown_type（调用方打 warn）
    {
        const lift::ModalSpec spec = lift::modalSpecFromUType(0x3Fu);
        LIFT_CHECK(spec.unknown_type);
        LIFT_CHECK_EQ_U32(lift::modalDefaultResult(spec), kIdOk);
    }
    // DEFBUTTON2 尊重
    {
        const lift::ModalSpec spec = lift::modalSpecFromUType(0x101u);
        LIFT_CHECK_EQ_U32(lift::modalDefaultResult(spec), kIdCancel);
    }
    // MB_YESNO | DEFBUTTON2 ⇒ 仍 IDNO + defbutton_overridden（⛔ 不许把 Yes 做默认）
    {
        const lift::ModalSpec spec = lift::modalSpecFromUType(0x104u);
        LIFT_CHECK_EQ_U32(lift::modalDefaultResult(spec), kIdNo);
        LIFT_CHECK(spec.defbutton_overridden);
    }
    // CANCELTRYCONTINUE 的按钮 id 顺序（2/10/11）+ 默认 2
    {
        const lift::ModalSpec spec = lift::modalSpecFromUType(0x6u);
        LIFT_CHECK_EQ_U32(spec.buttons[0].id, kIdCancel);
        LIFT_CHECK_EQ_U32(spec.buttons[1].id, kIdTryAgain);
        LIFT_CHECK_EQ_U32(spec.buttons[2].id, kIdContinue);
    }
}

void caseMessageText() {
    lift_test::g_current_case = "messagebox/text";
    const lift::ModalSpec spec =
        lift::makeMessageBoxSpec(0x0u, "cap", "line one\r\nline two\n\nline three\r");
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(spec.lines.size()), 4);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(spec.lines[0].size()), 8);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(spec.lines[1].size()), 8);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(spec.lines[2].size()), 0);  // 保留中间空行
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(spec.lines[3].size()), 10);
    const std::string joined = lift::modalMessageText(spec);
    LIFT_CHECK(joined == "line one\nline two\n\nline three");
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(spec.caption.size()), 3);
}

void caseCloseConfirmSpec() {
    lift_test::g_current_case = "messagebox/close-confirm-spec";
    const lift::ModalSpec spec = lift::makeCloseConfirmSpec();
    LIFT_CHECK(spec.caption == "終了");
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(spec.lines.size()), 1);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(spec.buttons.size()), 3);
    LIFT_CHECK_EQ_U32(spec.buttons[0].id, 1u);
    LIFT_CHECK_EQ_U32(spec.buttons[1].id, 2u);
    LIFT_CHECK_EQ_U32(spec.buttons[2].id, 0u);
    LIFT_CHECK_EQ_U32(lift::modalDefaultResult(spec), 1u);
    LIFT_CHECK_EQ_U32(spec.default_result, 1u);
    LIFT_CHECK_EQ_U32(spec.esc_result, 0u);
    LIFT_CHECK_EQ_U32(spec.close_result, 0u);
    LIFT_CHECK(spec.buttons[0].is_default);
    LIFT_CHECK(!spec.buttons[1].is_default && !spec.buttons[2].is_default);
    for (const lift::ModalButton& b : spec.buttons) LIFT_CHECK(!b.label.empty());
}

void casePolicy() {
    lift_test::g_current_case = "messagebox/policy";
    struct Case {
        const char* text;
        lift::CloseConfirmPolicy policy;
        const char* name;
        bool has_result;
        std::uint32_t result;
    };
    const Case cases[] = {
        {"ask", lift::CloseConfirmPolicy::Ask, "ask", false, 0},
        {"exit", lift::CloseConfirmPolicy::Exit, "exit", true, 1},
        {"title", lift::CloseConfirmPolicy::Title, "title", true, 2},
        {"cancel", lift::CloseConfirmPolicy::Cancel, "cancel", true, 0},
    };
    for (const Case& c : cases) {
        lift::CloseConfirmPolicy parsed = lift::CloseConfirmPolicy::Ask;
        LIFT_CHECK(lift::parseCloseConfirmPolicy(c.text, &parsed));
        LIFT_CHECK(parsed == c.policy);
        ++lift_test::g_checks;
        if (std::string(lift::closeConfirmPolicyName(parsed)) != c.name) {
            lift_test::failf(__FILE__, __LINE__, "policy name expected %s got %s", c.name,
                             lift::closeConfirmPolicyName(parsed));
        }
        const auto forced = lift::closeConfirmPolicyResult(parsed);
        LIFT_CHECK(forced.has_value() == c.has_result);
        if (forced) LIFT_CHECK_EQ_U32(*forced, c.result);
    }
    lift::CloseConfirmPolicy bad = lift::CloseConfirmPolicy::Ask;
    LIFT_CHECK(!lift::parseCloseConfirmPolicy("nope", &bad));
    LIFT_CHECK(!lift::parseCloseConfirmPolicy("", &bad));
    LIFT_CHECK(!lift::parseCloseConfirmPolicy("Ask", &bad));  // 大小写敏感
}

// ③ 系统框缝：MessageBoxA 的返回值 = 假系统框给出的 id；失败 ⇒ 默认值。
void caseSystemBoxSeam() {
    lift_test::g_current_case = "messagebox/system-box-seam";
    lift_fixture::Fixture f;
    LIFT_CHECK(f.ok);
    if (!f.ok) return;
    lift::registerUser32MenuShims(f.m.shims());
    auto* sink = new FakeSystemBox();
    f.m.presenter().setSink(std::unique_ptr<lift::PresentSink>(sink));
    const std::uint32_t stub = f.use("user32", "MessageBoxA");
    const std::uint32_t text = f.cstr("body");
    const std::uint32_t cap = f.cstr("cap");

    // 系统框回答 2 ⇒ MB_YESNOCANCEL 返回 2（不是默认 6）
    sink->answer = true;
    sink->answer_id = kIdCancel;
    LIFT_CHECK_EQ_U32(f.call(stub, {0u, text, cap, 0x3u}), kIdCancel);
    LIFT_CHECK_EQ_I64(sink->show_calls, 1);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(sink->last_spec.buttons.size()), 3);

    // ESC / X 的按钮 id 对应 spec（UI 侧由 SDL 的 flags 表达；这里断言 spec 给对了）
    LIFT_CHECK_EQ_U32(sink->last_spec.esc_result, kIdCancel);
    LIFT_CHECK_EQ_U32(sink->last_spec.close_result, kIdCancel);

    // 系统框起不来 ⇒ 默认值（+ 不挂）
    sink->answer = false;
    LIFT_CHECK_EQ_U32(f.call(stub, {0u, text, cap, 0x34u}), kIdNo);
    LIFT_CHECK_EQ_I64(sink->show_calls, 2);
    LIFT_CHECK_EQ_U32(lift::messageBoxLastResult(), kIdNo);
}

// ④ AGERC `_ShowDialog@12`：id==3 三臂 + 策略短路 + cancel 语义 + 其它 id。
void caseAgercShowDialog() {
    lift_test::g_current_case = "messagebox/agerc-show-dialog";
    lift_fixture::Fixture f;
    LIFT_CHECK(f.ok);
    if (!f.ok) return;
    lift::registerAgercShims(f.m.shims());
    auto* sink = new FakeSystemBox();
    f.m.presenter().setSink(std::unique_ptr<lift::PresentSink>(sink));
    const std::uint32_t stub = f.use("agerc", "_ShowDialog@12");
    LIFT_CHECK(stub != 0);

    // id==3 + ask：系统框给 2 ⇒ 返回 2，且 2/0 必须取消那次关窗请求
    f.m.setCloseConfirmPolicy(lift::CloseConfirmPolicy::Ask);
    sink->answer = true;
    sink->answer_id = 2;
    LIFT_CHECK_EQ_U32(f.call(stub, {3u, 0x00010000u, 0u}), 2u);
    LIFT_CHECK_EQ_I64(sink->show_calls, 1);
    LIFT_CHECK_EQ_I64(sink->cancel_calls, 1);
    LIFT_CHECK_EQ_U32(sink->last_spec.default_result, 1u);
    LIFT_CHECK_EQ_U32(sink->last_spec.esc_result, 0u);
    LIFT_CHECK_EQ_U32(sink->last_spec.close_result, 0u);

    // ask + 系统框给 1（終了）⇒ 返回 1，**不**取消（guest 自己关窗）
    sink->answer_id = 1;
    LIFT_CHECK_EQ_U32(f.call(stub, {3u, 0x00010000u, 0u}), 1u);
    LIFT_CHECK_EQ_I64(sink->cancel_calls, 1);  // 没有新增

    // ask + 系统框给 0 ⇒ 返回 0 + 取消
    sink->answer_id = 0;
    LIFT_CHECK_EQ_U32(f.call(stub, {3u, 0x00010000u, 0u}), 0u);
    LIFT_CHECK_EQ_I64(sink->cancel_calls, 2);

    // 系统框起不来 ⇒ 1（今天的行为）+ 不取消
    sink->answer = false;
    LIFT_CHECK_EQ_U32(f.call(stub, {3u, 0x00010000u, 0u}), 1u);
    LIFT_CHECK_EQ_I64(sink->cancel_calls, 2);

    // 策略短路：title ⇒ 2 + 取消；exit ⇒ 1 + 不取消；cancel ⇒ 0 + 取消；均不弹框
    const int before = sink->show_calls;
    f.m.setCloseConfirmPolicy(lift::CloseConfirmPolicy::Title);
    LIFT_CHECK_EQ_U32(f.call(stub, {3u, 0x00010000u, 0u}), 2u);
    LIFT_CHECK_EQ_I64(sink->cancel_calls, 3);
    f.m.setCloseConfirmPolicy(lift::CloseConfirmPolicy::Exit);
    LIFT_CHECK_EQ_U32(f.call(stub, {3u, 0x00010000u, 0u}), 1u);
    LIFT_CHECK_EQ_I64(sink->cancel_calls, 3);
    f.m.setCloseConfirmPolicy(lift::CloseConfirmPolicy::Cancel);
    LIFT_CHECK_EQ_U32(f.call(stub, {3u, 0x00010000u, 0u}), 0u);
    LIFT_CHECK_EQ_I64(sink->cancel_calls, 4);
    LIFT_CHECK_EQ_I64(sink->show_calls, before);  // 策略档一次也没弹

    // 其它 id ⇒ 1 + ⛔ 不弹框
    f.m.setCloseConfirmPolicy(lift::CloseConfirmPolicy::Ask);
    sink->answer = true;
    sink->answer_id = 2;
    LIFT_CHECK_EQ_U32(f.call(stub, {4u, 0x00010000u, 0u}), 1u);
    LIFT_CHECK_EQ_I64(sink->show_calls, before);

    // headless（没有 sink）⇒ 1
    f.m.presenter().clearSink();
    LIFT_CHECK_EQ_U32(f.call(stub, {3u, 0x00010000u, 0u}), 1u);
}

// ⑤ `close_window` 宿主动作：到期后交给 PresentSink::requestClose（一次）。
void caseCloseWindowAction() {
    lift_test::g_current_case = "messagebox/close-window-action";
    lift_fixture::Fixture f;
    LIFT_CHECK(f.ok);
    if (!f.ok) return;
    lift::registerUser32MenuShims(f.m.shims());
    auto* sink = new FakeSystemBox();
    f.m.presenter().setSink(std::unique_ptr<lift::PresentSink>(sink));
    std::string err;
    LIFT_CHECK(f.m.loadInputScriptText(
        "at insn 0\nclose_window\nat insn 2000000000\nmouse_move 640 360\n", "close-window-test", &err));
    const std::uint32_t stub = f.use("user32", "GetMenu");
    LIFT_CHECK(stub != 0);
    LIFT_CHECK_EQ_U32(f.call(stub, {0u}), 0u);
    LIFT_CHECK_EQ_I64(sink->close_requests, 1);
}

}  // namespace

int main() {
    caseHeadlessDefaults();
    caseSpecTable();
    caseMessageText();
    caseCloseConfirmSpec();
    casePolicy();
    caseSystemBoxSeam();
    caseAgercShowDialog();
    caseCloseWindowAction();
    return lift_test::finish("messagebox");
}
