# spike：AOT 臂能不能起 feeder（只读解码 cache 与重入，两条前提）

- id: REQ-01M7JN55VE9SA3KDHARYSSP6QW
- type: spike
- status: open
- parent: REQ-01M7G7GPBG9BK2360WSG6DBFNP
- tags: [emulator-lift, aot, audio, spike]

## 设问（限时，产出是结论不是实现）

AOT 臂要出声就必须有 feeder；而 feeder 今天**必须**用解释器的 `BlockCache`（`audio.cpp:1649/1677/2208`），
AOT 的 `cpu.cache == nullptr` ⇒ 与 `AGENTS.md` §12.8 第 1 条（执行来源只有 IR 与 shim）正面冲突。
**问：能不能在不引入第三份语义实现的前提下，让 feeder 起得来？** 本单只回答这个，不写一行产品代码。

## 两条前提（一条今天有读数、一条没有）

1. **续延机制是不是活的**：`repo:apps/emulator-lift/src/aot/aot_host.h` 声明 `hasNestedGuestExecutor() == false`，
   而 `repo:apps/emulator-lift/src/aot/aot_run.cpp` 里确实有一段 resume `GuestCallStack` 帧的代码。
   ⚠ 这两件事**不矛盾**（"挂起帧 resume" ≠ "嵌套 guest 执行器"），所以问题不是"谁错了"，而是：
   **那条 resume 路径今天真的被执行吗？** 已有读数形态：AOT 摘要的 `synthetic : 续延=N`
   （`aot_host.cpp:460` / `:517-523` 的 `callResumes_`）；一次 TITLE→quit 的 run 报 `续延=19`
   ⇒ **是活的**。本单要把"它能不能承载 feeder"钉死，而不是再证一次它存在。
2. **"只读解码 cache"能不能与"执行 cache"区分开**：若能，A 路成立（feeder 只拿它**取指/解码**，不执行）；
   若不能，只剩 B 路（feeder 经派发表取 `LiftFn`，在音频线程直调 —— 那就需要 `aot_run::run()` 的重入守卫；
   今天 `regs_` / `cpu_` 是单份状态）。

## 判据（结论必须落成这三条之一）

- **A 可**：给出"只读解码 cache 不构成执行来源"的**机械判据**（能让 §12.8 的守卫区分两者）+ 落地的改动点清单；或
- **B 可**：给出 `aot_run::run()` 的重入现状读数（`regs_` / `cpu_` 是否单份、并发下最先坏在哪一行）；或
- **两条都不行** ⇒ 明确写"必须改 §12.8 的口径"，并说清为什么（那是**裁决**，交用户，⛔ 不自裁）。
- ⛔ 结束时必须附**可复跑的读数**（命令 + 输出片段）；⛔ 不接受"我看了代码觉得"。

## 边界

⛔ 不做实现、不开子单、不动 `repo:apps/emulator-lift/src/runtime/audio.cpp`。
结论回到父单 `REQ-01M7G7GPBG9BK2360WSG6DBFNP` 的"选路"节。
