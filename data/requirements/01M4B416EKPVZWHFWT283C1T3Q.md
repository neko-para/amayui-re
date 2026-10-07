# 能力缺口 · 启动时按配置存在性设置 global（⇒ 否则 LOADCONFIG 那条支永远走不到）

- id: REQ-01M4B416EKPVZWHFWT283C1T3Q
- type: req
- status: open
- parent: REQ-01M4B248NB8FWDN61E2NMK8N6V

## 缺口是什么

标准启动链在 `SYSTEM4.BIN#56` 有一个**配置存在性门**：
```
jcc (global-int 5) ffffffff label    ; global5 != 0 ⇒ 落下执行 call-script LOADCONFIG
                                     ; global5 == 0 ⇒ 跳走，走 INITCONFIG（建默认配置）
```
**实测（可复算）**：让配置真的落盘（同一实例连跑两次），两次走的**都是 INITCONFIG 支**。
原因不是持久化没生效（配置文件确实写成了 14 条），而是：

* `load-int`（`0x1a3`）的**键取自该变量当时的值**：变量初值 0 ⇒ 查键 `\x03 00000000`；
* `save-int`（`0x1a2`）存的是**以该值为名**的条目（`\x03 00000001` = 1）⇒ 两者对不上；
* 而 `#56` 的 `jcc` 在**任何** `load-int` **之前**执行 ⇒ `global5` 那一刻必然是初值。

⇒ **启动时那个 global 由引擎侧设置**（"配置存在 ⇒ 置某 global"），**不是脚本自己读配置**。
本仓没有这个引擎侧动作 ⇒ `LOADCONFIG` 这条支**永远不会被走到**。

## 要做什么

1. 取证：引擎启动时按什么条件把哪个 global（或帧字段）置成什么值 —— 是**引擎构造函数 / 配置装载器**那一族；
   判据要逐字（EA）。★ 这属于技能里说的 **capability（门控标志 / 惰性创建）**：它没有 opcode 观测点，
   只能在**帧/启动**这个尺度上看出来。
2. 落进模型（`host` 侧？还是 `Machine` 初始化？）—— 位置取决于第 1 步的取证。
3. 判据：**启动链在不直跑子脚本的情况下真的走到 `LOADCONFIG`**，
   并且棘轮里那条"根脚本停在哪"的断言随之更新。

## 在此之前怎么量（不许糊）

以子脚本为根直接跑（`--root-id <id>`），并把结果钉进守卫
（`tools/test/emulator-headless-logo.assets.test.mjs` 的"三条子脚本各自跑到下一个明确缺口"）：

| 子脚本 | id | 结果 |
|---|---|---|
| `LOADCONFIG.BIN` | 21080 | 32 步 ⇒ 停在 `#33 0x61 lookup-array` |
| `CHECKCONFIG.BIN` | 20955 | 0 步 ⇒ 停在 `#0 0x2de` |
| `INITCONFIG.BIN` | 20956 | 跑通（含 `INITCONFIG0..4`）⇒ 停在 `INITCONFIG4#7 0x61 lookup-array` |

★ 这样做是**诚实**的：它量的是"这份脚本自己能跑多远"，并且**显式注明**绕过了引擎侧的初始化；
⛔ 不许把"直跑子脚本"说成"启动链走到了 LOADCONFIG"。
