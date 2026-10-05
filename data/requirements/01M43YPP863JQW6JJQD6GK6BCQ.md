# 翻译 patch：按锚直改 op（生成清单 → 逐条确认 → 一次写盘；折行走编辑草稿）

- id: REQ-01M43YPP863JQW6JJQD6GK6BCQ
- type: req
- status: done
- parent: REQ-01M3XJXVYBRFW1VT7RD8SNRXKV
- verify: tools/test/patch.test.mjs#★ 编辑清单
- tags: [translation patch]

## 范围
让"改文案"这件事**一步落到 patch**，而且**每一处都有人看过**：
不是"先改视图文件、之后再想办法同步"，也不是"一条命令无条件全库替换"。

## 为什么（用户口径）
`dist/views/**` 是**缓存**，而 `src` 投影依赖**可变**的 patch ⇒ 它每次改文案都会旧。
把"改文案"拆成"改缓存文件 + 另跑同步"，迟早会在"缓存是旧的"上出错（把旧状态当成你改的写回 patch）；
而**无条件替换**同样不行：没人逐条看过的替换会误伤同形词（实测：`赫塔`→`废柴雷斯` 得到"废柴雷斯雷斯"）。

## 交付（机械的那一半 + 落笔的那一半，**按锚寻址**）
| 步骤 | 命令 | 说明 |
|---|---|---|
| ① 生成清单 | `patch find '<旧串>' --edits e.txt [--to '<新串>']` | **只写清单文件**；一条记录一处，头行是 `<脚本> <锚>[+<k>]`；`--to` 只是**机械填空** |
| ② 逐条审 | （人） | `+` 行对不对、同形词有没有误伤 |
| ③ 按锚直改 | `patch set --edits e.txt [--write]` | 每条 `-` 必须与现场**逐字相同**；三种形态各映到对应 op（改载荷 / `insert-after` / `delete`）；判据 = **重建行空间逐行 == 独立算出的期望**；不绿一个字都不写 |
| （折行重排） | `patch view --kind src --name X --out dist/views` → 编辑器改 → `patch edit --name X --write` | **行数变化**只能走这条（那需要重新对齐整支脚本） |

**编辑清单的形态**（头行用**锚** = 基线行序，**不是文件行号**）：
```
$1$ITINIT.BIN 316
- set-string (global-string 19964) "赫塔雷斯之戒"
+ set-string (global-string 19964) "废柴雷斯之戒"
```
`i=316+1` = 挂在锚 316 后面的第 1 条插入行（`insert-after`）。
★ 为什么不能用文件行号：`src` 不再常驻，而文件行号是**渲染产物**（label 定义行落在哪里由汇编器决定）。

## 顺带（同一次改动里的另一半）
* **持久侧只依赖不可变的东西**：基线索引 + base 文本（见 `REQ-01M45AGK4JAGCX4KMZA6KRGBR1`）；
* 写盘**不需要刷新任何缓存**（`data` 视图与基线索引只依赖基线）；
* `edit` 的输入是"物化出来的一支草稿"，账本记来源（`baseSha`/`resultSha`/`subsSha`/`codecSha`），对不上就拒绝。

## 判据
* 机制契约：`tools/test/patch.test.mjs#★ 编辑清单（锚寻址）`（三种形态 / `-` 绑定 / 块替换明确拒绝并指路）
  与 `#★ merge on read`（投影与真视图行空间一致）；`#★ 基线索引`（codec 一变就红）；
* 端到端：`set --write` 之后 `pnpm tools patch verify` 全绿（实测在 patch 副本上 453/453）。
