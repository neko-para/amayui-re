# 批 S2i · T_Sys3Draw/T_DrawEffect 成员块（残差 1838→17）+ sub_499220 真因：0x1fac/0x1fb0 类型写反

- id: REQ-01M55FAGJRZ9XRJBW87QC9792M
- type: req
- status: done
- parent: REQ-01M54KEF7X560CMPMGABG2W3CW
- verify: tools/test/shape-claims-guards.test.mjs#★㉑
- tags: [engine shape]

# S2i · `T_Sys3Draw`/`T_DrawEffect` 成员块（+ `sub_499220` 冲突的真因：两个槽的类型**写反了**）

三个大函数（`sub_447810` 955 站 / `sub_441E10` 562 / `sub_444350` 556）的 `this` 都是 `T_Sys3Draw*`，
而 `T_Sys3Draw` 原来只有 **8 个成员** ⇒ 1838 条残差。本单把成员块补齐，并把 `sub_499220` 的冲突查清。

## 结果

- 新增成员 **63**：`T_Sys3Draw` **51 新 + 2 修正**（现 59 成员）· 新增 **`T_DrawEffect` 12 条**（`adapter:true`）。
- 55 个 obj0 偏移**全部有声明**，宽度 **55/55 = 4B**（零窄读、0 处猜）。
- 载体无新增（`c94/c95/c96` 已在上一批）；**残差从 1838 降到 17**：
  `c94` 800 站 = 解释 800 · 残差 7 · `c95` 545/545/9 · `c96` 516/516/1。

## 2 处**声明修正**（原声明写反了，有 EA 链）

| 槽 | 原 | 改 | 依据 |
|---|---|---|---|
| `T_Sys3Draw+0x1fac` | `ptr→T_Scene` | **`ptr→T_Renderer`** | `Command+0x4E824` 由 `sub_497810` 构造（`0x497843` 写 `??_7Renderer@@6B@`）⇒ `0x40e104 lea eax,[esi+4E824h]` → `0x40e112 call sub_44A9A0` → setter `0x44a9a9` 写 `[this+0x1fac]` |
| `T_Sys3Draw+0x1fb0` | `ptr→null` | **`ptr→T_Scene`** | `Command+0x4ED10` 由 `sub_4AAF90` 构造（`0x4aafcd` 写 `??_7Scene@@6B@`）⇒ `0x40e0c8 lea edi,[esi+4ED10h]` → 同一次 `sub_44A9A0` → `0x44a9af` 写 `[this+0x1fb0]` |

★ 修正后 `--propagate` **冲突 8 → 1**（`sub_499220` 的 6 处全消），可推导函数 384 → **390**。
⇒ `Scene` **不**在 +0x0 继承 `Renderer`（4 条反证）：Scene 构造器调用闭集 `{sub_4034A0, sub_49A690, sub_49DD70}`
里没有 Renderer 构造器 `sub_497810`；两张 vtable 的写入点集合不相交（`??_7Renderer` 只有 0x497843/0x4986e9，
`??_7Scene` 只有 0x4aafcd/0x4ab3bd）；Command 里是**两个并列内嵌子对象** `+0x4E824` 与 `+0x4ED10`。

## 继承链（新证据）：`BaseObject → Draw → DrawEffect → Sys3Draw`，全在 **+0x0**

每级构造器 `mov esi,ecx` 后**不改 ecx** 直接 `call` 上级，然后在**同一个 `[esi]`** 覆盖自己的 vtable
（`0x43aab4` Draw · `0x44a823` DrawEffect · `0x4b7d93` Sys3Draw），全链一处 `add ecx,K` 都没有。

## 工具新增一档：**基址/索引分不清**（★㉑ + 变异）

`[ecx+eax]` 两个寄存器都是"从内存读出来的值"时（`0x448069 cmp byte ptr [ecx+eax],0`：`ecx=[esi+0x204c]`、
`eax=[esi+0x2030]`），谁是指针、谁是游标**判不了** ⇒ 单列一档（守恒② 的等式把它算进去）。
⛔ 反事实实测：把 `0x204c` 写成 `kind:"ptr"` 会让 12 条红全消 —— 那是**假绿**，所以拒绝。

## 诚实项

- **34 个偏移没有构造器证据**（两个构造器都没写）⇒ 归属"未定"，暂挂 `T_Sys3Draw` 只为让访问被解释；
  其中 30 个落在 DrawEffect 的写窗口 `[0x1FAC,0x2070]`、4 个落在 Draw 的写窗口。
- `0x1f7c` 的证据指向 **`Draw`**（`0x43abbb`），不在本批两个类型里。
- 10 个 ptr 是 `type:null`（元素类型零观测）；数组 count 未观察 ⇒ 只声明 ptr。
- 容器/基类字段的**归属**由"谁构造/初始化它"决定，⛔ 不是凭偏移范围猜。

## 复现

`pnpm tools shape claims data/shape/carriers.json`（c94 800/800 · c95 545/545 · c96 516/516）·
`pnpm tools shape propagate`（390 可推导 · 冲突 1）· `pnpm test`（398）·
`pnpm tools mutate check --match 基址/索引分不清`。
