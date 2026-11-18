// emulator-lift · `--scale <auto|数值>` 的**唯一一份**口径（纯函数 / SDL-free）
//
// ★ 为什么抽这个头（`AGENTS.md` §10「不写第二份」）：`--scale` 有三个读者 ——
//   · CLI（`aot_main.cpp`）：解析 + 拒绝 + 启动 readout 的打印点；
//   · 窗口 sink（`present_sdl.cpp`）：窗口客户区建多大、`auto` 取哪个 DPI；
//   · pure CTest（`tests/window_scale.cpp`）。
//   窗口尺寸 / 自动 DPI / readout 文案若在臂与 sink 里各写一份，"窗口建了 1920×1080 但
//   readout 说 1280×720"这类漂移是**静默**的（没有任何断言能发现它）⇒ 公式与文案只在这里，
//   两边只做"取值 + 打印"。
//
// ★ 映射公式**不**在这里：窗口像素 → 画面像素仍然只有 `runtime/cursor_map.h` 那一份
//   （`mapWindowPixelToImage` / `letterboxFor`）。本头只决定**窗口客户区有多大**；
//   窗口一大，letterbox 自然得到 scale = 窗口/画面（1.25 / 1.5 都是精确比例），
//   `present_sdl.cpp` 的 `mapMouse` 一行都不用改 —— ⛔ 不许为 `--scale` 另写一套换算。
//
// ★ 范围裁决（2026-11-18，用户口径）：`--scale` **只改窗口，不改帧**。
//   guest 画面恒为 1280×720；窗口变大 ⇒ 画面被**放大**，模糊是**可接受**的（真机也没做特殊处理，
//   同样糊）。⇒ ⛔ 不为此做高分辨率重渲染 / 不改帧尺寸 / 不加额外的 upscale pass；本头里的
//   `image_w/image_h` 永远是被呈现的那一帧的尺寸（生产上 = 1280×720），不随 scale 变。
//
// ★ 缺省 = 1.0 ⇒ 窗口客户区 1280×720（与改动前的 `SDL_CreateWindow(…, 1280, 720, …)`
//   逐字节相同 ⇒ 缺省档行为不变）。
// ★ `--present headless` **不建可见窗口**（只建隐藏的离屏窗口拿 GPU device）⇒ `--scale` 对它
//   无副作用：CLI 接受、但不改隐藏窗口尺寸，也不去查 DPI（readout 里写明"不生效"）。
#pragma once

#include <cstdint>
#include <string>

namespace lift {

// ★ 参考画面 = 1280×720（guest 后缓冲的**唯一**基准；与 `cursor_map.h` 的 `kReferenceFrameW/H`
//   是同一个数，但语义不同：那是"配方的坐标参考系"，这里是"窗口尺寸的基准"）。
//   ⚠ 两者相等是**当前事实**（guest 屏就是 1280×720），⛔ 不要用一个去替换另一个：
//   将来后缓冲换尺寸时，窗口基准不必跟着变。
inline constexpr int kBaseWindowW = 1280;
inline constexpr int kBaseWindowH = 720;
inline constexpr double kDefaultScale = 1.0;

// `auto` 的可用上界：display content scale = DPI / 96（Windows 常见 1.0 / 1.25 / 1.5 / 1.75 / 2.0）。
// > 8 只可能是平台给回了别的东西（不是 DPI）⇒ 当"取不到"处理，⛔ 不拿它去建一个几万像素宽的窗口。
inline constexpr double kMaxAutoScale = 8.0;

// 量化量子：DPI 百分比是 1/100 粒度，但 SDL 返回的是 float（可能是 1.2499999 这类噪声）。
// `auto` 取到的值先四舍五入到 1/10000 ⇒ "readout 上印的数"与"真的拿去算窗口尺寸的数"是同一个。
inline constexpr double kScaleQuantum = 10000.0;

// `--scale` 的取值：缺省 `Fixed 1.0`（= 现状 1280×720）；`auto` = 按显示 DPI 取。
struct ScaleSpec {
    enum class Kind { Fixed, Auto };
    Kind kind = Kind::Fixed;
    double value = kDefaultScale;
};

// `--scale` 的解析结果。`ok == false` 时 `error` 是给人看的原因（CLI 原样拼进报错）。
// 拒绝口径（照现有 CLI 的风格）：非 `auto` 且非**正数** / NaN / Inf / 垃圾 ⇒ exit 2 + 点名 `--scale`。
struct ScaleParse {
    bool ok = false;
    ScaleSpec spec;
    const char* error = "只能是 auto 或正数（例：--scale 1.5）";
};

// 解析 `--scale` 的**唯一**入口（`auto` 大小写敏感，与 `--audio` 的取值口径一致）。
ScaleParse parseScaleSpec(const char* text);

struct WindowSize {
    int w = kBaseWindowW;
    int h = kBaseWindowH;
};

// 一维：`round(base * scale)`，至少 1 px（⛔ 不允许建 0 宽的窗口）。
// scale 非有限 / 非正 ⇒ 退回 `base`（防御；调用方已经校验过，这里是给"忘了校验"的兜底）。
int scaleWindowDimension(int base, double scale);

// 窗口客户区尺寸 = `round(1280 × s) × round(720 × s)`。**画面仍是 1280×720**（本函数不改 image）。
// ⚠ 只有"帧尺寸未知"的建窗那一刻用它（`SDL_CreateWindow`）；帧尺寸已知之后**一律**用
//   `windowSizeForFrame`（⛔ 两处各写一套换算 = 这次的 bug 本身，见下）。
WindowSize windowSizeForScale(double scale);

// ★ `--scale` 的**唯一**换算口径（2026-11-19 裁决 c-i：**窗口 = 帧尺寸 × scale**）：
//   一维 = `round(frame × s)`，四舍五入规则**只依赖** `scaleWindowDimension`（远离 0 取整、
//   至少 1 px、`>= INT_MAX` 钳到 INT_MAX）—— ⛔ 这里不另写 `+0.5` / `lround` / 类型转换。
//   边界：`scale` 非有限 / `<= 0` ⇒ **按 1.0 处理**（= 返回帧尺寸本身；调用方已校验，这里是兜底）；
//   `frame_w/frame_h <= 0` ⇒ 该维 0（`scaleWindowDimension` 的原口径，调用方负责挡）。
//   ⇒ `scale == 1.0` 时恒等于 `{frame_w, frame_h}` ⇒ 与"窗口跟随后缓冲尺寸"**逐字节相同**
//     （既有 `present-window` 的"窗口跟随帧尺寸"判据在缺省档一字不改）。
//   ⇒ `frame == 1280×720` 时恒等于 `windowSizeForScale(scale)`（= 文档里"窗口 = 1280·s"那条，
//     在 guest 换分辨率之前字面成立）。
// ★ 为什么要有它：`SDL_CreateWindow`（`present_sdl.cpp` 窗口档 init）之后，**每一次**"窗口该多大"
//   都发生在 `SDL_SetWindowSize`（SDL_Renderer 路径 + GPU 直出路径）。那两处历史上直接用了 guest
//   后缓冲尺寸 ⇒ 第一帧 Present 就把 `--scale` 静默抹掉（1280×720）。窗口尺寸的判据只能在这一份里。
WindowSize windowSizeForFrame(int frame_w, int frame_h, double scale);

// display content scale 是否可用（`auto` 的"取得到吗"判据）。⚠ 这是**唯一**一份判据。
bool autoContentScaleUsable(double content_scale);

// display content scale → 实际缩放：可用 ⇒ 量化后的值；取不到（<=0 / NaN / Inf / > 8）⇒ 1.0。
// ★ `auto` 的"取不到就回 1.0"这个决定只在这里；调用方只负责把 `auto_fallback` 打出来（⛔ 不静默）。
double autoScaleFromContentScale(double content_scale);

// 四舍五入到 1/10000；非有限 / 非正 ⇒ 1.0。
double quantizeScale(double scale);

// 启动 readout 的数据（**一次性**；由窗口 sink 在窗口建完之后填，headless 由 CLI 填 `applied=false`）。
struct ScaleReport {
    double scale = kDefaultScale;  // **生效**的缩放（applied=false 时是"请求值"，不生效）
    int window_w = kBaseWindowW;   // 窗口**客户区**像素（窗口档 = 建完后 `SDL_GetWindowSize` 回读）
    int window_h = kBaseWindowH;
    int image_w = kBaseWindowW;    // 被呈现的画面像素（guest 后缓冲；缺省 1280×720）
    int image_h = kBaseWindowH;
    bool applied = true;           // false = `--present headless`（无可见窗口 ⇒ --scale 不生效）
    bool auto_requested = false;   // 命令行给的是 `auto`
    bool auto_fallback = false;    // auto 取不到 display content scale ⇒ 回 1.0（必须打出来）
    double auto_content_scale = 0.0;  // SDL 原始回读（诊断用；取不到时是 0 / NaN）
};

// 启动 readout **一行**（含换行？不含）。格式（缺省档）：
//   `scale: 1.0（window 1280x720，image 1280x720）`
// 数值 1.5（**只改窗口、不改帧**；窗口比画面大 ⇒ 画面放大，模糊可接受）：
//   `scale: 1.5（window 1920x1080，image 1280x720；放大，接受模糊）`
// 缩小档（s < 1）说"缩小"；恰好 1.0 不加这一句。
// `auto` 成功 / 取不到 / headless 各一种后缀（见实现）。★ 格式只在这里一份。
std::string scaleReadoutLine(const ScaleReport& report);

}  // namespace lift
