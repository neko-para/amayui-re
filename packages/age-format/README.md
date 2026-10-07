# @amayui/age-format — AGE 二进制格式

## 1. 职责

AGE 引擎的**容器 / 资源格式**读写：**ALF**（归档）、**AGF**（图像）、**ASM**（脚本）。
解析、组装、校验 —— **只做容器与像素排布，不含任何游戏语义结论**。

★ **AGE 脚本成品与它的文本层是一件事**：`.BIN` ↔ 反汇编文本的往返（解析 / 组装）、脚本文本的 **reflow**
（排版层的机械规则：视觉行折行、专名不许腰斩、可选标记的写法）**都属于本包**。
不过 reflow 目前**只有下游的需要、没有实现** —— 现状与跟踪单见 §3.4。

```text
packages/age-format/
  cli.mjs              # CLI（查看 / 解包 / 重打包 / ★ verify 往返复验）
  tsconfig.json        # ★ 只做类型检查（noEmit）—— 本包**没有构建步骤**
  src/**.mts           # ★ 源码是 TypeScript，且**类型就写在实现里**（没有配对的 .d.mts）
    src/lzss.mts       #   纯工具：ALF 族的 LZSS（两个方向）
    src/alf.mts        #   领域模型：ALF 归档（索引 + 数据体）
    src/agf.mts        #   领域模型：AGF 图像（含 AGF 族的 LZSS 与像素排布）
    src/asm/           #   领域模型：AGE 脚本（指令表 + CP932 编解码 + 反汇编/重汇编 + ★ reflow 的落点）
      src/asm/bytes.mts     #   字节原语（比较 / 拼接 / latin1 / DataView 读写）
      src/asm/header.mts    #   脚本头结构 + readHeader
      src/asm/opcodes.mts   #   指令表：★ 随模块自带的 instruction-set.json（ESM JSON import）+ 建表 + 三向查找
      src/asm/runtime.mts   # ★ **运行期子集入口** —— 模拟器核心只 import 这个
      src/asm/index.mts     #   工具侧总入口（= 运行期子集 + 反汇编/重汇编 + 码页编解码）
    src/engine/        #   ★ **引擎镜像布局的观察记录**（槽位/偏移/opcode→handler）—— 逆向知识，不属于模拟器
  test/                # 守卫：三套格式的「解包 → 重打包逐字节相同」
```

★ **为什么是 `.mts` 而不是 `.mjs` + 手写 `.d.mts`**（2026-10 变更）：
`.d.mts` 与 `.mjs` 之间 **TypeScript 不交叉校验** —— 声明里写错**参数类型**，`tsc` 完全不响。
实测代价：那份手写声明里 `roundTripBytes` 被写成 `boolean`（真值是**字节**）、
`parseWhBpp` / `extractPaletteRgb` / `packSection` / `decodeRgba` 的返回**全写错** ——
名字层面对得上，签名全错。改成一份真源后，这些**立刻**变成 `tsc` 错误。
⇒ 消费方 `import type { Header } from '@amayui/age-format/src/asm/index.mts'`（入口用 `export type { … }` 再导出）。

★ **没有构建步骤**：`.mts` 由 **Node v24 的原生 type stripping** 直接跑（`node cli.mjs` 照旧）。
`tsc` 的唯一职责是"把类型写错变成红灯"：`pnpm typecheck`（根目录，覆盖本包与 `apps/emulator` 两个工程）。
⇒ 写法约束：**只许可擦除语法**（类型标注 / `interface` / `type` / `import type`），
❌ 不用 `enum` / `namespace` / 构造器参数属性 / 装饰器（它们要**代码生成**，会逼出构建步骤；
`tsconfig.json` 的 `erasableSyntaxOnly` 会提前把它变成类型错误）。
★ 本包**工具侧**需要 `@types/node`（`alf` / `agf` / `lzss` 用 `Buffer` / `node:fs` / `process`）。
★ 但 **`src/asm/**` 整个目录零 Node 依赖**（判据：`rg 'node:' packages/age-format/src/asm/` 为空）—— 见 §1.1。
★ 守卫：`tools/test/age-format-types.test.mjs` 盯三条 —— **幽灵声明** · **公开面类型可达** · **未注解导出为 0**。

### 1.1 ★ `src/asm/**`：**整个目录零 Node 依赖**，分界只剩"范围"

模拟器的核心**将来要跑在浏览器里**。这条口径的**根因**曾经是 `node:fs`：`opcodes.mts` 在模块顶层
`import fs` 读 `instruction-set.json` —— `Buffer` 还能 polyfill，**`fs` 没有 polyfill 可打**。

★ **根因已经拔掉**：指令表改成 **ESM JSON 模块 import**
（`import defs from './instruction-set.json' with { type: 'json' }`）—— Node 原生支持、打包器支持、
浏览器也支持（`with { type: 'json' }`）。⇒ 现在：

```
判据：rg 'node:' packages/age-format/src/asm/   →   空
```

于是两个入口的分工不再是"谁能跑在前端"，而是**范围**：

| 入口 | 是什么 | 谁 import |
|---|---|---|
| `asm/runtime.mts` | **运行期要的那一份**：`bytes` + `header` + `opcodes`(含自带指令表) + `types` | 模拟器核心（`apps/emulator`） |
| `asm/index.mts` | **全部**：上面的 ＋ `disassemble` / `assemble` / `codec` | `tools/**`、本包 `cli.mjs`、守卫 |

★ **"加载任意一份表"不是本层的事**：`buildOpcodeTable(entries, source)` 就是那道缝 —— 要读别的文件 /
从网络取的调用方自己把数组弄来。（原有个 `loadOpcodeTable(file?)`，实测**全仓无一处传路径** ⇒ 已删。）

★ **边界仍由类型系统强制**：`apps/emulator/tsconfig.json` 是 **`"types": []`** ⇒ 谁把 `Buffer`（TS2591）
或 `node:*`（TS2307）拉进**可达闭包**，`pnpm typecheck` **当场红**（实测，不需要"扫源码的测试"）。
⇒ 入参一律收 `Uint8Array`（`Buffer` 是它的子类 ⇒ Node 侧调用方零改动）；**返回 `Uint8Array`** 而**不是 `Buffer`**
—— ⚠ 代价是三个"`tsc` 抓不到"的静默陷阱（`.equals` 没了、`.toString()` 语义变了、`.slice()` 从视图变拷贝），
**逐条记在 `src/asm/bytes.mts` 头注里**，改动返回值类型时必须人肉过一遍（审计命令也写在那里）。
★ **WHATWG 通用类型怎么来**（`TextDecoder` / `URL` / `AbortController` / `structuredClone`…）：
用 TypeScript **内置的宿主库** `lib: ["ES2023", "WebWorker"]` —— 实测它给全这批 API，
且**不给** `document` / `window` / `HTMLElement` / `localStorage`（⚠ 但会给 worker 专属的 `self` / `postMessage`）。
★ 为什么不用另两条：`lib: DOM` 会把 DOM 全局全放行（而核心还要能在 Node 里跑）；
`@types/web` 就是 `lib.dom.d.ts` 的**同一份生成物**（解包 9.6 MB，仍然只有 DOM）。
上游那条"把 Node 与 DOM 公共的 API 抽成 `lib.common.d.ts`"**至今只是提案**（TypeScript #41727，标签 `Awaiting More Feedback`）。
★ 运行期子集**目前不含 `codec.mts`**，但那已经是**范围**问题、不再是类型问题（理由见 `runtime.mts` 头注）。

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
  详见 `src/asm/codec.mts` 头注释）。
* ★ 旧仓对 **v5 头字段整块错位 8 字节**（`parseNumericFields(buf, 16)` 又内用 `+8…+56`）；
  真实语料全是 v4 故一直没暴露。本包按字段顺序修正，并用合成 v5 脚本验过往返逐字节相同。

### 3.4 reflow：**落点已定，实现未做**

**分工**：`src/asm/` 只管"`.BIN` ↔ 反汇编文本"，**reflow**（脚本文本的排版层机械规则）也归它 ——
但两者**不是同一件事**：往返管的是"字节与文本互相是对方的可逆像"，reflow 管的是"文本怎么断行 / 怎么标"。

**为什么在这**：旧仓的 `scripts/` 里既没有独立的脚本包，也没有把 reflow 与格式层分开 ——
`.BIN` 的读写在 `scripts/asm/`，reflow 是同一批文本资产的后续加工 ⇒ 新仓不再造第二个包。

**现状**：`disassemble` / `assemble`（往返）已实现并有守卫（§2）；reflow **没有任何实现**
（`packages/script-dsl` 这个曾用名下的占位包已删除 —— 它无代码、无人依赖、
声称的三件事里两件半本来就在本包）。

**要什么再做什么**：reflow 是否需要机械化属**待裁决**，跟踪单在需求树
（`pnpm tools requirements show 1M47SGPFRBWPEQA8AF1WYQ8KP`）；裁决与判据写在那里，**本 README 不写状态**。

* **下游对它的依赖是软的**：翻译域当前接受**手工折行**（`.agents/skills/amayui-translate/SKILL.md` §8），
  所以本题不阻塞任何一批迁移。
* **它一旦落地，唯一验收口径仍与 §2 同源**：reflow 只允许产出**能反解回字节**的文本
  （`assemble(disassemble(bin))` 逐字节相同 ⇒ 折行不能改变指令流）。

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
