# AOT · S7 判据 4（间接 call）的可证放宽：已合并（形状级差分先红后绿）

- id: REQ-01M81PTTC7XBK365YGQNQTTXWA
- type: req
- status: done
- parent: REQ-01M7JHSEZVMYB4XHEN32V7V6HJ
- verify: apps/emulator-lift/tests/s7_indirect.cpp#s7-indirect
- tags: [emulator-lift, aot, s7]

# AOT · S7 判据 4（间接 call）的放宽：已收（形状级差分先红后绿）

## 一句话

判据 4「有间接 `call` ⇒ 整单元判死」曾占降级根 **76%**（首因 1,682）。放宽 = 间接 `call` 之后把 `esp`
标成**未知**（⛔ 不假设 callee 的 `ret imm16` 清字节数），只有函数自己能用**可跟踪的** `mov esp,ebp` /
`leave` 把 `esp` 重建回入口值时才放行；配套 `espAdd()`（`kUnknown + d` 仍是未知，避免假绿与有符号溢出）。
构建期开关 `LIFT_S7_RELAX_INDIRECT`（缺省 **ON**；`-D…=OFF` ⇒ 8 份 `.ll` 与放宽前**逐字节相同**）。

## 判据（接受条件，已满足）

1. **形状级差分 CTest `lift-s7-indirect`**（默认档）：夹具 = 单元内有间接 `call` + callee 用 `ret imm16`
   清字节 + 函数自己用 `mov esp,ebp` / `leave` 恢复帧；断言 ① 该单元被判 **structured**、
   ② 与解释器（`Machine`）**逐位相同**（`lift_dyn` 查找=1）。
   **先红后绿**：放宽前 ① 红 3 条（`structurable=1` / `ret in structured=1` / 有 `downgrade 00452FA0`），
   放宽后 **22/0 全绿**；`-DLIFT_S7_RELAX_INDIRECT=OFF` 时用例复红。
2. **D3（前置修正）**：间接 `call` 原来的 `%r = tail call i32 @lift_dyn(…)` 是**假尾调用**（`%r` 紧接着被
   `icmp` 用）⇒ 改回普通 `call`；`tail call @lift_dyn` 5,804→492，三份 exe `.text` sha256 全同（性能持平）。
3. 五项指纹（`exit` / `stop @` / arena / `table misses`）逐位同；`ctest` 140/140 · `pnpm test` 618 ·
   `pnpm verify` 466；变异 `--match s7-indirect` 实测红。

## 读数（2026-11-24 / win32 / dev `53d438c`；SAVE11，4 对交错 A/B）

| 读数 | 放宽前 | 放宽后 | Δ |
|---|---:|---:|---:|
| 可结构化单元（生成期） | 5,343 | **6,411** | +20.0% |
| 宿主 `call` 站点 | 1,572 | **2,967** | +88.7% |
| 间接 call 首因 | 1,682 | **703** | — |
| `iter`（8 s 窗口派发） | 56.35 M | **51.07 M** | −9.4% |
| `insns/iter` | 21.18 | **23.64** | +11.6% |
| 点击大帧 | 267.3 ms | **210.2 ms** | **−21.4%（4/4 同向）** |

## 未做 / 待核

- **没有解锁主循环/热泵**：`00412290`（主循环）与 `004BBAB0`（定时器泵）仍卡在 `004E8C21` / `004B60C0`
  —— 那两条间接 `call` 之后**没有可跟踪的帧恢复**，本次"可证"放宽结构上管不到。`004B5230` 在放宽前
  就已结构化（不是本单的功劳）。本单新解锁的是 `004E73EB`（614 个直接传染源）与 `004E8A6E`（180）。
- 其余四类降级（动态返回 / 可变长栈 / 写返回槽 / 表外 callee）的进一步放宽不随本单。
- "单元根必须是函数入口"另立（见 `REQ-01M81W63PV0JRRX8QZY7ZGEGTF` 的 dropped 理由）。

详见 `repo:docs/03-emulator/aot-s7-evaluation.md` §4.4–§5；派发密度那一层的三条修正见
`repo:docs/03-emulator/aot-dispatch-density.md`。
