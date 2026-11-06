# emulator-lift · 音频可行性：DirectSound 消费端 / 伪线程 / 最小可听路径

- id: REQ-01M6RSAZT5TFX1NV9EYA1SX674
- type: spike
- status: done
- parent: REQ-01M6K84KHPA1XF3B1HSHXME6W0
- done_reason: 只读评估完成：DSP 面薄（单 44.1k/16/2 PCM、无 3D/EAX），卡点是 guest 喂料线程（sub_4B5AA0@0x4B5AB6）不跑 ⇒ 环里最多 1.5 s；连续 BGM 需先裁决协作式 guest 线程。报告 repo:apps/emulator-lift/docs/audio-feasibility.md
- tags: [emulator-lift audio spike]

## 结论

音频 **DSP 面薄、能直接实现**：全引擎只有 44.1 kHz / 16 bit / 2 ch PCM（`0x4B5EF6..0x4B5F0B`），
无 3D / EAX / SetFX，`waveOut*` / `PlaySound` / MIDI / CD-DA 后端在 `.lst` 里 0 调用点。

**卡点不在解码，而在消费端**：能出声的 PCM 由 guest 自己的流式喂料线程产生。
`sub_4B5CF0` 在 `0x4B5FB5..0x4B5FD4` 建 61 个事件、`0x4B5FE4` `CreateThread(sub_4B5AA0)`；
`sub_4B5AA0@0x4B5AB6` 用 `WaitForMultipleObjects(0x3D, …, INFINITE)` 等 DirectSound 的 4 个位置通知，
每圈 `sub_4B6C30` 把 guest 解码器（WavePCM / OggVorbis / OggStream）的字节 `Lock/Unlock` 进
2 s 环形缓冲（352800 B，通知点 0 / L/4 / L/2 / 3L/4）。伪线程不跑 start routine ⇒ 只有主线程
Play 前预填的 3/4 圈（`0x4B74B4` 的 `for(i=1;i<4)`），最多 1.5 s：SE 能听完，BGM 是
“1.5 s 声音 + 0.5 s 静音”的 2 s 循环。

## 路径

- **不裁决也能做（第一刀，3–5 人日）**：真 DirectSound 设备模型（游标 / 4 通知 / 状态机）+
  `SetNotificationPositions` 真投递 + 宿主 `AudioSink` + `--audio-dump`；CTest `audio-device-model`
  机械断言 + 变异自检。
- **连续 BGM/语音（>1.5 s）** 只有两条路：① 协作式 guest 线程（第二个 guest 栈、仍单宿主线程；
  要改 `SCOPE.md` §1 / `README.md` §2.5 的单线程口径，**需用户裁决**）；② 宿主重写解码 + 环
  （更大、语义更假，不推荐）。没有第三条。

## 报告

repo:apps/emulator-lift/docs/audio-feasibility.md

报告还列出 4 条记账缺陷（含 `dsound.cpp` 声称引擎不 QI、实际 `0x4B7297` QI 了
`IID_IDirectSoundNotify`；`kCapsCtrlFrequency=0x08` 错标；winmm mmio stub 描述与实现相反）、
`[Sound+0x560]` 是死门（只有 ctor 写 0），以及 M4 单现有范围（MCI / mixer / mmio）与语料
（声音全在 DirectSound 流式缓冲）不一致。

## 需裁决

1. 是否允许协作式 guest 线程（第二 guest 栈、单宿主线程）；
2. 音频时钟档（virtual / SDL）；
3. 分阶段验收（第一刀是否只到“真设备模型 + dump”，不等连续 BGM）。
