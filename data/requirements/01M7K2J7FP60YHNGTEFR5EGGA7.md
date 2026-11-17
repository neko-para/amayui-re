# AOT 虚拟音频档的交付游标用自开机绝对时间 ⇒ generateTo 静默 return ⇒ mixed/pushed 恒 0、dump 只有 44 B

- id: REQ-01M7K2J7FP60YHNGTEFR5EGGA7
- type: bug
- status: done
- parent: REQ-01M7JZM1QQ728KX1C0G2F232T1
- verify: apps/emulator-lift/tests/audio.cpp#audio-device/epoch-relative-cursor
- repro: apps/emulator-lift/src/runtime/audio.cpp#generateTo
- severity: S2
- tags: [emulator-lift, audio, bug]

## 一句话

AOT 的虚拟音频档里，**设备交付游标用的是"自开机以来"的绝对时间** ⇒ `played_` 是 8e10 量级 ⇒
`generateTo()` 在 `out_frames64 > 0xFFFFFFFF` 处**静默 return** ⇒ `mixed/pushed` 恒 0、`--audio-dump` 永远只有 44 B 头。

## 为什么单独立这一节点

它是 **S4（出声）的前置缺陷**，而且让"dump 里有没有 PCM"变成**随机器开机时长变化**的读数
（本机 5 天机龄 ⇒ 恒 0；刚开机 ⇒ 可能有）。这一类"同一份代码、同一份输入，读数取决于机器已运行多久"
是本仓最忌的不可复现来源之一。

## 机制（2026-11-17，只读普查，行号在 `0b7423c` 与 `0027af48` 的交付里）

1. `AotHost::init` 把 `VirtualClock` 设成 **realTime**（`clock_.setRealTime(true)`）—— AOT 没有 Machine，
   虚拟档只能借墙钟。
2. `AudioEngine::advanceDevice()` 在虚拟档把设备游标算成 `clock_->now100ns() * 192000 / 1e7`。
3. `now100ns()` 是 steady_clock 的**绝对值**（自开机）⇒ 游标 = 自开机以来的"应播放字节数"。
4. `generateTo()` 在 `out_frames64 > 0xFFFFFFFF` 处 **`return`（静默）** ⇒ 本机（5 天）恒命中。
5. 症状：`audio : … fills=12 fill_bytes=1152000 mixed=0 pushed=0`，WAV = 44 B（`data_size=0`），**没有任何告警**。

## 判据

1. **游标必须相对"这一轮引擎的起点"**，不是自开机：AOT 的虚拟档要么给 Clock 一个相对零点，
   要么在 `advanceDevice` 里用"引擎 start 时刻"做减。⛔ 不许靠"把 realTime 关掉"绕过 ——
   那会改 AOT 时钟档的既有口径（S0 谓词的一半输入）。
2. **`generateTo()` 不许静默 return**：超出范围必须**记账 + 出声**（一行 stderr / 一个计数），
   判据 = 任何一条越界路径都能被读数看见。
3. **dump 字节数不随开机时长变**：同一 argv、同一安装树跑两次（中间隔一段时间），
   `size` 与 `data_size` 必须相同；且"有 PCM"时不等于 44。
4. **不改 guest 可见语义**：`insns` / `stop @` / arena 三项 / `table misses` 逐位不变。

## 裁决（2026-11-17 · 父级）

- 这**不是** guest 可见语义变更：改的是**宿主侧记账口径**（游标原点），guest 只看到
  "现在该填第几帧"这件事——而它今天算出来的是一个荒谬值。⇒ 归为**缺陷修复**，不是语义单。
- ⛔ **不许**顺手改 dump 的 WAV 结构或采样率；只修游标原点与静默 return。
- S4 的判据（`pushed_bytes > 0` 增长）**依赖本单**：先修这里，再谈"能不能听见"。

## 未做 / 边界

- 不研究"音频怎么和 input 系统配合"（用户口径）。
- 不改 `AudioEngine` 的混音 / 重采样 / 音量（那些已登记在 `docs/audio-implementation.md` §5）。
- 真机行为未取证的地方写 `<待核>`，⛔ 不猜。

## 收口（macOS 2026-11-17 / main 工作树）

- 修法：`AudioEngine::begin` 记 `origin_100ns_`；虚拟档 `advanceDevice` 用 `now - origin`；`generateTo` 越界改为 `++stats_.gen_overflow` + 首次一行 Warn（不再静默）。时钟档 / WAV 结构 / guest 路径未动。
- 判据 1/2/3 的守卫：`apps/emulator-lift/tests/audio.cpp#audio-device/epoch-relative-cursor`（主）+ `#audio-device/generate-to-overflow-counted`；修前 222 checks / 8 failures，修后 224 / 0。
- 读数：`ctest --test-dir out` 104/104（4 skip 全是平台 / 无 GPU）；变异 `--match '设备游标退回'` ✅ 红 1/1（49.6 s）。
- 判据 4（guest 指纹逐位不变）：mac 上 AOT 真 run 的 CTest 是 Windows 专有，没有本机读数；本次改动只在宿主侧记账（`begin` / `advanceDevice` / `generateTo` / `dumpSummary`），guest 路径一行未碰 ⇒ 不阻塞收口。Windows 侧补一次 `--audio virtual --audio-dump` 真 run 即可（读数 = `gen_overflow=0` 且 dump `data_size>44`）。
