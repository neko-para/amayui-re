# data/translations/ — 译文真源落点

## 1. 本目录将来放什么

翻译域的真源：日文只读基线 + 译文（或合并后的单份真源，取决于 §3 的待定项）。

## 2. 本轮状态：**语料仍是空的（有意为之）**，但多了一件真源

* 旧仓 `data/`(941 文件) 与 `src/`(941 文件) **一个都没搬** —— 它们是 `external-only`，
  见 `corpus/assets.json` 的 `translation/baseline` 与 `translation/translated`。
* 数据模型**本轮不定**（用户口径）：见 `docs/01-translation/README.md` §3 的备选方案 ——
  单份真源 + 机读修改点标记 vs 保留双份同构；以及"BIN 内嵌中日文让 emulator 切换"的前瞻。
* ★ **已入位一件**：`subs-cn-jp.json`（简→日写法占位字典，来源 = 旧仓 `res/subs_cn_jp.json`）。
  它不是译文，而是**编码方案的一半**（另一半是 cnjp 字体的 cmap 替换，归 `REQ-01M3SVH2V332E4CJYN0BHH0YK8`）。
  口径与改法见同名说明书 `subs-cn-jp.md`。

## 3. 迁移批次

**M5**：按定案后的数据模型搬 `data/` + `src/`；前置是数据模型拍板（§5.2）。

## 4. 落点纪律（与语言 / 存储口径一致）

* 译文与基线是**文本** ⇒ 走 git（不是 LFS），行尾由 `.gitattributes` 首行统一为 LF；
* 若将来出现"双语内嵌 BIN"，那些**是二进制载荷** ⇒ 走 LFS，且**解压 / 生成视图永不入库**；
* 不在此目录放任何生成物（生成物一律 `.gitignore`）。
