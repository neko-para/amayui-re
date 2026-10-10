# 前沿取证（第二榜）· 全语料用得最多而未登记的助手（sub_4034C0 / sub_42B4B0 / sub_4BC360 / sub_4BC350 …）

- id: REQ-01M4GA2A1TTTM2ZWGQD8Q4JDK2
- type: req
- status: done
- parent: REQ-01M4G9XYQG149FB3VG5Y6FW3YY
- verify: tools/test/ledger-coverage.test.mjs#Engine 尺度位移 ⇒ 判回引擎代码
- tags: [engine, coverage]

## 为什么另开一张榜

`pnpm tools ledger coverage` 的**第一张榜**（"被多少**已登记**函数调用"降序）回答的是"**它挡了多少活**"。
但有一类函数在这张榜上**排不高**，却在全语料里**用得极广**：**叶子助手**
（`sub_408050` 就是样本：110 个调用方 / 205 处调用，自己没有 callee ⇒ callee 方向没有前沿）。
⇒ 查询另给**第二张榜**（`mostUsedUnregistered`：全语料用得最多、而自己还没登记）。

本节点收口第二张榜的其余条目（第一榜那 5 个已各有子节点）。

## 当前榜单（★ 由 `pnpm tools ledger coverage` **现算**，口径 = **引擎函数**）

★ **口径声明（这一节以前是错的）**：下表一律取 `coverage` 的**引擎**口径 —— 即**先把"疑似库代码"候选摘出去**再数调用方 / 调用处
（`tools/lib/coverage.mjs` 的 `libraryLikely`）。它与**全语料**口径**不是同一组数**：`sub_4034C0` 引擎 **160 / 350** vs 全语料 **161 / 352**，
差的那 1 个调用方是库候选 `sub_45F1B0`（`coverage.mjs` 的反向表只遍历引擎函数）。两个数都对，但**混用会得出"我算错了"的假象** ⇒
本表只认引擎口径，全语料那套放在括号里当对照。

| 函数 | 引擎 调用方 / 调用处 | 全语料（对照） | 是什么 |
|---|---|---|---|
| `sub_4034C0` | 160 / 350 | 161 / 352 | 取出内层对象后**尾跳**的 thunk（不在第一榜上：它的调用方大多**未登记**） |
| `sub_4BC360` | 79 / 79 | 同 | Live2D 区**固定实参转发** → `sub_4BEDC0` |
| `sub_4BC350` | 66 / 71 | 同 | `live2d::LDObject` 构造函数（只写 vftable、**不返回 `this`**） |
| `sub_4BC340` | 58 / 86 | 同 | 同上，**返回 `this`** 的版本（★ 原表漏了它） |
| `sub_41C300` | 57 / 165 | 同 | **float 读值**原语（★ 原表漏了它） |
| `sub_4034D0` | 52 / 61 | 同 | 与 `sub_4034C0` 同段的 thunk → `sub_4976A0` |

★ **不在表里、但本批已收口**：`sub_42B4B0`（写值原语，131 / 223）与它的 float 兄弟 `sub_42BA00` —— 它们**曾经**被库候选启发式
**摘出榜单与分桶**（假阳性），于是原表"它在第二榜上"这句当时**不成立**；收窄规则修好后它们**回到**榜上，台账条目见
`Engine+0x42B4B0/library-candidate-false-positive`。

## 收口判据（逐条凭据）

| # | 判据 | 凭据 |
|---|---|---|
| 1 | 每个函数一条台账观察，**锚 = 二进制 EA**（函数起点 + 关键分支），且回答"它是什么 / 调用者依赖它做什么" | `01M4GWVTW03G3N0B2F5M2C2P08`（`reloc-thunk+0x4034C0/inner-object-forwarder`）· `01M4GX1QZ36T011C117S472Z18`（`Engine+0x42B4B0/operand-write-primitive-type-table`）· `01M4GWWZCP090C4R394Z4Y7F3P`（`live2d+0x4BC350/LDObject-vftable-ctor`）· `01M4GWX0SC4C281C1P592A2055`（`live2d+0x4BC360/fixed-arg-forwarder-to-4BEDC0`） |
| 2 | 若它是 `sub_41BF50` 的**对偶**（写侧），两侧口径写在**同一条**里 | `01M4GX1SDH4G0J2A5C4Q4M6P0Y`（`Engine+0x42B4B0\|0x41BF50/write-vs-read-support-set-and-codec-boundary`） |
| 3 | 守卫草案（能机械判的） | `verify` = `tools/test/ledger-coverage.test.mjs#Engine 尺度位移 ⇒ 判回引擎代码`（`@env pure`，本机必跑）；真语料那条另有 `tools/test/ledger-coverage.assets.test.mjs` |
| 4 | 它 callee 里新暴露的未登记函数 ⇒ 登记进**后续节点** | `REQ-01M4GWY83PCFDFDQYQ7X7Z2RBZ`（`sub_497620` / `sub_4976A0` / `sub_40C780` / `sub_45D240` / `sub_4BEDC0` + 本批未查明项） |

## 本批的订正

* 原表自称"由查询现算、不手写"，实际抄的是**全语料**口径 ⇒ 已按**引擎**口径重算并**写明两套口径**（见上）。
* 原表**漏了两条**：`sub_4BC340`（58 / 86）与 `sub_41C300`（57 / 165）。
* 原表把 `sub_4BC360` / `sub_4BC350` 归到"WndProc 族（`_WinMain@16` 注册的那个）附近" —— **实测不成立**：
  `_WinMain@16` 在 `0x4BA890`（`.lst:295818-296907`），`sub_4BC360` 在 `.lst:298331` ⇒ 相隔 **2513 行**，且那**整个区间**内 `4BC3` / `live2d` **0 命中**。
  正确归属 = **Live2D Cubism SDK 胶水区**（判据 = 符号逐字 `??_7LDObject@live2d@@6B@`）。见 `01M4GWX1W82Q06013Y7E664R3M`。

## 本批未查明（★ 不许当已解决；逐项挂在 `REQ-01M4GWY83PCFDFDQYQ7X7Z2RBZ`）

`0x4BC3xx` 胶水的**完整边界** · `sub_4BEDC0`/`sub_4BED90` 的**语义** · `sub_4034C0` 的**内层对象类型** ·
`sub_42B4B0` 的 case 7 / case 12 **为什么用不同池** · 写侧 `0x8005` 是否**存在分支** · `sub_41C300` / `sub_42BA00` 两个 **float 原语未取证**。

★ 榜单会随登记滚动：**每次收口后重新跑一次 `coverage`**，把新出现的名字补进来（或另开后继节点）。
