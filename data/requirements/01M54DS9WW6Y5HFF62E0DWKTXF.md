# 批 S1j · 闭合度：把'分析完成'变成可判的 L0 判据 + 未闭合按原因归档（四档逐个裁决）

- id: REQ-01M54DS9WW6Y5HFF62E0DWKTXF
- type: req
- status: done
- parent: REQ-01M52F3ASMGWQXX64JSA18MP2N
- order: 50
- verify: tools/test/shape-coverage.test.mjs#★arch-05
- tags: [engine shape]

## 背景（用户的问题）

用户问的是：**这一轮分析完成后，下一层真的能用吗？能不能可靠地发现"有指令没分析到 / 有逃逸的副作用"？**
举的例子是 `sub_41C7C0`：模型只给了 `ecx = T_Command*`，而它显然没分析完。

## 结论：判据已落地（L0 可判），四档未闭合的原因已逐个裁决

新增**闭合度**（`pnpm tools shape coverage`，并自动出现在每个 claims 报告末尾）与**归档**（`--json`）。

| 档（闭集合 `REASON_KINDS`） | 全表读数（`--all --limit 3000`） | 能不能消 | 处置 |
|---|---|---|---|
| **未归类行** `unparsedLine` | 147 个函数 → **0** | ✅ **已消** | 全表那 147 处**全是** `lea r,[r+0]`（MSVC 的 6 字节对齐填充）。access 层早已不当访问，coverage 层漏了同一口径 ⇒ 两端统一；守卫 `tools/test/shape-coverage.test.mjs#★arch-05` + 变异（改回去当场红） |
| **间接跳转** `indirectJmp` | 58 个函数 | ⚠️ **可消但不在本单** | `disasm-at cases` 能机械导出目标集（`sub_41BF50` = 14 个 case，全在函数内）⇒ 开单 `S1l` |
| **间接调用** `indirectCall` | 700 个函数 | ⛔ **不可消** | 虚表内容不在语料里（`.lst` 无 `.rdata` 内容）⇒ 如实留"目标未知"，由 L0 挡住（这正是本档存在的理由） |
| **基址未解析** `unresolvedBase` | 1384 个函数 | **分三档** | ①助手丢值（S1g/S1h/S1i 已修）②SEH catch ⇒ 局部类型载体 ③**值身份缺口**：帧记录 `+0` 读出的指针当基址 ⇒ 开单 `S1m` |

## 原单写的"控制流族 4/12 闭合、8 个未闭合"—— 已复核，结论不变

那 12 个载体在旧族文件里（`d5e7fd7:data/shape/control-flow-handlers.json`，现已合并进 `carriers.json`）。
从历史复原后逐个重跑：**仍是 4 闭合 / 8 未闭合**（`sub_41C6A0`/`sub_41C770`/`sub_41C8D0`/`sub_41C900` 闭合）。

* 间接调用 4 个：`sub_41A820`(2 处) · `sub_41C7C0`(1) · `sub_428A60`(6) · `sub_429460`(1)；
* 基址未解析 6 个：`sub_41A9B0` / `sub_41AB80` / `sub_4203D0` / `sub_420560`（各 1 处）+ `sub_428A60` / `sub_429460`；
* 逐个判的结果：**没有"差一步就能关上"的**。单处那一批经查是同一类（`mov ecx,[obj0+5D894h+120·cur]` 之后
  `lea edx,[ecx+eax*4]` —— 从帧记录 `+0` 读出的指针当基址），归 `S1m`；多处的归"间接调用不可消"。

## 归档（= "下一层能用的函数清单"，命令就是产物）

```bash
pnpm tools shape coverage --all --limit 3000 --json > .tmp/coverage-archive.json   # 全表
pnpm tools shape claims data/shape/carriers.json                                   # 末尾自带「闭合度」（载体级）
```

* 两个口径**分开报、不许相加**：`unclosed` 按**函数**计，`counts.byReason` 按**原因**计（一个函数可有两个原因）。
* 原因键是**闭集合**（`REASON_KINDS`）；未知 kind 当场报错 —— 否则新原因会被静默丢出归档。
* 载体级实测：230 个载体函数 **L0 闭合 78 · 未闭合 152**（末节的"未解析基址"按保守/校准两档读，差额不是缺口）。

## 判据纪律

任何声称"某函数分析完成"的地方，**先过 L0**（`claims` 报告末尾那一节不是装饰）：
没有未归类行 · 没有目标未知的间接调用/跳转 · 没有基址未解析站点。
