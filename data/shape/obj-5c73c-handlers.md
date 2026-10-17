# `obj-5c73c-handlers.json` —— **局部类型载体**族：某些 handler 的某个寄存器在活窗口里是 `T_5c73c_0*`

> 同名数据文件的说明书。字段 / 义务 / 守卫的真源是 `pnpm tools shape describe`（本文件不复述 schema）。
> 本文件的载体**全是 `kind:"local"`**（对象根载体在别的族文件里）—— 类型仍引用中心真源 `types.json`。

## 它声明什么

"在这个函数的这一段里，`edi` 装的是 `T_5c73c_0` 的对象"：

```jsonc
{ "id": "l01", "kind": "local", "fn": "sub_421450", "reg": "edi",
  "span": ["0x421544", "0x4215ca"], "type": "T_5c73c_0*" }
```

判定器（G10）拿 `span` 内的、以 `edi` 为基址的站点，逐个落到 `T_5c73c_0` 的成员表上。

## 为什么是"局部载体"而不是"路径"

* **路径**（`{off, kind:"ptr", type}`）表达的是"**从 `obj0` 的某个成员读出来的指针**"——
  零声明、跟着对象图走，但只有当那一段指针来源可表达时才用得上，而且**受下钻上限约束**（层级 2）。
* **局部载体**表达的是"**这个寄存器在这一段里是 `T*`**"——一句话断言，不依赖来源，
  因此它**跨得过层级上限**（本文件里被解释的两处站点，层级是 3）。
* ⇒ 口径：**来源可表达就用路径**（它是推导，不是断言）；**来源不可表达就用局部载体**
  （参数 / 调用返回值 / 数据段 / 跨层级）。两者**不许对同一处静默并存**——G10 会报一条 note，
  提示"同一事实有两套声明，建议只留一套"。

## 非显然口径（三条硬要求，都是 G10 的判据）

1. **`span` 必须逐字等于 `regions` 导出的"活窗口"** `[from..liveTo]`（⛔ 不许自己写）。
   `liveTo` = 本段第一处写点之后、**下一次写这个寄存器之前**的最后一条指令 ——
   所以它比 `regions` 打印的 `[from..to]`（只框住写点）**更长**，这是有意的：
   寄存器在最后一次写点之后仍然持有那个值（实测：`edi` 段 `[0x421544..0x421576]`，而用法在 `0x421588`）。
2. **活窗口里被 `call` 打断的 caller-saved 寄存器不许用**：`eax/ecx/edx` 跨 `call` 之后值可能已被抹掉，
   "它是 `T*`"这个断言不成立 ⇒ 红。`esi/edi/ebx` 等 callee-saved 不受这条限制。
3. **段落类别必须是"值来自内存 / 取址"**（`load` / `ptr` / `other`）；`const` / `init` 段说明那是个常量，
   ⛔ 不能断言它是指向某个类型的指针。

## 怎么查

```bash
pnpm tools shape describe                                        # G10 的判据 / 诚实项
pnpm tools shape regions sub_421450 edi                          # 活窗口的真源（[from..liveTo]）
pnpm tools shape claims data/shape/obj-5c73c-handlers.json        # 本族：局部桶的解释 / 残差 + 守恒③
pnpm tools shape access sub_421450                                # 逐 site（看 baseReg / baseOff）
```

## 怎么改

1. **先跑 `regions`** 拿活窗口，⛔ 不要手写 `span`；寄存器名与 `.c` 约定无关（局部载体不要求是对象根）。
2. **类型在 `types.json`**：目标类型的成员只能写**被观察到的偏移**（`pnpm tools shape access` 的 `baseOff`）；
   给不出就 `type: null`（不透明桶，⛔ 不算通过）。
3. **改完跑** `claims` 该族文件 + `claims data/shape/types.json`（类型共享）。
4. **别为了变绿扩大 span**：跨 `call` 的 caller-saved 断言**不许**用"把 span 切短"来规避——
   切短到没有 `call` 是合法的（说明那个值确实只在调用前有效），但要把这一判断写清楚。
