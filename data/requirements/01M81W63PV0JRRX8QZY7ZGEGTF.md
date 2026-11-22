# AOT · S7 的 esp 模型按 Intel 三条一起补（push ebp / pop ebp / leave）

- id: REQ-01M81W63PV0JRRX8QZY7ZGEGTF
- type: req
- status: open
- parent: REQ-01M7JHSEZVMYB4XHEN32V7V6HJ
- tags: [emulator-lift, aot, s7]

# AOT · S7 的 esp 模型按 Intel 三条一起补（push ebp / pop ebp / leave）

## 一句话

`apps/emulator-lift/src/aot/lift_struct.cpp` 的 `transferEsp` 有**三处** esp 偏差：`push ebp` 没做
`esp-=4`、`pop ebp` 没做 `esp+=4`、`leave` 没做 `pop ebp` 的 `+4`（后两者被"目的操作数是 EBP"的分支
截走，只把 `ebp` 置 `kUnknown`）。它们在同一段标准帧里**成对抵消**，所以判据"看起来对"。
按 Intel 三条一起补，实测**发射物逐字节等价**（8 份 `.ll` 与 base 只差 header 两处计数器，
总字节相同）、宿主 `call` 站点 1,572 不变、可结构化 5,343→5,335。⇒ 这是**把证明做对**的中性改动，
不是提速。

## 判据

1. 三条一起补后：宿主 `call` 站点 = **1,572**（不变）；8 份 `lift_aot.part*.ll` 与 base **逐字节只差
   header 两处计数器**（每份 4 字节，总字节 132,744,647 相同）。
2. `ctest --test-dir out` 全量绿；`lift-s7-structured` 必绿（它守"宿主 call/ret 与 guest 栈一致"）。
3. ⛔ **只补 `leave` 的 +4 要拒绝**：实测 63 个正确单元失格、宿主 `call` 1,572→1,498、IR 少 15,170 B。

## 未做（另立，不随本单收）

- **"单元根必须是函数入口"**：会把 11 个合法的共享尾声块（`… pop ebp; ret [imm16]`）判死，还会牵动
  一大批 soft root ⇒ 必须单独立项 + 单独读数，⛔ 不要和本单一起收。
- 其余四类降级（动态返回 / 可变长栈 / 写返回槽 / 表外 callee）的进一步放宽。

## 读数与复现

读数（生成期、同一闭包、主树 `a6674dc`）：

| 模型 | 自身通过 | 可结构化 | 宿主 `call` 站点 | 8 份 `.ll` 总字节 |
|---|---:|---:|---:|---:|
| 现状 | 8,600 | 5,343 | 1,572 | 132,744,647 |
| 只补 `leave` +4 | 8,393 | 5,280 | 1,498 | 132,729,477 |
| 三条一起补 | 8,589 | 5,335 | 1,572 | 132,744,647 |

取证与结论见 `repo:docs/03-emulator/aot-s7-evaluation.md` §6；复现见该文档 §7 ⑤。
