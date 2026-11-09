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

## 探测补记（2026-11-09：仓内哪些锚在、哪一段真的缺）

判据不变，但下面这份"证据分布"要先看，免得照着上面那张表直接开工：

| 要的东西 | 仓内状态 |
|---|---|
| 当前帧号 `cur` 的偏移（`Engine+0x5D880`） | ✅ 在仓：`repo:packages/age-format/src/engine/layout.mts`（`FRAME_ENGINE_SCALARS.cur`） |
| 帧记录基址 `0x5D894` / 步长 `0x78` / 40 槽 | ✅ 在仓：同文件 `FRAME_LAYOUT` |
| 身份格 = 记录 `+0x3c`（旧记法 `+0x50` = `0x5D8D0`） | ✅ 在仓：`layout.mts` 的 `frameArg` + `repo:data/ledger/observation/2026-10.jsonl` 的观察行 |
| 身份 = "统一文件 id"（高 8 位区号 / 低 24 位下标） | ✅ 在仓：`repo:data/spec/chapters/60-ops-input.md` 与 `60-ops-frame.md` |
| **`cur` / `frame[cur]` 所在的基址对象** | ⚠ 只在本单上面那张表里（"VM 循环头 / `Engine` 基址"）⇒ **`.tmp/anchor-design.md` 已丢失且不入库**；要重新取证 |
| **id→名字表基址**（`read32(索引+0x414) + 80×id`） | ⚠ 同上，只有公式没有基址 |
| **VM 循环头的 EA** | ⛔ 仓内零处（`rg` 无命中） |

⇒ 能立刻做的：**边沿检测 / 名字降级 / 超时响亮失败**这三件纯 CTest 可钉的（判据见上，语料不需要）。
做不了的：把 `when script <名字>` 接到**真画面** —— 缺的是"基址对象"与"循环头 EA"两次取证，不是实现。

★ 顺带一条可用推断（**未核**，别当事实）：`layout.mts` 记的帧区之后那个成员 `0x5EB54`，与 `--skip-logo`
用的 `T_Command + 0x5EB5C` 只差 8 字节且同属一个对象 ⇒ `T_Command` 很可能就是 `layout.mts` 里的那个 `Engine` 基址。
若成立，`cur` = `mem.rd32(base+0x5D880)`、身份 = `mem.rd32(base+0x5D894 + 0x78*cur + 0x3c)`，`base = mem.rd32(0x55E1BC)`。
核它的判据：LOGO 帧的身份应等于 `0x5262`（`data/ledger/observation/2026-10.jsonl` 里那条 SYS4INI 观察），
或 `cur ∈ [0,39]` 且记录 `+0x00` 指向已提交内存。
