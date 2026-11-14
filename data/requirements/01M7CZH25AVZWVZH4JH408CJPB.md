# M-lift-1 · 第一条可执行线：entry → TITLE（第一轮不注入输入；判据 = handoffs==0 + 与解释器臂同虚拟时钟逐位一致）

- id: REQ-01M7CZH25AVZWVZH4JH408CJPB
- type: req
- status: open
- parent: REQ-01M7CD1NRK9P5WKR5PJ6GW3067
- tags: [emulator-lift, lift, milestone]

## 范围（第一轮，**不注入任何输入**）

从 PE `entry` 起跑，把 boot 链（SYSTEM4 → … → LOGO → INIT → TITLE）整段走完。
⛔ 不做输入注入（`--graph` / 动作脚本一律不进第一轮）：注入本来是为了后续复杂路径，
而 boot→TITLE 是一条**无输入**路径 ⇒ 第一轮不需要它，也就不需要为它裁"输入 tick 的等价粒度"。

## 判据（机械化；"到 TITLE"本身不是断言）

1. **`handoffs == 0`**：整段跑完，trampoline 一次都没把 EA 交给解释器（`--lifted-only` 下也不失败）。
2. **与解释器臂逐位一致**：两臂都 `--clock=virtual`，从 `entry` 跑到**同一 present 计数 K**，
   比 guest 状态（8 GPR + EFLAGS + 关键全局/堆区哈希）+ **最后一帧的帧缓冲哈希**。
3. **没有硬停**：无 `_Exit(3/4/9)`、无 `llvm.trap`、无预算超限。
4. "抵达 TITLE" = 一次**人工确认** + 一条 `observations.md` 记录（带命令与日期），⛔ 不进测试
   （业务正确性不作判据，`AGENTS.md` §5）。

## 为达成它必须有的件

generator（形态 → IR）· `EA → 代码` 派发表 · PE 映射 / TLS / 导入绑定 · 指令计数（`--clock=virtual` 用）·
boot 路径用到的 shim（文件 / 注册表 / GDI 字体 / D3D9 表面）· 少量 x87 / SSE / string 指令 ·
**present 计数当停机判据**（⛔ 不用 marker：marker 若做成"该块不 lift"会与判据 1 自相矛盾）。

## 未做（后续）

输入注入（见输入重做单）· 音频 / 影片的真实时序 · FELLOW / ADV / SAVE 等复杂路径 · SMC ·
arm64 实跑 · 整段性能验收。
