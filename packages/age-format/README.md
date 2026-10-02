# @amayui/age-format — AGE 二进制格式

## 1. 职责

AGE 引擎的**容器 / 资源格式**读写：**ALF**（归档）、**AGF**（图像）、**ASM**（脚本）。
解析、组装、校验 —— **只做容器与像素排布，不含任何游戏语义结论**。

```text
packages/age-format/
  cli.mjs              # CLI（查看 / 解包 / 重打包 / ★ verify 往返复验）
  src/lzss.mjs         # 纯工具：ALF 族的 LZSS（两个方向）
  src/alf.mjs          # 领域模型：ALF 归档（索引 + 数据体）
  src/agf.mjs          # 领域模型：AGF 图像（含 AGF 族的 LZSS 与像素排布）
  src/asm/             # 领域模型：ASM 脚本（指令表 + CP932 编解码 + 反汇编/重汇编）
  test/                # 守卫：三套格式的「解包 → 重打包逐字节相同」
```

## 2. 判据：**解包 → 重打包逐字节相同**

这是本包**唯一**的验收口径，且由 `packages/age-format/test/*.test.mjs` 机械核对；
样本是**原始游戏文件**，按口径**不入库**（只登记来源与 sha256，见 `corpus/assets/samples.md`），
所以样本**不在场时用例跳过**（fresh clone 上不该红），而判据的实测证据记在需求节点
`REQ-01M3TCP0B0CTCSCJXYKM2ESKHG` 的凭据段。

```bash
node packages/age-format/cli.mjs verify            # 一条命令版：三套格式的往返复验
node packages/age-format/cli.mjs alf-list   <索引文件>
node packages/age-format/cli.mjs alf-unpack <索引文件> --out <目录>
node packages/age-format/cli.mjs alf-repack <索引文件> --out <文件>
node packages/age-format/cli.mjs agf-info   <*.AGF>
node packages/age-format/cli.mjs asm-dis <*.BIN> --out <*.txt>   # / asm-asm 反向
pnpm test                                          # 全仓测试（含本包的守卫）
```

## 3. 三套格式的盘上事实（**只写非显然的**；详细规则在各模块头注释里）

### 3.1 ALF：索引与数据体是**两个文件**

* 索引（`*.AAI` / `SYS?INI.BIN`）= 头（S4 300 B / S5 540 B，加装档段起点前移到 268 / 532）
  \+ 一个 **LZSS 压缩的目录区**；数据体（`*.ALF`）= **载荷字节的裸拼接**（无头、无目录区）。
  ⇒ **成对**才能验证载荷搬运；`readAlf` 只读索引，"看一眼目录"不需要几百 MB 的归档在场。
* 目录项里**文件名后面那截是未初始化内存** ⇒ 拼装时必须保留**原始定长字节**，
  只在真的要改名时覆写 —— "重拼名字再补零"必然与原件不同。
* ★ **LZSS 重压逐字节可复现**（实测 3/3，含 44.6 万字节的目录区）⇒ 判据能成立，不必退化成"复用原始压缩区"。

### 3.2 AGF：ACGF 头 + 三段

* `magic(4) + version(4) + unknown(4) + metaUnp(4) + metaUnp(4) + metaPak(4)`，
  然后 meta 数据、body 头 12 B（`unknown / unp / pak`）、body 数据、可选的 `ACIF` alpha 块。
* ★★ **头部 `+12/+16` 是未压缩大小、`+20` 才是压缩后大小** —— 把 `+12/+16` 写成压缩后大小会得到
  "长度对、内容错"的文件（真实件 `MI042/MI047` 就是这么抓出来的）。
* ★ **"原样还是压缩"的判定**：对**游戏自身打包**的件，规则是"压得比原文小就存压缩结果，否则原样"，
  且压缩结果与盘上**逐字节相同**（meta / body / **alpha 也一样会压**，旧仓注释只说了前两者）。
  ⚠ 旧仓注入链生成的件会把 body 直接写**未压缩**（哪怕压得小）⇒ 那是那条工具链的选择，**不是格式规则**。
* 8bpp 的调色板在 **meta `+56`** 起（每项 BGRA）；有的件 meta 较短 ⇒ **调色板项数不足 256**
  （"截断调色板"变体），此时"像素索引是否越界"必须显式报错，**不编造灰度**。

### 3.3 ASM：指令表驱动的脚本字节码

* 指令边界**只由 `argc` 决定**（一条指令 `4 + argc*8` 字节）；`instruction-set.json` 是**从登记来源机械派生**的
  **格式层四列**（`opcode` / `argc` / `name` / `aliases`，`pnpm tools opcodes derive`）。
  ★ **权威性**：`argc` 的权威来自**引擎**（拿 `handler` 指到的处理函数、数出它推进偏移的位置就能机械算出来），
  不是三方工具说了算；本表里的 `argc` 是**待复核副本**，复核入口 `handler` 归知识线
  （登记 `knowledge/opcode-handlers`，`blocks: [K1,K2,K3]`）。
  在复核之前，它的凭据是**可执行观测**：106 个真实 `.BIN` 里 104 个往返逐字节相同。
  ★ **`handler` 与 `argc` 是同类**（都是从引擎读出来的**观测**，判据是"能不能再观测一次"）：
  格式层不需要 `handler`，所以它**不进格式层**，但走**继承登记**、**不当结论丢掉**。
  唯一被丢的是 `status` —— **人工自述标签**，没有机械复核路径（留痕 `knowledge/opcode-status-labels`）。
  `name` / `aliases`（87 条）是三方工具旧资产、**无独特信息**，只作文本层兼容层。
  口径详见 `src/asm/instruction-set.md`。
* 码页：CP932 两个方向自持（含外字区 `0xF040–0xF9FC ↔ U+E000–U+E757` 与 IBM 扩展区归属，
  详见 `src/asm/codec.mjs` 头注释）。
* ★ 旧仓对 **v5 头字段整块错位 8 字节**（`parseNumericFields(buf, 16)` 又内用 `+8…+56`）；
  真实语料全是 v4 故一直没暴露。本包按字段顺序修正，并用合成 v5 脚本验过往返逐字节相同。

## 4. 纪律

* 格式层**只做容器**：字段含义 / 函数用途 / 脚本角色 / **opcode → 引擎函数**一律不在这里
  （那是待重建的知识层，见 `../../docs/00-origin/knowledge-rebuild.md`；K3 通过前不得进台账）；
  ★ 但**观测要能追溯**：指令表的来源登记在 `corpus/assets.json`（`knowledge/opcode-table-source`），
  派生器按条目解析路径 ⇒ 旧仓移除也不会断链；
* 解析器**不得改写原始素材**：任何"解析友好化"只允许是派生的内存视图（`AGENTS.md` §1 第 5 条）；
* 指令表里**不得**出现 `handler` / `status` 之类知识层字段（`pnpm tools opcodes describe` 会点名"有意丢弃"）。

## 5. 迁移批次

**M2**：本包落地（ALF / AGF / ASM 三套 + 守卫）。~~UIMAP~~ **不在范围内**（用户口径：uimap 只是一个工具）。
前置：M1（反汇编语料入位）。
