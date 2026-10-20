# apps/emulator-new —— 只读 `data/spec` 的参考实现

> ⛔ **本目录是 spec 的消费者，不是 spec 的来源。**
> 它**只读 `data/spec/`**（当前只有指令表 `format/opcode-table.json`）；⛔ 不 import `@amayui/age-format`、
> `data/shape`、`data/ledger`、`corpus/`，也⛔ 不复制 `apps/emulator`（已废弃，仅作架构参考）。
> 守卫：`tools/test/emulator-new-isolation.test.mjs`（会红）。

## 1. 这一层现在做什么

最小闭环：**读指令表 → 解析脚本头 → 按 `4 + 8×argc` 切指令 → 跑到第一个"没有实现"的 opcode 并报缺口**。

- 没有渲染 / 音频 / 输入；不记录外部副作用（`data/spec/core/README.md` §1 裁决）。
- **未实现即抛错**（不 no-op）：缺口带 `{文件, 指令序号, 偏移, opcode, argc}`，可以当门禁用。
- `CORE-OPS` 尚未成篇 ⇒ 当前没有任何 opcode 有实现，所以默认模式在**第一条指令**就报缺口；
  `--decode` 是纯解码走查（不解语义），用来检验 `CORE-ISA` 的 9 条规则够不够实现一个解码器。

## 2. 怎么跑

```bash
# 跑到第一个未实现 opcode（默认）
node apps/emulator-new/src/main.ts --script "E:/Games/Eushully/天結いキャッスルマイスター/SC0000.BIN"

# 只解码（走完代码区），验证头 / 切分 / 表指针
node apps/emulator-new/src/main.ts --script <file> --decode

# 机器可读
node apps/emulator-new/src/main.ts --script <file> --json
```

## 3. 本目录的硬口径

- 只读 `data/spec`：唯一碰文件系统的模块是 `src/spec.ts`。
- TypeScript 只写**可擦除语法**（类型标注 / `interface` / `type` / `import type`），由 Node 原生剥壳直接跑，**没有构建步骤**。
- 缺口不是失败：它是"spec 还缺哪一条 opcode 的语义"的机器清单；`--strict` 时才用退出码 1。
