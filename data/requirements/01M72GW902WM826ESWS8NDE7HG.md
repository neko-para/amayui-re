# 🐞S2 emulator-lift · 退出段 C++ EH 展开里 ~1/10 概率 SIGSEGV（eip=004E743F，写 0x06AD1CD5）

- id: REQ-01M72GW902WM826ESWS8NDE7HG
- type: bug
- status: open
- parent: REQ-01M6H6G6DSRG1ZRD810Z0HTH8V
- repro: apps/emulator-lift/src/runtime/seh.cpp#RtlUnwind
- severity: S2
- tags: [emulator-lift, seh, crash]

## 一句话
同一条 TITLE→Quit 命令，**约 1/10 概率**在退出段的 C++ EH 展开里崩：`=== guest memory fault === SIGSEGV WRITE 0x06AD1CD5 eip=004E743F`（栈上有 `0xE06D7363`）。不是每次复现，也不是本单的"字体名表"那条（那条稳定复现）。

## 读数（2026-11-10，macOS arm64 / RelWithDebInfo）
- 命令与 `.tmp/quit-inv/logs/` 里的 run 相同（`--skip-logo --present window --present-hidden --raster gpu --input-script <at frame 130 / click 1190 630>`）。
- 观测：`guest memory fault` 的 `eip=004E743F`（`0x4E743F` 落在 `sub_4E73EB`/`_CallCatchBlock2` 一族；`0x4E738B` 是 `_CallCatchBlock2` 的入口，语料里这段是 COLLAPSED CRT，未登记函数）；故障地址 `0x06AD1CD5` 是**写**（栈里出现的 `0xE06D7363` = C++ 异常码 ⇒ 处于异常展开中）。
- 频次：同一命令 **9 次里 1 次**；其余 8 次正常 exit 0。

## 与已知缺陷的关系
- 与 `REQ-01M72GAA7CVKBPP9QRT0PRPBTM`（SEH：handler 返回 ContinueExecution 且 CONTEXT 未改 ⇒ 异常被静默丢弃）**同族区域但不同失效方式**：那条是"异常没被转移"，这条是"展开过程踩坏指针 → 真 fault"。
- 与 BUG-15 的 `Command_Exit_Exception` / `RaiseException` 同一段路径（退出期 C++ EH）。

## 判据（修好后必须成立）
1. 同一命令连续 20 次跑，`guest memory fault` 出现次数 = 0（可用 `grep -c` 机械判）。
2. 若判定为"本线不支持某个 EH 形状"，则必须**响亮停**（明确 detail），⛔ 不是 SIGSEGV。

## 未做
- 未拿到可复现的最小触发（1/10，未定位触发条件）；未做 ASAN / 栈回溯。
- 修复前建议先做：`sub_4E73EB` / `_CallCatchBlock2` 的 `.lst` 逐字阅读 + 在那三个 EA 上加 fault 现场打印（异常对象指针 / ThrowInfo / 已展开层数）。
