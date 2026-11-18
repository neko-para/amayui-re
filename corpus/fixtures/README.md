# corpus/fixtures/ — 样本**台账** + 工具（本目录已无存档载荷）

★ **2026-11-18（用户裁决）：本目录的存档载荷已全部删除** —— 顶层槽 76–79 的 `.DAT`/`.STH`、
系统容器 `SAVE.DAT` / `RT.DAT`、以及老汉化 936/GBK 的 `saves/` 子目录。此后这里只剩三样**文字件**：
`README.md`（本文件）· `samples.json`（台账，**允许 0 个样本**）· `samples.md`（台账的说明书）。

* **规范存档样本在仓库根 `saves/`**（**用户自维护**：agent ⛔ 不新建 / 不修改 / 不重命名 / 不删除）。
  要跑存档就把用到的那对文件**复制**到本次运行私有的 `--appdata`（见 `saves/README.md` 与
  `docs/03-emulator/save-format.md` §外部组装）。
  ⛔ 不要把这句读成"用 `saves/` 顶替本目录原来那批"：**它们不是同一份字节**（老汉化那批的复现已不可用）。
* **工具不退役**：`tools/fixtures.mjs`（模型在 `tools/lib/samples.mjs`）仍是 `samples.json` 的**唯一编辑入口**；
  将来补样本仍走 `pnpm tools fixtures add` / `add-container`，不要手动拷文件。

## 1. 想知道什么就去哪

| 想知道 | 真源 | 说明书 |
|---|---|---|
| 登记了哪些**槽样本**、每个槽在游戏里的**定位**、每个文件的 **mtime** | `samples.json` 的 `samples[]` | **[`samples.md`](./samples.md)** |
| 有哪些**系统容器 / 玩家配置**（不属任何槽的文件） | `samples.json` 的 `containers[]` | 同上 |
| 这些登记在本清单里是**哪一条**、走哪种**存储** | `../assets.json` 的 `fixtures/save-samples` | [`../assets.md`](../assets.md) |
| 怎么加 / 删 / 重取 / 拨回 mtime | `tools/fixtures.mjs` | **`pnpm tools fixtures describe`** |

## 2. 三条**非显然**的口径（散文该写的就只有这种）

* **本目录的清单条目是"自足条目"**：入库的那一份**就是**原件 ⇒ `assets.json` **不登记 `origin`**
  （`storage` 只回答"它走 LFS 还是 git"）。这不因 2026-11-18 的载荷删除而改变。
* **槽样本与系统容器是两类东西**：槽样本一对 `.DAT`+`.STH`、有槽号、有游戏内定位（`where`）；
  系统容器（`SAVE.DAT` / `RT.DAT` 一类）**不属任何槽**、身份就是文件名 ⇒ 登记在 `containers[]`，
  用 `what` 说清"它是什么 + 这份样本本身的口径"。
  ⇒ **一个文件只能有**一个**身份**（不许既当槽载荷又当系统容器）。
* **只收真实引擎写出来的东西**：模拟器 / 参考实现自己写出的文件**不是**样本
  （那会把"我们的实现"当成"引擎的行为"来核对）。

## 3. 消费规则（只读）

1. **只读**：环境构造靠**复制**（拷进 `.tmp/` 的 appdata 或 overlay），**永远不要就地修改**，
   也不要把 emulator 写出来的东西放回这里。
2. **不要把样本伪装成真游戏目录**：跑不起来的用例应当**如实 skip**，
   而不是手写一份 `SAVE.DAT` / `SYS4REG.INI` 去骗过某条断言 —— 那会让红点出现在与实现无关的地方。
3. **不要给样本改名来凑用例**：需要那些槽就补**真的**样本（补样本走 `samples.md` 的 `add`，**不要手动拷文件**）。
4. ★ **mtime 是判据的一部分**（槽头 `+264` 起七个 u16 就是存档时刻，而 git 不保存 mtime）：
   `samples.json` 记的是**源 instant + 采集时区**，恢复用 `pnpm tools fixtures restore-mtime --write`
   （⛔ 不要手工 `touch -t`，跨时区会错）。怎么拨、为什么跨时区也不会错，见 **`samples.md`**。

## 4. 存储与校验

* 本目录剩下的三个文件是**纯文本**，走 **git**（⛔ 不要让它们落到 LFS）。
* 清单条目 `fixtures/save-samples`：`storage` 由 `lfs` 翻为 `git`（2026-11-18，随载荷删除）
  ⇒ `pnpm tools corpus validate` 的 **#6「LFS 一致性」对非 `lfs` 条目不适用**；
  **#3「目录型 dest 必须非空」** 仍由 `README.md` / `samples.json` / `samples.md` 满足。
* 测试（`pnpm verify` 的工具链档）：`samples.json` 的结构与槽配对（**0 个槽合法**）、
  `containers[]` 的身份唯一性、**盘上文件集合与登记集合相等**（两边都不许多）、
  以及"每个 `.DAT` 的槽头 == 记录的 mtime"（0 个样本时如实 `skip`）。
