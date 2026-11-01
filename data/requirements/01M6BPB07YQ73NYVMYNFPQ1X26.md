# shape 度量修复：把「未声明 0」变成可判的欠账（零成员类型 · 未解析基址按来路分组 · 块搬移 extent · interior 进内嵌数组）

- id: REQ-01M6BPB07YQ73NYVMYNFPQ1X26
- type: req
- status: done
- parent: REQ-01M52F3ASMGWQXX64JSA18MP2N
- order: 53
- verify: tools/test/shape-claims-guards.test.mjs#★㉞ 类型欠账
- tags: [shape tooling]

## 一句话

本单**不补任何成员**，只修"为什么成员表会长期缺着、而所有守卫全绿"这件事。触发证据：`census --debt` 报
"未声明 0"，可只给三个访问器补载体，`T_FadeTimer` 就冒出 21 处残差（成员补齐单
`REQ-01M6AKCBTRNBZQNERZGPT8VMPD`）。⇒ **欠账表的分母是"有载体的观察集"，而观察集又由载体决定**：
没有载体的字段永远进不了欠账表 —— 这是自证式全绿。

## 四项（按可落地顺序）

1. **T1 类型可证伪性清单**：对 `data/shape/types.json` 的每个类型报
   `{成员数, size, 载体数, 被引用次数, 观察档站点}`；把**零成员**与**成员跨度 < size** 的单列。
   这是 G7 的推广（G7 只管 `kind:"ptr"`）。存量第一个要能点名 `T_FadeTimer`（0 成员 / size 28）、
   `T_FileDB_424_10`、`T_Sound_690`。
2. **T2 未解析基址按"来路"分组**：现在只按基址寄存器分组（`eax×…` / `edi×…`），不可派活。改成五档
   —— `call` 返回值 / 栈出参 / 表查询结果 / `interior` 落进内嵌数组 / 参数未声明（另加间接调用目标未知），
   每档给样例 EA 与调用方。⛔ 只分来路，**不判类型**（"按偏移命中率猜类"仍是被否掉的做法）。
3. **T3 块搬移 / 清零登记 extent**：`rep movsd N` / `memcpy` / `memset` 的站点产出
   `{基址, 字节数, EA}`，并判它是否 obj0 相对。它不解成员，但给"类型大小 / 子块宽度"作**第二证据** ——
   这正是 `0x41edfc` 的 `0x4B0` 字节与 `0x41ee06` 的 20000 字节现在唯一有的证据。
4. **T4 `interior` 进内嵌数组元素**：把 `propagate` 的 `interior(obj+k)` 从"只在 `count === 1` 时换类"
   扩到"`k` 落进 `{type, count:N}` 的数组窗口且 `(k − off) % size == 0` ⇒ 元素类型"。现在这一档解不出，
   所以计时器访问器（29 个调用点）全无载体。守卫：合成用例 + 真语料上 `propagate` 必须把它们推成
   `T_FadeTimer`。

## 判据（做完的样子）

1. T1：`pnpm tools shape debt --types` 能跑，输出含上述四列与两条单列；`T_FadeTimer` 被点名。
   **不判红、不改 exit code**（否则会逼出"为了变绿编成员"）。
2. T2：`shape census` / `shape claims` 的未解析基址按五档报数、每档带样例 EA；总数与守恒③ 相等
   （不新增桶，也不把已有桶改名后重复计数）。
3. T3：对 `0x41edfc`（`0x4B0` 字节）与 `0x41ee06`（20000 字节）能报出宽度与基址归属；对
   `0x4AC120` 那类 740 字节副本同法。
4. T4：`propagate` 的 `conflicts` 不新增；新增推断能追到调用点 EA（`--json` 的 `via`）。
5. 四条各有守卫：T1/T2 用 `pure`（直接喂 `types` + `carriers`）；T3/T4 用 `assets`（真语料）。
   `pnpm tools mutate check --match shape` 每个至少一条"改坏就红"。

## 不做什么

- ⛔ 不放宽 G1 / G4 让存量变绿；⛔ 不按偏移命中率猜类；⛔ 不动 `data/spec/**`。
- ⛔ 不把 T1 写成"零成员必须清零"的硬红 —— 那会制造假声明（本仓最严重的错）。
- ⛔ 不把 T4 做成"任意 interior 都换类"：必须落在已声明数组窗口内，否则只记 note。

## 相邻单

- 成员补齐（手工，5 项）→ `REQ-01M6AKCBTRNBZQNERZGPT8VMPD`。
