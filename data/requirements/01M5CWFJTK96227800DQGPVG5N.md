# CORE-FS 成篇（取件 5 条）+ CORE-BOOT 章节骨架（残差显式）

- id: REQ-01M5CWFJTK96227800DQGPVG5N
- type: req
- status: open
- parent: REQ-01M5851Z51V1MTSWH414M7WN5Q
- tags: [engine spec, fs, boot]

## 一句话

S3（本轮的规范成熟度分析）判定：**CORE-FS 今天就能成篇**（取件语义在一个函数里逐支可读），**CORE-BOOT 只能成"最小篇 + 显式域外"**（12 个启动阶段里 7 个是宿主阶段，引擎侧没有可观察行为）。本单把这两件事拆成可验收的落点，锚清单取自 `.tmp/boot-ops/core-boot.md`（草稿，未入库）。

## 判据（做到这些才算完）

### CORE-FS：5 条 draft 规则（章 `data/spec/chapters/25-fs.md`）

| 规则 | 规定什么 |
|---|---|
| F1 统一 id 两个区 | 普通区 `[0, 条目数)`；扩展档 = 高 8 位档号 + 低 24 位档内号 |
| F2 松散优先 | 取件先看松散文件；成则**长度 = 实际文件大小** |
| F3 归档回退 | 档名表（256 × 档号）+ 定位 offset；**长度 = 登记长度** |
| F4 取件失败 | 三种原因（id 越界 / 松散与归档都打不开 / 扩展档问题）**一种表现**：报消息 + 中断，**不返回空** |
| F5 索引装载失败 | 四族签名 / 读失败 / 内存不足 ⇒ **返回**失败标志 |

- 机器绑定：`sub_4559C0` 逐支 EA（`0x4559ed` `0x4559f3` `0x455afc` `0x455b02` `0x455b08` `0x455b10` `0x455b31` `0x455b40` `0x455c0b` `0x455b4c` `0x455b53` `0x455b7c` `0x455bee` `0x455c04`；失败 `0x455c38` `0x455b9d` `0x455a10` `0x455a87`；throw `0x455c49` `0x455bd9` `0x455a4c` `0x455ac3`）与 `sub_414AC0`（`0x414b42` `0x414b5d` `0x414b71` `0x414b85` `0x414b96` `0x414ccb` `0x414cdb` `0x414d7a` `0x414bcb` `0x414bd2`）。
- `bindings.storage` **留空**：`data/shape/types.json` 里没有对应类型，⛔ 不许编。
- 证据：F1/F2 可挂 `guard:tools/test/emulator-new-alf.test.mjs#…`（`@env pure`）；F3/F4/F5 若无纯守卫 ⇒ 保持 `draft`（不许标 reviewed）。

### CORE-BOOT：章节骨架 + 残差（章 `data/spec/chapters/05-boot.md`）

- 只写有强锚的：`ENTRY`（`0x4bb517` 调用主循环）· `INDEX-FIRST`（`0x4bab1a`/`0x4bab26`，S3 回退 `0x4bab2f`/`0x4bab3b`）· `INDEX-FAIL`（`0x4bab44` `0x4bab4b` `0x4bab51` `0x4bab57`→`0x4bab7e`；尾声 `0x4bb5f6`）· `ROOT-SCRIPT`（`0x4bb4be` `0x4bb4c5` `0x4bb4cc` `0x4bb4d1`）· `ABORT-SHAPE`（`0x4bb096` `0x4bb0a8` `0x4bab7e` `0x4bb5f6`）。
- 三条残差必须显式：① 闸门判什么（`sub_418940` 未查明）② 主循环是否返回（`__noreturn` 只是 C 侧推导）③ 定时器回调缺锚。
- 宿主阶段（单实例 / COM / 窗口类 / 命令行 / 自更新 / 窗口创建 / 显示副作用）走 `audit` 的域外声明，⛔ 不写进正文、⛔ 不引 Win32 名。

### 两章同批落盘的三处配套

- `spec.json.chapters` += `fs` / `boot`；`audit.chapters` += 同 id（FS ⇒ `bootstrap`，BOOT ⇒ `bootstrap` 或有规则的 `partial`）；`audit.rules` += 每规则一条。
- `audit.planned` **删 `files`**（= FS + BOOT）：#12 有"planned 与已写章节重名 ⇒ 红"。
- 验收：`pnpm tools spec validate`（13/13）· `pnpm tools spec report` 条目数 +5/… 与草稿一致。

## 不做什么

- ⛔ 不把 `apps/emulator-new/src/boot.ts` 的 12 阶段抄成规范（那是**本实现的欠账清单**，7 个是宿主）。
- ⛔ 不为了"能 reviewed"去挂 assets 守卫：`reviewed` 要求至少一条 accepted 台账或 `@env pure` 守卫。
- ⛔ 不顺手改 `files` 之外的历史 plan 项。

## 复现 / 证据

- 分析稿：`.tmp/boot-ops/core-boot.md`（含逐节锚与"会失败的写法"两个反例）。
- 现状：`pnpm tools spec report` 的 `章节 16（已写 9 · 未成篇 7）`、`条目 56`。
