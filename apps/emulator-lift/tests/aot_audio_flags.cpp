// emulator-lift · AOT 音频的 **S0 定档谓词 + S1 参数面 + lead 参数 + AOT CLI 日志级别**（pure 档）
//
// 判据（红了意味着什么）：
//   ① **S0 真值表**：`audioThreadMode(mode, realTime)` —— **两个事实**（Sdl + 墙钟）才进真 feeder
//      线程。★ 专门断言 **(Sdl, realTime=true) == true** —— 那一格就是 AOT 臂的形状
//      （AOT 没有 Machine，但宿主有同一份 `GuestLayout`；用"有没有 Machine"当"能不能起 feeder"
//      的替身会让 AOT 恒走主线程 D 泵、预读 600 ms ⇒ SE 延迟 ~0.6 s）。
//      红了 ⇒ "AOT 形状不再进 thread 档"（有人把 `has_machine` 一类的门加回来了）。
//   ①b **布局是结构前提**：`Machine` 的 `attach(..., layout_)` 之后 `feederTebForTest()` 等于
//      `GuestLayout::feeder_teb`；**宿主给了布局但值不可用（0）⇒ `begin(Sdl, 墙钟)` 必须返回 false**
//      （⛔ 不静默退回 D 泵）。这一条在 `openSdl` 之前判 ⇒ 不需要音频设备，pure 档可跑。
//   ② **S1 值域**：`parseAudioMode` 只认 `off` / `sdl`（大小写敏感、不许前后缀 / 空串），
//      非法值返回 `!ok` + **非空** error，且错误文案**点名** `off / sdl`。
//      ★ `virtual` 已于 2026-11-22 整档移除 ⇒ 它现在是**非法值**（旧档没有 feeder 线程、
//        会被误读成产品态音频）。
//      红了 ⇒ 非法 `--audio` 被当成 off 静默接受（"以为有声"），或合法值被拒（"无法指定"）
//      —— 两种都比红难发现。
//   ②b **lead 参数面**：`leadBytesFor(thread_mode)` = 19200 / 115200（100 ms / 600 ms @ 48k/16/2ch）。
//      红了 ⇒ thread 档仍用 600 ms 的预读（= 本单要消灭的那 0.6 s 延迟），或两点常量漂了。
//   ③ **S1 静音门**：`AudioEngine` 的 `muted()` 与 `sound` **反相**（缺省 `--sound` 关 ⇒ muted）。
//      红了 ⇒ `--sound` 没接进 `mute_`（AOT 曾经的形状：`aot_host.cpp` 硬写 `sound=false`）⇒
//      就算设备 / 混音都跑通，交付缓冲仍被交付点 memset 成 0。
//   ④ **S1 dump 不是档位**：`configure(Off, <非空路径>, …)` ⇒ `configuredMode()` 仍是 `Off`
//      （⛔ 不再"提升"成某个档），但 `deviceFreeMix()` 为真（设备无关的 dump 路径）。
//      红了 ⇒ `--audio-dump` 给了却没有虚拟设备时钟 ⇒ 那 44 字节 WAV 永远只有头（S2 的
//      "有输出"判据消失）；或 dump 又把一个假档位装回参数面。
//   ④b **`--audio sdl` 打不开设备 = 没有输出**：用 SDL 自己的驱动选择（`SDL_AUDIODRIVER`
//      指向一个不存在的驱动）造一次必败的 `openSdl()` ⇒ `begin()` 仍返回 true，但
//      `mode()==Off`、`enabled()==false`（不跑混音）、`snapshot().device=="none"`、
//      `snapshot().muted==true`、`fills==0`。红了 ⇒ 设备失败被静默当成"有音频"或被当成
//      "没要音频"（前者假产品态，后者让摘要里连 `device=none` 都看不到）。
//      ★ 判据是**引擎状态**；不需要这台机器有/没有声卡（注入的是"驱动名不存在"）。
//   ⑤ **S2 缺省档**：`configure(Off, "", false)` + `begin()` + `end()` 之后 `enabled()` 仍 false、
//      `mode()` 仍 `Off`、`threadMode()` 仍 false。红了 ⇒ **默认档**（不带任何新 flag）会开设备 /
//      起泵 / 打音频读数 ⇒ 默认档输出不再逐字不变（本单最硬的一条）。
//   ⑤b **AOT CLI 日志级别**：`LogConfig` 缺省 = `warn`（默认档输出逐字不变的判据面）；
//      `logSetLevel` 只认闭集、非法值返回 false 且**不改**当前级别。
//      红了 ⇒ `--log-level` 的接线/缺省漂了（`[audio] mode=…` / `play state=` 的可见性随之漂）。
//
// ★ 为什么是 pure 档：`add_test` 在 CMakeLists 的最外层（§12.4 判据 = 注册位置）⇒ 默认 `ctest` 就跑；
//   本文件不碰**真**音频设备（`AudioMode::Sdl` 只作为**谓词输入** / 在 `openSdl` 之前就被拒的路径 /
//   显式注入"驱动名不存在"的失败面出现）、不读 LFS / 安装树。
// ★ 为什么把判据抽成纯函数（而不是"begin 之后看 threadMode()"）：后者在本机没有音频设备时会因
//   `openSdl` 失败而落回 off（`audio.cpp` 的 `begin()`）⇒ 假绿。
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

#include "check.h"
#include "runtime/audio_policy.h"
#include "runtime/log.h"
#include "runtime/machine.h"

namespace {

bool initMachine(lift::Machine& m, std::string* err) {
    if (!m.initBare(err)) return false;
    if (!m.prepareStack(err)) return false;
    return true;
}

// ★ 只在**本进程**里改一个 SDL hint（`SDL_AUDIODRIVER`）：指向一个不存在的驱动 ⇒
//   `SDL_InitSubSystem(SDL_INIT_AUDIO)` 必然失败 ⇒ 覆盖 `--audio sdl` 的失败面。
//   仓库先例：`data/requirements/…` 里用 `SDL_AUDIODRIVER=disk` 取非零 WAV；这里要的是**失败**。
void setSdlAudioDriver(const char* value) {
#if defined(_WIN32)
    _putenv_s("SDL_AUDIODRIVER", value);
#else
    ::setenv("SDL_AUDIODRIVER", value, 1);
#endif
}

}  // namespace

int main() {
    using namespace lift_test;

    // ---- ① S0 定档谓词：2 个输入的真值表（4 格全覆盖）----
    g_current_case = "s0-thread-mode-truth-table";
    {
        struct Row {
            lift::AudioMode mode;
            bool real_time;
            bool want;
        };
        const Row kRows[] = {
            // mode      realTime  want（thread 档）
            {lift::AudioMode::Off, false, false},
            {lift::AudioMode::Off, true, false},
            {lift::AudioMode::Sdl, false, false},   // 虚拟时钟 ⇒ D 泵
            // ★ 这一格 = **AOT 臂**（`--audio sdl` + 墙钟 + 宿主给了 GuestLayout）⇒ 必须 thread。
            //   AOT 没有 Machine，但 `AotHost::layout_` 与 `Machine::layout()` 是同一份 GuestLayout。
            {lift::AudioMode::Sdl, true, true},
        };
        for (const Row& r : kRows) {
            LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(lift::audioThreadMode(r.mode, r.real_time)),
                              static_cast<std::uint32_t>(r.want ? 1u : 0u));
        }
        // 用**一句话**再钉一次（读代码的人先看到它，再看上面那张表）：
        //   `Sdl + 墙钟 ⇒ thread`（这就是用户裁决的机械表达）。
        LIFT_CHECK(lift::audioThreadMode(lift::AudioMode::Sdl, true));
    }

    // ---- ①b 布局是结构前提：有布局 ⇒ TEB 来自布局；布局不可用 ⇒ begin 响亮失败 ----
    g_current_case = "s0-feeder-layout-is-structural";
    {
        std::string err;
        lift::Machine m;
        if (!initMachine(m, &err)) {
            LIFT_CHECK(false);
            return lift_test::finish("aot-audio-flags");
        }
        // `Machine::wire()` 用四参 attach ⇒ 引擎拿到的 TEB 就是布局常量（唯一真源 image.h）。
        LIFT_CHECK_EQ_U32(m.audio().feederTebForTest(), m.layout().feeder_teb);
        LIFT_CHECK_EQ_U32(m.audio().feederTebForTest(), 0x7FFD6000u);
        LIFT_CHECK(m.audio().feederLayoutGivenForTest());

        // 宿主给了布局但值不可用（0）⇒ `begin(Sdl, 墙钟)` **必须**返回 false，且**在 openSdl 之前**
        // 就返回（pure 档不能碰设备）。这是"⛔ 不静默退回 D 泵"的判据。
        lift::GuestLayout bad{};
        bad.feeder_teb = 0;  // 唯一的坏值
        lift::AudioEngine a;
        a.attach(&m.cpu(), &m.mem(), &m.clock(), bad);
        m.clock().setRealTime(true);  // 墙钟：thread 档的第二个事实（void，见 clock.h）
        LIFT_CHECK(m.clock().realTime());
        LIFT_CHECK(a.configure(lift::AudioMode::Sdl, std::string(), /*sound=*/false, &err));
        std::string begin_err;
        LIFT_CHECK(!a.begin(&begin_err));
        LIFT_CHECK(!begin_err.empty());
        LIFT_CHECK(!a.threadMode());
        // 同一台独立引擎：三参 attach（没有布局）⇒ `feederLayoutGivenForTest()` 为假（过渡重载）。
        lift::AudioEngine legacy;
        legacy.attach(&m.cpu(), &m.mem(), &m.clock());
        LIFT_CHECK(!legacy.feederLayoutGivenForTest());
    }

    // ---- ② S1 值域：两个合法值逐个对；非法值一律 `!ok` 且 error 非空、**点名 off / sdl** ----
    g_current_case = "s1-audio-mode-parse-accepts-closed-set";
    {
        struct Ok {
            const char* text;
            lift::AudioMode want;
        };
        const Ok kOk[] = {
            {"off", lift::AudioMode::Off},
            {"sdl", lift::AudioMode::Sdl},
        };
        for (const Ok& o : kOk) {
            const lift::AudioModeParse p = lift::parseAudioMode(o.text);
            LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(p.ok ? 1u : 0u), 1u);
            LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(p.mode), static_cast<std::uint32_t>(o.want));
            LIFT_CHECK(p.error == nullptr);
        }
    }
    g_current_case = "s1-audio-mode-parse-rejects-everything-else";
    {
        // ⛔ 大小写 / 前后缀 / 空串 / 拼错都**不许**落回 off（那会把"以为有声"变成静默失效）。
        // ★ `virtual` / `Virtual` 现在都是**非法值**（该档已于 2026-11-22 移除）。
        const char* const kBad[] = {"Off", "OFF", "SDL", "Sdl", "Virtual", "virtual", "virtual ",
                                    " sdl", "offf", "",    "0",       "--audio", "none", "silent"};
        for (const char* bad : kBad) {
            const lift::AudioModeParse p = lift::parseAudioMode(bad);
            LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(p.ok ? 1u : 0u), 0u);
            LIFT_CHECK(p.error != nullptr);
            // ★ 合法值域必须**写在报错里**（人照它改）：`只能是 off / sdl`。
            LIFT_CHECK(p.error != nullptr && std::strstr(p.error, "off / sdl") != nullptr);
            LIFT_CHECK(p.error != nullptr && std::strstr(p.error, "virtual") == nullptr);
        }
    }

    // ---- ②b lead 参数面：thread 档 = 100 ms（19200 B），泵档 = 600 ms（115200 B）----
    g_current_case = "s0-lead-bytes-by-thread-mode";
    {
        // 48k / 16 bit / 2 ch ⇒ 192000 B/s。
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(lift::kThreadLeadBytes), 19200);
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(lift::kLeadBytes), 115200);
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(lift::leadBytesFor(true)), 19200);
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(lift::leadBytesFor(false)), 115200);
        // 量级判据：thread 档的预读必须比泵档小一个量级（这正是本单要消灭的 0.6 s 延迟）。
        LIFT_CHECK(lift::leadBytesFor(true) * 4u < lift::leadBytesFor(false));
    }

    // ---- ③④ S1：`configure` 的静音门与 dump 提升（真源在 `AudioEngine::configure`）----
    g_current_case = "s1-configure-sound-and-dump-promotion";
    {
        std::string err;
        lift::Machine m;
        if (!initMachine(m, &err)) {
            LIFT_CHECK(false);
            return lift_test::finish("aot-audio-flags");
        }
        lift::AudioEngine& a = m.audio();

        // 缺省档（不带任何新 flag）：不启用 + 静音，dump 路径为空 ⇒ 档位不许被提升。
        LIFT_CHECK(m.configureAudio(lift::AudioMode::Off, std::string(), /*sound=*/false, &err));
        LIFT_CHECK(a.muted());
        LIFT_CHECK(!a.enabled());  // begin 之前 `mode_` 仍是 Off（enabled 只看 mode_）
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(a.configuredMode()),
                          static_cast<std::uint32_t>(lift::AudioMode::Off));

        // `--sound` 等价配置：静音门必须打开（这就是"`--sound` 真的接进 mute_"的那一格）。
        LIFT_CHECK(m.configureAudio(lift::AudioMode::Off, std::string(), /*sound=*/true, &err));
        LIFT_CHECK(!a.muted());
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(a.configuredMode()),
                          static_cast<std::uint32_t>(lift::AudioMode::Off));

        // `--audio-dump <非空>` + `--audio off` ⇒ 档位**不提升**（dump 不是档位）：`configuredMode()`
        // 仍是 Off，只是这一跑多一条"设备无关的 dump 路径"（虚拟时钟 + 交付点写 WAV）。
        // ⛔ `--audio` 的参数面只有 off / sdl（`virtual` 已于 2026-11-22 移除）。
        LIFT_CHECK(m.configureAudio(lift::AudioMode::Off, "lift-aot-audio-flags-not-written.wav",
                                    /*sound=*/false, &err));
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(a.configuredMode()),
                          static_cast<std::uint32_t>(lift::AudioMode::Off));
        LIFT_CHECK(a.deviceFreeMix());
        LIFT_CHECK(a.muted());

        // 显式档位照收（sdl 只是配置，不在这里 begin ⇒ 不碰设备）。
        LIFT_CHECK(m.configureAudio(lift::AudioMode::Sdl, std::string(), /*sound=*/false, &err));
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(a.configuredMode()),
                          static_cast<std::uint32_t>(lift::AudioMode::Sdl));
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(a.mode()),
                          static_cast<std::uint32_t>(lift::AudioMode::Off));  // begin 之前 mode_ 仍是 Off
    }

    // ---- ⑤ S2：缺省档的 begin/end 是**纯 no-op**（这是"默认档逐字不变"在音频这一面的机械判据）----
    g_current_case = "s2-default-off-begin-end-is-noop";
    {
        std::string err;
        lift::Machine m;
        if (!initMachine(m, &err)) {
            LIFT_CHECK(false);
            return lift_test::finish("aot-audio-flags");
        }
        lift::AudioEngine& a = m.audio();
        LIFT_CHECK(m.configureAudio(lift::AudioMode::Off, std::string(), /*sound=*/false, &err));
        LIFT_CHECK(a.begin(&err));
        LIFT_CHECK(!a.enabled());
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(a.mode()), static_cast<std::uint32_t>(lift::AudioMode::Off));
        LIFT_CHECK(!a.threadMode());
        LIFT_CHECK(a.muted());
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(a.stats().fills), 0);
        a.end();
        // end 之后仍是 off：⛔ 不许"收口时补开设备 / 补打一行看起来有音频的读数"。
        LIFT_CHECK(!a.enabled());
        LIFT_CHECK(!a.threadMode());
    }

    // ---- ④b `--audio sdl` 打不开设备 ⇒ **没有音频输出**（不跑混音；摘要 device=none / muted=1）----
    //      ★ 用 SDL 自己的驱动选择注入一次必败的 `openSdl()`（`SDL_AUDIODRIVER` 指向不存在的驱动）：
    //        这样判据不依赖"这台机器有没有声卡"，也不需要改产品代码。仓库先例 = requirements 里的
    //        `SDL_AUDIODRIVER=disk`（那要的是成功；这里要的是失败面）。
    g_current_case = "sdl-device-unavailable-means-no-output";
    {
        std::string err;
        lift::Machine m;
        if (!initMachine(m, &err)) {
            LIFT_CHECK(false);
            return lift_test::finish("aot-audio-flags");
        }
        lift::AudioEngine& a = m.audio();
        setSdlAudioDriver("lift-no-such-audio-driver");
        LIFT_CHECK(m.configureAudio(lift::AudioMode::Sdl, std::string(), /*sound=*/true, &err));
        const bool begun = a.begin(&err);
        setSdlAudioDriver("");  // 复原：本用例之后不再初始化 SDL 音频
        LIFT_CHECK(begun);      // 设备失败**不是** begin 失败（这一跑仍可跑，只是没有声音）
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(a.mode()),
                          static_cast<std::uint32_t>(lift::AudioMode::Off));
        LIFT_CHECK(!a.enabled());        // 没有输出 ⇒ 不跑混音 / 不注册 buffer
        LIFT_CHECK(a.audioRequested());  // 但这一跑**要过**音频 ⇒ 摘要仍要如实写
        LIFT_CHECK(!a.threadMode());
        const lift::AudioEngine::Snapshot snap = a.snapshot();
        LIFT_CHECK(std::strcmp(snap.device, "none") == 0);  // ⛔ 不是 fallback
        LIFT_CHECK(snap.muted);                             // 没有可听 / 可写出的 sink ⇒ muted=1
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(a.stats().fills), 0);
        a.end();
        // `end()` 之后摘要仍要写 `device=none`（`end()` 不许把它改回一个"看起来有设备"的值）。
        LIFT_CHECK(std::strcmp(a.snapshot().device, "none") == 0);
    }

    // ---- ⑤b AOT CLI 日志级别：缺省 warn；值域闭集；非法值不改当前级别 ----
    g_current_case = "cli-log-level-default-and-closed-set";
    {
        // 缺省 = warn（"默认档输出逐字不变"在日志这一面的判据）。
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(lift::logConfig().level),
                          static_cast<std::uint32_t>(lift::LogLevel::Warn));
        // 合法值逐个对（闭集与 `--audio` 同一条纪律：⛔ 不"认不出就当缺省"）。
        const struct {
            const char* text;
            lift::LogLevel want;
        } kOk[] = {
            {"quiet", lift::LogLevel::Quiet}, {"error", lift::LogLevel::Error},
            {"warn", lift::LogLevel::Warn},   {"info", lift::LogLevel::Info},
            {"debug", lift::LogLevel::Debug}, {"trace", lift::LogLevel::Trace},
        };
        for (const auto& o : kOk) {
            LIFT_CHECK(lift::logSetLevel(o.text));
            LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(lift::logConfig().level),
                              static_cast<std::uint32_t>(o.want));
        }
        // 非法值：false 且**不改**当前级别（这里当前是 trace）。
        LIFT_CHECK(!lift::logSetLevel("INFO"));
        LIFT_CHECK(!lift::logSetLevel(""));
        LIFT_CHECK(!lift::logSetLevel("notice"));
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(lift::logConfig().level),
                          static_cast<std::uint32_t>(lift::LogLevel::Trace));
        // 复位到缺省，避免本进程后面的输出（如果有）被改级别。
        LIFT_CHECK(lift::logSetLevel("warn"));
    }

    return lift_test::finish("aot-audio-flags");
}
