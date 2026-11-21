# 窗口缩放 `--scale <auto|数值>` + `--scale-filter nearest|linear`

> 状态口径：本文只写**不变量 / 判据 / 怎么看**，不写"当前做到哪"。进度看
> `pnpm tools requirements plan`，变更看 `git log`。

## 1. 它承诺什么

| 输入 | 窗口**客户区** | 被呈现的画面（guest 后缓冲） | 生效档位 |
|---|---|---|---|
| 缺省（不给 `--scale`） | `1280×720` | `1280×720` | window / auto / headless |
| `--scale 1.5` | `round(1280×1.5) × round(720×1.5)` = `1920×1080` | `1280×720`（不变） | window / auto |
| `--scale 1.25` | `1600×900` | `1280×720`（不变） | window / auto |
| `--scale auto` | 按显示 DPI（SDL 的 display content scale）算，公式同上 | `1280×720`（不变） | window / auto |
| `--scale-filter linear`（**缺省**） | 不变 | 放大用双线性（与真机 150% DPI 位图放大同档的平滑放大） | window / auto |
| `--scale-filter nearest` | 不变 | 放大用最近邻（硬边；1.5× 这类非整数倍率有锯齿） | window / auto |
| 任意 `--scale` + `--present headless` | 隐藏离屏窗口仍 `1280×720` | `1280×720` | **不生效**（接受 + readout 明说） |

- **缺省 1.0 = 改动前的逐字节行为**：`SDL_CreateWindow(title, 1280, 720, …)` 的实参不变。
- ★ **基准是"当前帧尺寸"（今天 = 1280×720）**：`--scale` 是倍率、图像尺寸跟 guest ⇒ 上表那两列在 guest
  换分辨率之后要按**当帧尺寸**算（窗口跟着帧走、倍率不变）。**唯一换算** =
  `windowSizeForFrame(frame_w, frame_h, scale)`；`scale == 1.0` ⇒ 恒等于帧尺寸（与"窗口跟随帧尺寸"
  逐字节相同）。⚠ 2026-11-19 之前两处 `SDL_SetWindowSize` 直接用了帧尺寸 ⇒ **第一帧 Present 就把
  `--scale` 静默抹掉**（`--scale auto` 建窗时变大、进界面又回 1×）；守卫 =
  CTest `present-window/scale-window-follows-frame`（1.5× ⇒ 1920×1080；帧变 640×480 ⇒ 960×720）。
- ★ **范围裁决（2026-11-18，用户口径）：`--scale` 只改窗口，不改帧。**
  guest 画面恒为 1280×720；窗口比画面大时画面是**放大**出来的。⇒ ⛔ 不做高分辨率重渲染 /
  ⛔ 不改帧尺寸 / ⛔ 不加额外 upscale pass。`auto` 也只决定窗口倍率，不改变帧尺寸。
- ★ **滤波器裁决（2026-11-21）：放大用 `linear`（缺省），`nearest` 是显式回退档。**
  取证：真机 `AGE.EXE` 的 manifest 无 DPI 声明、也没有 `SetProcessDPIAware` ⇒ 窗口一打开就是
  **DPI 放大后**的尺寸（用户实测 `1923×1151 = 1.5 × (1282×767.33)`）⇒ 真机 = "渲染 1280×720 →
  Windows 按 150% **平滑**放大"（不是重渲染）。NEAREST 在 1.5× 上每 2 源像素对 3 目标像素 ⇒
  锯齿（用户实测"明显比真机糙"）⇒ 缺省改成 linear 与真机同档。
  - 两条放大路径都换：SDL 纹理（`present_sdl.cpp` 的 `SDL_SetTextureScaleMode`）与 GPU 直出的
    letterbox blit（`d3d9_gpu.cpp` 的 `SDL_GPUBlitInfo::filter`）。⛔ 只改**最终呈现**那一次放大；
    游戏内容 / D3DXSprite 的纹理采样与 letterbox 的矩形算术一概不动。
  - `--scale-filter nearest` 是**回退口径**：与换滤波器之前**逐字节相同**（像素党 / A/B / 既有
    像素与截图断言都拿它当基准）。
  - 清晰度仍由源分辨率决定（`image` 恒 1280×720）；换滤波器只改放大时的采样，不"变清晰"。
- 缩放只改**宿主窗口大小**，⛔ 不改 guest 可见的任何东西（后缓冲、D3D9 模式、`GetClientRect` 的
  虚拟屏仍是 1280×720）。
- 公式 / 校验 / 滤波器 / readout 文案的**唯一一份**在 `apps/emulator-lift/src/runtime/window_scale.h`
  （`windowSizeForScale`（建窗那一次）/ `windowSizeForFrame`（此后每一次）/ `parseScaleSpec` /
  `parseScaleFilterSpec` / `autoScaleFromContentScale` / `scaleReadoutLine`）。
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
scale: 1.5（window 1920x1080，image 1280x720；filter=linear；放大：与真机 150% DPI 同档的平滑放大）
scale: 1.5（window 1920x1080，image 1280x720；filter=nearest；放大：硬边，非整数倍率有锯齿）
scale: 1.0（window 1280x720，image 1280x720；filter=linear）
scale: 0.5（window 640x360，image 1280x720；filter=linear；缩小）
scale: 1.5（window 1920x1080，image 1280x720；filter=linear；放大：与真机 150% DPI 同档的平滑放大；auto ← display content scale 1.5）
scale: 1.0（window 1280x720，image 1280x720；filter=linear；auto 取不到 display content scale（SDL 返回 0.0）⇒ 回 1.0）
scale: 1.5（--present headless ⇒ 无窗口可施，不生效；image 1280x720）
```

- `image` 恒为被呈现的那一帧（**1280×720**）—— 这一格就是"只改窗口、不改帧"的读数。
- `filter=` 是**生效的放大滤波器**（`--scale-filter`，缺省 `linear`）：`scale == 1.0` 时 1:1、两个
  滤波器无可见差别，但这一格照印（人读的那一行要说清"现在用的是哪一个"）。
- 窗口比 `image` 大时同一行带一句放大口径：`linear` ⇒ `放大：与真机 150% DPI 同档的平滑放大`，
  `nearest` ⇒ `放大：硬边，非整数倍率有锯齿`；s<1 则 `缩小`（⛔ 不冒充"放大"）；恰好 1.0 不带这一句。
- `window` 是**建完窗口之后** `SDL_GetWindowSize` 回读的客户区尺寸（⛔ 不是把请求值抄回来）。
- 只在启动打一次；`AGENTS.md` §12.8 第 3 条：一次性输出（启动 readout）⛔ 不许收。

## 4. 命令与读数

> 日期 2026-11-21 · win32 · `windows-x64` 预设（RelWithDebInfo，`split8-O2-lto-abi`）· **main** 工作树 ·
> 真 run 的 `--game-dir` 指向本树 `links/install`（符号链接 → `dist/install`），
> `--appdata` 显式指到本树 `.tmp/scale-filter/appdata`（`SAVE/` 里只放 SAVE07.DAT/STH）。

```powershell
cd E:\Projects\amayui-re\apps\emulator-lift
cmake --build --preset windows-x64 --parallel 6
ctest --test-dir out -R "window-scale|cli-aot-flags|present-window|present-readback|aot-overlay" --output-on-failure

# 真 run（--skip-logo + 几帧就够；读的是启动 readout 那一行）
.\out\emulator-lift-aot.exe --present window --scale 1.5 --scale-filter nearest --max-frames 5 --skip-logo `
  --game-dir E:\Projects\amayui-re\links\install `
  --appdata E:\Projects\amayui-re\.tmp\scale-filter\appdata
```

读数（2026-11-21，同一台机器，逐条命令各一次）：

| 命令里的 `--scale` / 档位 | 启动 readout 那一行 |
|---|---|
| `--present window --scale 1.5` | `scale: 1.5（window 1920x1080，image 1280x720；filter=linear；放大：与真机 150% DPI 同档的平滑放大）` |
| `--present window --scale 1.5 --scale-filter nearest` | `scale: 1.5（window 1920x1080，image 1280x720；filter=nearest；放大：硬边，非整数倍率有锯齿）` |
| `--present window --scale 1.25` | `scale: 1.25（window 1600x900，image 1280x720；filter=linear；放大：与真机 150% DPI 同档的平滑放大）` |
| `--present window --scale auto`（本机显示缩放 150%） | `scale: 1.5（window 1920x1080，image 1280x720；filter=linear；放大：与真机 150% DPI 同档的平滑放大；auto ← display content scale 1.5）` |
| `--present window`（缺省） | `scale: 1.0（window 1280x720，image 1280x720；filter=linear）` |
| `--present headless --scale 1.5` | `scale: 1.5（--present headless ⇒ 无窗口可施，不生效；image 1280x720）` |

`--present window --scale 1.5` 那次 run 的收口仍是
`present : … window=visible … present=direct/immediate`（缩放 / 滤波器都不改变呈现路径）；
`stop  : … max-frames`。

### 4.1 滤波器的像素读数（确定性；⛔ 不用游戏截图当判据）

`present-window/scale-filter-1.5`（`ctest -R '^present-window$'` 与 `^present-window-gpu$` 各跑一次）
用**本文件造的竖条纹**（偶列红 / 奇列蓝，1280×720）在 `--scale 1.5` 下回读窗口像素：

| 后端 | `nearest` | `linear`（窗口 (3,3)） |
|---|---|---|
| software（`SDL_RenderReadPixels` = 真实窗口像素） | 逐点 == 旧整数映射 `sx = wx*1280/1920` | `#D3002B`（nearest 同点 `#FF0000`） |
| gpu 直出（回读走 `directReadback` 的 CPU 重建） | 同上，逐点命中 | `#D5002A` |

★ 为什么**必须**用合成内容：游戏自己的屏按墙钟在动 —— `LOAD` / `TITLE` 屏在**同配置两次跑**、
同一 `--present-screenshot-frame`（300 / 150）上分别有 **44% / 59%** 的像素不同（`--scale 1.5`、
同一个 `--input-plan`、每次重建私有 `--appdata`）⇒ 跨跑截图不能当"逐字节相同 / 不同"的判据，
连"linear vs nearest 谁更糙"都不能拿它当读数（那是背景相位的差，不是放大滤波器的差）。

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
- **`--present-screenshot` 与屏幕上那一份的关系**：GPU 直出臂的 swapchain 是 write-only ⇒ 截图是
  从**同一份 target** 按 `layoutLetterbox` 重建的（`d3d9_gpu.cpp::directReadback`）。它现在按
  **与呈现同一个滤波器**重建：`nearest` 档与换滤波器之前逐字节相同；`linear` 档是 CPU 近似
  bilinear（GPU 若在 sRGB 空间混合，可能差几个单位 —— **不是**逐位等价，只保证"同一个滤波器、
  同一块矩形、同一份几何"）。`--scale 1.0`（1:1）时两者采样点都在 texel 中心 ⇒ 逐字节同值。
- **"与真机同档"是口径不是逐像素等价**：真机是 Windows 的系统 DPI 位图放大（具体是 SuperSampling/
  双线性取决于系统设置），我们固定用双线性；两者都是"平滑放大"，但**不**逐像素相同。

## 6. 怎么查 / 怎么改

- 看运行时的实际窗口：跑一次带 `--present window --scale <值>`，读启动 readout 的 `scale:` 一行
  （`window` 是 `SDL_GetWindowSize` 回读的事实；`filter=` 是生效的放大滤波器）。
- 看公式 / 拒绝口径 / 文案：`apps/emulator-lift/src/runtime/window_scale.h`（唯一一份）。
- 看守卫：`cd apps/emulator-lift && ctest --test-dir out -R "window-scale|cli-aot-flags" --output-on-failure`。
  - `window-scale/*`：缺省 1.0、数值取整（1.25/1.5/1.33）、鼠标映射（与 `control_endpoint` 判据 4
    的参考点互逆）、auto 的 DPI→scale、parse 的拒绝面、**`--scale-filter` 的解析 / 缺省 linear /
    readout 两个档**（`window-scale/parse-filter`）、headless readout。
  - `cli-aot-flags` 的 `scale-*`：`--scale` / `--scale-filter` 在 `--help` 里；垃圾值 / 缺值 ⇒
    exit 2 且点名参数；headless 下两者都接受（⛔ 不进"headless 不接受窗口参数"那一类）。
- 改缩放语义 ⇒ 先改 `window_scale.h`（公式 / 滤波器 / 文案只有一份），再跑上面两条 CTest；⛔ 不要在
  `aot_main.cpp` / `present_sdl.cpp` / `d3d9_gpu.cpp` 里另写数字或句子。
