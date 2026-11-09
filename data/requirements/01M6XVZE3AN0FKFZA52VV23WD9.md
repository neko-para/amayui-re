# emulator-lift · 分析工具与测量可信度：BMP 像素比对（tools/lift.mjs diff）/ profiler 膨胀口径 / 「动作没生效」的通用检测

- id: REQ-01M6XVZE3AN0FKFZA52VV23WD9
- type: req
- status: done
- parent: REQ-01M6XVYA68DQWWMQPY795WPN0Q
- order: 5
- verify: apps/emulator-lift/tests/input_expect.cpp#expect-change-not-changed
- done_reason: 判据 2 的『断言膨胀量级』按 AGENTS.md §10 改为口径不变量（开/关 profiler 的 insns 相等）+ 文档读数；三件（profiler 口径 / 时间线静默失败检测 / 变异条目）已落地，两条变异实测红。
- tags: [emulator-lift, tooling, measurement]

## 这个单收什么

不是"再写一个脚本"，而是把**测量本身**变成可复核的基建。四件事各有归宿：

1. **两张 BMP 的像素比对** —— 已立案 `REQ-01M6WSANRPND80XW52QY4Y5WA8`，**本单收养**（parent 已改到本单）；
2. **profiler 的膨胀口径** —— 开 profiler 的读数不能直接当基线（本仓实测膨胀量级 33–40%，同一份工程里口径要固定成一条能查的约定）；
3. **"动作发出去但没生效"这类静默失败的通用检测** —— 点击没被 guest 采到就是这么漏的（子单 G 是它的一个具体实例）；
4. **变异守卫缺口** —— 已立案 `REQ-01M6WSAGESXYYZ6QR919J2NFWC`，**本单收养**（`pnpm test:mutation` 缺 input-injection 条目）。

## 判据

- `pnpm tools lift diff a.bmp b.bmp` 可用，退出码语义固定（0 = 相同 / 1 = 有差 / 2 = 输入不可读或不一致），有 pure 守卫、不依赖游戏安装（细判据见被收养单）。
- profiler 口径**写在能查的地方**（跑测脚本注释 + 一条断言"膨胀量级"的守卫），⛔ 不把"开 profiler 的数字"与"关 profiler 的数字"混进同一张对照表。
- 静默失败检测：至少一条走真安装的路径断言"动作后必须观测到变化"；与子单 A 的"动作后必须变化"共用判据，但这里管的是**旧时间线脚本**也得被抓住。
- 变异守卫：`pnpm test:mutation` 里有 input-injection 条目，且短路注入 ⇒ 该守卫**当场红**（不是恒绿）。

## 不做

- ⛔ 不为了"工具好看"把 `.tmp/` 的手写脚本搬成仓库里的半成品：要么做成有退出码、能当门禁的基建，要么不搬。
- ⛔ 不让工具依赖游戏安装（能造夹具就造夹具）。
- ⛔ 不把工具做成"顺带跑一遍游戏"的集成测试（那属于 C / A）。

## 收口（2026-11-09，macOS arm64 / RelWithDebInfo）

- **判据 2 的口径变更（裁决）**：需求单原文要"一条断言**膨胀量级**的守卫"，与 `AGENTS.md` §10（⛔ 用 test 断言当前数值）冲突
  ⇒ 由**口径不变量**替代：开/关 profiler 的**指令数相等**（`tests/profile_window.cpp#profile-instrumentation-invariant`，四臂各一台新 Machine），
  膨胀读数只进文档（`docs/03-emulator/measurement.md` M-02 已重写：三开关 / 口径标签 / N=5 中位数 / ⛔ 开/关混表 / 旧 +33~40% 标成不可复现的历史读数）。
- profiler：运行摘要新增 `instr : profile=… frame-trace=… window-prof=… expect=…`，`window-prof` 读 `WindowProfiler::armed()` **真值**；
  M-02 更正了一处归因（每块 `steady_clock` 是基线也付的，不是 `--profile` 的边际）。
- 静默失败（旧时间线脚本）：`--expect-change-after click[,key,mouse]` + `--expect-change-window <samples>`；
  `not-changed` / `no-sample` / `unclosed` 三种都响亮失败（复用 `InteractionFailed` + `detail` 写种类 + exit 5 + `<脚本>.expect-fail.bmp`）。
  ⛔ 不改旧 `--input-script` 语法、⛔ 不加枚举成员。
- 变异：`ctest:profile-window` / `ctest:input-expect` 两条（各 1/1 红，实测）；`tools/mutate-check.mjs` 的 `DOMAIN.data` 过期自描述已修。
- 未做（在本单边界外）：真安装 N=5 A/B 与 `scripts/profile/` 脚本对（M-02 给了命令与落点，但本机无安装、`apps/emulator-lift/scripts/**` 不在本单）；
  时间线 expect **缺省开**（父级裁：不做，默认关）；`--tolerance` 保留真名（不加 `--allow-max-diff` 别名）。
