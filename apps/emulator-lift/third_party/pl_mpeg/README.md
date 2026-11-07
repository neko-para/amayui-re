# pl_mpeg（第三方单文件解码器）

`pl_mpeg.h` 是 **pl_mpeg** 的原样副本（未改一行）：MPEG-1 video + MP2 audio + MPEG-PS demux。

- 上游：<https://github.com/phoboslab/pl_mpeg>
- 作者：Dominic Szablewski · 许可证：MIT（见文件头 `SPDX-License-Identifier: MIT`）
- 来源：从 `apps/emulator-cxx/third_party/pl_mpeg/pl_mpeg.h` 逐字节复制（`Copy-Item`，不手改）。
- 本副本必须与另外两处**逐字节一致**：
  · `apps/emulator-cxx/third_party/pl_mpeg/pl_mpeg.h`（C++ 宿主直接编译）
  · `apps/emulator-new/src/platform/electron/renderer/movie/pl_mpeg.h`（Electron 侧编译成 `movie.wasm`）
  守卫：`tools/test/emulator-cxx-pl-mpeg-sync.test.mjs`（三处 sha256 相同 + 仍是上游 MIT 标识）。

## 为什么 vendor 它

游戏的影片资源是 **MPEG-1 program stream**（`.MPG`：`00 00 01 BA 21 …` 的 MPEG-1 pack header +
system header；本作实测 1280×720 / 29.97fps）。解码属**外部模块**
（`data/spec/chapters/60-ops-wait.md` 的「成立条件」：影片时长、解码、声音呈现都不在规范范围内）
⇒ 宿主可以自选实现。选它而不是 FFmpeg 的理由是**体积与可复现**：一个头文件、零构建依赖、MIT，
且同时给 MPEG-1 PS demux 与 MP2 解码（本线只开视频，音频在 `movie_decoder.cpp` 里关掉）。

## 怎么用

- 整个 app 里只有 `third_party/pl_mpeg/pl_mpeg_impl.cpp` 定义 `PL_MPEG_IMPLEMENTATION`；
  它被编进静态库 `lift-pl-mpeg`（见 `apps/emulator-lift/CMakeLists.txt` 末尾的追加块）。
- 其余代码只认 `src/runtime/video/movie_decoder.h` 的公共接口（⛔ 公共头里不 include `pl_mpeg.h`）；
  唯一 include `pl_mpeg.h` 的实现文件是 `src/runtime/video/movie_decoder.cpp`。
- CMake 目标 `lift-pl-mpeg` 的 include 目录是 **PUBLIC**，所以消费者写 `#include "pl_mpeg.h"`。

## 怎么改

- **升级**：三处副本**一起换**（本处 + `apps/emulator-cxx/...` + `apps/emulator-new/...`），
  然后跑 `node --test tools/test/emulator-cxx-pl-mpeg-sync.test.mjs`；只换一处 ⇒ 守卫当场红。
- **本地补丁**：⛔ 不要直接改这份副本 —— 那会让三处不一致，且无法说明来源；
  要改行为就在 `src/runtime/video/movie_decoder.cpp` 的包装层做（丢帧策略、像素布局、时间基准）。
- 不要手工重打本文件（换行 / 编码 / 尾随空格都会让逐字节守卫红）。
