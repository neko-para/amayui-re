# 🐞S2 emulator-lift · assets e2e 的「环境不可用⇒SKIP」词表含 --game-dir ⇒ 真红静默变 SKIP

- id: REQ-01M7AQRGPWRWPHZRVTJ2FS09VB
- type: bug
- status: open
- parent: REQ-01M6H6G6DSRG1ZRD810Z0HTH8V
- repro: docs/03-emulator/observations.md#OBS-20261114-01
- severity: S2
- tags: [emulator-lift, ctest, false-green]

## 现象

四条起真子进程的 assets 用例把「环境不可用 ⇒ `LIFT_SKIP(77)`」的词表里塞了 `--game-dir`：

- `apps/emulator-lift/tests/close_confirm.cpp:125`
- `apps/emulator-lift/tests/agerc_ui_state.cpp:123`
- `apps/emulator-lift/tests/save_graph.cpp:127`
- `apps/emulator-lift/tests/recovery_dialog.cpp:166`

而 `apps/emulator-lift/src/main.cpp:1084-1088` 在**默认日志级别**下就会打出一行含 `--game-dir` 的提示
（AGERC.DLL 装载失败 ⇒ 继续）。⇒ 在**装不上 AGERC 的树上**，这四条用例的**任何非 0 退出都会被当成 SKIP**，
真红静默消失。

## 判据（修好后必须成立）

1. 「无 GPU」只认**双侧成立**的判据（子进程文案 + 本机实探建不出设备）——现成的正确形态在
   `apps/emulator-lift/tests/subprocess_skip.h`（`subprocessRefusedForNoGpu` 要求 `拒绝启动` **且**
   `GPU 光栅化后端` **且** `hostHasNoGpu()`）。
2. 「环境不可用」词表**不许**含产品在默认级别会打出的通用提示串（`--game-dir` 是这一类）。
   已在仓里写明这条的是 `apps/emulator-lift/tests/title_entry_paths.cpp:136-138`（"别这么写"）。
3. 修完：在**人为去掉/改坏 AGERC 的树**上跑这四条，失败必须表现为 **FAIL**（或带具体原因的 SKIP），
   不许是"静默 Skipped"。

## 证伪条件

如果 `main.cpp` 的默认日志级别**不会**打出含 `--game-dir` 的行（给出反驳的行号与级别判断），本条不成立。

## 非目标

- ⛔ 不改产品日志文案（那是另一件事；本条只修测试侧的 SKIP 判据）。
- ⛔ 不为了让这四条变绿去调大超时 / 放宽断言。

## 已知的同类历史

`REQ-01M6JFZFXE17DV6E6K174YE06X`（"测试/变异基建的假绿口子"）修过 `LIFT_SKIP` 吞失败；
本条是它的邻居：**SKIP 的触发条件本身太宽**，把"产品没起来/装不上依赖"也吞了。
