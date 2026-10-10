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

把 `apps/emulator/**` 注释里的"引擎结论"逐条对齐二进制（5 路只读取证包）。每条都给了 EA 或 `.lst` 行号（可复跑）。

## 本轮（2026-10）状态：注释 10/10 对齐 · 守卫 4/4 补上 · 两个子单已收口

### 10 条注释/指针：逐条对齐（落点）

| # | 落点 | 本轮做了什么 |
|---|---|---|
| 1 | `tools/test/emulator-iterate.test.mjs` + `apps/emulator/src/model/iterate.ts:22`/`:117-123` | ★ 源码两处注释**也已订正**（原先仍写 `bad opcode : 0`）：`rep stosd` 1024 项预填默认 handler（`.lst:34077-34079`），opcode 0 落默认 handler 抛「このコマンドはサポートされていません．」（`0x10001`，`.lst:37038-37045`） |
| 2 | `apps/emulator/src/model/numeric-ops.ts:81`/`:113` + `apps/emulator/src/model/pools.ts:421` | ★ 三处源码注释订正为"**进入次数**计数器（第 13 次进入归零）、**没有重掷循环**，只有一次 `call _rand`"（`.lst:68761-68768`，口径照 `ops.ts:167-170`） |
| 3–9 | `vm/ops.ts` 的 `0x192` · 台账 int 配置对象 · `vm/operand.ts:154` · `engine-scalars.ts`/`host/random.ts` 锚指针 · `host/instance.ts` 指针 · 台账表项 | 上一轮已逐条改/追加更正（本轮复核仍在位） |
| 10 | `vm/ops.ts` 的 `0x101` 注释顺序 | 上一轮已改（先清 bit27、再清掩码） |
| 追加 | `apps/emulator/src/vm/ops.ts:1118` 的**指针落空** | ★ 本轮修：`REQ-01M4E1EH…` **不存在** ⇒ 改成真正的帧模型单 `REQ-01M4E4Q11P12MD0PJJDFZPZ5H0` |

### 四处守卫欠账：全部补上，各自都有变异条目（实测红）

| # | 欠账 | 守卫（validate `verify` 指同一处） | 变异条目 |
|---|---|---|---|
| 1 | `0x101 poll-input` 的"丢弃 + 置等输入"无守卫 | verify: `tools/test/emulator-engine-scalars.test.mjs`#★ `0x101 poll-input`：采样一次后**丢弃**（不留在任何状态里）、只发一条 modeled 记录 | `maskDiscarded` → `false`（`fail 1`） |
| 2 | `0xa0 jcc` 无守卫 | verify: `tools/test/emulator-engine-scalars.test.mjs`#★ `0xa0 jcc`：非 0 跳 op2 / 为 0 跳 op3 / 哨兵**落下**（三支都要可观测） | jcc 两支对调（`fail 1`） |
| 3 | `sub_418CC0` 的 28 字节分支 + 目标 type 计数不钉数 | verify: `tools/test/emulator-headless-logo.assets.test.mjs`#★★ `0x61`/`0x12c` 的目标 type 只有 `0xc`/`0xe`，且次数钉死（34269/674；4220/49） | 28 → 4 字节步长（`fail 1`） |
| 4 | "标量堆默认 0"恒真 | verify: `tools/test/emulator-engine-scalars.assets.test.mjs`#★★ `Engine+0x5EC9C..0x5ECE8`：构造函数里连续 20 条写 0（逐字锚） | `CTOR_ZERO_FILLED` 20 → 19（`fail 1`） |

★ 欠账 1 的**残余边界（不掩盖）**：`maskDiscarded`/`latchCleared` 是本模型自己的措辞 —— 把采样留在
**模型外宿主状态**的实现抓不住。要真抓住那一类，得先建模 `Engine+0xAAB44/+0xAAB48/+0x777FC/+0x77808`
（新能力，不在本轮范围）。这条已写进用例注释。

### 两条"请裁决"观察：冲突**已解除**（不需要人裁决）

`01M4E5THCZ1F1N5G0V7X64295W`（`0x2ee`）与 `01M4E5V1995K247X0M6B126V0V`（`0x1ca`）claim 里那句"⛔ 请裁决"
是上一轮的遗留措辞：源码（`packages/age-format/src/engine/layout.mts:245-250`、`apps/emulator/src/vm/ops.ts:444`）
与二进制（`0x42024F`/`0x426676`/`0x426694` 三处 `0AA514h`）**三方一致**。⇒ **不改那两条 observation**
（append-only），只追加台账 note：`KN-01M4GXX12V3Q6A0Q501D1D1236`（subject
`opcode/0x1ca+0x2ee/AA514-conflict-resolved`）。★ 该 note 是 **`proposed`** —— 它只有 reference 锚、
没有 self 锚，按台账不变量 #4 **不许算 accepted**；等 `engine/**` 有源码级守卫、或把该表加进
`tools/test/emulator-engine-scalars.test.mjs` 的逐条对账后再升。先落的那条 accepted 版本已按 append-only
撤回（`KN-01M4GXWXJ66X2E7R46790S6V2D`）。

### 两个子单（已收口）

* `REQ-01M4G9YTEKGER7N99C3M7F443R`（纯数值族 corpus 来源）⇒ **done**，
  `verify: tools/test/emulator-numeric-ops.test.mjs#★ 静态出现次数可复算：0 次的那批必须真的是 0（不是"没数到"）`。
  语料改成清单 `gameInstall` + ALF 解出（565 个 `.BIN` 条目），39 条 `staticUses` 按新语料重数 ——
  两源**不是同一份语料**（`0x50`：62945 vs 70439；零出现集合 6 → 7 条）。台账 note：
  `KN-01M4GXT5ES31425Y4F0Q652C3T`（subject `model/numeric-ops-staticuses-corpus-source`）。
* `REQ-01M4GE4HSJGZ4XF9BSTZFHKXKG`（AGERC pin）⇒ **dropped**（不是缺陷）：本机四份 AGERC 逐个实测
  都与清单一致、`corpus validate` #4 全绿；`5f3189` 是**另一条** `binary/agerc-packed`（root=gameInstall）
  的正当读数；`git show 16fbd0a:corpus/assets.json` 显示该条**自 init 起**就是 `install/AGERC.DLL + 6241de…`
  ⇒ 是**跨机器 root 解析差异**，真问题由 `REQ-01M4FEFA9RRP5TKVWH351NV1RD` 跟踪。★ mac 侧无法在本机验证。

## 本父单为什么不关

两个子单都已收口，但本单自己只剩"注释措辞"这类裁决项（无独立守卫可挂）；父 `…592` 还有别的活节点
⇒ **先不关父**，等那条支线一起收口时再决定本单是 `done`（挂上面四条 `verify` 中最强者）还是并入。
