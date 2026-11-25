// emulator-lift · 默认 HUD 脚本（`scripts/ui/hud.js`）的**内容判据**（B）
//
// ★ S3：`--ui` CLI 入口已删；这条用例驱动的是**内部 hud 回退**用的同一个 UiScript loader
//   （`--overlay` + 无 render 的 `--plugin` 走 `SdlWindow::init` ⇒ `loadUiScript`）。CTest 名
//   与用例 id 保持不变（`data/requirements` 的 `verify:` 锚着 `script-line-top-frame`）。
//
// 判据（红了意味着什么，缺一条都不算）：
//   ① **行序 / 文案（D4，2026-11-25）**：给一份 fake 读数（frame=1234 / mouse(812,622) / fps=59.9 /
//      script "startup" / worst wall=33.3 guest=21.5 shim=4.2）⇒ display list 里的 **Text 元素逐条**等于
//        `mouse 812,622` / `script startup` / `fps 59.9` / `worst 3s  wall=33.3ms guest=21.5ms shim=4.2ms` /
//        `settings…`
//      顺序就是**用户看到的顺序**；**没有 `frame <n>` 行**（总帧数无诊断价值，D4 删除）。
//      红了 ⇒ 默认 HUD 的文案 / 顺序变了（或脚本没被执行、`frame` 行又被加回来）。
//   ② **没有 raster / present 两行**（用户 2026-11-18 裁决：开 overlay 时人是在体验，不需要这两个
//      开关信息）。红了 ⇒ 旧的两行又被加回来了。
//   ③ **版式（D4）**：面板仍是 **elements[0]**，`x == 0`（贴左）、`2*y + panelH ∈ {720,721}`
//      （= `round((720-panelH)/2)` 的等价整数判据，1280×720 参考系里垂直居中）；`settings…` 是**最后
//      一行**，它上面叠一条 `fill:"#00000000"` / `key:"settings"` 的透明命中 Rect，**刚好盖住那一行**
//      （x/w 与面板一致、y == 该行文本 y、h == 行高）。红了 ⇒ 面板又回左上角 / 设置入口不可点 / 命中区
//      与文本错位（"看得到点不到"）。
//   ④ **PROFILE 默认隐藏**：默认读数（没有按键源）下，**没有任何** `key == "profile"` 的 Rect，也没有
//      `PROFILE` 文本。红了 ⇒ 默认又挂了一个不可用/会误点的按钮。
//   ⑤ **F9 切显隐（读数是计数、不是电平）**：注入 `overlayKeySeq` 0→1 ⇒ 出现 `key=="profile"` 的按钮
//      （画在面板**下方**：`y >= panel.y + panel.h`）；**同一个值重复**（1→1）**不**再切换；1→2 ⇒ 又消失。
//      红了 ⇒ PROFILE 要么切不过来，要么每帧都切（当电平用）。
//   ⑥ **设置入口（D4）**：点 `settings` 且 `h.config` 可用 ⇒ **该帧只有页面元素**（背板 `settings.bg`
//      是 elements[0]、面板/`settings…` 行都不画），`BACK`（`settings.back`）后回到 HUD；
//      `h.config` **缺失**（`undefined`）⇒ **仍画 HUD** + 状态行 `settings: 设置不可用`；
//      桥在但 `available !== true` ⇒ `settings: 没有 appdata`（两种原因分开写）；
//      config 在同一脚本实例里改回可用 ⇒ 那句状态**清掉**（不点也对）。
//   ⑦ **PROFILE 按钮（切片 3）**：可见时标签 `PROFILE` 是 14px / kit `idle.fg`；★ "HUD 行"的判据
//      收窄成 `Text && font_size==16 && color==FG` —— ⛔ 不 blanket 跳过所有非 16px 文本：
//      除按钮标签外，其余每个 Text 仍必须是 16px + FG。
//   ⑧ **状态机（`game.click` 自驱）**：四态 `开始` / `已复制` / `失败：<error>` / `INVALID（已复制）`
//      ／ 状态行是面板里的一行（在 `settings…` 之前）；两次击之间 `begin`/`end` 各**只**调一次；
//      闲置帧不再调宿主、但状态行留着（不按帧清）。红了 ⇒ 按钮点了没反应 / 失败被画成"已复制" /
//      点击被重复消费。
//   ⑨ **设置状态行的留存**（2026-11-25 阻断 2）：`set` 回执 `applied:false`（只读 appdata）⇒
//      `settings: 只读` 在**下一帧**（无点击）仍在；`BACK` 回 HUD 后仍在；成功回执后再点一次 ⇒ 清空。
//      红了 ⇒ 状态行只活 1 帧（≈16ms，人看不见）：只读 appdata 下点开关"零反馈"。
//   ⑩ **空 schema ⇒ 不打开**（顺带修）：桥可用但 `settings.schema` 没有 key ⇒ 点 `settings…` 仍是
//      HUD + 状态行 `settings: 没有注册设置`（⛔ 不打开空白页 / 不造死控件）；注册表回来才清。
// ★ 脚本是**运行时读的外置文件**（`LIFT_UI_SCRIPT_DIR "/hud.js"`，与内部 hud 回退同一个路径）：
//   改脚本 ⇒ 这条用例立刻看见。缺宿主字体（`ui.measure` 的来源）⇒ `LIFT_SKIP(77)`（⛔ 不假绿）。
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "check.h"
#include "runtime/config.h"
#include "runtime/font.h"
#include "runtime/vfs.h"  // openHostFileUtf8（Windows 的 UTF-8 路径）
#include "ui/element.h"
#include "ui/script.h"

namespace {

using lift::ui::Element;
using lift::ui::ElementKind;
using lift::ui::UiBuilder;
using lift::ui::UiFrameReadings;
using lift::ui::UiScript;

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
//   ⛔ 不 blanket 跳过所有非 16px 文本 —— 除了按钮文本，其余 Text 仍必须是 16px + FG（见 ⑦）。
bool isHudLine(const Element& e) {
    return e.kind == ElementKind::Text && e.font_size == 16 && e.color.r == 235 && e.color.g == 255 &&
           e.color.b == 235 && e.color.a == 255;
}

std::vector<std::string> hudLinesOf(const UiBuilder& builder) {
    std::vector<std::string> out;
    for (const Element& e : builder.elements()) {
        if (isHudLine(e)) out.push_back(e.text);
    }
    return out;
}

const Element* elementByKey(const UiBuilder& builder, const std::string& key) {
    for (const Element& e : builder.elements()) {
        if (e.kind == ElementKind::Rect && e.key == key) return &e;
    }
    return nullptr;
}

const Element* textElement(const UiBuilder& builder, const std::string& text) {
    for (const Element& e : builder.elements()) {
        if (e.kind == ElementKind::Text && e.text == text) return &e;
    }
    return nullptr;
}

bool hasTextContaining(const UiBuilder& builder, const std::string& needle) {
    for (const Element& e : builder.elements()) {
        if (e.kind == ElementKind::Text && e.text.find(needle) != std::string::npos) return true;
    }
    return false;
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

void expectNoText(const UiBuilder& builder, const std::string& text, const char* what) {
    ++lift_test::g_checks;
    if (textElement(builder, text) != nullptr) lift_test::failf(__FILE__, __LINE__, "%s：不许出现文本 `%s`", what, text.c_str());
}

void expectNoKey(const UiBuilder& builder, const std::string& key, const char* what) {
    ++lift_test::g_checks;
    if (elementByKey(builder, key) != nullptr) lift_test::failf(__FILE__, __LINE__, "%s：不许出现命中区 `%s`", what, key.c_str());
}

void expectRect(const UiBuilder& builder, const std::string& key, const char* what) {
    ++lift_test::g_checks;
    if (elementByKey(builder, key) == nullptr) {
        lift_test::failf(__FILE__, __LINE__, "%s：缺少命中区 `%s`", what, key.c_str());
    }
}

// `profile-status` 用例：状态行是面板里的一行（HUD 行），以 `profile: ` 开头。
std::string statusLineOf(const UiBuilder& builder) {
    for (const Element& e : builder.elements()) {
        if (e.kind == ElementKind::Text && e.text.rfind("profile: ", 0) == 0) return e.text;
    }
    return {};
}

void expectStatus(const UiBuilder& builder, const char* want) {
    ++lift_test::g_checks;
    const std::string got = statusLineOf(builder);
    if (got != want) lift_test::failf(__FILE__, __LINE__, "状态行：期望 `%s`，得到 `%s`", want, got.c_str());
}

bool renderWith(UiScript& script, const UiFrameReadings& readings, UiBuilder* out, const char* what) {
    std::string err;
    ++lift_test::g_checks;
    if (!script.render(*out, readings, &err)) {
        lift_test::failf(__FILE__, __LINE__, "%s：render 失败：%s", what, err.c_str());
        return false;
    }
    return true;
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

// ---------------------------------------------------------------------------------------
// 假 config 桥（D2/D4）：只回答 `available` + `settings.schema` 一个域。
//   `available = false` ⇒ 脚本侧 `host.config` 仍是对象但 `available !== true` ⇒ `canOpen` false
//   （与"桥整个没接"是两条来路，这里**都**要能造出来：没接 = `ops.config = nullptr`）。
//   ★ `schema_empty`：桥可用但 `settings.schema` 域**一个 key 都没有**（顺带修：空白页）。
//   ★ `set_applied` / `set_code`：`set` 的回执（`applied:false, code:"read_only"` = 只读 appdata）。
// ---------------------------------------------------------------------------------------
struct FakeConfig final : lift::plugin::ConfigBridge {
    bool is_available = true;
    bool schema_empty = false;
    bool set_applied = true;
    const char* set_code = nullptr;

    lift::plugin::ConfigValue schema() const {
        lift::plugin::ConfigValue root = lift::plugin::ConfigValue::object();
        if (schema_empty) return root;  // 空对象 = `settings.schema` 域没有 key
        lift::plugin::ConfigValue entries = lift::plugin::ConfigValue::object();
        lift::plugin::ConfigValue flag = lift::plugin::ConfigValue::object();
        flag.put("type", lift::plugin::ConfigValue::fromString("bool"));
        flag.put("label", lift::plugin::ConfigValue::fromString("flag label"));
        flag.put("default", lift::plugin::ConfigValue::fromBool(false));
        entries.put("flag", std::move(flag));
        lift::plugin::ConfigValue demo = lift::plugin::ConfigValue::object();
        demo.put("title", lift::plugin::ConfigValue::fromString("Demo"));
        demo.put("live", lift::plugin::ConfigValue::fromString("run"));
        demo.put("entries", std::move(entries));
        root.put("demo", std::move(demo));
        return root;
    }

    bool available() const override { return is_available; }
    bool checkDomain(const std::string&, std::string*) const override { return true; }
    bool get(const std::string&, const std::string&, lift::plugin::ConfigValue*, bool* found,
             std::string*) override {
        if (found != nullptr) *found = false;
        return true;
    }
    bool has(const std::string&, const std::string&, bool* found, std::string*) override {
        if (found != nullptr) *found = false;
        return true;
    }
    bool set(const std::string&, const std::string&, const lift::plugin::ConfigValue&,
             lift::plugin::ConfigReceipt* receipt, std::string*) override {
        if (receipt != nullptr) {
            receipt->applied = set_applied;
            receipt->code = set_code;
        }
        return true;
    }
    bool erase(const std::string&, const std::string&, bool* removed, std::string*) override {
        if (removed != nullptr) *removed = false;
        return true;
    }
    lift::plugin::ConfigValue all(const std::string& domain) override {
        if (domain == "settings.schema") return schema();
        return lift::plugin::ConfigValue::object();
    }
    void flushIfDirty(std::string*) override {}
};

// 一次带 click 的 render（`profile` 按钮；F9 已把它打开）。
lift::ui::UiFrameReadings clickReadings(const lift::ui::UiHostOps* ops, std::uint64_t frame,
                                        std::uint64_t key_seq) {
    UiFrameReadings r;
    r.frame = frame;
    r.host = ops;
    r.overlay_key_ok = true;
    r.overlay_key_seq = key_seq;
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
//   ★ D4：按钮默认隐藏 ⇒ 先喂两帧 `overlayKeySeq` 0→1 把它打开（计数变了才切；`1→1` 不再切）。
//   ★ 红了 ⇒ 按钮点了没反应 / 状态行不显示 / 同一次点击被重复消费 / 失败被画成"已复制"。
// ---------------------------------------------------------------------------------------
void primeProfile(UiScript& script, const lift::ui::UiHostOps* ops) {
    using namespace lift_test;
    UiFrameReadings base;
    base.frame = 1;
    base.host = ops;
    base.overlay_key_ok = true;
    base.overlay_key_seq = 0;
    UiBuilder b0;
    renderWith(script, base, &b0, "prime(F9 基线)");
    // 基线帧：PROFILE 必须还不在场（不是"第一帧就显示"）。
    expectNoKey(b0, "profile", "F9 基线帧");
    base.frame = 2;
    base.overlay_key_seq = 1;  // 0→1 ⇒ 打开
    UiBuilder b1;
    renderWith(script, base, &b1, "prime(F9 打开)");
    ++g_checks;
    if (elementByKey(b1, "profile") == nullptr) {
        failf(__FILE__, __LINE__, "overlayKeySeq 0→1 后应出现 key == 'profile' 的按钮");
    }
}

void caseProfileStatus(lift::ui::ScriptRuntime& runtime, const std::string& source, const std::string& path,
                       lift::FontEngine* font) {
    using namespace lift_test;
    g_current_case = "ui-hud-script/profile-status";

    // ---- ① 第一击 ⇒ 开始；第二击（text 含 INVALID）⇒ INVALID（已复制）；闲置帧不再调用 ----
    {
        std::string err;
        std::unique_ptr<UiScript> script = runtime.load(source, path, font, &err);
        LIFT_CHECK(script != nullptr);
        if (!script) return;
        FakeHostOps fake;
        const lift::ui::UiHostOps ops = opsFor(&fake);
        primeProfile(*script, &ops);

        UiBuilder b1;
        std::string rerr;
        LIFT_CHECK(script->render(b1, clickReadings(&ops, 3, 1), &rerr));
        expectStatus(b1, "profile: 开始");
        LIFT_CHECK_EQ_I64(fake.begin_calls, 1);
        LIFT_CHECK_EQ_I64(fake.end_calls, 0);
        ++g_checks;
        if (fake.last_label != "overlay") {
            failf(__FILE__, __LINE__, "profileBegin 的 label 应是 `overlay`，得到 `%s`", fake.last_label.c_str());
        }

        fake.end_text = "=== profile window: overlay ===\nunaccounted ... INVALID";
        UiBuilder b2;
        LIFT_CHECK(script->render(b2, clickReadings(&ops, 4, 1), &rerr));
        expectStatus(b2, "profile: INVALID（已复制）");
        LIFT_CHECK_EQ_I64(fake.begin_calls, 1);  // 两次击之间：begin / end 各只一次
        LIFT_CHECK_EQ_I64(fake.end_calls, 1);

        // 没有点击的帧：状态行留着（不按帧清），且不再调用宿主。
        UiFrameReadings idle;
        idle.frame = 5;
        idle.host = &ops;
        idle.overlay_key_ok = true;
        idle.overlay_key_seq = 1;  // 同一个值 ⇒ 不切换显隐
        UiBuilder b3;
        LIFT_CHECK(script->render(b3, idle, &rerr));
        expectStatus(b3, "profile: INVALID（已复制）");
        LIFT_CHECK_EQ_I64(fake.begin_calls, 1);
        LIFT_CHECK_EQ_I64(fake.end_calls, 1);
        // 状态行参与面板尺寸：这一帧的 HUD 行 = 状态行 + `settings…`（**没有** `frame` 行）。
        const std::vector<std::string> hud = hudLinesOf(b3);
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(hud.size()), 2);
        expectLine(hud, 0, "profile: INVALID（已复制）");
        expectLine(hud, 1, "settings…");
        // 按钮此刻仍在场（F9 没变）⇒ 状态行与设置行之外还有 14px 的 PROFILE 标签。
        expectNoText(b3, "frame 5", "D4 删掉 frame 行");
    }

    // ---- ② 结束成功且文本不含 INVALID ⇒ 已复制 ----
    {
        std::string err;
        std::unique_ptr<UiScript> script = runtime.load(source, path, font, &err);
        LIFT_CHECK(script != nullptr);
        if (!script) return;
        FakeHostOps fake;
        fake.end_text = "=== profile window: overlay ===\nverdict ...";
        const lift::ui::UiHostOps ops = opsFor(&fake);
        primeProfile(*script, &ops);
        lift::ui::UiBuilder b1;
        std::string rerr;
        LIFT_CHECK(script->render(b1, clickReadings(&ops, 3, 1), &rerr));
        lift::ui::UiBuilder b2;
        LIFT_CHECK(script->render(b2, clickReadings(&ops, 4, 1), &rerr));
        expectStatus(b2, "profile: 已复制");
        LIFT_CHECK_EQ_I64(fake.begin_calls, 1);
        LIFT_CHECK_EQ_I64(fake.end_calls, 1);
    }

    // ---- ③ 结束失败（ok:false）⇒ 失败：<error>（⛔ 不画成"已复制"）----
    {
        std::string err;
        std::unique_ptr<UiScript> script = runtime.load(source, path, font, &err);
        LIFT_CHECK(script != nullptr);
        if (!script) return;
        FakeHostOps fake;
        fake.end_ok = false;
        fake.end_error = "没有打开的 profile 窗口";
        const lift::ui::UiHostOps ops = opsFor(&fake);
        primeProfile(*script, &ops);
        lift::ui::UiBuilder b1;
        std::string rerr;
        LIFT_CHECK(script->render(b1, clickReadings(&ops, 3, 1), &rerr));
        lift::ui::UiBuilder b2;
        LIFT_CHECK(script->render(b2, clickReadings(&ops, 4, 1), &rerr));
        expectStatus(b2, "profile: 失败：没有打开的 profile 窗口");
    }

    // ---- ④ 复制失败（ok:true + error 非空）⇒ 失败：<error>（"结束 ≠ 复制成功"）----
    {
        std::string err;
        std::unique_ptr<UiScript> script = runtime.load(source, path, font, &err);
        LIFT_CHECK(script != nullptr);
        if (!script) return;
        FakeHostOps fake;
        fake.end_error = "SDL_SetClipboardText: boom";
        const lift::ui::UiHostOps ops = opsFor(&fake);
        primeProfile(*script, &ops);
        lift::ui::UiBuilder b1;
        std::string rerr;
        LIFT_CHECK(script->render(b1, clickReadings(&ops, 3, 1), &rerr));
        lift::ui::UiBuilder b2;
        LIFT_CHECK(script->render(b2, clickReadings(&ops, 4, 1), &rerr));
        expectStatus(b2, "profile: 失败：SDL_SetClipboardText: boom");
    }

    // ---- ⑤ 开始被拒 ⇒ 失败：<error>（状态机不进入 running）----
    {
        std::string err;
        std::unique_ptr<UiScript> script = runtime.load(source, path, font, &err);
        LIFT_CHECK(script != nullptr);
        if (!script) return;
        FakeHostOps fake;
        fake.begin_ok = false;
        fake.begin_error = "已经有一段 profile 窗口开着（begin 不会 reset 上一段）";
        const lift::ui::UiHostOps ops = opsFor(&fake);
        primeProfile(*script, &ops);
        lift::ui::UiBuilder b1;
        std::string rerr;
        LIFT_CHECK(script->render(b1, clickReadings(&ops, 3, 1), &rerr));
        expectStatus(b1, "profile: 失败：已经有一段 profile 窗口开着（begin 不会 reset 上一段）");
        LIFT_CHECK_EQ_I64(fake.begin_calls, 1);
        LIFT_CHECK_EQ_I64(fake.end_calls, 0);
    }
}

// ---------------------------------------------------------------------------------------
// ⑤ F9 的显隐切换（读数是**计数**，⛔ 电平）：默认隐藏；0→1 出现；1→1 不动；1→2 消失。
// ---------------------------------------------------------------------------------------
void caseProfileToggle(lift::ui::ScriptRuntime& runtime, const std::string& source, const std::string& path,
                       lift::FontEngine* font) {
    using namespace lift_test;
    g_current_case = "ui-hud-script/profile-toggle";
    std::string err;
    std::unique_ptr<UiScript> script = runtime.load(source, path, font, &err);
    LIFT_CHECK(script != nullptr);
    if (!script) return;

    auto renderSeq = [&](std::uint64_t frame, bool has_seq, std::uint64_t seq, UiBuilder* out) {
        UiFrameReadings r;
        r.frame = frame;
        r.overlay_key_ok = has_seq;
        r.overlay_key_seq = seq;
        return renderWith(*script, r, out, "profile-toggle");
    };

    // 没有按键源（headless / 测试 sink）⇒ 默认不画按钮、也不挂 key。
    UiBuilder no_source;
    LIFT_CHECK(renderSeq(1, false, 0, &no_source));
    expectNoKey(no_source, "profile", "无按键源");
    expectNoText(no_source, "PROFILE", "无按键源");
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(hudLinesOf(no_source).size()), 1);  // 只有 settings…

    // 第一次拿到值（0）= 只建立基线，不切换。
    UiBuilder baseline;
    LIFT_CHECK(renderSeq(2, true, 0, &baseline));
    expectNoKey(baseline, "profile", "基线帧（seq=0）");

    // 同一个值重复 ⇒ 仍是隐藏（⛔ 当电平用）。
    UiBuilder same_zero;
    LIFT_CHECK(renderSeq(3, true, 0, &same_zero));
    expectNoKey(same_zero, "profile", "同值（0→0）不切换");

    // 0→1 ⇒ 显示；按钮画在面板**下方**。
    UiBuilder shown;
    LIFT_CHECK(renderSeq(4, true, 1, &shown));
    const Element* button = elementByKey(shown, "profile");
    ++g_checks;
    if (button == nullptr) {
        failf(__FILE__, __LINE__, "overlayKeySeq 0→1 后应出现 key == 'profile' 的按钮");
    } else {
        LIFT_CHECK(!shown.elements().empty());
        const Element& panel = shown.elements().front();
        LIFT_CHECK(panel.kind == ElementKind::Rect);
        LIFT_CHECK(button->y >= panel.y + panel.h);  // 面板下方
        LIFT_CHECK(button->w > 0 && button->h > 0);
        const Element* label = textElement(shown, "PROFILE");
        ++g_checks;
        if (label == nullptr) {
            failf(__FILE__, __LINE__, "PROFILE 按钮没有标签文本");
        } else {
            LIFT_CHECK_EQ_I64(label->font_size, 14);  // kit DEFAULT_THEME.font
            LIFT_CHECK_EQ_U32(label->color.r, 235u);
            LIFT_CHECK_EQ_U32(label->color.g, 255u);
            LIFT_CHECK_EQ_U32(label->color.b, 235u);
        }
    }

    // 同值（1→1）⇒ 仍显示（不闪）。
    UiBuilder same_one;
    LIFT_CHECK(renderSeq(5, true, 1, &same_one));
    expectRect(same_one, "profile", "同值（1→1）不切换");

    // 1→2 ⇒ 又消失（key 也没了 ⇒ 不可命中）。
    UiBuilder hidden;
    LIFT_CHECK(renderSeq(6, true, 2, &hidden));
    expectNoKey(hidden, "profile", "1→2 应隐藏");
    expectNoText(hidden, "PROFILE", "1→2 应隐藏标签");
}

// ---------------------------------------------------------------------------------------
// ⑥ 设置入口（D4）：最后一行 `settings…` + 透明命中 Rect；点击开二级页；BACK 回 HUD；
//   `h.config` 缺失 ⇒ 不打开 + `settings: 设置不可用`；可用但 `available=false` ⇒ `settings: 没有 appdata`
//   （同一个脚本实例改回来才清）。
// ---------------------------------------------------------------------------------------
void caseSettingsEntry(lift::ui::ScriptRuntime& runtime, const std::string& source, const std::string& path,
                       lift::FontEngine* font) {
    using namespace lift_test;
    g_current_case = "ui-hud-script/settings-entry";

    std::string err;
    std::unique_ptr<UiScript> script = runtime.load(source, path, font, &err);
    LIFT_CHECK(script != nullptr);
    if (!script) return;

    auto renderClick = [&](std::uint64_t frame, const char* key, UiBuilder* out) {
        UiFrameReadings r;
        r.frame = frame;
        r.click_ok = true;
        r.click_key = key;
        r.click_x = 4;
        r.click_y = 4;
        return renderWith(*script, r, out, "settings-entry");
    };

    // ① `h.config` 整个没接（`ops.config = nullptr` ⇒ 脚本侧 `undefined`）⇒ 不打开，留一句状态。
    {
        UiBuilder b;
        LIFT_CHECK(renderClick(1, "settings", &b));
        LIFT_CHECK(!b.elements().empty());
        const Element& panel = b.elements().front();
        LIFT_CHECK(panel.kind == ElementKind::Rect);
        LIFT_CHECK(panel.key.empty());                 // 仍是 HUD 面板（不是页面背板）
        LIFT_CHECK_EQ_U32(panel.fill.a, 204u);         // #101820CC
        expectNoKey(b, "settings.bg", "没有 config 时不画设置页");
        const std::vector<std::string> hud = hudLinesOf(b);
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(hud.size()), 2);
        expectLine(hud, 0, "settings: 设置不可用");
        expectLine(hud, 1, "settings…");
        // 设置入口仍是最后一行、仍可命中。
        expectRect(b, "settings", "设置入口的透明命中 Rect");
        expectNoText(b, "frame 1", "D4 删掉 frame 行");
    }

    // ② 同一个脚本实例里 config 变成可用（不点）⇒ 那句状态清掉。
    FakeConfig config;
    {
        lift::ui::UiHostOps ops{};  // 这一帧把桥接上：同一实例的模块状态必须跨帧保留。
        ops.config = &config;
        UiFrameReadings r;
        r.frame = 2;
        r.host = &ops;
        UiBuilder b;
        LIFT_CHECK(renderWith(*script, r, &b, "settings-status-clear"));
        const std::vector<std::string> hud = hudLinesOf(b);
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(hud.size()), 1);
        expectLine(hud, 0, "settings…");
        expectNoText(b, "settings: 设置不可用", "config 改回可用 ⇒ 状态清掉");
    }

    // ③ 点 settings 且 config 可用 ⇒ **该帧只有页面元素**（背板 = elements[0]），HUD 行不画。
    {
        lift::ui::UiHostOps ops{};
        ops.config = &config;
        UiFrameReadings r;
        r.frame = 3;
        r.host = &ops;
        r.click_ok = true;
        r.click_key = "settings";
        r.click_x = 4;
        r.click_y = 4;
        UiBuilder b;
        LIFT_CHECK(renderWith(*script, r, &b, "settings-open"));
        LIFT_CHECK(!b.elements().empty());
        const Element& backdrop = b.elements().front();
        ++g_checks;
        if (backdrop.kind != ElementKind::Rect || backdrop.key != "settings.bg") {
            failf(__FILE__, __LINE__, "打开设置页那一帧 elements[0] 应是 key=settings.bg 的背板");
        } else {
            LIFT_CHECK_EQ_I64(backdrop.w, 1280);
            LIFT_CHECK_EQ_I64(backdrop.h, 720);
        }
        expectNoText(b, "settings…", "页面打开期间不画 settings… 行");
        expectNoKey(b, "settings", "页面打开期间不挂设置入口的 key");
        expectNoKey(b, "profile", "页面打开期间不画 PROFILE");
        expectRect(b, "settings.back", "页面的 BACK");
        expectNoText(b, "frame 3", "D4 删掉 frame 行（页面档同样没有）");
    }

    // ④ 点 BACK ⇒ 回到 HUD（同一帧就画 HUD）；页面元素不再出现。
    {
        lift::ui::UiHostOps ops{};
        ops.config = &config;
        UiFrameReadings r;
        r.frame = 4;
        r.host = &ops;
        r.click_ok = true;
        r.click_key = "settings.back";
        r.click_x = 4;
        r.click_y = 4;
        UiBuilder b;
        LIFT_CHECK(renderWith(*script, r, &b, "settings-back"));
        LIFT_CHECK(!b.elements().empty());
        const Element& panel = b.elements().front();
        LIFT_CHECK(panel.kind == ElementKind::Rect);
        LIFT_CHECK(panel.key.empty());  // HUD 面板回来了
        expectNoKey(b, "settings.bg", "BACK 后不再画设置页");
        const std::vector<std::string> hud = hudLinesOf(b);
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(hud.size()), 1);
        expectLine(hud, 0, "settings…");
        expectRect(b, "settings", "回到 HUD 后设置入口仍可命中");
    }

    // ⑤ 桥在打开期间断掉（available=false）⇒ 下一帧回到 HUD + 那句原因。
    FakeConfig unavailable;
    unavailable.is_available = false;
    {
        lift::ui::UiHostOps ops{};
        ops.config = &unavailable;
        UiFrameReadings r;
        r.frame = 5;
        r.host = &ops;
        r.click_ok = true;
        r.click_key = "settings";
        r.click_x = 4;
        r.click_y = 4;
        UiBuilder b;
        LIFT_CHECK(renderWith(*script, r, &b, "settings-available-false"));
        LIFT_CHECK(!b.elements().empty());
        LIFT_CHECK(b.elements().front().key.empty());  // HUD（`available !== true` ⇒ 不打开）
        const std::vector<std::string> hud = hudLinesOf(b);
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(hud.size()), 2);
        expectLine(hud, 0, "settings: 没有 appdata");
        expectLine(hud, 1, "settings…");
    }
}

// ---------------------------------------------------------------------------------------
// ⑨ 设置状态行的**留存**（D4；2026-11-25 阻断 2）
//   ★ 代表这一类错：`hud.js` 每帧 `const page = { status: "" }` 新建 ⇒ `settings-page.js` 写回的
//     `status`（`settings: 只读` / `settings: <错误>`）只在自己那一帧可见（≈16ms，人看不见）——
//     只读 appdata（`plugin_config.cpp` 的 `read_only` 可达）下点开关就是"零反馈"。
//   判据：
//     ① `set` 回执 `applied:false / read_only` ⇒ 点击那一帧页面里出现 `settings: 只读`；
//     ② **下一帧**（同一脚本实例、再 render、click 已消失）那句**仍在**（本条的承重断言）；
//     ③ 点 `BACK` 回 HUD ⇒ 那句**仍在**（关闭页面不清，状态行留在 HUD 上）；
//     ④ 重开页面 ⇒ 仍在；回执改成功（`applied:true`）后再点一次 ⇒ **清空**（口径 = 成功**无**
//        状态行，⛔ 不改写成成功文案；与 `drawPage` 的 `status === "" ? {} : {status}` 一致）。
// ---------------------------------------------------------------------------------------
void caseSettingsStatusRetention(lift::ui::ScriptRuntime& runtime, const std::string& source,
                                 const std::string& path, lift::FontEngine* font) {
    using namespace lift_test;
    g_current_case = "ui-hud-script/settings-status-retention";

    FakeConfig failing;  // 只读 appdata：`set` 内存回执 applied:false / read_only
    failing.set_applied = false;
    failing.set_code = "read_only";
    lift::ui::UiHostOps ops{};
    ops.config = &failing;

    std::string err;
    std::unique_ptr<UiScript> script = runtime.load(source, path, font, &err);
    LIFT_CHECK(script != nullptr);
    if (!script) return;

    auto render = [&](std::uint64_t frame, const char* key, UiBuilder* out) {
        UiFrameReadings r;
        r.frame = frame;
        r.host = &ops;
        if (key != nullptr) {
            r.click_ok = true;
            r.click_key = key;
            r.click_x = 4;
            r.click_y = 4;
        }
        return renderWith(*script, r, out, "settings-status-retention");
    };

    // ① 打开页面（点最后一行的透明命中 Rect）。
    {
        UiBuilder b;
        LIFT_CHECK(render(1, "settings", &b));
        expectRect(b, "settings.back", "点 settings… 应打开设置页");
    }
    // ② 点 bool 行 ⇒ 页面里出现失败回执 `settings: 只读`。
    {
        UiBuilder b;
        LIFT_CHECK(render(2, "set:demo:flag", &b));
        ++g_checks;
        if (!hasTextContaining(b, "settings: 只读")) {
            failf(__FILE__, __LINE__, "applied:false/read_only 点击后页面应显示 `settings: 只读`");
        }
        expectRect(b, "settings.back", "仍停在设置页");
    }
    // ③ ★ 阻断 2 的判据：下一帧（同一实例、click 已消失）状态行**仍在**。
    {
        UiBuilder b;
        LIFT_CHECK(render(3, nullptr, &b));
        ++g_checks;
        if (!hasTextContaining(b, "settings: 只读")) {
            failf(__FILE__, __LINE__,
                  "设置状态行只活 1 帧：下一帧（无点击）看不到 `settings: 只读`（失败回执必须留到下一次操作）");
        }
    }
    // ④ 点 BACK ⇒ 回到 HUD，那句**仍在**（关闭页面不清）。
    {
        UiBuilder b;
        LIFT_CHECK(render(4, "settings.back", &b));
        expectNoKey(b, "settings.bg", "BACK 后不再画设置页");
        const std::vector<std::string> hud = hudLinesOf(b);
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(hud.size()), 2);
        expectLine(hud, 0, "settings: 只读");
        expectLine(hud, 1, "settings…");
    }
    // ⑤ 重开页面 ⇒ 那句还在页面里（打开**不**清状态）。
    {
        UiBuilder b;
        LIFT_CHECK(render(5, "settings", &b));
        ++g_checks;
        if (!hasTextContaining(b, "settings: 只读")) {
            failf(__FILE__, __LINE__, "重开页面后失败回执应仍在（⛔ 打开不清状态）");
        }
    }
    // ⑥ 回执改成功后再点一次 ⇒ 状态行清空（成功 = 无状态行）。
    failing.set_applied = true;
    failing.set_code = nullptr;
    {
        UiBuilder b;
        LIFT_CHECK(render(6, "set:demo:flag", &b));
        expectNoText(b, "settings: 只读", "成功回执清掉上一次的失败回执");
        ++g_checks;
        if (hasTextContaining(b, "settings: ")) {
            failf(__FILE__, __LINE__, "成功回执后不该再有状态行（⛔ 也不改写成成功文案）");
        }
        expectRect(b, "settings.back", "仍在设置页");
    }
    // ⑦ 下一帧（无点击）也没有状态行 —— 清空是**状态**不是一帧的巧合。
    {
        UiBuilder b;
        LIFT_CHECK(render(7, nullptr, &b));
        ++g_checks;
        if (hasTextContaining(b, "settings: ")) {
            failf(__FILE__, __LINE__, "成功之后状态行应为空（拿到含 `settings: ` 的文本）");
        }
    }
}

// ---------------------------------------------------------------------------------------
// ⑩ 空 schema ⇒ **不打开**（顺带修：桥可用但 `settings.schema` 一个 key 都没有）
//   ★ 代表这一类错：`canOpen` 只看 `available` ⇒ 点 `settings…` 打开一张**空白页**
//     （没有域 / 没有项，只有 SETTINGS + live + BACK）—— 用户改了没有、什么都改不了。
//   判据：
//     ① 点 `settings…` ⇒ HUD 照画（elements[0] 仍是面板）、没有 `settings.bg`、
//        状态行 = `settings: 没有注册设置`（⛔ 不造死控件）；
//     ② 下一帧（无点击）那句仍在（模块级状态，⛔ 不按帧清）；
//     ③ 注册表变非空（**不点**也对）⇒ 那句清掉（与 `没有 appdata` 同口径）；
//     ④ 再点 ⇒ 真的打开页面（证明 ③ 之后入口能用）。
// ---------------------------------------------------------------------------------------
void caseEmptySchema(lift::ui::ScriptRuntime& runtime, const std::string& source, const std::string& path,
                     lift::FontEngine* font) {
    using namespace lift_test;
    g_current_case = "ui-hud-script/settings-empty-schema";

    FakeConfig empty;  // 桥可用（available:true）但 `settings.schema` 空表
    empty.schema_empty = true;
    lift::ui::UiHostOps ops{};
    ops.config = &empty;

    std::string err;
    std::unique_ptr<UiScript> script = runtime.load(source, path, font, &err);
    LIFT_CHECK(script != nullptr);
    if (!script) return;

    auto render = [&](std::uint64_t frame, const char* key, UiBuilder* out) {
        UiFrameReadings r;
        r.frame = frame;
        r.host = &ops;
        if (key != nullptr) {
            r.click_ok = true;
            r.click_key = key;
            r.click_x = 4;
            r.click_y = 4;
        }
        return renderWith(*script, r, out, "settings-empty-schema");
    };

    // ① 点 settings… ⇒ 不打开，留一句状态。
    {
        UiBuilder b;
        LIFT_CHECK(render(1, "settings", &b));
        LIFT_CHECK(!b.elements().empty());
        const Element& panel = b.elements().front();
        LIFT_CHECK(panel.kind == ElementKind::Rect);
        LIFT_CHECK(panel.key.empty());          // 仍是 HUD 面板（不是页面背板）
        LIFT_CHECK_EQ_U32(panel.fill.a, 204u);  // #101820CC
        expectNoKey(b, "settings.bg", "空 schema 不许打开空白页");
        const std::vector<std::string> hud = hudLinesOf(b);
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(hud.size()), 2);
        expectLine(hud, 0, "settings: 没有注册设置");
        expectLine(hud, 1, "settings…");
        expectRect(b, "settings", "入口仍可命中（注册表回来时能再点）");
    }
    // ② 下一帧（无点击）⇒ 那句仍在（⛔ 不按帧清）。
    {
        UiBuilder b;
        LIFT_CHECK(render(2, nullptr, &b));
        expectNoKey(b, "settings.bg", "空 schema 不打开（第二帧）");
        const std::vector<std::string> hud = hudLinesOf(b);
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(hud.size()), 2);
        expectLine(hud, 0, "settings: 没有注册设置");
        expectLine(hud, 1, "settings…");
    }
    // ③ 注册表变非空（不点）⇒ 那句清掉。
    empty.schema_empty = false;
    {
        UiBuilder b;
        LIFT_CHECK(render(3, nullptr, &b));
        const std::vector<std::string> hud = hudLinesOf(b);
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(hud.size()), 1);
        expectLine(hud, 0, "settings…");
        expectNoText(b, "settings: 没有注册设置", "注册表回来 ⇒ 那句状态清掉");
    }
    // ④ 再点 ⇒ 真的打开页面。
    {
        UiBuilder b;
        LIFT_CHECK(render(4, "settings", &b));
        LIFT_CHECK(!b.elements().empty());
        const Element& backdrop = b.elements().front();
        ++g_checks;
        if (backdrop.kind != ElementKind::Rect || backdrop.key != "settings.bg") {
            failf(__FILE__, __LINE__, "注册表回来之后点 settings… 应打开设置页（elements[0]=settings.bg）");
        }
        expectRect(b, "settings.back", "页面的 BACK");
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
    std::unique_ptr<UiScript> script = runtime->load(source, path, &font, &err);
    LIFT_CHECK(script != nullptr);
    if (script == nullptr) {
        std::fprintf(stderr, "加载 HUD 脚本失败：%s\n", err.c_str());
        return finish("ui-hud-script");
    }

    // ---- ① + ② + ③ + ④：满配读数 ⇒ 逐行文案 / 版式 / 默认无 PROFILE ----
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
        expectLine(texts, 3, "worst 3s  wall=33.3ms guest=21.5ms shim=4.2ms");
        expectLine(texts, 4, "settings…");  // D4：设置入口恒在最后一行

        // ② 没有 raster / present（任何一条 HUD 文本都不许含这两个词）；D4 没有 `frame` 行。
        for (const std::string& t : texts) {
            ++g_checks;
            if (t.find("raster") != std::string::npos || t.find("present") != std::string::npos) {
                failf(__FILE__, __LINE__, "HUD 文本里出现了 raster / present：`%s`", t.c_str());
            }
            ++g_checks;
            if (t.rfind("frame ", 0) == 0) failf(__FILE__, __LINE__, "D4 之后不许再有 `frame` 行：`%s`", t.c_str());
        }
        expectNoText(builder, "frame 1234", "D4 删掉 frame 行");

        // ③ 颜色精确值 + 版式（面板 = elements[0]，贴左、垂直居中）。
        LIFT_CHECK(!builder.elements().empty());
        int panel_y = 0;
        int panel_h = 0;
        int panel_w = 0;
        if (!builder.elements().empty()) {
            const Element& panel = builder.elements().front();
            LIFT_CHECK(panel.kind == ElementKind::Rect);
            LIFT_CHECK_EQ_U32(panel.fill.r, 16u);   // #101820
            LIFT_CHECK_EQ_U32(panel.fill.g, 24u);
            LIFT_CHECK_EQ_U32(panel.fill.b, 32u);
            LIFT_CHECK_EQ_U32(panel.fill.a, 204u);  // CC = 80% 不透明（2026-11-18 用户裁决）
            LIFT_CHECK(panel.w > 0 && panel.h > 0);
            LIFT_CHECK_EQ_I64(panel.x, 0);  // D4：贴左
            panel_y = panel.y;
            panel_h = panel.h;
            panel_w = panel.w;
            // y = round((720 - panelH) / 2) 的**整数等价判据**（720 与 panelH 同奇偶 ⇒ 恰好 720；
            // 差 1 = Math.round(0.5) 向上的那一档）。
            const int twice = 2 * panel.y + panel.h;
            ++g_checks;
            if (twice != 720 && twice != 721) {
                failf(__FILE__, __LINE__, "面板没有垂直居中：2*y+panelH=%d（期望 720 或 721，y=%d h=%d）", twice,
                      panel.y, panel.h);
            }
        }

        // `settings…` 那一行 + 透明命中 Rect（x/w 与面板一致、y/h 与那一行文本一致）。
        const Element* settings_text = textElement(builder, "settings…");
        const Element* settings_hit = elementByKey(builder, "settings");
        ++g_checks;
        if (settings_text == nullptr || settings_hit == nullptr) {
            failf(__FILE__, __LINE__, "缺 `settings…` 文本或它的命中 Rect（key=settings）");
        } else {
            const int lineH = (panel_h - 16) / 5;  // 5 行（mouse/script/fps/worst/settings…）
            LIFT_CHECK_EQ_I64(settings_hit->x, 0);
            LIFT_CHECK_EQ_I64(settings_hit->w, panel_w);
            LIFT_CHECK_EQ_I64(settings_hit->y, settings_text->y);
            LIFT_CHECK_EQ_I64(settings_hit->h, lineH);
            LIFT_CHECK_EQ_I64(settings_text->y + lineH, panel_y + panel_h - 8);  // 最后一行贴面板底
            LIFT_CHECK_EQ_U32(settings_hit->fill.r, 0u);  // #00000000 = 透明
            LIFT_CHECK_EQ_U32(settings_hit->fill.g, 0u);
            LIFT_CHECK_EQ_U32(settings_hit->fill.b, 0u);
            LIFT_CHECK_EQ_U32(settings_hit->fill.a, 0u);
            LIFT_CHECK(settings_hit->key == "settings");
            // 命中区画在面板之后（顺序不影响点击，但"面板是 elements[0]"必须仍然成立）。
            LIFT_CHECK(&builder.elements().front() != settings_hit);
        }

        // ④ PROFILE 默认隐藏：既没有命中区、也没有标签；除按钮标签外其余 Text 仍必须 16px + FG。
        bool saw_profile_label = false;
        for (const Element& e : builder.elements()) {
            if (e.kind != ElementKind::Text) continue;
            if (e.text == "PROFILE") {
                saw_profile_label = true;  // 默认读数下不该出现（下面断言）
                continue;
            }
            LIFT_CHECK_EQ_U32(e.color.r, 235u);  // #EBFFEB
            LIFT_CHECK_EQ_U32(e.color.g, 255u);
            LIFT_CHECK_EQ_U32(e.color.b, 235u);
            LIFT_CHECK_EQ_U32(e.color.a, 255u);
            LIFT_CHECK_EQ_I64(e.font_size, 16);  // 参考字号 = SIZE
        }
        ++g_checks;
        if (saw_profile_label) failf(__FILE__, __LINE__, "PROFILE 默认必须隐藏（不许画标签）");
        expectNoKey(builder, "profile", "PROFILE 默认必须隐藏（不许挂 key）");
    }

    // ---- ④ 缺省项整行省略 + `guest=--`（⛔ 不编 0；D4：没有 frame 行，settings… 仍在最后）----
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
        expectLine(texts, 0, "worst 3s  wall=1.2ms guest=-- shim=0.3ms");
        expectLine(texts, 1, "settings…");
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
        expectLine(texts, 1, "settings…");

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
        expectLine(texts2, 1, "settings…");
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
        expectLine(texts, 1, "settings…");
    }

    // ---- ⑧ PROFILE 按钮 + 状态机（切片 3；每次用一份**新的**脚本实例 = 干净的模块状态）----
    caseProfileStatus(*runtime, source, path, &font);

    // ---- ⑤ F9 显隐（每次用一份新实例）----
    caseProfileToggle(*runtime, source, path, &font);

    // ---- ⑥ 设置入口（一份实例走完：不可用 → 改回可用 → 打开 → BACK → 再断）----
    caseSettingsEntry(*runtime, source, path, &font);

    // ---- ⑨ 设置状态行留存（阻断 2；一份实例：只读点击 → 下一帧仍在 → BACK → 成功后清）----
    caseSettingsStatusRetention(*runtime, source, path, &font);

    // ---- ⑩ 空 schema ⇒ 不打开（一份实例：空表 → 状态行 → 注册表回来 → 打开）----
    caseEmptySchema(*runtime, source, path, &font);

    script->shutdown();
    return finish("ui-hud-script");
}
