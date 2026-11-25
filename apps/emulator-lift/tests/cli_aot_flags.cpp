// emulator-lift · **AOT 臂**的 `--cursor-warp` / `--sound` 参数面（CLI 级 + 真子进程）
//
// 判据（红了意味着什么）：
//   ① `--help` 里 `--cursor-warp` 与 `--sound` **都在**。
//      红了 ⇒ 用户按 README / 解释器臂的写法给参数时，AOT 臂连"有这个东西"都不知道。
//   ② `--cursor-warp --present headless` ⇒ exit 2，stderr 是**"headless 不接受窗口参数"**那句
//      （含 `--present headless` 与 `--cursor-warp`），**不含** `未知参数`。
//      红了 ⇒ 一个必然无效的开关被当成拼错的参数打回（或反过来被静默接受）。
//   ③ 错拼 `--cursor-wrap`（wrap ≠ warp）**仍然**是 `未知参数` + exit 2。
//      红了 ⇒ 参数解析退化成"接受任何参数"，② 的绿就没有意义（§12.5 的 `--native-overrides`
//      那条纪律：⛔ 不许靠"什么都收"让守卫变绿）。
//   ④ `--sound` ⇒ **被接受**（不是 `未知参数`）并打**一条**一次性说明：这条说明必须**与代码一致**
//      —— ⛔ 不许再说"本臂没有音频管线 / 不会有声音"（那是接线前的形状）。且这条说明在这份 stderr
//      里**恰好一次**。红了 ⇒ 要么"指定了就跑不起来"（另一种"无法指定"），要么静默接受。
//      ★ **洞 6**：这里⛔ **不**再断言"说明文本里含 `muted=0`"——那是**自证式断言**（只断言自己
//        打的那句话，`--sound` 的接线丢了它照绿）。真正的判据落在**引擎状态**上：
//        `aot-audio-wiring`（真 run，读收口摘要的 `audio     : … muted=0|1`，含 `muted=1` 的对照组）。
//      ★ **洞 4**：这段说明里"读数"那句必须限定"**仅** `--audio sdl` 或 `--audio-dump` 时
//        才有 `audio     :` 一行"（只给 `--sound` 时档位是 off ⇒ 没有那一行）。行为判据同样在
//        `aot-audio-wiring` 的①（那条 run 里那一行**不存在**）。
//   ⑤ 缺省档（**不给** `--sound`）这条说明**不出现**（同一份 stderr 面的对照组）。
//   ⑥ **S1 参数面**：`--audio sdl` **被接受**（旧文案"本臂只有 off"必须消失）；
//      **`--audio virtual` 必须被拒**：exit 2、stderr 是 `--audio` 的非法值文案、**点名
//      `off / sdl`**（⛔ 不许再把这个档位收回来）。该档已于 2026-11-22 整档移除：它看起来像
//      产品态、其实**没有 feeder 线程**（走主线程 D 泵），留着会让人把它当"产品态音频"读数。
//      非法值 `--audio bogus` 同样 exit 2 并点名合法值域（⛔ 不放宽成"认不出就当 off"）；
//      `--audio-dump` 缺值 ⇒ exit 2 并点名这个 flag（⛔ 不静默当成"没有 dump"）。
//      红了 ⇒ 用户要么无法指定后端 / dump，要么非法值（含已移除的 `virtual`）被静默吞掉。
//   ⑦ `--help` 里有 `--audio-dump`（否则用户不知道这条入口存在）；且它的说明**声明**了
//      「abort 之后 dump 不可信」（洞 7：`std::abort()` 路径跳过 `AudioEngine::end()` ⇒ WAV 两个
//      长度字段不回填。⛔ 不改 abort 语义，只声明）。
//   ⑧ `--profile-slow-frames <ms>`（按帧的 shim 分桶）：`--help` 里有它；**缺 `--profile` ⇒ exit 2
//      并点名两者**（含值 0 = 显式关那一档）；负数 / 小数 / 带单位 / 垃圾 / 缺值 ⇒ exit 2 且点名；
//      `--profile --profile-slow-frames 50` **被接受**。
//      红了 ⇒ 要么仪器"给了但跑不起来"，要么非法值被静默吞成 0（= 以为在量、其实没量）。
//   ⑨ **S9** `--ui-input=consume|pass`：help 里两种写法都在；非法值 ⇒ exit 2 + 点名合法值；
//      合法值被接受（走到 headless 的窗口参数拒绝，而不是「值非法」）；分离形式与 `=` 同结论。
//      ★ pure 值域判据在 `overlay/ui-input-parse`（⛔ 不靠本用例当唯一判据）。
//
// ★ 四条主断言**全部判在 GPU bring-up 之前**：AOT 臂的参数校验（`aot_main.cpp` 的
//   `--audio` / `--cursor-warp` 平台闸门 / `present_window_flag` / `--sound` 说明）都在
//   `SdlWindow::init`（窗口 + GPU device）**之前** ⇒ 本用例**不需要 GPU、不需要任何资产**
//   （不读 LFS、不碰游戏安装、不给 `--exe`：走到执行源定位时才失败，而那时判据已经判完）。
//   ⇒ 这里**没有** `LIFT_SKIP_IF_NO_GPU`：判据不依赖它，加了反而会让"没 GPU 就整条不判"。
// ★ 为什么是 CLI 级 + 真子进程：判据穿过"参数解析 → 早期拒绝 → 文案 → 退出码"整条链；
//   单测某个解析函数抓不到"解析对了但闸门漏了 / 文案还写着未知参数"这种半修。
// ★ 子进程 cwd 指到临时目录（照 `tests/cli_cursor_warp.cpp:17-18` 的理由）⇒ 结果不随宿主 cwd 漂。
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "check.h"
#include "support/subprocess.h"

namespace {

namespace fs = std::filesystem;

bool hasLineStartingWith(const std::string& text, const std::string& prefix) {
    std::size_t pos = 0;
    while (pos <= text.size()) {
        const std::size_t nl = text.find('\n', pos);
        const std::string line = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        if (line.compare(0, prefix.size(), prefix) == 0) return true;
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
    return false;
}

/** `needle` 在 `hay` 里出现几次（④ 的"**恰好一次**"判据）。 */
std::size_t countOccurrences(const std::string& hay, const std::string& needle) {
    if (needle.empty()) return 0;
    std::size_t n = 0;
    std::size_t pos = hay.find(needle);
    while (pos != std::string::npos) {
        ++n;
        pos = hay.find(needle, pos + needle.size());
    }
    return n;
}

/** `/` 与 `\` 统一成 `/`（**只用于断言路径子串**；⛔ 不参与任何真实路径计算）。 */
std::string normalizedSeparators(const std::string& text) {
    std::string out = text;
    for (char& c : out) {
        if (c == '\\') c = '/';
    }
    return out;
}

// ★ ④ 用来认"那条一次性说明"的**唯一**标记：说明的开头。⛔ 不用泛词（`--sound` 会命中 help /
//   其它文案）—— 标记必须只属于这句话，否则"恰好一次"这条断言会变成恒真。
const char* const kSoundNoticeMarker = "--sound 已接受";

/** ★ S1：`--ui` 的 deprecation 标记（**必须**与 `aot_main.cpp` 打印的那一行逐字一致）。 */
const char* const kUiDeprecatedMarker = "--ui 已弃用";

/** 本文件的公共 spawn 选项：cwd 指到临时目录；超时沿用原值 30 s（这几条都在建窗口之前返回）。 */
lift_test::ChildOptions childOptions(const fs::path& cwd) {
    lift_test::ChildOptions opts;
    opts.cwd = cwd;
    opts.timeout = std::chrono::milliseconds(30000);
    return opts;
}

}  // namespace

int main(int argc, char** argv) {
    using namespace lift_test;

    if (argc < 2) {
        LIFT_SKIP("需要 CTest 传 <emulator-lift-aot.exe>（见 CMakeLists 的 cli-aot-flags）");
    }
    const fs::path exe = fs::path(argv[1]);
    LIFT_CHECK(fs::exists(exe));

    std::error_code ec;
    const fs::path root = fs::temp_directory_path(ec) / ("lift-cli-aot-" + std::to_string(lift_test::processId()));
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);
    LIFT_CHECK(!ec);

    // ---- ① `--help`：两个 flag 都在（缺一个都红）----
    g_current_case = "help-lists-both-flags";
    {
        const ChildResult r = runChild(exe, {"--help"}, childOptions(root));
        LIFT_CHECK(r.started);
        LIFT_CHECK(!r.timed_out);
        LIFT_CHECK_EQ_U32(r.exit_code, 0u);
        LIFT_CHECK(r.out.find("--cursor-warp") != std::string::npos);
        LIFT_CHECK(r.out.find("--sound") != std::string::npos);
        // ★ S1：`--audio` 的两个档位与 `--audio-dump` 必须在 help 里（否则用户不知道它们存在）。
        //   ★ 2026-11-22：`virtual` 档整档移除 ⇒ help 里**不许**再列它（值域 = off / sdl）。
        LIFT_CHECK(r.out.find("--audio off|sdl") != std::string::npos);
        LIFT_CHECK(r.out.find("--audio off|virtual|sdl") == std::string::npos);
        LIFT_CHECK(r.out.find("--audio-dump") != std::string::npos);
        // ★ 洞 7：`--audio-dump` 的说明必须**声明**「abort 之后 dump 不可信」（不改 abort 语义，只声明）。
        //   红了 ⇒ 那条已知的失效方式（`std::abort()` 跳过 `end()` ⇒ WAV 长度字段不回填）又变成
        //   "只有读过代码的人知道"。
        LIFT_CHECK(r.out.find("abort") != std::string::npos);
        LIFT_CHECK(r.out.find("不可信") != std::string::npos);
        // ★ S4：`--input-plan` 必须在 help 里（否则用户不知道 AOT 臂有输入控制面）。
        LIFT_CHECK(r.out.find("--input-plan") != std::string::npos);
        // ★ 诊断档 `--input-plan-trace` 也必须在 help 里（否则"配方 click 不被消费"只能靠猜；
        //   见 `REQ-01M7PG086DG4BBPA40VCVZTP4X` B）。解析期判据另在 `input-plan-read-points`
        //   （⛔ 不靠这一条当唯一判据）。
        LIFT_CHECK(r.out.find("--input-plan-trace") != std::string::npos);
        // ★ S6：三条鼠标原语必须在 help 里（否则用户不知道"拖拽"能在配方里表达；
        //   词表与 `runtime/recipe.cpp` 的 `instructionList` 是**同一组**指令）。
        //   解析期判据另在 `input-plan-read-points` 的 `input-plan/parse-errors` /
        //   `input-plan/mouse-primitives`（⛔ 不靠这一条当唯一判据）。
        LIFT_CHECK(r.out.find("mouse_move") != std::string::npos);
        LIFT_CHECK(r.out.find("mouse_down") != std::string::npos);
        LIFT_CHECK(r.out.find("mouse_up") != std::string::npos);
        // ★ S9：`--ui-input` 的**两个合法值**与"两种写法都收"必须写在 help 里（D2；否则用户
        //   不知道"注入点击可以被叠层吞 / 怎么关掉"）。值域本身的机械判据另在
        //   `overlay/ui-input-parse`（pure），⛔ 不靠这一条当唯一判据。
        LIFT_CHECK(r.out.find("--ui-input=consume|pass") != std::string::npos);
        LIFT_CHECK(r.out.find("--ui-input consume|pass") != std::string::npos);
        // ★ `--profile-slow-frames` 必须在 help 里（否则"按帧的 shim 分桶"这条仪器等于不存在，
        //   而 wipe 剖面的最大缺口正是它 —— 见 `docs/03-emulator/wipe-profile.md`）。
        LIFT_CHECK(r.out.find("--profile-slow-frames") != std::string::npos);
        // ★ `--scale` 必须在 help 里，且说明里写明 `auto` 与 headless 的两条口径
        //   （否则用户不知道有这条入口，也不知道 headless 下它不生效）。
        LIFT_CHECK(r.out.find("--scale auto|数值") != std::string::npos);
        LIFT_CHECK(r.out.find("headless") != std::string::npos);
        // ★ `--scale-filter nearest|linear` 必须在 help 里（否则"放大用哪个滤波器"这条入口等于
        //   不存在，用户既找不到 nearest 回退档、也读不到缺省是 linear）。值域 / 拒绝口径的机械
        //   判据另在 pure 用例 `window-scale/parse-filter`，⛔ 不靠这一条当唯一判据。
        LIFT_CHECK(r.out.find("--scale-filter nearest|linear") != std::string::npos);
        // ★ C2a：`--plugin` / `--plugin-param` 必须在 help 里（否则用户不知道 AOT 臂有插件面；
        //   合同 `plugin-runtime.md` §4.6 / 裁决 D-C2-2。装载面的判据另在 `plugin-runtime`
        //   （pure），⛔ 不靠这一条当唯一判据）。
        //   ★ S1：`--plugin` 收 **id 与 path 两种 spec** ⇒ help 必须写 `<id|path>`，并把
        //   id 的两处**非递归**搜索根写出来（否则"为什么 `--plugin save` 找不到 lib/save.js"
        //   只能靠读源码）。红了 ⇒ help 与解析器分叉。
        LIFT_CHECK(r.out.find("--plugin <id|path>") != std::string::npos);
        LIFT_CHECK(r.out.find("scripts/plugins") != std::string::npos);
        LIFT_CHECK(r.out.find("LIFT_PLUGIN_DIR") != std::string::npos);
        LIFT_CHECK(r.out.find("不查 `lib/`") != std::string::npos);
        LIFT_CHECK(r.out.find("--plugin-param k=v") != std::string::npos);
        LIFT_CHECK(r.out.find("export default") != std::string::npos);
        // ★ S2：`hud` 是**保留 id**（`--overlay` = `--plugin hud` 预设）⇒ help 必须写出来
        //   （否则"为什么用户插件根里的 hud.js 会让启动 exit 2"只能靠读源码）。
        LIFT_CHECK(r.out.find("保留 id `hud`") != std::string::npos);
        LIFT_CHECK(r.out.find("--plugin hud") != std::string::npos);
        LIFT_CHECK(r.out.find("scripts/ui/hud.js") != std::string::npos);
        // ★ S1：`--ui` 仍在（**只 deprecated、不删**）⇒ help 必须标出来。
        LIFT_CHECK(r.out.find("已弃用") != std::string::npos);
    }

    // ---- ①⁰⁰ C2a `--plugin` 的**启动即拒绝**（parse 期；⛔ 不需要 GPU / 资产）----
    //   判据（红了意味着什么）：两套输入写预算被允许共存 / 同一字段给两份路径被静默取一个 ⇒
    //   运行时会出现"一读点两条写"或"我传的插件没跑"这种**没有症状**的错。
    g_current_case = "plugin-cli-mutex";
    {
        struct Case {
            const char* name;
            std::vector<std::string> args;
            const char* must;
            const char* also;
        };
        const Case cases[] = {
            {"plugin+input-plan", {"--plugin", "a.js", "--input-plan", "b.plan"}, "--plugin", "--input-plan"},
            {"plugin+control-root", {"--plugin", "a.js", "--control-root", "ctl"}, "--plugin", "--control-root"},
            {"plugin+ui-different", {"--plugin", "a.js", "--ui", "b.js"}, "--plugin", "--ui"},
            {"plugin-param-duplicate", {"--plugin", "a.js", "--plugin-param", "k=1", "--plugin-param", "k=2"},
             "--plugin-param", "k"},
            {"plugin-param-shape", {"--plugin", "a.js", "--plugin-param", "novalue"}, "--plugin-param", "k=v"},
            // ★ F6（C2a 复审）：重复 `--plugin` 给两份不同路径 — 与 `--plugin`/`--ui` 的
            //   "不静默只取一个"对称（旧实现无条件覆盖 ⇒ 静默用后一个）。
            {"plugin-duplicate", {"--plugin", "a.js", "--plugin", "b.js"}, "--plugin", "a.js"},
            // ★ F6 的另一半：`--plugin-param` 没有 `--plugin` ⇒ 它**没有消费者**，⛔ 不许静默忽略。
            {"plugin-param-without-plugin", {"--plugin-param", "k=v"}, "--plugin-param", "--plugin"},
        };
        for (const Case& c : cases) {
            const ChildResult bad = runChild(exe, c.args, childOptions(root));
            LIFT_CHECK(bad.started);
            LIFT_CHECK(!bad.timed_out);
            LIFT_CHECK_EQ_U32(bad.exit_code, 2u);
            LIFT_CHECK(bad.err.find("未知参数") == std::string::npos);
            LIFT_CHECK(bad.err.find(c.must) != std::string::npos);
            LIFT_CHECK(bad.err.find(c.also) != std::string::npos);
        }
    }

    // ---- ①⁰¹ S1 `--plugin <id>`：找不到 ⇒ **启动即 exit 2** + 列出搜过的根 / 每个候选 ----
    //   判据（红了意味着什么）：id 被当成路径（于是"装载失败"与"找不到 id"混成一句），或搜索根
    //   没被填进解析器（候选清单缺失）—— 后者会让"我明明放在 scripts/plugins/ 里"变成不可解释。
    //   ★ 这条也**穿过 aot_main 的接线**（两次根注入 + 错误文案）；纯解析器本身的判据在
    //     pure 用例 `plugin-resolve`（⛔ 不靠这一条当唯一判据）。
    g_current_case = "plugin-id-not-found";
    {
        const ChildResult r = runChild(exe, {"--plugin", "does-not-exist"}, childOptions(root));
        LIFT_CHECK(r.started);
        LIFT_CHECK(!r.timed_out);
        LIFT_CHECK_EQ_U32(r.exit_code, 2u);
        LIFT_CHECK(r.err.find("未知参数") == std::string::npos);
        LIFT_CHECK(r.err.find("找不到 id") != std::string::npos);
        LIFT_CHECK(r.err.find("does-not-exist") != std::string::npos);
        // ★ S1 复审：两个根都要**逐字**点名（只断言"含 scripts/plugins"两条候选都满足 ⇒ 删掉
        //   任一根仍绿）。候选行 = `<id>.js` 恰 2 次 + 发行根 / 开发根各一条绝对路径。
        const std::string exe_candidate =
            pathToUtf8(exe.parent_path() / "scripts" / "plugins" / "does-not-exist.js");
        const std::string dev_candidate = pathToUtf8(fs::path(LIFT_PLUGIN_DIR) / "does-not-exist.js");
        const std::string err_norm = normalizedSeparators(r.err);
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(countOccurrences(r.err, "does-not-exist.js")), 2);
        LIFT_CHECK(err_norm.find(normalizedSeparators(exe_candidate)) != std::string::npos);
        LIFT_CHECK(err_norm.find(normalizedSeparators(dev_candidate)) != std::string::npos);
        // 启动前拒绝 ⇒ 一行运行摘要都没有。
        LIFT_CHECK(!hasLineStartingWith(r.out, "stop"));
    }

    // ---- ①⁰²' S1 复审：显式空串 `--plugin ""` ⇒ exit 2（⛔ 不静默当成"没有插件"）----
    //   红线：旧实现让空 spec 整块跳过 ⇒ "我明明给了 --plugin 却没生效"；空串既不是 id 也不是
    //   可装载路径 ⇒ 启动即拒绝并点名"空串不是 id"（合法写法 = id 或路径）。
    g_current_case = "plugin-empty-spec";
    {
        const ChildResult r = runChild(exe, {"--plugin", ""}, childOptions(root));
        LIFT_CHECK(r.started);
        LIFT_CHECK(!r.timed_out);
        LIFT_CHECK_EQ_U32(r.exit_code, 2u);
        LIFT_CHECK(r.err.find("未知参数") == std::string::npos);
        LIFT_CHECK(r.err.find("收到空串") != std::string::npos);
        LIFT_CHECK(r.err.find("空串不是 id") != std::string::npos);
        LIFT_CHECK(!hasLineStartingWith(r.out, "stop"));
    }

    // ---- ①⁰² S1 `--ui` **只 deprecated**：每次给都打**一行** warn，前缀不是 `[ui]` ----
    //   判据（红了意味着什么）：`--ui` 被静默删掉（用户现有命令变成未知参数），或 deprecation
    //   变成周期输出 / 用了 `[ui]` 前缀（`tests/aot_overlay.cpp` 在数 `[ui]` / `[overlay]` 行数）。
    //   ★ 这条同时钉"`--ui` 仍是窗口参数"（headless 仍拒绝）—— 本轮 ⛔ 不改它的语义，只标弃用。
    g_current_case = "ui-deprecated-notice";
    {
        const ChildResult r =
            runChild(exe, {"--ui", "hud.js", "--present", "headless"}, childOptions(root));
        LIFT_CHECK(r.started);
        LIFT_CHECK(!r.timed_out);
        LIFT_CHECK_EQ_U32(r.exit_code, 2u);
        LIFT_CHECK(r.err.find("headless 不接受窗口参数") != std::string::npos);
        LIFT_CHECK(countOccurrences(r.err, kUiDeprecatedMarker) == 1u);
        LIFT_CHECK(r.err.find("[ui]") == std::string::npos);
        LIFT_CHECK(r.err.find("--plugin <path>") != std::string::npos);
    }

    // ---- ①⁰³ S2 `--overlay` = `--plugin hud` 预设，但**仍是窗口参数** ----
    //   判据（红了意味着什么）：预设把"要窗口"吃掉了（`--overlay --present headless` 不再 exit 2），
    //   或预设没解析 hud（没有那行 `--plugin id=hud` —— 用户看不到"它到底装了谁"）。
    //   ★ 这条在 `--present` 校验之前完成解析（与 `--plugin` 同一条），所以纯 CLI 可判。
    g_current_case = "overlay-headless-rejected";
    {
        const ChildResult r =
            runChild(exe, {"--overlay", "--present", "headless"}, childOptions(root));
        LIFT_CHECK(r.started);
        LIFT_CHECK(!r.timed_out);
        LIFT_CHECK_EQ_U32(r.exit_code, 2u);
        LIFT_CHECK(r.err.find("headless 不接受窗口参数") != std::string::npos);
        LIFT_CHECK(r.err.find("--overlay") != std::string::npos);  // 窗口参数清单点名它
        LIFT_CHECK(r.err.find("--plugin id=hud") != std::string::npos);  // 预设真的解析了保留 id
    }

    // ---- ①⁰ S9 `--ui-input=consume|pass`：值域 + **窗口参数**语义 ----
    //   判据（红了意味着什么）：
    //   ① 非法值（`pas`）⇒ exit 2 + 点名合法值，且**不是** `未知参数`（⛔ 不许"认不出就当 consume"）；
    //   ② 合法值被**接受**（只有解析通过才会走到 headless 的窗口参数拒绝）⇒
    //      `--present headless --ui-input=pass` = exit 2 + 「headless 不接受窗口参数」，
    //      而**不是**「值非法」；
    //   ③ 分离形式 `--ui-input consume` 与 `=` 形式同一结论（help 里两种写法都写了）；
    //   ④ 拒绝文案里的窗口参数清单含 `--ui-input`（用户照着改）。
    g_current_case = "ui-input-values";
    {
        const ChildResult bad = runChild(exe, {"--ui-input=pas"}, childOptions(root));
        LIFT_CHECK(bad.started);
        LIFT_CHECK(!bad.timed_out);
        LIFT_CHECK_EQ_U32(bad.exit_code, 2u);
        LIFT_CHECK(bad.err.find("未知参数") == std::string::npos);
        LIFT_CHECK(bad.err.find("consume") != std::string::npos);
        LIFT_CHECK(bad.err.find("pass") != std::string::npos);

        const ChildResult eq =
            runChild(exe, {"--present", "headless", "--ui-input=pass"}, childOptions(root));
        LIFT_CHECK(eq.started);
        LIFT_CHECK(!eq.timed_out);
        LIFT_CHECK_EQ_U32(eq.exit_code, 2u);
        LIFT_CHECK(eq.err.find("headless 不接受窗口参数") != std::string::npos);
        LIFT_CHECK(eq.err.find("值非法") == std::string::npos);
        LIFT_CHECK(eq.err.find("--ui-input") != std::string::npos);

        const ChildResult sep =
            runChild(exe, {"--present", "headless", "--ui-input", "consume"}, childOptions(root));
        LIFT_CHECK(sep.started);
        LIFT_CHECK(!sep.timed_out);
        LIFT_CHECK_EQ_U32(sep.exit_code, 2u);
        LIFT_CHECK(sep.err.find("headless 不接受窗口参数") != std::string::npos);
        LIFT_CHECK(sep.err.find("值非法") == std::string::npos);
    }

    // ---- ①' `--input-plan <不存在的文件>` ⇒ exit 2 且 stderr **点名那个路径** ----
    //      红了 ⇒ 计划文件被静默忽略（那正是"以为按了、其实没按"的失效方式）。
    g_current_case = "input-plan-missing-file";
    {
        const fs::path missing = root / "no-such-plan.plan";
        const ChildResult r = runChild(exe, {"--input-plan", pathToUtf8(missing)}, childOptions(root));
        LIFT_CHECK(r.started);
        LIFT_CHECK(!r.timed_out);
        LIFT_CHECK_EQ_U32(r.exit_code, 2u);
        LIFT_CHECK(r.err.find("未知参数") == std::string::npos);
        LIFT_CHECK(r.err.find("no-such-plan.plan") != std::string::npos);
    }

    // ---- ①'' `--input-plan <语法错文件>` ⇒ exit 2 且**点出行号**（`<path>:4:`）----
    //      红了 ⇒ 语法错被当成"空计划"吞掉，或者报错不带位置（人无法修）。
    //      ★ 坏行放在第 4 行：`expect …` 是**图语法**，plan 只认三条指令 ⇒ 未知指令。
    g_current_case = "input-plan-syntax-error-names-line";
    {
        const fs::path bad = root / "bad.plan";
        {
            std::ofstream out(bad, std::ios::binary);
            out << "# 语法错：第 4 行是图语法（--input-plan 不认）\n";
            out << "await script TITLE\n";
            out << "click 1 2\n";
            out << "expect screen changed\n";
        }
        const ChildResult r = runChild(exe, {"--input-plan", pathToUtf8(bad)}, childOptions(root));
        LIFT_CHECK(r.started);
        LIFT_CHECK(!r.timed_out);
        LIFT_CHECK_EQ_U32(r.exit_code, 2u);
        LIFT_CHECK(r.err.find("未知参数") == std::string::npos);
        // 行号 = 文件里第 4 行 —— 判据是"报错点出**那一行**"，不是"有行号"。
        LIFT_CHECK(r.err.find("bad.plan:4:") != std::string::npos);
        LIFT_CHECK(r.err.find("未知指令") != std::string::npos);
        LIFT_CHECK(r.err.find("expect") != std::string::npos);
    }

    // ---- ①''' 旧指令一律**解析期硬错**：`await always` / `await presents <n>` / `key` ----
    //      红了 ⇒ 旧配方被静默接受（"按了但语义不对"），或报错不点名行 / 不点名"未知指令"。
    g_current_case = "input-plan-rejects-old-instructions";
    {
        struct OldInstr {
            const char* name;
            const char* text;
        };
        const OldInstr kOld[] = {
            {"always.plan", "await always\n"},
            {"presents.plan", "await presents 120\n"},
            {"key.plan", "key 13\n"},
        };
        for (const OldInstr& o : kOld) {
            const fs::path p = root / o.name;
            {
                std::ofstream out(p, std::ios::binary);
                out << o.text;
            }
            const ChildResult r = runChild(exe, {"--input-plan", pathToUtf8(p)}, childOptions(root));
            LIFT_CHECK(r.started);
            LIFT_CHECK(!r.timed_out);
            LIFT_CHECK_EQ_U32(r.exit_code, 2u);
            LIFT_CHECK(r.err.find("未知参数") == std::string::npos);
            LIFT_CHECK(r.err.find(std::string(o.name) + ":1:") != std::string::npos);
            LIFT_CHECK(r.err.find("未知指令") != std::string::npos);
        }
    }

    // ---- ①'''' 缺参数 / 负毫秒 / 单位后缀 / `=left` ⇒ exit 2 + `文件:行号:` ----
    g_current_case = "input-plan-rejects-bad-arguments";
    {
        struct BadArg {
            const char* name;
            const char* text;
            const char* word;
        };
        const BadArg kBads[] = {
            {"neg.plan", "wait -5\n", "正整数"},
            {"unit.plan", "wait 500 wall_ms\n", "wait"},
            {"missing.plan", "click 1\n", "缺参数"},
            {"eqleft.plan", "click 1 2 =left\n", "按钮词"},
            // 行内 `timeout` 的缺参 / 非数字 ⇒ 同样是 exit 2 + 行号（⛔ 不许静默当成 0）。
            {"to-missing.plan", "await script TITLE timeout\n", "毫秒数"},
            {"to-nan.plan", "await script TITLE timeout abc\n", "正整数"},
        };
        for (const BadArg& b : kBads) {
            const fs::path p = root / b.name;
            {
                std::ofstream out(p, std::ios::binary);
                out << b.text;
            }
            const ChildResult r = runChild(exe, {"--input-plan", pathToUtf8(p)}, childOptions(root));
            LIFT_CHECK(r.started);
            LIFT_CHECK(!r.timed_out);
            LIFT_CHECK_EQ_U32(r.exit_code, 2u);
            LIFT_CHECK(r.err.find("未知参数") == std::string::npos);
            LIFT_CHECK(r.err.find(std::string(b.name) + ":1:") != std::string::npos);
            LIFT_CHECK(r.err.find(b.word) != std::string::npos);
        }
    }

    // ---- ①''''' `timeout` **只能写在 `await …` 那一行的末尾**：跨行 / 挂在 `wait` / `click` 上 ⇒
    //      exit 2 + 点名行号 + 文案指向修法。
    //      红了 ⇒ 旧写法（"守上一条指令"）被静默接受，或者用户看不出该怎么改。
    g_current_case = "input-plan-rejects-stray-timeout";
    {
        struct StrayTimeout {
            const char* name;
            const char* text;
            const char* want_line;  // `文件:行:`
            const char* word;       // 报错里必须出现的词（修法 / 原因）
            const char* word2;      // 可空：第二条必须出现的词（`wait` 的文案要点名改写后的整行）
        };
        const StrayTimeout kStray[] = {
            {"crossline.plan", "await script TITLE\ntimeout 30000\nclick 1 2\n", ":2:", "末尾", nullptr},
            {"waitto.plan", "wait 5000 timeout 9000\n", ":1:", "固定时长", "写回 `wait 5000`"},
            {"clickto.plan", "click 1186 373 timeout 5000\n", ":1:", "没有可等待的东西", nullptr},
            {"quitto.plan", "quit timeout 5000\n", ":1:", "没有可等待的东西", nullptr},
        };
        for (const StrayTimeout& s : kStray) {
            const fs::path p = root / s.name;
            {
                std::ofstream out(p, std::ios::binary);
                out << s.text;
            }
            const ChildResult r = runChild(exe, {"--input-plan", pathToUtf8(p)}, childOptions(root));
            LIFT_CHECK(r.started);
            LIFT_CHECK(!r.timed_out);
            LIFT_CHECK_EQ_U32(r.exit_code, 2u);
            LIFT_CHECK(r.err.find("未知参数") == std::string::npos);
            LIFT_CHECK(r.err.find(std::string(s.name) + s.want_line) != std::string::npos);
            LIFT_CHECK(r.err.find(s.word) != std::string::npos);
            if (s.word2 != nullptr) LIFT_CHECK(r.err.find(s.word2) != std::string::npos);
        }
    }

    // ---- ② `--cursor-warp` + 缺省呈现档（headless）⇒ exit 2，且是"headless 不接受窗口参数"----
    //      ⛔ 不是 `未知参数`：那正是改动前的形状（用户"无法指定"的现场）。
    g_current_case = "headless-rejects-cursor-warp";
    {
        const ChildResult r =
            runChild(exe, {"--cursor-warp", "--present", "headless"}, childOptions(root));
        LIFT_CHECK(r.started);
        LIFT_CHECK(!r.timed_out);
        LIFT_CHECK_EQ_U32(r.exit_code, 2u);
        LIFT_CHECK(r.err.find("未知参数") == std::string::npos);
        LIFT_CHECK(r.err.find("--present headless") != std::string::npos);
        LIFT_CHECK(r.err.find("--cursor-warp") != std::string::npos);
        // 启动前拒绝 ⇒ **一行运行摘要都没有**（不是"跑了一部分才停"）
        LIFT_CHECK(!hasLineStartingWith(r.out, "stop"));
        LIFT_CHECK(!hasLineStartingWith(r.out, "insns"));
    }

    // ---- ②' `--scale`：垃圾值 / 缺值 ⇒ exit 2 且**点名参数**（⛔ 不是"未知参数"，⛔ 不静默当缺省）----
    //      红了 ⇒ 用户给了非法倍率却跑出一个静默的缺省窗口（"看起来生效了、其实没生效"）。
    g_current_case = "scale-bogus-values-rejected";
    {
        struct BadScale {
            const char* arg;   // `--scale` 之后那个值（UTF-8 token，原样交给子进程）
            const char* word;  // stderr 里必须出现的词（值本身）
        };
        const BadScale kBads[] = {
            {"0", "0"},     {"-1", "-1"}, {"nan", "nan"}, {"inf", "inf"},
            {"abc", "abc"}, {"1.5x", "1.5x"},
        };
        for (const BadScale& b : kBads) {
            const ChildResult r =
                runChild(exe, {"--scale", b.arg, "--present", "headless"}, childOptions(root));
            LIFT_CHECK(r.started);
            LIFT_CHECK(!r.timed_out);
            LIFT_CHECK_EQ_U32(r.exit_code, 2u);
            LIFT_CHECK(r.err.find("未知参数") == std::string::npos);
            LIFT_CHECK(r.err.find("--scale") != std::string::npos);
            LIFT_CHECK(r.err.find(b.word) != std::string::npos);
            // 启动前拒绝 ⇒ 一行运行摘要都没有。
            LIFT_CHECK(!hasLineStartingWith(r.out, "stop"));
        }
        // 缺值：`--scale` 后面没有东西 ⇒ exit 2 + `需要一个值` + 点名前缀（与其它 flag 同一句）。
        const ChildResult missing = runChild(exe, {"--scale"}, childOptions(root));
        LIFT_CHECK(missing.started);
        LIFT_CHECK(!missing.timed_out);
        LIFT_CHECK_EQ_U32(missing.exit_code, 2u);
        LIFT_CHECK(missing.err.find("--scale") != std::string::npos);
        LIFT_CHECK(missing.err.find("需要一个值") != std::string::npos);
    }

    // ---- ②'' `--scale` 在 headless 下**接受但不生效**（⛔ 不进"headless 不接受窗口参数"那一类）----
    //      ★ 不判总退出码：这条命令之后本臂仍会因"临时 cwd 里没有执行源 / 没有 GPU"exit 2 ——
    //        判据是"`--scale` 这一关过去了"，⛔ 不拿总退出码冒充。readout 那一行（`scale: …`）
    //        在 GPU bring-up 之后才打（本用例**不假定 GPU 在场**）⇒ 那一面由 pure 用例
    //        `window-scale/headless-readout` 钉。
    g_current_case = "scale-headless-accepted";
    {
        const ChildResult r =
            runChild(exe, {"--scale", "1.5", "--present", "headless"}, childOptions(root));
        LIFT_CHECK(r.started);
        LIFT_CHECK(!r.timed_out);
        LIFT_CHECK_EQ_U32(r.exit_code, 2u);  // 仍会因别的原因退出；下面三条才是判据
        LIFT_CHECK(r.err.find("未知参数") == std::string::npos);
        LIFT_CHECK(r.err.find("--scale") == std::string::npos);           // 没有对 --scale 的报错
        // ⛔ 不是被当成窗口参数拒绝（判据是那句话，不是"出现过 --present headless"：
        //    无 GPU 的机器上拒绝启动的文案里**也会**提到 `--present headless`）。
        LIFT_CHECK(r.err.find("不接受窗口参数") == std::string::npos);
        // 对照组：`auto` 同样被接受（数值与 auto 走同一个分支）。
        const ChildResult ra =
            runChild(exe, {"--scale", "auto", "--present", "headless"}, childOptions(root));
        LIFT_CHECK(ra.started);
        LIFT_CHECK(!ra.timed_out);
        LIFT_CHECK(ra.err.find("未知参数") == std::string::npos);
        LIFT_CHECK(ra.err.find("--scale") == std::string::npos);
    }

    // ---- ②''' `--scale-filter`：未知值 / 缺值 ⇒ exit 2 且**点名参数**（⛔ 不是"未知参数"，
    //      ⛔ 不静默当缺省）----
    //   判据（红了意味着什么）：用户要 nearest 做 A/B / 回归对照却得到 linear（或反过来）——
    //   "以为在对照、其实两跑同一档"，而且截图与像素断言会一起变成不可信的对照。
    g_current_case = "scale-filter-bogus-values-rejected";
    {
        // 值域只有两个、大小写敏感、整串精确匹配（`--scale auto` 同一口径）。
        const char* const kBads[] = {"LINEAR", "Linear", "NEAREST", "Nearest", "nearestx", "lin", "blur", "0", ""};
        for (const char* bad : kBads) {
            const ChildResult r =
                runChild(exe, {"--scale-filter", bad, "--present", "headless"}, childOptions(root));
            LIFT_CHECK(r.started);
            LIFT_CHECK(!r.timed_out);
            LIFT_CHECK_EQ_U32(r.exit_code, 2u);
            LIFT_CHECK(r.err.find("未知参数") == std::string::npos);
            LIFT_CHECK(r.err.find("--scale-filter") != std::string::npos);
            // 拒绝原因点名值域（人照它改）——非空值要求原样出现在 stderr 里。
            if (bad[0] != '\0') LIFT_CHECK(r.err.find(bad) != std::string::npos);
            LIFT_CHECK(!hasLineStartingWith(r.out, "stop"));  // 启动前拒绝 ⇒ 没有运行摘要
        }
        // 缺值：`--scale-filter` 后面没有东西 ⇒ exit 2 + `需要一个值` + 点名前缀。
        const ChildResult missing = runChild(exe, {"--scale-filter"}, childOptions(root));
        LIFT_CHECK(missing.started);
        LIFT_CHECK(!missing.timed_out);
        LIFT_CHECK_EQ_U32(missing.exit_code, 2u);
        LIFT_CHECK(missing.err.find("--scale-filter") != std::string::npos);
        LIFT_CHECK(missing.err.find("需要一个值") != std::string::npos);
    }

    // ---- ②''' `--scale-filter` 在 headless 下**接受**（与 `--scale` 同一口径：无窗口可施但
    //      不拒绝、⛔ 不进"headless 不接受窗口参数"那一类）----
    //      ★ 不判总退出码（原因同 ②''：这一跑还会因缺执行源 / 缺 GPU 退出）。
    g_current_case = "scale-filter-headless-accepted";
    {
        const ChildResult r =
            runChild(exe, {"--scale", "1.5", "--scale-filter", "nearest", "--present", "headless"},
                     childOptions(root));
        LIFT_CHECK(r.started);
        LIFT_CHECK(!r.timed_out);
        LIFT_CHECK(r.err.find("未知参数") == std::string::npos);
        LIFT_CHECK(r.err.find("--scale-filter") == std::string::npos);  // 没有对它报错
        LIFT_CHECK(r.err.find("不接受窗口参数") == std::string::npos);
    }

    // ---- ③ 错拼（wrap ≠ warp）**仍然**是未知参数：钉住"② 不是靠接受任何参数"----
    g_current_case = "typo-cursor-wrap-still-unknown";
    {
        const ChildResult r =
            runChild(exe, {"--cursor-wrap", "--present", "headless"}, childOptions(root));
        LIFT_CHECK(r.started);
        LIFT_CHECK(!r.timed_out);
        LIFT_CHECK_EQ_U32(r.exit_code, 2u);
        LIFT_CHECK(r.err.find("未知参数") != std::string::npos);
        LIFT_CHECK(r.err.find("--cursor-wrap") != std::string::npos);
    }

    // ---- ④ `--sound`：**接受** + 一条一次性说明（与代码一致：真的进静音门），⛔ 不是 `未知参数` ----
    //      ★ 不判退出码：`--sound` 之后本臂仍会因"没有执行源 / 没有 GPU"而 exit 2 —— 那与
    //        `--sound` 无关，判据是"它被接受 + 说了实话"，在 GPU bring-up 之前就已成立。
    //      ★ **洞 6**：这条用例**不再**拿"说明文本里含 `muted=0`"当证据（自证式断言：接线丢了它照绿）。
    //        证据 = `aot-audio-wiring` 读收口摘要的 `audio     : … muted=0`（含 `muted=1` 对照组）。
    //        这里保留的文本判据只有一条**反向**的：⛔ 不许把接线前的谎话写回来。
    g_current_case = "sound-accepted-with-once-notice";
    {
        const ChildResult r = runChild(exe, {"--sound"}, childOptions(root));
        LIFT_CHECK(r.started);
        LIFT_CHECK(!r.timed_out);
        LIFT_CHECK(r.err.find("未知参数") == std::string::npos);
        LIFT_CHECK(countOccurrences(r.err, kSoundNoticeMarker) == 1u);
        // 反向判据：⛔ 不许再写回"没有音频管线 / 不会有声音"（接线前的形状）。
        LIFT_CHECK(r.err.find("没有音频管线") == std::string::npos);
        LIFT_CHECK(r.err.find("不会有声音") == std::string::npos);
        // ③ 跟踪单 id 必须**是真的**（`REQ-` + 26 位 ULID），⛔ 不许把占位符发出去。
        //    ★ 按**形态**判、不锚具体 id：换单号不该让守卫红，但留着 `TODO(` 必须红。
        LIFT_CHECK(r.err.find("REQ-") != std::string::npos);
        LIFT_CHECK(r.err.find("TODO(") == std::string::npos);
    }

    // ---- ⑥ S1：`--audio sdl` **被接受**（旧文案"本臂只有 off"必须消失）----
    //      ★ 仍然 exit 2 是**别的**原因（临时 cwd 里没有执行源 / 没有 GPU）—— 判据只判"音频这一关
    //        过去了没有"，⛔ 不拿总退出码冒充。
    g_current_case = "audio-sdl-accepted";
    {
        const ChildResult r = runChild(exe, {"--audio", "sdl", "--present", "headless"}, childOptions(root));
        LIFT_CHECK(r.started);
        LIFT_CHECK(!r.timed_out);
        LIFT_CHECK(r.err.find("未知参数") == std::string::npos);
        // 旧形状：`--audio %s 未实现（本臂只有 off：没有音频设备 / 喂料线程）`。
        LIFT_CHECK(r.err.find("未实现（本臂只有 off") == std::string::npos);
        LIFT_CHECK(r.err.find("值非法") == std::string::npos);
    }

    // ---- ⑥' `--audio virtual` **必须被拒**（该档已于 2026-11-22 移除）----
    //      ★ 判据（三条都要）：
    //        ① exit 2（⛔ 不是"接受后什么都不做"）；
    //        ② stderr 认得出是这个参数错了：含 `--audio`，且**不是** `未知参数`；
    //        ③ stderr **点名合法值** `off / sdl`（人照它改），而且**不再提 virtual**。
    //      红了 ⇒ 已移除的档位又被收回来（它没有 feeder 线程，会被当成"产品态音频"读数）。
    g_current_case = "audio-virtual-rejected";
    {
        const ChildResult r =
            runChild(exe, {"--audio", "virtual", "--present", "headless"}, childOptions(root));
        LIFT_CHECK(r.started);
        LIFT_CHECK(!r.timed_out);
        LIFT_CHECK_EQ_U32(r.exit_code, 2u);
        LIFT_CHECK(r.err.find("未知参数") == std::string::npos);
        LIFT_CHECK(r.err.find("--audio") != std::string::npos);
        LIFT_CHECK(r.err.find("virtual") != std::string::npos);  // 回显用户给的值
        LIFT_CHECK(r.err.find("off / sdl") != std::string::npos);  // ★ 点名合法值（逐字同 --help）
        LIFT_CHECK(r.err.find("virtual / sdl") == std::string::npos);  // ⛔ 旧值域不许回来
    }

    // ---- ⑥'' 非法值**仍然** exit 2，且点名合法值域（⛔ 不许"认不出就当 off"）----
    g_current_case = "audio-bogus-value-rejected";
    {
        const ChildResult r =
            runChild(exe, {"--audio", "bogus", "--present", "headless"}, childOptions(root));
        LIFT_CHECK(r.started);
        LIFT_CHECK(!r.timed_out);
        LIFT_CHECK_EQ_U32(r.exit_code, 2u);
        LIFT_CHECK(r.err.find("未知参数") == std::string::npos);
        LIFT_CHECK(r.err.find("--audio") != std::string::npos);
        LIFT_CHECK(r.err.find("bogus") != std::string::npos);
        // 合法值域必须**写在报错里**（人照它改），不是只说"非法"。
        LIFT_CHECK(r.err.find("off") != std::string::npos);
        LIFT_CHECK(r.err.find("sdl") != std::string::npos);
        LIFT_CHECK(r.err.find("off / sdl") != std::string::npos);
        LIFT_CHECK(r.err.find("virtual") == std::string::npos);  // 已移除的档位不许再出现在值域里
    }

    // ---- ⑥''' `--audio-dump` 缺值 ⇒ exit 2 + 点名这个 flag（⛔ 不静默当成"没有 dump"）----
    g_current_case = "audio-dump-needs-value";
    {
        const ChildResult r = runChild(exe, {"--audio-dump"}, childOptions(root));
        LIFT_CHECK(r.started);
        LIFT_CHECK(!r.timed_out);
        LIFT_CHECK_EQ_U32(r.exit_code, 2u);
        LIFT_CHECK(r.err.find("--audio-dump") != std::string::npos);
        LIFT_CHECK(r.err.find("需要一个值") != std::string::npos);
        LIFT_CHECK(r.err.find("未知参数") == std::string::npos);
    }

    // ---- ⑦ `--profile-slow-frames`：**依赖 `--profile`**（缺了它 ⇒ exit 2 并点名两者）----
    //      红了 ⇒ 一个拿不到墙钟尺的仪器被静默接受（表里 `wall_ms` 全 0 / 表根本不出现），
    //      而那正是"以为在量、其实没量"。★ 值 0（显式关）同样要带 `--profile`：判据只有一条。
    g_current_case = "profile-slow-frames-requires-profile";
    {
        const ChildResult r =
            runChild(exe, {"--profile-slow-frames", "50", "--present", "headless"}, childOptions(root));
        LIFT_CHECK(r.started);
        LIFT_CHECK(!r.timed_out);
        LIFT_CHECK_EQ_U32(r.exit_code, 2u);
        LIFT_CHECK(r.err.find("未知参数") == std::string::npos);
        LIFT_CHECK(r.err.find("--profile-slow-frames") != std::string::npos);
        LIFT_CHECK(r.err.find("同时给 --profile") != std::string::npos);
        // 启动前拒绝 ⇒ 一行运行摘要都没有（不是"跑了一部分才停"）
        LIFT_CHECK(!hasLineStartingWith(r.out, "stop"));
        const ChildResult r0 =
            runChild(exe, {"--profile-slow-frames", "0", "--present", "headless"}, childOptions(root));
        LIFT_CHECK(r0.started);
        LIFT_CHECK(!r0.timed_out);
        LIFT_CHECK_EQ_U32(r0.exit_code, 2u);
        LIFT_CHECK(r0.err.find("未知参数") == std::string::npos);
        LIFT_CHECK(r0.err.find("--profile-slow-frames") != std::string::npos);
    }

    // ---- ⑦' 取值：负数 / 小数 / 单位 / 垃圾 / 缺值 ⇒ exit 2 且**点名这个 flag** ----
    //      红了 ⇒ `strtoull("-1")` 式的静默回绕（负号被当成巨大毫秒数）或"认不出就当 0"
    //      （= 让人以为开着仪器、其实一次都不记）。
    g_current_case = "profile-slow-frames-bad-values";
    {
        struct BadSlow {
            const char* arg;
            const char* word;  // stderr 里必须出现的那个值（人照它改）
        };
        const BadSlow kBads[] = {{"abc", "abc"}, {"-1", "-1"}, {"1.5", "1.5"}, {"50ms", "50ms"}};
        for (const BadSlow& b : kBads) {
            const ChildResult r = runChild(exe,
                                           {"--profile", "--profile-slow-frames", b.arg, "--present", "headless"},
                                           childOptions(root));
            LIFT_CHECK(r.started);
            LIFT_CHECK(!r.timed_out);
            LIFT_CHECK_EQ_U32(r.exit_code, 2u);
            LIFT_CHECK(r.err.find("未知参数") == std::string::npos);
            LIFT_CHECK(r.err.find("--profile-slow-frames") != std::string::npos);
            LIFT_CHECK(r.err.find(b.word) != std::string::npos);
        }
        // 缺值：`--profile-slow-frames` 后面没有东西 ⇒ exit 2 + `需要一个值` + 点名（与其它 flag 同一句）。
        const ChildResult missing =
            runChild(exe, {"--profile", "--profile-slow-frames"}, childOptions(root));
        LIFT_CHECK(missing.started);
        LIFT_CHECK(!missing.timed_out);
        LIFT_CHECK_EQ_U32(missing.exit_code, 2u);
        LIFT_CHECK(missing.err.find("--profile-slow-frames") != std::string::npos);
        LIFT_CHECK(missing.err.find("需要一个值") != std::string::npos);
    }

    // ---- ⑦'' `--profile --profile-slow-frames 50` **被接受**（⛔ 不是 `未知参数`）----
    //      ★ 不判总退出码：之后本臂仍会因"临时 cwd 里没有执行源 / 没有 GPU"exit 2 ——
    //        判据是"这一关过去了"，⛔ 不拿总退出码冒充（与 `scale-headless-accepted` 同一条处置）。
    g_current_case = "profile-slow-frames-accepted-with-profile";
    {
        const ChildResult r = runChild(exe,
                                       {"--profile", "--profile-slow-frames", "50", "--present", "headless"},
                                       childOptions(root));
        LIFT_CHECK(r.started);
        LIFT_CHECK(!r.timed_out);
        LIFT_CHECK(r.err.find("未知参数") == std::string::npos);
        LIFT_CHECK(r.err.find("--profile-slow-frames") == std::string::npos);
    }

    // ---- ⑤ 对照组：**不给** `--sound` ⇒ 那条说明不出现（同一份 stderr 面）----
    g_current_case = "default-no-sound-notice";
    {
        const ChildResult r =
            runChild(exe, {"--present", "headless", "--present-hidden"}, childOptions(root));
        LIFT_CHECK(r.started);
        LIFT_CHECK(!r.timed_out);
        LIFT_CHECK_EQ_U32(r.exit_code, 2u);
        LIFT_CHECK(r.err.find("--present headless") != std::string::npos);
        LIFT_CHECK(countOccurrences(r.err, kSoundNoticeMarker) == 0u);
        // ★ S4 的对照：**不给** `--input-plan` ⇒ 那一行摘要**不出现**（缺省档逐字节不变的机械判据）。
        LIFT_CHECK(r.out.find("input-plan:") == std::string::npos);
        LIFT_CHECK(r.err.find("input-plan:") == std::string::npos);
    }

    fs::remove_all(root, ec);

    return lift_test::finish("cli-aot-flags");
}
