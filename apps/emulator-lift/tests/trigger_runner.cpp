// emulator-lift · 插件运行时 **`TriggerRunner`** 的 pure 守卫（顶层无条件注册 ⇒ pure 档）
//
// 合同：`docs/03-emulator/plugin-runtime.md` §4.2（具名集合 / 一读点最多一个 / 超时 / owner）、
//       §5 C1 判据、§6 行 31–33 / 36–37；施工图 `.tmp/plugin-runtime/c1-design-review.md` §2.2 / §3.2 / §6.1–6.3。
//
// ★ 夹具 = **零 guest**：`TriggerReadings` 是**值输入** ⇒ 触发语义一条都不需要假 Engine / 窗口 / `Machine`
//   （⛔ 不要照普查的 `Fixture + publishEngine` 写：那会同时撞三处假地址，而这里一个都用不上）。
//   "读点"由本文件自己驱动：`Fixture::pump()` 就是"一个读点恰好一次 `atPoint`"。
// ★ 两把注入的钟：`g_now_ms`（`atPoint` 的墙钟参数 + `begin` 的 `setClockForTest`）与 `g_now_ns`
//   （budget 的 `setNsClockForTest`）⇒ 判据不需要真睡（`AfterDelay` 是宿主墙钟、NON-DIFFABLE，
//   所以它只由注入钟判，⛔ 不用墙钟读数当判据）。
//
// 四条用例各自"改坏什么会红"：
//   ① `trigger-runner/after-position-once`：`[lo,hi)` 的**两端**（`pos==lo` 触发 / `pos==hi` 不）、
//      Once 只触发一次、**无效读数不改变状态**（Reenter 的边沿）、done 之后不再 poll。
//      ⇒ 把 `pos < hi` 写成 `pos <= hi` ⇒ `pos==200` 那条红；让无效读数也写 `last_inside` ⇒ 边沿那条红。
//      ★ 无效读数有**两半**（`pos_ok` / `script_ok`），(c) 各钉一条 ⇒ 只判其中一半 ⇒ 对应那条红。
//   ② `trigger-runner/after-script-once`：只比脚本名（**无区间**，`pos` 再离谱也照样触发）、
//      `script_ok=false` 不算匹配、Reenter 的 false→true，以及**重新 arm 不清边沿**。
//      ⇒ AfterScript 若混进区间判定 ⇒ `pos=999999` 那条红；若 re-arm 把边沿清零 ⇒ "同电平第二次"红。
//   ③ `trigger-runner/named-set`：多具名 / 一读点**最多一个**事件（注册顺序、**不级联**）/
//      返回集合 = **完整替换**（未返回的消失，含刚触发的）/ `[]` = done / 重复 name ⇒ fail 并点名 /
//      `maxFires` **续计** + 耗尽再放回 ⇒ fail + 新对象才是新预算 /
//      **fire 优先于 timeout**（同拍只一个事件、timeout 不计 fires、per-arm 重置 deadline）/ 超预算 ⇒ fail。
//      ⇒ 把"命中即 return"改成继续扫 ⇒ 同点回调两次 ⇒ 红；不判预算 ⇒ 超预算那条红。
//   ④ `trigger-runner/rearm`：Once 触发后再返回**同对象** ⇒ fail（⛔ 不是静默重置）；Reenter 需新的
//      有效外→内（**不会立刻再触发**）；`AfterDelay` 计时从头；`AtReadPoint` 从**下一个**匹配读点重新计。
//      ⇒ 去掉 Once 的"已耗尽"判据 ⇒ ① 那条红（"接受放回"= 静默重置预算）。
#include <cstdint>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

#include "check.h"
#include "runtime/context.h"
#include "runtime/edit_sink.h"
#include "runtime/engine_marker.h"  // kEngineSingletonGlobal（假 Engine 的发布点）
#include "runtime/engine_read.h"
#include "runtime/host_window.h"     // WindowManager（独立夹具；⛔ 不经过 Machine）
#include "runtime/input.h"           // kWmLButtonDown / kWmLButtonUp / kWmMouseMove（消息计数）
#include "runtime/input_point.h"
#include "runtime/intent.h"
#include "runtime/intent_executor.h"
#include "runtime/intent_queue.h"
#include "runtime/memory.h"          // GuestMemory（假 Engine / 假池）
#include "runtime/script_globals.h"  // readGlobal / writeGlobal / GlobalKind / GlobalValue
#include "runtime/trigger.h"
#include "runtime/trigger_runner.h"
#include "runtime/value_codec.h"     // encodeInt / decodeInt（ENC 往返的断言）

namespace {

using lift::InputPoint;
using lift::plugin::Budget;
using lift::plugin::Context;
using lift::plugin::Trigger;
using lift::plugin::TriggerEvent;
using lift::plugin::TriggerFactory;
using lift::plugin::TriggerHandler;
using lift::plugin::TriggerKind;
using lift::plugin::TriggerMode;
using lift::plugin::TriggerReadings;
using lift::plugin::TriggerRunner;

using TriggerSet = TriggerHandler::TriggerSet;
using Result = TriggerRunner::Result;

// =====================================================================================
// 注入的钟
// =====================================================================================
std::uint64_t g_now_ms = 0;  // 宿主墙钟毫秒（`atPoint` 的参数 / `begin` 的钟）
std::uint64_t g_now_ns = 0;  // budget 用的纳秒
std::uint64_t testNowMs() { return g_now_ms; }
std::uint64_t testNowNs() { return g_now_ns; }

// =====================================================================================
// 脚本化 handler：按顺序给"这一次回调要返回什么"（第一条给 `start`，此后每条给一次 `onTrigger`）
// =====================================================================================
struct Reply {
    TriggerSet set;                 // 返回的集合（= 完整替换）
    bool same_as_fired = false;     // ★ 返回"刚触发的那个对象"（`ev.trigger`；state 共享）
    std::uint64_t advance_ns = 0;   // ★ 让注入的 ns 钟前进（budget 用；不真睡）
    std::string err;                // 非空 ⇒ 写进 `err`（模拟回调报错）
};

Reply replyWith(TriggerSet set) {
    Reply reply;
    reply.set = std::move(set);
    return reply;
}
Reply replyDone() { return Reply{}; }
Reply replyAgain() {
    Reply reply;
    reply.same_as_fired = true;
    return reply;
}
Reply replyBurning(TriggerSet set, std::uint64_t advance_ns) {
    Reply reply;
    reply.set = std::move(set);
    reply.advance_ns = advance_ns;
    return reply;
}

class ScriptedHandler final : public TriggerHandler {
public:
    std::vector<Reply> replies;       // 顺序消费
    std::vector<TriggerEvent> events; // 每次 onTrigger 的 ev（"一读点最多一个"的证据）
    std::size_t start_calls = 0;

    std::size_t triggerCalls() const { return events.size(); }

    TriggerSet start(Context&, std::string* err) override {
        ++start_calls;
        return next(nullptr, err);
    }
    TriggerSet onTrigger(Context&, const TriggerEvent& ev, std::string* err) override {
        events.push_back(ev);
        return next(&ev, err);
    }

private:
    TriggerSet next(const TriggerEvent* ev, std::string* err) {
        if (cursor_ >= replies.size()) return TriggerSet{};  // 脚本用尽 ⇒ done（⛔ 不是"永远等"）
        const Reply& reply = replies[cursor_++];
        if (reply.advance_ns != 0) g_now_ns += reply.advance_ns;
        if (!reply.err.empty() && err != nullptr) *err = reply.err;
        if (reply.same_as_fired) {
            TriggerSet out;
            if (ev != nullptr) out.push_back(ev->trigger);
            return out;
        }
        return reply.set;
    }

    std::size_t cursor_ = 0;
};

// =====================================================================================
// 一个读点的读数构造器（读不到的东西**显式**标成 ok=false ⇒ 无效读数用例不需要编造）
// =====================================================================================
struct Rd {
    TriggerReadings readings;
    Rd& script(const char* name) {
        readings.script = name;
        readings.script_ok = true;
        return *this;
    }
    // ★ **保留**原始名字、只把 `script_ok` 关掉：这样"忽略 script_ok 的实现"会**触发** ⇒ 用例红。
    //   若连名字一起清空，`now.script == t.script` 本来就为假 ⇒ 那条断言恒真（⛔ 不许那样写）。
    Rd& scriptUnknown() {
        readings.script_ok = false;
        return *this;
    }
    Rd& pos(std::uint32_t value) {
        readings.pos = value;
        readings.pos_ok = true;
        return *this;
    }
    Rd& posUnknown(std::uint32_t raw) {
        readings.pos = raw;
        readings.pos_ok = false;
        return *this;
    }
    Rd& point(InputPoint value) {
        readings.point = value;
        return *this;
    }
    Rd& index(std::uint64_t value) {
        readings.read_index = value;
        return *this;
    }
};

// =====================================================================================
// 夹具：一个 runner + 一个脚本化 handler + 一个 Context（只接 now / trigger 两根线 —— C1b 的
// handler 不碰 Edit / Input / Read，那三个是 C1c 的事）
// =====================================================================================
struct Fixture {
    TriggerFactory factory;
    ScriptedHandler handler;
    TriggerRunner runner;
    Context ctx;
    TriggerReadings now;  // `ctx.now` 指向它（每个 pump 覆盖）
    TriggerEvent ev;
    std::string err;

    // 工厂的 owner **故意**与 runner 的不同（3 vs 7）⇒ `releaseOwner` / `armed().owner` 能证明
    // "owner 由 runner 盖章"（工厂那个值只是初值，⛔ 不是真相）。
    Fixture() : factory(3) {
        g_now_ms = 0;
        g_now_ns = 0;
        runner.setClockForTest(&testNowMs);
        runner.setNsClockForTest(&testNowNs);
        runner.setOwner(7);
        ctx.trigger = &factory;
    }

    bool begin() {
        ctx.now = &now;
        return runner.begin(handler, ctx, &err);
    }

    // ★ 一个读点 = 一次 atPoint（这就是"读点"在本文件的唯一含义）。
    Result pump(const Rd& reading) {
        now = reading.readings;
        ctx.now = &now;
        err.clear();
        return runner.atPoint(now, g_now_ms, ctx, &ev, &err);
    }
};

// armedNames() 的期望比较（避免在宏里写带逗号的 initializer_list）。
bool names(const TriggerRunner& runner, std::initializer_list<const char*> want) {
    const std::vector<std::string> got = runner.armedNames();
    if (got.size() != want.size()) return false;
    std::size_t i = 0;
    for (const char* name : want) {
        if (got[i] != name) return false;
        ++i;
    }
    return true;
}

bool contains(const std::string& text, const char* needle) {
    return text.find(needle) != std::string::npos;
}

// =====================================================================================
// ① after-position-once：`[lo,hi)` 两端 / Once 一次 / 无效读数不变 / done 后不再 poll
// =====================================================================================
void caseAfterPositionOnce() {
    lift_test::g_current_case = "trigger-runner/after-position-once";

    // (a) outside → inside 触发**一次**；`pos == lo`（含）触发；事件字段是**本读点**的读数。
    {
        Fixture f;
        const Trigger p = f.factory.afterPosition("P", "SCENE1", 100, 200, TriggerMode::Once);
        f.handler.replies = {replyWith({p}), replyDone()};

        LIFT_CHECK(f.begin());
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.runner.armedCount()), 1);
        LIFT_CHECK(names(f.runner, {"P"}));

        // 区间外（pos=99）：不触发，且不消耗回调
        LIFT_CHECK(f.pump(Rd().script("SCENE1").pos(99)) == Result::None);
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.runner.firesOf(p)), 0);
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.handler.triggerCalls()), 0);

        // pos == lo（**含**下界）：触发
        LIFT_CHECK(f.pump(Rd().script("SCENE1").pos(100).index(7)) == Result::Fired);
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.runner.firesOf(p)), 1);
        LIFT_CHECK(f.ev.what == TriggerEvent::What::Fired);
        LIFT_CHECK(std::string(f.ev.kind()) == "trigger");
        LIFT_CHECK(f.ev.name == "P");
        LIFT_CHECK(f.ev.trigger_kind == TriggerKind::AfterPosition);
        LIFT_CHECK_EQ_U32(f.ev.pos, 100);
        LIFT_CHECK(f.ev.pos_ok);
        LIFT_CHECK(f.ev.script == "SCENE1");
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.ev.read_index), 7);

        // `[]` = done（⛔ 不是"永远等"），且 done 之后不再 poll
        LIFT_CHECK(f.runner.done());
        LIFT_CHECK(!f.runner.failed());
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.runner.armedCount()), 0);
        LIFT_CHECK(f.pump(Rd().script("SCENE1").pos(150)) == Result::Done);
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.handler.triggerCalls()), 1);
    }

    // (b) 上界**不含**：`pos == hi` 不触发（把 `pos < hi` 写成 `<=` ⇒ 这一条红）。
    {
        Fixture f;
        const Trigger p = f.factory.afterPosition("P", "SCENE1", 100, 200, TriggerMode::Once);
        f.handler.replies = {replyWith({p}), replyDone()};
        LIFT_CHECK(f.begin());
        LIFT_CHECK(f.pump(Rd().script("SCENE1").pos(200)) == Result::None);
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.runner.firesOf(p)), 0);
        LIFT_CHECK(f.pump(Rd().script("SCENE1").pos(199)) == Result::Fired);
    }

    // (c) **无效读数不改变状态**（Reenter 的边沿）：arm 时在区间内触发过之后，
    //     一条"位置无效、原始值在区间外"的读数**不许**把 `last_inside` 写成 false。
    //     ⇒ 若它被写成 false，紧接着的"有效 inside"会**再触发**一次（那正是本用例要抓的）。
    {
        Fixture f;
        const Trigger p = f.factory.afterPosition("P", "SCENE1", 100, 200, TriggerMode::Reenter);
        f.handler.replies = {replyWith({p}), replyAgain(), replyDone()};
        LIFT_CHECK(f.begin());

        LIFT_CHECK(f.pump(Rd().script("SCENE1").pos(150)) == Result::Fired);  // 首边沿（外→内）
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.runner.firesOf(p)), 1);
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.runner.armedCount()), 1);  // 同一对象重新 arm

        LIFT_CHECK(f.pump(Rd().script("SCENE1").posUnknown(50)) == Result::None);  // ★ 无效读数
        LIFT_CHECK(f.pump(Rd().script("SCENE1").pos(160)) == Result::None);        // 还没有新的外→内
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.handler.triggerCalls()), 1);

        // ★ **同一个门的另一半**：`pos_ok` 之外，`script_ok=false` 也是无效读数（脚本身份读不到）
        //   ⇒ 不触发、**也不写 `last_inside`**。只判 `pos_ok` 的实现会把这条读数当成"有效的区间外"
        //   （名字对不上 ⇒ inside=false）把边沿清零 ⇒ 紧接着的有效 inside 被当成新边沿 ⇒ **多触发一次**。
        //   `Rd::scriptUnknown()` 故意不给名字：位置在区间内、名字无效 —— 正是"读数无效"的最小形状。
        LIFT_CHECK(f.pump(Rd().scriptUnknown().pos(160)) == Result::None);   // 名字读不到 ⇒ 不触发
        LIFT_CHECK(f.pump(Rd().script("SCENE1").pos(160)) == Result::None);  // 边沿没被非法清零
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.runner.firesOf(p)), 1);
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.handler.triggerCalls()), 1);

        LIFT_CHECK(f.pump(Rd().script("SCENE1").pos(50)) == Result::None);   // 真的出区间
        LIFT_CHECK(f.pump(Rd().script("SCENE1").pos(160)) == Result::Fired); // 再进来 ⇒ 新边沿
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.runner.firesOf(p)), 2);
        LIFT_CHECK(f.runner.done());
    }

    // (d) 脚本名不匹配 = **有效**读数（inside=false）⇒ 边沿清零；只差脚本名不触发。
    {
        Fixture f;
        const Trigger p = f.factory.afterPosition("P", "SCENE1", 100, 200, TriggerMode::Reenter);
        f.handler.replies = {replyWith({p}), replyAgain(), replyDone()};
        LIFT_CHECK(f.begin());
        LIFT_CHECK(f.pump(Rd().script("SCENE1").pos(150)) == Result::Fired);
        LIFT_CHECK(f.pump(Rd().script("SCENE2").pos(150)) == Result::None);  // 名字不对 ⇒ inside=false
        LIFT_CHECK(f.pump(Rd().script("SCENE1").pos(150)) == Result::Fired);  // 名字回来 ⇒ 新边沿
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.runner.firesOf(p)), 2);
    }
}

// =====================================================================================
// ② after-script-once：只比名字（无区间）/ script_ok 门 / Reenter 边沿 / re-arm 不清边沿
// =====================================================================================
void caseAfterScriptOnce() {
    lift_test::g_current_case = "trigger-runner/after-script-once";

    // (a) 纯电平门：`script_ok=false` 不算匹配；位置**再离谱也照样触发**（没有区间）。
    {
        Fixture f;
        const Trigger s = f.factory.afterScript("S", "SCENE1", TriggerMode::Once);
        f.handler.replies = {replyWith({s}), replyDone()};
        LIFT_CHECK(f.begin());
        LIFT_CHECK(names(f.runner, {"S"}));

        // ★ 名字读不到 ⇒ 不算匹配。**名字仍在**（只有 `script_ok=false`）⇒ 忽略这个门的实现会触发 ⇒ 红。
        LIFT_CHECK(f.pump(Rd().script("SCENE1").scriptUnknown().pos(0)) == Result::None);
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.runner.firesOf(s)), 0);
        LIFT_CHECK(f.pump(Rd().script("SCENE0").pos(0)) == Result::None);

        // ★ 位置远在任何 `[lo,hi)` 之外：AfterScript **没有**区间 ⇒ 照样触发
        LIFT_CHECK(f.pump(Rd().script("SCENE1").pos(999999)) == Result::Fired);
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.runner.firesOf(s)), 1);
        LIFT_CHECK(f.ev.trigger_kind == TriggerKind::AfterScript);
        LIFT_CHECK(f.ev.name == "S");
        LIFT_CHECK(f.ev.script == "SCENE1");
        LIFT_CHECK(f.runner.done());
    }

    // (b) Reenter：false→true 才触发；**同一对象重新 arm 不清边沿**（同电平不许再触发）。
    {
        Fixture f;
        const Trigger s = f.factory.afterScript("S", "SCENE1", TriggerMode::Reenter);
        f.handler.replies = {replyWith({s}), replyAgain(), replyDone()};
        LIFT_CHECK(f.begin());

        LIFT_CHECK(f.pump(Rd().script("SCENE1")) == Result::Fired);
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.runner.armedCount()), 1);  // 同一对象重新 arm

        // ★ 电平还是 1：没有新的 false→true ⇒ 不触发（re-arm 若把边沿清零 ⇒ 这一条红）
        LIFT_CHECK(f.pump(Rd().script("SCENE1")) == Result::None);
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.handler.triggerCalls()), 1);

        LIFT_CHECK(f.pump(Rd().script("SCENE0")) == Result::None);  // 出（false）
        LIFT_CHECK(f.pump(Rd().script("SCENE1")) == Result::Fired); // 再进（true）⇒ 新边沿
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.runner.firesOf(s)), 2);
        LIFT_CHECK(f.runner.done());
    }
}

// =====================================================================================
// ③ named-set：具名集合 / 一读点最多一个 / 完整替换 / [] = done / 重复 name / maxFires / timeout / budget
// =====================================================================================
void caseNamedSet() {
    lift_test::g_current_case = "trigger-runner/named-set";

    // (a) 两个具名、同一读点都满足 ⇒ **只触发注册顺序的第一个**；下一拍才轮到第二个（不级联）。
    //     返回的集合 = 完整替换（未返回的消失，含刚触发的那个）。
    {
        Fixture f;
        const Trigger a = f.factory.afterScript("A", "S", TriggerMode::Reenter);
        const Trigger b = f.factory.afterScript("B", "S", TriggerMode::Reenter);
        f.handler.replies = {replyWith({a, b}), replyWith({a, b}), replyWith({a, b}), replyWith({b}),
                             replyDone()};
        LIFT_CHECK(f.begin());
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.runner.armedCount()), 2);
        LIFT_CHECK(names(f.runner, {"A", "B"}));

        // 读点 1：A、B **都满足** ⇒ 恰好一个事件，且是注册顺序靠前的 A
        LIFT_CHECK(f.pump(Rd().script("S")) == Result::Fired);
        LIFT_CHECK(f.ev.name == "A");
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.handler.triggerCalls()), 1);
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.runner.firesOf(a)), 1);
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.runner.firesOf(b)), 0);

        // 读点 2：A 的边沿已是 true ⇒ 轮到 B（若上一拍级联过，这里会是第二个事件 ⇒ 红）
        LIFT_CHECK(f.pump(Rd().script("S")) == Result::Fired);
        LIFT_CHECK(f.ev.name == "B");
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.handler.triggerCalls()), 2);

        // 读点 3：两个边沿都是 true ⇒ 没有事件（⛔ 不是"每读点都触发"）
        LIFT_CHECK(f.pump(Rd().script("S")) == Result::None);
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.handler.triggerCalls()), 2);
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.runner.armedCount()), 2);

        // 读点 4：出电平（边沿清零，不触发）
        LIFT_CHECK(f.pump(Rd().script("S0")) == Result::None);

        // 读点 5：A 再触发；回调返回 {B} ⇒ **A 消失**（未返回的 = 消费掉，含刚触发的）
        LIFT_CHECK(f.pump(Rd().script("S")) == Result::Fired);
        LIFT_CHECK(f.ev.name == "A");
        LIFT_CHECK(names(f.runner, {"B"}));
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.runner.armedCount()), 1);

        // 读点 6：出电平；读点 7：B 触发 ⇒ `[]` = done
        LIFT_CHECK(f.pump(Rd().script("S0")) == Result::None);
        LIFT_CHECK(f.pump(Rd().script("S")) == Result::Fired);
        LIFT_CHECK(f.ev.name == "B");
        LIFT_CHECK(f.runner.done());
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.runner.armedCount()), 0);
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.runner.firesOf(a)), 2);
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.runner.firesOf(b)), 2);
    }

    // (b) 重复 name ⇒ fail **并点名**（⛔ 不静默覆盖）；fail 后不再 poll。
    {
        Fixture f;
        const Trigger x = f.factory.afterScript("dup", "S", TriggerMode::Once);
        const Trigger y = f.factory.afterScript("dup", "S", TriggerMode::Once);
        f.handler.replies = {replyWith({x, y})};
        LIFT_CHECK(!f.begin());
        LIFT_CHECK(f.runner.failed());
        LIFT_CHECK(!f.runner.done());
        LIFT_CHECK(f.runner.failName() == "dup");
        LIFT_CHECK(contains(f.runner.lastError(), "duplicate trigger name"));
        LIFT_CHECK(contains(f.err, "dup"));
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.runner.armedCount()), 0);
        LIFT_CHECK(f.pump(Rd().script("S")) == Result::Failed);
    }

    // (c) ★ **fire 优先于 timeout**：同拍"T 已超时 + B 满足" ⇒ 只报 fire(B)，T 留到下一拍；
    //     timeout **不是** fire（不增计数）；回调把 T 放回 ⇒ **per-arm 重置** deadline。
    {
        Fixture f;
        // T：脚本永不匹配 + 100 ms 超时（Once）；B：满足 S（Once）
        const Trigger t = f.factory.afterScript("T", "NEVER", TriggerMode::Once, 0, 100);
        const Trigger b = f.factory.afterScript("B", "S", TriggerMode::Once);
        const Trigger b2 = f.factory.afterScript("B", "S", TriggerMode::Once);  // 同名可复用（旧对象已消费）
        f.handler.replies = {replyWith({t, b}), replyWith({t, b2}), replyWith({b2}), replyDone()};

        g_now_ms = 0;
        LIFT_CHECK(f.begin());

        g_now_ms = 150;  // T 的 deadline(100) 已过，且 B 满足
        LIFT_CHECK(f.pump(Rd().script("S")) == Result::Fired);
        LIFT_CHECK(f.ev.what == TriggerEvent::What::Fired);
        LIFT_CHECK(f.ev.name == "B");
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.handler.triggerCalls()), 1);  // ★ 一拍只一个事件
        // 回调把 T 放回 ⇒ per-arm 重置：deadline = 150 + 100 = 250
        LIFT_CHECK(names(f.runner, {"T", "B"}));

        g_now_ms = 200;  // 过了**旧的** deadline、没过新的 ⇒ 不许超时（per-arm 重置的证据）
        LIFT_CHECK(f.pump(Rd().script("NONE")) == Result::None);
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.handler.triggerCalls()), 1);

        g_now_ms = 260;  // 过了新 deadline ⇒ timeout 事件
        LIFT_CHECK(f.pump(Rd().script("NONE")) == Result::TimedOut);
        LIFT_CHECK(f.ev.what == TriggerEvent::What::TimedOut);
        LIFT_CHECK(std::string(f.ev.kind()) == "timeout");
        LIFT_CHECK(f.ev.name == "T");
        LIFT_CHECK(f.ev.trigger_kind == TriggerKind::AfterScript);
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.runner.firesOf(t)), 0);  // ★ timeout 不计 fires
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.runner.armedCount()), 1); // 被移出 armed
        LIFT_CHECK(names(f.runner, {"B"}));
        LIFT_CHECK(!f.runner.done());  // timeout **不**自动 finish / quit

        g_now_ms = 300;  // 被移出的 T 不再参与；剩下的 B（新对象）照旧能触发 ⇒ [] = done
        LIFT_CHECK(f.pump(Rd().script("S")) == Result::Fired);
        LIFT_CHECK(f.ev.name == "B");
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.runner.firesOf(b2)), 1);
        LIFT_CHECK(f.runner.done());
    }

    // (d) `maxFires` **跨 arm 续计**：耗尽再放回同一对象 ⇒ fail；**新对象**才是新预算。
    {
        Fixture f;
        const Trigger m = f.factory.atReadPoint("M", InputPoint::Count, 2, TriggerMode::Reenter);
        f.handler.replies = {replyWith({m}), replyWith({m}), replyAgain()};
        LIFT_CHECK(f.begin());
        LIFT_CHECK(f.pump(Rd().point(InputPoint::PeekMsg)) == Result::Fired);
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.runner.firesOf(m)), 1);

        // 同一对象放回 ⇒ 续计（第 2 次 fire），但 `fires == max_fires` ⇒ 耗尽 ⇒ fail（点名）
        LIFT_CHECK(f.pump(Rd().point(InputPoint::PeekMsg)) == Result::Failed);
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.runner.firesOf(m)), 2);
        LIFT_CHECK(f.runner.failed());
        LIFT_CHECK(f.runner.failName() == "M");
        LIFT_CHECK(contains(f.runner.lastError(), "exhausted"));
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.runner.armedCount()), 0);
    }
    {
        // ★ 新对象（工厂新造 ⇒ fires=0）⇒ 新预算：同名、同参数、照旧能跑
        Fixture f;
        const Trigger fresh = f.factory.atReadPoint("M", InputPoint::Count, 2, TriggerMode::Reenter);
        f.handler.replies = {replyWith({fresh}), replyDone()};
        LIFT_CHECK(f.begin());
        LIFT_CHECK(f.pump(Rd().point(InputPoint::PeekMsg)) == Result::Fired);
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.runner.firesOf(fresh)), 1);
        LIFT_CHECK(f.runner.done());
    }

    // (e) budget：**2 ms 只罩用户 JS 本体**（判据在 handler 侧）⇒ runner 只留**宿主安全网**
    //     （预算 × 10）。紧预算 + 烧过 10× ⇒ fail 且 lastError 写清 `(host)`；放宽（缺省 2 ms）⇒ 能跑。
    {
        Fixture f;
        const Trigger s = f.factory.afterScript("S", "SCENE1", TriggerMode::Once);
        // ★ 烧 60 ns vs 紧预算 5 ns：安全网 = 5 × 10 = 50 ns ⇒ 60 越过（10 ns 就**不该**再红了）。
        f.handler.replies = {replyBurning({s}, 60)};
        Budget tight;
        tight.deadline_ns = 5;
        f.runner.setBudget(tight);
        LIFT_CHECK(!f.begin());
        LIFT_CHECK(f.runner.failed());
        LIFT_CHECK(contains(f.runner.lastError(), "budget"));
        // ★ 宿主段越线 ⇒ reason 必须带 `(host)`（与用户 JS 超限可区分）。
        LIFT_CHECK(contains(f.runner.lastError(), "(host)"));
        LIFT_CHECK(contains(f.err, "budget"));
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.runner.armedCount()), 0);
    }
    {
        Fixture f;  // 缺省预算（2 ms）⇒ 安全网 20 ms；10 ns 当然不算超 ⇒ 能跑
        const Trigger s = f.factory.afterScript("S", "SCENE1", TriggerMode::Once);
        f.handler.replies = {replyBurning({s}, 10), replyDone()};
        LIFT_CHECK(f.begin());
        LIFT_CHECK(!f.runner.failed());
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.runner.armedCount()), 1);
        LIFT_CHECK(f.pump(Rd().script("SCENE1")) == Result::Fired);
    }
    {
        // onTrigger 越过宿主安全网同样 ⇒ fail（点名那个 trigger、reason 带 `(host)`）
        Fixture f;
        const Trigger s = f.factory.afterScript("S", "SCENE1", TriggerMode::Once);
        f.handler.replies = {replyWith({s}), replyBurning(TriggerSet{}, 60)};
        Budget tight;
        tight.deadline_ns = 5;
        f.runner.setBudget(tight);
        LIFT_CHECK(f.begin());
        LIFT_CHECK(f.pump(Rd().script("SCENE1")) == Result::Failed);
        LIFT_CHECK(f.runner.failed());
        LIFT_CHECK(f.runner.failName() == "S");
        LIFT_CHECK(contains(f.runner.lastError(), "budget"));
        LIFT_CHECK(contains(f.runner.lastError(), "(host)"));
    }
}

// =====================================================================================
// ④ rearm：Once 放回 = fail / Reenter 需新边沿 / AfterDelay 计时从头 / AtReadPoint 下一匹配读点
// =====================================================================================
void caseRearm() {
    lift_test::g_current_case = "trigger-runner/rearm";

    // (a) `Once` 触发后再返回**同对象** ⇒ fail（⛔ 不是静默重置预算）。
    {
        Fixture f;
        const Trigger o = f.factory.afterScript("O", "S", TriggerMode::Once);
        f.handler.replies = {replyWith({o}), replyAgain()};
        LIFT_CHECK(f.begin());
        LIFT_CHECK(f.pump(Rd().script("S")) == Result::Failed);
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.runner.firesOf(o)), 1);  // 它**确实**触发过
        LIFT_CHECK(f.ev.name == "O");                                          // 事件照旧填
        LIFT_CHECK(f.runner.failed());
        LIFT_CHECK(f.runner.failName() == "O");
        LIFT_CHECK(contains(f.runner.lastError(), "exhausted"));
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.runner.armedCount()), 0);
    }

    // (b) `Reenter` 重新 arm：**不会立刻再触发**（要新的有效外→内）。
    {
        Fixture f;
        const Trigger r = f.factory.afterPosition("R", "S", 100, 200, TriggerMode::Reenter);
        f.handler.replies = {replyWith({r}), replyAgain(), replyDone()};
        LIFT_CHECK(f.begin());
        LIFT_CHECK(f.pump(Rd().script("S").pos(150)) == Result::Fired);
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.runner.armedCount()), 1);  // 同一对象重新 arm
        LIFT_CHECK(f.pump(Rd().script("S").pos(150)) == Result::None);           // ★ 不会立刻再触发
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.handler.triggerCalls()), 1);
        LIFT_CHECK(f.pump(Rd().script("S").pos(50)) == Result::None);            // 外
        LIFT_CHECK(f.pump(Rd().script("S").pos(150)) == Result::Fired);          // 内 ⇒ 新边沿
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.runner.firesOf(r)), 2);
        LIFT_CHECK(f.runner.done());
    }

    // (c) `AfterDelay`：重新 arm ⇒ **计时从头**（沿用旧的 armed_ms ⇒ 1150 会误触发 ⇒ 红）。
    {
        Fixture f;
        const Trigger d = f.factory.afterDelay("D", 100, TriggerMode::Reenter);
        f.handler.replies = {replyWith({d}), replyAgain(), replyDone()};
        g_now_ms = 1000;
        LIFT_CHECK(f.begin());

        g_now_ms = 1050;
        LIFT_CHECK(f.pump(Rd()) == Result::None);
        g_now_ms = 1100;
        LIFT_CHECK(f.pump(Rd()) == Result::Fired);  // 1000 + 100
        g_now_ms = 1150;
        LIFT_CHECK(f.pump(Rd()) == Result::None);   // ★ 1100 + 100 还没到（计时从头）
        g_now_ms = 1200;
        LIFT_CHECK(f.pump(Rd()) == Result::Fired);  // 1100 + 100
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.runner.firesOf(d)), 2);
        LIFT_CHECK(f.runner.done());
    }

    // (d) `AtReadPoint`：`point_filter` 生效；重新 arm 从**下一个**匹配读点重新计。
    {
        Fixture f;
        const Trigger p = f.factory.atReadPoint("P", InputPoint::PeekMsg, 10, TriggerMode::Reenter);
        f.handler.replies = {replyWith({p}), replyAgain(), replyDone()};
        LIFT_CHECK(f.begin());

        LIFT_CHECK(f.pump(Rd().point(InputPoint::GetMsg)) == Result::None);  // 不匹配的读点
        LIFT_CHECK(f.pump(Rd().point(InputPoint::PeekMsg).index(3)) == Result::Fired);
        LIFT_CHECK(f.ev.point == InputPoint::PeekMsg);
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.ev.read_index), 3);
        LIFT_CHECK(f.pump(Rd().point(InputPoint::PeekMsg).index(4)) == Result::Fired);  // 下一匹配读点
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.runner.firesOf(p)), 2);
        LIFT_CHECK(f.runner.done());
    }

    // (e) `releaseOwner`：owner 由 runner **盖章**（工厂那个 3 不作数）；只抹该 owner 的 armed 槽，
    //     ⛔ 不置 done（"Run 结束 ⇒ 没有 armed" ≠ "回调返回 [] ⇒ done"）。
    {
        Fixture f;
        const Trigger a = f.factory.afterScript("A", "S", TriggerMode::Reenter);
        f.handler.replies = {replyWith({a}), replyDone()};
        LIFT_CHECK(f.begin());
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.runner.armed().size()), 1);
        LIFT_CHECK_EQ_U32(f.runner.armed()[0].owner, 7);  // ★ runner 盖章（⛔ 不是工厂的 3）
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.runner.releaseOwner(3)), 0);
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.runner.armedCount()), 1);
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.runner.releaseOwner(7)), 1);
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.runner.armedCount()), 0);
        LIFT_CHECK(!f.runner.done());  // 只抹 armed，不是 done
        LIFT_CHECK(!f.runner.failed());
        LIFT_CHECK(f.pump(Rd().script("S")) == Result::None);  // 没有 armed ⇒ 没有事件
    }
}

// =====================================================================================
// ⑤–⑦ 的夹具：**独立** `GuestMemory`（假 Engine / 假池）+ **独立** `WindowManager` +
//   **可注入**读数源。★ 零 `Machine`、零 `publishEngine` 名字表、零 shim 桩页
//   （施工图 §6.1 的夹具修正：`TriggerReadings` 是值输入 ⇒ 触发语义不需要假 Engine；
//    只有 global 那一半要一块 `GuestMemory`，而它照 `tests/control_globals.cpp:64-73` 造）。
// =====================================================================================
constexpr std::uint32_t kFxBase = 0x00480000u;
constexpr std::uint32_t kFxBaseSize = 0x00100000u;   // [0x480000, 0x580000)
constexpr std::uint32_t kFxEngine = kFxBase;
constexpr std::uint32_t kFxIntPool = 0x004A0000u;    // 假 int 池（沿用 control_globals.cpp 的 0x4A0000）
constexpr std::uint32_t kFxFloatPool = 0x004B0000u;
constexpr std::uint32_t kFxKey = 0x1234ABCDu;        // 运行期的 ENC/DEC key（⛔ 不是默认 0）
constexpr std::uint32_t kFxIntCount = 0xB1B6u;
constexpr std::uint32_t kFxFloatCount = 0xB1B6u;
constexpr std::uint32_t kFxGlobalId = 0xB1B6u;  // 合同点名的那个 id（`global-get b1b6`）
constexpr std::uint32_t kFxOwner = 7;           // 本切片单 owner（C5 才多 owner）

bool put32(lift::GuestMemory& mem, std::uint32_t ea, std::uint32_t value) {
    if (!mem.committed(ea, 4u)) return false;
    mem.wr32(ea, value);
    return true;
}

bool makeFakeEngine(lift::GuestMemory& mem) {
    if (!mem.reserve()) return false;
    if (!mem.commitZeroed(kFxBase, kFxBaseSize, lift::RegionKind::Heap, "fake-script-globals")) return false;
    if (!put32(mem, lift::kEngineSingletonGlobal, kFxEngine)) return false;
    if (!put32(mem, kFxEngine + lift::kScriptGlobalIntBaseOffset, kFxIntPool)) return false;
    if (!put32(mem, kFxEngine + lift::kScriptGlobalIntCountOffset, kFxIntCount)) return false;
    if (!put32(mem, kFxEngine + lift::kScriptGlobalFloatBaseOffset, kFxFloatPool)) return false;
    if (!put32(mem, kFxEngine + lift::kScriptGlobalFloatCountOffset, kFxFloatCount)) return false;
    return put32(mem, kFxEngine + lift::kScriptGlobalIntKeyOffset, kFxKey);
}

std::uint32_t intAddr(std::uint32_t index) {
    return kFxIntPool + lift::kScriptGlobalSlotBytes * index;
}

// 合成读数（**可注入**：`ReadingsSource` 是 C1 的测试缝 ⇒ 触发语义零 guest）。
struct Readings final : lift::plugin::ReadingsSource {
    std::string script = "SCENE1";
    std::uint32_t pos = 0;
    bool pos_ok = true;
    std::uint64_t samples = 0;

    lift::plugin::TriggerReadings sample(lift::InputPoint) override {
        ++samples;
        lift::plugin::TriggerReadings readings;
        readings.script = script;
        readings.script_ok = !script.empty();
        readings.pos = pos;
        readings.pos_ok = pos_ok;
        return readings;
    }
};

// 合成 EngineRead：`globalGet` 直接走 `readGlobal`（**同一份**引擎层实现 ⇒ 不是第二套地址算术）。
struct FakeEngineRead final : lift::plugin::EngineRead {
    lift::GuestMemory* mem = nullptr;
    std::uint64_t calls = 0;

    bool globalGet(lift::GlobalKind kind, std::uint32_t id, lift::GlobalValue* out,
                   lift::GlobalError* err) override {
        ++calls;
        if (mem == nullptr) return false;
        return lift::readGlobal(*mem, id, kind, out, err);
    }
};

// 一次回调要做的全部事（顺序 = 合同 §4.4 的同点组合 `read → edit → input`）。
struct Action {
    bool do_read = false;
    bool read_ok = false;
    std::uint32_t read_value = 0;

    bool do_edit = false;
    std::uint32_t edit_mask = 0;  // 新值 = 读回值 | mask
    lift::plugin::Receipt edit_receipt;

    bool do_readback = false;
    bool readback_ok = false;
    std::uint32_t readback_value = 0;

    std::vector<lift::plugin::Intent> submit;             // 走 `ctx.input->submit`（staged）
    std::vector<lift::plugin::Receipt> receipts;          // 对应每次 submit 的回执

    // ★ 可判的"poll 在 releaseDue 之前"：回调里读 executor 的 `released()`。
    bool record_released = false;
    std::uint64_t released_seen = ~std::uint64_t{0};

    TriggerSet set;
    bool same_as_fired = false;
};

// 脚本化 handler：每次 `start` / `onTrigger` 消费一条 `Action`（用尽 ⇒ `[]` = done）。
class ActionHandler final : public TriggerHandler {
public:
    const lift::plugin::IntentExecutor* exec = nullptr;
    std::vector<Action> actions;
    std::vector<TriggerEvent> events;
    std::size_t start_calls = 0;

    std::size_t triggerCalls() const { return events.size(); }

    TriggerSet start(Context& ctx, std::string*) override {
        ++start_calls;
        return run(ctx, nullptr);
    }
    TriggerSet onTrigger(Context& ctx, const TriggerEvent& ev, std::string*) override {
        events.push_back(ev);
        return run(ctx, &ev);
    }

private:
    TriggerSet run(Context& ctx, const TriggerEvent* ev) {
        if (cursor_ >= actions.size()) return TriggerSet{};
        Action& action = actions[cursor_++];
        // ★ 顺序就是合同 §4.4 的同点组合（read → edit → input）；这里也证明"Edit 之后紧接着读
        //   拿到的是新值"（同一个回调栈里，⛔ 不等任何读点）。
        if (action.record_released && exec != nullptr) action.released_seen = exec->released();
        if (action.do_read && ctx.read != nullptr) {
            lift::GlobalValue value;
            lift::GlobalError err = lift::GlobalError::None;
            action.read_ok = ctx.read->globalGet(lift::GlobalKind::Int, kFxGlobalId, &value, &err);
            action.read_value = value.u32;
        }
        if (action.do_edit && ctx.edit != nullptr) {
            lift::GlobalValue value;
            value.kind = lift::GlobalKind::Int;
            value.u32 = action.read_value | action.edit_mask;
            action.edit_receipt = ctx.edit->globalSet(lift::GlobalKind::Int, kFxGlobalId, value);
        }
        if (action.do_readback && ctx.read != nullptr) {
            lift::GlobalValue value;
            lift::GlobalError err = lift::GlobalError::None;
            action.readback_ok = ctx.read->globalGet(lift::GlobalKind::Int, kFxGlobalId, &value, &err);
            action.readback_value = value.u32;
        }
        for (const lift::plugin::Intent& intent : action.submit) {
            if (ctx.input == nullptr) break;
            action.receipts.push_back(ctx.input->submit(intent));
        }
        if (action.same_as_fired) {
            TriggerSet out;
            if (ev != nullptr) out.push_back(ev->trigger);
            return out;
        }
        return action.set;
    }

    std::size_t cursor_ = 0;
};

// 一个 intent 的构造器（1280×720 参考坐标；owner 缺省 = 本切片的单 owner）。
lift::plugin::Intent intentOf(lift::plugin::IntentKind kind, int x, int y,
                             std::uint32_t owner = kFxOwner) {
    lift::plugin::Intent intent;
    intent.kind = kind;
    intent.cls = lift::plugin::IntentClass::GuestWrite;
    intent.origin = lift::plugin::Origin::QjsControl;
    intent.owner = owner;
    intent.x = x;
    intent.y = y;
    return intent;
}

// 夹具：一个 guest 内存 + 一个窗口 + 一个队列 + 一个 executor + 一个 runner/handler。
// ★ 构造函数里的**装配顺序**就是接线：`setBus` 填 `ctx.edit/read`，`context().trigger` 由夹具填
//   （本切片的 `atPoint` 只填 now/edit/input/read）。
struct ExecFixture {
    lift::GuestMemory mem;
    bool mem_ok = false;

    lift::WindowManager wm;
    std::uint32_t hwnd = 0;
    bool win_ok = false;

    lift::plugin::IntentQueue queue;
    Readings readings;
    lift::plugin::GuestEditSink edit;
    FakeEngineRead read;
    lift::plugin::IntentExecutor exec;

    TriggerFactory factory;
    ActionHandler handler;
    TriggerRunner runner;
    std::string err;

    ExecFixture() : edit(mem), exec(queue, readings) {
        g_now_ms = 1000;
        g_now_ns = 0;
        mem_ok = makeFakeEngine(mem);
        hwnd = wm.createWindow("IntentTest", 0u, 0u, 0u, 0u, 1280, 720, 0u);
        win_ok = hwnd != 0;
        read.mem = &mem;
        exec.setBus(&edit, &read);
        exec.setTriggerRunner(&runner);
        exec.context().trigger = &factory;
        exec.setClockForTest(&testNowMs);
        runner.setClockForTest(&testNowMs);
        runner.setNsClockForTest(&testNowNs);
        runner.setOwner(kFxOwner);
        factory.setOwner(kFxOwner);
        handler.exec = &exec;
    }

    bool begin() {
        err.clear();
        return runner.begin(handler, exec.context(), &err);
    }
    // ★ 一个读点 = 一次 `atPoint`（本文件里"读点"的唯一含义）。
    void pump(lift::InputPoint point = lift::InputPoint::PeekMsg) { exec.atPoint(wm, point); }

    void cursor(int* x, int* y) const { wm.cursorClientPos(hwnd, x, y); }
};

std::size_t countMsgs(lift::WindowManager& wm, std::size_t from, std::uint32_t message) {
    std::size_t n = 0;
    for (std::size_t i = from; i < wm.queueSize(); ++i) {
        lift::WinMessage m;
        if (!wm.peekAt(i, &m)) continue;
        if (m.message == message) ++n;
    }
    return n;
}

// 槽里的**原始** 32 位（ENC 的证据：⛔ 不是明文）。
std::uint32_t rawSlot(const lift::GuestMemory& mem) { return mem.rd32(intAddr(kFxGlobalId)); }

// =====================================================================================
// ⑤ global-read-modify-write-click：ENC 往返 / Edit **同拍**读回 / Input 一读点一条 +
//    click 两段 / Edit **不占** Input 预算
// =====================================================================================
void caseGlobalReadModifyWriteClick() {
    lift_test::g_current_case = "trigger-runner/global-read-modify-write-click";
    ExecFixture f;
    LIFT_CHECK(f.mem_ok);
    LIFT_CHECK(f.win_ok);
    if (!f.mem_ok || !f.win_ok) return;

    // 起点：0x1200（走 `writeGlobal` —— 与 Edit **同一份**写实现；槽里因此是 ENC，不是明文）。
    lift::GlobalValue seed;
    seed.kind = lift::GlobalKind::Int;
    seed.u32 = 0x1200u;
    lift::GlobalError seed_err = lift::GlobalError::None;
    LIFT_CHECK(lift::writeGlobal(f.mem, kFxGlobalId, lift::GlobalKind::Int, seed, &seed_err));
    LIFT_CHECK(seed_err == lift::GlobalError::None);
    // ★ **ENC 往返**：槽 = ENC(0x1200)；DEC(槽) = 0x1200。
    LIFT_CHECK_EQ_U32(rawSlot(f.mem), lift::encodeInt(0x1200u, kFxKey));
    LIFT_CHECK_EQ_U32(lift::decodeInt(rawSlot(f.mem), kFxKey), 0x1200u);

    const Trigger a = f.factory.afterScript("A", "SCENE1", TriggerMode::Once);
    const Trigger c = f.factory.afterDelay("C", 900, TriggerMode::Once);

    // 回调①（A 触发的那一拍）：read → edit|0x10 → **同拍读回** → submit click（staged）；
    //   返回 `C`（900 ms 之后再来一次 —— 那时 click 的抬起到期，见 pump#5）。
    {
        Action start;
        start.set = {a};  // start 只负责 arm A（⛔ 不在这里做 Edit / Input）
        Action fire;
        fire.do_read = true;
        fire.do_edit = true;
        fire.edit_mask = 0x10u;
        fire.do_readback = true;
        fire.submit = {intentOf(lift::plugin::IntentKind::Click, 100, 200)};
        fire.set = {c};
        f.handler.actions = {start, fire};
    }

    g_now_ms = 1000;
    LIFT_CHECK(f.begin());
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.runner.armedCount()), 1);

    const std::size_t q0 = f.wm.queueSize();
    f.readings.script = "SCENE1";
    f.pump();  // pump#1

    // ---- 回调里做了什么（读回 = 旧值 → Edit → 读回 = 新值）----
    const Action& a1 = f.handler.actions[1];
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.handler.triggerCalls()), 1);
    LIFT_CHECK(a1.read_ok);
    LIFT_CHECK_EQ_U32(a1.read_value, 0x1200u);   // ENC 往返：读回语义值
    LIFT_CHECK(a1.edit_receipt.accepted);        // Edit 成功
    LIFT_CHECK(a1.edit_receipt.code == nullptr);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(a1.edit_receipt.seq), 0);     // ★ Edit 不进队列
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(a1.edit_receipt.queued), 0);  // ★ 没有深度
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(a1.edit_receipt.id), 0);      // ★ 没有 id
    LIFT_CHECK(a1.readback_ok);
    LIFT_CHECK_EQ_U32(a1.readback_value, 0x1210u);  // ★ **同拍**读回 = |0x10（⛔ 不等读点）
    // 槽里已经是 ENC(0x1210)（Edit 走的就是 `writeGlobal` 那一份实现）。
    LIFT_CHECK_EQ_U32(rawSlot(f.mem), lift::encodeInt(0x1210u, kFxKey));

    // ---- trigger 的 Input **不在 fire 那拍落地**（staged ⇒ 下一读点）----
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(a1.receipts.size()), 1);
    LIFT_CHECK(a1.receipts[0].accepted);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(a1.receipts[0].seq), 1);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(a1.receipts[0].queued), 1);  // 回执立即给
    const std::uint64_t click_id = a1.receipts[0].id;
    LIFT_CHECK_NE_U32(static_cast<std::uint32_t>(click_id), 0u);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.queue.stagedDepth()), 1);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.queue.readyDepth()), 0);
    LIFT_CHECK(f.queue.status(click_id) == lift::plugin::IntentQueue::Status::Queued);
    // 本拍什么都没写 guest：没有 move / down、光标没动、也没有臂上的 click。
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.exec.armedPresses()), 0);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.exec.pendingReleases()), 0);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(countMsgs(f.wm, q0, lift::input::kWmMouseMove)), 0);

    // ---- pump#2：click 的**第一段**（映射 + move ⇒ 臂上；⛔ 还没按下）----
    g_now_ms = 1001;
    f.pump();
    int cx = 0;
    int cy = 0;
    f.cursor(&cx, &cy);
    LIFT_CHECK_EQ_I64(cx, 100);
    LIFT_CHECK_EQ_I64(cy, 200);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.exec.armedPresses()), 1);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.exec.pendingReleases()), 0);
    LIFT_CHECK_EQ_U32(f.wm.mouseButtons(), 0u);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(countMsgs(f.wm, q0, lift::input::kWmLButtonDown)), 0);
    LIFT_CHECK(f.queue.status(click_id) == lift::plugin::IntentQueue::Status::Applied);

    // ---- pump#3：还不到按下（闸门③ 的墙钟 250 ms 没到）----
    g_now_ms = 1002;
    f.pump();
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.exec.armedPresses()), 1);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.exec.pendingReleases()), 0);

    // ---- pump#4：**第二段**（按下；闸门③ 的墙钟兜底 250 ms 到期）----
    g_now_ms = 1251;
    f.pump();
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.exec.armedPresses()), 0);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.exec.pendingReleases()), 1);
    LIFT_CHECK_EQ_U32(f.wm.mouseButtons(), 0x1u);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(countMsgs(f.wm, q0, lift::input::kWmLButtonDown)), 1);

    // ---- pump#5：**同一拍**里"抬起到期" + "trigger 再 fire 一次并做 Edit"----
    //   ★ 这是"Edit 不占 Input 预算"的**可判形式**：抬起在 ④（releaseDue）落地，Edit 在 ③（poll）
    //   的同一个回调里发生；若 Edit 吃掉写预算，④ 的抬起会被挡掉 ⇒ `released()` 停在 0 ⇒ 红。
    //   回调里先记 `released()`：poll 在 releaseDue **之前** ⇒ 那一刻必须是 0。
    {
        Action act;
        act.do_read = true;         // 读回上一拍的 Edit 结果
        act.do_edit = true;         // 再做一次（|0x0100）
        act.edit_mask = 0x0100u;
        act.do_readback = true;
        act.record_released = true;
        act.set = {};               // [] = done（⛔ 不自动 finish）
        f.handler.actions.push_back(act);
    }
    g_now_ms = 1900;  // 按下在 1251 ⇒ 抬起兜底 500 ms 已到（poll 也正好在 C 的 900 ms 上）
    f.pump();
    const Action& a2 = f.handler.actions[2];
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.handler.triggerCalls()), 2);
    LIFT_CHECK_EQ_U32(a2.read_value, 0x1210u);   // 上一拍的 Edit 结果是**可见的**
    LIFT_CHECK(a2.edit_receipt.accepted);
    LIFT_CHECK_EQ_U32(a2.readback_value, 0x1310u);  // |0x0100 ⇒ 同拍读回
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(a2.released_seen), 0);  // ★ poll 在 releaseDue 前
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.exec.released()), 1);  // ★ 抬起仍在同拍落地
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.exec.pendingReleases()), 0);
    LIFT_CHECK_EQ_U32(f.wm.mouseButtons(), 0u);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(countMsgs(f.wm, q0, lift::input::kWmLButtonUp)), 1);
    LIFT_CHECK_EQ_U32(rawSlot(f.mem), lift::encodeInt(0x1310u, kFxKey));
    LIFT_CHECK(f.runner.done());
    LIFT_CHECK(!f.runner.failed());
}

// =====================================================================================
// ⑥ one-write-per-read-point：**staged 路径**（`ctx.input` 提交）——
//    两条 staged 落不同拍 / release·press 先占预算时 staged 等下一点 /
//    同按钮未抬起 ⇒ 停在队首（连它后面的也一起等）
// =====================================================================================
void caseOneWritePerReadPoint() {
    lift_test::g_current_case = "trigger-runner/one-write-per-read-point";
    ExecFixture f;
    LIFT_CHECK(f.mem_ok);
    LIFT_CHECK(f.win_ok);
    if (!f.mem_ok || !f.win_ok) return;

    // 每一拍都由 `R`（`AtReadPoint` + Reenter）回调驱动：它按 `actions[]` 提交 intent。
    // ★ 这样**所有**提交都走 `ctx.input->submit`（staged），⛔ 没有测试侧"直接落盘"的捷径。
    const Trigger r = f.factory.atReadPoint("R", InputPoint::Count, 100, TriggerMode::Reenter);

    f.handler.actions.resize(20);
    for (Action& action : f.handler.actions) action.set = {r};  // 每拍重新 arm 同一个对象
    // start：两条 move（staged ⇒ 第一拍提升、一读点一条 ⇒ 落不同拍）。
    f.handler.actions[0].submit = {intentOf(lift::plugin::IntentKind::Move, 10, 10),
                                  intentOf(lift::plugin::IntentKind::Move, 20, 20)};
    // 第 3 拍（fire#3）：提交一条 click（下一拍才臂上）。
    f.handler.actions[3].submit = {intentOf(lift::plugin::IntentKind::Click, 100, 200)};
    // 第 5 拍：提交 m3（下一拍正是"按下占预算"的那一拍）。
    f.handler.actions[5].submit = {intentOf(lift::plugin::IntentKind::Move, 30, 30)};
    // 第 8 拍：提交 m4（下一拍正是"抬起占预算"的那一拍）。
    f.handler.actions[8].submit = {intentOf(lift::plugin::IntentKind::Move, 40, 40)};
    // 第 10 拍：提交 clickB（Left）。
    f.handler.actions[10].submit = {intentOf(lift::plugin::IntentKind::Click, 200, 300)};
    // 第 12 拍：按下之后提交"同按钮的 clickC"+"它后面的 m5"。
    f.handler.actions[12].submit = {intentOf(lift::plugin::IntentKind::Click, 300, 400),
                                   intentOf(lift::plugin::IntentKind::Move, 50, 50)};

    g_now_ms = 1000;
    LIFT_CHECK(f.begin());

    int cx = 0;
    int cy = 0;

    // ---- 第 1 拍：m1 落地；m2 还在 ready_（一读点一条）----
    g_now_ms = 1000;
    f.pump();
    f.cursor(&cx, &cy);
    LIFT_CHECK_EQ_I64(cx, 10);
    LIFT_CHECK_EQ_I64(cy, 10);
    // ★ 预算记账：这一拍**真的**落了一条写 ⇒ `writesThisPoint()` 必须读到 1（恒 0 = executor 侧
    //   的写预算只是摆设 ⇒ C4 的第二条写路径会同拍写两条而不被 ⑥ 挡住）。
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.exec.writesThisPoint()), 1);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.queue.readyDepth()), 1);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.queue.stagedDepth()), 0);
    // 回执是**立即**给的，且 `queued` 就是提交那一刻的深度。
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.handler.actions[0].receipts.size()), 2);
    LIFT_CHECK(f.handler.actions[0].receipts[0].accepted);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.handler.actions[0].receipts[0].queued), 1);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.handler.actions[0].receipts[1].queued), 2);
    const std::uint64_t m1_id = f.handler.actions[0].receipts[0].id;
    LIFT_CHECK(f.queue.status(m1_id) == lift::plugin::IntentQueue::Status::Applied);
    LIFT_CHECK(f.queue.status(9999) == lift::plugin::IntentQueue::Status::Unknown);  // 从没分配过

    // ---- 第 2 拍：m2 落地（⇒ 两条 staged 确实落**不同**拍）----
    g_now_ms = 1001;
    f.pump();
    f.cursor(&cx, &cy);
    LIFT_CHECK_EQ_I64(cx, 20);
    LIFT_CHECK_EQ_I64(cy, 20);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.queue.depth()), 0);

    // ---- 第 3 拍：提交 clickA（staged ⇒ 本拍不落地）----
    g_now_ms = 1100;
    f.pump();
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.queue.readyDepth()), 0);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.queue.stagedDepth()), 1);

    // ---- 第 4 拍：clickA 的**第一段**（臂上 + move）----
    g_now_ms = 1150;
    f.pump();
    f.cursor(&cx, &cy);
    LIFT_CHECK_EQ_I64(cx, 100);
    LIFT_CHECK_EQ_I64(cy, 200);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.exec.armedPresses()), 1);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.exec.pendingReleases()), 0);

    // ---- 第 5 拍：提交 m3（下一拍是"按下占预算"的那拍）----
    g_now_ms = 1200;
    f.pump();
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.queue.stagedDepth()), 1);

    // ---- 第 6 拍：**按下**先占预算 ⇒ staged 的 m3 等下一拍（光标不动）----
    g_now_ms = 1449;  // 臂上在 1150 ⇒ 按下兜底 250 ms 早到期
    f.pump();
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.exec.pendingReleases()), 1);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.queue.readyDepth()), 1);  // ★ 还在队首
    f.cursor(&cx, &cy);
    LIFT_CHECK_EQ_I64(cx, 100);  // ★ 没被施加
    LIFT_CHECK_EQ_I64(cy, 200);

    // ---- 第 7 拍：下一个读点才落地 ----
    g_now_ms = 1450;
    f.pump();
    f.cursor(&cx, &cy);
    LIFT_CHECK_EQ_I64(cx, 30);
    LIFT_CHECK_EQ_I64(cy, 30);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.queue.depth()), 0);

    // ---- 第 8 拍：提交 m4（下一拍是"抬起占预算"的那拍）----
    g_now_ms = 1948;  // 按下在 1449 ⇒ 抬起兜底 500 ms 在 1949
    f.pump();
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.queue.stagedDepth()), 1);

    // ---- 第 9 拍：**抬起**先占预算 ⇒ staged 的 m4 等下一拍 ----
    g_now_ms = 1949;
    f.pump();
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.exec.pendingReleases()), 0);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.queue.readyDepth()), 1);  // ★ 还在队首
    f.cursor(&cx, &cy);
    LIFT_CHECK_EQ_I64(cx, 30);

    // ---- 第 10 拍：m4 落地；同时提 clickB ----
    g_now_ms = 1950;
    f.pump();
    f.cursor(&cx, &cy);
    LIFT_CHECK_EQ_I64(cx, 40);
    LIFT_CHECK_EQ_I64(cy, 40);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.queue.stagedDepth()), 1);  // clickB

    // ---- 第 11 拍：clickB 臂上 ----
    g_now_ms = 2000;
    f.pump();
    f.cursor(&cx, &cy);
    LIFT_CHECK_EQ_I64(cx, 200);
    LIFT_CHECK_EQ_I64(cy, 300);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.exec.armedPresses()), 1);

    // ---- 第 12 拍：clickB 按下；同时提交 clickC + m5（下一拍才 ready）----
    g_now_ms = 2250;  // 臂上在 2000 ⇒ 250 ms
    f.pump();
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.exec.pendingReleases()), 1);
    LIFT_CHECK_EQ_U32(f.wm.mouseButtons(), 0x1u);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.queue.stagedDepth()), 2);

    // ---- 第 13 拍：队首 clickC 的**同按钮还在按着** ⇒ 停在队首（后面 m5 也一起等）----
    g_now_ms = 2251;
    f.pump();
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.queue.readyDepth()), 2);  // ★ 一条都没落
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.exec.armedPresses()), 0);
    f.cursor(&cx, &cy);
    LIFT_CHECK_EQ_I64(cx, 200);  // ★ 队首没被施加 ⇒ 光标没去 (300,400)
    LIFT_CHECK_EQ_I64(cy, 300);

    // ---- 第 14 拍：抬起（500 ms）落地，但本拍写了 ⇒ 队首还等一拍 ----
    g_now_ms = 2750;
    f.pump();
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.exec.pendingReleases()), 0);
    LIFT_CHECK_EQ_U32(f.wm.mouseButtons(), 0u);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.queue.readyDepth()), 2);  // 抬起占了预算

    // ---- 第 15 拍：clickC 才臂上；m5 仍在它后面（队首头阻）----
    g_now_ms = 2751;
    f.pump();
    f.cursor(&cx, &cy);
    LIFT_CHECK_EQ_I64(cx, 300);
    LIFT_CHECK_EQ_I64(cy, 400);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.queue.readyDepth()), 1);  // ★ m5 还在后面
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.exec.armedPresses()), 1);
}

// =====================================================================================
// ⑦ owner-timeout：deadline / `>=` / **无 atPoint 不超时（结构性）** / 移除 + TimeoutEvent /
//    per-arm 重置 / 不自动 finish / maxFires 不增 / releaseOwner 不残留按住 /
//    **poll 在 releaseDue 之前**
// =====================================================================================
void caseOwnerTimeout() {
    lift_test::g_current_case = "trigger-runner/owner-timeout";
    ExecFixture f;
    LIFT_CHECK(f.mem_ok);
    LIFT_CHECK(f.win_ok);
    if (!f.mem_ok || !f.win_ok) return;

    // T1：脚本永不匹配 + timeout 100 ms（Once）。arm 在 1000 ⇒ deadline 1100。
    const Trigger t1 = f.factory.afterScript("T1", "NEVER", TriggerMode::Once, 0, 100);
    const Trigger t2 = f.factory.afterScript("T2", "NEVER", TriggerMode::Once, 0, 100);
    const Trigger t3 = f.factory.afterScript("T3", "NEVER", TriggerMode::Once, 0, 100);
    const Trigger t4 = f.factory.afterScript("T4", "NEVER", TriggerMode::Once, 0, 100000);
    // V：deadline 会与"抬起兜底"落在同一拍（poll-order 的判据那一段用）。
    const Trigger v = f.factory.afterScript("V", "NEVER", TriggerMode::Once, 0, 750);

    f.handler.actions.resize(6);
    f.handler.actions[0].set = {t1};  // start#1
    f.handler.actions[1].set = {t2};  // timeout#1 的回调：换成新对象（per-arm 重算 deadline）
    f.handler.actions[2].set = {t3};  // timeout#2 的回调
    f.handler.actions[3].set = {t4};  // start#2（"无 atPoint 不超时"那一段）
    f.handler.actions[4].set = {v};   // start#3（poll-order 那一段的那个 timeout trigger）
    f.handler.actions[5].record_released = true;  // timeout#3 的回调：记 poll 那一刻的 released()

    f.readings.script = "SCENE1";
    g_now_ms = 1000;
    LIFT_CHECK(f.begin());
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.runner.armedCount()), 1);

    // ---- `>=`：deadline **前一刻**不超时，**正好到点**超时（不是"超过才超"）----
    g_now_ms = 1099;
    f.pump();
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.handler.triggerCalls()), 0);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.runner.armedCount()), 1);

    g_now_ms = 1100;
    f.pump();
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.handler.triggerCalls()), 1);
    const TriggerEvent& ev1 = f.handler.events[0];
    LIFT_CHECK(ev1.what == TriggerEvent::What::TimedOut);
    LIFT_CHECK(std::string(ev1.kind()) == "timeout");
    LIFT_CHECK(ev1.name == "T1");
    // 移除 + maxFires 不增 + 不自动 finish
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.runner.armedCount()), 1);  // 只剩回调放回的 T2
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.runner.firesOf(t1)), 0);   // ★ timeout 不是 fire
    LIFT_CHECK(!f.runner.done());                                            // ★ 不自动 finish
    LIFT_CHECK(!f.runner.failed());

    // ---- per-arm 重置：T2 的 deadline = 1100 + 100 = 1200 ----
    g_now_ms = 1150;  // 旧的 deadline(1100) 早过了，但新的没过
    f.pump();
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.handler.triggerCalls()), 1);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.runner.armedCount()), 1);
    g_now_ms = 1200;
    f.pump();
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.handler.triggerCalls()), 2);
    LIFT_CHECK(f.handler.events[1].name == "T2");
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.runner.firesOf(t2)), 0);

    // ---- ★ 无 `atPoint` ⇒ 不超时（**结构性**：没有读点就没有 poll）----
    //   ⛔ 这不是"headless ⇒ 不超时"（那类恒真断言）：这里把钟推进到 deadline 之后，
    //   但**一次也不 pump** ⇒ 计数 / armed / 事件全部不动；随后一 pump 就立刻超时。
    g_now_ms = 3000;
    LIFT_CHECK(f.begin());  // start#2 ⇒ T4（deadline 103000，故意很远）
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.runner.armedCount()), 1);
    const std::int64_t calls_before = static_cast<std::int64_t>(f.handler.triggerCalls());
    const std::uint64_t reads_before = f.exec.readPoints();
    g_now_ms = 9000;  // 墙钟推进 6 s，但**没有读点**
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.handler.triggerCalls()), calls_before);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.runner.armedCount()), 1);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.exec.readPoints()), static_cast<std::int64_t>(reads_before));
    LIFT_CHECK(!f.runner.done());
    f.pump();  // ★ 一有读点就 poll（T4 的 deadline 还远 ⇒ 仍然没有事件）
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.handler.triggerCalls()), calls_before);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.runner.armedCount()), 1);

    // ---- releaseOwner：不残留按住（up 真的 post；只动该 owner；runner 那一侧也抹）----
    //   先把一条 click 按下去（owner = kFxOwner），再在队列里放两条（一个本 owner、一个别人的）。
    const lift::plugin::Receipt click_receipt =
        f.queue.submit(intentOf(lift::plugin::IntentKind::Click, 100, 200, kFxOwner));
    LIFT_CHECK(click_receipt.accepted);
    g_now_ms = 4000;
    f.pump();  // 臂上
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.exec.armedPresses()), 1);
    g_now_ms = 4300;
    f.pump();  // 按下
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.exec.pendingReleases()), 1);
    LIFT_CHECK_EQ_U32(f.wm.mouseButtons(), 0x1u);
    const std::size_t q1 = f.wm.queueSize();
    const lift::plugin::Receipt mine =
        f.queue.submit(intentOf(lift::plugin::IntentKind::Move, 11, 11, kFxOwner));
    const lift::plugin::Receipt other =
        f.queue.submit(intentOf(lift::plugin::IntentKind::Move, 22, 22, kFxOwner + 1));
    LIFT_CHECK(mine.accepted);
    LIFT_CHECK(other.accepted);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.runner.armedCount()), 1);  // T4 还 armed（owner 7）

    const std::size_t released = f.exec.releaseOwner(f.wm, kFxOwner);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(released), 1);  // 真的抬起了一条
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.exec.pendingReleases()), 0);   // ★ 不残留按住
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.exec.armedPresses()), 0);
    LIFT_CHECK_EQ_U32(f.wm.mouseButtons(), 0u);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(countMsgs(f.wm, q1, lift::input::kWmLButtonUp)), 1);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.runner.armedCount()), 0);  // ★ runner 那一侧也抹了
    LIFT_CHECK(!f.runner.done());                                           // ⛔ 不置 done
    LIFT_CHECK(!f.runner.failed());
    // 队列：只丢本 owner 的那条；别人的留着（C5 多 owner 的前提）
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.queue.depth()), 1);
    LIFT_CHECK(f.queue.status(mine.id) == lift::plugin::IntentQueue::Status::Dropped);
    LIFT_CHECK(f.queue.status(other.id) == lift::plugin::IntentQueue::Status::Queued);

    // 别的那条照旧能落地（releaseOwner 没把整条队列带走）
    g_now_ms = 4301;
    f.pump();
    int cx = 0;
    int cy = 0;
    f.cursor(&cx, &cy);
    LIFT_CHECK_EQ_I64(cx, 22);
    LIFT_CHECK_EQ_I64(cy, 22);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.queue.depth()), 0);

    // ---- ★ "poll 在 releaseDue 之前"的**可判形式**（timeout 事件这条腿）----
    //   V 的 deadline（arm 在 5000 + 750）与 click2 的抬起兜底（按下在 5250 + 500 = 5750）**同一拍**：
    //   回调里 `released()` 必须还是**旧值**（poll 先跑），pump 返回后必须 +1（releaseDue 后跑）。
    g_now_ms = 5000;
    LIFT_CHECK(f.begin());
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.runner.armedCount()), 1);

    const lift::plugin::Receipt click2 =
        f.queue.submit(intentOf(lift::plugin::IntentKind::Click, 300, 300, kFxOwner));
    LIFT_CHECK(click2.accepted);
    f.pump();  // 臂上（arm_ms = 5000）
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.exec.armedPresses()), 1);
    g_now_ms = 5250;
    f.pump();  // 按下（press_ms = 5250 ⇒ 抬起兜底 5750）
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.exec.pendingReleases()), 1);
    const std::uint64_t released_before = f.exec.released();  // 上一段 releaseOwner 抬起的那条
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(released_before), 1);

    g_now_ms = 5750;  // V 的 deadline 与抬起兜底**同一拍**
    f.pump();
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.handler.triggerCalls()),
                      static_cast<std::int64_t>(calls_before) + 1);
    LIFT_CHECK(f.handler.events.back().what == TriggerEvent::What::TimedOut);
    LIFT_CHECK(f.handler.events.back().name == "V");
    // ★ poll 在 releaseDue **之前**：回调里看到的是旧值……
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.handler.actions[5].released_seen),
                      static_cast<std::int64_t>(released_before));
    // ……而 pump 返回之后抬起**仍然**在同一拍落地（+1）。
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.exec.released()),
                      static_cast<std::int64_t>(released_before + 1));
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.exec.pendingReleases()), 0);
    LIFT_CHECK_EQ_U32(f.wm.mouseButtons(), 0u);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.runner.firesOf(v)), 0);
}

}  // namespace

int main() {
    caseAfterPositionOnce();
    caseAfterScriptOnce();
    caseNamedSet();
    caseRearm();
    caseGlobalReadModifyWriteClick();
    caseOneWritePerReadPoint();
    caseOwnerTimeout();
    return lift_test::finish("trigger-runner");
}
