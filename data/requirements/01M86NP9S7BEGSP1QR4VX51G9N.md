# AOT · 库函数内建普查：第 2 个内建收谁（用派发份额 × FLIRT 名）

- id: REQ-01M86NP9S7BEGSP1QR4VX51G9N
- type: req
- status: open
- parent: REQ-01M7JHSEZVMYB4XHEN32V7V6HJ
- tags: [emulator-lift, aot, intrinsics]

# AOT · 库函数内建普查：第 2 个内建收谁（用派发份额 × FLIRT 名）

## 一句话

`_memcpy` 内建（`8ab0861`）证明"把静态 CRT 的库函数换成宿主实现 + 闸门回退"这条流水线是通的，
收益也大（2.29 M 次 / 2.5 s、33.4→0.0014 insn/次、点击大帧 −25.4%、装载帧 insns −24.7%）。
本单是**普查**：把还剩的库函数候选按"调用量 × 单次成本"排个队，决定第 2 个内建收谁 ——
⛔ 不是"再挑一个看起来热的"。

## 已排除的（反例，别再查）

- **`_memset`（`0x004E7400`）不值得**：SAVE11 点击后 2.5 s 只有 **1,074 次**、p50 = 20 B、p90 = 1320 B、
  max = 22 KB —— 换内建的收益 < 0.1 ms，表里不收。

## 要做的

1. **候选表**：对 `window-dispatch-top` 的 EA（点击窗口 + 装载帧 + FELLOW 切换帧三份）与 `.lst` 的
   FLIRT 名做交集，列出所有"静态 CRT 库函数"候选（`_memcmp` / `_strlen` / `_memmove` / `_memset` /
   x87 帮助函数 `__ftol2` 等），每个给：EA、名字、**调用次数 / 派发份额、单次成本（insn/次 + ns/次）**。
2. **判据**：只有"调用次数 × 单次成本 ≥ 阈值"（阈值自定，写清理由；建议以"点击窗口 ≥ 5 ms 或装载帧 ≥ 20 ms"
   为线）且**契约可精确表达**（返回值 / 写哪些内存 / callee-saved / 特殊寄存器 / fault 行为）的才进表。
3. **落表**：进 `repo:apps/emulator-lift/aot/intrinsics.txt`（同一张表、同一套 `lift-gen` 硬校验：
   PE sha 前缀 + EA 必须是单元根）；契约差分按 `repo:docs/03-emulator/aot-intrinsics.md` §5 的六步走。
4. **读数**：大帧 wall + insns、该函数的 insn/次、派发/次；五项指纹逐位同；`ctest` 全绿 + 变异（闸门或
   helper 的关键判据）。

## 边界

- ⛔ 只收**静态 CRT / 平台库函数**（FLIRT 名 + 反汇编契约双证）；⛔ 不收引擎自己的 handler
  （那是"第三个语义实现"，§12.8 红线）。
- ⛔ 不按调用点/EA 开特判：进表条件是"库函数契约"，不是"这块慢"。
- 每个内建都要一份契约差分用例（size × 对齐 × 重叠 × 未提交页）与开↔关差分。
