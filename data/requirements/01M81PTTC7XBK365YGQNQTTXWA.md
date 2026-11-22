# AOT · S7 放宽（间接 call 后 esp 未知）未合入：补形状级差分用例再评估 · leave 的 esp 模型待核

- id: REQ-01M81PTTC7XBK365YGQNQTTXWA
- type: req
- status: open
- parent: REQ-01M7JHSEZVMYB4XHEN32V7V6HJ
- tags: [emulator-lift, aot, s7]

# AOT · S7 放宽（间接 call 后 esp 未知）未合入：补形状级差分用例再评估

- id: REQ-01M81PTTC7XBK365YGQNQTTXWA
- type: req
- status: open
- parent: REQ-01M7JHSEZVMYB4XHEN32V7V6HJ
- tags: [emulator-lift, aot, s7]

## 一句话

S7 判据 4（"有间接 `call` ⇒ 整单元不可结构化"）的一条**可证放宽**已在 dev 车道实现并实测
（间接 `call` 之后把 `esp` 标未知，不假设 callee 的 `ret imm16` 清字节数；只有函数自己能可证地
把 `esp` 重建回入口值才放行）。读数站得住，**但本批不收**：收益 ≈ 0.03 ms/切换帧，而它改的是
**发射形状**的判据面。本节点 = 补一条**形状级差分用例**（接受条件）。

★ 这是 `REQ-01M7FC3W0ZMA35PK3WV73YXB1Q`（M-lift-2）的**兄弟**节点，不是子节点：那单的判据 2 已裁
"剩余覆盖率不再投入"，本节点收的是"放宽 + 它缺的那条判据"。

## 判据（接受条件，不是愿望）

1. 新增一条 CTest / 差分用例，形状至少覆盖「单元内有间接 `call` **+** callee 用 `ret imm16` 清字节
   **+** 函数自己用 `mov esp,ebp` / `leave` 恢复帧」，要求与解释器（`Machine`，即 `tests/*` 用的语义
   oracle）臂**逐位相同**。夹具形态可在 `repo:apps/emulator-lift/tests/s7_structured.cpp` 上扩
   （现在只跑 `repo:apps/emulator-lift/tests/ir/s7_roots.txt` 的固定一对 F/G）。
2. 有了 1 之后再评估放宽本身（实现在 dev 车道的 `.tmp/lift-opt/s7-relax.patch`，gitignored、未入库；
   主树是**未放宽**形态）。
3. `leave` / `push ebp` / `pop ebp` 的 esp 模型**已查清**（2026-11-22）：**没有**错编译；修法是
   "三条一起补"（IR 等价），已另立 `REQ-01M81W63PV0JRRX8QZY7ZGEGTF`，⛔ 不随本单收。

## 为什么这一批不并（裁决，2026-11-22）

- 收益 ≈ **0.03 ms/切换帧**（本负载 −0.31% 派发）对上"改**发射形状**判据"的审查成本，不成比例。
- **论证有一处对不上**：判据 4 的理由是"caller 的宿主帧在那次尾调用里就没了"
  （`repo:apps/emulator-lift/src/aot/lift_struct.h` 里第 4 条判据与传递闭包那两段都写这个理由）。
  但当前发射器发的是
  `%r = tail call i32 @lift_dyn(...)` 紧跟 `emitCallReturnCheck(%r, resume)` —— **`%r` 在被用**
  ⇒ 该点**不是**真尾调用 ⇒ **D53 的前提在现状下需要重新证明**。实测指纹相同 ≠ 理由成立。
- **缺形状级差分**：`lift-s7-structured` 只跑 `s7_roots.txt` 的固定根
  （`repo:apps/emulator-lift/CMakeLists.txt` 里 `LIFT_S7_CALLER_EA` / `LIFT_S7_CALLEE_EA` 是硬编码的一对），
  覆盖不到新解锁的 **1,395** 个宿主 `call` 站点。
- 实测**没有**解锁 FELLOW 主循环 `00412290`：它仍因 `004E8C21` 传染降级，而后者自身仍是间接 `call`
  （站 `004E8C57`）、之后没有可跟踪的帧恢复 ⇒ 估的 85–105 ms 没拿到；`004BBAB0` 同理（卡 `004B60C0`）。

## 未做 / 待核

- **未做**：判据 1 的那条形状级差分用例（本节点的交付）。
- **另立**：`REQ-01M81W63PV0JRRX8QZY7ZGEGTF` —— esp 模型三条一起补（IR 等价）。
- **已核（2026-11-22）**：`leave` 那件事**不是缺陷**。63 个"靠偏差才通过"的单元在 Intel 正确模型下
  63/63 仍通过；真正靠偏差的是 8 个 `… pop ebp; ret [imm16]` 共享尾声块，`ret` 处 +4 就是返回槽。
  取证见 `repo:docs/03-emulator/aot-s7-evaluation.md` §6。
- 不进本节点：其余四类降级（动态返回 / 可变长栈 / 写返回槽 / 表外 callee）的进一步放宽；
  "单元根必须是函数入口"（另立、单独读数）。

## 读数与复现

全部读数、命令与口径（生成期 `lift-gen: [S7 criteria]` / `[S7 downgrade]`、30 帧指纹、`boundary` ns/派发）
见 `repo:docs/03-emulator/aot-s7-evaluation.md` §0 / §4 / §7。本节点不复述数字。
