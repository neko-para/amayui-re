# `data/spec/format/opcode-table.json` —— 说明书

> 同名说明书（约定见 `../../../docs/00-origin/decisions.md` §6，由 `tools/test/json-docs.test.mjs` 守）。
> 字段 / 不变量 / 操作的唯一真源是 `pnpm tools spec describe` —— 本文件不复述 schema。

## 这个文件是什么

**脚本二进制协议的指令表**（本规范自己的数据，不是 `age-format` 的 canonical 副本）。
每条 `{opcode, argc, name, aliases}`：

- `argc` 是**决定指令边界的唯一字段**：一条指令 = `4 + 8×argc` 字节；
- `name` / `aliases` 只用于文本表示与重汇编查找，不改变字节语义；
- 空 `name` 表示未命名 opcode（反汇编器用规范名 `iXXX`）。

★ **当前状态：待核验（unverified）**。表的内容最初来自 `age-format` 的现有实现（它仿照第三方库写成），
尚未逐条对引擎解码代码 / 长度字核验。规范正文（`chapters/10-format.md`）已把这个状态写进「核验状态」。

★ **依赖方向**：本文件是规范数据；`packages/age-format/src/asm/instruction-set.json` 是实现副本。
实现与本表不一致时以本表为准（但先要完成核验；未核验前不要拿它当"已证实"）。

相关核验记录：[`verification.md`](verification.md)（内部；EA / 台账 / 守卫）。

## 怎么查

```bash
pnpm tools spec describe                 # 本文件与规范 schema 的自描述
pnpm tools spec validate                 # 规范正文 / 绑定 / 审计的一致性
node -e "import('./data/spec/format/opcode-table.json', { with: { type: 'json' } }).then(m => console.log(m.default.length))"
```

## 怎么改

1. 先核验：在引擎里找到该 opcode 的**解码 / 长度推进 / handler**，把 `argc` 与语义关联登记到审计。
2. 改本表（规范数据）：只改已核验的条目；未核验的条目保持现状并继续标 `待核验`。
3. 同步实现副本：`packages/age-format/src/asm/instruction-set.json`（实现侧；可另跑其测试）。
4. 跑 `pnpm test` + `pnpm tools spec validate`。

★ 不要从 `age-format` 反向覆盖本表：实现不是规范来源。当前表只是**尚未核验的初始版本**。
