# emulator-lift · 交互图 wait 增加 wall_ms 单位（宿主墙钟纯等待）

- id: REQ-01M75A5HGMN4M70WDNHKW0G0TN
- type: req
- status: done
- parent: REQ-01M6H6G6DSRG1ZRD810Z0HTH8V
- verify: apps/emulator-lift/tests/interaction_graph.cpp#interaction-graph
- tags: [emulator-lift, interaction-graph]

## 一句话

交互图的 `wait` 增加 **`wall_ms`** 单位（宿主墙钟纯等待）：`wait 500 wall_ms` = 等真实的 500 ms，不碰输入、完成前阻塞下一个 step。

## 为什么

`wait` 原来只有 `frames`（Present 计数）与 `ms`（guest 虚拟毫秒）——两者都是**引擎时间**。等"宿主侧状态 / 外部世界"（例如窗口真正刷出来、另一个进程就绪）时没有写法。时间线侧早就有 `wall_ms` 单位（`at wall_ms`），图侧缺。

## 判据（实现即契约）

1. 单位闭集合 = `frames` | `ms` | `wall_ms`（⛔ 不跟着时间线放宽 `wall` / `host_ms` 别名）。
2. 语义 = `tick(...)` 传入的**宿主墙钟**（自 run 起算）；纯等待、不碰输入；完成前阻塞下一个 step。
3. `timeout` 仍管这一步（wall wait 不绕过它）⇒ 等不到按既有 `timeout` / `no-sample` 规则响亮失败，⛔ 不加无限等待。
4. 图里**出现过** `wall_ms` ⇒ trace 首行标 `# NON-DIFFABLE`（墙钟等待让同一输入两次跑的完成时刻不同）。解析期置 `usesWallClock()`，`main.cpp` 的 `setTrace(..., !window && !usesWallClock())`。
5. trace / fail 报告字段口径不变。

## 守卫

- pure：解析（`wall_ms` 收、非法单位报错、线性写法可写）；行为（假 `tick`：`wall=0..499` 不完成、`wall=500` 完成并进下一步）；timeout 子用例（`timeout 100` + `wait 1000 wall_ms` ⇒ Timeout）；trace（含 `wall_ms` ⇒ `# NON-DIFFABLE`）。
- 会红：把 `State::Acting` 的 WallMs 分支误写成 `guest_ms` ⇒ 行为用例红。

## 落点

`runtime/interaction_graph.{h,cpp}`（解析 / due / `Acting` 三分支 / `usesWallClock`）、`src/main.cpp`（setTrace 一处）、`tests/interaction_graph.cpp`、`docs/03-emulator/interaction-model.md`（§1 动作表三单位各一句 + §4 确定性一条）。

## 未做

- 时间线 `--input-script` 的 `wait` 是**相对**语义（要有同单位基准，`at wall_ms` 先写）；本单不改它 —— 时间线侧 `at wall_ms 0` + `wait N wall_ms` 已可用。
- 不做"等外部进程/文件"这类谓词条件（那是另一个条件源，不是时间单位）。
