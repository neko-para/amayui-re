# data/translations/ — 译文真源落点

## 1. 本目录放什么

翻译域的**真源**。数据模型**已拍板**（`docs/01-translation/README.md` §3）：**patch 叠加层**。

| 文件 | 是什么 |
|---|---|
| `patch.json` | ★ **唯一入库的翻译资产** = 相对原版 BIN 的变更叠加层（说明书 `patch.md`） |
| `subs-cn-jp.json` | 简→日写法占位字典（**编码方案的一半**，另一半是 cnjp 字体的 cmap 替换）——只读输入 |

**不放**：日文基线、`src` 视图、产出的汉化 BIN。它们都是**实时算出来的**（基线从安装目录解析，
视图由 patch 重放得到），入库等于给同一份信息再加副本。

## 2. 口径要点

* **旧仓 `data/`(941 文件) 与 `src/`(941 文件) 一个都没搬** —— 它们仍是 `external-only`
  （`corpus/assets.json` 的 `translation/baseline` / `translation/translated`）。
  实测 `src` 里的信息 **100% 可再算**（见 `docs/01-translation/patch-design.md` §2.2），
  所以"不搬会丢东西"这个顾虑不成立。
* **patch 里存中文**，不是 BIN 里的写法：BIN 走 cp932、简体字要用同码位日文写法占位
  ⇒ 同一个码位**无法区分**"本来就编得进去的汉字"与"占位" ⇒ BIN **反推不回中文**。
* **怎么查 / 怎么改**：`pnpm tools patch describe`（schema 的唯一真源）· `pnpm tools patch --help`。

## 3. 迁移批次

| 批次 | 内容 |
|---|---|
| **M5** | 按定案后的数据模型搬 `data/` + `src/` —— **已被 patch 方案取代**：现在只有 `patch.json` 入库，其余是视图 |
| **已做** | `subs-cn-jp.json`（M2）· `patch.json`（本节点 `REQ-01M3XJXVYBRFW1VT7RD8SNRXKV`） |

进度不在散文里：`pnpm tools requirements plan`。

## 4. 落点纪律（与语言 / 存储口径一致）

* 译文与基线是**文本** ⇒ 走 git（不是 LFS），行尾由 `.gitattributes` 首行统一为 LF；
* 若将来出现"双语内嵌 BIN"，那些**是二进制载荷** ⇒ 走 LFS，且**解压 / 生成视图永不入库**；
* 不在此目录放任何生成物（生成物一律 `.gitignore`）。
