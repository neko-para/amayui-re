# tools/ui-bake/effects.md — `tools/ui-bake/effects.json` 的说明书

> ★ 本仓约定：**JSON 是不透明数据** —— 字段 / 枚举 / 不变量 / 操作**只看控制脚本的自描述**。
> 这里只写"它是什么 + 非显然口径 + 指向"，**不复述 schema，也不抄任何 CSS 取值**。

## 它是什么

UI 文字效果的**共享字典**：把"同一个样式在多张图上复用"的那部分抽出来，让 recipe 只写差异。

* `font` —— 渲染字体族与基线（字号 / 字距的缺省值）；
* `roles` —— 画法层次的**全局定义**（`fill` / `outline` / `shadow`）。recipe 里每一层的 `role`
  必须在这里或某个 effect 的 `layers` 里出现，否则 `lint` 会红（那一层什么都不会画）；
* `effects` —— 效果条目 `E1…E10`（`layers` 列出该效果用到哪几层；`css` 给出该效果的**共享**声明；
  `fontSize` 给出该效果的字号）。**"哪种效果用在哪张图"不在这里** —— 那是各 recipe 的事，
  以及 `docs/` 里图片文档的解释；
* `svg` —— HTML 表达不了的效果（内阴影 / 膨胀外环这类），见下。

## 非显然的口径

1. **共享 CSS 会按属性分流**：`background` / `background-image` / `-webkit-background-clip` /
   `background-clip` / `-webkit-text-fill-color` 这五条会落到**最内层 span** 上，其余落在外层 div 上。
   理由：`background-clip:text` 只裁"**声明它的那个元素自己**的文字"；写在外层 div 上时
   div 自己没有文字节点 ⇒ 裁出空文字、渐变根本不上字。
2. **`role` 名不是随便起的**：同一块的多层靠 `z-index` 自下而上叠（旧仓口径是
   `shadow → outline → fill`）。名字只影响"哪条共享 CSS 生效"，不影响绘制顺序。
3. `svg` 段的条目是**"整包参数"**：recipe 的 svg 层可以 `effect:"<名>"` 整包引用、再逐项覆盖。

## 怎么查

```bash
pnpm tools ui-bake describe          # ★ 字段 / 不变量 / 命令（schema 的唯一真源）
pnpm tools ui-bake list              # 有哪些配方（块 / 生效版 / 是否汉化）
cat tools/ui-bake/effects.json       # 就是那份字典本身
pnpm tools ui-bake lint              # 效果层次与 recipe 的一致性守卫（红 = 退出码 1）
```

## 怎么改

* **加一个效果**：在 `effects.effects` 里加一条（`layers` + 可选 `css` / `fontSize`），
  然后跑 `pnpm tools ui-bake lint`（守卫会检查每个 effect 的层次都在 `roles` 里有定义）。
* **加一个画法层次**：先在 `roles` 里登记，再在 effect 的 `layers` 里引用。
* **改共享样式**：直接改该 effect 的 `css`；改了以后**必须**对用到它的每一张图跑
  `pnpm tools ui-bake verify <块>`（共享样式改动会同时影响多张图）。
* ⚠ 不要在这里写"某张图的私有取值" —— 那是 recipe 的 `layers[].css`。
* 字段 / 不变量的**真源**是 `pnpm tools ui-bake describe`；本文件不复述。
