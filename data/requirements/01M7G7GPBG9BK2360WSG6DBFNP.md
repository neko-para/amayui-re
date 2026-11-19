# AOT 音频管线：feeder 在 AOT 臂怎么跑（本臂音频恒 off 到真出声）

- id: REQ-01M7G7GPBG9BK2360WSG6DBFNP
- type: req
- status: done
- parent: REQ-01M7JHSDH16HCVEV4BA6NKSHN2
- done_reason: 能力已交付（不是 flag 缺口）：2026-11-17 落地 732e503（feeder 经派发表取 LiftFn，音频线程直接执行 IR；私有 audio_cpu_ 副本避免重入；不改 §12.8）。证据：docs/03-emulator/audio.md §2.1（AOT --audio sdl = 真 feeder 线程，feeder=host-thread / lead 19200 B）、§3.1 的 mac run（--audio sdl --sound ⇒ audio-pump: calls=0 fills=3 pushed=428800 feeder=host-thread lead=19200，pushed-played=19200 B）、§5.2 的 win32 3 跑（device=ok / underruns=0 / pushed=46,097,280）。子单 41H0 已 done（2026-11-19）⇒ 按"父 done ⇒ 子树无活"收口。
- tags: [emulator-lift, aot, audio]

## 现象（历史；2026-11-17/18 已修）

AOT 臂**曾恒无声**（用户 2026-11-17 实测："目前没有声音"）。当时确认**不是 flag 缺口**，是整条链路不存在：

- `repo:apps/emulator-lift/src/aot/aot_host.cpp` 硬写 `AudioMode::Off` + `sound=false`；
- `aot_run.cpp` 只有 `audio_.afterGuestCall()`，**没有 begin / pump / end**（对照解释器 `machine.cpp` 的 begin/pump/end 三个函数）；
- ⇒ 设备 / 混音 / 喂料 / guest DS 交付**全都不存在**。

★ 现状（2026-11-19 收口）：`--audio sdl --sound` 起真 feeder 线程（`feeder=host-thread`，lead 19200 B），feeder 的
guest 执行器走 AOT IR 派发（`repo:apps/emulator-lift/src/aot/aot_feeder.cpp`）；读数见
`docs/03-emulator/audio.md` §2.1 / §3.1 / §5.2（§3.1 的 mac run：`pushed=428800` / `played=409600` /
`pushed-played=19200 B`；§5.2 的 win32 3 跑 `device=ok` / `underruns=0` / `pushed=46,097,280`）。
子单 `REQ-01M7JFEA7J8AWQHPXTW6GB41H0` 已于 2026-11-19 `done` ⇒ 本节点按"父 done ⇒ 子树无活"收口。

## 能力面（本单的判据）

- `--sound` 下**真的有音频输出**，且一条可机械判的读数（例如 `fill` 计数 / 交付给 guest DS 的次数）
  与对照面**在同档下可比**（⚠ 解释器臂已退役 ⇒ 对照面改为 `Machine` 夹具，§12）。
- ⛔ **不许**用"我听到了"当判据。
- 守卫形状：一条 pure 断言（"`--sound` 的说明文案必须含本单号、且只在给了 `--sound` 时出现一次"）**已经存在**
  （`cli-aot-flags` 的 ④）⇒ 本单**不要**再重复它；要新增的是**能力**面那条。

## 选路（已裁决：B + feeder 私有 X86 副本；2026-11-17 落地 `732e503`）

原问题：feeder 一度**必须**用解释器的 `BlockCache`，而 AOT 的 `cpu.cache == nullptr` ⇒ 与 §12.8 第 1 条冲突。
已裁决并落地的是 **B**：feeder 经**派发表**取 `LiftFn`，在音频线程直接执行 IR；重入不靠重入守卫，而靠
`AudioEngine::audio_cpu_`（`cpu_` 的整份私有副本）—— `runAotFeederGuest` 只碰这份副本，主线程 `cpu_` / `regs_`
只在 fill 边界同步；注入 runner 时 `audio_cpu_.cache` 被置空（`audio.cpp:1813-1815`）⇒ 不需要 A 路的"只读解码 cache"，
也不改 §12.8。C 路（长期无音频）不再考虑。结论与读数见 spike 单 `REQ-01M7JN55VE9SA3KDHARYSSP6QW` 的 `done_reason`。

## 裁决（2026-11-17，用户口径）

> "不用过分研究如何让它和 input sys 配合。我理解绝大部分情况下，使用 input sys 的时候不会真正开启声音，
> 因此即使其内部在错误的 feed（比如重复播放），和实际在研究的内容（界面 / 内部状态）没什么关系。"

- ⇒ **"暂停必须 park feeder" 从"前置契约"降级为"可选加固"**：⛔ 不为它单独调研、⛔ 不设计音频↔输入通道。
- ⇒ 本单只在"**真出声**"这条能力上收口；音频侧的时序 / 配合问题**发现即记，不阻塞任何一轮**。
- ⇒ 2026-11-16 那份音频×暂停综合复审（`.tmp/audio-pause/`）里 §4/§5 的"配合契约"按本裁决**不再作为待办**；
  其中仍有效的只有 §3 的三条选路判据（已由 spike 收口）与 §7 的最小 spike（已转成 spike 单）。

## 复现 / 排障

```bash
cd apps/emulator-lift
LIFT_AOT_AUDIO_TRACE=1 ./out/emulator-lift-aot --skip-logo --max-frames 12 \
  --audio sdl --sound --game-dir ../../links/install --appdata <空目录> --log-level info
# 期望：audio     : mode=sdl … device=ok … muted=0；audio-pump: calls=0 fills>0 pushed>0 feeder=host-thread lead=19200
```

## 边界

- ⛔ 不动 §12.8 的词表；要改口径那是**裁决**（由 spike 的结论驱动，交用户）。
- ★ 音频与暂停 / 输入注入**不要求深度配合**（见上"裁决"）；发现真实阻塞再单开，⛔ 不要并进本单。
