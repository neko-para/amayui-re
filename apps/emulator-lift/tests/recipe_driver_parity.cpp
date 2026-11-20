// emulator-lift · **C3** 的 parity 判据：旧执行器（`InputPlan`，**生产真源**）vs JS driver
// （`scripts/plugins/lib/recipe.js` 的 `runRecipe`，**只用于离线 parity**）
//
// 合同：`docs/03-emulator/plugin-runtime.md` §5 C3（`:1162-1206`）；母单
//       `data/requirements/01M7T0VJPG2S0RZ1HNWW0HH3HY.md:51`（"JS recipe driver + parity（`InputPlan` 保留）"）。
//
// ★ 为什么这是**唯一**能证 parity 的地方：oracle 与 driver 只有在这里同时在场
//   （`InputPlan::atPoint` vs `PluginHost::onReadPoint`）。判据逐条按合同 `:1175-1177`：
//     ① 消息（`message` / `wParam` / `lParam`）**逐条**相同（队列指纹，⛔ 不比"画面动了"）；
//     ② 电平（`wm.mouseButtons()`）与光标（`wm.cursorClientPos`）相同；
//     ③ `fired`（按下消息数 vs oracle 的 `fired()`）相同；
//     ④ 完成 / 退出码**语义**相同（`Done` ⇒ 0；门超时 ⇒ 5；`quit` 截断 ⇒ 5）；
//     ⑤ `stepText` / 进度：driver arm 的那一步（trigger 的 kind/script/lo/hi/delay/timeout）必须与
//        oracle 的 `next_` 那一步**同一个**（`driverNext == oracle.stepsApplied()` 时比对）；
//     ⑥ profile 的 HostEffect 序列（begin 的 label + end 的条数）相同。
//   ★ **只承诺"语义等价 + 已知差异表"**（合同 `:1200`）：⛔ 不承诺逐位、⛔ 不宣称全覆盖。
//     已知差异由 `printf` 的 `[recipe-parity] diff:` 行列出（本文件末尾的 `kKnownDiffs` 是**同一份**文本，
//     用例会把它打出来 ⇒ 报告与代码不两处维护）。
//
// ★ 夹具口径照 `tests/plugin_runtime.cpp:52-55`（**零 `Machine`、零 GPU、零真窗口、零游戏安装**）：
//   一块假 `GuestMemory`（只摆 `MarkerSampler` 要的那几格）+ 默认 `WindowManager` + 注入读数源。
//   oracle 的身份 / 位置从**假 Engine 内存**读（`MarkerSampler` + `readScriptPositionSample`），
//   driver 的身份 / 位置从**同一个世界**（`FakeReadings`）读 ⇒ 两侧在**每个读点**看到同一组读数。
//   ⇒ `recipe-driver-parity` 注册在**顶层**（`AGENTS.md` §12.4 ⇒ pure 档）。
//
// ★ `Step[]` 怎么进 JS（合同 `:1166` 的"JSON/ABI"；**⛔ 不给 `ctx` 加成员**）：
//   1. `recipe_json.cpp` 的 `stepsJson()` 是 `Step[]` 的**唯一** JSON 出口；
//   2. 本文件把它写成 `recipe-steps.js`（`export default <那一个 JSON 对象>`），
//      与 `scripts/plugins/lib/recipe.js` 的**逐字节副本**一起放进一个 run 私有临时目录；
//   3. 入口 = `tests/plugin_fixtures/recipe-driver.js`（`origin` 指到那个临时目录 ⇒ 相对 import 落在这里）。
//   ⛔ 为什么不能按"`ctx.params` 里放一个 JSON 文件路径、driver 自己读"：沙箱里**没有文件读 API**
//     也没有 `JSON`（`src/ui/script_quickjs.cpp:9-22` 的 `JS_NewContextRaw` + 只加 BaseObjects；
//     同文件 `:1965` 的 D-C2-7 原话"沙箱里**没有 `JSON`**"），而 import 只收相对路径且必须落在
//     入口目录之下（`:1234-1250`）⇒ 模块装载是唯一"不新增 API 又能读宿主文件"的通道。
//     这是**实现约束**，不是取舍（见交付报告 / 待父级裁决）。
//
// ★ ⛔ 不碰冻结面：不改 `recipe.h/.cpp`（parser / `StepKind` / `stepWindowExpired`）、不改
//   `input_plan.*`、不改 `plugin_host.*`、不给 `ctx` 加成员（`plugin/api-surface` 是那条的机械判据）。
// ★ ⛔ 不 assert 墙钟（§12.6）：两侧共用**同一个**注入时钟（`setClockForTest`，与
//   `tests/input_plan.cpp:185-186` 同一条"不真睡"的口径），`wait` 只按**规则**断言（"到期 ⇒ 走"）。
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "check.h"
#include "plugin/plugin_host.h"
#include "runtime/engine_marker.h"
#include "runtime/host_window.h"
#include "runtime/input_plan.h"
#include "runtime/memory.h"
#include "runtime/recipe.h"
#include "runtime/recipe_json.h"
#include "runtime/vfs.h"
#include "ui/script.h"  // UiHostOps（`ctx.profile.*` 的 HostEffect 桥）

namespace {

namespace fs = std::filesystem;

// =====================================================================================
// 假 Engine（形状与 `tests/input_plan.cpp:188-223` 同源；⛔ 不需要 `Machine`）
// =====================================================================================
constexpr std::uint32_t kEngineBase = 0x00480000u;
constexpr std::uint32_t kEngineSize = 0x00100000u;  // [0x480000, 0x580000)
constexpr std::uint32_t kNameBase = 0x004A0000u;
constexpr std::uint32_t kCodeBase = 0x004C0000u;
constexpr std::uint32_t kFrameBase = kEngineBase + lift::kEngineFrameBase;
constexpr std::uint32_t kCurEa = kEngineBase + lift::kEngineCurOffset;
constexpr std::uint32_t kFileDbEa = kEngineBase + lift::kFileIndexOffset;

// 中性世界的 pos：**有效**（≥ header_len，pc = base）但落在任何门区间之外。
constexpr std::uint32_t kNeutralPos = 0x0100u;
// 每拍推进的（注入）墙钟毫秒。★ 只影响"门超时 / `wait` 何时到期"这类**规则**，
//   ⛔ 不与真机时序 / 墙钟读数挂钩（§12.6）。
constexpr std::uint64_t kTickMs = 50;
// 单条配方的读点上限（防夹具自己死循环；⛔ 不是性能判据）。
constexpr std::size_t kMaxFrames = 60000;
// `WM_LBUTTONDOWN`（与 `tests/input_plan.cpp:70` 同一个字面量；Win32 公开值）。
constexpr std::uint32_t kWmLButtonDown = 0x0201u;

std::uint64_t g_now_ms = 1000;
std::uint64_t parityNowMs() { return g_now_ms; }

struct ScriptTable {
    std::vector<std::string> names;
    std::map<std::string, std::uint32_t> ids;

    void add(const std::string& name) {
        if (ids.find(name) != ids.end()) return;
        ids[name] = static_cast<std::uint32_t>(names.size());
        names.push_back(name);
    }
    bool idOf(const std::string& name, std::uint32_t* out) const {
        const auto it = ids.find(name);
        if (it == ids.end()) return false;
        *out = it->second;
        return true;
    }
};

bool put32(lift::GuestMemory& mem, std::uint32_t ea, std::uint32_t v) {
    if (!mem.committed(ea, 4u)) return false;
    mem.wr32(ea, v);
    return true;
}

// 摆一条"身份 = id、名字表 = table"的假 Engine（与 `tests/input_plan.cpp` 的 publishEngine 同形）。
bool publishEngine(lift::GuestMemory& mem, const ScriptTable& table) {
    if (!mem.reserve()) return false;
    if (!mem.commitZeroed(kEngineBase, kEngineSize, lift::RegionKind::Heap, "recipe-parity-engine")) {
        std::fprintf(stderr, "  commitZeroed 失败：%s\n", mem.lastError());
        return false;
    }
    bool ok = put32(mem, lift::kEngineSingletonGlobal, kEngineBase) && put32(mem, kCurEa, 0u) &&
              put32(mem, kFrameBase, kCodeBase) &&  // 记录 +0x00 = 已提交的代码区基址
              put32(mem, kFileDbEa + lift::kFileRootCountOffset,
                    static_cast<std::uint32_t>(table.names.size())) &&
              put32(mem, kFileDbEa + lift::kFileRootNameBaseOffset, kNameBase);
    for (std::size_t i = 0; ok && i < table.names.size(); ++i) {
        const std::string& name = table.names[i];
        const std::uint32_t entry = kNameBase + lift::kFileNameStride * static_cast<std::uint32_t>(i);
        const std::size_t max = lift::kFileNameMaxBytes - 1u;
        const std::size_t n = name.size() < max ? name.size() : max;
        for (std::size_t k = 0; k < n; ++k) {
            mem.wr8(entry + static_cast<std::uint32_t>(k), static_cast<std::uint8_t>(name[k]));
        }
        mem.wr8(entry + static_cast<std::uint32_t>(n), 0u);
    }
    return ok;
}

// 本读点那个世界：身份（marker 名字）+ 脚本内位置。
struct World {
    std::string script;
    std::uint32_t pos = kNeutralPos;
};

// 把世界写进**oracle 的**假 Engine（driver 那侧由 `FakeReadings` 给同一组值）。
bool setWorld(lift::GuestMemory& mem, const ScriptTable& table, const World& w) {
    std::uint32_t id = 0;
    if (!table.idOf(w.script, &id)) return false;
    return put32(mem, kFrameBase + lift::kFrameIdentityOffset, id) &&
           put32(mem, kFrameBase + lift::kFramePcOffset, kCodeBase + (w.pos - lift::kScriptHeaderLen));
}

// ---- 注入的读数源（driver 侧；C1 的测试缝，`TriggerReadings` 是纯值）----
struct FakeReadings final : lift::plugin::ReadingsSource {
    World world;

    lift::plugin::TriggerReadings sample(lift::InputPoint point) override {
        lift::plugin::TriggerReadings r;
        r.point = point;
        r.script = world.script;
        r.script_ok = !world.script.empty();
        r.pos = world.pos;
        r.pos_ok = true;
        return r;
    }
};

// =====================================================================================
// 宿主文件读写 / run 私有临时目录
// =====================================================================================
bool readTextFile(const fs::path& path, std::string* out) {
    std::FILE* f = lift::openHostFileUtf8(path.string(), "rb");
    if (f == nullptr) return false;
    out->clear();
    char buf[8192];
    std::size_t n = 0;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out->append(buf, n);
    const bool bad = std::ferror(f) != 0;
    std::fclose(f);
    return !bad;
}

bool writeTextFile(const fs::path& path, const std::string& text) {
    std::FILE* f = lift::openHostFileUtf8(path.string(), "wb");
    if (f == nullptr) return false;
    const bool ok = std::fwrite(text.data(), 1, text.size(), f) == text.size();
    std::fclose(f);
    return ok;
}

// run 私有目录（`fs::temp_directory_path()` 之下；析构时删掉 ⇒ ⛔ 不污染工作树）。
class ScopedDir {
public:
    ScopedDir() = default;
    ~ScopedDir() { removeNow(); }
    ScopedDir(const ScopedDir&) = delete;
    ScopedDir& operator=(const ScopedDir&) = delete;

    bool make(const std::string& tag) {
        static std::uint32_t seq = 0;
        ++seq;
        std::error_code ec;
        path_ = fs::temp_directory_path(ec) / "lift-recipe-parity";
        if (ec) return false;
        path_ /= tag + "-" + std::to_string(seq);
        fs::remove_all(path_, ec);
        ec.clear();
        fs::create_directories(path_, ec);
        return !ec;
    }
    const fs::path& path() const { return path_; }
    void removeNow() {
        if (path_.empty()) return;
        std::error_code ec;
        fs::remove_all(path_, ec);
        path_.clear();
    }

private:
    fs::path path_;
};

// =====================================================================================
// 两个夹具
// =====================================================================================
struct Oracle {
    lift::GuestMemory mem;
    bool mem_ok = false;
    lift::WindowManager wm;
    std::uint32_t hwnd = 0;
    lift::InputPlan plan;
    std::vector<std::string> profile;  // "begin:<label>" / "end"
};

void oracleProfileHook(void* ctx, const char* label, bool begin) {
    auto* v = static_cast<std::vector<std::string>*>(ctx);
    std::string line = begin ? "begin:" : "end";
    if (begin) line += (label != nullptr ? label : "");
    v->push_back(std::move(line));
}

struct Driver {
    lift::GuestMemory mem;
    bool mem_ok = false;
    lift::WindowManager wm;
    std::uint32_t hwnd = 0;
    FakeReadings readings;
    std::vector<std::string> profile;  // "begin:<label>" / "end"
    lift::ui::UiHostOps ops;
    std::unique_ptr<lift::plugin::PluginHost> host;
    bool loaded = false;
    std::string load_error;
};

bool driverProfileBegin(void* ctx, const char* label, std::string* err) {
    (void)err;
    auto* v = static_cast<std::vector<std::string>*>(ctx);
    // ★ `ProfileApi.end()` 没有 label（`plugin.d.ts:272-275`）⇒ end 那一行不带 label（已知差异 E5）。
    v->push_back(std::string("begin:") + (label != nullptr ? label : ""));
    return true;
}

bool driverProfileEnd(void* ctx, std::string* text, std::string* err) {
    (void)err;
    static_cast<std::vector<std::string>*>(ctx)->push_back("end");
    if (text != nullptr) text->clear();  // ↑ 空文本 ⇒ `PluginHost::profileEnd` 不往 stdout 打报告
    return true;
}

// =====================================================================================
// 读点脚本（**世界是读点下标的纯函数** ⇒ 两侧逐点看到同一组读数；⛔ 不按"消费进度"自适应）
// =====================================================================================
struct Frame {
    World world;
    lift::InputPoint point = lift::InputPoint::PeekMsg;
};

std::vector<Frame> buildFrames(const std::vector<lift::Step>& steps, bool never_match) {
    const lift::InputPoint cycle[] = {lift::InputPoint::CursorPos, lift::InputPoint::KeyLevel,
                                      lift::InputPoint::PeekMsg, lift::InputPoint::CursorPos};
    std::vector<Frame> frames;
    std::size_t ci = 0;
    const World idle{"IDLE", kNeutralPos};
    auto push = [&](const World& w) {
        frames.push_back(Frame{w, cycle[ci % 4]});
        ++ci;
    };
    // 开头两拍：身份中性（`start` 在第一拍之后 arm 第一道门）。
    push(idle);
    push(idle);
    for (const lift::Step& s : steps) {
        if (s.isGate()) {
            World w = idle;
            if (!never_match) {
                w.script = s.script;
                // `await position`：区间中点（**含** FROM、**不含** TO 的半开区间内部）；
                // `await script`：位置与门无关（给一个有效但区间外的值）。
                w.pos = s.kind == lift::StepKind::AwaitPosition
                            ? s.pos_from + (s.pos_to - s.pos_from) / 2u
                            : kNeutralPos;
            }
            for (int i = 0; i < 4; ++i) push(w);  // 4 拍：吸收 driver 的一拍 arm 延迟
            push(idle);                           // 中性一拍：区分"这一道门真的成立过"
            continue;
        }
        if (s.kind == lift::StepKind::Wait) {
            const std::uint64_t n = s.wait_ms / kTickMs + 3;
            for (std::uint64_t i = 0; i < n; ++i) push(idle);
            continue;
        }
        if (s.isInputWrite()) {
            for (int i = 0; i < 4; ++i) push(idle);  // move → 按下 → 抬起 各自要一拍
            continue;
        }
        push(idle);  // profile_* / quit：一拍
    }
    for (int i = 0; i < 8; ++i) push(idle);  // 收尾（挂起的写落地 / 抬起 / 会话收口）
    return frames;
}

// =====================================================================================
// 一次 parity 的读数
// =====================================================================================
struct ParityResult {
    std::size_t steps = 0;
    bool mouse_button_steps = false;
    bool oracle_ok = false;
    bool driver_loaded = false;
    bool driver_failed = false;
    std::string driver_fail;
    std::string driver_finish_reason;
    int driver_finish_code = -1;
    bool driver_finished = false;
    std::int64_t oracle_applied = 0;
    std::int64_t driver_applied = 0;
    std::int64_t oracle_exit = 5;
    std::int64_t driver_exit = 5;
    bool oracle_quit = false;
    bool driver_quit = false;
    std::string oracle_queue;
    std::string driver_queue;
    std::string oracle_profile;
    std::string driver_profile;
    int oracle_x = 0;
    int oracle_y = 0;
    int driver_x = 0;
    int driver_y = 0;
    std::uint32_t oracle_buttons = 0;
    std::uint32_t driver_buttons = 0;
    std::uint64_t oracle_fired = 0;
    std::uint64_t oracle_downs = 0;
    std::uint64_t driver_downs = 0;
    std::int64_t translation_checks = 0;
    std::int64_t translation_mismatch = 0;
    bool driver_regressed = false;
    bool driver_ahead = false;
};

std::string queueFingerprint(lift::WindowManager& wm) {
    std::string out;
    char buf[64];
    for (std::size_t i = 0; i < wm.queueSize(); ++i) {
        lift::WinMessage m;
        if (!wm.peekAt(i, &m)) continue;
        std::snprintf(buf, sizeof(buf), "%04X/%04X/%08X;", static_cast<unsigned>(m.message),
                      static_cast<unsigned>(m.wparam), static_cast<unsigned>(m.lparam));
        out += buf;
    }
    return out;
}

std::size_t countMessage(lift::WindowManager& wm, std::uint32_t message) {
    std::size_t n = 0;
    for (std::size_t i = 0; i < wm.queueSize(); ++i) {
        lift::WinMessage m;
        if (wm.peekAt(i, &m) && m.message == message) ++n;
    }
    return n;
}

std::string joinLines(const std::vector<std::string>& v) {
    std::string out;
    for (const std::string& s : v) {
        out += s;
        out += ';';
    }
    return out;
}

// driver 的**进度**（`applied`；合同 `:1174` "driver 自己维护 applied/pending"）。
//   · 没到终态 ⇒ 它 arm 的那一步的下标（`recipe:<index>`；pending = steps − applied）；
//   · 终态（`ctx.finish`）⇒ 驱动自己记在收口原因里的 `applied=N`。
//     ★ **必须读它**：`done()` 不代表"全部消费" —— `quit` 之后还有指令时 runner 也 done，
//     但那时 applied < steps（那条路径正是 `exit 5` 的语义来源）。
std::int64_t driverProgress(const Driver& d, std::int64_t total) {
    if (!d.loaded || d.host == nullptr) return 0;
    if (d.host->finished()) {
        const std::string& reason = d.host->finishReason();
        const std::string needle = "applied=";
        const std::size_t at = reason.find(needle);
        if (at != std::string::npos) {
            const std::int64_t n = static_cast<std::int64_t>(
                std::strtoll(reason.c_str() + static_cast<std::ptrdiff_t>(at + needle.size()), nullptr, 10));
            if (n >= 0) return n;
        }
        return total;
    }
    if (d.host->failed()) return 0;  // 失败 ⇒ 进度不再是"消费到哪一步"（只作读数）
    if (d.host->done()) return total;
    std::int64_t best = total;
    for (const lift::plugin::Trigger& t : d.host->runner().armed()) {
        if (t.name.rfind("recipe:", 0) != 0) continue;
        const std::int64_t index = std::strtoll(t.name.c_str() + 7, nullptr, 10);
        if (index < best) best = index;
    }
    return best;
}

// `stepText` 那一档的 parity：driver arm 的 trigger 必须就是 oracle 卡住的那一步的**翻译**。
bool triggerMatchesStep(const lift::plugin::Trigger& t, const lift::Step& s, std::int64_t index) {
    if (t.name != ("recipe:" + std::to_string(static_cast<long long>(index)))) return false;
    switch (s.kind) {
        case lift::StepKind::AwaitScript:
            return t.kind == lift::plugin::TriggerKind::AfterScript && t.script == s.script &&
                   t.timeout_ms == s.timeout_ms;
        case lift::StepKind::AwaitPosition:
            return t.kind == lift::plugin::TriggerKind::AfterPosition && t.script == s.script &&
                   t.lo == s.pos_from && t.hi == s.pos_to && t.timeout_ms == s.timeout_ms;
        case lift::StepKind::Wait:
            return t.kind == lift::plugin::TriggerKind::AfterDelay && t.delay_ms == s.wait_ms;
        default:
            return true;  // 写 / profile_* / quit 不 arm（由 driver 自己的链消费）
    }
}

// =====================================================================================
// 一条配方的 parity 跑法
//   `never_match` = 世界里**没有**任何门能成立（只给 `gate-timeout` 用例：要那一侧真的到期）
// =====================================================================================
bool runParity(const fs::path& plan_path, const std::string& tag, ParityResult* out,
               bool never_match = false) {
    lift::Recipe recipe;
    std::string err;
    if (!recipe.loadFile(plan_path.string(), &err)) {
        lift_test::failf(__FILE__, __LINE__, "配方解析失败：%s（%s）", err.c_str(), plan_path.string().c_str());
        return false;
    }
    const std::vector<lift::Step>& steps = recipe.steps();
    out->steps = steps.size();
    for (const lift::Step& s : steps) {
        if (s.kind == lift::StepKind::MouseDown || s.kind == lift::StepKind::MouseUp) out->mouse_button_steps = true;
    }

    // ---- 世界需要的名字表（门点名的脚本 + 中性名）----
    ScriptTable table;
    for (const lift::Step& s : steps) {
        if (s.isGate()) table.add(s.script);
    }
    table.add("IDLE");

    // ---- run 私有模块目录（`recipe.js` 的逐字节副本 + 生成的 steps 模块）----
    ScopedDir dir;
    if (!dir.make(tag)) {
        lift_test::failf(__FILE__, __LINE__, "建不出临时目录（%s）", tag.c_str());
        return false;
    }
    std::string lib_source;
    if (!readTextFile(fs::path(LIFT_PLUGIN_LIB_DIR) / "recipe.js", &lib_source)) {
        lift_test::failf(__FILE__, __LINE__, "读不到 %s/recipe.js", LIFT_PLUGIN_LIB_DIR);
        return false;
    }
    if (!writeTextFile(dir.path() / "recipe.js", lib_source) ||
        !writeTextFile(dir.path() / "recipe-steps.js",
                       "// 生成物：`src/runtime/recipe_json.cpp` 的 `stepsJson()`（parity 夹具运行期生成）\n"
                       "export default " + lift::stepsJson(recipe) + ";\n")) {
        lift_test::failf(__FILE__, __LINE__, "写不进临时模块目录（%s）", dir.path().string().c_str());
        return false;
    }
    std::string fixture_source;
    if (!readTextFile(fs::path(LIFT_PLUGIN_FIXTURES_DIR) / "recipe-driver.js", &fixture_source)) {
        lift_test::failf(__FILE__, __LINE__, "读不到 %s/recipe-driver.js", LIFT_PLUGIN_FIXTURES_DIR);
        return false;
    }

    // ---- oracle ----
    Oracle o;
    o.mem_ok = publishEngine(o.mem, table);
    LIFT_CHECK(o.mem_ok);
    if (!o.mem_ok) return false;
    o.hwnd = o.wm.createWindow("RecipeParityOracle", 0u, 0u, 0u, 0u, 1280, 720, 0u);
    LIFT_CHECK(o.hwnd != 0);
    if (o.hwnd == 0) return false;
    o.plan.setClockForTest(&parityNowMs);
    o.plan.attachGuestMemory(&o.mem);
    o.plan.setProfileMarkHook(&o.profile, &oracleProfileHook);
    o.wm.setInputPlan(&o.plan);
    std::string plan_err;
    const bool plan_ok = o.plan.loadFile(plan_path.string(), &plan_err);
    if (!plan_ok) std::fprintf(stderr, "  plan load err: %s\n", plan_err.c_str());
    LIFT_CHECK(plan_ok);
    if (!plan_ok) return false;

    // ---- driver ----
    Driver d;
    d.mem_ok = publishEngine(d.mem, table);
    LIFT_CHECK(d.mem_ok);
    if (!d.mem_ok) return false;
    d.hwnd = d.wm.createWindow("RecipeParityDriver", 0u, 0u, 0u, 0u, 1280, 720, 0u);
    LIFT_CHECK(d.hwnd != 0);
    if (d.hwnd == 0) return false;
    d.ops.ctx = &d.profile;
    d.ops.profile_begin = &driverProfileBegin;
    d.ops.profile_end = &driverProfileEnd;
    d.host = std::make_unique<lift::plugin::PluginHost>(d.readings);
    // ★ 预算：与 `tests/plugin_runtime.cpp:205-209` 同一条（D5 的 2 ms / 100k 是**运行时**口径，
    //   不是本用例的判据；QJS 冷启动不该把 parity 变成随机红）。
    d.host->runner().setBudget(lift::plugin::Budget{500'000'000ull, 10'000'000u});
    d.host->runner().setClockForTest(&parityNowMs);
    d.host->executor().setClockForTest(&parityNowMs);
    const std::string origin = (dir.path() / "recipe-driver.js").string();
    const lift::plugin::PluginHost::Params params{{"origin", plan_path.string()}};
    d.loaded = d.host->init(fixture_source, origin, params, &d.ops, &d.load_error);
    if (d.loaded) d.host->attachMemory(d.mem);
    out->driver_loaded = d.loaded;
    if (!d.loaded) std::fprintf(stderr, "  driver load error: %s\n", d.load_error.c_str());
    LIFT_CHECK(d.loaded);

    // ---- 逐读点驱动两侧 ----
    const std::vector<Frame> frames = buildFrames(steps, never_match);
    if (frames.size() > kMaxFrames) {
        lift_test::failf(__FILE__, __LINE__, "读点脚本过长（%zu > %zu）", frames.size(), kMaxFrames);
        return false;
    }
    const std::int64_t total = static_cast<std::int64_t>(steps.size());
    std::int64_t prev_driver = 0;
    for (const Frame& f : frames) {
        // 世界（两侧同一组读数）
        if (!setWorld(o.mem, table, f.world)) {
            lift_test::failf(__FILE__, __LINE__, "setWorld 失败（script=%s）", f.world.script.c_str());
            return false;
        }
        d.readings.world = f.world;
        g_now_ms += kTickMs;
        o.plan.atPoint(o.wm, f.point);
        if (d.loaded) d.host->onReadPoint(d.wm, f.point);

        const std::int64_t oracle_next = static_cast<std::int64_t>(o.plan.stepsApplied());
        const std::int64_t driver_next = driverProgress(d, total);
        if (driver_next < prev_driver) out->driver_regressed = true;
        prev_driver = driver_next;
        // ★ 只查"driver 不许**超过** oracle"（eager 消费写 / quit 会让它在其它情况下落后，
        //   见差异表 E3）。⛔ 不按"最多差几拍"判 —— 那是实现的形状，不是合同。
        if (driver_next > oracle_next) out->driver_ahead = true;
        // `stepText` / 进度：两侧同步时，driver arm 的那一步必须与 oracle 卡住的那一步一致。
        if (driver_next == oracle_next && oracle_next < total) {
            const lift::Step* step = o.plan.currentStep();
            const auto& armed = d.host->runner().armed();
            if (step != nullptr && armed.size() == 1) {
                ++out->translation_checks;
                if (!triggerMatchesStep(armed.front(), *step, oracle_next)) {
                    ++out->translation_mismatch;
                    if (out->translation_mismatch <= 2) {
                        std::fprintf(stderr,
                                     "  [%s] armed 与 oracle 那一步不符：index=%lld armed=%s(kind=%d) "
                                     "oracle=%s\n",
                                     tag.c_str(), static_cast<long long>(oracle_next),
                                     armed.front().name.c_str(), static_cast<int>(armed.front().kind),
                                     lift::stepText(*step).c_str());
                    }
                }
            }
        }
    }

    // ---- 收口读数 ----
    out->oracle_ok = true;
    out->oracle_applied = static_cast<std::int64_t>(o.plan.stepsApplied());
    out->oracle_exit = lift::recipeOutcomeExitCode(o.plan.outcome());
    out->oracle_quit = o.plan.quitRequested();
    out->oracle_queue = queueFingerprint(o.wm);
    out->oracle_profile = joinLines(o.profile);
    o.wm.cursorClientPos(o.hwnd, &out->oracle_x, &out->oracle_y);
    out->oracle_buttons = o.wm.mouseButtons();
    out->oracle_fired = o.plan.fired();
    out->oracle_downs = countMessage(o.wm, kWmLButtonDown);

    out->driver_applied = driverProgress(d, total);
    out->driver_failed = d.host->failed();
    out->driver_fail = d.host->failName() + ": " + d.host->failReason();
    out->driver_finished = d.host->finished();
    out->driver_finish_code = d.host->finishCode();
    out->driver_finish_reason = d.host->finishReason();
    out->driver_quit = d.host->quitRequested();
    out->driver_queue = queueFingerprint(d.wm);
    out->driver_profile = joinLines(d.profile);
    d.wm.cursorClientPos(d.hwnd, &out->driver_x, &out->driver_y);
    out->driver_buttons = d.wm.mouseButtons();
    out->driver_downs = countMessage(d.wm, kWmLButtonDown);
    // ★ 退出码**语义**映射（合同 `:1181` 的"尽力"）：`finish(code)` ⇒ code；否则没走完 ⇒ 5。
    out->driver_exit = out->driver_finished ? out->driver_finish_code : 5;
    return true;
}

// =====================================================================================
// 已知差异表（合同 `:1180-1185` + 本切片实测出来的）
// =====================================================================================
const char* const kKnownDiffs[] = {
    "D1 timeout 边界：新 API `elapsed >= timeout_ms`，旧执行器 `stepWindowExpired` 是严格 `>`"
    "（`recipe.cpp:249`）⇒ 1 ms 差异（不逐位；本夹具只断言\"超时 ⇒ exit 5\"这条规则）",
    "D2 script 名字口径：旧执行器 `scriptNameMatches` 是 ASCII 大小写不敏感 + 两侧剥 `.BIN` + 接受"
    " `id:0x%08X` 退化名；driver 的 `afterScript/afterPosition` 走宿主**精确相等**"
    "（`trigger_runner.cpp:67-87`）⇒ driver 只覆盖精确名（本夹具的世界给的就是精确名）",
    "D3 `wait` 是宿主墙钟（NON-DIFFABLE）：本夹具两侧共用同一个注入时钟，只验证\"到期 ⇒ 走\"的顺序",
    "D4 `ctx.input.*` 是**下一读点**落地（`intent_queue.h:11-19` staged→ready），旧执行器在**同一次**"
    "读点施加；driver 的链还会**急**消费后续非阻塞指令 ⇒ 同一读点内的进度不逐拍对齐（消息序列 / 终态仍须相等）",
    "D5 `mouse_down` / `mouse_up`：C1 的 `IntentExecutor::drainQueue` **不施加**显式 down/up"
    "（`intent_executor.cpp:128-133`）⇒ driver 只能入队（队首头阻），旧执行器能施加 ⇒ 带 mouse_* 的配方"
    "NON-DIFFABLE（只承诺\"到断点为止逐条相同\"）",
    "D6 `quit` 的机制不同：旧执行器 `wm.postQuit(0)`（guest 可见 WM_QUIT，`input_plan.cpp:444-454`）；"
    "driver `ctx.quit()` 只是宿主 stop_requested（`plugin_host.cpp:416-421`）⇒ 队列指纹都不含虚拟 WM_QUIT，"
    "`quit` 只按\"两侧都请求了停止\"判",
    "D7 `profile_end <label>` 的 label：`ProfileApi.end()` 没有 label（`plugin.d.ts:272-275`）⇒ 只有 begin 的"
    "label 参与对齐（旧执行器对不一致只打一行提示，不失败）",
    "D8 `exit 5` / 失败报告是执行器状态机语义：driver 只能映射 `ctx.finish(5)` 的 receipt（合同 `:1181`），"
    "旧执行器的 `summary/failureReport/fault/trace/exit5` 仍是生产真源、不切换（`:1178`）",
    "D9 `Step[]` 的来路：`ctx.params` 能带字符串（`plugin.d.ts:359` / `script_quickjs.cpp:3498-3513`），"
    "但沙箱没有文件读 / `JSON`（`script_quickjs.cpp:9-22`、`:1965`）⇒ JSON 以**生成的 ES module** 进 driver；"
    "`ctx.params` 只带配方路径（provenance），⛔ 不给 `ctx` 加成员",
    "D10 **写指令与 `quit` 相邻**（同一个回调里）：plugin 的 `quit` 是**终态异常** ⇒ 同一回调里刚 staged 的"
    " input 会被立刻丢掉（`plugin_host.cpp:169-174` 的 F3）；旧执行器 `quit` 不丢（`input_plan.cpp:212-213`"
    " 的早退在 `releaseDue`/`pressDue` 之后 ⇒ 已臂上的 click 照旧按下 / 抬起）⇒ `click; quit` 这种形状"
    " **driver 会少落一次写**（官方 `.plan` 里没有这种形状；本夹具的 `instructions` 用一道门把它隔开）",
};

void printKnownDiffs() {
    std::printf("[recipe-parity] 已知差异表（%zu 条；合同 `:1180-1185` + 本切片实测）：\n",
                sizeof(kKnownDiffs) / sizeof(kKnownDiffs[0]));
    for (const char* line : kKnownDiffs) std::printf("[recipe-parity] diff: %s\n", line);
}

// =====================================================================================
// 断言：完整等价（EQUIV 档）
// =====================================================================================
void expectEquivalent(const std::string& tag, const ParityResult& r) {
    LIFT_CHECK(r.driver_loaded);
    LIFT_CHECK(!r.driver_failed);  // driver 自己失败 ⇒ 先看 failName/failReason（下面会打）
    if (r.driver_failed) std::fprintf(stderr, "  [%s] driver failed: %s\n", tag.c_str(), r.driver_fail.c_str());
    // ---- ① 消息（message/wParam/lParam）----
    LIFT_CHECK(r.driver_queue == r.oracle_queue);
    if (r.driver_queue != r.oracle_queue) {
        std::fprintf(stderr, "  [%s] 队列指纹不同\n    oracle=%s\n    driver=%s\n", tag.c_str(),
                     r.oracle_queue.c_str(), r.driver_queue.c_str());
    }
    // ---- ② 电平 + 光标 ----
    LIFT_CHECK_EQ_U32(r.driver_buttons, r.oracle_buttons);
    LIFT_CHECK_EQ_I64(r.driver_x, r.oracle_x);
    LIFT_CHECK_EQ_I64(r.driver_y, r.oracle_y);
    // ---- ③ fired（落地的按下条数）----
    // ★ 不拿 oracle 的 `fired()` 直接比：`fired_` 是**臂上**的 click 数（`input_plan.cpp` 的 `applyClick`
    //   里 +1），而一条 click 的 down 还要过 `pressDue` 的闸门 ⇒ 收口时可能还挂着一条没按下的
    //   （两侧**同样**挂着）。真正的"fired 相同" = 两侧落地的 `WM_LBUTTONDOWN` 条数相同；oracle 的
    //   `fired()` 只作读数（并断言 `fired ≥ 落地的 down`，否则说明计数口径坏了）。
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(r.driver_downs), static_cast<std::int64_t>(r.oracle_downs));
    LIFT_CHECK(r.oracle_fired >= r.oracle_downs);
    // ---- ④ 完成 / exit（**语义**相等；具体取值由各用例自己断言）----
    LIFT_CHECK_EQ_I64(r.driver_exit, r.oracle_exit);
    LIFT_CHECK(r.driver_finished);  // driver 的正常收口 = `ctx.finish(0, …)`（内部完成度账）
    if (r.driver_finished && r.driver_finish_code != 0) {
        std::fprintf(stderr, "  [%s] driver finish=%d reason=%s\n", tag.c_str(), r.driver_finish_code,
                     r.driver_finish_reason.c_str());
    }
    // ---- ⑤ 进度（applied/pending）----
    LIFT_CHECK_EQ_I64(r.driver_applied, r.oracle_applied);
    LIFT_CHECK(!r.driver_regressed);
    // ⚠ `r.driver_ahead` 只作**诊断**：driver 的链会**急**消费后续非阻塞指令（D4），而旧执行器是
    //   "一读点一条写"⇒ 同一读点内它可能领先 ⇒ ⛔ 不拿"领先"当判据（⛔ 也不许假装没这回事）。
    LIFT_CHECK_EQ_I64(r.translation_mismatch, 0);
    LIFT_CHECK(r.translation_checks > 0);
    // ---- ⑥ profile 的 HostEffect ----
    LIFT_CHECK(r.driver_profile == r.oracle_profile);
    if (r.driver_profile != r.oracle_profile) {
        std::fprintf(stderr, "  [%s] profile 序列不同\n    oracle=%s\n    driver=%s\n", tag.c_str(),
                     r.oracle_profile.c_str(), r.driver_profile.c_str());
    }
    // ---- ⑦ quit ----
    LIFT_CHECK_EQ_I64(r.driver_quit ? 1 : 0, r.oracle_quit ? 1 : 0);
}

// =====================================================================================
// 断言：已知缺口档（NON-DIFFABLE）—— 只承诺"到断点为止逐条相同 + 两侧都走完了链"
// =====================================================================================
void expectGapPrefix(const std::string& tag, const ParityResult& r) {
    LIFT_CHECK(r.driver_loaded);
    LIFT_CHECK(!r.driver_failed);
    if (r.driver_failed) std::fprintf(stderr, "  [%s] driver failed: %s\n", tag.c_str(), r.driver_fail.c_str());
    // driver 的消息必须是 oracle 消息的**前缀**（它只是少了 mouse_down/up 之后那些）。
    LIFT_CHECK(r.oracle_queue.rfind(r.driver_queue, 0) == 0);
    if (r.oracle_queue.rfind(r.driver_queue, 0) != 0) {
        std::fprintf(stderr, "  [%s] driver 的队列不是 oracle 的前缀\n    oracle=%s\n    driver=%s\n",
                     tag.c_str(), r.oracle_queue.c_str(), r.driver_queue.c_str());
    }
    LIFT_CHECK(r.driver_queue.size() < r.oracle_queue.size());  // 缺口必须**真的**存在（⛔ 不许假装等价）
    // 两侧都把**每一条指令**消费掉了（缺口在**施加**面，不在 driver 的链上）。
    LIFT_CHECK_EQ_I64(r.oracle_applied, static_cast<std::int64_t>(r.steps));
    LIFT_CHECK_EQ_I64(r.driver_applied, static_cast<std::int64_t>(r.steps));
    // oracle 的 fired 比 driver 落地的按下多（因为 down/up 之后还有 click）。
    LIFT_CHECK(r.driver_downs <= r.oracle_fired);
    LIFT_CHECK(r.driver_profile == r.oracle_profile);
    LIFT_CHECK(!r.driver_regressed);
}

// =====================================================================================
// 用例 1 · `Step[]` 的 JSON 出口（`recipe_json.cpp`）
// =====================================================================================
void caseStepsJson() {
    lift_test::g_current_case = "recipe-driver-parity/steps-json";
    lift::Recipe recipe;
    std::string err;
    const char* const kPlan =
        "# 行注释\n"
        "await script TITLE timeout 30000\n"
        "await position TITLE 0x0460 0x0520\n"
        "wait 500\n"
        "click 1070 480 right\n"
        "mouse_move 3 -4\n"
        "mouse_down\n"
        "mouse_up left\n"
        "profile_begin p1\n"
        "profile_end p1\n"
        "quit\n";
    const bool ok = recipe.loadText(kPlan, "mem", &err);
    if (!ok) std::fprintf(stderr, "  loadText err: %s\n", err.c_str());
    LIFT_CHECK(ok);
    if (!ok) return;
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(recipe.size()), 10);

    const std::string json = lift::stepsJson(recipe);
    // ★ `kind` 必须用 `stepKindName()` 的**同一份**字符串（闭集合的名字源只有一处）。
    struct Want {
        const char* kind;
        const char* fragment;
    };
    const Want wants[] = {
        {"await script", "\"kind\":\"await script\",\"line\":2,\"script\":\"TITLE\""},
        {"await position", "\"kind\":\"await position\",\"line\":3,\"script\":\"TITLE\",\"posFrom\":1120,\"posTo\":1312"},
        {"wait", "\"kind\":\"wait\",\"line\":4,\"script\":\"\",\"posFrom\":0,\"posTo\":0,\"waitMs\":500"},
        {"click", "\"kind\":\"click\",\"line\":5,\"script\":\"\",\"posFrom\":0,\"posTo\":0,\"waitMs\":0,\"x\":1070,\"y\":480,\"button\":\"right\""},
        {"mouse_move", "\"kind\":\"mouse_move\",\"line\":6,\"script\":\"\",\"posFrom\":0,\"posTo\":0,\"waitMs\":0,\"x\":3,\"y\":-4,\"button\":\"left\""},
        {"mouse_down", "\"kind\":\"mouse_down\""},
        {"mouse_up", "\"kind\":\"mouse_up\""},
        {"profile_begin", "\"kind\":\"profile_begin\",\"line\":9,\"script\":\"\",\"posFrom\":0,\"posTo\":0,\"waitMs\":0,\"x\":0,\"y\":0,\"button\":\"left\",\"label\":\"p1\""},
        {"profile_end", "\"kind\":\"profile_end\",\"line\":10,\"script\":\"\",\"posFrom\":0,\"posTo\":0,\"waitMs\":0,\"x\":0,\"y\":0,\"button\":\"left\",\"label\":\"p1\""},
        {"quit", "\"kind\":\"quit\""},
    };
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(sizeof(wants) / sizeof(wants[0])), 10);
    for (const Want& w : wants) {
        LIFT_CHECK(json.find(w.kind) != std::string::npos);
        LIFT_CHECK(json.find(w.fragment) != std::string::npos);
        if (json.find(w.fragment) == std::string::npos) {
            std::fprintf(stderr, "  [steps-json] 缺片段（%s）：%s\n", w.kind, w.fragment);
        }
    }
    // 行尾 `timeout` 只挂在门上；`wait` 的 timeoutMs 恒 0（解析期就禁止）。
    LIFT_CHECK(json.find("\"kind\":\"await script\",\"line\":2,\"script\":\"TITLE\",\"posFrom\":0,\"posTo\":0,"
                         "\"waitMs\":0,\"x\":0,\"y\":0,\"button\":\"left\",\"label\":\"\",\"timeoutMs\":30000") !=
               std::string::npos);
    LIFT_CHECK(json.find("\"origin\":\"mem\"") != std::string::npos);
    // 一条 `Step` 的入口与数组入口一致（⛔ 两份拼法）。
    LIFT_CHECK(json.find(lift::stepJson(recipe.steps()[0])) != std::string::npos);
}

// =====================================================================================
// 用例 2 · 九条指令的翻译（8 条可施加 + profile/quit）+ `quit` 截断 ⇒ exit 5
// =====================================================================================
void caseInstructions() {
    lift_test::g_current_case = "recipe-driver-parity/instructions";
    const fs::path plan = "recipe-parity-instructions.plan";
    ScopedDir dir;
    LIFT_CHECK(dir.make("instructions"));
    const std::string text =
        "profile_begin inst\n"
        "await script TITLE timeout 30000\n"
        "click 100 200\n"
        "mouse_move 110 210\n"
        "wait 100\n"
        "profile_end inst\n"
        "await position TITLE 0x0460 0x0520\n"
        "click 300 300 right\n"
        "await script FIELD\n"  // ★ 写之后要有**阻塞**的一步：否则 `click` 与 `quit` 同一次回调
                                //   ⇒ 见 D10（plugin 的 quit 是终态异常，会丢掉同一回调里 staged 的 input）
        "quit\n"
        "await script SC0000\n";  // ★ quit 之后还有一条 ⇒ 旧执行器 `outcome()==Running` ⇒ exit 5
    const fs::path path = dir.path() / plan;
    LIFT_CHECK(writeTextFile(path, text));
    ParityResult r;
    const bool ran = runParity(path, "instructions", &r);
    LIFT_CHECK(ran);
    if (!ran) return;
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(r.steps), 11);
    expectEquivalent("instructions", r);
    // ★ `quit` 截断 ⇒ 没走完 ⇒ exit 5（两侧同一条规则；⛔ 不 assert 具体退出码的"当前取值"）。
    LIFT_CHECK_EQ_I64(r.oracle_exit, 5);
    LIFT_CHECK_EQ_I64(r.driver_exit, 5);
    LIFT_CHECK(r.oracle_quit);
    LIFT_CHECK(r.driver_quit);
    LIFT_CHECK(r.oracle_applied < static_cast<std::int64_t>(r.steps));  // 最后一条没消费
    LIFT_CHECK(r.oracle_fired > 0);
    std::printf("[recipe-parity] instructions: msgs=%zu fired=%llu profile=%s exit=%lld\n",
                r.oracle_queue.size(), static_cast<unsigned long long>(r.oracle_fired), r.oracle_profile.c_str(),
                static_cast<long long>(r.oracle_exit));
}

// =====================================================================================
// 用例 3 · 门超时 ⇒ exit 5（"超时 ⇒ exit 5"这条**规则**，⛔ 不断言 1 ms 边界）
// =====================================================================================
void caseGateTimeout() {
    lift_test::g_current_case = "recipe-driver-parity/gate-timeout";
    ScopedDir dir;
    LIFT_CHECK(dir.make("timeout"));
    const fs::path path = dir.path() / "timeout.plan";
    // 世界永远是 `IDLE` ⇒ 这个门永远不成立。
    LIFT_CHECK(writeTextFile(path, "await script NEVER timeout 500\nclick 10 20\n"));
    ParityResult r;
    // ★ 世界的门**一条都不成立**（否则 oracle 会走完、driver 也会走完 ⇒ 测不到超时）。
    const bool ran = runParity(path, "timeout", &r, /*never_match=*/true);
    LIFT_CHECK(ran);
    if (!ran) return;
    // oracle：守卫窗口严格 `>` ⇒ 超时 ⇒ TimedOut ⇒ exit 5。
    LIFT_CHECK_EQ_I64(r.oracle_exit, 5);
    LIFT_CHECK_EQ_I64(r.oracle_applied, 0);
    // driver：`timeoutMs` 交给宿主 trigger ⇒ TimeoutEvent ⇒ `ctx.finish(5)`。
    LIFT_CHECK(r.driver_finished);
    LIFT_CHECK_EQ_I64(r.driver_finish_code, 5);
    LIFT_CHECK_EQ_I64(r.driver_exit, 5);
    // 两侧都没施加那次 click（超时之前不许"接着点"）。
    LIFT_CHECK(r.oracle_queue.empty());
    LIFT_CHECK(r.driver_queue.empty());
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(r.driver_downs), 0);
    // driver 的收口原因必须点名"为什么要 exit 5"（⛔ 不静默）。
    LIFT_CHECK(r.driver_finish_reason.find("timeout") != std::string::npos);
    std::printf("[recipe-parity] gate-timeout: oracle_exit=%lld driver_exit=%lld reason=%s\n",
                static_cast<long long>(r.oracle_exit), static_cast<long long>(r.driver_exit),
                r.driver_finish_reason.c_str());
}

// =====================================================================================
// 用例 4 · `mouse_down` / `mouse_up` 的**已知缺口**（C1 executor 不施加显式 down/up）
// =====================================================================================
void caseMouseButtonsGap() {
    lift_test::g_current_case = "recipe-driver-parity/mouse-buttons-gap";
    ScopedDir dir;
    LIFT_CHECK(dir.make("mouse-buttons"));
    const fs::path path = dir.path() / "mouse-buttons.plan";
    LIFT_CHECK(writeTextFile(path,
                             "mouse_move 100 200\n"
                             "mouse_down\n"
                             "mouse_up\n"
                             "click 300 300\n"));
    ParityResult r;
    const bool ran = runParity(path, "mouse-buttons", &r);
    LIFT_CHECK(ran);
    if (!ran) return;
    LIFT_CHECK(r.mouse_button_steps);
    expectGapPrefix("mouse-buttons", r);
    // 缺口必须是"显式 down/up 入队但落不了地"（⛔ 不是 driver 丢指令）。
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(r.driver_downs), 0);  // 一次都没按下
    LIFT_CHECK(r.oracle_fired >= 1);                                  // oracle 按下过
    // driver 的收口仍是正常的（缺口在施加面 ⇒ 链走完了）。
    LIFT_CHECK(r.driver_finished);
    std::printf("[recipe-parity] mouse-buttons-gap: oracle_msgs=%zu driver_msgs=%zu（NON-DIFFABLE："
                "C1 不施加显式 down/up）\n",
                r.oracle_queue.size(), r.driver_queue.size());
}

// =====================================================================================
// 用例 5 · 官方配方逐条 replay（`scripts/recipes/*.plan`）
// =====================================================================================
void caseOfficialPlans() {
    lift_test::g_current_case = "recipe-driver-parity/plans";
    std::vector<fs::path> plans;
    std::error_code ec;
    for (const fs::directory_entry& e : fs::directory_iterator(fs::path(LIFT_RECIPES_DIR), ec)) {
        if (!e.is_regular_file()) continue;
        if (e.path().extension() != ".plan") continue;
        plans.push_back(e.path());
    }
    if (ec) std::fprintf(stderr, "  读 %s 失败：%s\n", LIFT_RECIPES_DIR, ec.message().c_str());
    LIFT_CHECK(!plans.empty());
    if (plans.empty()) return;
    std::sort(plans.begin(), plans.end());
    std::size_t equiv = 0;
    std::size_t non_diffable = 0;
    for (const fs::path& p : plans) {
        ParityResult r;
        const std::string tag = p.stem().string();
        const bool ran = runParity(p, tag, &r);
        LIFT_CHECK(ran);
        if (!ran) continue;
        LIFT_CHECK(r.steps > 0);
        // oracle 必须把配方走完（否则"parity 的参照物"本身没跑到终点）。
        LIFT_CHECK_EQ_I64(r.oracle_applied, static_cast<std::int64_t>(r.steps));
        if (r.mouse_button_steps) {
            // ★ NON-DIFFABLE：缺口在**施加**面（E5 / D5），不是 driver 的链。
            ++non_diffable;
            expectGapPrefix(tag, r);
            std::printf("[recipe-parity] plan=%s steps=%zu verdict=NON-DIFFABLE gap=mouse_down/up "
                        "oracle_msgs=%zu driver_msgs=%zu driver_finish=%d\n",
                        p.filename().string().c_str(), r.steps, r.oracle_queue.size(), r.driver_queue.size(),
                        r.driver_finish_code);
        } else {
            ++equiv;
            expectEquivalent(tag, r);
            // 官方配方都是**走完**的（`Done`）⇒ exit 0（旧 `--input-plan` 的完成度口径不变）。
            LIFT_CHECK_EQ_I64(r.oracle_exit, 0);
            LIFT_CHECK_EQ_I64(r.oracle_applied, static_cast<std::int64_t>(r.steps));
            LIFT_CHECK(r.driver_finished);
            LIFT_CHECK_EQ_I64(r.driver_finish_code, 0);
            std::printf("[recipe-parity] plan=%s steps=%zu verdict=EQUIV msgs=%zu fired=%llu profile=%s exit=%lld"
                        "\n",
                        p.filename().string().c_str(), r.steps, r.oracle_queue.size(),
                        static_cast<unsigned long long>(r.oracle_fired), r.oracle_profile.c_str(),
                        static_cast<long long>(r.oracle_exit));
        }
    }
    std::printf("[recipe-parity] plans=%zu equiv=%zu non-diffable=%zu（缺口 = 含 mouse_down/mouse_up 的配方）\n",
                plans.size(), equiv, non_diffable);
    printKnownDiffs();
}

}  // namespace

int main() {
    caseStepsJson();
    caseInstructions();
    caseGateTimeout();
    caseMouseButtonsGap();
    caseOfficialPlans();
    return lift_test::finish("recipe-driver-parity");
}
