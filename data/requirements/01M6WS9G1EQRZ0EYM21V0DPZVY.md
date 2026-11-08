# emulator-lift · --anchor-script（以进入某引擎脚本为 0 点）需要先有「引擎脚本可观察事件」接口；本轮只认 at frame

- id: REQ-01M6WS9G1EQRZ0EYM21V0DPZVY
- type: req
- status: open
- parent: REQ-01M6H6G6DSRG1ZRD810Z0HTH8V
- tags: [emulator-lift input script 取证]

## 问题

- `--input-script` 有两档锚：`at frame N`（Present 计数）与 `at guest_ms T`（guest 虚拟时钟）。
- `--present headless`（缺省）两档都确定性；`--present window` 是 **real-time 时钟** ⇒ 毫秒锚不可复现（`repo:apps/emulator-lift/README.md` §4.2c 已写明“那一档用帧”）。
- 于是“等游戏进到 OPTION 再点”只能**手工标定帧号**：先跑一遍看日志找帧，再把 `at frame` 写成那个数。换机器 / 换呈现后端 / 换 CLI 组合，帧号就变。

## 想要的能力

`--anchor-script <id>`：时间线的 0 点 = **首次进入引擎脚本 `<id>`** 的那一刻，而不是 Present 计数。

## 为什么它得先有「引擎脚本可观察事件」接口

lift 执行的是**原机器码**：“进入脚本”是 guest 内部的帧 / 脚本切换，lift 侧没有这个语义事件。现有可用的只有 `repo:apps/emulator-lift/src/runtime/watch.h`（观察一个 guest DWORD 何时被改写；env 门控、调试用）⇒ 它证明“观察 guest 格”这条路存在，但**没有**“进入脚本 N”这个事件，也没有可挂的回调。

两条路都要先取证：① 在引擎的脚本切换点挂钩（先定位并锚 EA）；② 把某个“当前脚本 id 格”变成受监视观察点（先取证那一格是什么）。两者都是**新的可观察接口**，不是给 anchor 加个 CLI 参数。

## 父级裁决（2026-11-08）：本轮**只认 `at frame`**

anchor 是新能力、单独做；⛔ 不要在 `input_script.cpp` 里先做半截（用毫秒近似、或按日志行号猜）。

## 判据（若开做）

- 同一份脚本 + 同一个 anchor id，在 `--present window`（real-time）与 `--present headless` 两档下**触发在同一个 guest 脚本位置**（可观察：脚本 id / 首帧 draw 签名 + 消息序一致）。
- 没进入目标脚本时**响亮失败**（退出码非 0 + 说清“没进入脚本 N”），⛔ 不静默按 0 点跑。
- anchor 只影响时间线**原点**，不改注入通道（仍是 `runtime/input.h` 那一份状态源）。

## 现在绕到了哪里

窗口模式一律 `at frame` + 手工标定帧号；`at guest_ms` 只在 headless 有意义，脚本跨档复用要改时间锚。

`apps/emulator-cxx` 已有同名 `--anchor-script`（`repo:apps/emulator-cxx/src/app.h`）可作**语义参照**，但它是宿主建模的脚本 id，不是 lift 这种“观察 guest”的实现，⛔ 不要照搬。
