# emulator-lift · 影片准备不再停在 ole32!CoCreateInstance（DirectShow / AMultiMediaStream 形状层）

- id: REQ-01M6QPYG431WK2M3C76F6H1XBZ
- type: bug
- status: done
- parent: REQ-01M6K84KHPA1XF3B1HSHXME6W0
- verify: apps/emulator-lift/tests/quartz_video.cpp#quartz-video
- repro: apps/emulator-lift/tests/quartz_video.cpp#quartz-cocreate-filtergraph
- severity: S1
- done_reason: 形状层已落地并挂在 quartz-video（134 checks / 0）：CLSID_FilterGraph / FilterGraphNoThread / AMultiMediaStream 返回 guest 可见对象，Run/Stop/GetState/WaitForCompletion(EC_COMPLETE) 真实现，未知 CLSID ⇒ CLASS_E_CLASSNOTAVAILABLE、未知 IID ⇒ E_NOINTERFACE（不冒充支持）；其余站点（Live2D/GIT/MemoryAllocator）走真实「类未注册」失败。★ 此后更进一步：影片不再只是形状层 —— 真 DirectShow 服务端已实现（repo:apps/emulator-lift/docs/movie-dshow-protocol.md：图管理器 + 宿主解码 filter 把帧送进引擎自己的 CMovieTextureRenderer），2026-11-09 在本机真安装上跑到 LOGO 影片自然结束：stop=stop-after-movie、[dshow] 首帧已送渲染器、tex movie #1..#120 的 hash 逐段变化（nonzero 0→1366→4328→14400）、called-unimplemented 0、exit 0。唯一边界：Windows 实机复测仍只有用户能做（本机 macOS 不能替），以及 IAMMultiMediaStream 的 DDraw 媒体流接口按原判据不伪造（让 Movie::Init 走失败分支）。

## 复现

- 实机 / 用户口径（2026-11-06）：影片准备阶段停在 `ole32!CoCreateInstance`（loud trap）。
- 本机 headless 冷启动（`--appdata` 全新、`--max-frames 300/1500`）**没有**走到这条路径：
  影片由脚本 opcode 在特定路线才触发，当前无输入注入；因此以 pure 夹具固定形状层的契约。

## 期望 / 实际

- 期望：`CoCreateInstance(CLSID_FilterGraph / CLSID_AMMultiMediaStream)` 不再 loud trap；
  `IMediaControl::Run` / `IMediaEvent::WaitForCompletion` 立即完成，影片 opcode 继续。
- 实际（修后）：`FilterGraph` / `FilterGraphNoThread` / `AMMultiMediaStream` 返回 guest 可见的形状对象；`WaitForCompletion` 立即 `EC_COMPLETE`；
  未知 CLSID ⇒ `CLASS_E_CLASSNOTAVAILABLE`、未知 IID ⇒ `E_NOINTERFACE`（不冒充支持）；非影片 CLSID 的
  其他站点（Live2D / GIT / MemoryAllocator）也走真实的"类未注册"失败，而不是假装成功。
- ⛔ 不是真解码：MF 的 `MFCreate*` 仍返回 `E_FAIL`；`IVideoWindow` 不建宿主窗口；
  `IMediaSeeking` / `IMediaPosition` 时长恒 0。

## 影响

- 解除"准备视频"这一步的硬停，让脚本能继续跑；标题 / 后续场景是否真的到达仍取决于输入与影片 opcode 所在的路线。
- 落点：`repo:apps/emulator-lift/src/shims/quartz_video.cpp`、
  `repo:apps/emulator-lift/tests/quartz_video.cpp`、`repo:apps/emulator-lift/SCOPE.md`。

## 验证

- `ctest` 的 `quartz-video`：134 checks / 0 failures（`CoCreateInstance` + QI + Run/Stop/GetState +
  WaitForCompletion/EC_COMPLETE + GetDuration/GetCurrentPosition=0 + IAMMultiMediaStream 形状 + 失败路径）。
- 仍待：有输入的 Windows 实机复测（用户）——本机不能替。
- 边界：`IAMMultiMediaStream` 的 DDraw 媒体流接口（引擎自己的 `&unk_52FC18` / `&unk_52FC08`）
  **不伪造**（需要真 DDraw surface / sample）；QI 返回 `E_NOINTERFACE`，让 `Movie::Init` 走失败分支，
  ⛔ 不用"形状对象"硬顶（那会让后续 +0x24/+0x34 的 stdcall 参数个数不匹配）。
