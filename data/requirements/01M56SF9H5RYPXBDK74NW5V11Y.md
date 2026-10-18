# 批 S2x · claims/access 的四个工具缺口：lea [r+0] 当访问 · 算术标量当 memberptr · G9 误判必需 views · （④ findings 段是误读）

- id: REQ-01M56SF9H5RYPXBDK74NW5V11Y
- type: req
- status: open
- parent: REQ-01M54KEF7X560CMPMGABG2W3CW
- tags: [engine, shape]

# S2x · claims/access 的工具缺口（现场撞出来的，都会**制造假残差或假 ptr**）

## ① `lea reg,[reg+0]` 被当成一次访问 —— **已修**

* 现象：`T_Font` 残差里 4 处全是 `lea ebx,[ebx+0]`（MSVC 的 6 字节对齐填充），`shape access` 报成 `obj0+0x0` 的访问
  （实测 `0x46547A lea ebx,[ebx+0]`）。
* 代价：为了让残差归零，唯一的写法是声明 `{off:"0x0",width:4}` —— 那是**假成员**（子代理拒绝了，正确）。
* 修法（`tools/lib/shape-access.mjs` · `siteOf`）：`lea` 且 **目标寄存器 == 基址寄存器、无索引、位移 0**
  ⇒ 返回 `null`（不是访问）。`lea r,[r+K]`（K≠0，内嵌子对象基址）与 `lea r,[s+0]`（换寄存器）都仍然算。
* 效果：站点 8370 → 8366（−4），残差 478 → 474；`T_Font` 的 `0x0` 从热点里消失。
* 守卫：`★㉘` + 变异 `lea r,[r+0]（6 字节对齐填充）又被当成一次访问`（114 → 115 条）。

## ② "值只进算术"的标量被归成 `memberptr` —— **未修**

* 现象：`T_Font+0x574` 的站点 `base=memberptr`，但真基址是 `[ecx+0x2C]`，`0x574` 只贡献 `×24` 的索引。
* 代价：会诱导后续批次把它写成 `kind:"ptr"`（假声明换假绿）。
* 要立的判据：只有当"这个槽里的值**直接**作为下一次内存访问的基址"时才是 `memberptr`；
  参与了乘加后作为索引/偏移用的，应记 `其他/索引`。
* 卡在哪：这不是"改一行"能收的 —— 要在数据流里区分"`load` 出来的值被当基址"与"被当索引项"，
  而当前 `shapeOf`/`foldNote` 只记"最后一次 load 的来源"。需要一份能用合成 `.lst` 复现的对照
  （至少三种形态：直接基址 / `imul` 后当索引 / `add` 常量后当基址）。

## ③ G9 把**必需的** views 误标"多余" —— **未修**

* 现象：`T_Font+0x18f28` 的 `views[{elementDelta:1}]` 被判 `redundant`（"字节范围已被 delta 0 覆盖"）；
  去掉它，`T_Font` 残差 4 → 9（多出"元素内偏移 0x4 不是 T_el32 的成员"）。
* 原因：`coveredByZero` 只算**字节覆盖**，没算"带索引的站点落在元素内偏移 4、而元素只有 4 字节"。
* 要立的判据：`redundant` 应当要求"**所有**观察都能被 delta 0 解释"，而不只是"范围重叠"。

## ④ `renderReport` 的 findings 段（**子代理报告里的说法不成立**）

* 有子代理称"`renderReport` 不打印 findings，退出码 1 却看不到原因"。
* 实测（`.tmp/shape/batch-13/font/model-before.json`）：末尾打印了 `❌ 1 条红：` + `- G14 T_RemoteDebug：…`，
  随后 `ELIFECYCLE exit code 1`。
* ⇒ 这是**误读**（大概是 `Out-File` / 管道只截了尾部），不是缺口。⛔ 不要再"修"它。

## 附：同一批里"冻结快照"的坑（已修）

`batch-13/_input/types.json` 是**修 `T_RemoteDebug.note` 之前**的快照 ⇒ 凡是用它建的模型都带 1 条 G14 红。
三个子代理都独立撞到了。**下次冻结输入前先把红修完**（或把"已知红清单"写进 BRIEF）。

## 复现

`pnpm tools shape access sub_465390`（①：`0x46547a` 现在不再出现）·
`pnpm tools shape access sub_45B2B0`（②的 `base=memberptr` 形态）·
`pnpm tools shape claims .tmp/shape/batch-13/font/model-before.json`（④ 与 G14 红）。
