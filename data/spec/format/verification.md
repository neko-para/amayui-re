# 脚本二进制协议 · 核验台账（内部）

> 这不是分发的规范正文；它是 `chapters/10-format.md` 的**核验记录**。
> 规范正文里只写"部分 / 待核验"的状态，不列 EA / 台账 id / 守卫路径；证据在这里。

## 状态口径

- **verified**：已在引擎解码代码里找到对应点，且有台账记录或可执行守卫；
- **partial**：只有一部分（例如只验了 v4、只验了 `+32` 子头）成立；
- **unverified**：当前来自 `age-format` 的现有实现（仿照第三方库），尚未与引擎解码代码逐条关联；
- **out-of-scope**：不属于 ASM 脚本协议（ALF / AGF 等）。

## 台账

| id | 声明 | 状态 | 关联（EA / 台账 / 守卫） | 未完成 |
|---|---|---|---|---|
| `FMT-HEADER-SIGNATURE` | v4 签名 `SYS4`；v5 签名 `SY`（UTF-16LE） | **partial** | 加载器 `sub_40ED40`：`.text:0040EE97` 把 `"SYS4"` 拷进目标；`.text:0040EEC4` 对 4 字节做 `strncmp`；分支到 `loc_40EF61` | v5 在语料里无样本；`SYS3` 分支未归类 |
| `FMT-HEADER-LENGTH` | v4 头长 `0x3C`；v5 头长 `0x44` | **partial** | v4：`KN-01M48FRM964P523333585C4754`（v4 = 60 字节）+ `tools/test/emulator-iterate.test.mjs`；头 `+32` 子头见下条 | v5 无样本；`32 + u32@32` 与硬编码 60 的分叉未验证 |
| `FMT-HEADER-SUBHEADER` | 头 `+32` = 子头长度（含自身 4 字节）；`+36..+56` = 三组 `(len, off)` | **verified** | `KN-01M4GW2727581D5P1D5E7V3664`（accepted）；`tools/test/emulator-frame-tables.assets.test.mjs`（491/491） | `sub_header_length ≠ 28` 无样本 |
| `FMT-HEADER-FIELDS` | 13 个 u32 的顺序与偏移 | **partial** | 上一条覆盖 `+32..+56`；`+8..+28` 是 `local_*` / `unknown_data` 的名称 | `+8..+28` 的用途/来源未逐条对上加载器 |
| `FMT-INSN-BOUNDARY` | 指令 = `4 + 8×argc`；`operand_count = 2×argc+1` | **verified** | `KN-01M48FRM964P523333585C4754`：主循环 `.text:00413487` 用 `4×operand_count` 推进 `ip`；`.text:0041347D` 读 `帧+0x74`；`tools/test/emulator-iterate.test.mjs` | — |
| `FMT-OPCODE-ARGC` | 574 条 opcode 的 `argc` | **unverified** | `format/opcode-table.json`（当前来自 `age-format` 实现）；恒等式已验，逐条 `argc` 未验 | 逐条找 handler / 长度字写入点，与表对账 |
| `FMT-SUBTABLES` | 三张子表 = 三类 opcode 的 u32 位置数组 | **verified** | `KN-01M4GW25XQ0G1374026W7W6E76`（accepted）；`tools/test/emulator-frame-tables.assets.test.mjs` | 表类与 `0x71 / 0x03 / 0x8f` 的绑定已有；通用 label 表不存在 |
| `FMT-V5` | v5 头长 / 字段整体后移 8 | **unverified** | 无 v5 样本 | 找 v5 脚本或引擎 v5 分支 |
| `FMT-ALF-AGF` | ALF 归档 / AGF 图像 | **out-of-scope** | 不属于 ASM 脚本协议 | 另立协议 |

## 怎么用

- 规范正文的「核验状态」表只引用这里的**状态**，不复制 EA / 台账 id；
- 升级一条：先在引擎解码代码里找到点，补 `关联` 列，更新状态，再改 `chapters/10-format.md` 的状态表；
- 未核验的 `argc` 不得被当作"已证实的协议事实"；实现与规范冲突时，先核验再裁决。
