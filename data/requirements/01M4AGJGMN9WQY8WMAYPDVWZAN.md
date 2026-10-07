# M4-1a · headless 前端 v0：宿主抽象（fs/副作用/时钟/配置）+ 环境初始化 + 多实例隔离与注入 + 跑到 LOGO.BIN 的 play-movie

- id: REQ-01M4AGJGMN9WQY8WMAYPDVWZAN
- type: req
- status: done
- parent: REQ-01M47SGXYWPQ1KWET2J49TJQST
- verify: tools/test/emulator-headless-logo.assets.test.mjs#★ 从归档里取 LOGO.BIN 并跑到

## 判据（已达成）

- `apps/emulator/src/host/**`（**零 Node**，由 tsconfig 的 `types: []` 机械强制）：分层文件系统 + 「可写区与只读源同身份即拒绝构造」+ 名字归一化（拒 `..` / 绝对路径 / 盘符 / 控制字符）+ 读不到记 demand + 副作用三态 + 虚拟时钟 + 配置防丢键棘轮 + 环境解析（缺根即抛）+ Instance/Registry 隔离。
- `apps/emulator/src/vm/**`：script 装载 + operand（指针族 **响亮失败**）+ machine（主循环 + `0x400` 等待门）+ ops（LOGO 路径上的 17 条）。
- `apps/emulator/frontends/headless/**`：Node fs 适配 + ALF 归档来源 + 唯一路径解析点 + CLI（`pnpm emulator`）。
- 实测：停在 `0x20F play-movie`（第 46 条）；步数 47 / 帧数 314 / 虚拟时刻 5024ms / 副作用 31 条；`gate.open.waitedMs = 5024`（**等待门真的等了**）。
- 守卫：`tools/test/emulator-host.test.mjs`（19 例，`@env pure`）+ `tools/test/emulator-headless-logo.assets.test.mjs`（2 例，`@env assets`）+ 分区守卫扩到 `Machine`/`ScriptFrame` + 3 条变异条目（`pnpm test:mutation` 13/13 红）。

## 本单顺手订正的两处本仓说法（都被取证推翻）

1. `numeric-ops.ts` 的「纯数值族**不碰**任何引擎字段」按字面为假：这一族**每一条**都读 `Engine+0x5D880` 并写 `Engine+0x5D8F4+120·cur = 1+2·argc`（**分派器协议**，不写就把下一条指令的位置算错）。判据措辞已加 carve-out；`TOUCHES_ENGINE_STATE` 的例外集合实测正确。顺带得到 `argc` 的独立判据：**长度字 = 2·argc+1**（39/39）。
2. 两条指令的操作数语义**不许命名**：`0x203 set-draw-color-alpha` 的 op2 与 `0x322 set-vertex-color` 的 op2 都落在**读取点没拿到**的字段上 ⇒ 模型只**原样存下**（字段名 `param`，日志带 `paramSemantics: 'unverified'`），⛔ 不叫 "blend mode"、⛔ 不当 "顶点下标"。

## 下一步（不在本单内）

六份取证包（启动链 / 操作数原语 / 控制与等待门 / 纹理族 / 颜色与网格族 / 数值族复核）的结论与 EA 锚应当按 `AGENTS.md` §6 **登记进知识台账** `data/ledger/`（每条绑 EA 或守卫用例 id）。本单只把它们用进了实现与守卫。
