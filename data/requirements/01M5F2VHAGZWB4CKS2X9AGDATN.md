# spec：写 0x6 装载帧 / 0x8 换帧的规则（证据已有，此前被误写成「不规定」）

- id: REQ-01M5F2VHAGZWB4CKS2X9AGDATN
- type: req
- status: done
- parent: REQ-01M5D1G31PN9EGANRVAR753BPC
- verify: tools/test/emulator-frames.test.mjs#★★ `0x8 call-frame`
- tags: [spec, frame]

## 一句话

`0x6`（装载帧）与 `0x8`（换帧）在 spec 里长期只写「本版不规定」。2026-10-21 复核确认：**这不是有意的范围排除，而是"还没写成规则"**——`0x6` 早在旧参考实现里建模并有守卫，`0x8` 的逐字也已定位。本单把它们写成了 `CORE-OPS` 规则，并把"参考实现 / 守卫 / 台账"三件补齐。

## 来历（为什么之前写成"不规定"）

1. `15-frame` 是在 `689d487 feat: review`（2026-10-21 00:43）**新建**的，章首就从"本章只规定同脚本控制转移"这一**章范围决定**出发写了「本章仍不管：换帧（按槽号在已有帧之间切换，`0x6` 那一族）」——当时的归属还是错的（`0x6` 并不是换帧）。
2. 该判断没有绑定任何观察；同一时期 `REQ-01M4E4Q11P12MD0PJJDFZPZ5H0`（done）已经把 `0x6` 的槽语义写进旧参考实现并有守卫。
3. 2026-10-21 第三轮复核（F25/F58）纠正了归属（`0x6` = 装载帧、`0x8` = 换帧），但把"不规定"的措辞**沿用了下来**；而 audit 里两条残差的 `kind` 一直是 `unhandled`（未处理），两者互相矛盾。

## 做了什么

1. **spec**：`60-ops` 加两条规则 `CORE-OPS-FRAME-LOAD-FRAME`（`0x6`）/ `CORE-OPS-FRAME-CALL-FRAME`（`0x8`），章首从 45 → **47 个 opcode** / 39 → **41 条规则**；`15-frame` 的「尚未成篇」句改成指向这两条。
2. **bindings**：`0x6` 13 处站点 + 装载器 6 处；`0x8` 11 处站点；两条各挂函数与证据（台账 `KN-01M5F56MPF2S5R1M4Q1V6E7M4T` + 守卫）。
3. **audit**：两条 `unhandled` 章残差删除，改成两条规则自身的残差（见文末"没做的"）。
4. **参考实现**（`apps/emulator`）：新增 `Machine.callFrameAt` / `opCallFrame`（`0x8`）与 `Machine.returnToCaller`（`0x2` 的**条件释放 + 向下跳过空槽**，不再是 `cur -= 1`）；`loadFrameAt` 的父帧号改成"装载时正在执行的帧号"（引擎逐字是取 `0x6` 存下的旧 cur）。
5. **守卫**：`tools/test/emulator-frames.test.mjs` 新增 4 条 `0x8` 用例（切帧三格 / 未装载恢复 / 跨槽返回不释放 / 向下跳过空槽）；`tools/mutate-check.mjs` 新增 2 条（不写父帧号 / 无条件释放）。
6. **台账**：`KN-01M5F56MPF2S5R1M4Q1V6E7M4T`（accepted，guard 锚；含"换帧只改三格"与下面那条归属更正）。

## ★ 更正一处旧归属

`0x8` 的 handler 是 **`sub_41C900`**（表项偏移 `0x0A50BC` ⇒ `(0x0A50BC − 0xA509C)/4 = 8`），**不是** `sub_41C8D0`。`sub_41C8D0` 挂在 `0x0A50B8` 上 = **`0x7`**（体：写长度槽 3 后调 `sub_40EA00(this, op1)`，卸载指定帧）。⇒ "长度槽写 3"**不能**用来认归属（`0x7` / `0x8` 都写 3），只有表项偏移能。

本文旧版（以及 `docs/02-engine/spec-audit-2026-10.md` §十）里 `0x8` = `sub_41C8D0` 的写法已随之更正；`0x6` = `sub_41C7C0`（长度槽 5）不变。

## 判据（已过）

- `pnpm tools spec validate` **13/13**；两条规则各有多条 `binding.evidence`（含 accepted 台账记录）；
- `pnpm test`（纯，594+4 条）全绿；
- `node tools/mutate-check.mjs --match "不写父帧号"` 与 `--match "父帧号 + 1 == 当前帧号"` 各 **1/1 变红**。

## 没做的（在本单/audit 的残差里）

- `0x6`：两条按帧辅助数组的角色 · 宿主回调的身份 · 负帧号 · 深度报错之后的控制流；
- `0x8`：引擎级镜像格的读者 · 帧号 ≥ 40 的后果 · 同帧多次切入时返回栈的累积效应。
