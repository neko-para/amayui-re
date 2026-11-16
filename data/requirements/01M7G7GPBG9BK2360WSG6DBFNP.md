# AOT 音频管线：feeder 在 AOT 臂怎么跑（本臂音频恒 off 到真出声）

- id: REQ-01M7G7GPBG9BK2360WSG6DBFNP
- type: req
- status: open
- parent: REQ-01M7JHSDH16HCVEV4BA6NKSHN2
- tags: [emulator-lift, aot, audio]

## 现象

AOT 臂**恒无声**。用户实测："目前没有声音"。查清后确认这**不是 flag 缺口**，是整条链路不存在：

- `src/aot/aot_host.cpp:87-96` 硬写 `AudioMode::Off` + `sound=false`；
- `aot_run.cpp` 只有 `audio_.afterGuestCall()`，**没有 begin / pump / end**
  （对照解释器 `machine.cpp:1066` / `:1542` / `:1861`）；
- ⇒ 设备 / 混音 / 喂料 / guest DS 交付**全都不存在**。

本轮已做（**只做可指定与说真话**，不是本单）：AOT 臂 `--sound` 现在**可指定**，并打一条**一次性**说明
（"本臂不会有声音 + 理由 + 本单号"），⛔ 不 exit 2、⛔ 不静默。文案唯一落点
`src/runtime/present_flags.h` 的 `kAotSoundNotice`。

## 已知的硬阻塞（先解决它，才谈出声）

真 feeder 线程**必须用解释器的 `BlockCache`**：`audio.cpp:1649` / `:1677` / `:2208`。
而 AOT 的 `cpu.cache == nullptr` ⇒ 与 `AGENTS.md` §12.8 第 1 条（**AOT 的执行来源只有 IR 与 shim**）正面冲突。

⇒ 三条互斥的路，必须选一条并给出**可证伪的判据**：

| 路 | 做法 | 代价 / 风险 |
|---|---|---|
| **A** | 给 AOT 一个"只读解码 cache"（不是执行 cache）：feeder 只拿它**取指/解码**，不执行 | 要证明它不会变成执行来源；§12.8 的守卫要能区分 |
| **B** | feeder 改成经**派发表**取 `lift_cc_*` 函数指针（`LiftFn`），在 AudioEngine 的线程里直接调 | 重入问题：`aot_run` 的 `run()` **没有重入守卫**，`regs_` / `cpu_` 是单份状态 |
| **C** | 明确 drop：AOT 臂长期无音频，只保证"可指定 + 说真话" | 最省，但用户要的是能听见 |

## 一个必须先定死的可疑矛盾（未定）

`src/aot/aot_host.h:320` 声明 `hasNestedGuestExecutor() == false`，
但 `src/aot/aot_run.cpp:138-157` **实际会 resume `GuestCallStack` 帧**（注释还提到音频泵借 `[esp]`）。
这两句不能同时为真。⇒ **先实测这条**：若 `aot_run` 那条路是活的，B 路的重入问题可能已经解决了一半
（也可能相反：那条路本身就是坏的）。

## 复现 / 排障

```powershell
.\out\emulator-lift-aot.exe --skip-logo --present window --sound --game-dir ..\..\dist\install --appdata <空目录>
# 期望：stderr 出现那条一次性说明（含本单号）；⛔ 不会有声音（本单未做前）
```

## 判据（收口时要能挂上）

- 能力面：`--sound` 下**真的有音频输出**，且一条可机械判的读数（例如 `fill` 计数 / 交付给 guest DS 的次数）
  与解释器臂**在同档下可比**。
- ⛔ **不许**用"我听到了"当判据。
- 守卫形状：一条 `pure` 档断言（"`--sound` 的说明文案必须含本单号、且只在给了 `--sound` 时出现一次"）已经存在
  （`cli-aot-flags` 的 ④），本单**不要**再重复它；本单要新增的是**能力**面那条。
