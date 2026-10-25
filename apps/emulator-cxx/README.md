# emulator-cxx

《天結いキャッスルマイスター》原生 C++ 宿主（clean-room）。

目标不是保留 Electron，而是由应用自己拥有窗口与 surface：

- SDL3：窗口 / 输入 / 音频；
- SDL_GPU（当前 POC）：显式 `submit` + `present`，无浏览器合成器；
- bgfx（可选 feature）：后续 2D sprite/mesh/混合渲染；
- FreeType（可选）：文字；Shift-JIS 用平台代码页转换（Windows `MultiByteToWideChar(932)` / 其它平台 iconv）；
- FFmpeg（可选）：影片解码。

## 目录

```text
apps/emulator-cxx/
  CMakeLists.txt
  CMakePresets.json
  vcpkg.json
  vcpkg/                    git submodule（vcpkg 本体，不入库）
  src/main.cpp              窗口 / present / --smoke / core CLI
  src/core/                 C++ core（按 spec 重写）：ALF / LZSS / AGF / 脚本头 / opcode 解码
```

## 依赖（vcpkg submodule + manifest）

vcpkg 以 **git submodule** 固定在 `apps/emulator-cxx/vcpkg`；依赖由同目录的
`vcpkg.json` 声明（manifest mode）。默认装 `sdl3`、`nlohmann-json`，并开启
`shader` + `text` 两个 feature。

| feature | 依赖 | 用途 |
|---|---|---|
| `shader` | `sdl3-shadercross` | HLSL → DXIL/SPIR-V（默认开启） |
| `text` | `freetype` | 字形光栅化；Shift-JIS 用平台代码页转换（Windows `MultiByteToWideChar(932)`，其它平台系统 iconv）（默认开启） |
| `renderer` | `bgfx` | 2D 渲染后端（可选） |
| `video` | `ffmpeg` | 影片解码（可选） |

首次拉取：

```powershell
git submodule update --init --recursive
.\apps\emulator-cxx\vcpkg\bootstrap-vcpkg.bat -disableMetrics
```

## 构建（Windows）

本机 Visual Studio 18 的 `cl.exe` 在 CMake 探测阶段会报
`c1.dll ... failed to get command line into debug records`，因此默认 preset 使用
`clang-cl`（LLVM），SDL3 由 vcpkg 构建：

```powershell
cd apps\emulator-cxx
cmake --preset windows-x64
cmake --build --preset windows-x64
```

依赖 feature 打开方式：

```powershell
cmake --preset windows-x64 -DVCPKG_MANIFEST_FEATURES="renderer;text;video"
cmake --preset windows-x64 -DEMULATOR_CXX_WITH_BGFX=ON -DEMULATOR_CXX_WITH_TEXT=ON -DEMULATOR_CXX_WITH_VIDEO=ON
```

## C++ core（按 spec 重写）

第一层已落地，不调用 `apps/emulator-new` 的 TypeScript：

| 模块 | 规范 | 状态 |
|---|---|---|
| ALF 索引 / TOC / 尾部默认配置 | `20-alf`、`FMT-ALF` | S3/S4 族已解析；真实 `SYS4INI.BIN` 21109 条 |
| ALF LZSS | `20-alf` | 4 KiB 环形窗口、写指针 `0xFEE` |
| AGF/ACGF | `30-agf` | 8/24/32bpp、ACIF alpha、颜色键口径 |
| 脚本头 | `CORE-ISA` | `SYS4`/`SYS3`、`450 ` 子头、三组表指针 |
| opcode 静态解码 | `CORE-ISA` | 574 条 opcode 表；无处理器即停止并给偏移 |
| 值 / 池 / 帧（最小） | `CORE-POOL` / `CORE-FRAME` | 全局/局部 int/float、ENC/DEC、40 帧槽、返回栈 |
| 最小 VM | `CORE-VM` | 取指 → 域检查 → 长度槽 → ip；exit/ret/call/jump/mov/整数算术 |
| 主循环呈现 | `CORE-VM-LOOP`（取证：门之前） | 每次迭代最多一次渲染：绘制开关 = 1 且场景需要重绘时请求；`0x20C` 是脚本主动再要一次，不是唯一提交路径 |
| GPU 呈现 | SDL_GPU 场景批次 | 绘制项 / 4 顶点网格直接走 GPU（纹理缓存 + 每帧顶点缓冲）；命令档 / 文字暂回退软件合成 |
| 配置库 | `CORE-CONFIG` | 词表 + ALF 发行默认表 + `SYS4REG.INI` 覆盖 + 脚本键表 |
| 已接处理器族 | `CORE-OPS-*` | no-op、语音、标量、列表、字符串、数组/引用地址、纹理/绘制项/网格、`0x3` call-script |
| 启动链 | `CORE-BOOT` | 真实 `SYSTEM4.BIN` 越过 LOGO 影片，进入 `TITLE.BIN`（id 21092）主循环 |
| 影片 | `CORE-OPS-MOVIE` | `0x20F` 对象表 / 纹理绑定门 / 影片位；headless 后端立即收尾 |
| 渲染接入 | `CORE-OPS-TEXTURE-DRAW`、`CORE-TIMING-WINDOW` | AGF 像素进入纹理记录；绘制项 / 网格按有符号 key 软件合成；颜色窗四档派生；`0x33F` tint + 混合模式 |
| 场景命令档 | `CORE-OPS-SCENE-COMMAND-RECORDS`、`CORE-SCENE-DRAW-ORDER` | `0x223/24D/24F/250/251` 写表与校验；命令档 → 离屏画布 36/37 → 四种类合成（Fade / 遮罩 / 12 种抹除图案 / SlideBlur / ZoomBlur）→ 收尾扫描 |
| 离屏文字 | `CORE-OPS-RENDER-TEXT-204` | Shift-JIS（平台代码页 / iconv）→ FreeType 字形；主色 / 副色 / 绘制模式 / 对位状态来自字体对象 |
| 源矩形窗 | `CORE-OPS-SCENE3D-239` | `0x239` 步进 / 延迟 / 到期回写四条边 |
| 安装文件切片 | `CORE-FS` 临时引导 | 松散文件优先，否则 `DATA*.ALF` 按 offset/length 切片 |

★ 引擎字符串（ALF 名字 / 配置值 / 路径）是 **CP932（ANSI）字节**，核心内部保留原始字节；
显示与宿主路径统一经 `core/encoding` 解码为 UTF-8（Windows 路径再走 UTF-16），
控制台输出在 Windows 上切 `CP_UTF8`。⛔ 不要把 CP932 字节直接 `printf` 或塞进 `std::filesystem::path`。

★ 呈现分层：`--play-*` 的绘制项 / 4 顶点网格走 **SDL_GPU 场景批次**（纹理按 `TextureRecord::revision` 缓存）；
命令档与离屏文字还没有 GPU 实现，命中时回退到软件 `rasterize_scene`。`--render-title` 的 BMP 导出
仍用软件合成，它是像素 oracle，不是交互呈现路径。

```powershell
# ALF 索引 → 根脚本 id 0 → 脚本头 → 静态解码
.\out\build\windows-x64\emulator-cxx.exe --install "E:\Games\Eushully\天結\install" --to-title

# TITLE 合成帧导出 / 逐帧导出 / 窗口显示（Esc 退出）/ 逐帧播放
.\out\build\windows-x64\emulator-cxx.exe --install "E:\Games\Eushully\天結\install" --render-title
.\out\build\windows-x64\emulator-cxx.exe --install "E:\Games\Eushully\天結\install" --render-title-gpu
.\out\build\windows-x64\emulator-cxx.exe --install "E:\Games\Eushully\天結\install" --render-frames 4
.\out\build\windows-x64\emulator-cxx.exe --install "E:\Games\Eushully\天結\install" --show-title
.\out\build\windows-x64\emulator-cxx.exe --install "E:\Games\Eushully\天結\install" --play-title
.\out\build\windows-x64\emulator-cxx.exe --install "E:\Games\Eushully\天結\install" --play-boot

# 单步跟踪指定条数（调试主循环 / 门服务）
.\out\build\windows-x64\emulator-cxx.exe --install "E:\Games\Eushully\天結\install" --trace 40

# 单个 AGF
.\out\build\windows-x64\emulator-cxx.exe --agf "E:\Games\Eushully\天結\install\MI040.AGF"
```

★ 当前是 **最小可执行层**：`CORE-ISA` / `CORE-POOL` / `CORE-FRAME` / `CORE-VM` / `CORE-CONFIG` + `FMT-ALF` / `FMT-AGF`。
`--config` 可指定玩家 `SYS4REG.INI`；缺省按 `set:UseAppDataFolder` 选配置根（`%APPDATA%` / `XDG_CONFIG_HOME` / `~/.config`，否则安装根），
再拼 `set:RegFilePath` 与文件名。
`--to-title` 以 `TITLE.BIN`（统一文件 id 21092）为停止线；未知 opcode 仍停在它前面并报告偏移。
门 / 状态位服务、换帧/换脚本、容器句柄尚未完整建模。
`--stop-at <opcode>` 仍可用于在任意 opcode 前停止；`0` 表示不设停止线。
根脚本静态解码会在数据区遇到非法 opcode 后停止，这是规范允许的静态解码边界，不是执行边界。

`--render-frames N` 把 TITLE 之后的前 N 帧写成 `title-frame-NN.bmp`（每帧显式请求一次渲染步）；
`--render-title-gpu` 用 GPU 场景批次把 TITLE 画到离屏纹理、读回 `title-gpu.bmp`，用于核对 GPU 路径。
`--play-title` 在 TITLE 刚成为当前帧时就停（不预跑 TITLE 主体），再开窗用真实时钟跑 TITLE，
**只在 core 请求渲染时**合成 / 呈现（静态 TITLE 可能只呈现一帧）；
`--play-boot` 从根脚本开始就开窗，按引擎的「门前渲染」节奏呈现（场景脏 / 有动画 ⇒ 下一次满足闸门的迭代渲染一次），
因此能看到 LOGO 之后的渐变；影片本身仍是 headless。`--play-frames N` 是它们的无人值守版本。

## 运行 / 门禁

```powershell
.\out\build\windows-x64\emulator-cxx.exe           # 可见窗口，Esc 退出
.\out\build\windows-x64\emulator-cxx.exe --smoke   # 离屏 clear + 读回 + BMP
ctest --test-dir out\build\windows-x64 --output-on-failure

# 可选：把真实安装目录挂进 CTest（title / title-gpu / title-frames / title-play / boot-play）
cmake --preset windows-x64 -DEMULATOR_CXX_INSTALL_DIR="E:\Games\Eushully\天結\install"
ctest --test-dir out\build\windows-x64 --output-on-failure
```

`--smoke` 会：

1. 创建 1280×720 `R8G8B8A8_UNORM` 离屏纹理；
2. `SDL_BeginGPURenderPass` clear 到固定颜色；
3. `SDL_SubmitGPUCommandBufferAndAcquireFence` 等待完成；
4. `SDL_DownloadFromGPUTexture` 读回并断言中心像素；
5. 2×2 纹理 upload → download 往返断言；
6. 2×2 纹理四边形：alpha 混合与 reverse-subtract 混合，各自读回断言；
7. shadercross 编译 HLSL 为 DXIL 并创建 shader 对象；
8. 写 `emulator-cxx-smoke.bmp`。

它不依赖窗口合成器，因此是后续渲染回归的确定性基线。

## 当前 POC 验证什么

- vcpkg manifest + submodule 构建；
- SDL3 窗口创建；
- SDL_GPU 设备 / swapchain / 显式 present；
- 离屏 clear、GPU 命令提交、download 读回与像素断言；
- 纹理四边形：alpha / reverse-subtract 混合（HLSL sampler 走 SDL 约定的 `space2`）；
- 2×2 纹理 upload/download 往返；
- HLSL → DXIL 的 shadercross 编译；
- 程序不依赖 Electron / Chromium / WebGL。

下一步在 `renderer` feature 打开后接入 bgfx：
纹理四边形 → alpha / add / reverse-subtract 三种 blend → 与现有像素对照。

## 规范边界

`data/spec` 仍是唯一真源。C++ 实现只消费规范结论，不反向把实现细节写进 spec。
