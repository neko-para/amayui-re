// emulator-lift · 默认 HUD 脚本（`scripts/ui/hud.js`）的**内容判据**（B）
//
// 判据（红了意味着什么，缺一条都不算）：
//   ① **行序 / 文案**：给一份 fake 读数（frame=1234 / mouse(812,622) / fps=59.9 / script "startup" /
//      worst wall=33.3 guest=21.5 shim=4.2）⇒ display list 里的 **Text 元素逐条**等于
//        `mouse 812,622` / `script startup` / `fps 59.9` / `frame 1234` /
//        `worst 3s  wall=33.3ms guest=21.5ms shim=4.2ms`
//      顺序就是**用户看到的顺序**。红了 ⇒ 默认 HUD 的文案 / 顺序变了（或脚本没被执行）。
//   ② **没有 raster / present 两行**（用户 2026-11-18 裁决：开 overlay 时人是在体验，不需要这两个
//      开关信息）。红了 ⇒ 旧的两行又被加回来了。
//   ③ **颜色精确值**：面板 Rect 的 fill == `(16,24,32,204)`（`#101820CC` = 80% 不透明的底），
//      Text 的 color == `(235,255,235,255)`（`#EBFFEB`，⛔ 字不变淡）。红了 ⇒ HUD 底色 / 字色 /
//      半透明度被改了（窗口像素判据也会跟着红）。
//   ④ **缺省项整行省略**（⛔ 不编 0）：不给 mouse / script / fps 且 `worst_guest_ok=false`
//      ⇒ 只剩 `frame 7` 与 `worst … guest=--`（guest 那一列**没有值**时写 `--`）。
//   ⑤ **栈顶脚本名优先**：`readings.frames` 给出 top=SC0000 且 `readings.script` = "startup" 时，
//      `script` 行必须是 `script SC0000`（`game.local_frames()` 的 top 项压过 `game.script`）。
//      红了 ⇒ HUD 又只显示"当前帧"身份（槽乱序装载时不是栈顶）。
//   ⑥ **`cur` 不活动（`top` 全 false）⇒ 不许拿 `frames[0]` 冒充栈顶**：假帧源给两条 `top=false`
//      （`frames[0].script` = SC0000）+ `readings.script` = "startup" ⇒ 行必须是 `script startup`
//      （回退 `game.script`；与 ⑤ 的 top=true 用例互补）。红了 ⇒ `frames[0]` 又被当栈顶。
//   ⑦ **PROFILE 按钮（切片 3）**：面板仍是 `elements[0]`；存在 `key == "profile"` 的 Rect（画在
//      面板**之后**、面板下方）；按钮标签 `PROFILE` 是 14px / kit `idle.fg`。★ "HUD 行"的判据
//      收窄成 `Text && font_size==16 && color==FG` —— ⛔ 不 blanket 跳过所有非 16px 文本：
//      除按钮标签外，其余每个 Text 仍必须是 16px + FG。
//   ⑧ **状态机（`game.click` 自驱）**：四态 `开始` / `已复制` / `失败：<error>` / `INVALID（已复制）`
//      ／ 状态行是面板里的一行；两次击之间 `begin`/`end` 各**只**调一次；闲置帧不再调宿主、
//      但状态行留着（不按帧清）。红了 ⇒ 按钮点了没反应 / 失败被画成"已复制" / 点击被重复消费。
// ★ 脚本是**运行时读的外置文件**（`LIFT_UI_SCRIPT_DIR "/hud.js"`，与生产回退同一个路径）：
//   改脚本 ⇒ 这条用例立刻看见。缺宿主字体（`ui.measure` 的来源）⇒ `LIFT_SKIP(77)`（⛔ 不假绿）。
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "check.h"
#include "runtime/font.h"
#include "runtime/vfs.h"  // openHostFileUtf8（Windows 的 UTF-8 路径）
#include "ui/element.h"
#include "ui/script.h"

namespace {

// 生产回退的同一份路径（CMake 注入；`present_sdl.cpp` 的 `loadUiScript` 用的就是它）。
std::string hudScriptPath() { return std::string(LIFT_UI_SCRIPT_DIR) + "/hud.js"; }

bool readAll(const std::string& path, std::string* out) {
    std::FILE* f = lift::openHostFileUtf8(path, "rb");
    if (f == nullptr) return false;
    char buf[8192];
    std::size_t n = 0;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out->append(buf, n);
    const bool bad = std::ferror(f) != 0;
    std::fclose(f);
    return !bad;
}

// 把 display list 里的 **HUD 行**取出来：Text 且 `font_size == SIZE(16)` 且颜色 == FG。
// ★ 为什么收窄判据（切片 3）：PROFILE 按钮由 `kit.js` 画（14px / 三态配色），它不是 HUD 行；
//   ⛔ 不 blanket 跳过所有非 16px 文本 —— 除了按钮文本，其余 Text 仍必须是 16px + FG（见 ③）。
bool isHudLine(const lift::ui::Element& e) {
    return e.kind == lift::ui::ElementKind::Text && e.font_size == 16 && e.color.r == 235 &&
           e.color.g == 255 && e.color.b == 235 && e.color.a == 255;
}

std::vector<std::string> hudLinesOf(const lift::ui::UiBuilder& builder) {
    std::vector<std::string> out;
    for (const lift::ui::Element& e : builder.elements()) {
        if (isHudLine(e)) out.push_back(e.text);
    }
    return out;
}

void expectLine(const std::vector<std::string>& got, std::size_t i, const char* want) {
    if (i >= got.size()) {
        lift_test::failf(__FILE__, __LINE__, "缺第 %zu 条文本（期望 `%s`）", i, want);
        ++lift_test::g_checks;
        return;
    }
    ++lift_test::g_checks;
    if (got[i] != want) lift_test::failf(__FILE__, __LINE__, "第 %zu 条文本：期望 `%s`，得到 `%s`", i, want, got[i].c_str());
}

// `profile-status` 用例：状态行是面板里的一行（HUD 行），以 `profile: ` 开头。
std::string statusLineOf(const lift::ui::UiBuilder& builder) {
    for (const lift::ui::Element& e : builder.elements()) {
        if (e.kind == lift::ui::ElementKind::Text && e.text.rfind("profile: ", 0) == 0) return e.text;
    }
    return {};
}

void expectStatus(const lift::ui::UiBuilder& builder, const char* want) {
    ++lift_test::g_checks;
    const std::string got = statusLineOf(builder);
    if (got != want) lift_test::failf(__FILE__, __LINE__, "状态行：期望 `%s`，得到 `%s`", want, got.c_str());
}

// ★ 假帧源（`game.local_frames()` 的取数口）：只回给定条目 / err，不读 guest。
struct FakeFrameSource final : lift::ui::UiFrameSource {
    std::vector<lift::ui::UiFrameInfo> list;
    std::string err;
    std::vector<lift::ui::UiFrameInfo> localFrames(std::string* out_err) override {
        if (out_err != nullptr) *out_err = err;
        return list;
    }
};

// ★ 假宿主能力表（`host.profileBegin/End` 的取数口；切片 3）：记调用 + 可编排返回值。
struct FakeHostOps {
    int begin_calls = 0;
    int end_calls = 0;
    std::string last_label;
    bool begin_ok = true;
    std::string begin_error = "";
    bool end_ok = true;
    std::string end_text = "REPORT";
    std::string end_error = "";
};

bool fakeProfileBegin(void* ctx, const char* label, std::string* err) {
    auto* ops = static_cast<FakeHostOps*>(ctx);
    ++ops->begin_calls;
    ops->last_label = label != nullptr ? label : "";
    if (err != nullptr) *err = ops->begin_error;
    return ops->begin_ok;
}

bool fakeProfileEnd(void* ctx, std::string* text, std::string* err) {
    auto* ops = static_cast<FakeHostOps*>(ctx);
    ++ops->end_calls;
    if (text != nullptr) *text = ops->end_ok ? ops->end_text : "";
    if (err != nullptr) *err = ops->end_error;
    return ops->end_ok;
}

lift::ui::UiHostOps opsFor(FakeHostOps* fake) {
    lift::ui::UiHostOps ops{};
    ops.ctx = fake;
    ops.profile_begin = &fakeProfileBegin;
    ops.profile_end = &fakeProfileEnd;
    return ops;
}

// 一次带 click 的 render（`profile` 按钮）。
lift::ui::UiFrameReadings clickReadings(const lift::ui::UiHostOps* ops, std::uint64_t frame) {
    lift::ui::UiFrameReadings r;
    r.frame = frame;
    r.host = ops;
    r.click_ok = true;
    r.click_key = "profile";
    r.click_x = 10;
    r.click_y = 10;
    return r;
}

// ---------------------------------------------------------------------------------------
// ⑦ PROFILE 按钮的状态机（`REQ-01M7QF01A00F9946DX0GPH23JQ` 切片 3）
//   四态：`开始` / `已复制` / `失败：<error>` / `INVALID（已复制）`；状态行是面板里的一行。
//   ★ 副作用在 render **开头**（`game.click` 自驱），`Button` 只画外观、不传 `on`。
//   ★ 红了 ⇒ 按钮点了没反应 / 状态行不显示 / 同一次点击被重复消费 / 失败被画成"已复制"。
// ---------------------------------------------------------------------------------------
void caseProfileStatus(lift::ui::ScriptRuntime& runtime, const std::string& source, const std::string& path,
                       lift::FontEngine* font) {
    using namespace lift_test;
    g_current_case = "ui-hud-script/profile-status";

    // ---- ① 第一击 ⇒ 开始；第二击（text 含 INVALID）⇒ INVALID（已复制）；闲置帧不再调用 ----
    {
        std::string err;
        std::unique_ptr<lift::ui::UiScript> script = runtime.load(source, path, font, &err);
        LIFT_CHECK(script != nullptr);
        if (!script) return;
        FakeHostOps fake;
        const lift::ui::UiHostOps ops = opsFor(&fake);

        lift::ui::UiBuilder b1;
        std::string rerr;
        LIFT_CHECK(script->render(b1, clickReadings(&ops, 1), &rerr));
        expectStatus(b1, "profile: 开始");
        LIFT_CHECK_EQ_I64(fake.begin_calls, 1);
        LIFT_CHECK_EQ_I64(fake.end_calls, 0);
        ++g_checks;
        if (fake.last_label != "overlay") {
            failf(__FILE__, __LINE__, "profileBegin 的 label 应是 `overlay`，得到 `%s`", fake.last_label.c_str());
        }

        fake.end_text = "=== profile window: overlay ===\nunaccounted ... INVALID";
        lift::ui::UiBuilder b2;
        LIFT_CHECK(script->render(b2, clickReadings(&ops, 2), &rerr));
        expectStatus(b2, "profile: INVALID（已复制）");
        LIFT_CHECK_EQ_I64(fake.begin_calls, 1);  // 两次击之间：begin / end 各只一次
        LIFT_CHECK_EQ_I64(fake.end_calls, 1);

        // 没有点击的帧：状态行留着（不按帧清），且不再调用宿主。
        lift::ui::UiFrameReadings idle;
        idle.frame = 3;
        idle.host = &ops;
        lift::ui::UiBuilder b3;
        LIFT_CHECK(script->render(b3, idle, &rerr));
        expectStatus(b3, "profile: INVALID（已复制）");
        LIFT_CHECK_EQ_I64(fake.begin_calls, 1);
        LIFT_CHECK_EQ_I64(fake.end_calls, 1);
        // 状态行参与面板尺寸：这一帧的 HUD 行 = frame + 状态行。
        const std::vector<std::string> hud = hudLinesOf(b3);
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(hud.size()), 2);
        expectLine(hud, 0, "frame 3");
        expectLine(hud, 1, "profile: INVALID（已复制）");
    }

    // ---- ② 结束成功且文本不含 INVALID ⇒ 已复制 ----
    {
        std::string err;
        std::unique_ptr<lift::ui::UiScript> script = runtime.load(source, path, font, &err);
        LIFT_CHECK(script != nullptr);
        if (!script) return;
        FakeHostOps fake;
        fake.end_text = "=== profile window: overlay ===\nverdict ...";
        const lift::ui::UiHostOps ops = opsFor(&fake);
        lift::ui::UiBuilder b1;
        std::string rerr;
        LIFT_CHECK(script->render(b1, clickReadings(&ops, 1), &rerr));
        lift::ui::UiBuilder b2;
        LIFT_CHECK(script->render(b2, clickReadings(&ops, 2), &rerr));
        expectStatus(b2, "profile: 已复制");
        LIFT_CHECK_EQ_I64(fake.begin_calls, 1);
        LIFT_CHECK_EQ_I64(fake.end_calls, 1);
    }

    // ---- ③ 结束失败（ok:false）⇒ 失败：<error>（⛔ 不画成"已复制"）----
    {
        std::string err;
        std::unique_ptr<lift::ui::UiScript> script = runtime.load(source, path, font, &err);
        LIFT_CHECK(script != nullptr);
        if (!script) return;
        FakeHostOps fake;
        fake.end_ok = false;
        fake.end_error = "没有打开的 profile 窗口";
        const lift::ui::UiHostOps ops = opsFor(&fake);
        lift::ui::UiBuilder b1;
        std::string rerr;
        LIFT_CHECK(script->render(b1, clickReadings(&ops, 1), &rerr));
        lift::ui::UiBuilder b2;
        LIFT_CHECK(script->render(b2, clickReadings(&ops, 2), &rerr));
        expectStatus(b2, "profile: 失败：没有打开的 profile 窗口");
    }

    // ---- ④ 复制失败（ok:true + error 非空）⇒ 失败：<error>（"结束 ≠ 复制成功"）----
    {
        std::string err;
        std::unique_ptr<lift::ui::UiScript> script = runtime.load(source, path, font, &err);
        LIFT_CHECK(script != nullptr);
        if (!script) return;
        FakeHostOps fake;
        fake.end_error = "SDL_SetClipboardText: boom";
        const lift::ui::UiHostOps ops = opsFor(&fake);
        lift::ui::UiBuilder b1;
        std::string rerr;
        LIFT_CHECK(script->render(b1, clickReadings(&ops, 1), &rerr));
        lift::ui::UiBuilder b2;
        LIFT_CHECK(script->render(b2, clickReadings(&ops, 2), &rerr));
        expectStatus(b2, "profile: 失败：SDL_SetClipboardText: boom");
    }

    // ---- ⑤ 开始被拒 ⇒ 失败：<error>（状态机不进入 running）----
    {
        std::string err;
        std::unique_ptr<lift::ui::UiScript> script = runtime.load(source, path, font, &err);
        LIFT_CHECK(script != nullptr);
        if (!script) return;
        FakeHostOps fake;
        fake.begin_ok = false;
        fake.begin_error = "已经有一段 profile 窗口开着（begin 不会 reset 上一段）";
        const lift::ui::UiHostOps ops = opsFor(&fake);
        lift::ui::UiBuilder b1;
        std::string rerr;
        LIFT_CHECK(script->render(b1, clickReadings(&ops, 1), &rerr));
        expectStatus(b1, "profile: 失败：已经有一段 profile 窗口开着（begin 不会 reset 上一段）");
        LIFT_CHECK_EQ_I64(fake.begin_calls, 1);
        LIFT_CHECK_EQ_I64(fake.end_calls, 0);
    }
}

}  // namespace

int main() {
    using namespace lift_test;

    g_current_case = "ui-hud-script/fake-readings";

    // ★ 字体是脚本 `ui.measure` 的来源 ⇒ 缺它这条用例判不了（与 ui-text 同一档）。
    lift::FontEngine font;
    if (!font.fontFileAvailable(false)) {
        LIFT_SKIP("缺宿主字体 Amayui-CN_cnjp.ttf（LIFT_HOST_FONT_PATH / corpus/assets/fonts）");
    }

    const std::string path = hudScriptPath();
    std::string source;
    if (!readAll(path, &source)) {
        LIFT_CHECK(false);
        std::fprintf(stderr, "读不到默认 HUD 脚本：%s\n", path.c_str());
        return finish("ui-hud-script");
    }
    LIFT_CHECK(!source.empty());

    auto runtime = lift::ui::makeQuickJsRuntime();
    LIFT_CHECK(runtime != nullptr);
    if (runtime == nullptr) return finish("ui-hud-script");

    std::string err;
    std::unique_ptr<lift::ui::UiScript> script = runtime->load(source, path, &font, &err);
    LIFT_CHECK(script != nullptr);
    if (script == nullptr) {
        std::fprintf(stderr, "加载 HUD 脚本失败：%s\n", err.c_str());
        return finish("ui-hud-script");
    }

    // ---- ① + ② + ③：满配读数 ⇒ 逐行文案 / 颜色 ----
    {
        lift::ui::UiFrameReadings r;
        r.frame = 1234;
        r.mouse_ok = true;
        r.mouse_x = 812;
        r.mouse_y = 622;
        r.fps_ok = true;
        r.fps = 59.9;
        r.script_ok = true;
        r.script = "startup";
        r.worst_ok = true;
        r.worst_wall_ms = 33.3;
        r.worst_guest_ms = 21.5;
        r.worst_guest_ok = true;
        r.worst_shim_ms = 4.2;

        lift::ui::UiBuilder builder;
        std::string rerr;
        LIFT_CHECK(script->render(builder, r, &rerr));
        if (!rerr.empty()) std::fprintf(stderr, "render: %s\n", rerr.c_str());

        const std::vector<std::string> texts = hudLinesOf(builder);
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(texts.size()), 5);
        expectLine(texts, 0, "mouse 812,622");
        expectLine(texts, 1, "script startup");
        expectLine(texts, 2, "fps 59.9");
        expectLine(texts, 3, "frame 1234");
        expectLine(texts, 4, "worst 3s  wall=33.3ms guest=21.5ms shim=4.2ms");

        // ② 没有 raster / present（任何一条 HUD 文本都不许含这两个词）。
        for (const std::string& t : texts) {
            ++g_checks;
            if (t.find("raster") != std::string::npos || t.find("present") != std::string::npos) {
                failf(__FILE__, __LINE__, "HUD 文本里出现了 raster / present：`%s`", t.c_str());
            }
        }

        // ③ 颜色精确值：面板是第一条 Rect（fill = BG）；HUD 行全是 FG + 16px；
        //    唯一的非 HUD Text 是按钮标签 PROFILE（kit 的 14px / idle fg）。
        LIFT_CHECK(!builder.elements().empty());
        int panel_h = 0;
        if (!builder.elements().empty()) {
            const lift::ui::Element& panel = builder.elements().front();
            LIFT_CHECK(panel.kind == lift::ui::ElementKind::Rect);
            LIFT_CHECK_EQ_U32(panel.fill.r, 16u);   // #101820
            LIFT_CHECK_EQ_U32(panel.fill.g, 24u);
            LIFT_CHECK_EQ_U32(panel.fill.b, 32u);
            LIFT_CHECK_EQ_U32(panel.fill.a, 204u);  // CC = 80% 不透明（2026-11-18 用户裁决）
            LIFT_CHECK(panel.w > 0 && panel.h > 0);
            panel_h = panel.h;
        }
        bool saw_profile_label = false;
        std::size_t profile_rect = builder.elements().size();
        for (std::size_t i = 0; i < builder.elements().size(); ++i) {
            const lift::ui::Element& e = builder.elements()[i];
            if (e.kind == lift::ui::ElementKind::Rect && e.key == "profile") profile_rect = i;
            if (e.kind != lift::ui::ElementKind::Text) continue;
            if (e.text == "PROFILE") {
                // ★ 按钮文本 = kit 的 `DEFAULT_THEME.font`(14) / `idle.fg`(#EBFFEB)；⛔ 不是 HUD 行。
                saw_profile_label = true;
                LIFT_CHECK_EQ_I64(e.font_size, 14);
                LIFT_CHECK_EQ_U32(e.color.r, 235u);
                LIFT_CHECK_EQ_U32(e.color.g, 255u);
                LIFT_CHECK_EQ_U32(e.color.b, 235u);
                LIFT_CHECK_EQ_U32(e.color.a, 255u);
                continue;
            }
            // 其余每个 Text 都必须是 HUD 行（16px + FG）—— ⛔ 不是"非 16px 就跳过"。
            LIFT_CHECK_EQ_U32(e.color.r, 235u);  // #EBFFEB
            LIFT_CHECK_EQ_U32(e.color.g, 255u);
            LIFT_CHECK_EQ_U32(e.color.b, 235u);
            LIFT_CHECK_EQ_U32(e.color.a, 255u);
            LIFT_CHECK_EQ_I64(e.font_size, 16);  // 参考字号 = SIZE
        }
        LIFT_CHECK(saw_profile_label);
        // 按钮结构：面板仍是 elements[0]，按钮 Rect 画在面板**之后**且 key = profile（可命中）。
        ++g_checks;
        if (profile_rect == builder.elements().size()) {
            failf(__FILE__, __LINE__, "HUD 里没有 key == 'profile' 的按钮 Rect（PROFILE 按钮不可命中）");
        } else {
            LIFT_CHECK(profile_rect > 0);  // 面板仍是第一个元素
            const lift::ui::Element& button = builder.elements()[profile_rect];
            LIFT_CHECK(button.w > 0 && button.h > 0);
            LIFT_CHECK(button.y >= panel_h);  // 面板下方
        }
    }

    // ---- ④ 缺省项整行省略 + `guest=--`（⛔ 不编 0）----
    {
        lift::ui::UiFrameReadings r;
        r.frame = 7;
        r.worst_ok = true;
        r.worst_wall_ms = 1.2;
        r.worst_guest_ms = 0.0;
        r.worst_guest_ok = false;  // 块计时没开 ⇒ 脚本侧**没有** guest_ms 这个键
        r.worst_shim_ms = 0.3;

        lift::ui::UiBuilder builder;
        std::string rerr;
        LIFT_CHECK(script->render(builder, r, &rerr));
        if (!rerr.empty()) std::fprintf(stderr, "render: %s\n", rerr.c_str());

        const std::vector<std::string> texts = hudLinesOf(builder);
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(texts.size()), 2);
        expectLine(texts, 0, "frame 7");
        expectLine(texts, 1, "worst 3s  wall=1.2ms guest=-- shim=0.3ms");
    }

    // ---- ⑤ 栈顶脚本名优先于 `game.script`（`local_frames()[0].script` 压过当前帧身份）----
    {
        g_current_case = "ui-hud-script/script-line-top-frame";
        FakeFrameSource frames;
        {
            lift::ui::UiFrameInfo top;
            top.slot = 2;
            top.script_id = 0;
            top.parent = 0xFFFFFFFFu;
            top.script = "SC0000";
            top.top = true;
            lift::ui::UiFrameInfo below;
            below.slot = 1;
            below.script_id = 9;
            below.parent = 2;
            below.script = "SC0009";
            below.top = false;
            frames.list = {top, below};
        }
        lift::ui::UiFrameReadings r;
        r.frame = 9;
        r.script_ok = true;
        r.script = "startup";   // 回退值：有栈顶时必须**不**用这一条
        r.frames = &frames;

        lift::ui::UiBuilder builder;
        std::string rerr;
        LIFT_CHECK(script->render(builder, r, &rerr));
        if (!rerr.empty()) std::fprintf(stderr, "render: %s\n", rerr.c_str());

        const std::vector<std::string> texts = hudLinesOf(builder);
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(texts.size()), 2);
        expectLine(texts, 0, "script SC0000");
        expectLine(texts, 1, "frame 9");

        // 帧源读不到 ⇒ 回退 `game.script`（既有语义不许被这一单改掉）。
        g_current_case = "ui-hud-script/script-line-fallback";
        FakeFrameSource broken;
        broken.err = "no-active-frame";
        r.frames = &broken;
        lift::ui::UiBuilder builder2;
        std::string rerr2;
        LIFT_CHECK(script->render(builder2, r, &rerr2));
        if (!rerr2.empty()) std::fprintf(stderr, "render: %s\n", rerr2.c_str());
        const std::vector<std::string> texts2 = hudLinesOf(builder2);
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(texts2.size()), 2);
        expectLine(texts2, 0, "script startup");
        expectLine(texts2, 1, "frame 9");
    }

    // ---- ⑥ `cur` 不活动（top 全 false）⇒ 回退 `game.script`，⛔ 不拿 `frames[0]` 冒充 ----
    {
        g_current_case = "ui-hud-script/script-line-no-top";
        FakeFrameSource frames;
        {
            lift::ui::UiFrameInfo first;  // 最高活动槽：top-first 的 [0]，但**不是**栈顶
            first.slot = 37;
            first.script_id = 21092;
            first.parent = 0xFFFFFFF6u;
            first.script = "SC0000";
            first.top = false;
            lift::ui::UiFrameInfo second;
            second.slot = 31;
            second.script_id = 21085;
            second.parent = 0;
            second.script = "SC0009";
            second.top = false;
            frames.list = {first, second};
        }
        lift::ui::UiFrameReadings r;
        r.frame = 10;
        r.script_ok = true;
        r.script = "startup";
        r.frames = &frames;

        lift::ui::UiBuilder builder;
        std::string rerr;
        LIFT_CHECK(script->render(builder, r, &rerr));
        if (!rerr.empty()) std::fprintf(stderr, "render: %s\n", rerr.c_str());

        const std::vector<std::string> texts = hudLinesOf(builder);
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(texts.size()), 2);
        expectLine(texts, 0, "script startup");  // ★ 不是 `script SC0000`（frames[0] 不是栈顶）
        expectLine(texts, 1, "frame 10");
    }

    // ---- ⑦ / ⑧ PROFILE 按钮 + 状态机（切片 3；每次用一份**新的**脚本实例 = 干净的模块状态）----
    caseProfileStatus(*runtime, source, path, &font);

    script->shutdown();
    return finish("ui-hud-script");
}
