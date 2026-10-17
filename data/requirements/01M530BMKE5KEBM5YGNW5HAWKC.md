# 批 S1e · handler 体的 A 层义务 A1–A6 落在哪里（判定器 or 另开一层）

- id: REQ-01M530BMKE5KEBM5YGNW5HAWKC
- type: req
- status: done
- parent: REQ-01M52F3ASMGWQXX64JSA18MP2N
- order: 45
- verify: tools/test/shape-bodies.assets.test.mjs#② 调用守恒：每个直接调用都落桶
- tags: [engine shape tooling]

## 裁决（已落）：**A1–A6 不做成义务；改成算出来的普查**

原表六条（记录槽立即数 / 调用序列 / 位置性观察 / 算术与来源 / 跨视图 / 守恒）**逐条审计**：

| 原项 | 结论 | 去向 |
|---|---|---|
| A1 记录槽立即数 | 声明的 (EA, 立即数, 位移) **全都能从 EA + 语料机械读出** ⇒ 进产物就是第二份真源，必然与语料漂 | 算出来：`bodies.recordWrites` |
| A2 调用序列 | 同上；"同一 callee 的实参立即数集合"本来就是**分组键**，不是声明 | 算出来：`bodies.calls` |
| A3 位置性观察 | 它本来就是**口径禁令**（"只观察立即数 1，别读成第 1 个操作数"），不是可判定的声明 | 留作口径；`calls` 里照样数得出 `(1)` |
| A4 算术与来源 | 左右序**由寄存器流定**（不需要声明）；两个来源是哪次调用的返回值也是观察 | 算出来（进骨架签名） |
| A5 跨视图 | **不可机械化**（是"怎么对待 `.c`"的纪律），且实质已由 `shape-census.md` §2 判据 1–3（两个独立实现）覆盖 | **删除**（§2 已经管了） |
| A6 守恒 | 纯守恒关系 | 算出来：`bodies.conservation`（会红） |

**落地**：`pnpm tools shape bodies`（`tools/lib/shape-bodies.mjs`）按"记录槽写法 + 调用序列完全相同"分组。
A 层唯一真正需要**判断**的东西因此剩下一个：**族边界**（看分组，而不是拍）。

## 判据（可执行）

* `tools/test/shape-bodies.assets.test.mjs` 四条：**分组穷尽**（Σ 组大小 == 普查数，且一个 handler 只在一组）·
  **调用守恒**（每个直接调用落桶：有立即数实参 / 无 / 尾跳）· **立即数真解得出**（⛔ 不许 `←NaN`）·
  `--match` 收窄但底数仍报全表口径。
* `pnpm tools mutate check --match shape` 里没有 bodies 的变异 —— 它守的是**闭合关系**（恒等式），
  变异源在 claims 侧已有；bodies 的 bug 会直接让等式差数（实测：正则少一个捕获组 ⇒ 全部 `←NaN`，第③条当场红）。

## 没做什么（诚实项）

* **不做**"某个 opcode 取几个操作数 / 算什么" —— 那是 B 层（台账，挂 EA 过准入门）。
* **不做**语义命名（"立即数 1 是写回第一个操作数"）—— 只在口径里禁掉这种读法。
