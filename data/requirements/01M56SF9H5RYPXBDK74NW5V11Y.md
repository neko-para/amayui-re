# 批 S2x · claims/access 的四个工具缺口：lea [r+0] 当访问 · 算术标量当 memberptr · G9 误判必需 views · （④ findings 段是误读）

- id: REQ-01M56SF9H5RYPXBDK74NW5V11Y
- type: req
- status: done
- parent: REQ-01M54KEF7X560CMPMGABG2W3CW
- done_reason: 四项收口：①lea [r+0] 当访问已修（★㉘+变异，T_Font+0x0 的 4 次填充不再计入）；③G9「多余」加对齐条件（stride*d % elemSize==0），仍判多余的只剩 T_Font+0x18f28/0x35618（真多余）；②算术标量归 memberptr 的机制核对为不可达（bv.t==='val' ⇒ problems.push('base') ⇒ baseKind='unresolved'），且全量实测 --by-type 0 处残差、T_Font 各载体残差 0，症状不可复现 ⇒ 以 --by-type 全量检查为判据；④findings 段不打印是误读，不做
- tags: [engine, shape]

# S2x · claims/access 的四个工具缺口（收口记录）

## ④ 「findings 段不打印」是**误读** —— 不做

`renderReport` 确实打印 findings（输出里有 `❌ N 条红` 段）。子代理当时读的是裁剪过的输出。**无需修**。

## ① `lea reg,[reg+0]` 当访问 —— 已修

`siteOf` 里对"同寄存器、无索引、位移 0"的 `lea` 返回 `null`（MSVC 的对齐填充不是访问）。
实测收益：`T_Font+0x0` 的 5 次里有 4 次是填充（`0x46547a`/`0x4600da`/`0x4606ea`/`0x460dfa`），修后不再计入。
守卫：`★㉘`（`tools/test/shape-claims-guards.test.mjs`）+ 变异 `lea [r+0] 当访问`。

## ③ G9 把必需 `views` 判成"多余" —— 已修

"多余"的判据原先只看"视图窗口被数组罩住"。但视图的语义是「同一数组、索引差 k」：
若 `stride*k` **不是** `elemSize` 的整数倍，它把字节**重新分组**，数组自己的元素边界罩不住它 ⇒ 不是多余。
修法（`tools/lib/shape-claims.mjs`）：`coveredByZero` 追加 `aligned = (stride*d) % elemSize === 0`。
实测：仍判"多余"的只剩 `T_Font+0x18f28` / `T_Font+0x35618`（索引差 1、对齐 ⇒ 真多余）；
`T_Command+0x5d894` 的 `elementDelta:-1`（必需）不受影响。

## ② 算术标量被归 `memberptr` —— **症状已不可复现**（按实测收口）

- 机制核对：`shapeOf` 里寄存器值是整数（`bv.t === 'val'`）时走 `else` 分支 ⇒ `problems.push('base')` 且 `obj` 为空；
  `siteOf` 的 `baseKind` 于是算成 `'unresolved'`（`!sh.obj`），**不是** `memberptr`（`memberptr` 只在 `bv.t === 'load'` 时出现，
  那正是"从成员里读出一个指针再当基址"的正确归类）。
- 实测核对：`pnpm tools shape claims data/shape/carriers.json --by-type` ⇒ **0 处残差**；
  逐个 `T_Font` 载体的行现状（现跑）：
  ```
  c83 sub_428990 解释 2 · 残差 0    c52 sub_455D20 解释 1 · 残差 0
  c54 sub_456C90 解释 23 · 残差 0   c82 sub_456DF0 解释 11 · 残差 0
  ```
  原先的样例偏移 `T_Font+0x574` 已不再出现在任何残差/named 项里。

⇒ **收口方式**：不做代码改动（现状已正确），把"可复现症状"作为判据 —— 若这个缺口再出现，
`--by-type` 会重新列出对应类型/偏移、`claims` 退出码或守恒等式会变红。这比"再写一条合成用例"更有约束力，
因为它是**全量语料**级别的检查，不是单点构造。

## 复现（本条的全部证据都可重跑）

```powershell
pnpm tools shape claims data/shape/carriers.json --by-type | rg "残差按类型|T_Font"
pnpm tools shape claims data/shape/carriers.json | rg "G9|多余"
```
