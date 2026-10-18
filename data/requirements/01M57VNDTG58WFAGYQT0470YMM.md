# 批 S1m · 值身份缺口：数组元素内的字段（帧记录 +0 的指针）当基址 ⇒ 要么扩模型、要么如实剔出清单

- id: REQ-01M57VNDTG58WFAGYQT0470YMM
- type: req
- status: open
- parent: REQ-01M52F3ASMGWQXX64JSA18MP2N
- order: 54
- tags: [engine shape]

## 目标（能力缺口，登记用；⛔ 不是"再补一批声明"）

让**"数组元素内的字段"**能当**值来源 / 基址** —— 现在它一律掉进"基址未解析"（`unresolvedBase`），
而它其实**观察得出来**，只是形状层没有表达它的位置。

## 实测形态（一个函数里最典型，四条同一类）

```asm
.text:00420402  mov  ecx, [esi+5D880h]          ; cur（帧索引）
.text:0042040A  shl  edx, 4
.text:0042040D  sub  edx, ecx                   ; edx = 15·cur
.text:0042040F  mov  ecx, [esi+edx*8+5D894h]    ; ecx = T1[cur] 的 dword 0 —— 一个指针
.text:00420416  lea  edx, [ecx+eax*4]           ; ecx 当基址 ⇒ 助手给不出身份 ⇒ ⚠ base
```

同族：`sub_41A9B0`（`0x41aa0b`）· `sub_41AB80`（`0x41ac45`）· `sub_4203D0`（`0x420416`）· `sub_420560`（`0x420605`）。

## 为什么现在表达不了

* 载体的 `pointeePath` 是**成员偏移链**（`[0x5d894]` = "从 `obj0+0x5d894` 读出的指针"），
  而这里读出的是 **`T1[cur]` 这个元素**的 dword 0 —— 身份里必须带**元素下标**；
* 助手也如实拒绝猜测（`evalOperand` 要求 `!indexSym` 才给 `load` 身份）⇒ 值变 `UNK` ⇒ 下游 `lea` 基址未解析。
  ★ 这是**诚实**的，不是 bug —— 但它是"表达力"缺口，不是"没观察"。

## 两条可能的路（二选一，要裁决，不许两条都上）

1. **扩模型**：让指针身份能带"数组元素内字段"（例如 `pointeePath` 允许 `{member, indexSym, stride, elemOff}`），
   判定器用元素类型跑一遍成员查找（与 G2 第二级同源）。收益：能判；代价：模型/判定器复杂度。
2. **局部类型载体**（现有机制）：在这几个块里断言"`ecx` 装的是某类型对象"。
   前提是**先知道它指向什么** —— 而这一点没有语料证据（读出的指针随后被 `lea` 加偏移存回记录里）。

## 判据（收口时）

* 选 ① ⇒ 那 4 个函数从"基址未解析"变"已解释"，且守恒③ 分桶不变（不许多一个桶）；
  守卫：合成 `.lst` 里放上面那 4 条指令 ⇒ 判出元素内字段的指针；变异一条。
* 选 ② ⇒ 每个断言都要 `regions` 的**活窗口**逐字引用 + witness EA，且 `calls` 不出现 `mismatch`。
* 两条路都走不通 ⇒ 如实登记为"本层表达不了"，并从"下一轮该补"的清单里**显式剔除**（⛔ 不许每隔一轮重新发现一次）。

## 怎么查

```bash
pnpm tools shape coverage sub_4203D0        # 看 ⚠ base 那两行
pnpm tools shape regions sub_4203D0 ecx     # 若走局部载体：段落 / 活窗口
pnpm tools shape access  sub_4203D0         # 逐站点（带 fold 说明）
```
