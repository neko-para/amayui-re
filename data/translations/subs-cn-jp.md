# data/translations/subs-cn-jp.md — `data/translations/subs-cn-jp.json` 的说明书

> ★ 本仓约定（`docs/00-origin/decisions.md` §6）：**JSON 是不透明数据** ——
> 字段 / 不变量 / 怎么查 / 怎么改**全部由控制脚本自描述**。
> ⚠ **本文件目前没有控制脚本**：它是一件**只读数据真源**（不是由某个工具生成的台账），
> 所以 `describe` 暂时无处可指。消费方（M5 的翻译管线）落地时**必须**补一个控制脚本，
> 并让它 `--describe` 出这份口径；在那之前，本文件就是唯一说明。
> **不要手改**格式相关的约定；数据本身可以改（见 §怎么改）。

## 它是什么

一张 `简体字 → 日文写法` 的映射表：游戏走 Shift-JIS（cp932），简体字多半编不进去，
于是写脚本时先用同码位的**日文写法**占位，再由 cnjp 字体把该码位字形换成简体字形。
⇒ 它和**字体构建**是同一方案的两半（见同目录 `README.md` §2）。

**来源**：旧仓 `res/subs_cn_jp.json`（逐字节相同；来源与 sha256 登记在 `../../corpus/assets.json`
的 `translation/subs-cn-jp` 一条）。

## 非显然的口径

* **值是"日文写法"，不是"繁体字"**：选的是**字形等价且落在 cp932 内**的那个码位，
  所以少数条目看起来像异体字（例如 `你→凜`）—— 那是码位选择的后果，不是错字。
* 它**不含**字形替换规则本身：`哪个码位 → 哪个字形` 在字体里（cmap），本文件只负责"文本层**写哪个码位**"。
* **条目的当前取值是状态**：不要在任何文档里抄条目数或具体键值。

## 怎么查

```bash
# 它就是一张扁平 JSON 对象（键=简体，值=日文写法）；没有查询工具，直接读或用 node/python 数
node -e "const m=require('fs').readFileSync('data/translations/subs-cn-jp.json','utf8');const o=JSON.parse(m);console.log(Object.keys(o).length)"
pnpm tools corpus list        # 存储去向 / 来源登记（说明书写在 corpus/assets.md）
```

## 怎么改

* **数据**：直接编辑本 JSON（`git diff` 有意义）；删条目的口径见同目录 `README.md` §4。
* **字段 / 不变量**：本文件没有 schema（就是一个对象），因此没有 `--describe` 可指 ——
  等 M5 的消费方落地时补控制脚本，把"怎么改"迁到它的自描述里，届时**本文件不再承担这份责任**。
