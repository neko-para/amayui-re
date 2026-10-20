# apps/emulator-new —— 只读 `data/spec` 的参考实现

> ⛔ **本目录是 spec 的消费者，不是 spec 的来源。**
> 它**只读 `data/spec/`**（指令表 + 章节规则）；⛔ 不 import `@amayui/age-format`、
> `data/shape`、`data/ledger`、`corpus/`，也⛔ 不复制 `apps/emulator`（已废弃，仅作架构参考）。
> 守卫：`tools/test/emulator-new-isolation.test.mjs`（会红）。

## 1. 这一层做什么

**真实入口**：安装目录 → `SYS4INI.BIN`（ALF 索引）→ **统一文件 id == TOC 条目下标** → 取根脚本 **id 0**（`SYSTEM4.BIN`）
→ 解析脚本头 → **执行**：取指 → 按 opcode 选处理器 → 执行 → 按「操作数个数」槽推进 `4 × (2×argc+1)` 字节；
门关着时只推进虚拟时钟并服务状态位（`70-vm.md`）。

- 取件顺序：**安装根下的松散文件优先**，否则按 TOC 的 `(archive, offset, length)` 从 `DATA*.ALF` **切片**（不整份读）。
  ⛔ 不许"直接读某个脚本文件"当启动：那会跳过索引与初始化流程（`--script` 只是调试口）。
- ALF 目录区：4 KiB 环形窗口 LZSS；**匹配的位置是窗口内的绝对写位置**（不是距离），窗口初值 0、写指针起点 `0xFEE`
  —— 这两点是"能把真实 21109 条目的索引解出来"的实现判据（见 `tools/test/emulator-new-alf.test.mjs` 与 boot assets 守卫）。

- 没有渲染 / 音频 / 输入；外部请求只记进 `m.effects`（`data/spec/core/README.md` §1 裁决）。
- **未实现即停**（不 no-op）：缺口带 `{指令序号, 偏移, opcode, argc}`，停在它**前面**，可以当门禁用。
- `handler` 表只包含 `data/spec` 里**已成篇**的 opcode；解不出的操作数类型 / 未取证的值一律**响亮失败**。
- 整型编码的 key、虚拟时钟步长、未初始化浮点的读法都是**显式参数**：⛔ 没有"悄悄用 0"的默认路径。
- `--decode` 是纯解码走查（不解语义），用来检验 `CORE-ISA` 的结构规则够不够实现一个解码器。

## 2. 怎么跑

```bash
# 真实入口：安装目录 → 索引 → 根脚本 id 0（--key 是整型编码的运行期 key，必须显式给）
node apps/emulator-new/src/main.ts --install "<安装目录>" --key 0x1234abcd

# 换一个统一文件 id（例：0x5262 = LOGO.BIN）做诊断
node apps/emulator-new/src/main.ts --install "<安装目录>" --root-id 0x5262 --key 0x1234abcd

# 调试口：直接给一份脚本文件（不是启动路径）
node apps/emulator-new/src/main.ts --script <file> --key 0x1234abcd

# 命中停止条件（如 0x20f play-movie）就停下（停止条件是"停在它前面"，不执行它）
node apps/emulator-new/src/main.ts --script <file> --key 0x1234abcd --stop-at 0x20f

# 仿真参数：时钟步长（毫秒）与未初始化浮点策略（zero ⇒ 读成 0 并记诊断）
node apps/emulator-new/src/main.ts --script <file> --key 0x1234abcd --tick-ms 16 --uninitialized-float zero

# 只解码（走完代码区），验证头 / 切分 / 表指针
node apps/emulator-new/src/main.ts --script <file> --decode

# 机器可读
node apps/emulator-new/src/main.ts --script <file> --key 0x1234abcd --json
```

## 3. 本目录的硬口径

- 只读 `data/spec`：碰文件系统的只有 `src/spec.ts`（读规范表）与 `src/host.ts`（读安装目录 / 索引 / 归档 / 松散文件）；
  `alf.ts` / `lzss.ts` / `machine.ts` / `values.ts` / `handlers.ts` / `main.ts` **零 fs**（隔离守卫会红）。
- TypeScript 只写**可擦除语法**（类型标注 / `interface` / `type` / `import type`），由 Node 原生剥壳直接跑，**没有构建步骤**。
- 缺口不是失败：它是"spec 还缺哪一条 opcode 的语义"的机器清单；`--strict` 时才用退出码 1。
- 前沿（"执行到哪一条 / 停在哪"）由 `tools/test/emulator-new-logo.assets.test.mjs` 的**棘轮**钉住：只许往前走。
- 浮点池的"未写槽"规范不说初值：默认**响亮失败**；要跑真实脚本必须显式选 `--uninitialized-float zero`，
  且每次读都会进 `diagnostics`（⛔ 不许静默补 0）。
