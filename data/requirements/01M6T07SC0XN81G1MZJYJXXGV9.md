# emulator-lift · 视频 A v1：DirectShow 纹理影片真实现（LOGO）

- id: REQ-01M6T07SC0XN81G1MZJYJXXGV9
- type: req
- status: done
- parent: REQ-01M6K84KHPA1XF3B1HSHXME6W0
- verify: apps/emulator-lift/tests/movie_logo.cpp#movie-logo
- tags: [video, emulator-lift]

# emulator-lift · 视频 A v1：DirectShow 纹理影片真实现（LOGO）

## 目标

把 `apps/emulator-lift` 的纹理影片从「形状层 + 零长」换成**真实现**：宿主提供 DirectShow 服务端，
接住引擎自带的 `CStreamReader`（源过滤器）与 `CMovieTextureRenderer`（纹理渲染器），字节**从引擎自己的
源 pin 取**（ALF/松散文件解析是 guest 的事，宿主不重复实现），用 vendored `pl_mpeg` 解 MPEG-1，帧按
`cpu.clock` 送进引擎渲染器写纹理；影片按真实时长播完并推进脚本。

## 不做（v1 边界）

- **MF / EVR / DXVA2**：名字含 `WMV`/`MP4` 才可达（`0x487A80`）；发行影片是 MPEG-1 PS ⇒ 维持 `E_FAIL`。
- **窗口影片族**（`0x1c0` / `0x1c6` → `MovieIVideoWindow` + `IVideoWindow`）：延后；spec 侧仍是
  `RES-MOVIE-WINDOW-FAMILY`。
- **影片音频（MP2）**：已接（2026-11-07）。原版由 quartz 智能连接建出「音频→默认渲染器」支路；我们是 quartz ⇒ 宿主侧 `dshow::AudioRenderer` 承担同步（`cpu.clock` 同一份时钟；`pts <= 流now + 200 ms` 交设备、迟到 300 ms 丢弃、队列空记 underrun），`pl_mpeg` 解 MP2 → 44.1k→48k 重采样 → `AudioEngine::pushHostPcm` 混进同一路 SDL 输出；音量走 `IBasicAudio::put_Volume`（−100 dB = 静音）。`set:DependMovieSound` / `sound:Movie` 的语义未动（只影响音量）。
- **宿主解析 ALF**：**已否决**（见下「否决记录」）。
- **不新增宿主线程**：v1 用主线程协作泵（引擎每拍会问位置）。若实测证明取字节必须阻塞，再按音频
  feeder 的先例单开一张单，不顺手做。

## 否决记录

- **宿主自己解析 ALF 索引（`SYS4INI.BIN` + LZSS + `DATA*.ALF` 切片）**：**否决**。理由：引擎的
  `CreateFileA` 探松散文件 → 打不开时走**它自己的** ALF 层；`DirectShow` 接口不认识 ALF，说明字节
  必须由 guest 侧流对象交给宿主（引擎的 `CStreamReader` 就是干这个的）。宿主再实现一遍 ALF 是
  **同一语义两份实现**，还要靠 CreateFileA 探针猜影片名 ⇒ 越界且脆。
  ⇒ 正确路径：**从引擎源 pin 取字节**（`IAsyncReader` 是唯一标准拉取接口）。若 `SyncRead` 真的会等
  它的异步线程，再按"真线程 + 真同步"的先例解决，而不是回到 ALF。

## 已定口径

- **时间 = `cpu.clock`**：窗口模式/`--real-time` 下它就是宿主 `steady_clock`（墙钟），headless 下是
  虚拟钟。⛔ 不引入第二个墙钟（引擎的收尾/等待/跳过/暂停全在同一钟上）。
- **字节来源 = 引擎的源 pin**：guest 已做过名字解析 / 松散文件 / ALF 回退；宿主只做"按 pos/len 取一段"。
- **像素归引擎**：`CMovieTextureRenderer::DoRenderSample`（`0x487C60`）自己做 3/4bpp→32bpp、
  alpha 缩放、颜色键清零与 `LockRect`；宿主只喂样本，⛔ 不自己写纹理。
- **结束判据**：`get_StopTime() <= get_CurrentPosition()`（`0x4080B0` / `0x408130`）⇒
  `IMediaControl::Run` 必须把停止时间填成片长，否则 `0 <= 0` 一拍收尾（就是现在的零长）。

## 验收

1. **pure CTest**：图 / 引脚 / 媒体类型 / 分配器 / 样本协议（假引擎 filter 夹具）；
   解码模块（垃圾输入不崩、时间单调、`ended` 只置一次）。
2. **assets CTest**（真实安装，**不用 `--skip-logo`**）：从根脚本走到 LOGO 的 `0x20f`：
   - 影片真的开起来（宽高/时长非 0；交付样本数 ≥ N）；
   - 纹理/画面像素确实变了（非全黑，帧间有差异）；
   - 影片按时长收尾（位置到达停止时间后槽被清），脚本继续到 TITLE。
3. `ctest` 全绿；`pnpm test` 绿；`pnpm tools requirements validate` 绿。

## 证据锚（EA）

- `0x488DC0` `CMovieToTexture::Init`（`CoCreateInstance(FilterGraph)` + QI + `[+0x43C]` 模式 0 = DirectShow / 1 = MF）
- `0x489230` `Open`（建 `CStreamReader`（`0x1E8`）与 `CMovieTextureRenderer`（`0x5C0`）；`AddFilter` ×2；`Render(pin)`；文件由引擎经 VFS 读）
- `0x487C60` `DoRenderSample`（`IMediaSample::GetPointer` → 转换 → `LockRect` 写 `[+0x56C]` 纹理）
- `0x4080B0` / `0x408130` 停止时间 / 当前位置（`IMediaPosition` +0x28 / +0x24）
- `0x487A80` 名字含 `WMV`/`MP4` ⇒ MF
- `0x487270` 窗口影片每拍 `WaitForCompletion(10ms)`

## 进度口径

真源仍是需求树（本节点）+ `apps/emulator-lift/docs/movie-dshow-protocol.md`（协议契约）。
