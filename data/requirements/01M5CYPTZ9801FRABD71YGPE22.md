# spike：0x3 call-script / 0x2 exit / 0x9 exit-script 的控制流语义（取证 + 设计）

- id: REQ-01M5CYPTZ9801FRABD71YGPE22
- type: spike
- status: dropped
- parent: REQ-01M5CYFJY8P1PQ0AANGE87K1VJ
- dropped_reason: spike 的问题已由 spec 第三轮回答：0x3 / 0x2 / 0x9 的逐字行为已写进 data/spec/chapters/60-ops.md 的 CORE-OPS-FRAME-* 规则（EA 锚在 bindings.json），本设问不再需要独立跟踪。
- tags: [emulator, boot, control-flow]

## 一句话

参考实现已跑到 `#71` 的第一条 `0x3 call-script 21080`（LOADCONFIG）。**这条不能靠猜**：它要求"多脚本"（换 body、帧号 +1、返回位置写父帧），而 `0x2 exit` / `0x9 exit-script` 到底是"整机结束"还是"子脚本返回父脚本"**没有观察**。本单是**限时设问**：取证 + 设计，产出是结论而不是代码。

## 要回答

1. `0x3` 的完整语义：操作数口径（统一文件 id？）、装载失败表现、返回位置写父帧哪一格（dword 序号/字节、是否 +3）、局部池是否隔离。
2. 子脚本结束谁还控制权：`0x2 exit` / `0x9 exit-script` 的逐字与**分叉判据**。
3. `Machine` 需要新增什么状态（脚本栈 / 父帧返回槽 / 帧号 / 池隔离），三件（`0x3`/`0x2`/`0x9`）各自实现要点，**哪些点必须留残差**。
4. 2–3 条会红的守卫草案。

## 现成证据（不重复验证）

- `0x3` handler `sub_41C6A0`：`0x41C736` 存旧帧号、`0x41C73D` 帧号 +1、`0x41C724` 返回位置写父帧 `+0x5D8F0`（不 +3）、`0x41C746` → `sub_40ED40` 装载（GlobalAlloc / ReadFile / `SYS4` 头校验）。
- `0x5 ret` 的跳转路径还会从引擎级栈 `+0x68EB0/+0x68EB4` 弹值写 `+0xAAB44`（用途未定）。
- 根脚本 `#67/#68/#70/#71` = `0x3`（21080/20931/20956/20936）；`#114` = `0x3 21089`；`#134` = `0x3 21090 = LOGO.BIN`。
- `LOADCONFIG.BIN` 末尾有一条 `0x2 exit`。

## 判据（本单 done 的条件）

- 每个被问的点要么有 EA 级逐字证据 + 结论，要么明确写"没拿到 + 缺哪个观察"；
- 第 3 问给出**可实现的模型**（状态字段清单 + 三条指令的实现要点），并列出必须留残差的点；
- 结论进 `data/ledger/`（accepted，带 EA 锚），设计要点进本单正文或 `docs/`。

## 不做

- ⛔ 不为了推进棘轮把 `0x3` 当 no-op（那会跳过被调脚本的状态，属于静默分歧）。
- ⛔ 不在没有观察的情况下写"`0x2 exit` 一定返回父脚本"这类断言。
