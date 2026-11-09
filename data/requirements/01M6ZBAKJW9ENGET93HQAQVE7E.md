# emulator-lift · M1 收口暴露的技术债（conformance 恒等判据 / movsd 碰撞 / SSE 对齐守卫 / MXCSR 覆盖）

- id: REQ-01M6ZBAKJW9ENGET93HQAQVE7E
- type: req
- status: done
- parent: REQ-01M3TC7BK0J9TBQKGGWQ35A3NR
- verify: apps/emulator-lift/CMakeLists.txt#conformance-rejects-hollow-case
- tags: [emulator-lift, debt]

## 一句话

本轮收口 M1 时暴露的**不属于**那两条单（locale / SSE）的技术债。四条**全部落地**：①②③ 修好并各配守卫，
④ 只记账并登记重开条件。

## 四条现状（2026-11-09）

| # | 债 | 现状 | 守卫 / 落点 |
|---|---|---|---|
| ① | conformance 的"恒等夹具"判据几乎恒假 | **修好** | 判别式换成 `sameStateIgnoringSentinelRet`（除 `esp` 外逐位相等，且 `esp` 恰好 +4）；负控 CTest `conformance-rejects-hollow-case`（`LIFT_CONFORMANCE_SELFCHECK=1`：空心夹具必须**只**因这条判据红） |
| ② | `movsd` 助记符碰撞（`F2 0F 10` vs `A5`） | **修好** | `SseOp::Movsd` + 解码按**操作数形态**分派（`A5` 仍是 `Op::Movs`）；用例 `sse-movsd-reg-keep-high` / `sse-movsd-load-zero-high` / `sse-movsd-store-8bytes` |
| ③ | SSE 对齐违例没有守卫 | **修好** | `tests/dispatch.cpp`：`unaligned-movaps-is-loud` / `unaligned-movdqa-store-is-loud`（实现本来就在，缺的只是守卫） |
| ④ | MXCSR 只建模 IE | **只记账** | `repo:docs/03-emulator/design-notes.md` 的 `L3-sse-01`（读数 + 重写必须保持不变 + 三条重开条件） |

## 实测口径（2026-11-09，macOS arm64 / RelWithDebInfo）

- 空心夹具（setup 只有一条 `C3`）跑完**只有 `esp` 变**（`6FFFFFFC → 70000000`），`memHash` / `xmmHash` / `eflags` /
  `mxcsr` 全不变 ⇒ 原文"哨兵 `ret` 必然改 `memHash`"是错的（已改）。修前把 `xmmHash` 移出比较：236 条用例 **0 红**（判据空转）。
- 顺带修正两条"只验缺省值"的夹具（`leave` 预置 `ebp=0xDEADBEEF`、`x87-finit` 预置控制字 `0x027F`）—— 它们被新判据
  判红的**原因**正是"什么都没验"，所以改夹具而不是加豁免。
- 变异自检：三条守卫各在 `repo:tools/mutate-check.mjs` 加了条目，逐条 `--match` 实测红后还原
  （`conformance-rejects-hollow-case` / `sse-movsd-*` / `unaligned-*`）。

## ④ 的重开条件（逐字见 `L3-sse-01`）

① 语料或 `src` 出现 `ldmxcsr`/`stmxcsr`/`fxsave`/`fxrstor`（机械触发器：assets 档 `corpus-mnemonics` 会先红）；
② 要投递 `#XM` 或实现掩码 / RC；③ feeder 可达任何 XMM 指令 —— 那时 `AudioEngine::Context` 必须**一起**加 `mxcsr`，
否则 pump 路径会**静默**泄漏 IE。

## 不做什么

- ⛔ 不为 ②③ 扩语料 / 改 `.lst`；语料没有就按重开条件记。
- ⛔ 不把 ① 修成"跳过恒等判据"（那等于删守卫）；⛔ 不给空心夹具开条目级豁免。
- ⛔ ④ 现在就"补建模"：没有消费者 ⇒ 那是猜测性实现（`AGENTS.md` §10）。
