# emulator-lift · 脚本身份 marker 事件源：TriggerKind::Marker（VM 循环头采样 cur + frame[cur].+0x50 → id→名字 → 边沿触发 → 超时响亮失败）

- id: REQ-01M6XVZ4ANQRC215ACG90A35NR
- type: req
- status: done
- parent: REQ-01M6XVYA68DQWWMQPY795WPN0Q
- order: 2
- verify: apps/emulator-lift/tests/input_marker.cpp#marker-value-change-one-event
- tags: [emulator-lift, interaction, marker]

# emulator-lift · 脚本身份 marker 事件源：TriggerKind::Marker（VM 循环头采样 cur + frame[cur].+0x50 → id→名字 → 边沿触发 → 超时响亮失败）

- id: REQ-01M6XVZ4ANQRC215ACG90A35NR
- type: req
- status: open
- parent: REQ-01M6XVYA68DQWWMQPY795WPN0Q
- order: 2
- tags: [emulator-lift, interaction, marker]

## 缺的是什么

`--anchor-script`（子节点 `REQ-01M6WS9G1EQRZ0EYM21V0DPZVY`）要求"以进入某引擎脚本为 0 点"，但**现在没有任何"引擎脚本可观察事件"的接口**。本单只做那个**事件源**。

取证与设计在 `.tmp/anchor-design.md`（**不入库**；不可用时按下表判据做）。三条已取证、不再重证：

| 事实 | 出处 |
|---|---|
| 身份格不是单一固定地址：写点是 `帧基址 + 120×cur + 0x50`（40 槽），`cur` 是另一个固定格 | 设计 §0 / §1 |
| 采样点选 **VM 循环头**（每迭代一次、装载器不在其中），读 `cur` 与 `frame[cur].+0x50` 两个 dword，值变即发事件 | 设计 §0.2 |
| 值**可解析成名字**（引擎自己就这么干过：拿该值与 `Engine[0xCA6C]` 比，不等就抛 `Depth が不正です %s != %s`） | 设计 §1 |

## 本单的合同

1. `TriggerKind::Marker`：与既有 `TriggerKind`（`repo:apps/emulator-lift/src/runtime/input_script.h`）并列，触发条件 = **身份值变化（边沿）**，不是电平。
2. 载荷 = **名字**（`id → CStr(read32(索引+0x414) + 80×id)`；越界 / 负数 ⇒ `"ERROR"`）。
3. 边沿 = `(cur, id)` 的**值变化**；同一 `(cur, id)` 重复出现**不重复发**。
4. 超时：等到指定名字而超时 ⇒ **响亮失败**（非 0 退出 + 说出"等的是哪个名字、等到的是什么"）。⛔ 不许静默继续。
5. 名字表读不到（指针为 0 / 越界）⇒ 降级成 `id` 仍可触发，但**打一条警告**（否则"名字对不上"会变成整条链静默失明）。

## 判据

- **不装游戏、纯 CTest** 就能钉住：用假 guest 内存布置 `cur` / `frame[cur].+0x50` / 名字表，断言"值变 ⇒ 一次事件、值不变 ⇒ 零事件、来回变 ⇒ 两次事件"。
- id→名字：正常 id / 越界 id / 负数 id / 名字表指针为 0，各一条用例（后两者必须是 `"ERROR"` 或响亮降级，不是崩溃）。
- 超时用例：等一个不会出现的名字 ⇒ 断言**非 0 退出**并带名字与等待时长。
- 消费者单能只靠这个接口写"以脚本 X 为 0 点"的输入脚本；⛔ 本单不实现 `--anchor-script` 本身。

## 与 A 为什么分开

B 的证据链**全部在 guest 内存与 VM 循环**里 ⇒ 纯 CTest 可钉；A 的消费者必须**真安装 + 真画面**。放进同一张单，B 就会因为"要跑游戏"而永远没有绿灯判据。

## 探测补记（已被收口取代）

三项缺口（VM 循环头 `0x4123F0` / 基址 `0x55E1BC` / 名字表 `FileDB`）已在收口前补齐，证据链见分析报告。
`.tmp/anchor-design.md` 早已丢失，不再作为依据。

## 收口（2026-11-09）

- 已实现 `TriggerKind::Marker` + `at marker <名字>`：边沿 = `(cur,id)` 值变（首采样只播种）；名字读不到 ⇒ 降级 `id:0x…` **仍触发** + 每类一次警告；
  超时 ⇒ `InteractionFailed` + `detail = marker-timeout: waited/last/waited_ms` + exit 5（复用，未加枚举）。
- ★ 订正分析 §1.4：采样点 = 块入口 `eip ∈ {0x4123F0, 0x4123E5}`（块缓存按 `Jcc` 切块，只挂前者会漏消息泵回来的 pass；用例 `marker-pump-return-entry` 钉住）；
  超时要 `postQuit` 唤醒阻塞在 `GetMessageA` 的 guest。
- 守卫：`apps/emulator-lift/tests/input_marker.cpp`（16 例 / 631 checks），`verify` 挂 `#marker-value-change-one-event`；
  变异 3 条（边沿恒触发 / 降级不触发 / 超时短路）⇒ 3/3 红。
- ⛔ 未实现 `--anchor-script`（消费者 `REQ-01M6WS9G1EQRZ0EYM21V0DPZVY` 仍未收口）。
- 已知差异（不改 spec）：`Engine+0xCA6C` 只有 1 处读、**0 个写者**，而 `60-ops-frame.md` 说 `0x199` 写；本实现不用该格。
  spec 线自 2026-11 冻结 ⇒ 记此处，不单独开单。
- 分析全文 `.tmp/reveq/01M6XVZ4ANQRC215ACG90A35NR/analysis.md`（**不入库**；入库的是本节要点）。
