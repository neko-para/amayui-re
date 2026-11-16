# 位置谓词进 DSL：await position <SCRIPT> <FROM> <TO>（替掉不可靠的 await script + wait）

- id: REQ-01M7HFWKVZJFD2GQ1KRAA6NZVH
- type: req
- status: done
- parent: REQ-01M7G7EJF062NKQ0D40RF3BZ4Z
- verify: apps/emulator-lift/CMakeLists.txt#input-plan-read-points
- done_reason: await position <SCRIPT> <FROM> <TO> 落地（hex、FROM<TO、半开区间、读不到继续等 + positionMisses 诊断）。★ 确定性成立：正例 [0x0460,0x0520) + click 1070 480 left ⇒ fired=1 + region 117490928/25，4 次逐位相同（实现者 3 + 父级 1）；负对照 [0x388C,0x38A4) ⇒ fired=0 + 基线 108595152/21/6674。★ 两条我把票写错的、已纠正：(1) 区间写反了——轮询体 [0x039C,0x0460) 门命中但点击无效果，能生效的是 TITLE 回调体输入端 [0x0460,0x0520)；(2) header_len 不能从装载头读（头被读进栈、不进 guest 内存），只能是常量 0x3C（941/941 SYS4450）。门禁：build exit 0 · ctest 102/102（6 skip）· pnpm test 552/552 · pnpm verify 500/500 · 变异 3/3 红（含半开区间改闭区间）。
- tags: [emulator-lift, input, dsl, position]

## 一句话

给 `--input-plan` 加第四条指令 **`await position <SCRIPT> <FROM> <TO>`** —— 脚本内位置的**电平门**（半开区间，hex）。
它是"到标题菜单再点"这类配方的**第一种确定性门**。

## 为什么（都有读数）

- `await script TITLE` **太粗**：`--skip-logo` 下第一个 guest 读点身份就已是 `TITLE.BIN` ⇒ 点击打在 boot 上（实测读数 = 无输入基线）。
- `wait <ms>` **不可靠**：墙钟 ≠ guest 推进 ⇒ 同一配方 12 跑 4 成（`docs/03-emulator/input-plan.md` §2.6 的边界段）。
- 位置公式与 TITLE 输入循环区间**已取证**（`REQ-01M7HC1Z1RD4DNH25BN5ZE1HQK`）：
  `pos = header_len + (frame[cur].+0x04 − frame[cur].+0x00)`，TITLE 输入循环 = `[0x039C, 0x0460)`（清单 56–64 行）。

## 语义（已裁决）

- **语法**：`await position <SCRIPT> <FROM> <TO>`。名字沿用现有匹配（大小写不敏感、两侧剥 `.bin`）；
  `FROM`/`TO` 必须 `0x` 前缀的 hex；**FROM 含、TO 不含**；解析期校验 `FROM < TO`。
- **判定**：`script_id` 匹配 **且** `FROM <= pos < TO` ⇒ 通过；否则继续等（⛔ 不报错、不超时，卡住由 `--time-limit`/`--max-frames` 报）。
- **`header_len` = 常量 `0x3C`**（★ 2026-11-16 实测纠正：**不能从装载头读** —— 装载器把 0x20 头与 `sub_header_length`
  读进**栈**（`0x40EE67`/`0x40EFF2`/`0x40F01C`），`0x40F69E` 从文件偏移 0x3C 起读进 `frame+0x00` ⇒ 头在 guest 里**没有落点**。
  语料 941/941 视图全 `SYS4450`。若将来出现别的前缀，`sample` 要如实报 `header_len_source:"const-0x3C"`）。装载长度同样读不到
  ⇒ 用"同区域 + committed + 语料上界"代替。`base==0` / `pc` 越界 ⇒ 门不满足、不崩。
- ⛔ 仍只在 `atPoint()`（guest 读点）里判；`machine.cpp` / `aot_run.cpp` 禁用词仍 0 命中。

## 判据（逐条要能红）

1. 单测：命中 / `pos==FROM` 命中 / `pos==TO` 不命中 / `base==0` 与 `pc` 越界 ⇒ 不满足且不崩；解析期错（`FROM>=TO`、缺参、非 hex）。
2. **端到端逐位相同**（2026-11-16 实测裁决后的正确区间）：
   - 正例 `await position TITLE 0x0460 0x0520` + `click 1070 480 left` ⇒ `fired=1` + `region in_use=117490928 / surfaces=25`
     （`heap_blocks` ±1）；**4 次逐位相同**（实现者 3 次 + 父级 1 次）。
   - 负对照 `await position TITLE 0x388C 0x38A4`（出口段，轮询期不执行）⇒ `fired=0` + 基线 `108595152/21/6674`。
   - ★ **反例留档**：轮询体 `[0x039C,0x0460)` 门**命中且确定**，但点击**无效果**（`region=…/21/6675`）⇒ "菜单就绪"不等于
     "输入循环在跑"，而是"TITLE 回调体输入端"。⚠ 机制（为什么轮询体里点无效，疑似消息泵在 `i2fc` 前把注入取走）**未验证**。
3. 变异自检：位置门加一条变异（`< TO` → `<= TO`，或去掉 `header_len`），必须按预期红。
4. 文档：`input-plan.md` 语法表 + `pos` 的坐标系（绝对字节偏移、4 字节对齐、与 `label_%08x` 同坐标系）+ help 段。

## 未做

解释器仍不接 `--input-plan`；`ScreenRect` / `click center` 不做；位置只对**同一份脚本字节**成立（汉化 `insert-after` 会移偏移）。
