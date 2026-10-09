# 注释/守卫与二进制不一致的一批（bad opcode:0 · 重掷 12 次 · 0x192 读两个操作数 · int 配置对象 0xAA514 · sub_41BF50 无 case 14 · EVIDENCE/需求单 指针落空）+ 四处守卫欠账

- id: REQ-01M4FEF7VH1N0JDT5ZVPZS204M
- type: req
- status: open
- parent: REQ-01M4AGPP1T9A60HQYW3GX9W592
- tags: [emulator, ledger, guard]

# 注释/守卫与二进制不一致的一批（bad opcode:0 · 重掷 12 次 · 0x192 读两个操作数 · int 配置对象 0xAA514 · sub_41BF50 无 case 14 · EVIDENCE/需求单 指针落空）+ 四处守卫欠账

- id: REQ-01M4FEF7VH1N0JDT5ZVPZS204M
- type: req
- status: open
- parent: REQ-01M4AGPP1T9A60HQYW3GX9W592
- tags: [emulator, ledger, guard]

## 背景

本轮把 `apps/emulator/**` 注释里的"设计取舍"迁进知识台账（域 `Emulator`，46 条）并对注释里的
**引擎结论**做了独立取证（5 路只读取证包）。取证**逐条核过**下面这些"注释/守卫说的事实与二进制不符"。
每条都给了 EA 或 `.lst` 行号（可直接复跑）。

## 逐条（都在本轮已记为台账条目，这里只列**要改的地方**）

| # | 现场 | 二进制说 | 处置 |
|---|---|---|---|
| 1 | `tools/test/emulator-iterate.test.mjs` 的用例名与消息把「`opcode == 0` ⇒ 引擎报 `bad opcode : 0`」当事实 | 派发表 **1024 项全部预填** `sub_418E30`（`.lst:34077-34079 mov ecx,400h` / `rep stosd`）；opcode 0 直接查表 ⇒ 落到默认 handler ⇒ 抛 `このコマンドはサポートされていません．`（`0x10001`，`.lst:37038-37045`）。全语料/`.c`/原始字节里**没有** `bad opcode` 这个串 | 改**用例名与消息**（别再复述一句不存在的台词）；★ 该断言现在是**恒真**的，要换成"能让它红的判据" |
| 2 | `vm/ops.ts` 的 `0x60 random` 注释 + 台账 `01M48H8Y912C4D035W1P5W443F`：「体内有上限 12 的**重掷**计数器」「什么条件下重掷没有取证」 | `sub_42CA50` **没有回跳重掷循环**，只有一次 `call _rand`；`[esi+69330h]` 是**进入次数**计数器（`inc` / `cmp …,0Ch` / `jle` / 归零），第 13 次进入归零（`.lst:68761-68768`） | 改注释措辞；台账那条按"追加更正记录"处理 |
| 3 | `vm/ops.ts` 的 `0x192`：「用 `sub_42A420(this, v3, 2)` **一次把两个操作数都读成文本**」 | `sub_42A420` 的**第 3 个参数是操作数序号**，只读**一个**（`.lst:65076/65081/65083`）；真正"一次读两个"的是 `0x194`（`sub_42CF10`，`.lst:69309/69318`） | 改注释；并补 `sub_433310` 的 **`0x8005`/`0x800B` 分支**（不抛，`.lst:79960/79962`）与 `sub_418AE0` **不支持 type 11**（会抛，`.lst:36720-36724`）这两条口径（`ops.ts` 现在允许 `5/0xb` 是**过宽**） |
| 4 | 台账 `01M4ASXQ587J7G7E6E5R3R2Y2K` 说 int 配置对象是 `Engine+0xAA514` | 两个调用点逐字 `lea ecx,[esi+5530h]` ⇒ int 配置哈希表 = **`Engine+0x5530`**（`.lst:71000` / `.lst:83032`）；`AA514h` 是**另一个**带 vtable 的子系统对象（`.lst:8650-8654`） | 追加更正记录（字符串侧的 `0x511C` 已订正，int 侧没有） |
| 5 | `vm/operand.ts:154` 把字符串指针的逐字出处写成「`sub_41BF50` 的 case 14」 | **`sub_41BF50` 没有 case 14**：`.lst:41465 cmp edx,0Dh` / `ja def_41BF99` ⇒ `0xe` 走 default 抛 `Command_Type_Exception`（`.lst:41726`）。真出处是 **`sub_41B640` case 14**（`.lst:40896-40903`，`0x41B92D`；另一份 `sub_41B9B0` `0x41BCFD` 同形） | 改注释出处（行为不变） |
| 6 | `model/engine-scalars.ts` 与 `emulator-engine-scalars.test.mjs` 头注：「取证见 `layout.mts` 的 `EVIDENCE`」 | `layout.mts` 的 `EVIDENCE` **没有这一条**（只有 frameStride / localPoolBases / intPoolBase / codecKey / encZero） | 改成指向台账条目 `Engine+0x5EC9C..0x5ECE8/ctor-zero-fill`（本轮已登记，含 `.lst:33974-33994` 二十条逐字） |
| 7 | `host/random.ts` 头注：「锚见需求单」 | 台账里 `srand` / `timeGetTime` **0 命中**，也没有点名 EA 的需求节点 | 改成指向台账条目 `Engine-ctor/timeGetTime-to-srand`（本轮已登记，`.lst:33783-33789`） |
| 8 | `host/instance.ts` 说"随机源的守卫在 `tools/test/emulator-host.test.mjs`" | 实际那条在 `tools/test/emulator-headless-logo.assets.test.mjs`（断言 `asm.instance.random` 存在、label 以 `seeded(0x` 开头） | 改指针 |
| 9 | 台账 `01M4B4HE3Z6T6K7M114D0V5J2T` 正文写「表项 `Engine+0x0A5D58`」 | `0x2de` 的表项是 `Engine+0xA5C14`（`.lst:34528`，`(0xA5C14−0xA509C)/4 = 0x2DE`）；`0xA5D58` 是 `0x32f` | 追加更正记录 |
| 10 | `vm/ops.ts` 的 `0x101` 注释：「把它清零，**再**清'未消费输入'闩锁」 | 逐字**顺序相反**：`.lst:38262 and [esi+0AAB44h],0F7FFFFFFh`（先清 bit27）→ `.lst:38263 mov [edi],0`（再清输入掩码） | 改注释顺序 |

## 守卫欠账（另外记，不混在上面）

* **`0x101 poll-input` 的"丢弃 + 置等输入"没有任何守卫**（全仓只命中 `ops.ts` 自己两行）
  ⇒ 把它实现成"读一次输入并保存"不会红。这是语义最容易写反的一条。
* **`0xa0 jcc` 没有专门的台账条目/守卫覆盖**（本轮已补台账）。
* `sub_418CC0` 的 **28 字节分支**（`8`/`14` ⇒ `base+28*idx`）与 `0x61`/`0x12c` 的**目标 type 计数**
  （`0xc`×34269 / `0xe`×674；`0x12c`：`0xc`×4220 / `0xe`×49）**只在注释与需求单正文里**，无台账、无守卫
  （`emulator-headless-logo.assets.test.mjs` 只断言 `0xc > 10000` / `0xe > 1000`，**不钉这几个数**）。
* `emulator-engine-scalars.test.mjs` 的「标量堆默认 0」只断言空实例读 0 ⇒ **恒真**（不回语料），
  补上 `Engine+0x5EC9C..0x5ECE8` 的逐字锚之后才真的在守。

## 收口判据

上面 10 条逐条对齐二进制（注释改写或台账追加更正）；守卫欠账的每一条补成**会让它红**的断言
（并往 `tools/mutate-check.mjs` 加一条变异）。

## 补：`pnpm test:mutation` 的实测结果（本轮跑过全套）

45 条变异里 **44 条按预期变红**，**1 条没红**（⇒ 那条守卫不名不副实）：

| 变异 | 该抓它的守卫 | 结果 |
|---|---|---|
| `0x2D2` 的 `staticUses` **0 → 7** | `tools/test/emulator-numeric-ops.test.mjs#★ 静态出现次数可复算：0 次的那批必须真的是 0` | ❌ **没红** |

★ 根因（**同一个病**）：那条用例大概是**从模型里取"`staticUses === 0` 的清单"再去核**，于是把某条的
`staticUses` 改成 7 之后，它**从待核清单里消失了** ⇒ 用例没有东西可核，静默通过。
⇒ 正确的形状是"**从语料现算**每个 opcode 的出现次数，与模型**逐个**比"（多一个或少一个都红），
而不是"拿模型自己的零清单去验模型自己的零"。**这就是本单的主病**：判据看起来在守，其实在复述。

## 补：另一条被本轮踩到的纪律（写给下一个写锚的人）

台账新锚**必须从守卫文件里复制完整用例名**（`rg "^test\(" <守卫文件>`）——本轮先写了缩写片段，
被 `tools/test/ledger.test.mjs` 的元判据抓住 16 条（去掉空白 < 8 字符），追了 16 条撤回 + 更正记录才修好。
★ 现状：在play的 guard 锚 126 条里**片段锚正好 15 条**（上限就是 15，全是历史遗留）⇒ **headroom = 0**：
**下一条新锚只要还是片段，门禁立刻红**。（修法不是改历史，而是新锚一律复制完整名。）
