# 🐞S2 存档 010 的 MVS*.MPG 被整部判死：音频头不是 MP2 ⇒ 视频可解却没播（已修：只播视频）

- id: REQ-01M813G170ZNEQZ9YK3PE0GB9Y
- type: bug
- status: done
- parent: REQ-01M7JHSD5MWTB9N8WJC2Y2T69M
- verify: apps/emulator-lift/tests/movie_decoder.cpp#video-only-audio-not-mp2
- repro: tools/mutate-check.mjs#音频头不是 MP2
- severity: S2
- tags: [emulator-lift, video, pl-mpeg, dshow]

> 挂载理由：影片→纹理这条链（DirectShow / dshow 渲染器）的既有缺陷都挂在「shim 层 · 图形侧」下（如 `REQ-01M7R1AR688T09G2T65GY8SZGG` 那条"只送出 1 帧"），本单是同一跳的**解码侧前置** —— 播不出来就进不了渲染器。

## 一句话

用户 2026-11-22 报：加载**存档 10**（`saves/SAVE10.DAT`）进 FIELD、点 **(540,450)**，因为**播不了一个特效**而退出。那"特效"是 `MVS*.MPG`（ALF 里恰 **65540 字节**、**120×120 / 30 fps** 的短小片）：它的 `0xC0` 包里没有 MPEG audio 帧头 ⇒ vendored pl_mpeg 把**整部片**判死，哪怕视频能完整解。修法 = **视频头是硬判据、音频只作降级**，⛔ 不改 vendored。

## 现象

- 用户口径：点 (540,450) 之后**因为播不了那个特效而退出**（不是黑屏、不是卡住）。
- 后果链：`MovieDecoder::open` 返回 false ⇒ dshow 的 `Render` 没有可接的输入 pin ⇒ 引擎拿不到帧 ⇒ 那次交互以退出收场。

## 根因（vendored 行号 + 逐字读数）

- `plm_init_decoders`（`repo:apps/emulator-lift/third_party/pl_mpeg/pl_mpeg.h` 的 `:939-948`）在 `num_audio_streams > 0` 时**无条件**建音频解码器 —— `audio_enabled` 只决定 packet type，**不阻止建对象**。
- `plm_has_headers`（同文件 `:979-984`）要求**视频头与音频头都成立**（`(video_decoder && !video_has_header) || (audio_decoder && !audio_has_header)` ⇒ false）。
- 这些片的 `0xC0` 包**没有 MPEG audio 帧头**（不是 MPEG-1 Layer II）⇒ 音频头永不成立 ⇒ 整部片判死，尽管视频可完整解。
- 逐字读数（2026-11-22 / win32 / `.tmp/save10-crash/probe2.exe`）：`probe2 MVS006.MPG` ⇒ `video_has_header=1`（`has_sequence_header=1`）但 **`has_headers=0`**；对照 `MVB001.MPG`（音频是 MP2）= **1**。视频-only 口径（`probe3.exe`）：`MVS006` / `MVS058` 解出 **26** 帧、`MVS023` **29** 帧，三者 `w=120 h=120 fps=30.000`。

## 修法（提交 `dcf7513`）

- `repo:apps/emulator-lift/src/runtime/video/movie_decoder.cpp`：`plm_has_headers` 为假 ⇒ `plm_set_audio_enabled(plm_, 0)` + `video_only_ = true`，再用 sequence header 的宽/高/帧率当判据（宽高 ≤0 或 fps ≤0 ⇒ **仍然拒绝**）；`nextAudio()` 在 video-only 下直接报"没有音频样本"（⛔ 不去 pl_mpeg 里扫一个解不了的音频流：`audio_packet_type = 0` 时它会读到流尾才返回）。
- 边界：⛔ **不改 vendored `third_party/pl_mpeg`**（三份副本逐字节相同；原 `tools/test/emulator-cxx-pl-mpeg-sync.test.mjs` 已于 2026-11-25 测试清理中删除 ⇒ 这条约束改为人工约定）；⛔ 不放宽视频判据；正常 MP2 片（LOGO 1280×720）走原路一字不变。

## 守卫与"红了意味着什么"

- CTest `movie-decoder` 的用例 `video-only-audio-not-mp2`（`repo:apps/emulator-lift/tests/movie_decoder.cpp`）：纯字节构造"视频头 OK + `0xC0` 包不是 MP2"的 PS —— ① 必须 open 成功且 120×120 / `fps ∈ (29,31)`；② 负对照（同长度但抹掉 sequence header）必须**仍然拒绝**。
- **红了意味着什么**：退了"只播视频"的退路 ⇒ `MVS*.MPG` 这类片重新整部判死（用户症状复现）；或退路放宽到"没有视频头也接受" ⇒ 坏片被当成 0×0 影片送进渲染器。
- 变异：`repo:tools/mutate-check.mjs` 里 `guard: 'ctest:movie-decoder'` 那条（拆掉退路 ⇒ 当场红）；实测红 = CTest 退出码 8，1/1。

## 读数（端到端）

- 入口 = `repo:apps/emulator-lift/scripts/plugins/scenario-save10-field.ts`（`--plugin-param slot=10`；跑前 `scripts/clean-appdata.sh <appdata>`）。
- **3/3 都 `finish(0)`**，每次都有：`[warn] [video] movie: 音频头不是 MPEG-1 Layer II ⇒ 只播视频` · `[video] movie: opened: 120x120 @ 30.000 fps, duration 0.933 s` · `[dshow] tex movie #1 w=120 h=120 nonzero=17 sampled=225`（像素真的进了纹理）· `scenario-save10: 点击之后 8000 ms 仍在跑`。
- ⚠ 这 3 次跑的是**页链改写前**的 `save.ts`；页链落地后**用最终 `save.ts` 又跑一次**：`calls=10 run=finished exit=0 reason=scenario-save10: survived`（页读数 `load-save: page 1 (target 1, slot 10)` —— 槽 10 就在当前页 ⇒ 不翻页）。两版在 `slot=10` 这条分支上逐字相同，⛔ 别把"3 次"说成"3 次都用最终版"。
- 门禁（2026-11-22 / win32）：`ctest` 137/137 · `pnpm test` 614/614 · `pnpm verify` 500 pass / 2 skip。

## 未做 / 边界

- 只播视频 ⇒ 这些片**没有声音**（不是静音补偿）。
- `MVS*.MPG` 是"刻意无音轨"还是"有非 MP2 音轨（MPEG-2 audio / AC-3 …）"**未取证** —— 两种都落进同一条退路。
- `plm_audio_has_header` 只认 MPEG-1 Layer II ⇒ 其它编码的音频仍然不会播。
- 退路只在 `plm_has_headers` 为假时启用；**没有**区分"音频坏了"与"根本没有音频流"（后者本来就走正常路）。

## 复现

```bash
node tools/mutate-check.mjs --match '音频头不是 MP2'   # 期望：CTest movie-decoder 红（退出码 8）
```

CTest：`ctest --test-dir apps/emulator-lift/out -R movie-decoder --output-on-failure`；缺陷现场全文见 `repo:docs/03-emulator/defects.md` 的 BUG-39。
