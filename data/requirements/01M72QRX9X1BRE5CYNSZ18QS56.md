# emulator-lift · 运行摘要的 present-tail 计数（最后一次 Present 调用 → run 停止的 guest 指令数）

- id: REQ-01M72QRX9X1BRE5CYNSZ18QS56
- type: req
- status: done
- parent: REQ-01M72GW002QVRSG5J5WJ0KB1KF
- verify: apps/emulator-lift/tests/input_injection.cpp#present-tail-stamp
- tags: [emulator-lift, summary, profile]

## 一句话
运行摘要里加一行 `present-tail:`：**本轮 run 内最后一次 `HeadlessPresenter::present()` 调用 → run 停止**之间的 guest 指令数（`tail_insns`），让「退出/停手前的尾段有多长」从"手算 `Σ[frame]`"变成**一眼可读、且在可复现档可进 CTest** 的读数。

## 为什么
`TITLE→Quit` 的 4.1 s 冻结只能靠 `LIFT_FRAME_TRACE=1` 的 `Σ[frame] wall` 与 `--profile` 窗口**手算**才知道（≈131 M 条指令 / 0 次 Present）。没有这一行时，「退出很慢」在 run summary 里**完全不可见** —— 摘要只有总 `insns`。

## 口径（实现即契约，逐条可判）
1. 盖章时刻 = `HeadlessPresenter::present()` 里 `++frame_count_` 的**同一刻**，且在 sink / dump **之前** ⇒ 记的是「Present **调用**」（含 sink/dump 失败的那次），⛔ 不是"成功"。
2. `any` = **本轮 run 内**出现过 Present（`run()` 开头的 `frame_at_run_start` 差分，⛔ 不是 `frameCount()!=0`；同进程多轮 `run()` 各自独立）。
3. 打印 4 字段：`last_frame` / `insns_at_last_present` / `tail_insns`（= 停止时 `insns` − 章，带 clamp）/ `tail_wall_ms`（宿主墙钟，单样本）。
4. 0 次 Present ⇒ **不打这一行**（打了会让"没卡"与"没测"混淆）。
5. **可复现档**（`--present headless` + 虚拟时钟）下 `tail_insns` 可进断言；real-time 档（`--present window` / `--real-time` / `--audio sdl`）三个数都只能当单样本。

## 判据
- 纯档 `present-tail-stamp`：run 内从 shim 调一次 `present()`，断言 `any` / `last_frame==1` / **`insns_at_last_present` 与 shim 自己记下的 `cpu.insns` 精确相等** / `tail_insns == insns − 章`；同一 Machine 第二轮不 Present ⇒ `any==false`。
- 纯档（0-Present 契约）：`any=false` 时摘要**不含**该行；`any=true` 时该行匹配正则。
- assets 档 `quit-tail`：TITLE→Quit 跑两次逐字相同命令，两次都 exit 0 + `[input-script] finished` + 恰好 1 行 + 恒等式 + 两次前缀逐字相同（⛔ 不断言任何当前数字）。

## 实测读数（2026-11-10，macOS arm64 / RelWithDebInfo，headless `--raster gpu`）
`present-tail: last_frame=150 insns_at_last_present=811344273 tail_insns=131104827 tail_wall_ms=2595.4`（两次独立 run 前缀逐字相同）。

## 未做
- `tail_wall_ms` 的**单位**无守卫（ms→us 的变异全绿）—— 它是读数，不进断言。
- `--max-frames ⇒ tail_insns == 0` 实测成立，但**没有用例钉**（README 已标"实测"）。
