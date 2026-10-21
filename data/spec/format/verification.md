# 脚本二进制协议 · 核验台账（内部）

> 这不是分发的规范正文；它是 `chapters/10-format.md` 的**核验记录**。
> 规范正文里只写"已核验 / 部分 / 观察值"三种状态，不列 EA / 台账 id / 守卫路径；证据在这里。

## 状态口径

- **verified（正文写"已核验"）**：已在引擎解码代码里找到对应点，且有台账记录或可执行守卫；
- **partial（正文写"部分"）**：只有一部分成立，未闭合项写在该行"未完成"列；
- **unverified（正文写"观察值"）**：当前来自语料或 `age-format` 的现有实现（仿照第三方库），尚未与引擎解码代码逐条关联；
- **out-of-scope**：不属于 ASM 脚本协议（ALF / AGF 等）。

★ **两套词不是一回事**：本表的 `verified / partial / unverified` 是**协议条目的核验状态**；
`audit.json` 的 `status: draft | reviewed` 是**规则的证据档位**（`reviewed` 要求全部证据 strong、至少一条 accepted 台账或 `@env pure` 守卫、且有 falsifiers）。
映射只有一条：`verified` ⇔ 至少能拿到 accepted 台账或 pure 守卫；`partial` / `unverified` 都只能落在 `draft`（`partial` 可以`有部分强证据 + 未闭合残差`）。⛔ 不由正文的三词推出 `reviewed`。

## 台账

| id | 声明 | 状态 | 关联（EA / 台账 / 守卫） | 未完成 |
|---|---|---|---|---|
| `FMT-HEADER-SIGNATURE` | v4 签名 `SYS4`；装载器另有 `SYS3` 分支 | **partial** | 加载器 `sub_40ED40`：`.text:0040EE97` 把 `"SYS4"` 拷进目标；`.text:0040EEC4` 对 4 字节做 `strncmp`；分支到 `loc_40EF61` | `SYS3` 在语料里无样本，其头布局未归类。★ UTF-16 / `SY` 签名是**现有实现**的说法，本作装载器**没有**该分支（本版不规定） |
| `FMT-HEADER-LENGTH` | v4 头长 `0x3C`；`32 + u32@32`（有子头时） | **partial** | v4：`KN-01M48FRM964P523333585C4754`（v4 = 60 字节）+ `tools/test/emulator-iterate.test.mjs`；头 `+32` 子头见下条 | 老布局（版本串 < `"080 "`）无样本；现有实现里"v5 头长 0x44"未取证 |
| `FMT-HEADER-SUBHEADER` | 头 `+32` = 子头长度（含自身 4 字节）；`+36..+56` = 三组 `(len, off)`；短于 28 缺的整数为 0、长于 28 的多余字节不参与解释 | **verified** | `KN-01M4GW2727581D5P1D5E7V3664`（accepted）；`tools/test/emulator-frame-tables.assets.test.mjs`（491/491） | `sub_header_length ≠ 28` 无样本（行为来自代码，不是样本） |
| `FMT-VERSION-THRESHOLD` | 版本串与常量 `"080 "` 做 4 字节无符号 C 字符串比较（遇零或满 4 字节停），只取符号；< 0 ⇒ 老布局（无子头、头长 32）。本作语料版本串恒为 `"450 "` | **verified** | `0x40EFCC..0x40EFE4`（比较）；`.data:0051F0F4 Str2 db '080 ',0`（常量本体）；`0x40EF70..0x40EF8A` 与 `0x40F0FF..0x40F198`（接受性：版本串 > 引擎自身版本串 ⇒ 装载失败） | 老布局分支与"引擎自身版本串"的取值无样本 |
| `FMT-HEADER-FIELDS` | 13 个 u32 的顺序与偏移 | **partial** | 上一条覆盖 `+32..+56`；`+8..+28` 是 `local_*` / `unknown_data` 的名称 | `+8..+28` 的用途/来源未逐条对上加载器（六整数只有第 1 个对上消费点） |
| `FMT-INSN-BOUNDARY` | 指令 = `4 + 8×argc`；`operand_count = 2×argc+1`；分派器按 `4×长度槽` 推进 | **partial** | `KN-01M48FRM964P523333585C4754`：主循环 `.text:00413487` 用 `4×operand_count` 推进 `ip`；`.text:0041347D` 读 `帧+0x74`；`tools/test/emulator-iterate.test.mjs` | 直线指令是否**每条**都写长度槽未逐条核（控制转移类已分类） |
| `FMT-OPCODE-ARGC` | 574 条 opcode 的 `argc`；其中 544 条有非缺省处理函数、30 条落缺省 | **unverified** | `format/opcode-table.json`（内容当前来自 `age-format` 实现；`hasHandler` 列来自语料分派表 `0x0A509C` 的赋值行，544/30） | 逐条 `argc` 与 handler 写的长度字对账 |
| `FMT-SUBTABLES` | 三张子表 = 三类 opcode 的 u32 位置数组 | **verified** | `KN-01M4GW25XQ0G1374026W7W6E76`（accepted）；`tools/test/emulator-frame-tables.assets.test.mjs` | 表类与 `0x71 / 0x03 / 0x8f` 的绑定已有；通用 label 表不存在 |
| `FMT-V5` | 现有实现里 v5 头长 / 字段整体后移 8 | **unverified** | 无 v5 样本；本作装载器的签名选择只有 `SYS4` / `SYS3` 两条分支（`sub_40ED40`） | 若将来出现 v5 样本，先归类到签名 / 头布局两条，再决定是否单列 |
| `FMT-ALF-AGF` | ALF 归档 / AGF 图像 | **out-of-scope** | 不属于 ASM 脚本协议 | 另立协议（见 `alf-agf-verification.md`） |

## 怎么用

- 规范正文的「核验状态」表只引用这里的**状态**，不复制 EA / 台账 id / 守卫路径；
- 升级一条：先在引擎解码代码里找到点，补 `关联` 列，更新状态，再改 `chapters/10-format.md` 的状态表；
- 未核验的 `argc` 不得被当作"已证实的协议事实"；实现与规范冲突时，先核验再裁决。
