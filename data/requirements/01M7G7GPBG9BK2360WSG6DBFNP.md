# AOT 音频管线：feeder 在 AOT 臂怎么跑（本臂音频恒 off 到真出声）

- id: REQ-01M7G7GPBG9BK2360WSG6DBFNP
- type: req
- status: open
- parent: REQ-01M7JHSDH16HCVEV4BA6NKSHN2
- tags: [emulator-lift, aot, audio]

## 现象

AOT 臂**恒无声**。用户实测："目前没有声音"。查清后确认这**不是 flag 缺口**，是整条链路不存在：

- `repo:apps/emulator-lift/src/aot/aot_host.cpp` 硬写 `AudioMode::Off` + `sound=false`；
- `aot_run.cpp` 只有 `audio_.afterGuestCall()`，**没有 begin / pump / end**（对照解释器 `machine.cpp:1066` / `:1542` / `:1861`）；
- ⇒ 设备 / 混音 / 喂料 / guest DS 交付**全都不存在**。

本轮已做（**只做可指定与说真话**，不是本单）：AOT 臂 `--sound` 现在**可指定**，并打一条**一次性**说明
（"本臂不会有声音 + 理由 + 本单号"），⛔ 不 exit 2、⛔ 不静默。文案唯一落点
`repo:apps/emulator-lift/src/runtime/present_flags.h` 的 `kAotSoundNotice`。

## 能力面（本单的判据）

- `--sound` 下**真的有音频输出**，且一条可机械判的读数（例如 `fill` 计数 / 交付给 guest DS 的次数）
  与对照面**在同档下可比**（⚠ 解释器臂已退役 ⇒ 对照面改为 `Machine` 夹具，§12）。
- ⛔ **不许**用"我听到了"当判据。
- 守卫形状：一条 pure 断言（"`--sound` 的说明文案必须含本单号、且只在给了 `--sound` 时出现一次"）**已经存在**
  （`cli-aot-flags` 的 ④）⇒ 本单**不要**再重复它；要新增的是**能力**面那条。

## 选路（未定；定案后本段降级成一行裁决）

真 feeder 线程**必须用解释器的 `BlockCache`**（`audio.cpp:1649` / `:1677` / `:2208`），而 AOT 的 `cpu.cache == nullptr`
⇒ 与 §12.8 第 1 条（执行来源只有 IR 与 shim）正面冲突。三条互斥的路：

| 路 | 做法 | 代价 / 风险 |
|---|---|---|
| **A** | 给 AOT 一个"只读解码 cache"（不是执行 cache）：feeder 只拿它**取指/解码**，不执行 | 要证明它不会变成执行来源；§12.8 的守卫要能区分 |
| **B** | feeder 改经**派发表**取 `LiftFn`，在音频引擎线程里直接调 | 重入：`aot_run::run()` 没有重入守卫，`regs_` / `cpu_` 是单份状态 |
| **C** | 明确 drop：长期无音频，只保证"可指定 + 说真话" | 最省，但用户要的是能听见 |

★ **选路不由本单自裁**：A/B 能不能成立取决于 `REQ-01M7JN55VE9SA3KDHARYSSP6QW`（spike）的两条读数
（续延机制是否真的被执行、只读解码 cache 能否与执行 cache 分开）⇒ **先做 spike，再由用户裁路**，本单才动手。

## 复现 / 排障

```powershell
.\out\emulator-lift-aot.exe --skip-logo --present window --sound --game-dir ..\..\dist\install --appdata <空目录>
# 期望：stderr 出现那条一次性说明（含本单号）；⛔ 不会有声音（本单未做前）
```

## 边界

- ⛔ 不动 §12.8 的词表；要改口径那是**裁决**（由 spike 的结论驱动，交用户）。
- ⛔ 不把"音频与暂停/恢复的配合"写进本单（另一条工作线：暂停必须 park feeder）。
