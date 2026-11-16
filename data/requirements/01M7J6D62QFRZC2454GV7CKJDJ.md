# AOT 侧覆盖缺口（解释器退役的代价）：--time-limit 解析 / guest 退出码接线 / --help 编码 / --dump-frame 预建目录 / hidden 窗口 / 合成 PE 形态消失

- id: REQ-01M7J6D62QFRZC2454GV7CKJDJ
- type: req
- status: open
- parent: REQ-01M7JHSEKXW8KAKB6K2RS645M7
- tags: [emulator-lift, coverage, aot]

## 一句话

解释器 2026-11-16 退役后，**AOT 臂上没有任何端到端断言**的几个面。这是**已知代价**，⛔ 不是"没有缺口"。
本单只登记；要不要补由人按"用户可见的错 / 会不会有人发现 / 删了会不会静默通过"（§10）逐条裁决。

## 清单（每条：现在靠什么、代价是什么）

1. **`--time-limit` 的解析与语义**：原断言 `cli-time-limit`（`zero-equals-omit`：0 与省略等价）随解释器退役。
   今天只剩 `aot_main.cpp` 的实现，**没有**任何用例断言"0 = 不限 / 负数 = exit 2"。
2. **guest 退出码的接线**：映射本身有 pure 守卫（`lift-guest-exit`，`tests/guest_exit.cpp`），但
   "AOT 进程最终 `return exitCodeForGuestExit(shim.exit_code)`"这一行**没有 e2e**（执行源被 pin ⇒ 合成 PE 喂不进）。
   ⇒ 回归方式只能是代码审阅。🐞`REQ-01M7HP2YRZG222E0NE2H6MW6NK` 就是这样漏过一次的。
3. **AOT 的 `--help` / 日文 `--game-dir` 编码**：原 `host-encoding` 用例退役（它断言的正是解释器 CLI 的输出面）。
4. **`--dump-frame` 的"开跑前把目录建好"闸门**：原 `cli-dump-frame` 退役。AOT 侧**没有**这个闸门
   ⇒ 目录不存在时会跑到写盘失败才报（旧缺陷的形态是空转 546 s）。⚠ 这条最像"该补"的一条。
5. **`present : window=hidden`**：`gpu_batch.cpp` 的 `caseHeadlessHiddenWindow` 随 `--stdout-quiet` 一起退役
   （它靠起 `out/emulator-lift` + 合成 PE）。AOT 侧现在**没有**隐藏窗口的断言。
6. ★ **测试形态整体消失**："起一个真子进程 + 合成 PE + 断言 CLI 行为"这一整类在 AOT 上**结构性不可用**
   （IR 是构建期从被 pin 的那份 PE 抬升的）。⇒ 以后要测 CLI 面，只有两条路：**pure 单测**或**真安装的 assets 档**。

## 判据（补的话，每条能红）

- 4 ⇒ 新增一条 pure 档 CTest：自造一个不存在的目录当 `--dump-frame`，AOT 必须**在开跑前**建目录（exit 0 + 目录存在）；
  目录建不出来 ⇒ exit 2 且 stderr 点名 `--dump-frame`。⚠ 这条**不需要** guest 代码（在启动期判定）。
- 1 ⇒ pure 单测把 `--time-limit` 的解析抽成纯函数（`strtod + isfinite + <0 ⇒ 2`），断言 0 / -1 / abc / 1e400。
- 5 ⇒ 需要真安装的 assets 用例（`--present hidden --present window` 两档各跑一帧）；成本高，先裁决要不要。
- 2 / 3 ⇒ 只能靠代码审阅 + `docs/` 口径；⛔ 不要为了"有守卫"去伪造。

## 未做 / 边界

本单不实现任何东西。⛔ 不要把"补缺口"当成解释器退役的前置（退役已经完成，缺口是**已知债务**）。
