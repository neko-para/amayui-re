# 让 feeder 也能吃段级 override（解锁项）+ PCM 转换段 0x48D767 落地

- id: REQ-01M75W6BB3PY3MF42ZTS23KTBS
- type: req
- status: open
- parent: REQ-01M6H6G6DSRG1ZRD810Z0HTH8V
- tags: [emulator-lift audio performance]

## 目标

让 **feeder 线程也能吃到"段级 override"**，并把 PCM 转换那一段（`0x48D767`）先落下去。
这是一个**解锁项**：没有它，所有段级优化在用户实际听到的模式里都是 **0 ms**。

## 为什么（已核，这是本单存在的理由）

- `runFeederGuest`（`audio.cpp:1753-1766`）**只查函数级 override 表**，没有段级表；
- 而 `thread_mode_ = Sdl && realTime`（`audio.cpp:185`）⇒ **默认的"真设备 + 窗口"就是 feeder 模式**；
- feeder 还有**私有 BlockCache**、没有 `window_prof_` ⇒ `--profile` / 窗口 profiler 在 feeder 下**看不到它**。

⇒ 段级 override 在 feeder 模式下会**静默不生效**，而 pump 模式却能量到"省了"。**这是必须防的假绿。**

## 先要落的读数（判据基础）

**凡 feeder 路径上的优化，判据只能用** `LIFT_AUDIO_TRACE=1` 的
`[audio] feeder fill … guest=XX ms … ov=N`：`ov` 不涨 = override 没生效。⛔ 不许用 profiler 判。

## 落地内容

1. **feeder 的段级 dispatch**：让 `runFeederGuest` 的块执行路径与主线程一致地查段级表。
   ⚠ 安全前提：段级 override 必须满足"在 feeder 上下文里也能跑"（不碰主线程私有状态、不假设已有令牌）。
   若不满足，就在表项上加"仅主线程"标记并跳过——⛔ 不许为了统一而放宽这个前提。
2. **PCM 转换段级替换**：入口 `0x48D767` → 出口 `0x48D89A`，纯算术、不碰 x87（让 guest 在 `0x48D8C9 fstp st` 自己弹 32767.0）。
   只写两处：PCM 缓冲 + `[ebp-108h]`（**裁剪标志必须写**，否则静默丢掉 "Clipping in frame" 消息）。
3. **数值必须逐位一致**：乘数是 **32767.0**（`0x127BB8` = `00 00 00 00 C0 FF DF 40`），**不是** 32768；
   guest 走 int64 截断取低 32 位再做**有符号**比较 ⇒ 用 `mulps(32767.0f)` 差 1 LSB，用 32768 会让每个负满幅差 1 LSB 且裁剪计数偏多。
   ⛔ 不允许"±1 LSB 可接受"。

## 判据

- 差分臂：解释器 vs 段级替换，逐样本逐字节比 PCM 输出 + 裁剪标志；边界含 `±32767`、`±32768`、`-32769`、`0`、`NaN`、`±Inf`、以及刚好落在裁剪边界的值。
- **feeder 生效判据**：`--audio sdl` 下跑 `graphs/se-eof.graph`，`[audio] feeder fill … ov=` **必须增长**（pump 模式增长不算数）。
- 一条变异：把乘数改成 32768.0f ⇒ 差分区间的负满幅用例必须红。
- 性能读数（只进文档）：`guest=` 那一列在改动前后对比。

## 依赖 / 排序

- **排在"令牌尖峰"（`REQ-01M75VZXV4SDMVVM1Z72G009XJ`）之后** —— 尖峰是当前听得见的 bug，本单是稳态优化。
- 收益口径**待重取**：本块每刀 **32.6 ms（直接求和 / 12 fills）** 与 **65 ms（残差）** 差 5.6×，以直接求和为准；
  重取一次（RelWithDebInfo + pump + 明确 `fills` 计数）之后才能写进预算。
- 与 Huffman/位读（`REQ-01M75PTEGCR1Y91FKKJ1CWPNH2`）是**不同段**，互不替代。
