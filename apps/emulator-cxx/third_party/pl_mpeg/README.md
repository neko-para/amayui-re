# pl_mpeg（第三方单文件解码器）

`pl_mpeg.h` 是 **pl_mpeg** 的原样副本（未改一行）：MPEG-1 video + MP2 audio + MPEG-PS demux。

- 上游：<https://github.com/phoboslab/pl_mpeg>
- 作者：Dominic Szablewski · 许可证：MIT（见文件头 `SPDX-License-Identifier: MIT`）
- 本仓字节：sha256 `3a8cb30c83c2a1147719c30fe0c8b93da2987aa43140077c575b39aaa75fc2c9`
- 同一份字节在本仓还有一处：`apps/emulator-new/src/platform/electron/renderer/movie/pl_mpeg.h`
  （Electron 侧编译成 `movie.wasm`）。两处**必须逐字节相同**，
  守卫 `tools/test/emulator-cxx-pl-mpeg-sync.test.mjs` 盯这件事。

## 为什么是它

游戏的影片资源是 **MPEG-1 program stream**（`.MPG`：`00 00 01 BA 21 …` 的 MPEG-1 pack header +
MPEG-1 sequence header；本作实测 1280×720 / 29.97fps）。解码属**外部模块**
（`data/spec/chapters/60-ops-wait.md` 的「成立条件」：影片时长、解码、声音呈现都不在规范范围内）⇒
宿主可以自选实现。选它而不是 FFmpeg 的理由是**体积与可复现**：一个头文件、零构建依赖、MIT。

只启用视频：`movie.cpp` 里 `plm_set_audio_enabled(plm, 0)`。规范不要求音频输出，
忽略音频不得改变影片位、对象生命周期或阻塞。

## 怎么用

只有 `apps/emulator-cxx/src/core/movie.cpp` `#define PL_MPEG_IMPLEMENTATION` 并 include 它；
其余代码只认 `core/movie.h` 的 `MovieBackend` 抽象。⛔ 不要把 `pl_mpeg.h` include 进 `vm.h` / `main.cpp`。

## 怎么改

* 升级：同时替换本文件与 Electron 侧那一份（守卫会红）。
* 本地补丁：⛔ 不要直接改这份副本 —— 那会让两处不一致，且无法说明来源；
  要改就在 `movie.cpp` 的包装层做（例如换像素布局、丢帧策略）。
