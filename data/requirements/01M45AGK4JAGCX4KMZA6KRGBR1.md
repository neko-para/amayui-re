# 翻译 patch：基线索引 + merge on read（持久侧只依赖不可变的东西）

- id: REQ-01M45AGK4JAGCX4KMZA6KRGBR1
- type: req
- status: done
- parent: REQ-01M3XJXVYBRFW1VT7RD8SNRXKV
- verify: tools/test/patch.test.mjs#★ merge on read：`projectedRows` 与真视图的行空间一致（src = base 行 ∖ 被 replace/delete ∪ op 载荷）
- tags: [translation patch]

## 范围
把"持久侧"与"投影侧"分开：**只让依赖不可变输入的东西落盘**，投影一律按需现算（**merge on read**）。

## 为什么要分（用户口径 + 实测）
原来的模型把 `data`（基线侧）与 `src`（基线 + patch）**都**当缓存落盘 ⇒ `src` 依赖可变的 patch，
于是每改一次文案就要刷新、陈旧了还要判"能不能信"，全库检索动辄 10 s（缓存）～57 s（内存重算）。
但 `src` 其实只是 base 与 patch 的 **join 结果**，而 `patch.json` 里存的**就是中文** ⇒
* 查日文 ⇒ 只需要 base；
* 查中文 ⇒ 只需要 op 载荷；
* 查 `src` 的完整语义 ⇒ 一条**谓词**：`base 行 ∖ {被 replace/delete 的行} ∪ op 载荷`；
* 编辑 ⇒ 落在**锚上的 op**（改载荷 / 插一条 / 删一条），不需要"渲染 + 反解 + 整支重排对齐"。

## 交付
| 件 | 依赖 | 寿命 | 落盘 |
|---|---|---|---|
| **基线索引** `dist/index/base.json` | 基线 BIN + **codec 指纹** | **永不陈旧** | ✅ `patch index --write`（~0.2 s） |
| **base 文本** `dist/views/data/**` | 基线 BIN + codec | **永不陈旧** | ✅ `patch view`（941 支 ≈ 3.3 s / 83 MB） |
| **src 投影** | 基线 + **可变的 patch** | 每次改文案就旧 | ❌ 不常驻（`--kind src --name X` 按支物化草稿） |

* `find`：**不物化投影**（日文扫 base、中文扫 op 载荷），命中后按**锚**配对；实测全库 **1.4 s**；
* `set`：**按锚直改 op**（改字面量 / 插一行 / 删一行），不渲染、不重跑 diff；判据 = **重建行空间逐行等于独立算出的期望**；
* `edit`：只服务"用编辑器整篇改"（折行重排走这条），输入是物化出来的 `src` 草稿 + 草稿账本验来源；
* **codec 指纹**（`packages/age-format/src/asm/**` 的内容哈希）= "BIN 是文本的可逆像"这句话的**证人**：
  它一变，旧索引/旧草稿里的文本就不再担保能重建出同样字节；`patch status` 报它。

## 判据
* 机制契约：`tools/test/patch.test.mjs#★ merge on read`（`projectedRows` 与真视图的行空间逐行相同
  —— 除了 label 地址：投影沿用基线那份，真视图用重建后的）；
  同文件的 `#★ 基线索引`（只依赖基线 + codec；codec 一变就红）与 `#★ 编辑清单（锚寻址）`（三种形态各映到对应 op）；
* 端到端：`patch view` 941 支 ≈ 3.3 s；`patch find` 全库 ≈ 1.4 s；`patch set --write` 后 `patch verify` 全绿。
