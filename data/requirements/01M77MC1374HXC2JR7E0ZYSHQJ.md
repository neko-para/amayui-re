# 🐞S2 emulator-lift · TITLE→Quit 命中 unimplemented @ eip=0x109A5A71（sn0000-quit.graph：ADV 退出后回 TITLE 再 Quit）

- id: REQ-01M77MC1374HXC2JR7E0ZYSHQJ
- type: bug
- status: done
- parent: REQ-01M6H6G6DSRG1ZRD810Z0HTH8V
- verify: apps/emulator-lift/tests/quit_graph.cpp#quit-graph
- repro: graphs/sn0000-quit.graph#click 1180 640
- severity: S2
- tags: [emulator-lift, quit, title, unimplemented]

## 现象
`graphs/sn0000-quit.graph`（TITLE → GAMESTART → ADV（SN0000）→ 右键退出 → FELLOW → … → 回到 TITLE →
点 Quit `1180,640`）跑到 Quit 时**不是正常退出**，而是：

```
stop      : unimplemented @ eip=109A5A71
```

用户实测 2026-11-12。EIP `0x109A5A71` 是**宿主/guest 的某个动态地址**，不是 AGE.EXE 的 `.text`
（镜像基址 0x400000）里的正常指令地址。

## 复现
```bash
cd apps/emulator-lift
./out/emulator-lift --appdata <tmp> --game-dir ../../links/install \
  --present window --present-hidden --raster gpu --native-overrides all --skip-logo \
  --interaction-graph ../../graphs/sn0000-quit.graph --max-frames 4000
```
（图里 TITLE 点"はじめから" `1186,373`、GAMESTART 点"下一步" `830,592`、ADV 右键 `500,500`、
FELLOW / TITLE 的后续点击、最后 TITLE Quit `1180,640`。）

## 已知（待核，⛔ 不当结论）
- `0x109A5A71` 可能落在 **guest 堆**（`GuestLayout::heap_base = 0x10000000` 一档）或某个动态 stub 区；
  先机械核 `GuestLayout` / `ComRegistry` 动态槽 / `SurfaceArena` / 堆区间，再谈语义。
- `stop : unimplemented @ eip=…` 的文案来自 dispatcher 的"没有实现"分支（`machine.cpp` 的
  `had_impl` / `unimplemented_calls`），**不是** guest 指令异常；eip 指向的应是一条没有实现的
  import / COM vtable 槽 / 动态 thunk（或一个被写坏的函数指针）。
- 与既有 TITLE→Quit 单（`REQ-01M72GW002QVRSG5J5WJ0KB1KF`）相邻但不同：那张是"最后一帧后 4.1 s 无 Present +
  退场 fade"，本单是 Quit 路径上的 unimplemented。

## 分析计划（先取证，不猜）
1. `--trace-shims` / `--until` / 日志定位：最后一条成功的 shim、返回地址、栈上的对象 / vtable；
2. 机械映射 `0x109A5A71` 的归属（布局常量 / 动态 stub 分配 / 堆 / arena），给 `文件:行`；
3. 对照 `sn0000` / `FELLOW` / TITLE→Quit 的既有语义与 AGERC `_ShowDialog` / Quit 路径；
4. 修复：实现缺失的 import / COM 槽，或修正被写坏的指针来源；加可复跑的守卫（同 graph 子进程
   `exit 0` 且 `stop` 不是 `unimplemented`）。

## 判据
- 同 graph 复跑不再出现 `unimplemented @ eip=…`；退出路径按引擎语义收口（Quit / ExitProcess）；
- 新守卫能红（修前该 graph 的 `stop` 行必须是 `unimplemented`），`ctest` 默认档绿。

## 未做 / <待核>
- 未取证 `0x109A5A71` 的归属与调用点；本单先开单 + 分析，修复另派。

## 2026-11-12 分析（`.tmp/audio-token/quit-unimplemented-analysis.md`）
- `0x109A5A71` ∈ **guest 堆** `[0x10000000,0x18000000)`（`image.h:93-94`），偏移 9.65 MiB；`≡1 mod 8` ⇒ **不是**任何分配起点
  （块内奇地址）；排除桩页 / SurfaceArena / VirtualAlloc / AGERC / 栈 / TEB。堆 128 MiB 全提交 ⇒ 解码器会把堆数据当指令。
- 触发 = **`machine.cpp:1663`**（块内 `execInsn` 失败，eip = 失败指令自身），**不是缺 import**（`unimplemented_calls` 只在 shim 内自增，
  exec 路径不碰；桩页那处的 eip 必在 `0x7FFE…`）。
- 调用点 = stderr 的 `guest stack` 顶部 `[esp]`（`exec.cpp:276-290` 已打印，缺陷单只是没抄）；Quit 链：
  `0x418E60` 抛 Command_Exit_Exception → `_JumpToContinuation` → **`0x4138C2 SendMessageA(WM_CLOSE,1,0)`** → WndProc case16
  （`wParam==1` 跳过 `sub_4057B0`）→ 前 4 次间接调用全走 **[T_Command+0xAA514] 的堆内 vtable 指针** → shutdown（`sub_40FDE0` 的 5 处 `call edx`）→ ExitProcess。
- 修复顺序：**A** 诊断（`regionOf` + 最近 24 条指令 + "数据地址当 eip"警告，零成本）→ 跑 graph 抓 `=== unimplemented instruction ===`
  整块与 `[esp]` → 再决定 **B**（按 regionKind 拒绝堆/栈）、**C**（按调用点修生命周期 / FreeLibrary 段校验 / SEH CONTEXT）。
  ⛔ 不补 import（机械上已否定）；⛔ 不把并发/堆损坏当结论。

## 2026-11-12 实现 + 取证
- **根因（实测）**：音频 feeder（真宿主线程、跑 guest 代码）与主线程**共用 TEB** ⇒ feeder 的 SEH prologue/epilogue
  改写主线程 `fs:[0]` 链头（一次 run **173,755 次**）；Quit 的 `_CxxThrowException` 派发到 feeder 栈上**已被复用**的
  record ⇒ `Handler` 是数据（堆 EA / ThrowInfo）⇒ `eip` 落到 guest 堆。⛔ 不是缺 import、不是堆损坏、不是引擎 stale 指针。
- **修法**：`runtime/audio.cpp` 的 `setupFeederContext()` 给 feeder 一份**自己的 TEB**（`GuestLayout::feeder_teb` = 0x7FFD6000；提交前显式查占用），
  `audio_cpu_.fs_base` 指过去（每线程 `fs:[0]` / 栈界 / Self / ThreadId）；⛔ 不动 audio 语义、不做 per-thread TLS。
- **诊断（零行为改变）**：`runtime/exec.cpp` 的 `unimplemented()` 打 `regionOf(eip)` + 最近 24 条 ring +
  "数据地址当 eip"警告（调用点 = `[esp]`；`LIFT_SEH_TRACE` 下另有链头逐环区域归属 / feeder `fs:[0]` 观察点）。
- **守卫**：CTest `quit-graph`（`tests/quit_graph.cpp`；真 feeder 臂 `feeder=host-thread`+`device=ok`，同一 graph
  跑两遍；修后 `stop : stopped @ 004EE0EA` + `ExitProcess`）。变异 `tools/mutate-check.mjs` 的 `ctest:quit-graph` ⇒ 红。
- 完整读数 / 复现 / 判据：`docs/03-emulator/defects.md` 的 **BUG-22** 与 `.tmp/audio-token/quit-unimplemented-analysis.md` §9。
- ⛔ **headless 不是有效臂**：本图在 headless 下停在 step3（等 `script GAMESTART` 超时）；`--audio off` 没有 feeder。
