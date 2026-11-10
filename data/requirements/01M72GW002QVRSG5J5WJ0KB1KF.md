# 🐞S2 emulator-lift · TITLE→Quit：最后一帧后 4.1 s 无 Present（退出时同步写存档）+ 退场 fade 层与背景同纹理（没有渐变到黑）

- id: REQ-01M72GW002QVRSG5J5WJ0KB1KF
- type: bug
- status: open
- parent: REQ-01M6H6G6DSRG1ZRD810Z0HTH8V
- repro: apps/emulator-lift/src/runtime/present_sdl.cpp#postMessage
- severity: S2
- tags: [emulator-lift, quit, teardown, fade]

## 一句话
TITLE 点 Quit 之后：**(A)** 窗口停在最后一帧 **约 4.1 s**（期间 0 次 Present）才退出；**(B)** 真机会先渐变到黑，我们**一帧暗/黑都没有**。两者**不是同一个原因**：(A) = guest 侧同步拆除（退出时写存档 + 释放全部资源）× 解释器速度；(B) = 退场期那笔"fade 层"每帧提交的状态与背景**逐字相同**（同图叠同图，在 SRCALPHA 下对任意 alpha 幂等）⇒ 渐变不可见。

## 复现（2026-11-10，macOS arm64 / RelWithDebInfo，`apps/emulator-lift`）
```bash
./out/emulator-lift --game-dir ../../links/install --appdata <私有 tmp> --skip-logo \
  --present window --present-hidden --raster gpu --time-limit 120 \
  --input-script <脚本>        # 脚本：at frame 130 / click 1190 630
```
结局正常：`stop : stopped @ eip=004EE0EA` · `detail : shim 主动结束运行：kernel32!ExitProcess` · exit 0。

## (A) 4.1 s 卡住 = guest 同步拆除（宿主不是原因）
- 判据 1（无 Present 的尾部）：`LIFT_FRAME_TRACE=1` 的 `Σ[frame] wall = 26.846 s` vs `real = 30.971 s` ⇒ **最后一帧之后 4.125 s 内 0 次 Present**；同期 `user 35.230 s > real 30.971 s`（CPU 满载）、`--profile` 的 `host` 桶仅 `160.7 ms`（音频 28.4 / 事件泵 49.3 / 脚本 83.0），`guest+interp` 占 87%。
- 判据 2（排除宿主）：`--audio off` 4.13 s vs `--audio sdl` 4.18 s（差 ~50 ms）；日志里 `SetEvent(quit…)` 与 `feeder 线程已 join` **相邻无其它行**。
- 判据 3（他做什么）：`sub_40AAE0`（写 SAVE.DAT；`MoveFileA`/`DeleteFileA`）→ `sub_437480` → `sub_4367B0`（LZSS + `sub_436550` self 1.21 s / 150,086 次），点击后窗口 inclusive **3.16 s**；落地件 `SAVE.DAT 48,128 B` + `RT.DAT 276 B` + `SYS4REG.INI 918 B`，且只在**最后一次 Present 之后**出现（`--max-frames 131` 的 run 里 appdata 只有 `$$AGE.TMP`）。
- ⇒ 真机同一段只要 ~100 ms，所以用户看不到；我们慢的是**解释执行 LZSS + 文件 I/O**，不是卡在某个等待上。

## (B) 没有渐变：Quit 支**按引擎代码根本不走**减光/确认（另一条独立支）
- 引擎的 WM_CLOSE 处理在 `sub_4B92F0`（WndProc）case 16：`0x4B95FA cmp ebx,1` / `0x4B95FD jz` ⇒ **wParam==1 是"强制关窗"，跳过 `call sub_4057B0`（0x4B95FF）**；只有 wParam!=1 才走 `sub_4057B0` 的**减光（`0x4058A9 push 80h` → `0x4058BC`，图层 0x1F、alpha 128）→ 二次确认对话框 → alpha 255 全黑**。
- 实测（`--trace-shims`）：TITLE 的 Quit 按钮支由引擎自己的退出清理段发 `user32!SendMessageA(00010000, WM_CLOSE=0x10, wParam=1, 0)`（返回地址 `0x4138C8`，`sub_412290`）⇒ **走强制支**；全表里 `ret=0x405xxx` 的调用计数 = **0** ⇒ `sub_4057B0` 一次都没跑。
- 对照（引擎确有减光/全黑能力）：ESC 支 = 引擎自己在 WM_KEYDOWN 0x1B 上 `PostMessageA(hwnd, WM_CLOSE, 0, 0)`（EA `0x4B9373`）⇒ 走 `sub_4057B0` ⇒ `frame 131` 背景 `0x00FDF3E9→0x007E7974`（减光）、`frame 132` **921,600 px 全 `0x00000000`**、`frame 133` 恢复。
- ⇒ **"Quit 没有渐变"不是渲染丢帧，也不是 fade 层画错**：这条支按引擎代码就没有减光/确认。点击后每帧多出的那笔全屏四边形（`draws` 1→2，两笔字段逐字相同）不是 fade 层（见 `<待核>` 1）。
- 推论（`<待核>`）：真机在 TITLE 点 Quit 若**真的**渐变到黑，说明真机走的是 wParam!=1 那条（或脚本层另有 fade）—— 那与本仓"引擎自己发 WM_CLOSE(1)"的实测冲突，需要真机侧一次观测才能定案。

## <待核>（决定 (B) 怎么修）
1. 那笔"第二个全屏四边形"的**纹理身份**：是同一张背景纹理，还是"抓屏 → 纹理"抓失败后回落到同一张？（`[draw]` 只打 `w/h/format`，不打纹理 id / 顶点色）
2. 顶点 diffuse 的 alpha（`fvf=0x244` 含 `D3DFVF_DIFFUSE`）在 31 帧里是否真的推进过（需探针）。
3. 真机 TITLE 的 Quit 是否也发 `WM_CLOSE(1)`；若也是，则真机的"渐变"来自脚本层 fade（另一条命令路径），本单 (B) 的修法就不是改渲染。

## 判据（修好后必须成立）
1. (A) run summary 能给出**确定性的** `present-tail`（最后一帧 → 退出的 guest 指令数；可进 CTest），且退出期不再是"40 M 量级的 LZSS 解释"——或明确登记为**已知性能偏差**（真机 ~100 ms）。
2. (B) 退出序列的**连续帧像素均值单调下降**且**至少一帧**背景像素值 ≈ `0x00000000`（ESC 支已经能做到，可作为同一判据的参照）。

## 未做
- 未定位 (B) 那笔叠加层的纹理来源（需加一行诊断：打印纹理 id + 顶点色 alpha）。
- 未改任何代码；未 commit。
