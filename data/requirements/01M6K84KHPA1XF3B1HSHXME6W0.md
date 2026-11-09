# emulator-lift · M4 · 音频 / 视频（延后；音频可能提前）

- id: REQ-01M6K84KHPA1XF3B1HSHXME6W0
- type: req
- status: done
- parent: REQ-01M3TC7BK0J9TBQKGGWQ35A3NR
- done_reason: 范围裁决（2026-11-09）：两条腿都已交付 —— ① **音频**走 DirectSound 流式缓冲 + 真 feeder 线程 + 独立 mixer（REQ-01M6RXQ0A1B2C3D4E5F6G7H8J9 / 并发契约 / 默认静音三单全 done），SE 与 BGM 同一条 Play→notify→fill→混音 路径；② **视频**走 DirectShow 真服务端（REQ-01M6T07SC0XN81G1MZJYJXXGV9：宿主解码 filter 把帧送进引擎自己的 CMovieTextureRenderer）。本机 2026-11-09 实测 LOGO 影片自然放完：stop=stop-after-movie、[dshow] 首帧已送渲染器、tex movie #1..#120 的 hash 逐段变化（nonzero 0→1366→4328→14400）、called-unimplemented 0、exit 0。★ 范围里原列的 MF 7 / MFPlat 6 / EVR 1 / DXVA2 1 **不做**（记为超出范围）：引擎的影片路径是 DirectShow（quartz），这些 MF 导出在 boot→TITLE→影片→EU-ROOM→OPTION 的所有实跑里**一次都没被调用**（日志 grep 计数 0；run summary 恒为 imported 310 / called-unimplemented 0），它们仍是带理由的显式 stub（reason 见 data/lift/overrides.json）。MCI / mixer 同理：引擎不调用（audio-feasibility.md 的调用点扫描 + 实跑日志），保持 no-op stub；mmio 则早已是真实现，登记此前误标 stub，已在 REQ-01M6ZSF6RH014QZ907WN6WRFJW 修正。边界：Windows 实机复测仍只能由用户做（本机 macOS），音频在 Windows 上的设备/延迟读数未在本次收口内。
- tags: [emulator-lift, m4]

## 一句话
把 M1 期间按 `SCOPE.md` §1 no-op 的**音频 / 视频**补成真实现：音频优先（BGM / SE / 语音），视频随后（OP / 剧情影片）。★ 用户 2026-11-04：音频大概率比视频更靠前；具体顺序到 M2 时再调。

## 范围
- **音频（先）**：MCI 3（`mciSendCommandA` / `mciSendStringA` / `mciGetErrorStringA`）· mixer 3（`mixerOpen` / `mixerClose` / `mixerSetControlDetails`）· mmio 5（`mmioOpenA` / `mmioRead` / `mmioClose` / `mmioAscend` / `mmioDescend`）。计时复用 M1 的 `timeSetEvent` / `timeGetTime`。
- **视频（后）**：MF 7 · MFPlat 6 · EVR 1 · DXVA2 1（清单见 `pnpm tools lift report` 的 mf / mfplat / evr / dxva2）。影片 opcode 的推进与结束通知在 SCOPE 已保留。
- **依赖**：M2 的窗口 / 消息泵（音频回调可能需要线程或定时器；若届时仍坚持单线程，要先设计回调泵）、M1 的虚拟时钟。

## 判据（开工时写实）
- 音频：能听见 BGM / SE / 语音；静音 / 音量 / 停止的返回码与真机一致；mmio 能读出**真实**音频字节（不再是空流）。
- 视频：OP / 剧情影片能播放、能跳过、结束通知不丢；颜色键 / 缩放正确（若经 DDraw）。
- ⛔ 不允许把"没有声音 / 没有画面"当成成功：SCOPE 的 no-op 只在本单开工前有效。

## 边界
- 音频先支持默认设备；MIDI / CD-DA 等特殊后端若主线不需要，登记为不做。
- 视频若最终走 D3D 后端，依赖 M3；若走 DDraw / 软解，可在 M2 之后先做。

## 2026-11-06 评估补记（只读）

音频实际几乎全走 **DirectSound 流式缓冲**：15 槽 × 352800 B（2 s 环）+ 61 个位置通知事件 +
guest 喂料线程 `sub_4B5AA0`（`sub_4B5CF0` 在 `0x4B5FE4` 建线程；`0x4B5AB6` 等
`WaitForMultipleObjects(0x3D, …, INFINITE)`）。本次评估在 `.lst` 里**未见** MCI / mixer 的调用点，
mmio 只用于 WAV 头解析；⇒ 上面的“范围”不能当成音频实现清单。

连续 BGM / 语音受“伪线程不跑 start routine”限制：环里只有 Play 前预填的 3/4 圈（最多 1.5 s），
要先裁决是否允许**协作式 guest 线程**。完整报告（含证据 EA / 第一刀 / 3 个裁决项）见
repo:apps/emulator-lift/docs/audio-feasibility.md。

## 2026-11-07 音频已落地（方案 C）

用户先裁决 **V1 = D**，实测后澄清要真线程入口，已升级为 **C（真 feeder + 独立 mixer）**；
实现 / 实测 / 偏差见 `REQ-01M6RXQ0A1B2C3D4E5F6G7H8J9` 与
repo:apps/emulator-lift/docs/audio-implementation.md 。TITLE BGM 已连续可听；SE 走同一条
`Play → notify → fill → 混音` 路径，延迟 ≈100 ms。M4 仍 open **只为视频**（MF / EVR / DXVA2 / 影片 opcode）。
