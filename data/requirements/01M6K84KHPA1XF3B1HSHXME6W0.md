# emulator-lift · M4 · 音频 / 视频（延后；音频可能提前）

- id: REQ-01M6K84KHPA1XF3B1HSHXME6W0
- type: req
- status: open
- parent: REQ-01M3TC7BK0J9TBQKGGWQ35A3NR
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
