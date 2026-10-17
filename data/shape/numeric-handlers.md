# `numeric-handlers.json` —— **数值计算指令 handler 族**的载体表

> 同名数据文件的说明书。字段 / 义务 / 守卫的真源是 `pnpm tools shape describe`（本文件不复述 schema）。
> 本文件**只放载体**（机器槽 → 类型）；类型在中心真源 `types.json` 里，走 `typesFrom` 引用。

## 它声明什么

"这一族 handler 的**对象根寄存器**装的是哪个类型的对象"。每条载体 = `{ id, kind:"arg", fn:<handler 符号>, reg:"ecx", type:"T0*" }`，
其中 `type` 指向 `types.json`。⇒ 判定器把该函数的**逐 site 访问记录**（`pnpm tools shape access <fn>` 的同一份观察）
投到那张成员表上，给出**解释 / 残差**，并检查 G1..G8、J3 与守恒①②③。

**覆盖范围（判据，不是名录）**：opcode `0x0050..0x0060` 的 handler，外加 `0x0191` 与 `0x02d0`
（后两个与前者**体形相同**：读 `obj0` 的一个标量、把立即数写进记录数组的同一个元素内偏移）。
opcode ↔ handler 符号的映射**现查**：`pnpm tools opcodes handlers`（⛔ 那个映射属于"桥"，不写进本 JSON）。
★ 同一体形的 handler **远多于**这一段 opcode（全表口径见需求单 `REQ-01M530BB8BY4XVP32E7Z2E6W5N`）——
本文件的边界是"被指令表**命名**为算数 / 比较 / 随机的那一段"，不是"所有体形相同的函数"。

## 非显然口径

* **载体只放机器槽**：字段不是载体 —— 成员写在 `types.json` 里。把同一个成员写成"字段载体 + 类型成员"
  = 同一件事两份写法 ⇒ 必漂。
* **`reg` 必须与 `.c` 的调用约定一致**：判定器从 `.c` 侧读"对象根在哪个寄存器"，
  查不到对象根的函数 ⇒ 载体**无法检查**（红，⛔ 不许当绿）。
* **残差是清单，不是失败**：解释不了的访问如实报出来（带 EA）—— 那正是下一轮要做的。
  本族里"数据段符号"（`ds:` 前缀的进程全局，没有"对象根 + 位移"形状）就是一例：形状层**表达不了**它，
  判定器把它单列成独立一桶（守恒③ 的一个加数），⛔ 不是"已通过"。
* **同形不等于独立证据**：这一族里多数函数彼此逐字同形 ⇒ 站点的**独立信息量**远小于站点数。
  别拿它当"覆盖得很广"的证据。

## 怎么查

```bash
pnpm tools shape describe                                  # 义务 / 守卫 / 诚实项（先看这个）
pnpm tools shape claims data/shape/numeric-handlers.json   # 本族的 解释 / 残差 / 守恒①②③（退出码 0 = 绿）
pnpm tools shape access sub_42C5E0                         # 逐 site 访问记录（EA 锚）
pnpm tools shape verify 0x42C5F1                           # ★ 独立复核器：按 EA 回语料重解（不信转述）
pnpm tools opcodes handlers                                # opcode ↔ handler（"桥"，现查）
```

## 怎么改

1. **加一个载体**：先 `pnpm tools shape access <handler>` 拿观察，确认它的对象根寄存器（`.c` 侧约定）
   与 `type`，再往 `carriers` 里加一条**中性编号**的载体（⛔ 不写助记符 / opcode / EA / 行号）。
2. **需要新成员时改的是 `types.json`**（类型是中心数据），⛔ 不要在本文件里另立一份类型。
3. **改完必须跑** `claims data/shape/numeric-handlers.json` 且退出码 0；改不动就又红 ⇒
   分清"我错 / 工具错"（工具错 ⇒ 先跑 `claims --sample` 与 `mutate check --match shape`）。
4. **不许为准而编成员**：判不了就报残差。若某条观察形状层表达不了（例如 `ds:` 符号），
   把它留在桶里并说清为什么 —— 这一条比"看起来全绿"重要得多。
