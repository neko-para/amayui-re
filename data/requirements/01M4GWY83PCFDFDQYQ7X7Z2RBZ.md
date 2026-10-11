# 前沿取证 · 第二榜后续 · 新暴露的未登记 callee（sub_497620 / sub_4976A0 / sub_40C780 / sub_45D240 / sub_4BEDC0）+ 本批未查明项

- id: REQ-01M4GWY83PCFDFDQYQ7X7Z2RBZ
- type: req
- status: done
- parent: REQ-01M4G9XYQG149FB3VG5Y6FW3YY
- verify: tools/test/emulator-engine-scalars.test.mjs#★ 配置的整型两条
- tags: [engine, coverage]

## 结果（本单可收口的部分）

★ **5 个 callee 各一条台账观察已落**（`status=accepted`，锚 = 二进制 EA；都补了一条 `self` 守卫锚，见各自的 `note`）：

| callee | subject（台账） | 结论一句话 |
|---|---|---|
| `sub_4BEDC0` | `live2d+0x4BC360/fixed-arg-forwarder-to-4BEDC0`（`KN-01M4GWX0SC4C281C1P592A2055`，**本单之前就有**） | `if (p) { vt = *(p-4); f = *(vt); f(p) }` = 一次虚调用（`p-4` 是 vftable 指针） |
| `sub_497620` | `Engine+0x410\|0x420/output-sink-with-one-shot-hook`（`KN-01M4N26CCC6B25406G2X1P110H`） | 带**一次性钩子**的输出汇：有钩就先给钩子，**钩子返 0 才**回落默认输出并**卸钩**（`+0x420` 是"已安装"标志、`+0x410` 是钩子指针） |
| `sub_4976A0` | `Engine+0x5D880/diagnostic-printer-line-prefix`（`KN-01M4N26CW16S1D2W0S4Q126R75`） | **诊断打印器**：加 `"(%s：%d行目) %s"` 前缀再转发 `sub_497620`；行号取不到（`0xFFFF`）⇒ 直转发 |
| `sub_40C780` | `container/insert-at-iterator-4byte-element`（`KN-01M4N26DAH46356C1Z2P303F0C`） | **4 字节元素**容器的"在迭代位置插入"（越界即插末尾；`sar edi,2` + `add [esi+4],4`） |
| `sub_45D240` | `Engine+0x14D30/container-insert-8byte-element`（`KN-01M4N26DRS3F2D706V122Q1X73`） | **8 字节元素**的同形插入（`sar edi,3` + `add [esi+4],8`）；与 4 字节族**不重叠**的 8 个调用方 |

★ **顺带落的两条**（本单正文里没有、复核时冒出来的，各自都是独立结论）：
* `Engine+0x4E3AC/table-lookup-by-id-then-index`（`KN-01M4N1Z19R063F6X450239303X`）—— `sub_48F000`：先按 id 拼键 `.XXXXXXXX` 查表，命中且 `count > a3` 才返回 `*(*(v+8) + 4*a3)`，否则 **0**。
* `tool/disasm/function-attribution-blind-spot-for-orphan-chunks`（`KN-01M4N26EPA756B2J0T4W372H20`）—— `functionOfEa` 对 `; START OF FUNCTION CHUNK FOR …` **返回 null**、`enclosingFunction` 会给出前一个已 `endp` 的函数 ⇒ 凡写"调用方 N 个"的地方都要注明"机械归属下的下界"。

## "本批未查明"7 项的处置（逐项，**不许静默留**）

| # | 项 | 处置 |
|---|---|---|
| 1 | `0x4BC3xx` 补参数转发胶水的**完整边界** | **转移**：它只影响 `sub_4BEDC0` / `sub_4BED90` 那块的边界，与**第二榜**的 Live2D 归属同源（`KN-01M4GWX1W82Q06013Y7E664R3M` 已写明"没量"） ⇒ 不在本单重复跟踪 |
| 2 | `sub_4BEDC0` / `sub_4BED90` 的**语义** | **已收口**：`KN-01M4GWX0SC4C281C1P592A2055` 的逐字 = 一次虚调用；"它是什么"要落在被调对象上，那需要 Live2D SDK 侧的取证 ⇒ 另开单（见下） |
| 3 | `sub_4034C0` / `sub_4034D0` 的**内层对象类型**（`this+4`） | **另开单**：这需要把"外层对象 → 内层对象"那族（`+4` / vtable 槽 `+4`/`+8`）连起来看，属 `sub_4034C0` 那一支 |
| 4 | `sub_42B4B0` 的 case 7 / case 12 **为什么用不同池** | **另开单**：这是"取址/写值原语"那一族的池分派，与帧池布局同域 |
| 5 | 写侧 `0x8005`（字符串数组句柄）**是否存在分支** | **另开单**：`0x192 set-string` 的落点分派 |
| 6 | `sub_41C300`（float 读）/ `sub_42BA00`（float 写）**未取证** | **另开单**：这两个原语是"指针族 + 容器句柄"那条主线的下一层 |
| 7 | `sub_4BC350` 的 `endp` EA 与末条指令 EA 相同（`.lst` 排版事实） | **已收口**：这是**工具口径**问题 —— 销项判据 = `disasm` 层怎么归属 `endp`；本轮实测的同类事实（孤立函数块 ⇒ `functionOfEa` 返回 null）已进台账 `tool/disasm/function-attribution-blind-spot-for-orphan-chunks`，本条不再单列 |

★ 上面 3 个"另开单"的项合成本单的**后继单** `REQ-01M4N2AC3GE71SSV2YFKGSKJN0`（同一父下），避免本单停在 open 上让整棵子树收不了口。

## 判据

1. 5 个 callee 各一条台账观察，锚 = EA，`status=accepted`（★ 已达成：`pnpm tools ledger list --subject …` 可复算；`pnpm tools ledger validate` 7/7 通过）。
2. "未查明"7 项逐项有处置（查明 / 显式降级）—— 见上表。
3. 新暴露的前沿已由台账 `coverage` 重跑体现（`pnpm tools ledger coverage`）。

★ 本单的 `verify` 挂的是**本轮同时修掉的那个"错近似"**的守卫（配置键的声称写实）—— 它不是本单 5 个 callee 的直接守卫（那些的锚是 EA），但它是本轮唯一"会红"的机械判据。
