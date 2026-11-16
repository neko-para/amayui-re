// emulator-lift · AOT 音频的 **S0 定档谓词 + S1 参数面**（pure 档：不碰 SDL 设备、不读任何资产）
//
// 判据（红了意味着什么）：
//   ① **S0 真值表**：`audioThreadMode(mode, realTime, hasMachine)` —— 三者全真（Sdl + 墙钟 +
//      有 Machine）才进真 feeder 线程。★ 专门断言 **(Sdl, realTime=true, hasMachine=false) == false**
//      —— 那一格就是 AOT 臂（`cpu_.machine == nullptr` 是**有意**的，见 `aot_host.h` 的文件头）。
//      红了 ⇒ AOT 会先起 mixer 线程、等 `setupFeederContext` 失败之后才回退成 D 泵，而
//      `pump()` 在那段时间里恒 `Idle`（penalty = 起播前整段不推进 + 每次 run 一条误导性 Warn
//      "feeder 真线程启动失败"）。
//   ② **S1 值域**：`parseAudioMode` 只认 `off` / `virtual` / `sdl`（大小写敏感、不许前后缀 / 空串），
//      非法值返回 `!ok` + **非空** error。红了 ⇒ 非法 `--audio` 被当成 off 静默接受（"以为有声"），
//      或合法值被拒（"无法指定"）—— 两种都比红难发现。
//   ③ **S1 静音门**：`AudioEngine` 的 `muted()` 与 `sound` **反相**（缺省 `--sound` 关 ⇒ muted）。
//      红了 ⇒ `--sound` 没接进 `mute_`（AOT 曾经的形状：`aot_host.cpp` 硬写 `sound=false`）⇒
//      就算设备 / 混音都跑通，交付缓冲仍被交付点 memset 成 0。
//   ④ **S1 dump 提升**：`configure(Off, <非空路径>, …)` ⇒ `configuredMode()` 变 `Virtual`（规则在
//      `AudioEngine::configure`，唯一一份；这里钉住它没漂）。红了 ⇒ `--audio-dump` 给了却没有虚拟
//      设备时钟 ⇒ 那 44 字节 WAV 永远只有头（S2 的"有输出"判据消失）。
//   ⑤ **S2 缺省档**：`configure(Off, "", false)` + `begin()` + `end()` 之后 `enabled()` 仍 false、
//      `mode()` 仍 `Off`、`threadMode()` 仍 false。红了 ⇒ **默认档**（不带任何新 flag）会开设备 /
//      起泵 / 打音频读数 ⇒ 默认档输出不再逐字不变（本单最硬的一条）。
//
// ★ 为什么是 pure 档：`add_test` 在 CMakeLists 的最外层（§12.4 判据 = 注册位置）⇒ 默认 `ctest` 就跑；
//   本文件不碰 SDL 设备（`AudioMode::Sdl` 只作为**谓词输入**出现，不 `begin()`）、不读 LFS / 安装树。
// ★ 为什么把判据抽成纯函数（而不是"begin 之后看 threadMode()"）：后者在本机没有音频设备时会因
//   `openSdl` 失败退回 `Virtual` 而**假绿**（`audio.cpp` 的 `begin()` 回退块）。
#include <cstdint>
#include <string>

#include "check.h"
#include "runtime/audio_policy.h"
#include "runtime/machine.h"

namespace {

bool initMachine(lift::Machine& m, std::string* err) {
    if (!m.initBare(err)) return false;
    if (!m.prepareStack(err)) return false;
    return true;
}

}  // namespace

int main() {
    using namespace lift_test;

    // ---- ① S0 定档谓词：3 个输入的真值表（8 格全覆盖）----
    g_current_case = "s0-thread-mode-truth-table";
    {
        struct Row {
            lift::AudioMode mode;
            bool real_time;
            bool has_machine;
            bool want;
        };
        const Row kRows[] = {
            // mode      realTime  hasMachine  want（thread 档）
            {lift::AudioMode::Off, false, false, false},
            {lift::AudioMode::Off, true, true, false},
            {lift::AudioMode::Virtual, false, false, false},
            {lift::AudioMode::Virtual, true, true, false},
            {lift::AudioMode::Sdl, false, false, false},
            {lift::AudioMode::Sdl, false, true, false},
            {lift::AudioMode::Sdl, true, true, true},
            // ★ 这一格 = AOT 臂：真设备 + 墙钟 + **没有 Machine** ⇒ 必须走主线程 D 泵。
            {lift::AudioMode::Sdl, true, false, false},
        };
        for (const Row& r : kRows) {
            LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(lift::audioThreadMode(r.mode, r.real_time, r.has_machine)),
                              static_cast<std::uint32_t>(r.want ? 1u : 0u));
        }
        // AOT 的判据用**一句话**再钉一次（读代码的人先看到它，再看上面那张表）：
        //   `has_machine == false` ⇒ 无论档位 / 时钟，恒 D 泵。
        for (lift::AudioMode m : {lift::AudioMode::Off, lift::AudioMode::Virtual, lift::AudioMode::Sdl}) {
            for (int rt = 0; rt < 2; ++rt) {
                LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(lift::audioThreadMode(m, rt != 0, false)), 0u);
            }
        }
    }

    // ---- ② S1 值域：三个合法值逐个对；非法值一律 `!ok` 且 error 非空 ----
    g_current_case = "s1-audio-mode-parse-accepts-closed-set";
    {
        struct Ok {
            const char* text;
            lift::AudioMode want;
        };
        const Ok kOk[] = {
            {"off", lift::AudioMode::Off},
            {"virtual", lift::AudioMode::Virtual},
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
        const char* const kBad[] = {"Off", "OFF", "SDL", "Sdl", "Virtual", "virtual ", " sdl", "offf",
                                    "",    "0",   "--audio", "none", "silent"};
        for (const char* bad : kBad) {
            const lift::AudioModeParse p = lift::parseAudioMode(bad);
            LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(p.ok ? 1u : 0u), 0u);
            LIFT_CHECK(p.error != nullptr);
        }
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

        // `--audio-dump <非空>` + `--audio off` ⇒ 档位**提升**成 Virtual（否则 dump 没有设备时钟）。
        LIFT_CHECK(m.configureAudio(lift::AudioMode::Off, "lift-aot-audio-flags-not-written.wav",
                                    /*sound=*/false, &err));
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(a.configuredMode()),
                          static_cast<std::uint32_t>(lift::AudioMode::Virtual));
        LIFT_CHECK(a.muted());

        // 显式档位照收（virtual / sdl 都只是配置，不在这里 begin ⇒ 不碰设备）。
        LIFT_CHECK(m.configureAudio(lift::AudioMode::Virtual, std::string(), /*sound=*/true, &err));
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(a.configuredMode()),
                          static_cast<std::uint32_t>(lift::AudioMode::Virtual));
        LIFT_CHECK(m.configureAudio(lift::AudioMode::Sdl, std::string(), /*sound=*/false, &err));
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(a.configuredMode()),
                          static_cast<std::uint32_t>(lift::AudioMode::Sdl));
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
        // end 之后仍是 off：⛔ 不许"收口时把档位改成 virtual"或补开设备。
        LIFT_CHECK(!a.enabled());
        LIFT_CHECK(!a.threadMode());
    }

    return lift_test::finish("aot-audio-flags");
}
