# spec 基建缺口（ISA/POOL/VM 复核反馈）：守卫误标 / 证据档 / 选择器形状 / 工具盲区

- id: REQ-01M5AM4M5A5TV4APMPN0QWEA96
- type: req
- status: open
- parent: REQ-01M5851Z51V1MTSWH414M7WN5Q
- order: 66
- tags: [engine spec infra]

## 背景

`CORE-ISA` / `CORE-POOL` / `CORE-VM` 三个取证子代理各自交了 `feedback.md`（每条附可执行反例）。
这里收**基建 / 口径**问题；规范内容问题已直接修（见 `10-format.md` 与 `core/README.md`）。

## 清单

1. **守卫误标 + 覆盖不足**：`tools/test/engine-operand-types.test.mjs` 只钉 5 张分派表里的 3 张，
   且把 `sub_42B4B0` 叫 "float 取址"（实为**写整**）；`bindings` 拿它当"五原语支持集"的证据。
   → 扩到 5 项（12/14/14/10/11）+ 中性命名；或把规则证据缩到它真覆盖的 3 张表。
2. **layout 注释差一个字段**：`FRAME_LAYOUT.off.operands` 写"opcode 在 [第一操作数−4]"，
   实为 **type**；按它写解码器会整体错 4 字节。
3. **编码 key / 编码零没有诚实的 `storage` 指针**：只在 `layout.mts` 的 `EVIDENCE` 里；
   `layout:EVIDENCE.codecKey` 能过校验但语义是滥用。→ 加 `CODEC` 常量，`EVIDENCE` 引用它。
4. **草稿模板与 audit 字段形状不一致**：`sites`（站点列表）≠ `covers`（effect 选择器）。
   → 已在 `data/spec/core/README.md` §6 要求草稿带 `covers`；本条只剩"旧草稿补齐"。
5. **`pilot.outOfScope` 的语义**：用"switch 之前的独立分支"排除了 4 个句柄分支，
   于是"未登记 0"只对声明的 58 块成立。→ 这种"按结构位置排除"应进 `gaps`（未归属），不要放 `outOfScope`。
6. **`reviewed` 与语料型机制冲突**：`reviewed` 要求至少一条 `@env pure` 守卫，
   但本机制的判据必须读语料（相关守卫都是 `@env assets`）⇒ 语料型机制永远升不到 reviewed。
   → 需要裁决：允许 assets 守卫算强证据（附 tier 说明），或加一档状态。
7. **`shape access` 漏寄存器快照位测试**：主循环状态字只报 32 行 / 4 处内存位测试，
   而 20 处判据在寄存器快照上（覆盖 19 位）⇒ 只按它会 23 位读成 3 位。
8. **`spec validate` 只认 `KN:` / `guard:`**：草稿阶段拿不到 → 已定口径"草稿可用 `draft:` 占位、合并前换掉"，
   写进 `core/README.md` §6；不需要改校验器。

## 判据

* 1 扩到 5 张表后，改坏任意一张表的 case 都会让守卫红（变异条目为证）。
* 2 注释与实测一致（`[第一操作数−4]` = type）。
* 3 `layout:CODEC.*` 能解析，且 `EVIDENCE` 里不再有裸地址常量。
* 6 裁决落到文档（`core/README.md` 或 `AGENTS.md`），并有一条守卫或明确的"不改，理由"。
* 7 要么补工具（寄存器快照的位测试），要么在 shape 文档里标明"位测试不在覆盖内"。
