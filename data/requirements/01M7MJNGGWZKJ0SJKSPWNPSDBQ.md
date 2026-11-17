# 🐞S2 SN0000→SC0000 的「渐变到黑」被一段 39.5e9 指令的计算整段吃掉 ⇒ 渐变塌成一帧（疑似空转/等线程）

- id: REQ-01M7MJNGGWZKJ0SJKSPWNPSDBQ
- type: bug
- status: open
- parent: REQ-01M7JHSD5MWTB9N8WJC2Y2T69M
- repro: .tmp/sc0000-gfx/report.md#C1
- severity: S2
- tags: [emulator-lift, sc0000, script-wait]

## 一句话

SN0000 最后一句之后那段「横向模糊 + 渐变到黑」**不是没画、也不是空转**：模糊是引擎的
**`BlurTexture`（`sub_4A62A0`，EA `0x004A62A0`）**做的逐像素 × 逐 tap x87 浮点采样循环
（循环体 `0x004A67E1`–`0x004A68F3`，约 100 条指令/tap，每 tap 两次浮点转整 `__ftol2_sse`@`0x4E82C0` / `__ftol2`@`0x4E82F6`），
每帧调一次。**真正的问题是每帧计算量几何增长**（0.85 → 1.74 → 4.68 → 12.6 → 20.5e9 条 ≈ **2.7×/帧**）
⇒ 一段 4–5 s 的纯计算把渐变每一步的等待预算整段吃掉，观感变成"一帧跳到黑"。

## 读数（2026-11-17 / mac arm64 / `links/raw` / headless / 二进制副本 sha `8332003f…`；报告 `.tmp/sc0000-fade/report.md`）

- 窗口（`--frame-log` 记账）：S1..S5 = 846,141,019 / 1,741,351,560 / 4,684,011,794 / 12,611,538,487 / 20,455,714,096 条；
  合计 39,492,615,937 条（与父级早先读数逐位对上；⚠ 早先"帧 1004→1005"的帧号差一位）。
- 段内 shim 每段仅 613–618 次（全是一次 `BlurTexture` 的 surface I/O：`LockRect`×2/`UnlockRect`×2/`HeapAlloc`×6/`HeapFree`×4/`UpdateSurface`）；
  `Sleep`=1、`timeGetTime`=3、**`WaitForSingleObject`=0、`GetExitCodeThread`=0**；全跑唯一 `CreateThread` 在 boot
  （`0x004B5FE4`，start=`sub_4B5AA0` 音频喂料，**伪线程不执行 start routine**）⇒ **"等一个不会来的线程"不成立**。
- 计算体止点（6/6 次 `--max-insns` 停点）= **`0x00508B27`**（`operator delete[](v74)`，双循环之后第一句，`.lst:387488`）。
- ★ 取数读数的两条坑：① 这段跑在 **S7 结构化单元**里 ⇒ 4–5 s 内 `--max-insns`/`--time-limit`/控制层闸门拿不到控制权
  （普通区 1e9 只超 5 条；转场区 30e9 超 14e9）；② **`--profile` 的 dispatch 采样看不见这段**（累计到 43.98e9 时 ms 第一名
  `004AF198` 只有 11 个采样点；`__ftol2_sse` 的 3 万+ 采样点全在 43.98e9 **之后**=白闪/章节卡段）。
- 时钟口径更正：AOT 臂是**墙钟**（`aot_run.cpp:374`）⇒ 预算是 2500 ms 墙钟，不是 2.5e9 条指令；`Sleep` 在 real 档对 guest 时间无效。

## 还没答的问题（下一步，判据已写）

**这个"每帧 2.7×"是引擎行为还是我们的偏差**：真机上这段应当**不是** 17–18 s 的纯计算（用户口径是"有动画"，不是"卡住"）。
⇒ 读 `0x004A6C50` / `0x0041EA22` callsite 的 **dot（a14）实参来源**：若它由 `timeGetTime` 派生 ⇒ 是"step 追赶"反馈环
（我们的每帧太慢 ⇒ 计时跳变 ⇒ 模糊半径/范围跳大 ⇒ 更慢）；若它是固定/脚本给的 ⇒ 说明我们的 x87 或 surface ping-pong 让范围增长。

## 判据（收口时）

- 那一段有**多级**灰阶帧（逐帧 `--frame-log` hash 序列单调变化 ≥ N 帧），而不是一帧跳黑；
- 且给出"每帧计算量为什么增长"的 EA 级归因（不是"画面变好看了"）。
- ⛔ 不许为了让画面像而改时钟/改预算（那是 guest 语义，要先立裁决）。
