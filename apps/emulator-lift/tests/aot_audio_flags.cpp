// emulator-lift · AOT 音频的 **参数面 + lead + AOT CLI 日志级别**（pure 档）
//
// 判据（红了意味着什么）：
//   ① **布局是结构前提**：`Machine` 的 `attach(..., layout_)` 之后 `feederTebForTest()` 等于
//      `GuestLayout::feeder_teb`；**宿主给了布局但值不可用（0）⇒ `begin(Sdl)` 必须返回 false**
//      （⛔ 不静默退化）。这一条在 `openSdl` 之前判 ⇒ 不需要音频设备，pure 档可跑。
//   ② **S1 值域**：`parseAudioMode` 只认 `off` / `sdl`（大小写敏感、不许前后缀 / 空串），
//      非法值返回 `!ok` + **非空** error，且错误文案**点名** `off / sdl`。
//      ★ `virtual` 已于 2026-11-22 整档移除 ⇒ 它现在是**非法值**。
//      红了 ⇒ 非法 `--audio` 被当成 off 静默接受（"以为有声"），或合法值被拒（"无法指定"）。
//   ②b **lead 参数面**：`kThreadLeadBytes` = 19200（100 ms @ 48k/16/2ch，唯一档 = feeder 线程）。
//      红了 ⇒ 有人把 600 ms 的旧 D 档 lead 加回来，或常量与音频格式漂了。
//   ③ **S1 静音门**：`AudioEngine` 的 `muted()` 与 `sound` **反相**（缺省 `--sound` 关 ⇒ muted）。
//   ④ **dump 不是档位**：`configure(Off, <非空路径>, …)` ⇒ `configuredMode()` 仍是 `Off`，
//      但 `deviceFreeMix()` 为真（设备无关的 dump 路径）。
//   ④b **`--audio sdl` 打不开设备 = 没有输出**：注入一个不存在的 `SDL_AUDIODRIVER` ⇒ `begin()`
//      仍返回 true，但 `mode()==Off`、`enabled()==false`、`snapshot().device=="none"`、`muted==true`。
//   ⑤ **缺省档**：`configure(Off, "", false)` + `begin()` + `end()` 之后 `enabled()` 仍 false。
//   ⑤b **AOT CLI 日志级别**：`LogConfig` 缺省 = `warn`；`logSetLevel` 只认闭集。
//
// ★ 为什么是 pure 档：`add_test` 在 CMakeLists 的最外层（§12.4 判据 = 注册位置）⇒ 默认 `ctest` 就跑；
//   本文件不碰**真**音频设备（`AudioMode::Sdl` 只作为在 `openSdl` 之前就被拒的路径 /
//   显式注入"驱动名不存在"的失败面出现）、不读 LFS / 安装树。
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

    // ---- ① 布局是结构前提：有布局 ⇒ TEB 来自布局；布局不可用 ⇒ begin 响亮失败 ----
    g_current_case = "feeder-layout-is-structural";
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

        // 宿主给了布局但值不可用（0）⇒ `begin(Sdl)` **必须**返回 false，且**在 openSdl 之前**
        // 就返回（pure 档不能碰设备）。这是"⛔ 不静默退化"的判据。
        lift::GuestLayout bad{};
        bad.feeder_teb = 0;  // 唯一的坏值
        lift::AudioEngine a;
        a.attach(&m.cpu(), &m.mem(), &m.clock(), bad);
        LIFT_CHECK(a.configure(lift::AudioMode::Sdl, std::string(), /*sound=*/false, &err));
        std::string begin_err;
        LIFT_CHECK(!a.begin(&begin_err));
        LIFT_CHECK(!begin_err.empty());
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

    // ---- ②b lead：唯一档 = feeder 线程 ⇒ 100 ms（19200 B）----
    g_current_case = "lead-bytes";
    {
        // 48k / 16 bit / 2 ch ⇒ 192000 B/s。
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(lift::kThreadLeadBytes), 19200);
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
        // 仍是 Off，只是这一跑多一条"设备无关的 dump 路径"（墙钟游标 + 交付点写 WAV）。
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
        LIFT_CHECK(a.muted());
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(a.stats().fills), 0);
        a.end();
        // end 之后仍是 off：⛔ 不许"收口时补开设备 / 补打一行看起来有音频的读数"。
        LIFT_CHECK(!a.enabled());
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
