# M4-1b-1 · 推过 SYSTEM4 启动前段（16 个 opcode，handler 已机械点名）+ 前沿棘轮

- id: REQ-01M4B248NB8FWDN61E2NMK8N6V
- type: req
- status: open
- parent: REQ-01M4AGPP1T9A60HQYW3GX9W592

## 当前实测（本轮：前沿**未回退**，290 步 → 14288 步不变）

| 入口 | 现在 |
|---|---|
| 启动链根脚本 | **14288 步** ⇒ `SYSTEM4.BIN#100 = 0x30a` |
| `LOADCONFIG` / `CHECKCONFIG` / `INITCONFIG(0..5)` / `INITCHARM` | 都**跑完** |

## 本轮做完（ADR 第 ② 步的剩余 —— **float 族已迁**）

* **float 族（`float`/`floatPtr`/`float`/`floatRef`）迁到区域**：格内容 = **float32 位模式**
  （`fstp dword ptr` 取证），读/写/快照各自在边界换算。
* ★★ 途中把**三条换算口径**分开写清（这是本轮最容易出错的地方，守卫逐条抓过）：
  1. **`read()`**：编码族 DEC、float 族 `floatFromBits`、ptr 族原样 ⇒ `cellToValue`；
  2. **快照**：口径是"**与 Map 路径存的东西逐值相同**"（`[下标, 位模式][]`）⇒ 编码族**不 DEC**、
     float 族写**数值** ⇒ `cellToSnapshot`（⛔ 用 `cellToValue` 会把 int 族的快照从位模式变成解码值 = **改外部形状**）；
  3. **搬进区域**（构造器 / `restore`）：快照里编码族**已经是位模式** ⇒ `snapshotToCell`（⛔ 再 ENC 一次 = 双重编码）。
* 位模式换算提到 `model/float-bits.ts`（**一份真源**）：原来它在 `vm/operand.ts`，而 `model/pools.ts` 不许反向依赖 `vm/`。
* ⛔ **只剩 `string` 族没迁**（28 字节 SSO）：它是**一等一的树节点** `REQ-01M4BEHCHZE5QDHCNTA77V8ZJG`，
  里面写清了为什么不能照做（长串 ≥16 字节时 `+0` 存的是**指向堆的指针**，而本层没有堆 ⇒ 要么加"字符串数据区 + 分配器"，要么承认指针语义），
  以及**明确否掉的替代方案**（把 JS 字符串塞进 28 字节格 —— 那样任何按字节读这一格的地方都会得到"看起来正常"的错值）。

## 下一单元

1. **先核对路径**（见上一轮的建议，仍然成立）：`REQ-01M4B416EKPVZWHFWT283C1T3Q`（`SYSTEM4#56` 的配置门）。
2. `0x30a` 等 `SYSTEM4` 长尾 opcode。
3. string 族迁移（要先裁决"堆"这件事）。
