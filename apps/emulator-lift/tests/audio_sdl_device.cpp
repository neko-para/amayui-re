// emulator-lift · **真音频设备档**（配置期 opt-in：`LIFT_AUDIO_SDL_DEVICE` 存在时才注册）
//
// 判据（红了意味着什么）：
//   ① `--audio sdl` 的生命周期在**真设备**上成立：`configure(Sdl, <dump>, /*sound=*/true)` + `begin()`
//      之后 `mode() == Sdl` **且** `snapshot().device == "ok"`（= SDL 设备真的打开了）。
//      红了 ⇒ "sdl 档能跑"只剩"进程没崩"这一句：设备其实没开（退回 virtual / 或根本没打开）。
//      ⛔ 判据**不是**退出码 0，也⛔ 不是 "output 非空"。
//   ② `--sound` 的静音门在真设备档下是开的（`muted() == false`）；同一台引擎改成 `sound=false`
//      再配置一次 ⇒ `muted() == true`（反相，钉住"开关真的进 mute_"）。
//   ③ `end()` 之后 WAV 容器合法：≥ 44 字节、`RIFF`/`WAVE`/`fmt `/`data` 四个标记在位，且
//      `data` 长度 == 文件长度 − 44（`closeDump` 的回填）。红了 ⇒ `--audio-dump` 的落盘路径断了
//      （S2 的"有输出"读不出来）。
//
// ★ **没有可用音频设备 ⇒ SKIP 77**：本机可能没有声卡（无桌面会话 / 服务账号 / 远程会话）。
//   SKIP 是"没测"，⛔ 不是"测过了没崩" —— 所以本档**不注册在默认 ctest 里**，而是配置期 opt-in：
//     `$env:LIFT_AUDIO_SDL_DEVICE=1; cmake --preset windows-x64`
//   之后 `ctest --test-dir out -R audio-sdl-device --output-on-failure`。
//   ⛔ 不要把它写成恒绿：skip 的**唯一**来源是"测试自己先探一次设备、探针打不开"（本机真没声卡）；
//   探针打得开而引擎打不开 ⇒ 下面每一条断言都会**红**（引擎坏掉不会退化成 skip）。
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <SDL3/SDL.h>

#include "check.h"
#include "runtime/machine.h"

namespace {

namespace fs = std::filesystem;

// ★ **先自己探一次设备**，把"本机没有设备"与"引擎打不开设备"分成两件事：
//   探针打不开 ⇒ SKIP 77（真的没设备）；探针打得开而引擎没开 ⇒ **红**（引擎路径坏了）。
//   ⛔ 直接靠"引擎退回 virtual 就 skip"会把一个坏掉的 `openSdl()` 伪装成"本机没声卡"。
bool probeSdlAudioDevice() {
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) return false;
    SDL_AudioSpec spec{};
    spec.format = SDL_AUDIO_S16LE;
    spec.channels = 2;
    spec.freq = 48000;
    SDL_AudioStream* s = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    if (s == nullptr) {
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        return false;
    }
    SDL_DestroyAudioStream(s);
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
    return true;
}

std::uint32_t le32(const std::vector<char>& b, std::size_t off) {
    return static_cast<std::uint32_t>(static_cast<unsigned char>(b[off])) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(b[off + 1])) << 8) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(b[off + 2])) << 16) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(b[off + 3])) << 24);
}

std::vector<char> readAll(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return {};
    return std::vector<char>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

bool initMachine(lift::Machine& m, std::string* err) {
    if (!m.initBare(err)) return false;
    if (!m.prepareStack(err)) return false;
    return true;
}

}  // namespace

int main() {
    using namespace lift_test;

    // ★ 没有设备 ⇒ SKIP 77（"没测"，⛔ 不是"测过没崩"）。探针在**引擎之外**，所以下面每一条都是
    //   "引擎必须做到"的断言（⛔ 不会因为引擎坏掉而退化成 skip）。
    if (!probeSdlAudioDevice()) {
        LIFT_SKIP("本机没有可用的 SDL 音频设备（探针打不开默认播放设备）⇒ 本档 SKIP 77，⛔ 不是通过");
    }

    std::error_code ec;
    const fs::path root = fs::temp_directory_path(ec) / "lift-audio-sdl-device";
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);
    LIFT_CHECK(!ec);
    const fs::path dump = root / "sdl-device.wav";

    std::string err;
    lift::Machine m;
    if (!initMachine(m, &err)) {
        LIFT_CHECK(false);
        return lift_test::finish("audio-sdl-device");
    }
    lift::AudioEngine& a = m.audio();

    g_current_case = "sdl-device-opens-and-dumps";
    LIFT_CHECK(m.configureAudio(lift::AudioMode::Sdl, dump.string(), /*sound=*/true, &err));
    if (!a.begin(&err)) {
        // `begin()` 只在"没 attach / 同一 Machine 第二轮"时失败 —— 那是夹具错，不是设备缺失。
        LIFT_CHECK(false);
        return lift_test::finish("audio-sdl-device");
    }
    // ★ 探针已经证明这台机器有设备 ⇒ **这里不再有 skip 分支**：引擎必须报 `mode=sdl device=ok`。
    //   引擎退回 `Virtual`（`sdl_fallback_`）⇒ 直接红（那是实现坏了，不是机器没声卡）。
    LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(a.mode()), static_cast<std::uint32_t>(lift::AudioMode::Sdl));
    // ★ 机械判据 = `device=ok`（"有输出"的那一面），⛔ 不是"进程没崩"。
    LIFT_CHECK(std::strcmp(a.snapshot().device, "ok") == 0);
    LIFT_CHECK(!a.muted());  // `sound=true` ⇒ 静音门开
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(a.stats().fills), 0);  // 本档只判生命周期（泵 = S3）

    a.end();

    // ---- WAV 容器合法（`closeDump` 回填了长度）----
    const std::vector<char> bytes = readAll(dump);
    LIFT_CHECK(bytes.size() >= 44);
    if (bytes.size() >= 44) {
        LIFT_CHECK(std::memcmp(bytes.data(), "RIFF", 4) == 0);
        LIFT_CHECK(std::memcmp(bytes.data() + 8, "WAVE", 4) == 0);
        LIFT_CHECK(std::memcmp(bytes.data() + 12, "fmt ", 4) == 0);
        LIFT_CHECK(std::memcmp(bytes.data() + 36, "data", 4) == 0);
        LIFT_CHECK_EQ_U32(le32(bytes, 40), static_cast<std::uint32_t>(bytes.size() - 44));
        LIFT_CHECK_EQ_U32(le32(bytes, 4), static_cast<std::uint32_t>(bytes.size() - 8));
    }

    // ---- ② 静音门反相：同一台 Machine 上再 configure 一次（begin 已 end ⇒ 允许）----
    g_current_case = "sound-flag-drives-mute";
    {
        lift::Machine m2;
        if (initMachine(m2, &err)) {
            LIFT_CHECK(m2.configureAudio(lift::AudioMode::Sdl, std::string(), /*sound=*/true, &err));
            LIFT_CHECK(!m2.audio().muted());
            LIFT_CHECK(m2.configureAudio(lift::AudioMode::Sdl, std::string(), /*sound=*/false, &err));
            LIFT_CHECK(m2.audio().muted());
        } else {
            LIFT_CHECK(false);
        }
    }

    fs::remove_all(root, ec);
    return lift_test::finish("audio-sdl-device");
}
