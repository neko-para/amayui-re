# emulator-new：补**指针池**并把 `0x61 lookup-array`（及其 2d/64 兄弟）落地

- id: REQ-01M5F8JE55PWCDAJM941GFV7FV
- type: req
- status: open
- parent: REQ-01M3TCB0S0BTW41KW0JBJMWHHH
- tags: [emulator, frame, pools]

## 一句话

启动链现在停在 `INITCONFIG4.BIN` 的 `0x61 lookup-array`（第 140 条 · 偏移 164）。这一条**不是"再补一个 handler"**：它的落点是**指针操作数**（`type 0xc` / `0xe` 族），而 emulator-new 目前只有值池（整型 / 浮点 / 字符串），**没有指针池**。本单把这块能力补上。

## 为什么它挡住启动链

`0x61 lookup-array`（argc 3）的语义（旧实现 + 台账 `01M4ASXQ587J7G7E6E5R3R2Y2K` ⑤）：

```
dest = op1（指针族：写"地址"）
base = 取址(op2)             ; 数组首格的地址
idx  = 读整(op3)
写 dest ← base + 4*idx       ; ★ 写的是**地址**，不是那一格的值
```

`INITCONFIG4` 用它把刚用 `0x6c`/`0x2d8` 填好的两张表（全局 int 池 15182 / 43493 起各 1000 格）的**元素地址**算进局部指针，随后逐格读写。⇒ 没有指针池，这条只能停在第 140 条。

## 要做什么

1. **指针池**（每帧局部 + 全局）进 `Machine`：`Map<number, number>`（值是"地址"）；地址口径按 spec 的取址原语（池 + 槽号）。
2. `0x61 lookup-array` / `0x12c lookup-array-2d` / `0x64 copy-local-array`：地址算术 + 越界口径（台账 ⑤：**越界不检查**，⛔ 不许私自加边界）。
3. **写指针操作数**的原语（`writeAddress`）：把算出的地址写进 dest 的池格；不支持的类型响亮失败。
4. 守卫：`0x61` 的地址算术（`base + 4*idx`）、二维索引（`op3*op4 + op5`，乘在前）、`0x64` 的逐 dword 线性拷贝。
5. 变异：步长写成 1 / 二维索引写成加 / 越界被 clamp。

## 判据

- `pnpm test` 全绿且新守卫会红（变异逐条 1/1）；
- 启动链棘轮从第 140 条继续往后走（`emulator-new-boot.assets.test.mjs` 同步抬高）。
