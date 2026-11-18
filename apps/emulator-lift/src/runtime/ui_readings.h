// emulator-lift · UI 脚本的**每帧只读读数**（`--overlay` / `--ui`；B 从旧 overlay.h 迁出）
//
// ★ 这里只有"引擎侧算出来的数"：脚本身份（`script`）与最近 3 s 窗口内每帧的 wall / guest / shim
//   最大值（`worst`）。⛔ 没有颜色 / 字形 / 面板尺寸 / 渲染 —— 那些归 `scripts/ui/hud.js`
//   （display list）与 `ui/raster.cpp`（把 list 变成像素），本头**不画任何东西**。
// ★ 纯逻辑、零 SDL / GPU / 字体类型：`Machine`（生产者）与窗口 sink（消费者）共用这一份定义，
//   pure CTest（`tests/overlay.cpp` 的 `uiScriptPayload` / 3 s 窗口两节）直接 include 它。
//
// ★ 两个字段的口径（与旧 overlay.h 逐字相同，只是换了名字）：
//   · `script` = 当前顶层脚本身份（名字来自引擎名字表；采样点是 VM 循环头，见
//     `runtime/engine_marker.h`）。**从未**采到身份 ⇒ 整行省略；名字读不到（降级）时写
//     `script id:0x%08X（降级）`（⛔ 不编一个名字）。
//   · `worst` = **最近 3 s 窗口**内**每帧** wall / guest / shim 的**最大值**。
//     口径：**每帧 = 相邻两次 Present 之间的 steady_clock 间隔**；窗口按"最新样本往前 3 s"裁。
//     `guest` 只有在**块计时**打开时才有值（`--profile` / profile 窗口）⇒ 没开时 HUD 写 `guest=--`。
//     ⛔ 叠层**不会**为了这一行去打开块计时：那会改变被观测的性能本身。
// ★ 数据流：`Machine`（每帧 / 身份变化时推）→ `PresentSink::setUiReadings` →
//   `present_sdl.cpp` 的 `drawOverlay` 把值打包成 `ui::UiFrameReadings` → JS 脚本每帧重画。
//   ★ 没有"文本缓存 / dirty"这一层了：脚本每帧都跑，所以本头也只存值。
#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <string>

namespace lift {

// ---------------------------------------------------------------------------------------
// 最近 3 s 的"每帧最大值"（纯逻辑：带假时间戳即可测）
// ---------------------------------------------------------------------------------------
// ★ 这是 `worst` 行窗口口径的**唯一定义**（3 s 常量，不提供配置）。
inline constexpr std::chrono::milliseconds kUiWorstWindow{3000};

// 一帧的读数：`at` = 这一帧 Present 的时刻；三者单位都是 ms。
struct UiFrameSample {
    std::chrono::steady_clock::time_point at;
    double wall_ms = 0.0;
    double guest_ms = 0.0;
    double shim_ms = 0.0;
};

struct UiWorst {
    bool has_worst = false;  // 窗口里有 ≥1 个样本
    double wall_max_ms = 0.0;
    double guest_max_ms = 0.0;
    double shim_max_ms = 0.0;
};

// 把一帧压进窗口：先按 `sample.at` 丢掉窗口外的旧样本（窗口 = [at-3s, at]）。
// ★ 时间戳必须单调（调用方用 steady_clock）；倒退的样本直接丢（⛔ 不让窗口判据被搅乱）。
inline void uiWorstPush(std::deque<UiFrameSample>& samples, const UiFrameSample& sample) {
    // 时间戳必须单调（调用方只喂 `steady_clock`）：倒退的样本会让"窗口 = 最新样本往前 3 s"
    // 失去意义 ⇒ 直接丢（⛔ 不排序、不猜哪一条才是"现在"）。
    if (!samples.empty() && sample.at < samples.back().at) return;
    samples.push_back(sample);
    const auto cutoff = sample.at - kUiWorstWindow;
    while (!samples.empty() && samples.front().at < cutoff) samples.pop_front();
}

// 窗口内三个字段各自的最大值；空窗口 ⇒ `has_worst=false`（⛔ 不编 0）。
inline UiWorst uiWorstMax(const std::deque<UiFrameSample>& samples) {
    UiWorst out;
    for (const UiFrameSample& s : samples) {
        out.has_worst = true;
        out.wall_max_ms = std::max(out.wall_max_ms, s.wall_ms);
        out.guest_max_ms = std::max(out.guest_max_ms, s.guest_ms);
        out.shim_max_ms = std::max(out.shim_max_ms, s.shim_ms);
    }
    return out;
}

// ---------------------------------------------------------------------------------------
// 窗口 sink 存下来的那一份（由 `Machine` 推；每帧被脚本读一次）
// ---------------------------------------------------------------------------------------
// 每个字段都带"拿得到吗"的开关：拿不到的**整行省略**（⛔ 不编数字）。
struct UiReadings {
    bool has_script = false;  // 从未采到身份 ⇒ `script` 整行省略（⛔ 不写 "script ?"）
    std::string script;       // "script " **之后**的文本（见 `uiScriptPayload`）
    bool has_worst = false;   // 3 s 窗口里一个样本都没有 ⇒ `worst` 整行省略
    double wall_max_ms = 0.0;
    double guest_max_ms = 0.0;
    bool has_guest = false;  // false ⇒ HUD 写 `guest=--`（块计时没开；⛔ 不写 0.0 冒充读数）
    double shim_max_ms = 0.0;
};

// `script` 行的载荷：正常 = 名字原样；降级 = `id:0x%08X（降级）`。
// ★ 纯函数（可测）：降级形态**不依赖**名字表是否可读。
inline std::string uiScriptPayload(const std::string& name, bool name_degraded, std::uint32_t id) {
    // ★ 降级形态**只用 id**（⛔ 不猜名字）：`MarkerSample.name` 在降级时也是 `id:0x…`，
    //   这里显式格式化一次，保证"名字表读不到"与"名字恰好为空"不会混为一谈。
    if (!name_degraded) return name;
    char buf[64];
    std::snprintf(buf, sizeof(buf), "id:0x%08X（降级）", static_cast<unsigned>(id));
    return std::string(buf);
}

}  // namespace lift
