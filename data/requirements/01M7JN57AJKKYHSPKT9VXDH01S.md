# 导入桩台账数据清理（一次性）：SCOPE §1 补名 + 37 条过期 reason 重写 + imm32 条款裁决

- id: REQ-01M7JN57AJKKYHSPKT9VXDH01S
- type: req
- status: open
- parent: REQ-01M7A5TZP0P9AFNGQ79N321J75
- tags: [emulator-lift, overrides, text]

## 为什么从 B 单拆出来（父单 `REQ-01M7A5TZP0P9AFNGQ79N321J75`）

B 单里混了两种**失效方式完全不同**的东西：
- **机制**（`scope_ref` 必填 / reason 从 SCOPE 派生 / `MODULE_REASONS` 退役）⇒ 红了 = 守卫当场红，机器能判、要长期守；
- **一次性数据清理**（37 条过期 reason 重写 / SCOPE §1 补名 / `imm32` 条款裁决）⇒ 红了 = **没人能发现**，
  只有人逐条核对才成立，而且**做完就再也没有第二次**。
⇒ 机制留 B；数据清理归本单（做完即 close，⛔ 没有守卫可挂）。

## 要做（全是"改文本 + 人核"）

1. **SCOPE §1 补名**：把 `still-refused` 的 API 在对应行里点名（当前 27 条 reason 只有 `M1/M2` 锚）。
2. **37 条过期 reason 重写**：按 SCOPE 今天的口径（音频→已实现方案 C / 视频→已实现方案 A / 3D→部分落地）。
3. **`imm32!ImmAssociateContext` 的"IME 不做"条款**：要么在 SCOPE §1 立一行，要么删条目 —— 二选一，写清理由。
4. ⚠ **顺序**：要在 B 的 `scope_ref` 机制**之后**跑（否则重写完还得再改一遍）；取证可以并行。

## 判据

- `pnpm tools lift check` 绿（机制面由 B 保证），且**逐条人核**：每条 reason 都能在 SCOPE §1 找到对应行
  （读数形状：条数 / 命中数 / 未命中清单）。
- 条目数对照审计预测：74 → 62（57 导入 + 5 不导入），`not-referenced` 18 → 5（都带活锚）。⚠ 预测不是判据，实测才算。
- 收口以 `done_reason`：写明"哪 37 条改成了什么口径 + 未命中项如何处理"。

## 边界

⛔ 不重写机制（B 管）；⛔ 不动 C++ 登记（那 12 条不可达登记归 A 单 `REQ-01M7A5TZ91V3MRNEFDHWKN20MY`）。
