# 建模字体重建入口 sub_459F40 的内部（0x78 / 0x2db 的尾跳目标）

- id: REQ-01M699AAGTFA9NS8RGBGXJEY3E
- type: spike
- status: dropped
- parent: REQ-01M674XHSX2HSFWSKDWZV2C63D
- dropped_reason: 内容就是 RES-OPS-SET-TAIL-FIELDS 这条残差；残差只登记在 audit。
- tags: [spec, residual]

# 建模字体重建入口 sub_459F40 的内部（0x78 / 0x2db 的尾跳目标）

- 要收口的残差：`RES-OPS-SET-TAIL-FIELDS`
- 所属规则：`CORE-OPS-LIST-SUBSYSTEM-SET-TAIL`
- 入口 EA：`0x459f40`（613 行 `.lst` / 253 行 C）

## 已经知道什么（带 EA）

- `0x78` / `0x2db` 各写字体对象的一格（`0x41f476` = `+0x55C` 绘制模式；`0x426526` = `+0x313D0` 对位模式），
  然后**尾跳**进 `0x459f40`（`0x41f47f` / `0x42652f`）。
- 两格的领域角色与初值已定：绘制模式初值 1（`0x465611`）、对位模式初值 0（`0x465617`）；
  `+0x313D0` 在尾跳目标体内被直接读（`0x459fa4 cmp ...,0`），`+0x55C` 由绘制路径读
  （`0x456279` / `0x45aebf` / `0x45afd0`）。
- 尾跳目标体还调用 `sub_459A20` / `sub_459C50` / `sub_456C90` / `sub_4745A0` / `sub_408050`
  （六个直接目标），并大量使用 `LOGFONTA` 的 `lfHeight` / `lfWidth` / `lfFaceName`
  —— 它是**按两份字体描述重建宿主字体句柄**的那一步。

## 还缺什么

- 重建入口**改哪些字体状态**、发哪些平台请求、返回什么（`0x78` / `0x2db` 只写一格，其余效果全在入口体内）。
- 绘制模式 = 3 时的径向 pass 参数（`sub_459F40` 的其它写点）是否也改字体状态。

## 判据（什么算做完）

1. 给出 `sub_459F40` 的**可观察效果表**：读哪些字体字段 / 描述、写哪些句柄或标志、发哪些平台请求；
2. 把「重建入口之后的过程属实现侧」这句**收窄到只剩平台细节**，不再把引擎侧写点算作"不规定"；
3. `0x78` / `0x2db` 的正文只引用该表，不复述平台细节。

## 取证入口

- `repo:data/spec/chapters/60-ops-list.md`（本条 + 残差）
- `repo:data/spec/bindings.json`（`CORE-OPS-LIST-SUBSYSTEM-SET-TAIL` 的站点）
- `node tools/disasm-fn.mjs show sub_459F40 --lst`
