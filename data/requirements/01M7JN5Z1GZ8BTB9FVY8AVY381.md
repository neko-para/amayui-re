# AOT 覆盖缺口（需真安装 assets）：present hidden 窗口两档各跑一帧

- id: REQ-01M7JN5Z1GZ8BTB9FVY8AVY381
- type: req
- status: open
- parent: REQ-01M7J6D62QFRZC2454GV7CKJDJ
- tags: [emulator-lift, aot, present, assets]

## 为什么单独一张（父单 `REQ-01M7J6D62QFRZC2454GV7CKJDJ` 的三分之一）

`present : window=hidden` 的断言**必须起真安装**（assets 档）。解释器臂原来那条 `caseHeadlessHiddenWindow`
随 `--stdout-quiet` 一起退役了（它靠起 `out/emulator-lift` + 合成 PE；那段代码今天**已不在源码里**）⇒
AOT 侧今天没有任何用例断言"窗口存在且被隐藏"。
与父单另外两格的区别是**成本**：这格要 assets，不是 pure、也不是人读。

## 今天在的地方（要断言的正是这两处）

- 判定：`repo:apps/emulator-lift/src/aot/aot_main.cpp` 的 `wopt.hidden = present_mode == "headless" || present_hidden;`
  （以及 `--present-hidden` 只为 `window/auto` 生效）；
- 读数面：`repo:apps/emulator-lift/src/runtime/control_snapshot.h` 的 `present` 字段（`"visible" | "hidden"`，
  取自 `windowHidden()`，⛔ 不由 `--present` 反推）—— 控制层 `emu-ctl sample` 就能读到它。

## 要做 / 判据

- assets 档用例：`--present window --present-hidden` 与 `--present window` 两档各跑一帧，
  断言 `sample.present` 分别是 `hidden` / `visible`（读数形状：该字段值 + 帧号）。不需要看图。
- ⚠ **先裁决要不要做**：成本高（真安装 + 两档各一帧），防的是"隐藏窗口回归"这一类 ⇒ 由人定。
- 收口 = 要么挂上 assets 档 CTest，要么以 `done_reason` 写"裁决不做 + 理由"（⛔ 不许静默挂着）。

## 边界

⛔ 不动 `present` 的语义；⛔ 不把窗口相关的东西塞进 pure 档（§12.4：判据 = 注册位置）。
