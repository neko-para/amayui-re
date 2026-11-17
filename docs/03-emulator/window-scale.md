# 窗口缩放 `--scale <auto|数值>`

> 状态口径：本文只写**不变量 / 判据 / 怎么看**，不写"当前做到哪"。进度看
> `pnpm tools requirements plan`，变更看 `git log`。

## 1. 它承诺什么

| 输入 | 窗口**客户区** | 被呈现的画面（guest 后缓冲） | 生效档位 |
|---|---|---|---|
| 缺省（不给 `--scale`） | `1280×720` | `1280×720` | window / auto / headless |
| `--scale 1.5` | `round(1280×1.5) × round(720×1.5)` = `1920×1080` | `1280×720`（不变） | window / auto |
| `--scale 1.25` | `1600×900` | `1280×720`（不变） | window / auto |
| `--scale auto` | 按显示 DPI（SDL 的 display content scale）算，公式同上 | `1280×720`（不变） | window / auto |
| 任意 `--scale` + `--present headless` | 隐藏离屏窗口仍 `1280×720` | `1280×720` | **不生效**（接受 + readout 明说） |

- **缺省 1.0 = 改动前的逐字节行为**：`SDL_CreateWindow(title, 1280, 720, …)` 的实参不变。
- 缩放只改**宿主窗口大小**，⛔ 不改 guest 可见的任何东西（后缓冲、D3D9 模式、`GetClientRect` 的
  虚拟屏仍是 1280×720）。
- 公式 / 校验 / readout 文案的**唯一一份**在 `apps/emulator-lift/src/runtime/window_scale.h`
  （`windowSizeForScale` / `parseScaleSpec` / `autoScaleFromContentScale` / `scaleReadoutLine`）。
- 窗口像素 → 画面像素**仍然只有一份公式**：`runtime/cursor_map.h` 的 `mapWindowPixelToImage`
  （`present_sdl.cpp` 的 `mapMouse` 只调它）。窗口变大后 letterbox 自然得到 `scale = 窗口/画面`，
  ⛔ 没有为 `--scale` 另写换算。

## 2. `auto` 的口径

- 取数：`SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay())`（主显示的 DPI 内容缩放；1.0 = 96 dpi）。
- **进程 DPI awareness**：`present_sdl.cpp` 在 `SDL_Init(VIDEO)` **之前**显式调用
  `SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)`（Windows）。
  不 aware 时 Windows 会把显示缩放虚拟化 ⇒ content scale 恒报 1.0，`auto` 就静默退化成 1.0。
  已由 manifest 设过时该调用返回 `ERROR_ACCESS_DENIED` —— 那是"已经对了"，不是失败。
- 值先量化到 1/10000（`1.2499999 ⇒ 1.25`），让 readout 印的数与真正用于算窗口尺寸的数是同一个。
- **取不到就回 1.0**：content scale 为 0 / 负数 / NaN / Inf / `> 8`（不是 DPI）⇒ 1.0，并且启动
  readout 里写明"取不到 display content scale（SDL 返回 X）⇒ 回 1.0"，⛔ 不静默。
- 已知边界：多显示器时取的是**主显示**（窗口还没建，拿不到 per-window scale）；窗口建完之后
  拖动到另一块 DPI 不同的屏不会重新缩放。这两条都写进 §5 的"未做/风险"。

## 3. 启动 readout（一次性，缺省也打）

格式串只有一份（`window_scale.h::scaleReadoutLine`）：

```
scale: 1.5（window 1920x1080，image 1280x720）
scale: 1.0（window 1280x720，image 1280x720）
scale: 1.5（window 1920x1080，image 1280x720；auto ← display content scale 1.5）
scale: 1.0（window 1280x720，image 1280x720；auto 取不到 display content scale（SDL 返回 0.0）⇒ 回 1.0）
scale: 1.5（--present headless ⇒ 无窗口可施，不生效；image 1280x720）
```

- `window` 是**建完窗口之后** `SDL_GetWindowSize` 回读的客户区尺寸（⛔ 不是把请求值抄回来）。
- 只在启动打一次；`AGENTS.md` §12.8 第 3 条：一次性输出（启动 readout）⛔ 不许收。

## 4. 命令与读数

> 日期 2026-11-18 · win32 · `windows-x64` 预设（RelWithDebInfo）· dev 工作树 ·
> 构建 `cmake --preset windows-x64 -DLIFT_AOT_LTO=OFF`（dev 缓存无 LTO 键）。
> 真 run 的 `--game-dir` 指向本树 `links/install`（junction → main 的 `dist/install`，§14.5），
> `--appdata` 显式指到本树 `.tmp/scale/appdata`（⛔ 不写 main 的树）。

```powershell
cd E:\Projects\amayui-re-dev\apps\emulator-lift
cmake --preset windows-x64 -DLIFT_AOT_LTO=OFF
cmake --build --preset windows-x64 --parallel 4
ctest --test-dir out -R "cursor-warp|window-scale|cli-aot-flags|lift-control-endpoint" --output-on-failure

# 真 run（每次 ~4 s；--skip-logo 让它在标题画面之前就停）
.\out\emulator-lift-aot.exe --present window --scale 1.5 --max-frames 2 --skip-logo `
  --appdata E:\Projects\amayui-re-dev\.tmp\scale\appdata `
  --game-dir E:\Projects\amayui-re-dev\links\install
```

读数（2026-11-18，同一台机器，逐条命令各一次）：

| 命令里的 `--scale` / 档位 | 启动 readout 那一行 |
|---|---|
| `--present window --scale 1.5` | `scale: 1.5（window 1920x1080，image 1280x720）` |
| `--present window --scale 1.25` | `scale: 1.25（window 1600x900，image 1280x720）` |
| `--present window --scale auto`（本机显示缩放 150%） | `scale: 1.5（window 1920x1080，image 1280x720；auto ← display content scale 1.5）` |
| `--present window`（缺省） | `scale: 1.0（window 1280x720，image 1280x720）` |
| `--present headless --scale 1.5` | `scale: 1.5（--present headless ⇒ 无窗口可施，不生效；image 1280x720）` |

`--present window --scale 1.5` 那次 run 的收口仍是
`present : … window=visible … present=direct/immediate frames=2`（缩放不改变呈现路径）；
`stop  : … max-frames`。

## 5. 未做 / 风险

- **鼠标映射只做了纯几何判据**（`tests/window_scale.cpp` 的 `window-scale/mouse-mapping`：
  1.5 / 1.25 / 1.33 三档，含非 16:9 残差偏移），**没有**用真人手在 1.5 倍窗口上点过：
  真机手感 / 窗口原点 / 多显示器负坐标仍未观测。几何公式与 `mapMouse` 是同一份，所以这条
  风险落在"窗口坐标本身对不对"，不落在"又写了一套公式"。
- **多显示器 / 运行时换屏**：`auto` 只看主显示；窗口拖到另一块 DPI 不同的屏不会重算（要重算
  得处理 `SDL_EVENT_WINDOW_DISPLAY_CHANGED` + 重新 letterbox，不在本次范围）。
- **非整数倍率的像素级往返不闭合**：1.25 / 1.33 下"窗口 → 画面 → 窗口"不保证逐像素回到原点
  （`cursor-map/round-trip` 的既有口径：只要求正向线性 + 端点 + 参考点互逆）。这是缩放本身的
  信息损失，不是公式错。
- **非 16:9 的窗口有 <1 px 的呈示/映射差**：直出呈示的 letterbox 矩形由 `d3d9_gpu.cpp` 的
  `layoutLetterbox` 按整数 `dw/dh` 居中（`--scale 1.33` ⇒ 958 高的窗口里画面占 957、`y=0`），
  而 `cursor_map.h` 用分数偏移（`off_y = 0.3125`）。两者最多差 ~0.5 px —— 这是**既有**的两处算术
  （不是本次引入的第二套鼠标公式），1.25 / 1.5 都是精确 16:9（差为 0）。要收口得先让呈示侧也用
  `letterboxFor`，那属于另一件事。
- **`auto` 依赖平台 content scale**：本机（150%）取到 1.5；100% 机器上取到 1.0（等价缺省）。
  ⛔ 不要拿"本机读数是 1.5"当断言 —— 那是读数，不是合同。
- **headless 不查 DPI**：`--present headless` 下 `--scale` 连 `SDL_GetDisplayContentScale` 都不调
  （无窗口可施，且不想让"只是拿 GPU device"多一条平台依赖）。readout 明说不生效。

## 6. 怎么查 / 怎么改

- 看运行时的实际窗口：跑一次带 `--present window --scale <值>`，读启动 readout 的 `scale:` 一行
  （`window` 是 `SDL_GetWindowSize` 回读的事实）。
- 看公式 / 拒绝口径 / 文案：`apps/emulator-lift/src/runtime/window_scale.h`（唯一一份）。
- 看守卫：`cd apps/emulator-lift && ctest --test-dir out -R "window-scale|cli-aot-flags" --output-on-failure`。
  - `window-scale/*`：缺省 1.0、数值取整（1.25/1.5/1.33）、鼠标映射（与 `control_endpoint` 判据 4
    的参考点互逆）、auto 的 DPI→scale、parse 的拒绝面、headless readout。
  - `cli-aot-flags` 的 `scale-*`：`--scale` 在 `--help` 里；垃圾值 / 缺值 ⇒ exit 2 且点名参数；
    headless 下接受（⛔ 不进"headless 不接受窗口参数"那一类）。
- 改缩放语义 ⇒ 先改 `window_scale.h`（公式与文案只有一份），再跑上面两条 CTest；⛔ 不要在
  `aot_main.cpp` / `present_sdl.cpp` 里另写数字或句子。
