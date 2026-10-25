# emulator-cxx

《天結いキャッスルマイスター》原生 C++ 宿主（clean-room）。

目标不是保留 Electron，而是由应用自己拥有窗口与 surface：

- SDL3：窗口 / 输入 / 音频；
- SDL_GPU（当前 POC）：显式 `submit` + `present`，无浏览器合成器；
- bgfx（可选 feature）：后续 2D sprite/mesh/混合渲染；
- FreeType + HarfBuzz（可选）：文字；
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
`vcpkg.json` 声明（manifest mode）。默认装 `sdl3` 与 `nlohmann-json`。

| feature | 依赖 | 用途 |
|---|---|---|
| `shader` | `sdl3-shadercross` | HLSL → DXIL/SPIR-V（默认开启） |
| `renderer` | `bgfx` | 2D 渲染后端 |
| `text` | `freetype`、`harfbuzz` | 字形/排版 |
| `video` | `ffmpeg` | 影片解码 |

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
| 配置库 | `CORE-CONFIG` | 词表 + ALF 发行默认表 + `SYS4REG.INI` 覆盖 + 脚本键表 |
| 已接处理器族 | `CORE-OPS-*` | no-op、语音、标量、列表、字符串、数组/引用地址、纹理/绘制项/网格、`0x3` call-script |
| 启动链 | `CORE-BOOT` | 真实 `SYSTEM4.BIN` 跑到 LOGO 的 `0x20F play-movie` 前（96196 步） |
| 安装文件切片 | `CORE-FS` 临时引导 | 松散文件优先，否则 `DATA*.ALF` 按 offset/length 切片 |

```powershell
# ALF 索引 → 根脚本 id 0 → 脚本头 → 静态解码
.\out\build\windows-x64\emulator-cxx.exe --install "E:\Games\Eushully\天結\install" --stop-at 0x20F

# 单个 AGF
.\out\build\windows-x64\emulator-cxx.exe --agf "E:\Games\Eushully\天結\install\MI040.AGF"
```

★ 当前是 **最小可执行层**：`CORE-ISA` / `CORE-POOL` / `CORE-FRAME` / `CORE-VM` / `CORE-CONFIG` + `FMT-ALF` / `FMT-AGF`。
`--config` 可指定玩家 `SYS4REG.INI`；缺省按 `set:RegFilePath` 拼安装根。
门 / 状态位服务、换帧/换脚本、容器句柄尚未完整建模；未实现 opcode 停在它前面并报告偏移。
`--stop-at 0x20F` 是当前启动链的停止线：命中时打印 `stop-opcode`，不执行影片请求。
根脚本静态解码会在数据区遇到非法 opcode 后停止，这是规范允许的静态解码边界，不是执行边界。

## 运行 / 门禁

```powershell
.\out\build\windows-x64\emulator-cxx.exe           # 可见窗口，Esc 退出
.\out\build\windows-x64\emulator-cxx.exe --smoke   # 离屏 clear + 读回 + BMP
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








