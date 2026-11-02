# audio 13 条转挂：不是后端缺失，是引擎侧可判 / 别的通道

- id: REQ-01M6DANAKTCW1F0AP21GN1P0R2
- type: req
- status: done
- parent: REQ-01M6DAMF1MN58WVR07ZYMQ8FCP
- done_reason: 18 条逐条裁决：4 条写进合同（0xb4 请求参数序 / 同一批 15 槽、0x2f8 通道号判据、0xbf 音量请求）、9 条收窄并指向正确通道、5 条真后端类保持 unknown；未改 discarded。独立复审无硬矛盾。
- tags: [spec, audio]

## 已落（2026-11-02）

18 条逐条裁决：**4 条写进合同 + 9 条收窄 + 5 条保持 `unknown`**。

**写进合同（4）**：

- `0xb4`：交给后端的请求参数顺序 = **（槽号、资源标识）**（与脚本侧读序 1→2 相反）；删 `RES-AUDIO-WAV-PARAM-ORDER`。
- `0xb4` 成立条件：**后端的**音效槽与语音通道是**同一批 15 槽**（语音槽 0/1/2 = 通道 12/13/14）；删 `RES-AUDIO-WAV-SLOT-VS-CHANNEL`。
- `0x2f8`：判据改成**通道号 ≥ 15（有符号）**；通道 = 组号 + 12；组号 −12..−1 不拒绝（写到非语音通道），< −12 负下标 = UB；删 `RES-OPS-VOICE-VOLUME-NEGATIVE`。
- `0xbf`：第 3 步写明 `set:KeepMusicVolume` / `sound:MusicFadeOnVoicePlaying` 都为 1、标记格为 0 ⇒ 读 `sound:MusicFadeOnVoicePlayingVolume`，值 = 键 × 100，播放请求之后发一次音量请求；两个键的命名含义不解释（只取"都为 1"）。

**收窄（9）**：`LOAD-FAIL`（转异常通道）· `RESOURCE`（整型 + 串表）· `SLOT-OOB`（表尾==对象尾 / T_Voice+0..0x10）· `TABLE-READER`（四个装载 handler）· `VOICE-SLOTS`（引擎侧读者）· `BGM-BIT9`（5 清 / 2 置 + 整字写点）· `FADE-A`（门模型 + cxx service branch）· `CLEAR-PROTOCOL`（bit 0x10000 写者）· `B6-SLOT-FIELDS`（两格身份 / 越界落点）。

**保持 `unknown`（5，真后端类）**：`NO-BACKEND` · `VOICE-RELEASE` · `VOLUME-BACKEND` · `BGM-FADE-UNIT` · `478-BIT0`。

## 判据

- 18 条每条有裁决；5 条 deferred 未改 `discarded`；
- `pnpm tools spec validate` + 独立复审绿；路径内 113 → 110。
